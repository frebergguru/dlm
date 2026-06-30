/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — Internet Archive authentication / credential storage. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "dlm/iaauth.h"
#include "dlm/dlm.h"
#include "httpget.h"
#include "internal.h"
#include "compat/compat.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#if defined(_WIN32)
#  include <io.h>
#else
#  include <unistd.h>
#endif

/* Scrub a sensitive buffer before freeing/returning so plaintext credentials
 * don't linger in freed heap or stack. Volatile pointer defeats dead-store
 * elimination without depending on a platform explicit_bzero. */
static void secure_zero(void *p, size_t n)
{
    if (!p) return;
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

/* ---- config path ------------------------------------------------------ */

static void config_path(char *buf, size_t n)
{
    char dir[400];
    dlm_config_dir(dir, sizeof dir);
    snprintf(buf, n, "%s/credentials", dir);
}

static void ensure_config_dir(void)
{
    char dir[400];
    dlm_config_dir(dir, sizeof dir);
    dlm_mkdir_p(dir);
}

/* ---- load / save ------------------------------------------------------ */

const char *dlm_ia_mode_str(ia_auth_mode m)
{
    switch (m) {
    case IA_AUTH_NONE: return "anonymous";
    case IA_AUTH_S3: return "signed in (S3 keys)";
    case IA_AUTH_COOKIE: return "signed in (cookie)";
    }
    return "unknown";
}

void dlm_ia_credentials_free(ia_credentials *c)
{
    if (!c) return;
    free(c->access);
    free(c->secret);
    free(c->cookie);
    c->access = c->secret = c->cookie = NULL;
    c->mode = IA_AUTH_NONE;
}

int dlm_ia_load(ia_credentials *out)
{
    memset(out, 0, sizeof *out);
    out->mode = IA_AUTH_NONE;

    char path[512];
    config_path(path, sizeof path);
    json_error_t e;
    json_t *root = json_load_file(path, 0, &e);
    if (!root) return 0; /* no config -> anonymous */

    json_t *ia = json_object_get(root, "ia");
    if (json_is_object(ia)) {
        const char *mode = json_string_value(json_object_get(ia, "mode"));
        const char *access = json_string_value(json_object_get(ia, "access"));
        const char *secret = json_string_value(json_object_get(ia, "secret"));
        const char *cookie = json_string_value(json_object_get(ia, "cookie"));
        if (mode && !strcmp(mode, "s3") && access && secret) {
            out->mode = IA_AUTH_S3;
            out->access = dlm_xstrdup(access);
            out->secret = dlm_xstrdup(secret);
        } else if (mode && !strcmp(mode, "cookie") && cookie) {
            out->mode = IA_AUTH_COOKIE;
            out->cookie = dlm_xstrdup(cookie);
        }
    }
    json_decref(root);
    return 0;
}

static int save_root(json_t *ia)
{
    ensure_config_dir();
    json_t *root = json_object();
    json_object_set_new(root, "ia", ia);

    char path[512], tmp[520];
    config_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    /* Create the temp file 0600 up front: the secret must never touch the disk
     * with broader permissions, which json_dump_file would do (it fopen()s the
     * file 0644 & ~umask and only then would we chmod it). */
    int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC | DLM_O_CLOEXEC | DLM_O_NOFOLLOW | DLM_O_BINARY, 0600);
    if (fd < 0) {
        DLM_ERROR("ia: cannot create %s: %s", tmp, strerror(errno));
        json_decref(root);
        return -1;
    }
    FILE *fp = fdopen(fd, "w");
    if (!fp) {
        DLM_ERROR("ia: cannot open %s: %s", tmp, strerror(errno));
        close(fd);
        json_decref(root);
        return -1;
    }
    int rc = json_dumpf(root, fp, JSON_INDENT(2));
    if (fclose(fp) != 0) rc = -1;
    json_decref(root);
    if (rc != 0) { DLM_ERROR("ia: cannot write %s", tmp); remove(tmp); return -1; }
    if (dlm_rename_replace(tmp, path) != 0) {
        DLM_ERROR("ia: cannot rename config");
        remove(tmp); /* don't leave a stale partial credential file behind */
        return -1;
    }
    return 0;
}

int dlm_ia_save_s3(const char *access, const char *secret)
{
    if (!access || !secret) return -1;
    json_t *ia = json_object();
    json_object_set_new(ia, "mode", json_string("s3"));
    json_object_set_new(ia, "access", json_string(access));
    json_object_set_new(ia, "secret", json_string(secret));
    return save_root(ia);
}

