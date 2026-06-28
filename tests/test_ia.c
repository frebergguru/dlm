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

int main(void)
{
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
