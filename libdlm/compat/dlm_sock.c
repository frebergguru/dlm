/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm/compat — AF_UNIX sockets + poll, portable across POSIX and Winsock. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "compat/dlm_sock.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#include <ws2tcpip.h>
#include <afunix.h>

static int g_wsa_started = 0;

int dlm_net_init(void)
{
    if (g_wsa_started) return 0;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    g_wsa_started = 1;
    return 0;
}

void dlm_net_cleanup(void)
{
    if (g_wsa_started) { WSACleanup(); g_wsa_started = 0; }
}

static int set_sun(struct sockaddr_un *a, const char *path)
{
    size_t len = strlen(path);
    if (len >= sizeof a->sun_path) return -1;
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    memcpy(a->sun_path, path, len + 1);
    return 0;
}

dlm_sock_t dlm_unix_listen(const char *path, int backlog)
{
    dlm_net_init();
    SOCKET fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) return DLM_INVALID_SOCK;
    struct sockaddr_un a;
    if (set_sun(&a, path) != 0) { closesocket(fd); return DLM_INVALID_SOCK; }
    DeleteFileA(path); /* clear any stale socket file */
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(fd); return DLM_INVALID_SOCK; }
    if (listen(fd, backlog) != 0) { closesocket(fd); return DLM_INVALID_SOCK; }
    dlm_sock_set_nonblock(fd);
    return fd;
}

dlm_sock_t dlm_unix_connect(const char *path)
{
    dlm_net_init();
    SOCKET fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) return DLM_INVALID_SOCK;
    struct sockaddr_un a;
    if (set_sun(&a, path) != 0) { closesocket(fd); return DLM_INVALID_SOCK; }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(fd); return DLM_INVALID_SOCK; }
    return fd;
}

dlm_sock_t dlm_sock_accept(dlm_sock_t lfd) { return accept(lfd, NULL, NULL); }

int dlm_sock_set_nonblock(dlm_sock_t fd)
{
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? 0 : -1;
}

long dlm_sock_read(dlm_sock_t fd, void *buf, size_t n)
{
    return recv(fd, (char *)buf, (int)n, 0);
}

long dlm_sock_write(dlm_sock_t fd, const void *buf, size_t n)
{
    return send(fd, (const char *)buf, (int)n, 0);
}

void dlm_sock_close(dlm_sock_t fd) { closesocket(fd); }

int dlm_sock_would_block(void)
{
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK;
}

int dlm_sock_was_intr(void)
{
    return WSAGetLastError() == WSAEINTR;
}

int dlm_poll(struct dlm_pollfd *fds, unsigned n, int timeout_ms)
{
    WSAPOLLFD stackbuf[16];
    WSAPOLLFD *pf = n <= 16 ? stackbuf : malloc(n * sizeof *pf);
    if (!pf) return -1;
    for (unsigned i = 0; i < n; i++) {
        pf[i].fd = fds[i].fd;
        short ev = 0;
        if (fds[i].events & DLM_POLLIN) ev |= POLLRDNORM;
        if (fds[i].events & DLM_POLLOUT) ev |= POLLWRNORM;
        pf[i].events = ev;
        pf[i].revents = 0;
    }
    int r = WSAPoll(pf, n, timeout_ms);
    for (unsigned i = 0; i < n; i++) {
        short re = 0;
        if (pf[i].revents & (POLLRDNORM | POLLIN)) re |= DLM_POLLIN;
        if (pf[i].revents & (POLLWRNORM | POLLOUT)) re |= DLM_POLLOUT;
        if (pf[i].revents & POLLERR) re |= DLM_POLLERR;
        if (pf[i].revents & POLLHUP) re |= DLM_POLLHUP;
        fds[i].revents = re;
    }
    if (pf != stackbuf) free(pf);
    return r;
}

#else /* POSIX */

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int dlm_net_init(void) { return 0; }
void dlm_net_cleanup(void) {}

static int set_sun(struct sockaddr_un *a, const char *path)
{
    size_t len = strlen(path);
    if (len >= sizeof a->sun_path) return -1;
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    memcpy(a->sun_path, path, len + 1);
    return 0;
}

dlm_sock_t dlm_unix_listen(const char *path, int backlog)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return DLM_INVALID_SOCK;
    struct sockaddr_un a;
    if (set_sun(&a, path) != 0) { close(fd); return DLM_INVALID_SOCK; }
    unlink(path); /* clear any stale socket */
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return DLM_INVALID_SOCK; }
    if (listen(fd, backlog) < 0) { close(fd); return DLM_INVALID_SOCK; }
    dlm_sock_set_nonblock(fd);
    return fd;
}

dlm_sock_t dlm_unix_connect(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return DLM_INVALID_SOCK;
    struct sockaddr_un a;
    if (set_sun(&a, path) != 0) { close(fd); return DLM_INVALID_SOCK; }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return DLM_INVALID_SOCK; }
    return fd;
}

dlm_sock_t dlm_sock_accept(dlm_sock_t lfd) { return accept(lfd, NULL, NULL); }

int dlm_sock_set_nonblock(dlm_sock_t fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

long dlm_sock_read(dlm_sock_t fd, void *buf, size_t n) { return read(fd, buf, n); }
long dlm_sock_write(dlm_sock_t fd, const void *buf, size_t n)
{
    /* MSG_NOSIGNAL: writing to a peer that already closed must return EPIPE,
     * not raise SIGPIPE — otherwise a one-shot CLI (which doesn't ignore the
     * signal) is killed instead of getting a clean error. */
#ifdef MSG_NOSIGNAL
    return send(fd, buf, n, MSG_NOSIGNAL);
#else
    return send(fd, buf, n, 0);
#endif
}
void dlm_sock_close(dlm_sock_t fd) { close(fd); }

int dlm_sock_would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }
int dlm_sock_was_intr(void) { return errno == EINTR; }

int dlm_poll(struct dlm_pollfd *fds, unsigned n, int timeout_ms)
{
    struct pollfd stackbuf[16];
    struct pollfd *pf = n <= 16 ? stackbuf : malloc(n * sizeof *pf);
    if (!pf) return -1;
    for (unsigned i = 0; i < n; i++) {
        pf[i].fd = fds[i].fd;
        short ev = 0;
        if (fds[i].events & DLM_POLLIN) ev |= POLLIN;
        if (fds[i].events & DLM_POLLOUT) ev |= POLLOUT;
        pf[i].events = ev;
        pf[i].revents = 0;
    }
    int r = poll(pf, (nfds_t)n, timeout_ms);
    for (unsigned i = 0; i < n; i++) {
        short re = 0;
        if (pf[i].revents & POLLIN) re |= DLM_POLLIN;
        if (pf[i].revents & POLLOUT) re |= DLM_POLLOUT;
        if (pf[i].revents & POLLERR) re |= DLM_POLLERR;
        if (pf[i].revents & POLLHUP) re |= DLM_POLLHUP;
        fds[i].revents = re;
    }
    if (pf != stackbuf) free(pf);
    return r;
}

/* shared by both: probe whether a server accepts connections */
#endif

int dlm_unix_probe(const char *path)
{
    dlm_sock_t fd = dlm_unix_connect(path);
    if (fd == DLM_INVALID_SOCK) return 0;
    dlm_sock_close(fd);
    return 1;
}
