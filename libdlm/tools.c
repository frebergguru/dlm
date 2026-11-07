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
#include <pthread.h>

#if defined(_WIN32)
#  include <io.h>
#  include <process.h>
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

/* ---- locks ------------------------------------------------------------
 *
 * Two independent locks, each an in-process mutex (threads of the daemon: the
 * hourly tools thread, IPC "tools_update", workers) plus a cross-process file
 * lock (daemon + CLI + GUI):
 *   - the TOOLS lock (<tools>/.lock) serialises downloads/installs; it is held
 *     for the whole acquire..release span, which can be long;
 *   - the STATE lock (<tools>/.state.lock) serialises state.json
 *     load-modify-save; it is only ever held briefly and never across network
 *     I/O.
 * Lock order: TOOLS may be held while taking STATE, never the reverse. Neither
 * is re-entrant: no function below takes a lock it may already hold. */

static pthread_mutex_t g_tools_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_state_mtx = PTHREAD_MUTEX_INITIALIZER;

#if defined(_WIN32)
typedef HANDLE flock_t;
#  define FLOCK_NONE INVALID_HANDLE_VALUE
/* opening with no sharing serializes processes */
static flock_t file_lock_open(const char *name)
{
    char dir[512], path[600];
    tools_dir(dir, sizeof dir);
    dlm_mkdir_p(dir);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    for (int tries = 0; tries < 300; tries++) {
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) return h;
        Sleep(100);
    }
    return FLOCK_NONE;
}
static void file_lock_close(flock_t h)
{
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}
#else
typedef int flock_t;
#  define FLOCK_NONE (-1)
static flock_t file_lock_open(const char *name)
{
    char dir[512], path[600];
    tools_dir(dir, sizeof dir);
    dlm_mkdir_p(dir);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    int fd = open(path, O_CREAT | O_RDWR | DLM_O_CLOEXEC, 0644); /* don't leak to children */
    if (fd < 0) return FLOCK_NONE;
    if (flock(fd, LOCK_EX) != 0) { close(fd); return FLOCK_NONE; }
    return fd;
}
static void file_lock_close(flock_t fd)
{
    if (fd >= 0) { flock(fd, LOCK_UN); close(fd); }
}
#endif

/* Only touched while g_tools_mtx is held. */
static flock_t g_lock = FLOCK_NONE;

/* Cross-process + cross-thread tools lock (daemon + CLI must not both
 * download). On success the caller holds it until lock_release(). */
static int lock_acquire(void)
{
    pthread_mutex_lock(&g_tools_mtx);
    flock_t h = file_lock_open(".lock");
    if (h == FLOCK_NONE) {
        pthread_mutex_unlock(&g_tools_mtx);
        return -1;
    }
    g_lock = h;
    return 0;
}

static void lock_release(void)
{
    flock_t h = g_lock;
    g_lock = FLOCK_NONE;
    file_lock_close(h);
    pthread_mutex_unlock(&g_tools_mtx);
}

/* State lock: returns the file lock (FLOCK_NONE if the file lock could not be
 * taken; the in-process mutex is still held then, so threads stay serialised). */
static flock_t state_lock(void)
{
    pthread_mutex_lock(&g_state_mtx);
    flock_t h = file_lock_open(".state.lock");
    if (h == FLOCK_NONE) DLM_WARN("tools: cannot lock state.json; continuing unlocked");
    return h;
}

static void state_unlock(flock_t h)
{
    file_lock_close(h);
    pthread_mutex_unlock(&g_state_mtx);
}

/* ---- state.json (call only with the state lock held) ------------------ */

/* Last successfully read "auto" value: -1 unknown, else 0/1. Guarded by
 * g_state_mtx. Used when state.json exists but is unreadable. */
static int g_auto_known = -1;

/* Load state.json. A missing or empty file yields a fresh object. If the file
 * exists with content but does not parse to an object, *corrupt is set (and a
 * fresh object is still returned so readers can carry on); callers must then
 * not save, so a damaged file is never silently replaced by defaults. */
