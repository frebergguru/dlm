/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — auto-managed external tools (yt-dlp, ffmpeg/ffprobe).
 *
 * Binaries live in <data>/dlm/tools. yt-dlp is a single file (download + verify
 * SHA-256 + atomic swap). ffmpeg/ffprobe come from an archive that we extract by
 * shelling out to `tar` (universal on modern Win10+/Linux/mac); if tar or the
 * download is unavailable we fall back to whatever is on PATH. A small state.json
 * records versions + the last update check so we only poll weekly.
 */
#if !defined(_WIN32)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "dlm/tools.h"
#include "dlm/dlm.h"
#include "dlm/verify.h"
#include "httpget.h"
#include "internal.h"
#include "compat/compat.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#if defined(_WIN32)
#  include <io.h>
#  include <windows.h>
#  define EXE_SUFFIX ".exe"
#  define PATH_LIST_SEP ';'
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/file.h>
#  define EXE_SUFFIX ""
#  define PATH_LIST_SEP ':'
#endif

#define WEEK_SECONDS (7 * 24 * 60 * 60)

#define YTDLP_DL_BASE "https://github.com/yt-dlp/yt-dlp/releases/latest/download/"
#define YTDLP_API "https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest"
#define FFMPEG_DL_BASE "https://github.com/yt-dlp/FFmpeg-Builds/releases/latest/download/"

/* Per-OS asset names. */
#if defined(_WIN32)
#  define YTDLP_ASSET "yt-dlp.exe"
#  define FFMPEG_ARCHIVE "ffmpeg-master-latest-win64-gpl.zip"
#  define FFMPEG_HAVE_ARCHIVE 1
#elif defined(__APPLE__)
#  define YTDLP_ASSET "yt-dlp_macos"
#  define FFMPEG_ARCHIVE ""
#  define FFMPEG_HAVE_ARCHIVE 0 /* no first-party mac ffmpeg build */
#else
#  define YTDLP_ASSET "yt-dlp"
#  define FFMPEG_ARCHIVE "ffmpeg-master-latest-linux64-gpl.tar.xz"
#  define FFMPEG_HAVE_ARCHIVE 1
#endif

/* ---- small fs helpers ------------------------------------------------- */

static void tools_dir(char *buf, size_t n)
{
    char data[256];
    dlm_data_dir(data, sizeof data);
    snprintf(buf, n, "%s/tools", data);
}

static void state_path(char *buf, size_t n)
{
    char dir[300];
    tools_dir(dir, sizeof dir);
    snprintf(buf, n, "%s/state.json", dir);
}

/* Path to a managed binary by base name (adds the OS exe suffix). */
static void managed_path(const char *name, char *buf, size_t n)
{
    char dir[300];
    tools_dir(dir, sizeof dir);
    snprintf(buf, n, "%s/%s%s", dir, name, EXE_SUFFIX);
}

static int file_exists(const char *path)
{
#if defined(_WIN32)
    return _access(path, 0) == 0;
#else
    return access(path, X_OK) == 0;
#endif
}

static void make_executable(const char *path)
{
#if !defined(_WIN32)
    chmod(path, 0755);
#else
    (void)path;
#endif
}

/* Find `name`(+suffix) on PATH; returns 1 and fills buf if found. */
static int which_tool(const char *name, char *buf, size_t n)
{
    const char *path = getenv("PATH");
    if (!path || !*path) return 0;
    const char *p = path;
    while (*p) {
        const char *sep = strchr(p, PATH_LIST_SEP);
        size_t dlen = sep ? (size_t)(sep - p) : strlen(p);
        if (dlen > 0 && dlen < n) {
            char cand[1024];
            snprintf(cand, sizeof cand, "%.*s/%s%s", (int)dlen, p, name, EXE_SUFFIX);
            if (file_exists(cand)) { snprintf(buf, n, "%s", cand); return 1; }
        }
        if (!sep) break;
        p = sep + 1;
    }
    return 0;
}

/* ---- path resolution (cached, stable storage per tool) ---------------- */

/* Thread-local so concurrent download workers each get their own storage and
 * don't race on / overwrite a shared buffer between a sibling's call and use. */
static _Thread_local char g_buf_ytdlp[1024];
static _Thread_local char g_buf_ffmpeg[1024];
static _Thread_local char g_buf_ffprobe[1024];

