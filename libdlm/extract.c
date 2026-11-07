/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — extractor dispatch.
 *
 * Routes a URL to the right extractor. archive.org is handled natively; a URL
 * that looks like a direct file is fetched by the engine; anything else is
 * resolved through yt-dlp, and if yt-dlp rejects it we fall back to a direct
 * engine download so generic files still work.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "dlm/extract.h"
#include "dlm/dlm.h"
#include "extractors/archiveorg.h"
#include "extractors/ytdlp.h"
#include "internal.h"

#include <ctype.h>
#include "compat/compat.h" /* strcasecmp on all platforms */
#if !defined(_WIN32)
#  include <strings.h>
#endif

void dlm_extract_result_free(dlm_extract_result *r)
{
    if (!r) return;
    for (int i = 0; i < r->count; i++) {
        dlm_task *t = &r->tasks[i];
        free(t->url);
        free(t->filename);
        free(t->md5);
        free(t->sha1);
        if (t->headers) {
            for (int j = 0; t->headers[j]; j++) free(t->headers[j]);
            free(t->headers);
        }
    }
    free(r->tasks);
    free(r->source);
    free(r->title);
    memset(r, 0, sizeof *r);
}

/* Build a single direct-download task from a URL. */
static int extract_direct(const char *url, dlm_extract_result *out)
{
    memset(out, 0, sizeof *out);
    out->tasks = dlm_xcalloc(1, sizeof *out->tasks);
    out->count = 1;
    out->source = dlm_xstrdup("direct");
    out->tasks[0].url = dlm_xstrdup(url);
    out->tasks[0].filename = dlm_filename_from_url(url);
    out->tasks[0].size = -1;
    return DLM_OK;
}

/* Common directly-downloadable file extensions. A URL whose path basename ends
 * in one of these is fetched directly (segmented); anything else (a web page,
 * a stream) is resolved through yt-dlp. */
static const char *DIRECT_EXTS[] = {
    "zip","tar","gz","tgz","bz2","xz","7z","rar","iso","img","exe","dmg","pkg",
    "deb","rpm","apk","msi","bin","pdf","epub","mobi","djvu","azw3","cbz","cbr",
    "mp3","flac","wav","ogg","opus","m4a","aac","mp4","mkv","webm","avi","mov",
    "wmv","flv","ts","jpg","jpeg","png","gif","webp","bmp","svg","txt","csv",
    "json","xml","srt","md","doc","docx","xls","xlsx","ppt","pptx","wasm",
    "dat","zst","lz","lzma","cab","appimage","jar","war","whl","gem","flatpak",
    "snap","qcow2","vdi","vmdk","tif","tiff","ico","tsv","yaml","yml","toml",
    "rtf","odt","ods","odp","wma","aiff","m4b","mpg","mpeg","3gp","m2ts", NULL
};

static int looks_like_direct_file(const char *url)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t pathlen = strcspn(p, "?#");
    /* basename within the path portion */
    const char *base = p, *end = p + pathlen;
    for (const char *c = p; c < end; c++)
        if (*c == '/') base = c + 1;
    const char *dot = NULL;
    for (const char *c = base; c < end; c++)
        if (*c == '.') dot = c;
    if (!dot || dot + 1 >= end) return 0;
    size_t extlen = (size_t)(end - dot - 1);
    char ext[16];
    if (extlen == 0 || extlen >= sizeof ext) return 0;
    for (size_t i = 0; i < extlen; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    ext[extlen] = '\0';
    for (int i = 0; DIRECT_EXTS[i]; i++)
        if (strcmp(ext, DIRECT_EXTS[i]) == 0) return 1;
    return 0;
}

/* Is `name` already used (case-insensitively, as on Windows/macOS) by any task
 * other than `self`? */
static int name_taken(const dlm_extract_result *r, int self, const char *name)
{
    for (int j = 0; j < r->count; j++) {
        if (j == self || !r->tasks[j].filename) continue;
        if (strcasecmp(r->tasks[j].filename, name) == 0) return 1;
    }
    return 0;
}

void dlm_extract_dedupe_names(dlm_extract_result *r)
{
    if (!r || !r->tasks) return;
    for (int i = 1; i < r->count; i++) {
        char *fn = r->tasks[i].filename;
        if (!fn) continue;
        /* only rename when an EARLIER task already has this name, so the first
         * occurrence (and every non-colliding name) keeps its name unchanged */
        int clash = 0;
        for (int j = 0; j < i && !clash; j++)
            if (r->tasks[j].filename && strcasecmp(r->tasks[j].filename, fn) == 0) clash = 1;
        if (!clash) continue;

        /* split "stem.ext" at the last dot (a leading dot is not an extension) */
        const char *dot = strrchr(fn, '.');
        if (dot == fn) dot = NULL;
        size_t stemlen = dot ? (size_t)(dot - fn) : strlen(fn);
        const char *ext = dot ? dot : "";
        size_t cap = strlen(fn) + 24;
        char *cand = dlm_xmalloc(cap);
        for (long k = 2;; k++) {
            snprintf(cand, cap, "%.*s (%ld)%s", (int)stemlen, fn, k, ext);
            if (!name_taken(r, i, cand)) break;
        }
        free(r->tasks[i].filename);
        r->tasks[i].filename = cand;
    }
}

int dlm_extract(const char *url, dlm_extract_result *out)
{
    if (!url || !out) return DLM_ERR_ARG;
    int rc;
    if (dlm_is_archiveorg_url(url))
        rc = dlm_extract_archiveorg(url, out);
    else if (looks_like_direct_file(url))
        rc = extract_direct(url, out);
    else {
        /* a web page / stream: resolve via yt-dlp */
        rc = dlm_extract_ytdlp(url, out);
        if (rc != DLM_OK) {
            /* yt-dlp rejected this URL (unsupported site, or no extractable
             * media). Fall back to fetching it directly with the engine — this
             * is how generic files (download endpoints, extensionless or
             * unrecognised URLs) that yt-dlp refuses still get downloaded. */
            DLM_INFO("yt-dlp could not handle %s; downloading it directly", url);
            rc = extract_direct(url, out);
        }
    }
    /* archive.org files from different subfolders share a basename, and
     * playlist entries can share a title: make output names unique. */
    if (rc == DLM_OK) dlm_extract_dedupe_names(out);
    return rc;
}
