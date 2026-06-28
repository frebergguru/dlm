/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — native archive.org extractor.
 *
 * Resolves an archive.org item (details/download/metadata URL) into one task
 * per file via the Internet Archive Metadata API:
 *     GET https://archive.org/metadata/<identifier>
 *         -> { files:[{name,size,md5,sha1,...}], ... }
 * Download URLs use https://archive.org/download/<id>/<name>, which 302-redirect
 * to a storage node the engine follows. md5/sha1 are attached for verification.
 * The current IA auth (anonymous, S3 keys, or cookie) is applied to every
 * request so account-restricted items work when signed in.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "archiveorg.h"
#include "dlm/dlm.h"
#include "dlm/iaauth.h"
#include "httpget.h"
#include "internal.h"
#include "compat/compat.h"

#include <jansson.h>
#if !defined(_WIN32)
#  include <strings.h>
#endif

/* host is (something.)archive.org ? */
static int host_is_archiveorg(const char *url)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t hostlen = strcspn(p, "/");
    /* compare suffix against "archive.org" */
    const char *suffix = "archive.org";
    size_t sl = strlen(suffix);
    if (hostlen < sl) return 0;
    return strncasecmp(p + hostlen - sl, suffix, sl) == 0 &&
           (hostlen == sl || p[hostlen - sl - 1] == '.');
}

/* Split the path after the host into kind / id / file (file may be NULL).
 * Returns 0 on success. Caller frees *id and *file. */
static int parse_url(const char *url, char **kind, char **id, char **file)
{
    *kind = *id = *file = NULL;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    p += strcspn(p, "/"); /* now at first '/' of path (or end) */
    if (*p != '/') return -1;
    p++;

    size_t klen = strcspn(p, "/");
    if (klen == 0) return -1;
    char *k = dlm_xmalloc(klen + 1);
    memcpy(k, p, klen);
    k[klen] = '\0';
    p += klen;
    if (*p == '/') p++;

    /* identifier ends at next '/', '?' or '#' */
    size_t ilen = strcspn(p, "/?#");
    if (ilen == 0) { free(k); return -1; }
    char *i = dlm_xmalloc(ilen + 1);
    memcpy(i, p, ilen);
    i[ilen] = '\0';
    p += ilen;

    char *f = NULL;
    if (*p == '/') {
        p++;
        size_t flen = strcspn(p, "?#");
        if (flen > 0) {
            f = dlm_xmalloc(flen + 1);
            memcpy(f, p, flen);
            f[flen] = '\0';
        }
    }
    *kind = k;
    *id = i;
    *file = f;
    return 0;
}

int dlm_is_archiveorg_url(const char *url)
{
    if (!url || !host_is_archiveorg(url)) return 0;
    char *kind = NULL, *id = NULL, *file = NULL;
    if (parse_url(url, &kind, &id, &file) != 0) return 0;
    int ok = kind && id &&
             (!strcmp(kind, "details") || !strcmp(kind, "download") ||
              !strcmp(kind, "metadata"));
    free(kind);
    free(id);
    free(file);
    return ok;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* percent-decode in place; collapses %XX to a byte. */
static void percent_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        int hi, lo;
        if (*r == '%' && (hi = hexval(r[1])) >= 0 && (lo = hexval(r[2])) >= 0) {
            *w++ = (char)((hi << 4) | lo);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* percent-encode a file path for a URL, preserving '/'. */
static char *url_encode_path(const char *s)
{
    size_t n = strlen(s);
    char *out = dlm_xmalloc(n * 3 + 1);
    char *w = out;
    static const char hx[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == '/') {
            *w++ = (char)c;
        } else {
            *w++ = '%';
            *w++ = hx[c >> 4];
            *w++ = hx[c & 15];
        }
    }
    *w = '\0';
    return out;
}

static char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    return dlm_xstrdup(slash ? slash + 1 : path);
}

static char **dup_headers(char *const *h)
{
    if (!h) return NULL;
    int n = 0;
    while (h[n]) n++;
    char **out = dlm_xcalloc((size_t)n + 1, sizeof *out);
    for (int i = 0; i < n; i++) out[i] = dlm_xstrdup(h[i]);
    return out;
}

