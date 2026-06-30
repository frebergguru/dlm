/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — yt-dlp extractor.
 *
 * For any URL not handled natively, we shell out to yt-dlp to resolve the page
 * into a concrete media stream:
 *     yt-dlp -J --no-warnings --no-playlist <url>
 * The JSON it prints describes the formats. A progressive http(s) format with a
 * direct URL becomes a normal engine task (segmented download, with yt-dlp's
 * required http_headers passed through). A fragmented stream (HLS/DASH, or a
 * merge of separate audio+video) is marked delegate=1 so the download is handed
 * back to yt-dlp (which handles segment fetching, muxing and decryption).
 *
 * Parsing is split out (dlm_ytdlp_parse) so it can be unit-tested against fixed
 * JSON without invoking yt-dlp.
 */
#define _POSIX_C_SOURCE 200809L
#include "ytdlp.h"
#include "dlm/dlm.h"
#include "dlm/tools.h"
#include "internal.h"
#include "compat/compat.h"

#include <jansson.h>

/* ---- helpers ---------------------------------------------------------- */

static char *sanitize_filename(const char *s)
{
    if (!s || !*s) return dlm_xstrdup("download");
    char *out = dlm_xstrdup(s);
    for (char *p = out; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':' || (unsigned char)*p < 0x20)
            *p = '_';
    /* A leading '.' (dotfile, or "."/"..") or '-' (option-looking) is risky once
     * this becomes a basename; neutralise it. Separators are already gone, so
     * the result can't traverse out of the target directory. */
    if (out[0] == '.' || out[0] == '-') out[0] = '_';
    return out;
}

/* Convert a yt-dlp http_headers object {"K":"V",...} to a "K: V" array. */
static char **headers_from_json(json_t *hdrs)
{
    if (!json_is_object(hdrs)) return NULL;
    size_t n = json_object_size(hdrs);
    if (n == 0) return NULL;
    char **out = dlm_xcalloc(n + 1, sizeof *out);
    int i = 0;
    const char *key;
    json_t *val;
    json_object_foreach(hdrs, key, val) {
        const char *v = json_string_value(val);
        if (!v) continue;
        /* drop header name/value with embedded CR/LF (or controls): yt-dlp's
         * http_headers is remote-influenced and must not be able to inject
         * extra request headers via a smuggled newline. */
        if (strpbrk(key, "\r\n") || strpbrk(v, "\r\n")) continue;
        size_t len = strlen(key) + strlen(v) + 3;
        char *line = dlm_xmalloc(len);
        snprintf(line, len, "%s: %s", key, v);
        out[i++] = line;
    }
    out[i] = NULL;
    return out;
}

static int is_fragmented_proto(const char *proto)
{
    if (!proto) return 0;
    return strstr(proto, "m3u8") != NULL || strstr(proto, "dash") != NULL ||
           strstr(proto, "ism") != NULL || strstr(proto, "f4m") != NULL;
}

/* Build one task from a single yt-dlp info dict. Returns 0 on success. */
static int task_from_info(json_t *info, const char *input_url, dlm_task *t)
{
    memset(t, 0, sizeof *t);

    const char *title = json_string_value(json_object_get(info, "title"));
    const char *ext = json_string_value(json_object_get(info, "ext"));
    const char *page = json_string_value(json_object_get(info, "webpage_url"));
    const char *direct = json_string_value(json_object_get(info, "url"));
    const char *proto = json_string_value(json_object_get(info, "protocol"));
    json_t *reqf = json_object_get(info, "requested_formats");
    json_t *frags = json_object_get(info, "fragments");
    json_t *hdrs = json_object_get(info, "http_headers");

    /* yt-dlp tags media it couldn't identify with an "unknown"/"unknown_video"
     * ext; for a generic direct file that yields a useless name like
     * "100Mb.unknown_video". Flag it so the progressive branch below can recover
     * the real name from the URL basename instead. */
    int junk_ext = !ext || !*ext || strcmp(ext, "unknown") == 0 ||
                   strcmp(ext, "unknown_video") == 0;

    /* filename: <title>.<ext> */
    char *base = sanitize_filename(title ? title : "download");
    size_t fnlen = strlen(base) + (ext ? strlen(ext) : 3) + 2;
    t->filename = dlm_xmalloc(fnlen);
    snprintf(t->filename, fnlen, "%s.%s", base, ext ? ext : "bin");
    free(base);

    int fragmented = is_fragmented_proto(proto) ||
                     (json_is_array(reqf) && json_array_size(reqf) > 1) ||
                     (json_is_array(frags) && json_array_size(frags) > 0);

    if (!fragmented && direct && (proto == NULL ||
        strncmp(proto, "http", 4) == 0)) {
        /* progressive http(s): segmented engine download */
        t->url = dlm_xstrdup(direct);
        t->delegate = 0;
        t->headers = headers_from_json(hdrs);
        json_t *fs = json_object_get(info, "filesize");
        if (!json_is_integer(fs)) fs = json_object_get(info, "filesize_approx");
        t->size = json_is_integer(fs) ? json_integer_value(fs) : -1;
        /* a real direct file yt-dlp couldn't identify: name it from the URL
         * (e.g. "100Mb.dat") rather than "<title>.unknown_video" */
        if (junk_ext) {
            free(t->filename);
            t->filename = dlm_filename_from_url(direct);
        }
    } else {
        /* fragmented / merge / unknown: hand back to yt-dlp */
        const char *du = page ? page : input_url;
        if (!du) { free(t->filename); t->filename = NULL; return -1; } /* nothing to delegate */
        t->url = dlm_xstrdup(du);
        t->delegate = 1;
        t->size = -1;
    }
    return 0;
}

