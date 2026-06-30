/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — internal HTTP GET/POST-to-memory helper. */
#ifndef DLM_HTTPGET_H
#define DLM_HTTPGET_H

#include <stddef.h>

/* Perform a GET (post_fields==NULL) or POST request, capturing the body into a
 * malloc'd, NUL-terminated string (*body, caller frees) and the HTTP status.
 * `headers` is a NULL-terminated "Key: Value" array or NULL. Returns DLM_OK on
 * a completed transfer (check *status for the HTTP code). */
int dlm_http_request(const char *url, const char *post_fields,
                     const char *const *headers, char **body, long *status);

int dlm_http_get(const char *url, const char *const *headers, char **body,
                 long *status);

/* Like dlm_http_get but also reports the body length, so binary responses
 * (e.g. favicons) that may contain NUL bytes can be used safely. *body is
 * still NUL-terminated; caller frees. */
int dlm_http_get_blob(const char *url, const char *const *headers, char **body,
                      size_t *len, long *status);

/* case-insensitive prefix test (returns 1 if `s` starts with `p`) */
int dlm_ci_prefix(const char *s, const char *p);

/* Attach a session cookie to a curl easy handle via the cookie engine, scoped to
 * the URL host's domain and marked Secure (see httpget.c). `handle` is a CURL*.
 * Use this instead of a raw "Cookie:" header so the secret can't leak across a
 * cross-site or http redirect. */
void dlm_curl_set_scoped_cookie(void *handle, const char *url, const char *cookie_value);

#endif /* DLM_HTTPGET_H */
