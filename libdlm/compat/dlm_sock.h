/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm/compat — AF_UNIX sockets + poll, portable across POSIX and Winsock. */
#ifndef DLM_COMPAT_SOCK_H
#define DLM_COMPAT_SOCK_H

#include <stddef.h>

#if defined(_WIN32)
#  include <winsock2.h>
typedef SOCKET dlm_sock_t;
#  define DLM_INVALID_SOCK INVALID_SOCKET
#else
typedef int dlm_sock_t;
#  define DLM_INVALID_SOCK (-1)
#endif

/* poll() event flags (mapped to the platform's native flags inside dlm_poll). */
#define DLM_POLLIN  0x0001
#define DLM_POLLOUT 0x0004
#define DLM_POLLERR 0x0008
#define DLM_POLLHUP 0x0010

struct dlm_pollfd {
    dlm_sock_t fd;
    short events;
    short revents;
};

/* One-time Winsock init/teardown (no-op on POSIX). dlm_net_init() is idempotent
 * and safe to call from each binary's startup. */
int dlm_net_init(void);
void dlm_net_cleanup(void);

/* Create an AF_UNIX stream socket bound and listening at `path`, set
 * non-blocking. Removes any stale socket file first. Returns the listening
 * socket or DLM_INVALID_SOCK. */
dlm_sock_t dlm_unix_listen(const char *path, int backlog);

/* Connect a new AF_UNIX stream socket to `path`. Returns the connected socket or
 * DLM_INVALID_SOCK. */
dlm_sock_t dlm_unix_connect(const char *path);

/* Returns 1 if a server currently accepts connections at `path`, else 0. */
int dlm_unix_probe(const char *path);

/* Accept one pending connection on a (non-blocking) listening socket. Returns
 * the new socket, or DLM_INVALID_SOCK when the backlog is drained. */
dlm_sock_t dlm_sock_accept(dlm_sock_t lfd);

int  dlm_sock_set_nonblock(dlm_sock_t fd);
long dlm_sock_read(dlm_sock_t fd, void *buf, size_t n);
long dlm_sock_write(dlm_sock_t fd, const void *buf, size_t n);
void dlm_sock_close(dlm_sock_t fd);

/* Last-operation error classification (errno or WSAGetLastError under the
 * hood). Use after a read/write/accept returning < 0. */
int dlm_sock_would_block(void);
int dlm_sock_was_intr(void);

/* poll() wrapper. timeout_ms < 0 blocks indefinitely. */
int dlm_poll(struct dlm_pollfd *fds, unsigned n, int timeout_ms);

#endif /* DLM_COMPAT_SOCK_H */
