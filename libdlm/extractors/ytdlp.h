/* libdlm — yt-dlp extractor (internal). */
#ifndef DLM_YTDLP_H
#define DLM_YTDLP_H

#include "dlm/extract.h"

/* Run yt-dlp on `url` and resolve it into download tasks. */
int dlm_extract_ytdlp(const char *url, dlm_extract_result *out);

/* Parse yt-dlp `-J` JSON into tasks (testable without invoking yt-dlp).
 * input_url is used as a fallback page URL for delegated streams. */
int dlm_ytdlp_parse(const char *json_text, const char *input_url,
                    dlm_extract_result *out);

#endif /* DLM_YTDLP_H */
