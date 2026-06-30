/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — segmented HTTP(S) downloader with resume.
 *
 * Strategy:
 *   1. Probe the URL (HEAD, falling back to a Range: 0-0 GET) to learn the
 *      total size and whether the server honours byte ranges.
 *   2. Plan N segments over [0, total). Each segment is an independent libcurl
 *      easy handle requesting its byte range; the shared curl_multi loop drives
 *      them concurrently. Each write lands at its absolute offset via pwrite()
 *      into one pre-allocated sparse part file.
 *   3. A JSON sidecar journal records per-segment progress so an interrupted
 *      download resumes from where each segment left off.
 *   4. On success the part file is renamed to the final path and the journal
 *      removed.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "dlm/dlm.h"
#include "internal.h"
#include "httpget.h"
#include "compat/compat.h"

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdarg.h>
#include <sys/stat.h>
#if defined(_WIN32)
#  include <io.h>
#else
#  include <strings.h>
#  include <unistd.h>
#endif

#define DLM_UA "dlm/0.1 (+segmented-downloader)"
#define DLM_DEFAULT_CONNS 4
#define DLM_DEFAULT_MIN_SPLIT (1 << 20) /* 1 MiB */
#define DLM_DEFAULT_RETRIES 5
#define DLM_PROGRESS_INTERVAL 0.2       /* seconds */
#define DLM_JOURNAL_INTERVAL 1.0        /* seconds */

/* ---- types ------------------------------------------------------------ */

struct dlm_download_s;

typedef struct {
    struct dlm_download_s *dl;
    int index;
    int64_t start;   /* absolute inclusive start offset */
    int64_t end;     /* absolute inclusive end; -1 = open-ended/unknown size */
    int64_t done;    /* bytes already written for this segment */
    long retries;
    long status;     /* HTTP status of the current attempt */
    int checked;     /* validated status-vs-range on this attempt */
    int complete;
    int failed;      /* exhausted retries */
    CURL *easy;
} dlm_segment_t;

typedef struct dlm_download_s {
    char *url;
    char *out_path;
    char *part_path;
    char *journal_path;

    struct curl_slist *header_list;
    char *cookie;    /* IA session cookie, applied via the scoped cookie engine */

    int64_t total;   /* -1 unknown */
    int resumable;
    int nsegments;
    dlm_segment_t *segs;

    int fd;
    int64_t downloaded;
    int io_error;
    int range_ignored; /* server returned 200 for a ranged request */
    long http_status;  /* offending HTTP status on a DLM_ERR_HTTP failure, else 0 */

    long max_retries;
    int64_t max_speed; /* total bytes/sec cap; 0 = unlimited */
    volatile int *cancel;

    dlm_progress_cb on_progress;
    void *userdata;

    /* speed estimation */
    double last_t;
    int64_t last_bytes;
    double speed_bps;
} dlm_download_t;

/* ---- probe ------------------------------------------------------------ */

typedef struct {
    int accept_ranges_bytes;
    int64_t content_length;     /* -1 unknown */
    int64_t content_range_total;/* total parsed from Content-Range; -1 unknown */
} probe_headers_t;

static size_t probe_header_cb(char *buf, size_t size, size_t nmemb, void *userp)
{
    probe_headers_t *ph = userp;
    size_t len = size * nmemb;

    /* case-insensitive prefix match helper */
    if (len >= 14 && strncasecmp(buf, "accept-ranges:", 14) == 0) {
        if (len < 256) {
            char tmp[256];
            memcpy(tmp, buf + 14, len - 14);
            tmp[len - 14] = '\0';
            for (char *p = tmp; *p; p++) *p = (char)tolower((unsigned char)*p);
            if (strstr(tmp, "bytes")) ph->accept_ranges_bytes = 1;
        }
    } else if (len >= 15 && strncasecmp(buf, "content-length:", 15) == 0) {
        ph->content_length = (int64_t)strtoll(buf + 15, NULL, 10);
    } else if (len >= 14 && strncasecmp(buf, "content-range:", 14) == 0) {
        /* Content-Range: bytes 0-0/12345 */
        const char *slash = memchr(buf, '/', len);
        if (slash && slash + 1 < buf + len && slash[1] != '*')
            ph->content_range_total = (int64_t)strtoll(slash + 1, NULL, 10);
    }
    return len;
}

