/* dlmd — the dlm download daemon.
 *
 * Owns the queue/engine and serves an AF_UNIX JSON-lines socket. A single poll()
 * loop multiplexes the listening socket and connected clients; worker threads
 * (in queue.c) perform the downloads. The loop ticks the scheduler and pushes
 * periodic progress events to subscribed clients.
 */
#define _POSIX_C_SOURCE 200809L
#include "ipc.h"
#include "queue.h"
#include "dlm/dlm.h"
#include "dlm/store.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define TICK_MS 200
#define PROGRESS_INTERVAL 0.5

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

/* ---- per-client buffers ----------------------------------------------- */

/* Cap a single inbound request line and the pending outbound queue so a slow
 * or hostile client can't drive unbounded memory growth in the daemon. */
#define MAX_CLIENT_INBUF (1 << 20)  /* 1 MiB per request line */
#define MAX_CLIENT_OUTBUF (8 << 20) /* 8 MiB of unflushed responses/events */

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
        c->cap = c->cap ? c->cap * 2 : 8;
        c->v = realloc(c->v, (size_t)c->cap * sizeof *c->v);
    }
    client_t *cl = &c->v[c->n++];
    memset(cl, 0, sizeof *cl);
    cl->fd = fd;
    return cl;
}

static void clients_remove(clients_t *c, int idx)
{
    close(c->v[idx].fd);
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
        ssize_t w = write(cl->fd, cl->out + off, cl->outlen - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
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

/* ---- paths ------------------------------------------------------------ */

static void mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void socket_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt)
        snprintf(buf, n, "%s/dlm.sock", rt);
    else
        snprintf(buf, n, "/tmp/dlm-%d.sock", (int)getuid());
}

static void db_path(char *buf, size_t n)
{
    const char *override = getenv("DLM_DB");
    if (override && *override) { snprintf(buf, n, "%s", override); return; }
    const char *data = getenv("XDG_DATA_HOME");
    char dir[400];
    if (data && *data)
        snprintf(dir, sizeof dir, "%s/dlm", data);
    else
        snprintf(dir, sizeof dir, "%s/.local/share/dlm", getenv("HOME"));
    mkdir_p(dir);
    snprintf(buf, n, "%s/queue.db", dir);
}

/* ---- single-instance guard ------------------------------------------- */

/* Copy a path into a sockaddr_un, returning 0 if it fits, -1 otherwise. */
static int set_sun_path(struct sockaddr_un *a, const char *path)
{
    size_t len = strlen(path);
    if (len >= sizeof a->sun_path) return -1;
    memcpy(a->sun_path, path, len + 1);
    return 0;
}

/* Returns 1 if a daemon already accepts connections at `path`. */
static int already_running(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (set_sun_path(&a, path) != 0) { close(fd); return 0; }
    int ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}

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

    char sock_path[256], dbpath[512];
    socket_path(sock_path, sizeof sock_path);
    db_path(dbpath, sizeof dbpath);

    struct sockaddr_un szchk;
    if (strlen(sock_path) >= sizeof szchk.sun_path) {
        fprintf(stderr, "dlmd: socket path too long (%zu >= %zu): %s\n",
                strlen(sock_path), sizeof szchk.sun_path, sock_path);
        return 1;
    }
    (void)szchk;

    if (already_running(sock_path)) {
        fprintf(stderr, "dlmd: already running at %s\n", sock_path);
        return 1;
    }
    unlink(sock_path); /* clear any stale socket */

    dlm_global_init();
    dlm_store *store = dlm_store_open(dbpath);
    if (!store) { fprintf(stderr, "dlmd: cannot open store %s\n", dbpath); return 1; }
    dlm_queue *q = dlm_queue_create(store, max_active);

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    set_sun_path(&addr, sock_path); /* length already validated above */
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(lfd, 16) < 0) { perror("listen"); return 1; }
    fcntl(lfd, F_SETFL, O_NONBLOCK); /* accept() must not block the poll loop */

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "dlmd: listening on %s (db %s, max_active %d)\n",
            sock_path, dbpath, max_active);

    clients_t clients = {0};
    double last_progress = 0;

    while (!g_stop) {
        int nclients = clients.n;
        int nfds = 1 + nclients;
        struct pollfd *pfd = calloc((size_t)nfds, sizeof *pfd);
        pfd[0].fd = lfd;
        pfd[0].events = POLLIN;
        for (int i = 0; i < nclients; i++) {
            pfd[i + 1].fd = clients.v[i].fd;
            pfd[i + 1].events = POLLIN;
            if (clients.v[i].outlen > 0) pfd[i + 1].events |= POLLOUT;
        }

        int r = poll(pfd, (nfds_t)nfds, TICK_MS);
        if (r < 0 && errno != EINTR) { perror("poll"); free(pfd); break; }

        /* accept new connections (drain the backlog; lfd is non-blocking) */
        if (r > 0 && (pfd[0].revents & POLLIN)) {
            for (;;) {
                int cfd = accept(lfd, NULL, NULL);
                if (cfd < 0) break;
                fcntl(cfd, F_SETFL, O_NONBLOCK);
                clients_add(&clients, cfd);
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
            if (re & POLLOUT) client_flush(cl);
            if (cl->gone) continue;
            if (re & (POLLHUP | POLLERR)) { cl->gone = 1; continue; }
            if (!(re & POLLIN)) continue;

            char tmp[4096];
            ssize_t got = read(cl->fd, tmp, sizeof tmp);
            if (got <= 0) {
                if (got == 0 || (errno != EAGAIN && errno != EINTR))
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

        free(pfd);

        /* scheduler tick */
        dlm_queue_tick(q);

        /* push progress to subscribers */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
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

    fprintf(stderr, "\ndlmd: shutting down (pausing %d active)\n",
            dlm_queue_active_count(q));
    for (int i = 0; i < clients.n; i++) close(clients.v[i].fd);
    free(clients.v);
    dlm_queue_destroy(q); /* stops + joins workers, persists state */
    dlm_store_close(store);
    close(lfd);
    unlink(sock_path);
    dlm_global_cleanup();
    return 0;
}
