/* libdlm — logging and small utility helpers. */
#define _POSIX_C_SOURCE 200809L
#include "dlm/dlm.h"
#include "internal.h"

#include <ctype.h>
#include <stdarg.h>

/* ---- logging ---------------------------------------------------------- */

dlm_log_level dlm_log_threshold(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("DLM_LOG");
        if (!e) cached = DLM_LOG_INFO;
        else if (!strcmp(e, "error")) cached = DLM_LOG_ERROR;
        else if (!strcmp(e, "warn")) cached = DLM_LOG_WARN;
        else if (!strcmp(e, "info")) cached = DLM_LOG_INFO;
        else if (!strcmp(e, "debug")) cached = DLM_LOG_DEBUG;
        else cached = DLM_LOG_INFO;
    }
    return (dlm_log_level)cached;
}

void dlm_logf(dlm_log_level lvl, const char *fmt, ...)
{
    static const char *names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char tbuf[32];
    strftime(tbuf, sizeof tbuf, "%H:%M:%S", &tm);
    fprintf(stderr, "%s.%03ld [%-5s] ", tbuf, ts.tv_nsec / 1000000L, names[lvl]);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- allocation ------------------------------------------------------- */

static void oom(void)
{
    fputs("dlm: out of memory\n", stderr);
    abort();
}

void *dlm_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

void *dlm_xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) oom();
    return p;
}

void *dlm_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) oom();
    return q;
}

char *dlm_xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = dlm_xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ---- time ------------------------------------------------------------- */

double dlm_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- url -> filename -------------------------------------------------- */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Percent-decode in place; collapses %XX to a byte. */
static void percent_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '%' && hexval(r[1]) >= 0 && hexval(r[2]) >= 0) {
            *w++ = (char)((hexval(r[1]) << 4) | hexval(r[2]));
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

char *dlm_filename_from_url(const char *url)
{
    if (!url) return dlm_xstrdup("download");

    /* skip scheme:// */
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;

    /* strip query and fragment by finding their start */
    size_t pathlen = strcspn(p, "?#");

    /* find last '/' within the path portion */
    const char *slash = NULL;
    for (size_t i = 0; i < pathlen; i++)
        if (p[i] == '/') slash = p + i;

    const char *name;
    size_t namelen;
    if (slash) {
        name = slash + 1;
        namelen = (size_t)(p + pathlen - name);
    } else {
        name = p;
        namelen = pathlen;
    }

    char *out;
    if (namelen == 0) {
        out = dlm_xstrdup("download");
    } else {
        out = dlm_xmalloc(namelen + 1);
        memcpy(out, name, namelen);
        out[namelen] = '\0';
        percent_decode(out);
        /* sanitise path separators that may survive decoding */
        for (char *c = out; *c; c++)
            if (*c == '/' || *c == '\\') *c = '_';
        if (out[0] == '\0') {
            free(out);
            out = dlm_xstrdup("download");
        }
    }
    return out;
}

/* ---- human bytes ------------------------------------------------------ */

void dlm_human_bytes(int64_t n, char *buf, size_t buflen)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    if (n < 0) {
        snprintf(buf, buflen, "?");
        return;
    }
    double v = (double)n;
    int u = 0;
    while (v >= 1024.0 && u < 5) {
        v /= 1024.0;
        u++;
    }
    if (u == 0)
        snprintf(buf, buflen, "%lld %s", (long long)n, units[u]);
    else
        snprintf(buf, buflen, "%.1f %s", v, units[u]);
}

/* ---- transfer rates --------------------------------------------------- */

int64_t dlm_parse_rate(const char *s)
{
    if (!s || !*s) return 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || v <= 0) return 0;
    while (*end == ' ') end++;
    double mult = 1.0;
    switch (tolower((unsigned char)*end)) {
    case 'k': mult = 1024.0; break;
    case 'm': mult = 1024.0 * 1024.0; break;
    case 'g': mult = 1024.0 * 1024.0 * 1024.0; break;
    case 't': mult = 1024.0 * 1024.0 * 1024.0 * 1024.0; break;
    case '\0': case 'b': mult = 1.0; break;
    default: mult = 1.0; break;
    }
    double bytes = v * mult;
    if (bytes < 1) bytes = 1;
    return (int64_t)bytes;
}

void dlm_format_rate(int64_t bps, char *buf, size_t buflen)
{
    if (bps <= 0) {
        snprintf(buf, buflen, "unlimited");
        return;
    }
    char b[32];
    dlm_human_bytes(bps, b, sizeof b);
    snprintf(buf, buflen, "%s/s", b);
}
