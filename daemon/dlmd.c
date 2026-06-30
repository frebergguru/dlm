/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dlmd — the dlm download daemon.
 *
 * Owns the queue/engine and serves an AF_UNIX JSON-lines socket. A single poll()
 * loop multiplexes the listening socket and connected clients; worker threads
 * (in queue.c) perform the downloads. The loop ticks the scheduler and pushes
 * periodic progress events to subscribed clients.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "ipc.h"
#include "queue.h"
#include "dlm/dlm.h"
#include "dlm/store.h"
#include "dlm/tools.h"
#include "compat/compat.h"
#include "internal.h" /* dlm_now */

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICK_MS 200
#define PROGRESS_INTERVAL 0.5
#define TOOLS_CHECK_INTERVAL 3600.0 /* re-trigger the (weekly-gated) tool check hourly */

static volatile sig_atomic_t g_stop = 0;
static void on_stop(void) { g_stop = 1; }

/* The daemon owns tool management: ensure required tools exist and run the
 * weekly yt-dlp update check on a detached thread so startup/downloads never
 * block on the network. */
static void *tools_thread(void *arg)
{
    (void)arg;
    dlm_tools_ensure_ready(1);
    dlm_tools_check_updates(0);
    return NULL;
}

static void trigger_tools_check(void)
{
    pthread_t th;
    if (pthread_create(&th, NULL, tools_thread, NULL) == 0)
        pthread_detach(th);
}

/* ---- per-client buffers ----------------------------------------------- */

/* Cap a single inbound request line and the pending outbound queue so a slow
 * or hostile client can't drive unbounded memory growth in the daemon. */
#define MAX_CLIENT_INBUF (1 << 20)  /* 1 MiB per request line */
#define MAX_CLIENT_OUTBUF (8 << 20) /* 8 MiB of unflushed responses/events */
#define DLMD_MAX_CLIENTS 128        /* refuse past this so a flood can't exhaust fds */

typedef struct {
    int fd;
    char *buf;          /* inbound: accumulated bytes awaiting a '\n' */
    size_t len, cap;
    char *out;          /* outbound: bytes queued for write, drained on POLLOUT */
    size_t outlen, outcap;
    int subscribed;
    int gone;           /* marked for removal after this poll iteration */
} client_t;

typedef struct {
    client_t *v;
    int n, cap;
} clients_t;

static client_t *clients_add(clients_t *c, int fd)
{
    if (c->n == c->cap) {
        int ncap = c->cap ? c->cap * 2 : 8;
        client_t *nv = realloc(c->v, (size_t)ncap * sizeof *c->v);
        if (!nv) return NULL;
        c->v = nv;
        c->cap = ncap;
    }
    client_t *cl = &c->v[c->n++];
    memset(cl, 0, sizeof *cl);
    cl->fd = fd;
    return cl;
}

static void clients_remove(clients_t *c, int idx)
{
    dlm_sock_close((dlm_sock_t)c->v[idx].fd);
    free(c->v[idx].buf);
    free(c->v[idx].out);
    c->v[idx] = c->v[--c->n];
}

/* Append inbound bytes to a client's read buffer. Marks the client gone if the
 * unparsed line exceeds MAX_CLIENT_INBUF (no newline within the limit). */
static void client_append(client_t *cl, const char *data, size_t n)
{
    if (cl->len + n + 1 > MAX_CLIENT_INBUF) {
        cl->gone = 1;
        return;
    }
    if (cl->len + n + 1 > cl->cap) {
        cl->cap = cl->len + n + 1;
        if (cl->cap < 256) cl->cap = 256;
        cl->buf = realloc(cl->buf, cl->cap);
    }
    memcpy(cl->buf + cl->len, data, n);
    cl->len += n;
    cl->buf[cl->len] = '\0';
}

/* Queue a line (a newline is appended) for asynchronous delivery. Never blocks
 * and never writes a partial line; the loop drains cl->out on POLLOUT. Marks the
 * client gone if its backlog exceeds MAX_CLIENT_OUTBUF. */
static void client_queue(client_t *cl, const char *s)
{
    if (!s) return;
    size_t len = strlen(s);
    size_t need = cl->outlen + len + 1; /* + newline */
    if (need > MAX_CLIENT_OUTBUF) { cl->gone = 1; return; }
    if (need > cl->outcap) {
        size_t nc = cl->outcap ? cl->outcap : 256;
        while (nc < need) nc *= 2;
        cl->out = realloc(cl->out, nc);
        cl->outcap = nc;
    }
    memcpy(cl->out + cl->outlen, s, len);
    cl->outlen += len;
    cl->out[cl->outlen++] = '\n';
}

