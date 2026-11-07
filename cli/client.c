/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dlm CLI — daemon client transport. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "client.h"
#include "dlm/proto.h"
#include "compat/compat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <io.h>
#  include <winsock2.h>
#  define DLMD_NAME "/dlmd.exe"
#else
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#  define DLMD_NAME "/dlmd"
#endif

/* Request/response RPCs must not hang forever on a stuck daemon: every daemon
 * command answers immediately, so a reply that takes longer than this means
 * the daemon is wedged. Applied only around one request/response exchange
 * (and the connect-time ping) and cleared afterwards, so long-lived subscribe
 * streams (CLI `watch`, the GUI) — which may legitimately sit idle — never
 * time out. */
#define DLM_RPC_TIMEOUT_MS 15000

/* Set (ms > 0) or clear (ms == 0) the send/receive timeout on a client fd. */
static void set_io_timeout(int fd, int ms)
{
#if defined(_WIN32)
    DWORD t = (DWORD)ms; /* Winsock: milliseconds as a DWORD, 0 = none */
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
    setsockopt((SOCKET)fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t);
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

/* Set by read_line/write_all when the last socket error was a timeout. */
static int g_timed_out;

static int last_error_is_timeout(void)
{
#if defined(_WIN32)
    int e = WSAGetLastError();
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

/* upper bound on a single protocol line; a peer that never sends '\n' must not
 * be able to grow the read buffer without limit */
#define DLM_LINE_MAX (16u * 1024 * 1024)

/* The public API speaks in int fds; on Windows a SOCKET round-trips through int
 * (user-mode socket handles fit), and the compat layer does the real work. */
static int try_connect(const char *path)
{
    dlm_net_init();
    dlm_sock_t s = dlm_unix_connect(path);
    return (s == DLM_INVALID_SOCK) ? -1 : (int)s;
}

/* Resolve the path to the dlmd binary that sits next to this executable. */
static int dlmd_path(char *buf, size_t n)
{
    char dir[512];
    if (dlm_self_dir(dir, sizeof dir) != 0) return -1;
    snprintf(buf, n, "%s" DLMD_NAME, dir);
    return 0;
}

static int dlmd_exists(const char *path)
{
#if defined(_WIN32)
    return _access(path, 0) == 0;
#else
    return access(path, X_OK) == 0;
#endif
}

static int spawn_daemon(void)
{
    char path[1024];
    if (dlmd_path(path, sizeof path) != 0) return -1;
    if (!dlmd_exists(path)) {
        fprintf(stderr, "dlm: cannot find dlmd at %s\n", path);
        return -1;
    }
    const char *argv[] = {path, NULL};
    return dlm_proc_spawn_detached(argv);
}

int dlm_client_connect_existing(void)
{
    char path[256];
    dlm_socket_path(path, sizeof path);
    return try_connect(path);
}

void dlm_client_close(int fd)
{
    if (fd >= 0) dlm_sock_close((dlm_sock_t)fd);
}

static int write_all(int fd, const char *s, size_t len);

/* Ping the daemon and return 1 if its protocol matches this client's. */
static int daemon_proto_ok(int fd)
{
    const char *ping = "{\"cmd\":\"ping\"}\n";
    set_io_timeout(fd, DLM_RPC_TIMEOUT_MS);
    char *line = NULL;
    if (write_all(fd, ping, strlen(ping)) == 0) line = dlm_client_read_line(fd);
    set_io_timeout(fd, 0);
    if (!line) return 0;
    char marker[32];
    snprintf(marker, sizeof marker, "\"proto\":%d", DLM_PROTO_VERSION);
    int ok = strstr(line, marker) != NULL;
    free(line);
    return ok;
}

static int spawn_and_wait(const char *path)
{
    if (spawn_daemon() != 0) return -1;
    for (int i = 0; i < 50; i++) { /* up to ~5s */
        dlm_sleep_ms(100);
        int fd = try_connect(path);
        if (fd >= 0) return fd;
    }
    fprintf(stderr, "dlm: daemon did not come up\n");
    return -1;
}

int dlm_client_connect(void)
{
    char path[256];
    dlm_socket_path(path, sizeof path);

    int fd = try_connect(path);
    if (fd >= 0) {
        if (daemon_proto_ok(fd)) return fd;
        /* a stale daemon (older protocol) is running — shut it down so our
         * matching daemon can take over, then respawn */
        fprintf(stderr, "dlm: restarting outdated daemon\n");
        const char *sd = "{\"cmd\":\"shutdown\"}\n";
        set_io_timeout(fd, DLM_RPC_TIMEOUT_MS);
        if (write_all(fd, sd, strlen(sd)) == 0) free(dlm_client_read_line(fd));
        dlm_client_close(fd);
        /* the old daemon must be gone before we spawn: otherwise the new one
         * exits "already running" and we would reconnect to the old one */
        int gone = 0;
        for (int i = 0; i < 150; i++) { /* up to ~15s */
            dlm_sleep_ms(100);
            int t = try_connect(path);
            if (t < 0) { gone = 1; break; }
            dlm_client_close(t);
        }
        if (!gone) {
            fprintf(stderr, "dlm: the outdated daemon did not shut down; "
                            "stop the running dlmd and try again\n");
            return -1;
        }
    }
    return spawn_and_wait(path);
}

static int write_all(int fd, const char *s, size_t len)
{
    size_t off = 0;
    while (off < len) {
        long w = dlm_sock_write((dlm_sock_t)fd, s + off, len - off);
        if (w < 0) {
            if (dlm_sock_was_intr()) continue;
            g_timed_out = last_error_is_timeout();
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

char *dlm_client_read_line(int fd)
{
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        char c;
        long r = dlm_sock_read((dlm_sock_t)fd, &c, 1);
        if (r < 0) {
            if (dlm_sock_was_intr()) continue;
            g_timed_out = last_error_is_timeout();
            free(buf);
            return NULL;
        }
        if (r == 0) { /* EOF */
            if (len == 0) { free(buf); return NULL; }
            break;
        }
        if (c == '\n') break;
        if (len + 1 >= cap) {
            if (cap >= DLM_LINE_MAX) { free(buf); return NULL; } /* refuse runaway lines */
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
            cap *= 2;
        }
        buf[len++] = c;
    }
    buf[len] = '\0';
    return buf;
}

void dlm_client_reader_init(dlm_client_reader *r, int fd)
{
    r->fd = fd;
    r->start = r->end = 0;
}

char *dlm_client_reader_line(dlm_client_reader *r)
{
    size_t cap = 4096, len = 0;
    char *line = malloc(cap);
    if (!line) return NULL;
    for (;;) {
        /* emit from the buffered window up to a newline */
        while (r->start < r->end) {
            char c = r->buf[r->start++];
            if (c == '\n') { line[len] = '\0'; return line; }
            if (len + 1 >= cap) {
                if (cap >= DLM_LINE_MAX) { free(line); return NULL; } /* refuse runaway lines */
                char *nb = realloc(line, cap * 2);
                if (!nb) { free(line); return NULL; }
                line = nb;
                cap *= 2;
            }
            line[len++] = c;
        }
        /* window drained: refill in one syscall */
        long got = dlm_sock_read((dlm_sock_t)r->fd, r->buf, sizeof r->buf);
        if (got < 0) {
            if (dlm_sock_was_intr()) continue;
            free(line);
            return NULL;
        }
        if (got == 0) { /* EOF */
            if (len == 0) { free(line); return NULL; }
            line[len] = '\0';
            return line;
        }
        r->start = 0;
        r->end = (size_t)got;
    }
}

char *dlm_client_rpc(int fd, const char *req)
{
    size_t len = strlen(req);
    char *line = malloc(len + 2);
    if (!line) return NULL;
    memcpy(line, req, len);
    line[len] = '\n';
    line[len + 1] = '\0';
    g_timed_out = 0;
    set_io_timeout(fd, DLM_RPC_TIMEOUT_MS);
    int rc = write_all(fd, line, len + 1);
    free(line);
    char *resp = rc == 0 ? dlm_client_read_line(fd) : NULL;
    /* clear again: the caller may keep reading this fd as a stream (subscribe) */
    set_io_timeout(fd, 0);
    if (!resp && g_timed_out)
        fprintf(stderr, "dlm: daemon did not respond within %d s\n",
                DLM_RPC_TIMEOUT_MS / 1000);
    return resp;
}