int dlm_extract_archiveorg(const char *url, dlm_extract_result *out)
{
    memset(out, 0, sizeof *out);
    char *kind = NULL, *id = NULL, *want_file = NULL;
    if (parse_url(url, &kind, &id, &want_file) != 0) return DLM_ERR_ARG;

    /* The API 'name' field is percent-decoded; the requested file came verbatim
     * from the URL path, so decode it before comparing (e.g. "My%20File.mp4"). */
    if (want_file) percent_decode(want_file);

    /* load auth and build header array for IA requests */
    ia_credentials cred;
    dlm_ia_load(&cred);
    char **auth = dlm_ia_auth_headers(&cred);

    /* The identifier is interpolated into request URLs; encode it so a stray
     * space or reserved char yields a well-formed URL. */
    char *id_enc = url_encode_path(id);
    char meta_url[1024];
    snprintf(meta_url, sizeof meta_url, "https://archive.org/metadata/%s", id_enc);

    char *body = NULL;
    long status = 0;
    int rc = dlm_http_get(meta_url, (const char *const *)auth, &body, &status);
    if (rc != DLM_OK || status >= 400 || !body) {
        DLM_ERROR("archive.org: metadata fetch failed (HTTP %ld) for %s", status, id);
        rc = (status == 403 || status == 401)
                 ? DLM_ERR_HTTP /* likely needs login */
                 : DLM_ERR_NET;
        goto done;
    }

    json_error_t je;
    json_t *root = json_loads(body, 0, &je);
    if (!root) { rc = DLM_ERR_NET; goto done; }

    json_t *files = json_object_get(root, "files");
    json_t *is_dark = json_object_get(root, "is_dark");
    if (json_is_true(is_dark)) {
        DLM_ERROR("archive.org: item '%s' is access-restricted (dark)", id);
        rc = DLM_ERR_HTTP;
        json_decref(root);
        goto done;
    }
    if (!json_is_array(files) || json_array_size(files) == 0) {
        DLM_ERROR("archive.org: no files for '%s'%s", id,
                  cred.mode == IA_AUTH_NONE ? " (try signing in)" : "");
        rc = DLM_ERR_HTTP;
        json_decref(root);
        goto done;
    }

    int cap = (int)json_array_size(files);
    out->tasks = dlm_xcalloc((size_t)cap, sizeof *out->tasks);
    out->count = 0;
    out->source = dlm_xstrdup("archive.org");

    size_t idx;
    json_t *f;
    json_array_foreach(files, idx, f) {
        const char *name = json_string_value(json_object_get(f, "name"));
        if (!name) continue;
        /* if a specific file was requested, keep only that one */
        if (want_file && strcmp(want_file, name) != 0) continue;

        dlm_task *t = &out->tasks[out->count];
        char *enc = url_encode_path(name);
        char dl[2048];
        snprintf(dl, sizeof dl, "https://archive.org/download/%s/%s", id_enc, enc);
        free(enc);
        t->url = dlm_xstrdup(dl);
        t->filename = basename_of(name);

        const char *sz = json_string_value(json_object_get(f, "size"));
        t->size = sz ? (int64_t)strtoll(sz, NULL, 10) : -1;
        const char *md5 = json_string_value(json_object_get(f, "md5"));
        const char *sha1 = json_string_value(json_object_get(f, "sha1"));
        t->md5 = md5 ? dlm_xstrdup(md5) : NULL;
        t->sha1 = sha1 ? dlm_xstrdup(sha1) : NULL;
        t->headers = dup_headers(auth);
        out->count++;
    }
    json_decref(root);

    if (out->count == 0) {
        DLM_ERROR("archive.org: requested file '%s' not found in '%s'",
                  want_file ? want_file : "?", id);
        rc = DLM_ERR_ARG;
        goto done;
    }
    DLM_INFO("archive.org: '%s' -> %d file(s) [%s]", id, out->count,
             dlm_ia_mode_str(cred.mode));
    rc = DLM_OK;

done:
    dlm_ia_free_headers(auth);
    dlm_ia_credentials_free(&cred);
    free(body);
    free(kind);
    free(id);
    free(id_enc);
    free(want_file);
    if (rc != DLM_OK) dlm_extract_result_free(out);
    return rc;
}
