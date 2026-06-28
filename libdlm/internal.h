/* libdlm — internal helpers shared across translation units. Not installed. */
#ifndef DLM_INTERNAL_H
#define DLM_INTERNAL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- logging ---------------------------------------------------------- */
/* Levels controlled by the DLM_LOG env var: error|warn|info|debug (default
 * info). Logs go to stderr with a millisecond timestamp. */

typedef enum { DLM_LOG_ERROR = 0, DLM_LOG_WARN, DLM_LOG_INFO, DLM_LOG_DEBUG } dlm_log_level;

dlm_log_level dlm_log_threshold(void);
void dlm_logf(dlm_log_level lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define DLM_LOG(lvl, ...)                                                      \
    do {                                                                      \
        if ((lvl) <= dlm_log_threshold()) dlm_logf((lvl), __VA_ARGS__);       \
    } while (0)

#define DLM_ERROR(...) DLM_LOG(DLM_LOG_ERROR, __VA_ARGS__)
#define DLM_WARN(...) DLM_LOG(DLM_LOG_WARN, __VA_ARGS__)
#define DLM_INFO(...) DLM_LOG(DLM_LOG_INFO, __VA_ARGS__)
#define DLM_DEBUG(...) DLM_LOG(DLM_LOG_DEBUG, __VA_ARGS__)

/* ---- small helpers ---------------------------------------------------- */

/* strdup that aborts on OOM (callers treat memory as infallible). */
char *dlm_xstrdup(const char *s);
void *dlm_xmalloc(size_t n);
void *dlm_xcalloc(size_t n, size_t sz);
void *dlm_xrealloc(void *p, size_t n);

/* Monotonic clock in seconds (double). */
double dlm_now(void);

/* Derive a filename from a URL (basename of the path, percent-decoded, query
 * stripped). Returns a malloc'd string; falls back to "download" when empty.
 * Caller frees. */
char *dlm_filename_from_url(const char *url);

/* Format a byte count into a compact human string (e.g. "12.3 MiB") into buf. */
void dlm_human_bytes(int64_t n, char *buf, size_t buflen);

#endif /* DLM_INTERNAL_H */