/* Abort the body transfer during the GET probe: returning 0 tells libcurl to
 * stop (CURLE_WRITE_ERROR), so a server that ignores "Range: 0-0" and answers
 * 200 doesn't make us stream the whole file just to read its headers. */
static size_t probe_abort_cb(char *p, size_t s, size_t n, void *u)
{
    (void)p; (void)s; (void)n; (void)u;
    return 0;
}

static void apply_common_opts(CURL *c, dlm_download_t *dl)
{
    curl_easy_setopt(c, CURLOPT_URL, dl->url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 20L);
    /* Lock the engine to web transfer protocols so an attacker-supplied URL or
     * a 30x redirect can't reach file:// (local-file read), scp/gopher/dict
     * (SSRF), etc. Applies to the initial request and every redirect hop. */
    dlm_curl_restrict_protocols(c, 1 /* allow ftp */);
    curl_easy_setopt(c, CURLOPT_SSLVERSION, (long)CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, DLM_UA);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    if (dl->header_list)
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, dl->header_list);
    if (dl->cookie)
        dlm_curl_set_scoped_cookie(c, dl->url, dl->cookie);
}

/* Probe size + range support. Sets dl->total and dl->resumable. */
static dlm_result probe(dlm_download_t *dl)
{
    probe_headers_t ph = {0, -1, -1};
    CURL *c = curl_easy_init();
    if (!c) return DLM_ERR_NOMEM;

    apply_common_opts(c, dl);
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L); /* HEAD */
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, probe_header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &ph);

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);

    int need_get_probe = 0;
    if (rc != CURLE_OK || code >= 400 || ph.content_length < 0)
        need_get_probe = 1;
    curl_easy_cleanup(c);

    if (need_get_probe) {
        /* Some servers reject HEAD; ask for one byte and read Content-Range. */
        ph.content_length = -1;
        c = curl_easy_init();
        if (!c) return DLM_ERR_NOMEM;
        apply_common_opts(c, dl);
        curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, probe_header_cb);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &ph);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, probe_abort_cb);
        rc = curl_easy_perform(c);
        code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(c);
        /* probe_abort_cb stops the body, which surfaces as CURLE_WRITE_ERROR;
         * the headers we needed already arrived, so treat that as success. */
        if (rc == CURLE_WRITE_ERROR) rc = CURLE_OK;
        if (rc != CURLE_OK || code >= 400) {
            /* Distinguish a real transport failure ("couldn't connect") from the
             * server answering with an error status — a 404/403/500 is a
             * *successful* curl transfer (rc==CURLE_OK), so reporting it as a
             * network error is misleading. */
            if (rc != CURLE_OK) {
                DLM_ERROR("probe failed: %s", curl_easy_strerror(rc));
                return DLM_ERR_NET;
            }
            DLM_ERROR("probe failed: server returned HTTP %ld", code);
            dl->http_status = code;
            return DLM_ERR_HTTP;
        }
        if (code == 206) {
            dl->resumable = 1;
            if (ph.content_range_total >= 0) dl->total = ph.content_range_total;
            /* The Content-Length on a 206 is just the 1-byte probe range, not
             * the file size; don't let it become dl->total below. (A 200 here
             * means the server ignored the range and its Content-Length *is*
             * the full size, so keep that one.) */
            ph.content_length = -1;
        }
    }

    if (dl->total < 0 && ph.content_length >= 0) dl->total = ph.content_length;
    if (ph.accept_ranges_bytes) dl->resumable = 1;

    DLM_INFO("probe: total=%lld resumable=%d", (long long)dl->total, dl->resumable);
    return DLM_OK;
}

/* ---- transfer callbacks ---------------------------------------------- */

static size_t seg_header_cb(char *buf, size_t size, size_t nmemb, void *userp)
{
    dlm_segment_t *s = userp;
    size_t len = size * nmemb;
    if (len >= 5 && strncmp(buf, "HTTP/", 5) == 0) {
        const char *sp = memchr(buf, ' ', len);
        s->status = sp ? atol(sp + 1) : 0;
        s->checked = 0; /* re-validate on next body write (handles redirects) */
    }
    return len;
}