static char *buf_for(const char *name)
{
    if (!strcmp(name, "yt-dlp")) return g_buf_ytdlp;
    if (!strcmp(name, "ffmpeg")) return g_buf_ffmpeg;
    if (!strcmp(name, "ffprobe")) return g_buf_ffprobe;
    return NULL;
}

const char *dlm_tool_path(const char *name)
{
    char *out = buf_for(name);
    if (!out) return name; /* unknown tool: trust PATH */

    char managed[1024];
    managed_path(name, managed, sizeof managed);
    if (file_exists(managed)) { snprintf(out, 1024, "%s", managed); return out; }

    char onpath[1024];
    if (which_tool(name, onpath, sizeof onpath)) {
        /* bare name lets the spawner's own PATH search locate it */
        snprintf(out, 1024, "%s", name);
        return out;
    }
    /* not present anywhere yet: hand back the managed path so an ensure can fill it */
    snprintf(out, 1024, "%s", managed);
    return out;
}

const char *dlm_tool_ffmpeg_dir(void)
{
    static _Thread_local char dir[512];
    char managed[1024];
    managed_path("ffmpeg", managed, sizeof managed);
    if (!file_exists(managed)) return NULL;
    tools_dir(dir, sizeof dir);
    return dir;
}

static int tool_present(const char *name)
{
    char managed[1024], onpath[1024];
    managed_path(name, managed, sizeof managed);
    if (file_exists(managed)) return 1;
    return which_tool(name, onpath, sizeof onpath);
}

/* ---- state.json ------------------------------------------------------- */

static json_t *load_state(void)
{
    char path[600];
    state_path(path, sizeof path);
    json_error_t e;
    json_t *root = json_load_file(path, 0, &e);
    if (!root || !json_is_object(root)) {
        if (root) json_decref(root);
        root = json_object();
    }
    return root;
}

static int save_state(json_t *root)
{
    char dir[512];
    tools_dir(dir, sizeof dir);
    dlm_mkdir_p(dir);

    char path[600], tmp[640];
    state_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    if (json_dump_file(root, tmp, JSON_INDENT(2)) != 0) {
        DLM_WARN("tools: cannot write %s", tmp);
        return -1;
    }
    if (dlm_rename_replace(tmp, path) != 0) {
        DLM_WARN("tools: cannot rename state into place");
        return -1;
    }
    return 0;
}

/* ---- cross-process lock (daemon + CLI must not both download) --------- */

#if defined(_WIN32)
static HANDLE g_lock = INVALID_HANDLE_VALUE;
static int lock_acquire(void)
{
    char dir[512], path[600];
    tools_dir(dir, sizeof dir);
    dlm_mkdir_p(dir);
    snprintf(path, sizeof path, "%s/.lock", dir);
    /* opening with no sharing serializes processes */
    for (int tries = 0; tries < 300; tries++) {
        g_lock = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_lock != INVALID_HANDLE_VALUE) return 0;
        Sleep(100);
    }
    return -1;
}
static void lock_release(void)
{
    if (g_lock != INVALID_HANDLE_VALUE) { CloseHandle(g_lock); g_lock = INVALID_HANDLE_VALUE; }
}
#else
static int g_lock = -1;
static int lock_acquire(void)
{
    char dir[512], path[600];
    tools_dir(dir, sizeof dir);
    dlm_mkdir_p(dir);
    snprintf(path, sizeof path, "%s/.lock", dir);
    g_lock = open(path, O_CREAT | O_RDWR, 0644);
    if (g_lock < 0) return -1;
    if (flock(g_lock, LOCK_EX) != 0) { close(g_lock); g_lock = -1; return -1; }
    return 0;
}
static void lock_release(void)
{
    if (g_lock >= 0) { flock(g_lock, LOCK_UN); close(g_lock); g_lock = -1; }
}
#endif

/* ---- auto switch ------------------------------------------------------ */

static int env_disables_auto(void)
{
    const char *e = getenv("DLM_NO_AUTO_TOOLS");
    return e && *e && strcmp(e, "0") != 0;
}

int dlm_tools_auto_enabled(void)
{
    if (env_disables_auto()) return 0;
    json_t *root = load_state();
    json_t *a = json_object_get(root, "auto");
    int on = a ? json_is_true(a) || (json_is_integer(a) && json_integer_value(a)) : 1;
    if (!a) on = 1; /* default on */
    json_decref(root);
    return on;
}

