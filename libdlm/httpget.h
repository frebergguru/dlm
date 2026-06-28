/* libdlm — internal HTTP GET/POST-to-memory helper. */
#ifndef DLM_HTTPGET_H
#define DLM_HTTPGET_H

/* Perform a GET (post_fields==NULL) or POST request, capturing the body into a
 * malloc'd, NUL-terminated string (*body, caller frees) and the HTTP status.
 * `headers` is a NULL-terminated "Key: Value" array or NULL. Returns DLM_OK on
 * a completed transfer (check *status for the HTTP code). */
int dlm_http_request(const char *url, const char *post_fields,
                     const char *const *headers, char **body, long *status);

int dlm_http_get(const char *url, const char *const *headers, char **body,
                 long *status);

#endif /* DLM_HTTPGET_H */
