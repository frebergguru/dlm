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
#  define DLMD_NAME "/dlmd.exe"
#else
#  include <unistd.h>
#  define DLMD_NAME "/dlmd"
#endif

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
    if (write_all(fd, ping, strlen(ping)) != 0) return 0;
    char *line = dlm_client_read_line(fd);
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
        write_all(fd, sd, strlen(sd));
        free(dlm_client_read_line(fd));
        dlm_client_close(fd);
        for (int i = 0; i < 30; i++) {
            dlm_sleep_ms(100);
            int t = try_connect(path);
            if (t < 0) break;
            dlm_client_close(t);
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
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

char *dlm_client_read_line(int fd)
{
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        char c;
        long r = dlm_sock_read((dlm_sock_t)fd, &c, 1);
        if (r < 0) {
            if (dlm_sock_was_intr()) continue;
            free(buf);
            return NULL;
        }
        if (r == 0) { /* EOF */
            if (len == 0) { free(buf); return NULL; }
            break;
        }
        if (c == '\n') break;
        if (len + 1 >= cap) {
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

char *dlm_client_rpc(int fd, const char *req)
{
    size_t len = strlen(req);
    char *line = malloc(len + 2);
    if (!line) return NULL;
    memcpy(line, req, len);
    line[len] = '\n';
    line[len + 1] = '\0';
    int rc = write_all(fd, line, len + 1);
    free(line);
    if (rc != 0) return NULL;
    return dlm_client_read_line(fd);
}
