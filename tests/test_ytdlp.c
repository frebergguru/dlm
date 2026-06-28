/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Offline test: yt-dlp JSON -> tasks, without invoking yt-dlp. Verifies the
 * progressive-http path (engine download) vs the fragmented/merge path
 * (delegate to yt-dlp), header passthrough, and playlist entries. */
#include "extractors/ytdlp.h"
#include "dlm/extract.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg)                                                       \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

/* progressive http(s) mp4 with headers -> direct engine task */
static const char *PROGRESSIVE =
    "{\"title\":\"My Video\",\"ext\":\"mp4\",\"protocol\":\"https\","
    "\"url\":\"https://cdn.example.com/v.mp4\",\"filesize\":12345,"
    "\"http_headers\":{\"User-Agent\":\"yt\",\"Referer\":\"https://x/\"},"
    "\"webpage_url\":\"https://site/watch?v=1\"}";

/* HLS stream -> delegate */
static const char *HLS =
    "{\"title\":\"Live/Stream\",\"ext\":\"mp4\",\"protocol\":\"m3u8_native\","
    "\"url\":\"https://cdn/playlist.m3u8\",\"webpage_url\":\"https://site/live\"}";

/* separate audio+video requested_formats -> delegate (needs muxing) */
static const char *MERGE =
    "{\"title\":\"HD\",\"ext\":\"mkv\",\"webpage_url\":\"https://site/v\","
    "\"requested_formats\":[{\"url\":\"https://cdn/v\"},{\"url\":\"https://cdn/a\"}]}";

/* playlist with two entries */
static const char *PLAYLIST =
    "{\"entries\":["
    "{\"title\":\"A\",\"ext\":\"mp4\",\"protocol\":\"https\",\"url\":\"https://c/a.mp4\"},"
    "{\"title\":\"B\",\"ext\":\"webm\",\"protocol\":\"m3u8\",\"url\":\"https://c/b.m3u8\",\"webpage_url\":\"https://s/b\"}"
    "]}";

int main(void)
{
    dlm_extract_result r;

    /* progressive */
    CHECK(dlm_ytdlp_parse(PROGRESSIVE, "https://site/watch?v=1", &r) == 0, "parse progressive");
    CHECK(r.count == 1, "one task");
    CHECK(r.tasks[0].delegate == 0, "progressive not delegated");
    CHECK(strcmp(r.tasks[0].url, "https://cdn.example.com/v.mp4") == 0, "direct url");
    CHECK(strcmp(r.tasks[0].filename, "My Video.mp4") == 0, "filename");
    CHECK(r.tasks[0].size == 12345, "filesize");
    CHECK(r.tasks[0].headers && r.tasks[0].headers[0], "headers present");
    int seen_ua = 0;
    for (int i = 0; r.tasks[0].headers && r.tasks[0].headers[i]; i++)
        if (strstr(r.tasks[0].headers[i], "User-Agent: yt")) seen_ua = 1;
    CHECK(seen_ua, "User-Agent header passed through");
    dlm_extract_result_free(&r);

    /* HLS -> delegate, filename sanitized */
    CHECK(dlm_ytdlp_parse(HLS, "https://site/live", &r) == 0, "parse hls");
    CHECK(r.count == 1 && r.tasks[0].delegate == 1, "hls delegated");
    CHECK(strcmp(r.tasks[0].url, "https://site/live") == 0, "delegate uses page url");
    CHECK(strcmp(r.tasks[0].filename, "Live_Stream.mp4") == 0, "slash sanitized in filename");
    dlm_extract_result_free(&r);

    /* merge -> delegate */
    CHECK(dlm_ytdlp_parse(MERGE, "https://site/v", &r) == 0, "parse merge");
    CHECK(r.count == 1 && r.tasks[0].delegate == 1, "merge delegated");
    dlm_extract_result_free(&r);

    /* playlist -> 2 tasks, mixed */
    CHECK(dlm_ytdlp_parse(PLAYLIST, "https://s", &r) == 0, "parse playlist");
    CHECK(r.count == 2, "two entries");
    CHECK(r.tasks[0].delegate == 0, "entry A progressive");
    CHECK(r.tasks[1].delegate == 1, "entry B hls delegated");
    dlm_extract_result_free(&r);

    if (failures == 0) { printf("test_ytdlp: all passed\n"); return 0; }
    fprintf(stderr, "test_ytdlp: %d failure(s)\n", failures);
    return 1;
}
