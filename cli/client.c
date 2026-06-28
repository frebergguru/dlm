/* dlm CLI — daemon client transport. */
#define _POSIX_C_SOURCE 200809L
#include "client.h"
#include "dlm/proto.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void socket_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt)
        snprintf(buf, n, "%s/dlm.sock", rt);
    else
        snprintf(buf, n, "/tmp/dlm-%d.sock", (int)getuid());
}

static int try_connect(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path) { /* would silently truncate */
        close(fd);
        return -1;
    }
    memcpy(a.sun_path, path, strlen(path) + 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) return fd;
    close(fd);
    return -1;
}

/* Resolve the path to the dlmd binary that sits next to this executable. */
static int dlmd_path(char *buf, size_t n)
{
    char exe[PATH_MAX];
    ssize_t r = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (r < 0) return -1;
    exe[r] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash) return -1;
    *slash = '\0';
    snprintf(buf, n, "%s/dlmd", exe);
    return 0;
}

static int spawn_daemon(void)
{
    char path[PATH_MAX];
    if (dlmd_path(path, sizeof path) != 0) return -1;
    if (access(path, X_OK) != 0) {
        fprintf(stderr, "dlm: cannot find dlmd at %s\n", path);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* detach: new session, redirect std fds */
        setsid();
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, 0);
            dup2(dn, 1); /* keep stderr for logs */
            if (dn > 2) close(dn);
        }
        execl(path, "dlmd", (char *)NULL);
        _exit(127);
    }
    return 0;
}

int dlm_client_connect_existing(void)
{
    char path[256];
    socket_path(path, sizeof path);
    return try_connect(path);
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
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
        int fd = try_connect(path);
        if (fd >= 0) return fd;
    }
    fprintf(stderr, "dlm: daemon did not come up\n");
    return -1;
}

int dlm_client_connect(void)
{
    char path[256];
    socket_path(path, sizeof path);

    int fd = try_connect(path);
    if (fd >= 0) {
        if (daemon_proto_ok(fd)) return fd;
        /* a stale daemon (older protocol) is running — shut it down so our
         * matching daemon can take over, then respawn */
        fprintf(stderr, "dlm: restarting outdated daemon\n");
        const char *sd = "{\"cmd\":\"shutdown\"}\n";
        write_all(fd, sd, strlen(sd));
        free(dlm_client_read_line(fd));
        close(fd);
        for (int i = 0; i < 30; i++) {
            struct timespec ts = {0, 100 * 1000 * 1000};
            nanosleep(&ts, NULL);
            int t = try_connect(path);
            if (t < 0) break;
            close(t);
        }
    }
    return spawn_and_wait(path);
}

static int write_all(int fd, const char *s, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, s + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
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
        ssize_t r = read(fd, &c, 1);
        if (r < 0) {
            if (errno == EINTR) continue;
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
