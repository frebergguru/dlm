/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dlm CLI — daemon client transport.
 *
 * Connects to the dlmd AF_UNIX socket, auto-starting the daemon if it is not
 * already running, and exchanges JSON-lines requests/responses.
 */
#ifndef DLM_CLIENT_H
#define DLM_CLIENT_H

/* Connect to the daemon, spawning dlmd if needed. Returns a socket fd or -1. */
int dlm_client_connect(void);

/* Connect only if a daemon is already running (never spawns). fd or -1. */
int dlm_client_connect_existing(void);

/* Send one JSON request line and read one JSON response line.
 * Returns a malloc'd response string (no newline) or NULL on error. */
char *dlm_client_rpc(int fd, const char *json_request);

/* Read a single line from the socket (for streaming subscribe events).
 * Returns malloc'd line or NULL on EOF/error. */
char *dlm_client_read_line(int fd);

/* Buffered line reader for streaming flows (e.g. `watch`): reads the socket in
 * chunks and hands back one line at a time, instead of one syscall per byte.
 * Init once on a connected fd, then call _line repeatedly. Do not mix with
 * dlm_client_read_line()/dlm_client_rpc() on the same fd after init (the reader
 * may hold buffered bytes). */
#include <stddef.h>
typedef struct {
    int fd;
    size_t start, end; /* unconsumed window [start,end) in buf */
    char buf[8192];
} dlm_client_reader;

void dlm_client_reader_init(dlm_client_reader *r, int fd);
char *dlm_client_reader_line(dlm_client_reader *r);

/* Close a client socket fd returned by the connect functions. (Necessary
 * because a raw close() is wrong for Windows sockets.) */
void dlm_client_close(int fd);

#endif /* DLM_CLIENT_H */
