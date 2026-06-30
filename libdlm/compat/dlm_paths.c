/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm/compat — per-user directories and directory creation. */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "compat/dlm_paths.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#if defined(_WIN32)
#  include <direct.h>
#  include <io.h>
#else
#  include <unistd.h>
#endif

#if !defined(_WIN32)
static const char *env_or(const char *name, const char *fallback)
{
    const char *v = getenv(name);
    return (v && *v) ? v : fallback;
}
#endif

#if defined(_WIN32)

/* On Windows the per-user roots come from APPDATA / LOCALAPPDATA, which the OS
 * always sets. We fall back to the profile dir, then the current dir. */
static void win_root(char *buf, size_t n, const char *primary)
{
    const char *base = getenv(primary);
    if (!base || !*base) base = getenv("USERPROFILE");
    if (!base || !*base) base = ".";
    snprintf(buf, n, "%s", base);
}

void dlm_config_dir(char *buf, size_t n)
{
    char root[400];
    win_root(root, sizeof root, "APPDATA");
    snprintf(buf, n, "%s/dlm", root);
}

void dlm_data_dir(char *buf, size_t n)
{
    char root[400];
    win_root(root, sizeof root, "LOCALAPPDATA");
    snprintf(buf, n, "%s/dlm", root);
}

void dlm_cache_dir(char *buf, size_t n)
{
    char root[400];
    win_root(root, sizeof root, "LOCALAPPDATA");
    snprintf(buf, n, "%s/dlm/cache", root);
}

void dlm_runtime_dir(char *buf, size_t n)
{
    char root[400];
    win_root(root, sizeof root, "LOCALAPPDATA");
    snprintf(buf, n, "%s/dlm", root);
}

#else /* POSIX */

void dlm_config_dir(char *buf, size_t n)
{
    const char *x = getenv("XDG_CONFIG_HOME");
    if (x && *x) snprintf(buf, n, "%s/dlm", x);
    else snprintf(buf, n, "%s/.config/dlm", env_or("HOME", "."));
}

void dlm_data_dir(char *buf, size_t n)
{
    const char *x = getenv("XDG_DATA_HOME");
    if (x && *x) snprintf(buf, n, "%s/dlm", x);
    else snprintf(buf, n, "%s/.local/share/dlm", env_or("HOME", "."));
}

void dlm_cache_dir(char *buf, size_t n)
{
    const char *x = getenv("XDG_CACHE_HOME");
    if (x && *x) snprintf(buf, n, "%s/dlm", x);
    else snprintf(buf, n, "%s/.cache/dlm", env_or("HOME", "."));
}

void dlm_runtime_dir(char *buf, size_t n)
{
    snprintf(buf, n, "%s", env_or("XDG_RUNTIME_DIR", "/tmp"));
}

#endif

void dlm_socket_path(char *buf, size_t n)
{
#if defined(_WIN32)
    char rt[400];
    dlm_runtime_dir(rt, sizeof rt);
    dlm_mkdir_p(rt);
    snprintf(buf, n, "%s/dlm.sock", rt);
#else
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) snprintf(buf, n, "%s/dlm.sock", rt);
    else snprintf(buf, n, "/tmp/dlm-%d.sock", (int)getuid());
#endif
}

void dlm_db_path(char *buf, size_t n)
{
    const char *override = getenv("DLM_DB");
    if (override && *override) { snprintf(buf, n, "%s", override); return; }
    char dir[400];
    dlm_data_dir(dir, sizeof dir);
    dlm_mkdir_p(dir);
    snprintf(buf, n, "%s/queue.db", dir);
}

int dlm_mkdir_mode(const char *path, int mode)
{
#if defined(_WIN32)
    (void)mode;
    if (_mkdir(path) == 0) return 0;
#else
    if (mkdir(path, (mode_t)mode) == 0) return 0;
#endif
    /* treat an existing *directory* as success — but a plain file sitting where
     * a directory should be is a real failure, not "already exists". */
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    return -1;
}

int dlm_mkdir_p(const char *path)
{
    char tmp[4096];
    int wn = snprintf(tmp, sizeof tmp, "%s", path);
    if (wn < 0 || (size_t)wn >= sizeof tmp) return -1; /* don't mkdir a truncated path */
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    /* strip a trailing separator so we don't mkdir("") */
    if (tmp[len - 1] == '/' || tmp[len - 1] == '\\') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char sep = *p;
            *p = '\0';
            dlm_mkdir_mode(tmp, 0755);
            *p = sep;
        }
    }
    return dlm_mkdir_mode(tmp, 0755);
}
