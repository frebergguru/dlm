/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — small HTTP GET/POST-to-memory helper used by extractors and auth. */
#define _POSIX_C_SOURCE 200809L
#include "httpget.h"
#include "internal.h"
#include "dlm/dlm.h"

#include <curl/curl.h>

#define HTTPGET_UA "dlm/0.1 (+segmented-downloader)"
#define HTTPGET_MAX (64 * 1024 * 1024) /* cap response bodies at 64 MiB */

typedef struct {
    char *data;
    size_t len;
    size_t cap; /* allocated bytes (>= len + 1) */
} membuf;

static size_t write_mem(char *ptr, size_t size, size_t nmemb, void *userp)
{
    membuf *m = userp;
    size_t add = size * nmemb;
    if (m->len + add > HTTPGET_MAX) return 0;
    size_t need = m->len + add + 1; /* + NUL terminator */
    if (need > m->cap) {
        /* grow geometrically so a body assembled from many small chunks costs
         * O(n) total instead of O(n^2) reallocs/copies */
        size_t nc = m->cap ? m->cap : 4096;
        while (nc < need) nc *= 2;
        m->data = dlm_xrealloc(m->data, nc);
        m->cap = nc;
    }
    memcpy(m->data + m->len, ptr, add);
    m->len += add;
    m->data[m->len] = '\0';
    return add;
}

/* Build a curl header list. *ok is set to 0 if an append fails (OOM): callers
 * must abort rather than silently send the request with headers dropped (which
 * for archive.org would downgrade an authenticated request to anonymous). */
static struct curl_slist *build_list(const char *const *headers, int *ok)
{
    *ok = 1;
    struct curl_slist *l = NULL;
    if (!headers) return NULL;
    for (int i = 0; headers[i]; i++) {
        struct curl_slist *nl = curl_slist_append(l, headers[i]);
        if (!nl) { curl_slist_free_all(l); *ok = 0; return NULL; }
        l = nl;
    }
    return l;
}

static int http_do(const char *url, const char *post_fields,
                   const char *const *headers, char **body, size_t *len,
                   long *status)
{
    /* default the out-params up front so early-error returns never leave the
     * caller reading/freeing an uninitialised *body. */
    if (body) *body = NULL;
    if (len) *len = 0;
    if (status) *status = 0;

    dlm_global_init();
    CURL *c = curl_easy_init();
    if (!c) return DLM_ERR_NOMEM;

    membuf m = {NULL, 0, 0};
    int hl_ok = 1;
    struct curl_slist *hl = build_list(headers, &hl_ok);
    if (!hl_ok) { curl_easy_cleanup(c); return DLM_ERR_NOMEM; }

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 20L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, HTTPGET_UA);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_mem);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    if (post_fields) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, post_fields);
    }

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    if (status) *status = code;
    if (hl) curl_slist_free_all(hl);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        DLM_ERROR("http %s failed: %s", url, curl_easy_strerror(rc));
        free(m.data);
        if (body) *body = NULL;
        if (len) *len = 0;
        return DLM_ERR_NET;
    }
    if (len) *len = m.data ? m.len : 0;
    if (body) *body = m.data ? m.data : dlm_xstrdup("");
    else free(m.data);
    return DLM_OK;
}

int dlm_http_request(const char *url, const char *post_fields,
                     const char *const *headers, char **body, long *status)
{
    return http_do(url, post_fields, headers, body, NULL, status);
}

int dlm_http_get_blob(const char *url, const char *const *headers, char **body,
                      size_t *len, long *status)
{
    return http_do(url, NULL, headers, body, len, status);
}

int dlm_http_get(const char *url, const char *const *headers, char **body,
                 long *status)
{
    return dlm_http_request(url, NULL, headers, body, status);
}