static size_t seg_write_cb(char *ptr, size_t size, size_t nmemb, void *userp)
{
    dlm_segment_t *s = userp;
    dlm_download_t *dl = s->dl;
    size_t len = size * nmemb;

    if (dl->cancel && *dl->cancel) return 0; /* abort transfer */

    if (!s->checked) {
        s->checked = 1;
        int needs_range = (s->end >= 0) || (s->done > 0);
        if (needs_range && s->status == 200) {
            /* Server ignored our Range: writing at an offset would corrupt the
             * file. Abort and let the caller fall back to a single stream. */
            dl->range_ignored = 1;
            return 0;
        }
    }

    int64_t off = (int64_t)(s->start + s->done);
    long long w = dlm_pwrite(dl->fd, ptr, len, off);
    if (w < 0 || (size_t)w != len) {
        DLM_ERROR("pwrite failed at %lld: %s", (long long)off, strerror(errno));
        dl->io_error = 1;
        return 0;
    }
    s->done += len;
    dl->downloaded += len;
    return len;
}

/* Build (or rebuild for retry) a segment's easy handle with the right range. */
static CURL *seg_make_easy(dlm_segment_t *s)
{
    dlm_download_t *dl = s->dl;
    CURL *c = curl_easy_init();
    if (!c) return NULL;

    apply_common_opts(c, dl);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, seg_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, seg_header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, s);
    curl_easy_setopt(c, CURLOPT_PRIVATE, s);

    /* split a global rate cap across the segments so the sum approximates the
     * requested total throughput */
    if (dl->max_speed > 0) {
        int nseg = dl->nsegments > 0 ? dl->nsegments : 1;
        curl_off_t per = (curl_off_t)(dl->max_speed / nseg);
        if (per < 1) per = 1;
        curl_easy_setopt(c, CURLOPT_MAX_RECV_SPEED_LARGE, per);
    }

    int64_t from = s->start + s->done;
    char range[64];
    if (s->end >= 0) {
        snprintf(range, sizeof range, "%lld-%lld", (long long)from, (long long)s->end);
        curl_easy_setopt(c, CURLOPT_RANGE, range);
    } else if (s->done > 0) {
        snprintf(range, sizeof range, "%lld-", (long long)from);
        curl_easy_setopt(c, CURLOPT_RANGE, range);
    }
    s->status = 0;
    s->checked = 0;
    s->easy = c;
    return c;
}

/* ---- journal ---------------------------------------------------------- */

static void journal_save(dlm_download_t *dl)
{
    json_t *root = json_object();
    json_object_set_new(root, "version", json_integer(1));
    json_object_set_new(root, "url", json_string(dl->url));
    json_object_set_new(root, "total", json_integer(dl->total));
    json_object_set_new(root, "nsegments", json_integer(dl->nsegments));
    json_t *arr = json_array();
    for (int i = 0; i < dl->nsegments; i++) {
        json_t *o = json_object();
        json_object_set_new(o, "start", json_integer(dl->segs[i].start));
        json_object_set_new(o, "end", json_integer(dl->segs[i].end));
        json_object_set_new(o, "done", json_integer(dl->segs[i].done));
        json_array_append_new(arr, o);
    }
    json_object_set_new(root, "segments", arr);

    /* atomic: write tmp then rename */
    char *tmp = dlm_xmalloc(strlen(dl->journal_path) + 5);
    sprintf(tmp, "%s.tmp", dl->journal_path);
    if (json_dump_file(root, tmp, JSON_COMPACT) == 0)
        dlm_rename_replace(tmp, dl->journal_path);
    free(tmp);
    json_decref(root);
}