static json_t *load_state_locked(int *corrupt)
{
    *corrupt = 0;
    char path[600];
    state_path(path, sizeof path);
    json_error_t e;
    json_t *root = json_load_file(path, 0, &e);
    if (root && json_is_object(root)) return root;
    if (root) json_decref(root);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) {
        DLM_WARN("tools: %s exists but cannot be parsed (%s); not overwriting it",
                 path, e.text);
        *corrupt = 1;
    }
    return json_object();
}

static int save_state_locked(json_t *root)
{
    static unsigned long counter = 0; /* guarded by g_state_mtx */
    char dir[512];
    tools_dir(dir, sizeof dir);
    dlm_mkdir_p(dir);

    char path[600], tmp[700];
    state_path(path, sizeof path);
    /* unique per process + call, so no two writers ever share a temp file */
#if defined(_WIN32)
    long pid = (long)_getpid();
#else
    long pid = (long)getpid();
#endif
    snprintf(tmp, sizeof tmp, "%s.%ld.%lu.tmp", path, pid, ++counter);

    if (json_dump_file(root, tmp, JSON_INDENT(2)) != 0) {
        DLM_WARN("tools: cannot write %s", tmp);
        remove(tmp);
        return -1;
    }
    if (dlm_rename_replace(tmp, path) != 0) {
        DLM_WARN("tools: cannot rename state into place");
        remove(tmp);
        return -1;
    }
    return 0;
}

/* Read-only snapshot of state.json (taken under the state lock). */
static json_t *load_state(void)
{
    flock_t h = state_lock();
    int corrupt = 0;
    json_t *root = load_state_locked(&corrupt);
    state_unlock(h);
    return root;
}

/* Atomic load-modify-save of state.json under the state lock. `fn` edits the
 * loaded object in place. If the existing file is unreadable it is left alone
 * (returns -1 without saving). */
static int state_update(void (*fn)(json_t *root, void *ctx), void *ctx)
{
    flock_t h = state_lock();
    int corrupt = 0;
    json_t *root = load_state_locked(&corrupt);
    int rc = -1;
    if (!corrupt) {
        fn(root, ctx);
        rc = save_state_locked(root);
    }
    json_decref(root);
    state_unlock(h);
    return rc;
}

/* Get (creating if needed) the "ytdlp" sub-object. */
static json_t *ytdlp_obj(json_t *root)
{
    json_t *yt = json_object_get(root, "ytdlp");
    if (!json_is_object(yt)) { yt = json_object(); json_object_set_new(root, "ytdlp", yt); }
    return yt;
}

/* ---- auto switch ------------------------------------------------------ */

static int env_disables_auto(void)
{
    const char *e = getenv("DLM_NO_AUTO_TOOLS");
    return e && *e && strcmp(e, "0") != 0;
}

int dlm_tools_auto_enabled(void)
{
    if (env_disables_auto()) return 0;
    flock_t h = state_lock();
    int corrupt = 0;
    json_t *root = load_state_locked(&corrupt);
    int on;
    if (corrupt) {
        /* Don't silently fall back to "on": keep the last value we saw in this
         * process, or treat auto-tools as off if we never saw one. */
        on = g_auto_known >= 0 ? g_auto_known : 0;
    } else {
        json_t *a = json_object_get(root, "auto");
        on = a ? json_is_true(a) || (json_is_integer(a) && json_integer_value(a)) : 1;
        if (!a) on = 1; /* default on */
        g_auto_known = on;
    }
    json_decref(root);
    state_unlock(h);
    return on;
}

