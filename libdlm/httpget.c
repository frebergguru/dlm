/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — small HTTP GET/POST-to-memory helper used by extractors and auth. */
#define _POSIX_C_SOURCE 200809L
#include "httpget.h"
#include "internal.h"
#include "dlm/dlm.h"

#include <curl/curl.h>
#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif
#include <ctype.h>
#include <string.h>

#define HTTPGET_UA DLM_USER_AGENT
#define HTTPGET_MAX (64 * 1024 * 1024) /* cap response bodies at 64 MiB */

/* Restrict a handle to web protocols (and ftp if allow_ftp) on both the request
 * and redirects, so an attacker URL/redirect can't reach file:// (local-file
 * read) or scp/gopher/dict (SSRF). The *_STR options need libcurl 7.85; fall
 * back to the always-available bitmask so the restriction applies on any
 * version (a missing restriction would silently re-open the hole). */
void dlm_curl_restrict_protocols(void *handle, int allow_ftp)
{
    CURL *c = handle;
#if LIBCURL_VERSION_NUM >= 0x075500
    const char *p = allow_ftp ? "http,https,ftp,ftps" : "http,https";
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, p);
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, p);
#else
    long p = CURLPROTO_HTTP | CURLPROTO_HTTPS |
             (allow_ftp ? (CURLPROTO_FTP | CURLPROTO_FTPS) : 0);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, p);
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, p);
#endif
}

/* case-insensitive "does s start with prefix p?" */
int dlm_ci_prefix(const char *s, const char *p)
{
    for (; *p; s++, p++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*p)) return 0;
    return 1;
}

/* Registrable-ish cookie domain ".<last two labels>" of the URL's host
 * (e.g. https://archive.org/... or ia6.us.archive.org -> ".archive.org"). */
static void cookie_domain_of(const char *url, char *buf, size_t n)
{
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    size_t hl = strcspn(h, "/:?#");
    char host[256];
    if (hl >= sizeof host) hl = sizeof host - 1;
    memcpy(host, h, hl);
    host[hl] = '\0';
    const char *dom = host;
    char *last = strrchr(host, '.');
    if (last) {
        *last = '\0';
        char *prev = strrchr(host, '.');
        *last = '.';
        dom = prev ? prev + 1 : host;
    }
    snprintf(buf, n, ".%s", dom);
}

/* Attach a session cookie ("name=val; name2=val2") via libcurl's cookie engine,
 * scoped to the request host's registrable domain and marked Secure. Unlike a
 * raw "Cookie:" header (which libcurl re-sends verbatim on every redirect hop,
 * including off-site and http downgrades), an engine cookie is sent ONLY to the
 * matching domain over https — so it can't leak if a redirect leaves the site. */