/* Returns 1 if a compatible journal was loaded and segment progress restored. */
static int journal_load(dlm_download_t *dl)
{
    json_error_t err;
    json_t *root = json_load_file(dl->journal_path, 0, &err);
    if (!root) return 0;

    int ok = 0;
    const char *url = json_string_value(json_object_get(root, "url"));
    int64_t total = json_integer_value(json_object_get(root, "total"));
    int nseg = (int)json_integer_value(json_object_get(root, "nsegments"));
    json_t *arr = json_object_get(root, "segments");

    if (url && strcmp(url, dl->url) == 0 && total == dl->total &&
        json_is_array(arr) && (int)json_array_size(arr) == nseg && nseg > 0) {
        dl->nsegments = nseg;
        dl->segs = dlm_xcalloc((size_t)nseg, sizeof *dl->segs);
        dl->downloaded = 0;
        for (int i = 0; i < nseg; i++) {
            json_t *o = json_array_get(arr, i);
            dl->segs[i].dl = dl;
            dl->segs[i].index = i;
            dl->segs[i].start = json_integer_value(json_object_get(o, "start"));
            dl->segs[i].end = json_integer_value(json_object_get(o, "end"));
            dl->segs[i].done = json_integer_value(json_object_get(o, "done"));
            dl->downloaded += dl->segs[i].done;
        }
        ok = 1;
        DLM_INFO("resuming: %lld/%lld bytes already present",
                 (long long)dl->downloaded, (long long)dl->total);
    }
    json_decref(root);
    return ok;
}

/* ---- planning --------------------------------------------------------- */

static void plan_segments(dlm_download_t *dl, int connections, int64_t min_split)
{
    int n = 1;
    if (dl->resumable && dl->total >= min_split && connections > 1) {
        n = connections;
        /* don't create segments smaller than ~min_split/... keep it simple:
         * cap n so each chunk is at least 1 byte and not absurdly small */
        int64_t max_n = dl->total / (min_split > 0 ? min_split : 1);
        if (max_n < 1) max_n = 1;
        if ((int64_t)n > max_n) n = (int)max_n;
        if (n < 1) n = 1;
    }

    dl->nsegments = n;
    dl->segs = dlm_xcalloc((size_t)n, sizeof *dl->segs);
    if (n == 1) {
        dl->segs[0].dl = dl;
        dl->segs[0].index = 0;
        dl->segs[0].start = 0;
        /* Single stream: leave end open (-1) so no Range header is sent. A whole
         * file fetched as one segment needs no range, and sending one only risks
         * a spurious "range ignored" abort when the server answers 200. Size is
         * carried by dl->total; completion is decided by CURLE_OK. */
        dl->segs[0].end = -1;
        dl->segs[0].done = 0;
        return;
    }
    int64_t chunk = dl->total / n;
    for (int i = 0; i < n; i++) {
        dl->segs[i].dl = dl;
        dl->segs[i].index = i;
        dl->segs[i].start = (int64_t)i * chunk;
        dl->segs[i].end = (i == n - 1) ? dl->total - 1 : (int64_t)(i + 1) * chunk - 1;
        dl->segs[i].done = 0;
    }
}

/* ---- progress --------------------------------------------------------- */

static void emit_progress(dlm_download_t *dl, int force)
{
    if (!dl->on_progress) return;
    double now = dlm_now();
    double dt = now - dl->last_t;
    if (!force && dt < DLM_PROGRESS_INTERVAL) return;
    if (dt > 0) {
        double inst = (double)(dl->downloaded - dl->last_bytes) / dt;
        /* exponential smoothing */
        dl->speed_bps = dl->speed_bps <= 0 ? inst : dl->speed_bps * 0.7 + inst * 0.3;
    }
    dl->last_t = now;
    dl->last_bytes = dl->downloaded;
    dl->on_progress(dl->userdata, dl->downloaded, dl->total, dl->speed_bps);
}

/* ---- driver ----------------------------------------------------------- */

static int seg_is_done(dlm_segment_t *s)
{
    if (s->end < 0) return 0; /* unknown size: completion decided by curl OK */
    return s->done >= (s->end - s->start + 1);
}