void dlm_tools_set_auto_enabled(int on)
{
    flock_t h = state_lock();
    int corrupt = 0;
    json_t *root = load_state_locked(&corrupt);
    if (corrupt) {
        /* An explicit user choice may replace an unreadable file, but keep the
         * damaged copy aside instead of destroying it. */
        char path[600], bak[640];
        state_path(path, sizeof path);
        snprintf(bak, sizeof bak, "%s.corrupt", path);
        if (dlm_rename_replace(path, bak) == 0)
            DLM_WARN("tools: moved unreadable state.json to %s", bak);
    }
    json_object_set_new(root, "auto", on ? json_true() : json_false());
    if (save_state_locked(root) == 0) g_auto_known = on ? 1 : 0;
    json_decref(root);
    state_unlock(h);
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

/* Fetch a sha256sum-style list ("<hex>  <name>" or "<hex> *<name>" per line)
 * and return the hex for `asset` (or NULL if missing/unfetchable). */
static char *fetch_sha256_for(const char *sums_url, const char *asset)
{
    char *body = NULL;
    long status = 0;
    if (dlm_http_get(sums_url, NULL, &body, &status) != DLM_OK ||
        status != 200 || !body)
        { free(body); return NULL; }
    char *hex = NULL;
    char *save = NULL;
    size_t alen = strlen(asset);
    for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        /* hex digest = leading run of hex digits, then whitespace */
        size_t hlen = 0;
        while ((line[hlen] >= '0' && line[hlen] <= '9') ||
               (line[hlen] >= 'a' && line[hlen] <= 'f') ||
               (line[hlen] >= 'A' && line[hlen] <= 'F'))
            hlen++;
        if (hlen != 64 || (line[hlen] != ' ' && line[hlen] != '\t')) continue;
        const char *fname = line + hlen;
        while (*fname == ' ' || *fname == '\t') fname++;
        if (*fname == '*') fname++;              /* binary-mode marker */
        if (fname[0] == '.' && fname[1] == '/') fname += 2;
        size_t flen = strlen(fname);
        while (flen && (fname[flen - 1] == '\r' || fname[flen - 1] == ' ')) flen--;
        if (flen == alen && strncmp(fname, asset, alen) == 0) {
            hex = malloc(hlen + 1);
            if (hex) { memcpy(hex, line, hlen); hex[hlen] = '\0'; }
            break;
        }
    }
    free(body);
    return hex;
}

/* Fetch the SHA2-256SUMS file and return the hex for `asset` (or NULL). */
static char *fetch_ytdlp_sha256(const char *asset)
{
    return fetch_sha256_for(YTDLP_DL_BASE "SHA2-256SUMS", asset);
}

/* True if an archive member name is safe to pass to `tar -x` and to join
 * under the extract dir: not option-looking, not absolute, no ".." parts. */
static int member_name_safe(const char *m)
{
    if (!m || !*m || m[0] == '-') return 0;
    if (m[0] == '/' || m[0] == '\\') return 0;
    if (((m[0] >= 'A' && m[0] <= 'Z') || (m[0] >= 'a' && m[0] <= 'z')) && m[1] == ':')
        return 0; /* Windows drive path */
    const char *p = m;
    while (*p) {
        size_t n = strcspn(p, "/\\");
        if (n == 2 && p[0] == '.' && p[1] == '.') return 0;
        p += n;
        if (*p) p++;
    }
    return 1;
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
        /* No checksum -> don't install an unverified executable we will run.
         * Leave any existing yt-dlp in place; a later update attempt retries. */
        DLM_WARN("tools: could not fetch yt-dlp checksum; refusing to install unverified");
        remove(tmp);
        return -1;
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
        remove(archive);
        return -1;
    }

    /* Verify against the release's published checksums before unpacking
     * anything we will later execute (same policy as yt-dlp). */
    char *sha = fetch_sha256_for(FFMPEG_DL_BASE "checksums.sha256", FFMPEG_ARCHIVE);
    if (!sha) {
        DLM_WARN("tools: could not fetch ffmpeg checksum; refusing to install unverified");
        remove(archive);
        return -1;
    }
    int v = dlm_verify_sha256(archive, sha);
    free(sha);
    if (v != DLM_VERIFY_OK) {
        DLM_WARN("tools: ffmpeg checksum mismatch; discarding download");
        remove(archive);
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
        char *save = NULL;
        for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            size_t ll = strlen(line);
            while (ll && (line[ll - 1] == '\r' || line[ll - 1] == '\n')) line[--ll] = '\0';
            size_t nl = strlen(needle);
            if (ll >= nl && strcmp(line + ll - nl, needle) == 0) {
                snprintf(member, sizeof member, "%s", line);
                break;
            }
        }
        free(copy);
        if (!member[0]) { ok = 0; DLM_WARN("tools: %s not found in archive", wants[w]); continue; }
        if (!member_name_safe(member)) {
            ok = 0;
            DLM_WARN("tools: refusing unsafe archive member name for %s", wants[w]);
            continue;
        }

        /* "--" so the member can never be parsed as a tar option */
        const char *ex[] = {"tar", "-xf", archive, "-C", extract, "--", member, NULL};
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