void dlm_curl_set_scoped_cookie(void *handle, const char *url, const char *cookie_value)
{
    CURL *c = handle;
    if (!c || !cookie_value || !*cookie_value) return;
    char domain[260];
    cookie_domain_of(url, domain, sizeof domain);
    curl_easy_setopt(c, CURLOPT_COOKIEFILE, ""); /* enable in-memory cookie engine */
    char *dup = dlm_xstrdup(cookie_value);
    char *save = NULL;
    /* one Set-Cookie per pair: libcurl reads only the first "k=v" as the cookie
     * and the rest of the line as attributes, so pairs must be split. */
    for (char *tok = strtok_r(dup, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        if (!strchr(tok, '=')) continue;
        char line[2048];
        snprintf(line, sizeof line, "Set-Cookie: %s; domain=%s; path=/; secure",
                 tok, domain);
        curl_easy_setopt(c, CURLOPT_COOKIELIST, line);
    }
    free(dup);
}

typedef struct {
    char *data;
    size_t len;
    size_t cap; /* allocated bytes (>= len + 1) */
    size_t max; /* body cap in bytes */
} membuf;

/* ---- public-address-only connections ----------------------------------- */

/* True for addresses a background fetch must never reach: loopback, private,
 * link-local, CGNAT, multicast/reserved, unspecified, IPv6 ULA/link-local and
 * IPv4-mapped forms of those. Checked on the address curl actually connects
 * to, so it also covers redirects and DNS names that resolve inward. */
static int ip4_internal(const unsigned char a[4])
{
    return a[0] == 0 || a[0] == 10 || a[0] == 127 ||
           (a[0] == 100 && (a[1] & 0xC0) == 64) ||   /* 100.64/10 CGNAT */
           (a[0] == 169 && a[1] == 254) ||
           (a[0] == 172 && (a[1] & 0xF0) == 16) ||
           (a[0] == 192 && a[1] == 168) ||
           (a[0] == 192 && a[1] == 0 && a[2] == 0) ||
           (a[0] == 198 && (a[1] & 0xFE) == 18) ||   /* 198.18/15 */
           a[0] >= 224;                               /* multicast/reserved */
}

static int addr_internal(const struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        return ip4_internal((const unsigned char *)&in->sin_addr);
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        const unsigned char *b = (const unsigned char *)&in6->sin6_addr;
        static const unsigned char zero[16] = {0};
        static const unsigned char loop[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        if (!memcmp(b, zero, 16) || !memcmp(b, loop, 16)) return 1;
        if ((b[0] & 0xFE) == 0xFC) return 1;                 /* fc00::/7 */
        if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return 1; /* fe80::/10 */
        if (b[0] == 0xFF) return 1;                          /* multicast */
        static const unsigned char v4map[12] = {0,0,0,0,0,0,0,0,0,0,0xFF,0xFF};
        if (!memcmp(b, v4map, 12)) return ip4_internal(b + 12);
        return 0;
    }
    return 1; /* anything else (AF_UNIX...) is not a public web address */
}

static curl_socket_t public_only_opensocket(void *clientp, curlsocktype purpose,
                                            struct curl_sockaddr *addr)
{
    (void)clientp;
    if (purpose != CURLSOCKTYPE_IPCXN || addr_internal(&addr->addr)) {
        DLM_WARN("http: refusing connection to a non-public address");
        return CURL_SOCKET_BAD;
    }
    return socket(addr->family, addr->socktype, addr->protocol);
}

static size_t write_mem(char *ptr, size_t size, size_t nmemb, void *userp)
{
    membuf *m = userp;
    size_t add = size * nmemb;
    if (m->len + add > m->max) return 0;
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
static struct curl_slist *build_list(const char *const *headers, int *ok,
                                     char **cookie_out)
{
    *ok = 1;
    if (cookie_out) *cookie_out = NULL;
    struct curl_slist *l = NULL;
    if (!headers) return NULL;
    for (int i = 0; headers[i]; i++) {
        /* pull any "Cookie:" header out of the raw list so it can go through the
         * cookie engine (scoped + Secure) instead of leaking across redirects. */
        if (cookie_out && dlm_ci_prefix(headers[i], "Cookie:")) {
            const char *v = headers[i] + 7;
            while (*v == ' ') v++;
            free(*cookie_out);
            *cookie_out = dlm_xstrdup(v);
            continue;
        }
        struct curl_slist *nl = curl_slist_append(l, headers[i]);
        if (!nl) { curl_slist_free_all(l); *ok = 0; return NULL; }
        l = nl;
    }
    return l;
}

static int http_do(const char *url, const char *post_fields,
                   const char *const *headers, char **body, size_t *len,
                   long *status, size_t max_bytes, int public_only)
{
    /* default the out-params up front so early-error returns never leave the
     * caller reading/freeing an uninitialised *body. */
    if (body) *body = NULL;
    if (len) *len = 0;
    if (status) *status = 0;

    dlm_global_init();
    CURL *c = curl_easy_init();
    if (!c) return DLM_ERR_NOMEM;

    membuf m = {NULL, 0, 0, max_bytes ? max_bytes : HTTPGET_MAX};
    int hl_ok = 1;
    char *cookie = NULL;
    struct curl_slist *hl = build_list(headers, &hl_ok, &cookie);
    if (!hl_ok) { curl_easy_cleanup(c); free(cookie); return DLM_ERR_NOMEM; }
    if (cookie) { dlm_curl_set_scoped_cookie(c, url, cookie); free(cookie); }

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 20L);
    /* restrict to web protocols (no file://, scp, gopher, dict) on the request
     * and on redirects — defends the metadata/API fetches against SSRF too */
    dlm_curl_restrict_protocols(c, 0 /* http/https only */);
    curl_easy_setopt(c, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, HTTPGET_UA);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_mem);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    if (public_only) {
        curl_easy_setopt(c, CURLOPT_OPENSOCKETFUNCTION, public_only_opensocket);
        curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        /* a declared Content-Length over the cap fails before any body */
        curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)m.max);
    }
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
    return http_do(url, post_fields, headers, body, NULL, status, 0, 0);
}

int dlm_http_get_blob(const char *url, const char *const *headers, char **body,
                      size_t *len, long *status)
{
    return http_do(url, NULL, headers, body, len, status, 0, 0);
}

int dlm_http_get_public(const char *url, char **body, size_t *len, long *status,
                        size_t max_bytes)
{
    return http_do(url, NULL, NULL, body, len, status, max_bytes, 1);
}

int dlm_http_get(const char *url, const char *const *headers, char **body,
                 long *status)
{
    return dlm_http_request(url, NULL, headers, body, status);
}
