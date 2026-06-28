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

#endif /* DLM_CLIENT_H */