/* Write as much of the outbound queue as the socket will take. Stops on EAGAIN
 * (the rest stays queued for the next POLLOUT); marks the client gone only on a
 * real write error. */
static void client_flush(client_t *cl)
{
    size_t off = 0;
    while (off < cl->outlen) {
        long w = dlm_sock_write((dlm_sock_t)cl->fd, cl->out + off, cl->outlen - off);
        if (w < 0) {
            if (dlm_sock_was_intr()) continue;
            if (dlm_sock_would_block()) break;
            cl->gone = 1;
            break;
        }
        off += (size_t)w;
    }
    if (off > 0) {
        memmove(cl->out, cl->out + off, cl->outlen - off);
        cl->outlen -= off;
    }
}

/* Socket address and DB path now come from the shared compat layer
 * (dlm_socket_path / dlm_db_path), so the daemon and clients always agree. The
 * single-instance guard is a connect-probe via dlm_unix_probe(). */

/* ---- main ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int max_active = 3;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-j") && i + 1 < argc)
            max_active = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--help")) {
            printf("usage: dlmd [-j max_active]\n");
            return 0;
        }
    }

    dlm_net_init();

    char sock_path[256], dbpath[512];
    dlm_socket_path(sock_path, sizeof sock_path);
    dlm_db_path(dbpath, sizeof dbpath);

    if (dlm_unix_probe(sock_path)) {
        fprintf(stderr, "dlmd: already running at %s\n", sock_path);
        return 1;
    }

    dlm_global_init();
    dlm_store *store = dlm_store_open(dbpath);
    if (!store) { fprintf(stderr, "dlmd: cannot open store %s\n", dbpath); return 1; }
    dlm_queue *q = dlm_queue_create(store, max_active);

    dlm_sock_t lfd = dlm_unix_listen(sock_path, 16); /* binds + listens + nonblock */
    if (lfd == DLM_INVALID_SOCK) {
        fprintf(stderr, "dlmd: cannot listen on %s\n", sock_path);
        return 1;
    }

    dlm_install_term_handler(on_stop); /* SIGINT/SIGTERM (+ignore SIGPIPE) */

    fprintf(stderr, "dlmd: listening on %s (db %s, max_active %d)\n",
            sock_path, dbpath, max_active);

    clients_t clients = {0};
    double last_progress = 0;
    double last_tools = 0;

    /* pollfd array reused across iterations (grown as clients connect) so an
     * idle daemon does no per-tick allocation. */
    struct dlm_pollfd *pfd = NULL;
    int pfd_cap = 0;

    while (!g_stop) {
        int nclients = clients.n;
        int nfds = 1 + nclients;
        if (nfds > pfd_cap) {
            int ncap = pfd_cap ? pfd_cap : 8;
            while (ncap < nfds) ncap *= 2;
            struct dlm_pollfd *np = realloc(pfd, (size_t)ncap * sizeof *pfd);
            if (!np) { dlm_sleep_ms(TICK_MS); continue; } /* transient OOM */
            pfd = np;
            pfd_cap = ncap;
        }
        pfd[0].fd = lfd;
        pfd[0].events = DLM_POLLIN;
        for (int i = 0; i < nclients; i++) {
            pfd[i + 1].fd = (dlm_sock_t)clients.v[i].fd;
            pfd[i + 1].events = DLM_POLLIN;
            if (clients.v[i].outlen > 0) pfd[i + 1].events |= DLM_POLLOUT;
        }

        /* Adaptive poll timeout. Tick cadence only while the scheduler has work
         * (active workers or startable items); the progress cadence while a
         * subscriber is attached; otherwise block until a client speaks — poll
         * wakes on any socket activity, and new downloads only arrive via a
         * client command. Capped at the next hourly tools check so it still
         * fires when idle. This keeps an idle daemon at ~0% CPU instead of
         * waking 1000/TICK_MS times a second. */
        int timeout;
        if (dlm_queue_has_pending_work(q)) {
            timeout = TICK_MS;
        } else {
            int has_sub = 0;
            for (int i = 0; i < clients.n; i++)
                if (clients.v[i].subscribed) { has_sub = 1; break; }
            if (has_sub) {
                timeout = (int)(PROGRESS_INTERVAL * 1000);
            } else if (last_tools == 0) {
                timeout = 0; /* run the startup tools check immediately */
            } else {
                double wait = (last_tools + TOOLS_CHECK_INTERVAL) - dlm_now();
                if (wait < 1.0) wait = 1.0;
                timeout = (int)(wait * 1000);
            }
        }

        int r = dlm_poll(pfd, (unsigned)nfds, timeout);
        if (r < 0 && !dlm_sock_was_intr()) { perror("poll"); break; }

        /* accept new connections (drain the backlog; lfd is non-blocking) */
        if (r > 0 && (pfd[0].revents & DLM_POLLIN)) {
            for (;;) {
                dlm_sock_t cfd = dlm_sock_accept(lfd);
                if (cfd == DLM_INVALID_SOCK) break;
                /* Refuse past a sane cap, but still accept+close so the backlog
                 * drains — otherwise a local flood would keep lfd readable and
                 * spin the poll loop, and unbounded clients_add could exhaust
                 * fds. Closing here keeps us well under the OS fd limit. */
                if (clients.n >= DLMD_MAX_CLIENTS) { dlm_sock_close(cfd); continue; }
                dlm_sock_set_nonblock(cfd);
                if (!clients_add(&clients, (int)cfd)) dlm_sock_close(cfd); /* OOM: drop */
            }
        }

        /* Service only the clients that had a pollfd slot this round; clients
         * accepted just above sit past index nclients and are handled next
         * iteration. Bounding by nclients keeps pfd[i + 1] in range. Removal is
         * deferred (via cl->gone) so indices stay stable through the loop. */
        int shutdown_req = 0;
        for (int i = nclients - 1; i >= 0; i--) {
            client_t *cl = &clients.v[i];
            short re = pfd[i + 1].revents;
            if (re & DLM_POLLOUT) client_flush(cl);
            if (cl->gone) continue;
            if (re & (DLM_POLLHUP | DLM_POLLERR)) { cl->gone = 1; continue; }
            if (!(re & DLM_POLLIN)) continue;

            char tmp[4096];
            long got = dlm_sock_read((dlm_sock_t)cl->fd, tmp, sizeof tmp);
            if (got <= 0) {
                if (got == 0 || (!dlm_sock_would_block() && !dlm_sock_was_intr()))
                    cl->gone = 1;
                continue;
            }
            client_append(cl, tmp, (size_t)got);

            /* process complete lines */
            char *nl;
            while (!cl->gone && (nl = memchr(cl->buf, '\n', cl->len))) {
                *nl = '\0';
                int sub = 0;
                char *resp = dlm_ipc_handle(q, cl->buf, &sub, &shutdown_req);
                if (sub) cl->subscribed = 1;
                if (resp) { client_queue(cl, resp); free(resp); }
                /* shift remaining bytes down */
                size_t consumed = (size_t)(nl - cl->buf) + 1;
                memmove(cl->buf, nl + 1, cl->len - consumed);
                cl->len -= consumed;
                cl->buf[cl->len] = '\0';
            }
            if (!cl->gone) client_flush(cl);
        }
        if (shutdown_req) g_stop = 1;

        /* scheduler tick */
        dlm_queue_tick(q);

        double now = dlm_now();

        /* ensure tools at startup, then re-trigger the weekly-gated update check
         * hourly (the check itself no-ops until the 7-day window elapses) */
        if (now - last_tools >= TOOLS_CHECK_INTERVAL || last_tools == 0) {
            last_tools = now;
            trigger_tools_check();
        }

        /* push progress to subscribers */
        if (now - last_progress >= PROGRESS_INTERVAL) {
            last_progress = now;
            int has_sub = 0;
            for (int i = 0; i < clients.n; i++)
                if (clients.v[i].subscribed) { has_sub = 1; break; }
            if (has_sub) {
                char *ev = dlm_ipc_progress_event(q);
                if (ev) {
                    for (int i = 0; i < clients.n; i++) {
                        if (!clients.v[i].subscribed) continue;
                        client_queue(&clients.v[i], ev);
                        client_flush(&clients.v[i]);
                    }
                    free(ev);
                }
            }
        }

        /* reap any clients flagged for removal this iteration */
        for (int i = clients.n - 1; i >= 0; i--)
            if (clients.v[i].gone) clients_remove(&clients, i);
    }

    free(pfd);
    fprintf(stderr, "\ndlmd: shutting down (pausing %d active)\n",
            dlm_queue_active_count(q));
    for (int i = 0; i < clients.n; i++) {
        dlm_sock_close((dlm_sock_t)clients.v[i].fd);
        free(clients.v[i].buf);
        free(clients.v[i].out);
    }
    free(clients.v);
    dlm_queue_destroy(q); /* stops + joins workers, persists state */
    dlm_store_close(store);
    dlm_sock_close(lfd);
    remove(sock_path);
    dlm_net_cleanup();
    dlm_global_cleanup();
    return 0;
}