static dlm_result run_multi(dlm_download_t *dl)
{
    CURLM *multi = curl_multi_init();
    if (!multi) return DLM_ERR_NOMEM;

    int active = 0;
    for (int i = 0; i < dl->nsegments; i++) {
        dlm_segment_t *s = &dl->segs[i];
        if (seg_is_done(s)) {
            s->complete = 1;
            continue;
        }
        if (seg_make_easy(s)) {
            curl_multi_add_handle(multi, s->easy);
            active++;
        }
    }

    dl->last_t = dlm_now();
    dl->last_bytes = dl->downloaded;
    double last_journal = dlm_now();
    dlm_result result = DLM_OK;

    while (active > 0) {
        int running = 0;
        CURLMcode mc = curl_multi_perform(multi, &running);
        if (mc == CURLM_OK)
            curl_multi_poll(multi, NULL, 0, 200, NULL);

        /* drain completed transfers */
        int msgs = 0;
        CURLMsg *m;
        while ((m = curl_multi_info_read(multi, &msgs))) {
            if (m->msg != CURLMSG_DONE) continue;
            dlm_segment_t *s = NULL;
            curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, (char **)&s);
            CURLcode res = m->data.result;
            long code = 0;
            curl_easy_getinfo(m->easy_handle, CURLINFO_RESPONSE_CODE, &code);

            curl_multi_remove_handle(multi, m->easy_handle);
            curl_easy_cleanup(m->easy_handle);
            s->easy = NULL;
            active--;

            if (dl->cancel && *dl->cancel) { result = DLM_ERR_CANCELLED; goto cleanup; }
            if (dl->io_error) { result = DLM_ERR_IO; goto cleanup; }
            if (dl->range_ignored) { result = DLM_ERR_HTTP; goto cleanup; }

            int http_ok = (code == 200 || code == 206 || code == 0);
            /* For an open-ended segment (end==-1) curl's own Content-Length
             * check guards short reads when the size is known; when it isn't
             * (dl->total<0) we can only trust CURLE_OK. When it IS known, also
             * require the whole file so a clean mid-body close isn't accepted. */
            int len_ok = (s->end >= 0) ? seg_is_done(s)
                       : (dl->total < 0 || s->done >= dl->total);
            if (res == CURLE_OK && http_ok && len_ok) {
                s->complete = 1;
                DLM_DEBUG("segment %d complete (%lld bytes)", s->index,
                          (long long)s->done);
            } else {
                /* A permanent client error (404/403/...) won't recover, so don't
                 * waste retries on it; 408/429 and 5xx/network errors are worth
                 * retrying. Surface the HTTP status either way (DLM_ERR_HTTP) so
                 * the user isn't told "network error" for a server rejection. */
                int permanent = code >= 400 && code < 500 &&
                                code != 408 && code != 429;
                s->retries++;
                if (permanent || s->retries > dl->max_retries) {
                    DLM_ERROR("segment %d failed: %s (HTTP %ld)", s->index,
                              curl_easy_strerror(res), code);
                    s->failed = 1;
                    if (code >= 400) { dl->http_status = code; result = DLM_ERR_HTTP; }
                    else result = DLM_ERR_NET;
                    goto cleanup;
                }
                DLM_WARN("segment %d retry %ld/%ld: %s (HTTP %ld)", s->index,
                         s->retries, dl->max_retries, curl_easy_strerror(res), code);
                if (seg_make_easy(s)) {
                    curl_multi_add_handle(multi, s->easy);
                    active++;
                }
            }
        }

        emit_progress(dl, 0);
        if (dlm_now() - last_journal >= DLM_JOURNAL_INTERVAL) {
            /* flush the data before recording progress, so a crash can't leave
             * the journal claiming bytes that never reached disk (resume would
             * then skip them and silently corrupt the output). */
            dlm_fsync(dl->fd);
            journal_save(dl);
            last_journal = dlm_now();
        }
    }

cleanup:
    /* tear down any handles still attached (cancel/error paths) */
    for (int i = 0; i < dl->nsegments; i++) {
        if (dl->segs[i].easy) {
            curl_multi_remove_handle(multi, dl->segs[i].easy);
            curl_easy_cleanup(dl->segs[i].easy);
            dl->segs[i].easy = NULL;
        }
    }
    curl_multi_cleanup(multi);
    dlm_fsync(dl->fd);
    journal_save(dl);
    emit_progress(dl, 1);
    return result;
}

/* ---- public entrypoint ------------------------------------------------ */

