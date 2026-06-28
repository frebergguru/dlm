/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm/compat — small platform shims (sleep, exe path, hidden prompt,
 * signal/termination handling). */
#ifndef DLM_COMPAT_MISC_H
#define DLM_COMPAT_MISC_H

#include <stddef.h>

/* Sleep for the given number of milliseconds. */
void dlm_sleep_ms(unsigned ms);

/* Fill buf with the directory containing the current executable (no trailing
 * separator). Returns 0 on success, -1 if it could not be resolved. */
int dlm_self_dir(char *buf, size_t n);

/* Read a line from stdin with terminal echo disabled (for passwords). Returns a
 * malloc'd string without the trailing newline, or NULL on error/EOF. */
char *dlm_prompt_hidden(const char *prompt);

/* Install a process-wide handler invoked on Ctrl-C / termination requests
 * (SIGINT+SIGTERM on POSIX, console control events on Windows). On POSIX this
 * also ignores SIGPIPE. The callback runs from a signal/handler context, so it
 * should only set a flag. */
void dlm_install_term_handler(void (*fn)(void));

#endif /* DLM_COMPAT_MISC_H */
