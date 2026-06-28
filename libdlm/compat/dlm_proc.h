/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm/compat — child process spawning with optional stdout capture. */
#ifndef DLM_COMPAT_PROC_H
#define DLM_COMPAT_PROC_H

#include <stddef.h>

/* stdout/stderr capture mode for dlm_proc_spawn(). */
enum {
    DLM_SPAWN_INHERIT = 0,   /* child shares our stdio (no pipe) */
    DLM_SPAWN_STDOUT = 1,    /* capture child stdout via a pipe */
    DLM_SPAWN_STDOUT_ERR = 2 /* capture child stdout AND stderr (merged) */
};

typedef struct dlm_proc dlm_proc;

/* Spawn argv[0] (looked up on PATH if it has no path separator), NULL-terminated
 * argv. Returns an opaque handle or NULL on failure. */
dlm_proc *dlm_proc_spawn(const char *const argv[], int capture);

/* Read up to n bytes of captured output. timeout_ms < 0 blocks until data or
 * EOF; >= 0 returns -1 (no data yet) after the timeout so callers can re-check a
 * cancel flag. Returns >0 bytes, 0 at EOF, -1 on timeout/error. Only valid when
 * spawned with a capture mode. */
long dlm_proc_read(dlm_proc *p, void *buf, size_t n, int timeout_ms);

/* Request the child terminate (SIGTERM / TerminateProcess). */
void dlm_proc_terminate(dlm_proc *p);

/* Wait for the child to exit; stores its exit code (or -1 if abnormal). Returns
 * 0 on success, -1 on error. */
int dlm_proc_wait(dlm_proc *p, int *exit_code);

/* Release the handle (closes the capture pipe; does not wait). */
void dlm_proc_free(dlm_proc *p);

/* Spawn a detached, backgrounded process (no controlling terminal/console, stdio
 * to the null device). Used to auto-start the daemon. Returns 0 on success. */
int dlm_proc_spawn_detached(const char *const argv[]);

#endif /* DLM_COMPAT_PROC_H */
