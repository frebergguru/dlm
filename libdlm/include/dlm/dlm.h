/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — public API for the dlm download engine.
 *
 * Phase 1 surface: a self-contained segmented HTTP(S) downloader with resume.
 * Later phases add the queue/scheduler, persistence and extractors on top of
 * these primitives.
 */
#ifndef DLM_H
#define DLM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result codes returned by the engine. */
typedef enum {
    DLM_OK = 0,
    DLM_ERR_ARG = -1,      /* bad argument */
    DLM_ERR_NET = -2,      /* network / curl failure that survived retries */
    DLM_ERR_IO = -3,       /* local filesystem error */
    DLM_ERR_HTTP = -4,     /* non-success HTTP status */
    DLM_ERR_NOMEM = -5,    /* allocation failure */
    DLM_ERR_CANCELLED = -6 /* cancelled via the cancel flag */
} dlm_result;

/* Progress callback. Invoked periodically on the thread that drives the
 * download. `done`/`total` are byte counts (total < 0 means unknown);
 * `speed_bps` is a smoothed bytes-per-second estimate. */
typedef void (*dlm_progress_cb)(void *userdata, int64_t done, int64_t total,
                                double speed_bps);

/* Options for a single-file download. Zero-initialise and set what you need. */
typedef struct {
    const char *url;        /* required: source URL */
    const char *out_path;   /* required: final output path */
    int connections;        /* parallel segments; 0 -> engine default (4) */
    int64_t min_split_size; /* don't segment files smaller than this; 0 -> 1 MiB */
    long max_retries;       /* per-segment retry attempts; 0 -> default (5) */
    int64_t max_speed;      /* total download rate cap in bytes/sec; 0 -> unlimited */

    /* Extra HTTP request headers, as an array of "Key: Value" strings,
     * NULL-terminated. Used to pass yt-dlp/IA auth headers through. */
    const char *const *headers;

    dlm_progress_cb on_progress;
    void *userdata;

    /* Optional cooperative cancel flag: set *cancel to non-zero to abort. */
    volatile int *cancel;
} dlm_options;

/* Download a single URL to out_path, using segmented connections and resuming
 * from any existing partial state. Blocks until complete, failed or cancelled.
 * Returns DLM_OK on success. */
dlm_result dlm_download_file(const dlm_options *opt);

/* One-time process-wide init/cleanup (wraps curl_global_*). Safe to call once
 * at startup / shutdown. dlm_download_file() will lazily init if you skip this. */
dlm_result dlm_global_init(void);
void dlm_global_cleanup(void);

/* Human-readable string for a dlm_result. */
const char *dlm_strerror(dlm_result r);

/* Library version string. */
const char *dlm_version(void);

/* Parse a human transfer rate ("500k", "2M", "1.5g", "1048576") into bytes per
 * second. Suffixes k/m/g (case-insensitive) are powers of 1024. Returns 0 for
 * NULL/empty/unparseable input (i.e. "unlimited"). */
int64_t dlm_parse_rate(const char *s);

/* Format a byte/sec rate into a compact human string (e.g. "1.5 MiB/s"). */
void dlm_format_rate(int64_t bps, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* DLM_H */