static struct curl_slist *build_header_list(const char *const *headers,
                                            char **cookie_out)
{
    if (cookie_out) *cookie_out = NULL;
    struct curl_slist *list = NULL;
    if (!headers) return NULL;
    for (int i = 0; headers[i]; i++) {
        /* a "Cookie:" header is routed through the scoped cookie engine instead
         * of being sent raw, so it can't follow a redirect off-site or to http */
        if (cookie_out && dlm_ci_prefix(headers[i], "Cookie:")) {
            const char *v = headers[i] + 7;
            while (*v == ' ') v++;
            free(*cookie_out);
            *cookie_out = dlm_xstrdup(v);
            continue;
        }
        list = curl_slist_append(list, headers[i]);
    }
    return list;
}

/* Largest byte offset the journal claims is already written to the part file. */
static int64_t resume_extent(const dlm_download_t *dl)
{
    int64_t max = 0;
    for (int i = 0; i < dl->nsegments; i++) {
        int64_t ext = dl->segs[i].start + dl->segs[i].done;
        if (ext > max) max = ext;
    }
    return max;
}

/* Open (creating) the .dlmpart file. If need_extent>0 (a journal was loaded)
 * and the existing file is too small to hold those bytes — e.g. it was deleted
 * or truncated by something else between runs — *stale is set so the caller can
 * discard the stale progress and restart cleanly instead of resuming over holes. */
static dlm_result open_part_file(dlm_download_t *dl, int64_t need_extent, int *stale)
{
    if (stale) *stale = 0;
    /* O_NOFOLLOW: refuse a symlink planted at <out>.dlmpart (a local attacker
     * could otherwise redirect our ftruncate/pwrite onto a victim-owned file in
     * a shared download dir). O_CLOEXEC: don't leak the fd to helper children. */
    dl->fd = open(dl->part_path, O_RDWR | O_CREAT | DLM_O_CLOEXEC | DLM_O_NOFOLLOW | DLM_O_BINARY, 0644);
    if (dl->fd < 0) {
        DLM_ERROR("cannot open %s: %s", dl->part_path, strerror(errno));
        return DLM_ERR_IO;
    }
    if (stale && need_extent > 0) {
        struct stat st;
        int64_t have = (fstat(dl->fd, &st) == 0) ? (int64_t)st.st_size : 0;
        if (have < need_extent) {
            DLM_WARN("resume: %s holds %lld bytes but the journal expects >= %lld;"
                     " restarting clean", dl->part_path, (long long)have,
                     (long long)need_extent);
            *stale = 1;
        }
    }
    if (dl->total > 0) {
        if (dlm_ftruncate(dl->fd, dl->total) != 0) {
            DLM_ERROR("ftruncate %s: %s", dl->part_path, strerror(errno));
            return DLM_ERR_IO;
        }
    }
    return DLM_OK;
}

static int all_segments_complete(dlm_download_t *dl)
{
    for (int i = 0; i < dl->nsegments; i++)
        if (!dl->segs[i].complete) return 0;
    return 1;
}