int dlm_ia_save_cookie(const char *cookie)
{
    if (!cookie) return -1;
    json_t *ia = json_object();
    json_object_set_new(ia, "mode", json_string("cookie"));
    json_object_set_new(ia, "cookie", json_string(cookie));
    return save_root(ia);
}

int dlm_ia_logout(void)
{
    char path[512];
    config_path(path, sizeof path);
    remove(path);
    return 0;
}

/* ---- auth headers ----------------------------------------------------- */

void dlm_ia_free_headers(char **h)
{
    if (!h) return;
    /* these carry the S3 secret / session cookie; scrub before releasing */
    for (int i = 0; h[i]; i++) { secure_zero(h[i], strlen(h[i])); free(h[i]); }
    free(h);
}

char **dlm_ia_auth_headers(const ia_credentials *c)
{
    if (!c || c->mode == IA_AUTH_NONE) return NULL;
    char **h = dlm_xcalloc(2, sizeof *h);
    char buf[1024];
    if (c->mode == IA_AUTH_S3 && c->access && c->secret) {
        snprintf(buf, sizeof buf, "Authorization: LOW %s:%s", c->access, c->secret);
        h[0] = dlm_xstrdup(buf);
    } else if (c->mode == IA_AUTH_COOKIE && c->cookie) {
        snprintf(buf, sizeof buf, "Cookie: %s", c->cookie);
        h[0] = dlm_xstrdup(buf);
    } else {
        free(h);
        return NULL;
    }
    h[1] = NULL;
    secure_zero(buf, sizeof buf); /* scrub the secret/cookie copy off the stack */
    return h;
}

/* ---- password login (xauthn) ------------------------------------------ */

/* URL-encode a form value into a malloc'd string. */
static char *form_encode(const char *s)
{
    size_t n = strlen(s);
    char *out = dlm_xmalloc(n * 3 + 1);
    char *w = out;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            *w++ = (char)ch;
        else {
            static const char hx[] = "0123456789ABCDEF";
            *w++ = '%';
            *w++ = hx[ch >> 4];
            *w++ = hx[ch & 15];
        }
    }
    *w = '\0';
    return out;
}

int dlm_ia_login_password(const char *email, const char *password, char **err)
{
    if (err) *err = NULL;
    if (!email || !password) {
        if (err) *err = dlm_xstrdup("email and password required");
        return -1;
    }
    char *e = form_encode(email);
    char *p = form_encode(password);
    char *fields = dlm_xmalloc(strlen(e) + strlen(p) + 32);
    sprintf(fields, "email=%s&password=%s", e, p);
    secure_zero(e, strlen(e));
    secure_zero(p, strlen(p));
    free(e);
    free(p);

    char *body = NULL;
    long status = 0;
    const char *hdrs[] = {"Accept: application/json", NULL};
    int rc = dlm_http_request("https://archive.org/services/xauthn/?op=login",
                              fields, hdrs, &body, &status);
    secure_zero(fields, strlen(fields)); /* contains the plaintext password */
    free(fields);
    if (rc != DLM_OK) {
        if (err) *err = dlm_xstrdup("network error contacting archive.org");
        free(body);
        return -1;
    }

    /* xauthn returns JSON; on success it carries the session values used to
     * build the logged-in cookie. Parse defensively across field names. */
    int ok = 0;
    if (body) {
        json_error_t je;
        json_t *root = json_loads(body, 0, &je);
        if (root) {
            json_t *success = json_object_get(root, "success");
            json_t *values = json_object_get(root, "values");
            if (json_is_true(success) && json_is_object(values)) {
                const char *user = json_string_value(json_object_get(values, "logged-in-user"));
                const char *sig = json_string_value(json_object_get(values, "logged-in-sig"));
                if (user && sig) {
                    char cookie[2048];
                    snprintf(cookie, sizeof cookie,
                             "logged-in-user=%s; logged-in-sig=%s", user, sig);
                    if (dlm_ia_save_cookie(cookie) == 0) ok = 1;
                    secure_zero(cookie, sizeof cookie); /* session credential */
                }
            }
            if (!ok) {
                const char *msg = json_string_value(json_object_get(root, "error"));
                if (err) *err = dlm_xstrdup(msg ? msg : "login rejected by archive.org");
            }
            json_decref(root);
        } else if (err) {
            *err = dlm_xstrdup("unexpected login response");
        }
    }
    free(body);
    return ok ? 0 : -1;
}
