/* Unit tests for libdlm utility helpers. */
#include "dlm/dlm.h"
#include "internal.h"

#include <assert.h>
#include <stdio.h>

static int failures = 0;

#define CHECK_STR(expr, want)                                                  \
    do {                                                                       \
        char *got = (expr);                                                    \
        if (strcmp(got, want) != 0) {                                          \
            fprintf(stderr, "FAIL %s: got '%s' want '%s'\n", #expr, got, want);\
            failures++;                                                        \
        }                                                                      \
        free(got);                                                             \
    } while (0)

#define CHECK_HUMAN(n, want)                                                   \
    do {                                                                       \
        char buf[32];                                                          \
        dlm_human_bytes((n), buf, sizeof buf);                                 \
        if (strcmp(buf, want) != 0) {                                          \
            fprintf(stderr, "FAIL human(%lld): got '%s' want '%s'\n",          \
                    (long long)(n), buf, want);                                \
            failures++;                                                        \
        }                                                                      \
    } while (0)

int main(void)
{
    /* filename derivation */
    CHECK_STR(dlm_filename_from_url("https://archive.org/download/x/file.pdf"),
              "file.pdf");
    CHECK_STR(dlm_filename_from_url("https://host/a/b/c.tar.gz?token=123"),
              "c.tar.gz");
    CHECK_STR(dlm_filename_from_url("https://host/path/na%20me.bin"),
              "na me.bin");
    CHECK_STR(dlm_filename_from_url("https://host/"), "download");
    CHECK_STR(dlm_filename_from_url("https://host"), "host");
    CHECK_STR(dlm_filename_from_url("https://host/file#frag"), "file");

    /* human bytes */
    CHECK_HUMAN(0, "0 B");
    CHECK_HUMAN(512, "512 B");
    CHECK_HUMAN(1024, "1.0 KiB");
    CHECK_HUMAN(1536, "1.5 KiB");
    CHECK_HUMAN((int64_t)5 * 1024 * 1024, "5.0 MiB");
    CHECK_HUMAN(-1, "?");

    /* transfer-rate parsing */
#define CHECK_RATE(s, want)                                                    \
    do { int64_t g = dlm_parse_rate(s);                                        \
         if (g != (want)) { fprintf(stderr, "FAIL rate(%s): got %lld want %lld\n", \
             #s, (long long)g, (long long)(want)); failures++; } } while (0)
    CHECK_RATE("0", 0);
    CHECK_RATE("", 0);
    CHECK_RATE(NULL, 0);
    CHECK_RATE("1024", 1024);
    CHECK_RATE("500k", 500 * 1024);
    CHECK_RATE("2M", 2 * 1024 * 1024);
    CHECK_RATE("1.5m", (int64_t)(1.5 * 1024 * 1024));
    CHECK_RATE("1g", (int64_t)1024 * 1024 * 1024);
    CHECK_RATE("garbage", 0);

    char rb[32];
    dlm_format_rate(0, rb, sizeof rb);
    if (strcmp(rb, "unlimited") != 0) { fprintf(stderr, "FAIL fmt rate 0\n"); failures++; }
    dlm_format_rate(2 * 1024 * 1024, rb, sizeof rb);
    if (strcmp(rb, "2.0 MiB/s") != 0) { fprintf(stderr, "FAIL fmt rate: %s\n", rb); failures++; }

    if (failures == 0) {
        printf("test_util: all passed\n");
        return 0;
    }
    fprintf(stderr, "test_util: %d failure(s)\n", failures);
    return 1;
}
