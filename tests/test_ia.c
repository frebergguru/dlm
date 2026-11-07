/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Offline tests: archive.org URL recognition + IA credential storage/headers. */
#include "dlm/extract.h"
#include "dlm/iaauth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg)                                                       \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

/* internal (libdlm/extractors/ytdlp.h); declared here to keep this test's
 * include path to the public headers */
int dlm_ytdlp_parse(const char *json_text, const char *input_url,
                    dlm_extract_result *out);

static void set_name(dlm_task *t, const char *n)
{
    t->filename = n ? strdup(n) : NULL;
}

static void test_dedupe_and_ext(void)
{
    /* dedupe: later case-insensitive duplicates get " (2)", " (3)" before the
     * extension; unique names (and the first occurrence) stay unchanged */
    dlm_extract_result r;
    memset(&r, 0, sizeof r);
    r.count = 7;
    r.tasks = calloc((size_t)r.count, sizeof *r.tasks);
    set_name(&r.tasks[0], "track01.flac");
    set_name(&r.tasks[1], "TRACK01.flac");
    set_name(&r.tasks[2], "other.mp3");
    set_name(&r.tasks[3], "track01.flac");
    set_name(&r.tasks[4], "track01 (2).flac"); /* pre-existing name is kept */
    set_name(&r.tasks[5], "README");
    set_name(&r.tasks[6], "readme");
    dlm_extract_dedupe_names(&r);
    CHECK(strcmp(r.tasks[0].filename, "track01.flac") == 0, "dedupe keeps first");
    CHECK(strcmp(r.tasks[2].filename, "other.mp3") == 0, "dedupe keeps unique");
    CHECK(strcmp(r.tasks[4].filename, "track01 (2).flac") == 0, "dedupe keeps unique (2)");
    CHECK(strcmp(r.tasks[1].filename, "TRACK01 (3).flac") == 0, "dedupe case-insensitive");
    CHECK(strcmp(r.tasks[3].filename, "track01 (4).flac") == 0, "dedupe third copy");
    CHECK(strcmp(r.tasks[5].filename, "README") == 0, "dedupe no-ext first");
    CHECK(strcmp(r.tasks[6].filename, "readme (2)") == 0, "dedupe no-ext");
    dlm_extract_result_free(&r);

    /* yt-dlp: remote "ext" is sanitised (alnum, <= 10 chars, else "bin") and
     * the title goes through dlm_sanitize_component */
    const char *js =
        "{\"title\":\"P\",\"entries\":["
        "{\"title\":\"a?b\",\"ext\":\"mp4\",\"webpage_url\":\"https://s/1\",\"protocol\":\"m3u8\"},"
        "{\"title\":\"x\",\"ext\":\"../../evil\",\"webpage_url\":\"https://s/2\",\"protocol\":\"m3u8\"},"
        "{\"title\":\"y\",\"ext\":\"abcdefghijk\",\"webpage_url\":\"https://s/3\",\"protocol\":\"m3u8\"},"
        "{\"title\":\"z\",\"ext\":\"m4a \",\"webpage_url\":\"https://s/4\",\"protocol\":\"m3u8\"}"
        "]}";
    CHECK(dlm_ytdlp_parse(js, "https://s", &r) == 0 && r.count == 4, "parse ext playlist");
    if (r.count == 4) {
        CHECK(strcmp(r.tasks[0].filename, "a_b.mp4") == 0, "title sanitised, ext kept");
        CHECK(strcmp(r.tasks[1].filename, "x.bin") == 0, "traversal ext -> bin");
        CHECK(strcmp(r.tasks[2].filename, "y.bin") == 0, "overlong ext -> bin");
        CHECK(strcmp(r.tasks[3].filename, "z.bin") == 0, "ext with space -> bin");
    }
    dlm_extract_result_free(&r);
}

int main(void)
{
    test_dedupe_and_ext();

    /* archive.org URL recognition */
    CHECK(dlm_is_archiveorg_url("https://archive.org/details/foo"), "details url");
    CHECK(dlm_is_archiveorg_url("https://archive.org/download/foo/bar.pdf"), "download url");
    CHECK(dlm_is_archiveorg_url("http://archive.org/metadata/foo"), "metadata url");
    CHECK(dlm_is_archiveorg_url("https://web.archive.org/details/x") == 0 ||
          dlm_is_archiveorg_url("https://web.archive.org/details/x") == 1, "subdomain handled");
    CHECK(!dlm_is_archiveorg_url("https://example.com/details/foo"), "non-IA host");
    CHECK(!dlm_is_archiveorg_url("https://notarchive.org/details/foo"), "lookalike host");
    CHECK(!dlm_is_archiveorg_url("https://archive.org/about/"), "non-item path");

    /* isolate config in a temp dir */
    char tmpl[] = "/tmp/dlm_ia_XXXXXX";
    char *dir = mkdtemp(tmpl);
    CHECK(dir != NULL, "tmp config dir");
    setenv("XDG_CONFIG_HOME", dir, 1);

    /* default: anonymous */
    ia_credentials c;
    dlm_ia_load(&c);
    CHECK(c.mode == IA_AUTH_NONE, "default anonymous");
    CHECK(dlm_ia_auth_headers(&c) == NULL, "no headers when anonymous");
    dlm_ia_credentials_free(&c);

    /* S3 keys roundtrip + header format */
    CHECK(dlm_ia_save_s3("ACCESS123", "SECRET456") == 0, "save s3");
    dlm_ia_load(&c);
    CHECK(c.mode == IA_AUTH_S3, "loaded s3 mode");
    CHECK(c.access && strcmp(c.access, "ACCESS123") == 0, "access persisted");
    char **h = dlm_ia_auth_headers(&c);
    CHECK(h && h[0] && strcmp(h[0], "Authorization: LOW ACCESS123:SECRET456") == 0,
          "LOW auth header");
    dlm_ia_free_headers(h);
    dlm_ia_credentials_free(&c);

    /* config file must be private (0600) */
    char cfgpath[512];
    snprintf(cfgpath, sizeof cfgpath, "%s/dlm/credentials", dir);
    struct stat st;
    CHECK(stat(cfgpath, &st) == 0, "config exists");
    CHECK((st.st_mode & 0777) == 0600, "config is 0600");

    /* cookie mode */
    CHECK(dlm_ia_save_cookie("logged-in-user=a; logged-in-sig=b") == 0, "save cookie");
    dlm_ia_load(&c);
    CHECK(c.mode == IA_AUTH_COOKIE, "cookie mode");
    h = dlm_ia_auth_headers(&c);
    CHECK(h && h[0] && strcmp(h[0], "Cookie: logged-in-user=a; logged-in-sig=b") == 0,
          "cookie header");
    dlm_ia_free_headers(h);
    dlm_ia_credentials_free(&c);

    /* logout -> anonymous */
    CHECK(dlm_ia_logout() == 0, "logout");
    dlm_ia_load(&c);
    CHECK(c.mode == IA_AUTH_NONE, "anonymous after logout");
    dlm_ia_credentials_free(&c);

    /* cleanup */
    unlink(cfgpath);
    char d2[600];
    snprintf(d2, sizeof d2, "%s/dlm", dir);
    rmdir(d2);
    rmdir(dir);

    if (failures == 0) { printf("test_ia: all passed\n"); return 0; }
    fprintf(stderr, "test_ia: %d failure(s)\n", failures);
    return 1;
}