void dlm_tools_set_auto_enabled(int on)
{
    json_t *root = load_state();
    json_object_set_new(root, "auto", on ? json_true() : json_false());
    save_state(root);
    json_decref(root);
}

/* ---- download primitives ---------------------------------------------- */

/* Stream a URL to dest via the segmented engine (no 64 MiB cap). */
static int download_to_file(const char *url, const char *dest)
{
    dlm_options opt;
    memset(&opt, 0, sizeof opt);
    opt.url = url;
    opt.out_path = dest;
    opt.connections = 1; /* small file off a redirecting CDN; keep it simple */
    dlm_result r = dlm_download_file(&opt);
    return r == DLM_OK ? 0 : -1;
}

/* Capture a command's stdout into a malloc'd string; returns exit code or -1. */
static int capture_cmd(const char *const argv[], char **out)
{
    *out = NULL;
    dlm_proc *p = dlm_proc_spawn(argv, DLM_SPAWN_STDOUT);
    if (!p) return -1;
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) { dlm_proc_terminate(p); dlm_proc_wait(p, NULL); dlm_proc_free(p); return -1; }
    char tmp[4096];
    long got;
    while ((got = dlm_proc_read(p, tmp, sizeof tmp, -1)) != 0) {
        if (got < 0) continue;
        if (len + (size_t)got + 1 > cap) {
            while (cap < len + (size_t)got + 1) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); dlm_proc_terminate(p); dlm_proc_wait(p, NULL); dlm_proc_free(p); return -1; }
            buf = nb;
        }
        memcpy(buf + len, tmp, (size_t)got);
        len += (size_t)got;
    }
    buf[len] = '\0';
    int ec = -1;
    dlm_proc_wait(p, &ec);
    dlm_proc_free(p);
    *out = buf;
    return ec;
}

static int run_cmd(const char *const argv[])
{
    dlm_proc *p = dlm_proc_spawn(argv, DLM_SPAWN_INHERIT);
    if (!p) return -1;
    int ec = -1;
    dlm_proc_wait(p, &ec);
    dlm_proc_free(p);
    return ec;
}

/* Fetch the SHA2-256SUMS file and return the hex for `asset` (or NULL). */
static char *fetch_ytdlp_sha256(const char *asset)
{
    char *body = NULL;
    long status = 0;
    if (dlm_http_get(YTDLP_DL_BASE "SHA2-256SUMS", NULL, &body, &status) != DLM_OK ||
        status != 200 || !body)
        { free(body); return NULL; }
    char *hex = NULL;
    char *line = strtok(body, "\n");
    size_t alen = strlen(asset);
    while (line) {
        /* format: "<hex>  <asset>" */
        char *sp = strstr(line, "  ");
        if (sp) {
            const char *fname = sp + 2;
            size_t flen = strlen(fname);
            while (flen && (fname[flen - 1] == '\r' || fname[flen - 1] == ' ')) flen--;
            if (flen == alen && strncmp(fname, asset, alen) == 0) {
                size_t hlen = (size_t)(sp - line);
                hex = malloc(hlen + 1);
                if (hex) { memcpy(hex, line, hlen); hex[hlen] = '\0'; }
                break;
            }
        }
        line = strtok(NULL, "\n");
    }
    free(body);
    return hex;
}

/* Query the latest yt-dlp release tag (e.g. "2026.01.15"). Returns 0 on success. */
static int latest_ytdlp_version(char *buf, size_t n)
{
    char *body = NULL;
    long status = 0;
    const char *hdrs[] = {"Accept: application/vnd.github+json", NULL};
    if (dlm_http_get(YTDLP_API, hdrs, &body, &status) != DLM_OK || status != 200 || !body) {
        free(body);
        return -1;
    }
    int rc = -1;
    json_error_t e;
    json_t *root = json_loads(body, 0, &e);
    if (root) {
        const char *tag = json_string_value(json_object_get(root, "tag_name"));
        if (tag) { snprintf(buf, n, "%s", tag); rc = 0; }
        json_decref(root);
    }
    free(body);
    return rc;
}