/* ---- parse (testable) ------------------------------------------------- */

int dlm_ytdlp_parse(const char *json_text, const char *input_url,
                    dlm_extract_result *out)
{
    memset(out, 0, sizeof *out);
    json_error_t e;
    json_t *root = json_loads(json_text, 0, &e);
    if (!root) {
        DLM_ERROR("yt-dlp: unparseable JSON: %s", e.text);
        return DLM_ERR_NET;
    }
    /* A usable info dict is always a JSON object (single video or playlist). A
     * bare null/array (e.g. yt-dlp printed "null" for a removed video) is not
     * something we can download — reject so the caller falls back to direct. */
    if (!json_is_object(root)) {
        json_decref(root);
        return DLM_ERR_NET;
    }

    out->source = dlm_xstrdup("yt-dlp");
    /* root title: playlist title for a playlist, video title for a single item;
     * names the package / save subfolder. */
    const char *yt_title = json_string_value(json_object_get(root, "title"));
    if (yt_title && *yt_title) out->title = dlm_xstrdup(yt_title);
    json_t *entries = json_object_get(root, "entries");
    if (json_is_array(entries) && json_array_size(entries) > 0) {
        size_t n = json_array_size(entries);
        out->tasks = dlm_xcalloc(n, sizeof *out->tasks);
        size_t idx;
        json_t *ent;
        json_array_foreach(entries, idx, ent) {
            if (json_is_object(ent) &&
                task_from_info(ent, input_url, &out->tasks[out->count]) == 0)
                out->count++;
        }
    } else {
        out->tasks = dlm_xcalloc(1, sizeof *out->tasks);
        if (task_from_info(root, input_url, &out->tasks[0]) == 0)
            out->count = 1;
    }
    json_decref(root);

    if (out->count == 0) { dlm_extract_result_free(out); return DLM_ERR_NET; }
    return DLM_OK;
}

/* ---- spawn yt-dlp ----------------------------------------------------- */

/* Run yt-dlp with the given args, capturing stdout into a malloc'd string. */
static int run_capture(const char *const argv[], char **out, int *exit_code)
{
    dlm_proc *p = dlm_proc_spawn(argv, DLM_SPAWN_STDOUT);
    if (!p) {
        DLM_ERROR("yt-dlp: cannot spawn (is it installed?)");
        return -1;
    }

    /* Cap total capture so a runaway child can't drive an unbounded (and, via
     * the doubling, overflow-prone) allocation. yt-dlp -J output is tiny. */
    const size_t MAX_CAPTURE = (size_t)256 * 1024 * 1024;
    size_t cap = 65536, len = 0;
    char *buf = dlm_xmalloc(cap);
    char tmp[8192];
    int too_big = 0;
    long got;
    while ((got = dlm_proc_read(p, tmp, sizeof tmp, -1)) != 0) {
        if (got < 0) continue;
        size_t need = len + (size_t)got + 1;
        if (need > MAX_CAPTURE) { too_big = 1; break; }
        if (need > cap) {
            while (cap < need) cap *= 2;
            buf = dlm_xrealloc(buf, cap);
        }
        memcpy(buf + len, tmp, (size_t)got);
        len += (size_t)got;
    }
    buf[len] = '\0';

    if (too_big) dlm_proc_terminate(p);
    int ec = -1;
    dlm_proc_wait(p, &ec);
    dlm_proc_free(p);
    if (too_big) {
        DLM_ERROR("yt-dlp: output exceeded %zu bytes; aborting", MAX_CAPTURE);
        free(buf);
        *out = NULL;
        return -1;
    }
    if (exit_code) *exit_code = ec;
    *out = buf;
    return 0;
}

int dlm_extract_ytdlp(const char *url, dlm_extract_result *out)
{
    dlm_tools_ensure_ready(1); /* fetch yt-dlp on first use if missing */
    /* No --no-playlist here: a playlist/season URL expands to one info dict per
     * entry (dlm_ytdlp_parse walks "entries"), so the whole set is staged as a
     * package. --no-playlist is kept on the per-episode download (queue.c) so a
     * single confirmed link fetches just that video.
     *
     * --ignore-errors so a playlist with a few unavailable videos still yields
     * the rest (yt-dlp emits null for the bad entries, which the parser skips)
     * instead of aborting the whole crawl. */
    const char *argv[] = {dlm_tool_path("yt-dlp"), "-J", "--no-warnings",
                          "--ignore-errors", "--", url, NULL};
    char *json = NULL;
    int ec = 0;
    if (run_capture(argv, &json, &ec) != 0) {
        free(json);
        return DLM_ERR_NET;
    }
    if (!json || !*json) {
        DLM_ERROR("yt-dlp: no output (exit %d) for %s", ec, url);
        free(json);
        return DLM_ERR_NET;
    }
    /* yt-dlp can exit non-zero yet still emit a usable info JSON — a playlist
     * where some entries failed exits 1 but lists the good ones. Parse whatever
     * came back; dlm_ytdlp_parse rejects genuinely empty/unusable output, and
     * the caller then falls back to a direct download. */
    int rc = dlm_ytdlp_parse(json, url, out);
    if (rc != DLM_OK)
        DLM_ERROR("yt-dlp: extraction failed (exit %d) for %s", ec, url);
    free(json);
    return rc;
}