/* ---- state edits ------------------------------------------------------ */

struct ytdlp_install {
    const char *version; /* installed version, or NULL if not installed/unknown */
    char vbuf[64];
    time_t when;         /* installed_at (when version != NULL or installed) */
    time_t last_check;   /* 0 = leave unchanged */
    int installed;       /* set by check_updates; ensure_ready always installs */
};

static void apply_ytdlp_install(json_t *root, void *ctx)
{
    const struct ytdlp_install *ins = ctx;
    json_t *yt = ytdlp_obj(root);
    if (ins->version)
        json_object_set_new(yt, "version", json_string(ins->version));
    json_object_set_new(yt, "installed_at", json_integer((json_int_t)ins->when));
    if (ins->last_check)
        json_object_set_new(yt, "last_check", json_integer((json_int_t)ins->last_check));
}

static void apply_last_check(json_t *root, void *ctx)
{
    const struct ytdlp_install *ins = ctx;
    json_object_set_new(ytdlp_obj(root), "last_check", json_integer((json_int_t)ins->last_check));
}

/* Current recorded yt-dlp version (malloc'd) and last_check, read under the
 * state lock. */
static char *recorded_ytdlp(json_int_t *last_check)
{
    json_t *root = load_state();
    json_t *yt = json_object_get(root, "ytdlp");
    if (last_check) {
        json_t *lc = json_object_get(yt, "last_check");
        *last_check = json_is_integer(lc) ? json_integer_value(lc) : 0;
    }
    const char *v = json_string_value(json_object_get(yt, "version"));
    char *out = v ? dlm_xstrdup(v) : NULL;
    json_decref(root);
    return out;
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
            struct ytdlp_install ins;
            memset(&ins, 0, sizeof ins);
            ins.version = latest_ytdlp_version(ins.vbuf, sizeof ins.vbuf) == 0 ? ins.vbuf : NULL;
            ins.when = time(NULL);
            state_update(apply_ytdlp_install, &ins); /* network done before locking */
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

    /* Snapshot state, then do network I/O with no state lock held; the final
     * write is a separate atomic load-modify-save, so concurrent callers can
     * never clobber each other's fields. */
    json_int_t last = 0;
    char *cur = recorded_ytdlp(&last);
    time_t now = time(NULL);
    if (!force && last && (now - (time_t)last) < WEEK_SECONDS) { free(cur); return 0; }

    struct ytdlp_install ins;
    memset(&ins, 0, sizeof ins);
    ins.last_check = now;

    char latest[64];
    if (latest_ytdlp_version(latest, sizeof latest) != 0) {
        DLM_WARN("tools: could not check yt-dlp version");
        /* still record the attempt so we don't hammer the API */
        state_update(apply_last_check, &ins);
        free(cur);
        return 0;
    }

    char managed[1024];
    managed_path("yt-dlp", managed, sizeof managed);
    int outdated = !cur || !file_exists(managed) || strcmp(cur, latest) != 0;
    free(cur);

    if (outdated && lock_acquire() == 0) {
        /* re-check under the tools lock: another thread/process may have just
         * installed this version while we were waiting */
        char *again = recorded_ytdlp(NULL);
        int still = !again || !file_exists(managed) || strcmp(again, latest) != 0;
        free(again);
        if (still) {
            DLM_INFO("tools: updating yt-dlp -> %s", latest);
            if (download_ytdlp() == 0) {
                snprintf(ins.vbuf, sizeof ins.vbuf, "%s", latest);
                ins.version = ins.vbuf;
                ins.when = now;
                ins.installed = 1;
            }
        }
        lock_release();
    }
    state_update(ins.installed ? apply_ytdlp_install : apply_last_check, &ins);
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