/* Download yt-dlp into the managed location, verifying its checksum. Returns 0. */
static int download_ytdlp(void)
{
    char dir[512], dest[1024], tmp[1100];
    tools_dir(dir, sizeof dir);
    dlm_mkdir_p(dir);
    managed_path("yt-dlp", dest, sizeof dest);
    snprintf(tmp, sizeof tmp, "%s.new", dest);

    DLM_INFO("tools: downloading yt-dlp ...");
    if (download_to_file(YTDLP_DL_BASE YTDLP_ASSET, tmp) != 0) {
        DLM_WARN("tools: yt-dlp download failed");
        return -1;
    }
    char *sha = fetch_ytdlp_sha256(YTDLP_ASSET);
    if (sha) {
        int v = dlm_verify_sha256(tmp, sha);
        free(sha);
        if (v != DLM_VERIFY_OK) {
            DLM_WARN("tools: yt-dlp checksum mismatch; discarding download");
            remove(tmp);
            return -1;
        }
    } else {
        DLM_WARN("tools: could not fetch yt-dlp checksum; proceeding unverified");
    }
    make_executable(tmp);
    if (dlm_rename_replace(tmp, dest) != 0) { remove(tmp); return -1; }
    DLM_INFO("tools: yt-dlp ready at %s", dest);
    return 0;
}

/* Extract ffmpeg + ffprobe from the release archive using `tar`. Returns 0 on
 * success, -1 to signal a fall back to PATH. */
static int download_ffmpeg(void)
{
#if !FFMPEG_HAVE_ARCHIVE
    DLM_WARN("tools: no ffmpeg build available to auto-download on this OS; using PATH");
    return -1;
#else
    /* tar must be available to unpack the archive */
    const char *tarcheck[] = {"tar", "--version", NULL};
    char *probe = NULL;
    if (capture_cmd(tarcheck, &probe) != 0) {
        free(probe);
        DLM_WARN("tools: `tar` unavailable; cannot unpack ffmpeg, using PATH");
        return -1;
    }
    free(probe);

    char dir[512], archive[1100], extract[1024];
    tools_dir(dir, sizeof dir);
    dlm_mkdir_p(dir);
    snprintf(archive, sizeof archive, "%s/%s", dir, FFMPEG_ARCHIVE);
    snprintf(extract, sizeof extract, "%s/ffextract", dir);
    dlm_mkdir_p(extract);

    DLM_INFO("tools: downloading ffmpeg ...");
    if (download_to_file(FFMPEG_DL_BASE FFMPEG_ARCHIVE, archive) != 0) {
        DLM_WARN("tools: ffmpeg download failed; using PATH");
        return -1;
    }

    /* list archive members so we can find the bin/ paths without a dir walk */
    const char *listing[] = {"tar", "-tf", archive, NULL};
    char *members = NULL;
    if (capture_cmd(listing, &members) != 0 || !members) {
        free(members);
        DLM_WARN("tools: cannot list ffmpeg archive; using PATH");
        remove(archive);
        return -1;
    }

    const char *wants[] = {"ffmpeg", "ffprobe"};
    int ok = 1;
    for (int w = 0; w < 2; w++) {
        char needle[64];
        snprintf(needle, sizeof needle, "/bin/%s%s", wants[w], EXE_SUFFIX);
        /* find a member ending in needle */
        char member[1024] = {0};
        char *copy = dlm_xstrdup(members);
        char *line = strtok(copy, "\n");
        while (line) {
            size_t ll = strlen(line);
            while (ll && (line[ll - 1] == '\r' || line[ll - 1] == '\n')) line[--ll] = '\0';
            size_t nl = strlen(needle);
            if (ll >= nl && strcmp(line + ll - nl, needle) == 0) {
                snprintf(member, sizeof member, "%s", line);
                break;
            }
            line = strtok(NULL, "\n");
        }
        free(copy);
        if (!member[0]) { ok = 0; DLM_WARN("tools: %s not found in archive", wants[w]); continue; }

        const char *ex[] = {"tar", "-xf", archive, "-C", extract, member, NULL};
        if (run_cmd(ex) != 0) { ok = 0; continue; }

        char src[2200], dest[1024];
        snprintf(src, sizeof src, "%s/%s", extract, member);
        managed_path(wants[w], dest, sizeof dest);
        if (dlm_rename_replace(src, dest) != 0) { ok = 0; continue; }
        make_executable(dest);
    }
    free(members);
    remove(archive);
    /* best-effort cleanup of the extract tree */
#if defined(_WIN32)
    const char *rm[] = {"cmd", "/c", "rmdir", "/s", "/q", extract, NULL};
#else
    const char *rm[] = {"rm", "-rf", extract, NULL};
#endif
    run_cmd(rm);

    if (ok) DLM_INFO("tools: ffmpeg ready");
    return ok ? 0 : -1;
#endif
}