dlm_result dlm_download_file(const dlm_options *opt)
{
    if (!opt || !opt->url || !opt->out_path) return DLM_ERR_ARG;
    dlm_global_init();

    dlm_download_t dl;
    memset(&dl, 0, sizeof dl);
    dl.url = dlm_xstrdup(opt->url);
    dl.out_path = dlm_xstrdup(opt->out_path);
    dl.part_path = dlm_xmalloc(strlen(dl.out_path) + 9);
    sprintf(dl.part_path, "%s.dlmpart", dl.out_path);
    dl.journal_path = dlm_xmalloc(strlen(dl.out_path) + 10);
    sprintf(dl.journal_path, "%s.dlmjson", dl.out_path);
    dl.total = -1;
    dl.resumable = 0;
    dl.fd = -1;
    dl.max_retries = opt->max_retries > 0 ? opt->max_retries : DLM_DEFAULT_RETRIES;
    dl.max_speed = opt->max_speed > 0 ? opt->max_speed : 0;
    dl.cancel = opt->cancel;
    dl.on_progress = opt->on_progress;
    dl.userdata = opt->userdata;
    dl.header_list = build_header_list(opt->headers, &dl.cookie);

    int connections = opt->connections > 0 ? opt->connections : DLM_DEFAULT_CONNS;
    int64_t min_split = opt->min_split_size > 0 ? opt->min_split_size
                                                : DLM_DEFAULT_MIN_SPLIT;

    dlm_result rc = probe(&dl);
    if (rc != DLM_OK) goto done;

    int resumed = journal_load(&dl);
    if (!resumed)
        plan_segments(&dl, connections, min_split);

    int stale = 0;
    rc = open_part_file(&dl, resumed ? resume_extent(&dl) : 0, &stale);
    if (rc != DLM_OK) goto done;
    if (stale) {
        /* journal outlived its part file; throw the progress away and re-plan
         * from zero (the file is now truncated to total, so every byte is
         * re-fetched and overwritten — no holes survive). */
        free(dl.segs);
        dl.segs = NULL;
        dl.downloaded = 0;
        plan_segments(&dl, connections, min_split);
    }

    rc = run_multi(&dl);

    /* Fallback: server ignored ranges -> redo as one sequential stream. */
    if (rc == DLM_ERR_HTTP && dl.range_ignored) {
        DLM_WARN("server ignored Range; restarting as single stream");
        free(dl.segs);
        dl.range_ignored = 0;
        dl.resumable = 0;
        dl.downloaded = 0;
        dl.nsegments = 1;
        dl.segs = dlm_xcalloc(1, sizeof *dl.segs);
        dl.segs[0].dl = &dl;
        dl.segs[0].start = 0;
        dl.segs[0].end = -1; /* no Range header (see plan_segments) so the
                              * server's 200 full-body response is accepted */
        if (dl.total > 0) dlm_ftruncate(dl.fd, dl.total);
        rc = run_multi(&dl);
    }

    if (rc == DLM_OK && all_segments_complete(&dl)) {
        dlm_fsync(dl.fd);
        close(dl.fd);
        dl.fd = -1;
        if (dlm_rename_replace(dl.part_path, dl.out_path) != 0) {
            DLM_ERROR("rename %s -> %s: %s", dl.part_path, dl.out_path, strerror(errno));
            rc = DLM_ERR_IO;
        } else {
            remove(dl.journal_path);
            DLM_INFO("done: %s (%lld bytes)", dl.out_path, (long long)dl.downloaded);
        }
    } else if (rc == DLM_OK) {
        rc = DLM_ERR_NET; /* incomplete without an explicit error */
    }

done:
    if (opt->http_status) *opt->http_status = dl.http_status;
    if (dl.fd >= 0) close(dl.fd);
    if (dl.header_list) curl_slist_free_all(dl.header_list);
    free(dl.cookie);
    free(dl.segs);
    free(dl.url);
    free(dl.out_path);
    free(dl.part_path);
    free(dl.journal_path);
    return rc;
}

/* ---- lifecycle / misc ------------------------------------------------- */

static int g_inited = 0;

dlm_result dlm_global_init(void)
{
    if (!g_inited) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) return DLM_ERR_NET;
        g_inited = 1;
    }
    return DLM_OK;
}

void dlm_global_cleanup(void)
{
    if (g_inited) {
        curl_global_cleanup();
        g_inited = 0;
    }
}

const char *dlm_strerror(dlm_result r)
{
    switch (r) {
    case DLM_OK: return "ok";
    case DLM_ERR_ARG: return "invalid argument";
    case DLM_ERR_NET: return "could not connect to the server";
    case DLM_ERR_IO: return "i/o error";
    case DLM_ERR_HTTP: return "server returned an error status";
    case DLM_ERR_NOMEM: return "out of memory";
    case DLM_ERR_CANCELLED: return "cancelled";
    }
    return "unknown error";
}

void dlm_http_error_str(long code, char *buf, size_t n)
{
    const char *reason;
    switch (code) {
    case 400: reason = "bad request"; break;
    case 401: reason = "unauthorized — login required"; break;
    case 403: reason = "forbidden — access denied"; break;
    case 404: case 410: reason = "file not found"; break;
    case 408: reason = "request timed out"; break;
    case 429: reason = "rate limited — try again later"; break;
    default:
        if (code >= 500) reason = "server error";
        else if (code >= 400) reason = "request rejected";
        else reason = NULL;
        break;
    }
    if (reason) snprintf(buf, n, "HTTP %ld (%s)", code, reason);
    else snprintf(buf, n, "HTTP %ld", code);
}

const char *dlm_version(void) { return "dlm 0.1.0"; }
