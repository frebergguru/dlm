/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — URL extraction.
 *
 * An extractor turns a user-supplied URL (a web page, an archive.org item, or a
 * direct file link) into one or more concrete download tasks the engine can
 * fetch. archive.org is handled natively; other URLs fall through to a direct
 * download (and, in a later phase, to yt-dlp).
 */
#ifndef DLM_EXTRACT_H
#define DLM_EXTRACT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One downloadable file produced by an extractor. */
typedef struct {
    char *url;        /* direct download URL, or the page URL when delegate=1 */
    char *filename;   /* suggested output filename (basename) */
    int64_t size;     /* bytes, or -1 if unknown */
    char *md5;        /* hex md5 for verification, or NULL */
    char *sha1;       /* hex sha1 for verification, or NULL */
    char **headers;   /* NULL-terminated "Key: Value" array, or NULL */
    int delegate;     /* 1 => fragmented stream: download via yt-dlp, not the
                       *      segmented engine (url holds the page URL) */
} dlm_task;

typedef struct {
    dlm_task *tasks;
    int count;
    char *source;     /* extractor name that produced these ("archive.org", ...) */
} dlm_extract_result;

/* Extract a URL into download tasks. Returns DLM_OK and fills `out` (which the
 * caller frees with dlm_extract_result_free). */
int dlm_extract(const char *url, dlm_extract_result *out);

void dlm_extract_result_free(dlm_extract_result *r);

/* True if this URL is handled by the native archive.org extractor. */
int dlm_is_archiveorg_url(const char *url);

#ifdef __cplusplus
}
#endif

#endif /* DLM_EXTRACT_H */