/* ---- public entry points ---------------------------------------------- */

int dlm_tools_ensure_ready(int allow_network)
{
    if (!allow_network || !dlm_tools_auto_enabled()) return 0;

    /* Check-before-download: skip anything already usable (managed or PATH). */
    int need_ytdlp = !tool_present("yt-dlp");
    int need_ffmpeg = !tool_present("ffmpeg") || !tool_present("ffprobe");
    if (!need_ytdlp && !need_ffmpeg) return 0;

    if (lock_acquire() != 0) {
        DLM_WARN("tools: cannot acquire tools lock; skipping ensure");
        return 0;
    }
    /* re-check under the lock (another process may have just fetched) */
    if (need_ytdlp && !tool_present("yt-dlp")) {
        if (download_ytdlp() == 0) {
            char ver[64];
            json_t *root = load_state();
            json_t *yt = json_object_get(root, "ytdlp");
            if (!json_is_object(yt)) { yt = json_object(); json_object_set_new(root, "ytdlp", yt); }
            if (latest_ytdlp_version(ver, sizeof ver) == 0)
                json_object_set_new(yt, "version", json_string(ver));
            json_object_set_new(yt, "installed_at", json_integer((json_int_t)time(NULL)));
            save_state(root);
            json_decref(root);
        }
    }
    if (need_ffmpeg && (!tool_present("ffmpeg") || !tool_present("ffprobe")))
        download_ffmpeg(); /* non-fatal */

    lock_release();
    return 0;
}

int dlm_tools_check_updates(int force)
{
    if (!dlm_tools_auto_enabled()) return 0;

    json_t *root = load_state();
    json_t *yt = json_object_get(root, "ytdlp");
    if (!json_is_object(yt)) { yt = json_object(); json_object_set_new(root, "ytdlp", yt); }

    json_int_t last = 0;
    json_t *lc = json_object_get(yt, "last_check");
    if (json_is_integer(lc)) last = json_integer_value(lc);
    time_t now = time(NULL);
    if (!force && last && (now - (time_t)last) < WEEK_SECONDS) { json_decref(root); return 0; }

    char latest[64];
    if (latest_ytdlp_version(latest, sizeof latest) != 0) {
        DLM_WARN("tools: could not check yt-dlp version");
        /* still record the attempt so we don't hammer the API */
        json_object_set_new(yt, "last_check", json_integer((json_int_t)now));
        save_state(root);
        json_decref(root);
        return 0;
    }

    const char *cur = json_string_value(json_object_get(yt, "version"));
    char managed[1024];
    managed_path("yt-dlp", managed, sizeof managed);
    int have_managed = file_exists(managed);
    int outdated = !cur || !have_managed || strcmp(cur, latest) != 0;

    if (outdated) {
        if (lock_acquire() == 0) {
            DLM_INFO("tools: updating yt-dlp -> %s", latest);
            if (download_ytdlp() == 0) {
                json_object_set_new(yt, "version", json_string(latest));
                json_object_set_new(yt, "installed_at", json_integer((json_int_t)now));
            }
            lock_release();
        }
    }
    json_object_set_new(yt, "last_check", json_integer((json_int_t)now));
    save_state(root);
    json_decref(root);
    return 0;
}

int dlm_tools_status_json(char **out)
{
    if (!out) return -1;
    json_t *state = load_state();
    json_t *root = json_object();

    const char *names[] = {"yt-dlp", "ffmpeg", "ffprobe"};
    const char *keys[] = {"ytdlp", "ffmpeg", "ffprobe"};
    for (int i = 0; i < 3; i++) {
        char managed[1024];
        managed_path(names[i], managed, sizeof managed);
        json_t *t = json_object();
        json_object_set_new(t, "managed", file_exists(managed) ? json_true() : json_false());
        json_object_set_new(t, "present", tool_present(names[i]) ? json_true() : json_false());
        json_t *st = json_object_get(state, keys[i]);
        const char *ver = st ? json_string_value(json_object_get(st, "version")) : NULL;
        if (ver) json_object_set_new(t, "version", json_string(ver));
        json_object_set_new(root, names[i], t);
    }
    json_object_set_new(root, "auto", dlm_tools_auto_enabled() ? json_true() : json_false());

    *out = json_dumps(root, JSON_INDENT(2));
    json_decref(root);
    json_decref(state);
    return *out ? 0 : -1;
}
