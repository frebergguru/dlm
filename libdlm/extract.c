/* libdlm — extractor dispatch.
 *
 * Routes a URL to the right extractor. archive.org is handled natively; any
 * other URL becomes a single direct-download task (a later phase adds yt-dlp as
 * the fallback for arbitrary sites).
 */
#define _POSIX_C_SOURCE 200809L
#include "dlm/extract.h"
#include "dlm/dlm.h"
#include "extractors/archiveorg.h"
#include "extractors/ytdlp.h"
#include "internal.h"

#include <ctype.h>
#include <strings.h>

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

int dlm_extract(const char *url, dlm_extract_result *out)
{
    if (!url || !out) return DLM_ERR_ARG;
    if (dlm_is_archiveorg_url(url))
        return dlm_extract_archiveorg(url, out);
    if (looks_like_direct_file(url))
        return extract_direct(url, out);
    /* a web page / stream: resolve via yt-dlp */
    return dlm_extract_ytdlp(url, out);
}
