/* dlm — command-line client.
 *
 *   dlm get <url> [-o out] [-c n]   download directly (in-process, no daemon)
 *   dlm add <url> [-o out] [-c n]   enqueue on the daemon
 *   dlm ls                          list downloads
 *   dlm watch                       live-updating status table
 *   dlm pause|resume|cancel|rm <id>
 *   dlm set -j <n>                  set max concurrent downloads
 *   dlm version
 */
#define _POSIX_C_SOURCE 200809L
#include "client.h"
#include "dlm/dlm.h"
#include "dlm/extract.h"
#include "dlm/iaauth.h"
#include "dlm/verify.h"

#include <jansson.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ===================== direct (in-process) get ======================== */

static volatile int g_cancel = 0;
static void on_sigint(int s) { (void)s; g_cancel = 1; }

static void human(double n, char *buf, size_t len)
{
    static const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int i = 0;
    while (n >= 1024.0 && i < 4) { n /= 1024.0; i++; }
    snprintf(buf, len, "%.1f %s", n, u[i]);
}

/* Default download directory: $DLM_DOWNLOAD_DIR, else ~/Downloads if it exists,
 * else the current directory. Written into buf. */
static void default_download_dir(char *buf, size_t len)
{
    const char *env = getenv("DLM_DOWNLOAD_DIR");
    if (env && *env) { snprintf(buf, len, "%s", env); return; }
    const char *home = getenv("HOME");
    if (home && *home) {
        char dl[1024];
        snprintf(dl, sizeof dl, "%s/Downloads", home);
        struct stat st;
        if (stat(dl, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(buf, len, "%s", dl);
            return;
        }
    }
    snprintf(buf, len, ".");
}

static void progress(void *ud, int64_t done, int64_t total, double bps)
{
    (void)ud;
    char db[32], sb[32];
    human((double)done, db, sizeof db);
    human(bps, sb, sizeof sb);
    if (total > 0) {
        double pct = 100.0 * (double)done / (double)total;
        int width = 30, filled = (int)(pct / 100.0 * width);
        char bar[64];
        for (int i = 0; i < width; i++) bar[i] = i < filled ? '#' : '-';
        bar[width] = '\0';
        char tb[32];
        human((double)total, tb, sizeof tb);
        fprintf(stderr, "\r[%s] %5.1f%%  %s / %s  %s/s   ", bar, pct, db, tb, sb);
    } else {
        fprintf(stderr, "\r%s  %s/s   ", db, sb);
    }
    fflush(stderr);
}

/* Known container extensions yt-dlp can merge into. */
static const char *merge_ext(const char *out_path)
{
    const char *dot = strrchr(out_path, '.');
    if (!dot) return NULL;
    const char *ext = dot + 1;
    static const char *ok[] = {"mp4", "mkv", "webm", "ogg", "flv", "mov", NULL};
    for (int i = 0; ok[i]; i++)
        if (!strcasecmp(ext, ok[i])) return ext;
    return NULL;
}

/* Hand a fragmented-stream task to yt-dlp to download to out_path. */
static int delegate_to_ytdlp(const dlm_task *t, const char *out_path, int64_t limit)
{
    fprintf(stderr, "dlm: stream -> delegating to yt-dlp: %s -> %s\n", t->url, out_path);

    /* build argv dynamically: base + optional --limit-rate + optional
     * --merge-output-format so the final file matches the requested name */
    const char *argv[16];
    int n = 0;
    argv[n++] = "yt-dlp";
    argv[n++] = "--no-warnings";
    argv[n++] = "--no-playlist";
    char rate[32];
    if (limit > 0) {
        snprintf(rate, sizeof rate, "%lld", (long long)limit);
        argv[n++] = "--limit-rate";
        argv[n++] = rate;
    }
    const char *mext = merge_ext(out_path);
    if (mext) {
        argv[n++] = "--merge-output-format";
        argv[n++] = mext;
    }
    argv[n++] = "-o";
    argv[n++] = out_path;
    argv[n++] = "--";
    argv[n++] = t->url;
    argv[n] = NULL;

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        execvp("yt-dlp", (char *const *)argv);
        fprintf(stderr, "dlm: cannot exec yt-dlp (is it installed?)\n");
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int ec = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (ec != 0) fprintf(stderr, "dlm: yt-dlp exited %d\n", ec);
    return ec == 0 ? 0 : 1;
}

/* Download one extracted task to out_path, then verify md5/sha1 if present. */
static int download_task(const dlm_task *t, const char *out_path, int conns,
                         int64_t limit)
{
    if (t->delegate) return delegate_to_ytdlp(t, out_path, limit);

    dlm_options opt;
    memset(&opt, 0, sizeof opt);
    opt.url = t->url;
    opt.out_path = out_path;
    opt.connections = conns;
    opt.max_speed = limit;
    opt.on_progress = progress;
    opt.cancel = &g_cancel;
    opt.headers = (const char *const *)t->headers;

    fprintf(stderr, "dlm: %s -> %s\n", t->url, out_path);
    dlm_result r = dlm_download_file(&opt);
    fputc('\n', stderr);
    if (r != DLM_OK) {
        fprintf(stderr, "dlm: failed: %s\n", dlm_strerror(r));
        return 1;
    }
    if (t->md5) {
        int v = dlm_verify_md5(out_path, t->md5);
        fprintf(stderr, "dlm: md5 %s\n",
                v == DLM_VERIFY_OK ? "verified \xE2\x9C\x93"
                : v == DLM_VERIFY_MISMATCH ? "MISMATCH \xE2\x9C\x97" : "unverifiable");
        if (v == DLM_VERIFY_MISMATCH) return 1;
    } else if (t->sha1) {
        int v = dlm_verify_sha1(out_path, t->sha1);
        fprintf(stderr, "dlm: sha1 %s\n",
                v == DLM_VERIFY_OK ? "verified \xE2\x9C\x93"
                : v == DLM_VERIFY_MISMATCH ? "MISMATCH \xE2\x9C\x97" : "unverifiable");
        if (v == DLM_VERIFY_MISMATCH) return 1;
    }
    return 0;
}

static int cmd_get(int argc, char **argv)
{
    const char *url = NULL, *out = NULL, *dir = NULL;
    int conns = 0;
    int64_t limit = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if ((!strcmp(argv[i], "-d") || !strcmp(argv[i], "--dir")) && i + 1 < argc)
            dir = argv[++i];
        else if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--connections")) && i + 1 < argc)
            conns = atoi(argv[++i]);
        else if ((!strcmp(argv[i], "-l") || !strcmp(argv[i], "--limit")) && i + 1 < argc)
            limit = dlm_parse_rate(argv[++i]);
        else if (argv[i][0] != '-') url = argv[i];
    }
    if (!url) {
        fprintf(stderr, "usage: dlm get <url> [-o out] [-d dir] [-c n] [--limit RATE]\n");
        return 2;
    }

    signal(SIGINT, on_sigint);
    if (limit > 0) {
        char rb[32];
        dlm_format_rate(limit, rb, sizeof rb);
        fprintf(stderr, "dlm: rate limited to %s\n", rb);
    }

    dlm_extract_result res;
    int er = dlm_extract(url, &res);
    if (er != DLM_OK || res.count == 0) {
        fprintf(stderr, "dlm: could not resolve '%s': %s\n", url, dlm_strerror(er));
        return 1;
    }

    int rc = 0;
    if (res.count == 1 && out) {
        rc = download_task(&res.tasks[0], out, conns, limit);
    } else {
        /* one or more files: place them under the chosen directory */
        const char *d = dir ? dir : ".";
        if (strcmp(d, ".") != 0) mkdir(d, 0755);
        if (res.count > 1)
            fprintf(stderr, "dlm: %s -> %d files into %s/\n", res.source, res.count, d);
        int okc = 0;
        for (int i = 0; i < res.count && !g_cancel; i++) {
            char path[4096];
            snprintf(path, sizeof path, "%s/%s", d, res.tasks[i].filename);
            if (res.count > 1) fprintf(stderr, "[%d/%d] ", i + 1, res.count);
            if (download_task(&res.tasks[i], path, conns, limit) == 0) okc++;
            else rc = 1;
        }
        if (res.count > 1) fprintf(stderr, "dlm: %d/%d files ok\n", okc, res.count);
    }

    dlm_extract_result_free(&res);
    dlm_global_cleanup();
    return rc;
}

/* Resolve a URL and print the resulting tasks without downloading. */
static int cmd_resolve(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "usage: dlm resolve <url>\n"); return 2; }
    dlm_extract_result res;
    int r = dlm_extract(argv[0], &res);
    if (r != DLM_OK) {
        fprintf(stderr, "dlm: could not resolve: %s\n", dlm_strerror(r));
        return 1;
    }
    printf("source: %s  (%d task%s)\n", res.source, res.count,
           res.count == 1 ? "" : "s");
    for (int i = 0; i < res.count; i++) {
        dlm_task *t = &res.tasks[i];
        int nh = 0;
        for (int j = 0; t->headers && t->headers[j]; j++) nh++;
        printf("  [%d] %-9s %-28s size=%lld hdrs=%d md5=%s\n      %s\n",
               i + 1, t->delegate ? "DELEGATE" : "direct",
               t->filename, (long long)t->size, nh, t->md5 ? "yes" : "-", t->url);
    }
    dlm_extract_result_free(&res);
    return 0;
}

/* ===================== archive.org auth =============================== */

static char *prompt_hidden(const char *prompt)
{
    fprintf(stderr, "%s", prompt);
    fflush(stderr);
    struct termios old, no;
    int have_tty = tcgetattr(STDIN_FILENO, &old) == 0;
    if (have_tty) { no = old; no.c_lflag &= ~(tcflag_t)ECHO; tcsetattr(STDIN_FILENO, TCSAFLUSH, &no); }
    char buf[512];
    char *r = fgets(buf, sizeof buf, stdin);
    if (have_tty) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &old); fputc('\n', stderr); }
    if (!r) return NULL;
    buf[strcspn(buf, "\r\n")] = '\0';
    return strdup(buf);
}

static int cmd_ia_status(void)
{
    ia_credentials c;
    dlm_ia_load(&c);
    printf("archive.org: %s\n", dlm_ia_mode_str(c.mode));
    if (c.mode == IA_AUTH_S3) printf("  access key: %s\n", c.access);
    dlm_ia_credentials_free(&c);
    return 0;
}

static int cmd_ia_login(int argc, char **argv)
{
    if (argc >= 1 && !strcmp(argv[0], "--s3")) {
        if (argc < 3) { fprintf(stderr, "usage: dlm ia-login --s3 <access> <secret>\n"); return 2; }
        int rc = dlm_ia_save_s3(argv[1], argv[2]);
        if (rc == 0) printf("saved S3 keys (signed in)\n");
        else fprintf(stderr, "dlm: failed to save keys\n");
        return rc == 0 ? 0 : 1;
    }
    if (argc >= 1 && !strcmp(argv[0], "--cookie")) {
        if (argc < 2) { fprintf(stderr, "usage: dlm ia-login --cookie '<cookie>'\n"); return 2; }
        int rc = dlm_ia_save_cookie(argv[1]);
        if (rc == 0) printf("saved session cookie (signed in)\n");
        return rc == 0 ? 0 : 1;
    }
    if (argc >= 2 && !strcmp(argv[0], "--email")) {
        char *pw = prompt_hidden("archive.org password: ");
        if (!pw) return 1;
        char *err = NULL;
        int rc = dlm_ia_login_password(argv[1], pw, &err);
        free(pw);
        if (rc == 0) printf("logged in as %s\n", argv[1]);
        else { fprintf(stderr, "dlm: login failed: %s\n", err ? err : "?"); free(err); }
        return rc == 0 ? 0 : 1;
    }
    fprintf(stderr,
            "usage:\n"
            "  dlm ia-login --s3 <access> <secret>   use S3 keys (recommended)\n"
            "  dlm ia-login --cookie '<cookie>'      use a pasted session cookie\n"
            "  dlm ia-login --email <addr>           log in with email + password\n");
    return 2;
}

static int cmd_ia_logout(void)
{
    dlm_ia_logout();
    printf("signed out (anonymous)\n");
    return 0;
}

/* ===================== daemon lifecycle =============================== */

static int cmd_shutdown(void)
{
    int fd = dlm_client_connect_existing();
    if (fd < 0) { printf("daemon not running\n"); return 0; }
    char *resp = dlm_client_rpc(fd, "{\"cmd\":\"shutdown\"}");
    free(resp);
    close(fd);
    printf("daemon shutting down\n");
    return 0;
}

static int cmd_restart(void)
{
    int fd = dlm_client_connect_existing();
    if (fd >= 0) {
        char *resp = dlm_client_rpc(fd, "{\"cmd\":\"shutdown\"}");
        free(resp);
        close(fd);
        /* wait for the old socket to disappear */
        for (int i = 0; i < 30; i++) {
            struct timespec ts = {0, 100 * 1000 * 1000};
            nanosleep(&ts, NULL);
            int t = dlm_client_connect_existing();
            if (t < 0) break;
            close(t);
        }
    }
    fd = dlm_client_connect(); /* spawns a fresh daemon */
    if (fd < 0) { fprintf(stderr, "dlm: could not start daemon\n"); return 1; }
    char *resp = dlm_client_rpc(fd, "{\"cmd\":\"ping\"}");
    free(resp);
    close(fd);
    printf("daemon restarted\n");
    return 0;
}

/* ===================== daemon-routed commands ========================= */

/* Parse a response, print error if not ok. Returns the parsed root (caller
 * decrefs) or NULL on transport/parse error. */
static json_t *parse_resp(char *resp)
{
    if (!resp) { fprintf(stderr, "dlm: no response from daemon\n"); return NULL; }
    json_error_t e;
    json_t *root = json_loads(resp, 0, &e);
    free(resp);
    if (!root) { fprintf(stderr, "dlm: bad response\n"); return NULL; }
    return root;
}

static int check_ok(json_t *root)
{
    if (!json_is_true(json_object_get(root, "ok"))) {
        const char *err = json_string_value(json_object_get(root, "error"));
        fprintf(stderr, "dlm: %s\n", err ? err : "request failed");
        return 0;
    }
    return 1;
}

static int rpc_simple(const char *req)
{
    int fd = dlm_client_connect();
    if (fd < 0) return 1;
    json_t *root = parse_resp(dlm_client_rpc(fd, req));
    close(fd);
    if (!root) return 1;
    int ok = check_ok(root);
    json_decref(root);
    return ok ? 0 : 1;
}

/* Send one add request; returns 1 on success, 0 on failure. */
static int send_add(int fd, const char *url, const char *out_path, int conns,
                    int delegate)
{
    json_t *req = json_object();
    json_object_set_new(req, "cmd", json_string("add"));
    json_object_set_new(req, "url", json_string(url));
    if (out_path) json_object_set_new(req, "out", json_string(out_path));
    if (conns) json_object_set_new(req, "connections", json_integer(conns));
    if (delegate) json_object_set_new(req, "delegate", json_true());
    char *reqs = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    json_t *root = parse_resp(dlm_client_rpc(fd, reqs));
    free(reqs);
    int ok = root && check_ok(root);
    if (root) json_decref(root);
    return ok ? 1 : 0;
}

static int cmd_add(int argc, char **argv)
{
    const char *url = NULL, *out = NULL, *dir = NULL;
    int conns = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if ((!strcmp(argv[i], "-d") || !strcmp(argv[i], "--dir")) && i + 1 < argc)
            dir = argv[++i];
        else if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--connections")) && i + 1 < argc)
            conns = atoi(argv[++i]);
        else if (argv[i][0] != '-') url = argv[i];
    }
    if (!url) {
        fprintf(stderr, "usage: dlm add <url> [-o out] [-d dir] [-c n]\n");
        return 2;
    }

    /* resolve the URL (archive.org item, yt-dlp page/series, or direct file)
     * into concrete tasks, then enqueue each on the daemon */
    dlm_extract_result res;
    int er = dlm_extract(url, &res);
    if (er != DLM_OK || res.count == 0) {
        fprintf(stderr, "dlm: could not resolve '%s': %s\n", url, dlm_strerror(er));
        return 1;
    }

    /* destination directory: -d, else the default download dir */
    char dbuf[1024];
    if (!dir) { default_download_dir(dbuf, sizeof dbuf); dir = dbuf; }
    if (strcmp(dir, ".") != 0) mkdir(dir, 0755);

    int fd = dlm_client_connect();
    if (fd < 0) { dlm_extract_result_free(&res); return 1; }

    int added = 0;
    for (int i = 0; i < res.count; i++) {
        char path[4096];
        if (res.count == 1 && out)
            snprintf(path, sizeof path, "%s", out);
        else
            snprintf(path, sizeof path, "%s/%s", dir, res.tasks[i].filename);
        added += send_add(fd, res.tasks[i].url, path, conns, res.tasks[i].delegate);
    }
    close(fd);
    if (res.count == 1)
        printf("added 1 download (%s) to %s\n", res.source, dir);
    else
        printf("added %d/%d files (%s) to %s/\n", added, res.count, res.source, dir);
    dlm_extract_result_free(&res);
    return added > 0 ? 0 : 1;
}

static int cmd_id_op(const char *cmd, int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "usage: dlm %s <id>\n", cmd); return 2; }
    json_t *req = json_object();
    json_object_set_new(req, "cmd", json_string(cmd));
    json_object_set_new(req, "id", json_integer(atoll(argv[0])));
    char *reqs = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    int rc = rpc_simple(reqs);
    free(reqs);
    if (rc == 0) printf("%s #%s ok\n", cmd, argv[0]);
    return rc;
}

static int cmd_set(int argc, char **argv)
{
    int max_active = -1;
    int64_t limit = -1;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-j") && i + 1 < argc) max_active = atoi(argv[++i]);
        else if ((!strcmp(argv[i], "-l") || !strcmp(argv[i], "--limit")) && i + 1 < argc) {
            const char *v = argv[++i];
            /* "0", "off" or "none" clears the cap */
            limit = (!strcmp(v, "off") || !strcmp(v, "none")) ? 0 : dlm_parse_rate(v);
            if (limit < 0) limit = 0;
        }
    }
    if (max_active < 0 && limit < 0) {
        fprintf(stderr, "usage: dlm set [-j <n>] [--limit <rate|off>]\n");
        return 2;
    }
    json_t *req = json_object();
    json_object_set_new(req, "cmd", json_string("set"));
    if (max_active >= 0) json_object_set_new(req, "max_active", json_integer(max_active));
    if (limit >= 0) json_object_set_new(req, "max_speed", json_integer(limit));
    char *reqs = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    int rc = rpc_simple(reqs);
    free(reqs);
    if (rc == 0) {
        if (max_active >= 0) printf("max concurrent downloads: %d\n", max_active);
        if (limit >= 0) {
            char rb[32];
            dlm_format_rate(limit, rb, sizeof rb);
            printf("global speed limit: %s\n", rb);
        }
    }
    return rc;
}

/* JDownloader priority levels (name <-> numeric -3..3). */
static const struct { const char *name; int level; } PRIOS[] = {
    {"highest", 3}, {"higher", 2}, {"high", 1}, {"default", 0},
    {"low", -1}, {"lower", -2}, {"lowest", -3},
};

static int parse_priority(const char *s, int *out)
{
    if (!s) return -1;
    for (size_t i = 0; i < sizeof PRIOS / sizeof PRIOS[0]; i++)
        if (!strcasecmp(s, PRIOS[i].name)) { *out = PRIOS[i].level; return 0; }
    char *end;
    long v = strtol(s, &end, 10);
    if (*s && !*end && v >= -3 && v <= 3) { *out = (int)v; return 0; }
    return -1;
}

/* Short priority tag for the table ("" for default). */
static const char *prio_tag(int level)
{
    switch (level) {
    case 3: return "HST"; case 2: return "HR"; case 1: return "HI";
    case -1: return "LO"; case -2: return "LR"; case -3: return "LST";
    default: return "";
    }
}

/* One-line link row, indented under its package. */
static void print_link_row(json_t *o)
{
    long long id = json_integer_value(json_object_get(o, "id"));
    const char *state = json_string_value(json_object_get(o, "state"));
    long long total = json_integer_value(json_object_get(o, "total"));
    long long done = json_integer_value(json_object_get(o, "downloaded"));
    double speed = json_real_value(json_object_get(o, "speed"));
    const char *path = json_string_value(json_object_get(o, "out_path"));
    const char *nm = json_string_value(json_object_get(o, "name"));
    int prio = json_integer_value(json_object_get(o, "priority"));
    int enabled = json_is_true(json_object_get(o, "enabled"));
    int autostart = json_is_true(json_object_get(o, "autostart"));
    int force = json_is_true(json_object_get(o, "force"));
    const char *avail = json_string_value(json_object_get(o, "availability"));

    const char *name = nm && *nm ? nm : (path ? path : "");
    const char *slash = strrchr(name, '/');
    if (slash) name = slash + 1;

    char pct[8];
    if (total > 0) snprintf(pct, sizeof pct, "%.0f%%", 100.0 * (double)done / (double)total);
    else snprintf(pct, sizeof pct, "-");
    char hb[24], size[24];
    human((double)total, hb, sizeof hb);
    snprintf(size, sizeof size, "%s", total > 0 ? hb : "?");
    char sb[24], spd[28];
    human(speed, sb, sizeof sb);
    snprintf(spd, sizeof spd, "%s/s", sb);

    /* flags: disabled / manual(no-autostart) / force / offline */
    char flags[8];
    int f = 0;
    if (!enabled) flags[f++] = 'x';
    if (!autostart) flags[f++] = 'm';
    if (force) flags[f++] = 'F';
    if (avail && !strcmp(avail, "offline")) flags[f++] = 'X';
    flags[f] = '\0';

    printf("  %-4lld %-8s %6s %10s %10s %-3s %-3s %s\n", id, state ? state : "?",
           pct, size, speed > 1 ? spd : "-", prio_tag(prio), flags, name);
}

/* Render the downloads/packages of a response, grouped into packages, showing
 * only the requested list ("download" or "linkgrabber"). */
static void render_downloads(json_t *root, const char *want_list, int clear)
{
    json_t *dls = json_object_get(root, "downloads");
    json_t *pkgs = json_object_get(root, "packages");
    if (!json_is_array(dls)) return;
    if (clear) printf("\033[H\033[2J"); /* home + clear for watch mode */
    printf("  %-4s %-8s %6s %10s %10s %-3s %-3s %s\n", "ID", "STATE", "%",
           "SIZE", "SPEED", "PRI", "FLG", "NAME");

    size_t i;
    json_t *o;
    int shown = 0;

    /* packaged links */
    if (json_is_array(pkgs)) {
        json_t *p;
        json_array_foreach(pkgs, i, p) {
            const char *plist = json_string_value(json_object_get(p, "list"));
            if (!plist || strcmp(plist, want_list)) continue;
            long long pid = json_integer_value(json_object_get(p, "id"));
            const char *pname = json_string_value(json_object_get(p, "name"));
            const char *folder = json_string_value(json_object_get(p, "folder"));
            int pprio = json_integer_value(json_object_get(p, "priority"));
            printf("[p%lld] %s%s%s%s%s\n", pid, pname ? pname : "package",
                   folder ? "  ("  : "", folder ? folder : "", folder ? ")" : "",
                   *prio_tag(pprio) ? "" : "");
            size_t j;
            json_t *d;
            json_array_foreach(dls, j, d) {
                long long dpid = json_integer_value(json_object_get(d, "package_id"));
                const char *dlist = json_string_value(json_object_get(d, "list"));
                if (dpid == pid && dlist && !strcmp(dlist, want_list)) {
                    print_link_row(d);
                    shown++;
                }
            }
        }
    }
    /* links with no package — or whose package lives in the other list — are
     * shown ungrouped so nothing in this list can be hidden */
    int printed_loose_header = 0;
    json_array_foreach(dls, i, o) {
        long long dpid = json_integer_value(json_object_get(o, "package_id"));
        const char *dlist = json_string_value(json_object_get(o, "list"));
        if (!dlist || strcmp(dlist, want_list)) continue;
        int has_pkg_here = 0;
        if (dpid != 0 && json_is_array(pkgs)) {
            size_t k;
            json_t *p;
            json_array_foreach(pkgs, k, p) {
                const char *pl = json_string_value(json_object_get(p, "list"));
                if (json_integer_value(json_object_get(p, "id")) == dpid &&
                    pl && !strcmp(pl, want_list)) { has_pkg_here = 1; break; }
            }
        }
        if (has_pkg_here) continue; /* already printed under its package */
        if (!printed_loose_header) { printf("[ungrouped]\n"); printed_loose_header = 1; }
        print_link_row(o);
        shown++;
    }
    if (!shown) printf("  (empty)\n");
}

static int cmd_ls(void)
{
    int fd = dlm_client_connect();
    if (fd < 0) return 1;
    json_t *root = parse_resp(dlm_client_rpc(fd, "{\"cmd\":\"list\"}"));
    close(fd);
    if (!root) return 1;
    int rc = 1;
    if (check_ok(root)) {
        json_t *ms = json_object_get(root, "max_speed");
        long long ma = json_integer_value(json_object_get(root, "max_active"));
        int autostart = json_is_true(json_object_get(root, "autostart"));
        char rb[32];
        dlm_format_rate(json_integer_value(ms), rb, sizeof rb);
        printf("max concurrent: %lld   global limit: %s   downloads: %s\n", ma,
               rb, autostart ? "running" : "stopped (manual)");
        render_downloads(root, "download", 0);
        rc = 0;
    }
    json_decref(root);
    return rc;
}

/* List the linkgrabber (staged links awaiting confirmation). */
static int cmd_lg(void)
{
    int fd = dlm_client_connect();
    if (fd < 0) return 1;
    json_t *root = parse_resp(dlm_client_rpc(fd, "{\"cmd\":\"list\"}"));
    close(fd);
    if (!root) return 1;
    int rc = 1;
    if (check_ok(root)) {
        printf("linkgrabber (confirm to move into the download queue)\n");
        render_downloads(root, "linkgrabber", 0);
        rc = 0;
    }
    json_decref(root);
    return rc;
}

static int cmd_watch(void)
{
    int fd = dlm_client_connect();
    if (fd < 0) return 1;
    signal(SIGINT, on_sigint);
    char *resp = dlm_client_rpc(fd, "{\"cmd\":\"subscribe\"}");
    json_t *root = parse_resp(resp);
    if (root) { render_downloads(root, "download", 1); json_decref(root); }
    while (!g_cancel) {
        char *line = dlm_client_read_line(fd);
        if (!line) break;
        json_error_t e;
        json_t *ev = json_loads(line, 0, &e);
        free(line);
        if (ev) {
            const char *evt = json_string_value(json_object_get(ev, "event"));
            if (evt && !strcmp(evt, "progress")) render_downloads(ev, "download", 1);
            json_decref(ev);
        }
    }
    close(fd);
    return 0;
}

/* ===================== queue / linkgrabber ops ======================== */

static json_t *req_obj(const char *cmd)
{
    json_t *o = json_object();
    json_object_set_new(o, "cmd", json_string(cmd));
    return o;
}

/* Send a built request; on success optionally print "<verb> <count_key>". */
static int send_req(json_t *req, const char *count_key, const char *verb)
{
    char *s = json_dumps(req, JSON_COMPACT);
    json_decref(req);
    int fd = dlm_client_connect();
    if (fd < 0) { free(s); return 1; }
    json_t *root = parse_resp(dlm_client_rpc(fd, s));
    free(s);
    close(fd);
    if (!root) return 1;
    int rc = 1;
    if (check_ok(root)) {
        rc = 0;
        if (verb) {
            if (count_key)
                printf("%s %lld\n", verb,
                       (long long)json_integer_value(json_object_get(root, count_key)));
            else
                printf("%s\n", verb);
        }
    }
    json_decref(root);
    return rc;
}

/* First non-flag arg as id; sets *is_pkg if -p/--package is present. */
static int64_t arg_id_pkg(int argc, char **argv, int *is_pkg)
{
    int64_t id = -1;
    *is_pkg = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--package")) *is_pkg = 1;
        else if (argv[i][0] != '-' && id < 0) {
            const char *a = argv[i];
            if (*a == 'p') { *is_pkg = 1; a++; } /* allow "p7" to mean package 7 */
            id = atoll(a);
        }
    }
    return id;
}

/* Crawl a URL and stage the resulting links into the linkgrabber. */
static int cmd_grab(int argc, char **argv)
{
    const char *url = NULL, *dir = NULL;
    int conns = 0;
    for (int i = 0; i < argc; i++) {
        if ((!strcmp(argv[i], "-d") || !strcmp(argv[i], "--dir")) && i + 1 < argc)
            dir = argv[++i];
        else if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--connections")) && i + 1 < argc)
            conns = atoi(argv[++i]);
        else if (argv[i][0] != '-') url = argv[i];
    }
    if (!url) { fprintf(stderr, "usage: dlm grab <url> [-d dir] [-c n]\n"); return 2; }

    dlm_extract_result res;
    int er = dlm_extract(url, &res);
    if (er != DLM_OK || res.count == 0) {
        fprintf(stderr, "dlm: could not resolve '%s': %s\n", url, dlm_strerror(er));
        return 1;
    }
    char dbuf[1024];
    if (!dir) { default_download_dir(dbuf, sizeof dbuf); dir = dbuf; }

    /* package name: the URL's last path component, else the extractor name */
    char pkgname[256];
    const char *base = strrchr(url, '/');
    base = base ? base + 1 : url;
    size_t blen = strcspn(base, "?#");
    if (blen == 0 || blen >= sizeof pkgname) snprintf(pkgname, sizeof pkgname, "%s", res.source);
    else { memcpy(pkgname, base, blen); pkgname[blen] = '\0'; }

    json_t *req = req_obj("grab");
    json_object_set_new(req, "name", json_string(pkgname));
    json_object_set_new(req, "folder", json_string(dir));
    json_t *links = json_array();
    for (int i = 0; i < res.count; i++) {
        dlm_task *t = &res.tasks[i];
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, t->filename);
        json_t *l = json_object();
        json_object_set_new(l, "url", json_string(t->url));
        json_object_set_new(l, "out", json_string(path));
        json_object_set_new(l, "name", json_string(t->filename));
        json_object_set_new(l, "size", json_integer(t->size));
        if (conns) json_object_set_new(l, "connections", json_integer(conns));
        if (t->delegate) json_object_set_new(l, "delegate", json_true());
        json_object_set_new(l, "availability",
                            json_string(t->size >= 0 ? "online" : "unknown"));
        json_array_append_new(links, l);
    }
    json_object_set_new(req, "links", links);

    int count = res.count;
    const char *src = res.source;
    char verb[128];
    snprintf(verb, sizeof verb, "staged %d link%s (%s) in the linkgrabber — package",
             count, count == 1 ? "" : "s", src);
    dlm_extract_result_free(&res);
    return send_req(req, "package_id", verb);
}

static int cmd_confirm(int argc, char **argv)
{
    int is_pkg;
    int64_t id = arg_id_pkg(argc, argv, &is_pkg);
    int start = 1;
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--no-start") || !strcmp(argv[i], "-n")) start = 0;
    json_t *req = req_obj("confirm");
    if (id > 0) json_object_set_new(req, "id", json_integer(id));
    if (is_pkg) json_object_set_new(req, "package", json_true());
    json_object_set_new(req, "start", json_boolean(start));
    return send_req(req, "moved", "confirmed links:");
}

static int cmd_lg_remove(int argc, char **argv)
{
    int is_pkg;
    int64_t id = arg_id_pkg(argc, argv, &is_pkg);
    if (id <= 0) { fprintf(stderr, "usage: dlm lg-rm <id> [-p]\n"); return 2; }
    json_t *req = req_obj("lg_remove");
    json_object_set_new(req, "id", json_integer(id));
    if (is_pkg) json_object_set_new(req, "package", json_true());
    return send_req(req, "removed", "removed from linkgrabber:");
}

static int cmd_lg_clear(void)
{
    return send_req(req_obj("lg_clear"), "removed", "cleared linkgrabber:");
}

static int cmd_priority(int argc, char **argv)
{
    int is_pkg = 0;
    int64_t id = -1;
    const char *lvl = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--package")) { is_pkg = 1; continue; }
        /* first token => id, second => priority level (which may be e.g. "-3") */
        if (id < 0) {
            const char *a = argv[i];
            if (*a == 'p') { is_pkg = 1; a++; }
            id = atoll(a);
        } else if (!lvl) {
            lvl = argv[i];
        }
    }
    int level;
    if (id <= 0 || parse_priority(lvl, &level) != 0) {
        fprintf(stderr, "usage: dlm priority <id> <highest|higher|high|default|low|lower|lowest | -3..3> [-p]\n");
        return 2;
    }
    json_t *req = req_obj("priority");
    json_object_set_new(req, "id", json_integer(id));
    json_object_set_new(req, "level", json_integer(level));
    if (is_pkg) json_object_set_new(req, "package", json_true());
    return send_req(req, NULL, "priority set");
}

/* enable / disable / force: single id + optional -p */
static int cmd_flag(const char *cmd, const char *verb, int argc, char **argv)
{
    int is_pkg;
    int64_t id = arg_id_pkg(argc, argv, &is_pkg);
    if (id <= 0) { fprintf(stderr, "usage: dlm %s <id> [-p]\n", cmd); return 2; }
    json_t *req = req_obj(cmd);
    json_object_set_new(req, "id", json_integer(id));
    if (is_pkg) json_object_set_new(req, "package", json_true());
    return send_req(req, NULL, verb);
}

static int cmd_auto(int argc, char **argv)
{
    int is_pkg = 0, on = -1;
    int64_t id = -1;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--package")) is_pkg = 1;
        else if (!strcasecmp(argv[i], "on")) on = 1;
        else if (!strcasecmp(argv[i], "off")) on = 0;
        else if (argv[i][0] != '-' && id < 0) {
            const char *a = argv[i];
            if (*a == 'p') { is_pkg = 1; a++; }
            id = atoll(a);
        }
    }
    if (id <= 0 || on < 0) { fprintf(stderr, "usage: dlm auto <id> on|off [-p]\n"); return 2; }
    json_t *req = req_obj("autostart");
    json_object_set_new(req, "id", json_integer(id));
    json_object_set_new(req, "on", json_boolean(on));
    if (is_pkg) json_object_set_new(req, "package", json_true());
    return send_req(req, NULL, on ? "auto-download on" : "auto-download off");
}

static int cmd_move(int argc, char **argv)
{
    int is_pkg = 0;
    int64_t id = -1;
    const char *dir = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--package")) is_pkg = 1;
        else if (!strcmp(argv[i], "up") || !strcmp(argv[i], "down") ||
                 !strcmp(argv[i], "top") || !strcmp(argv[i], "bottom")) dir = argv[i];
        else if (argv[i][0] != '-' && id < 0) {
            const char *a = argv[i];
            if (*a == 'p') { is_pkg = 1; a++; }
            id = atoll(a);
        }
    }
    if (id <= 0 || !dir) { fprintf(stderr, "usage: dlm move <id> up|down|top|bottom [-p]\n"); return 2; }
    json_t *req = req_obj("move");
    json_object_set_new(req, "id", json_integer(id));
    json_object_set_new(req, "dir", json_string(dir));
    if (is_pkg) json_object_set_new(req, "package", json_true());
    return send_req(req, NULL, "moved");
}

static int cmd_pkg(int argc, char **argv)
{
    int64_t id = -1;
    json_t *req = req_obj("pkg");
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc)
            json_object_set_new(req, "name", json_string(argv[++i]));
        else if ((!strcmp(argv[i], "--folder") || !strcmp(argv[i], "-d")) && i + 1 < argc)
            json_object_set_new(req, "folder", json_string(argv[++i]));
        else if (!strcmp(argv[i], "--comment") && i + 1 < argc)
            json_object_set_new(req, "comment", json_string(argv[++i]));
        else if (!strcmp(argv[i], "--priority") && i + 1 < argc) {
            int lvl;
            if (parse_priority(argv[++i], &lvl) == 0)
                json_object_set_new(req, "priority", json_integer(lvl));
        } else if (!strcmp(argv[i], "--collapse"))
            json_object_set_new(req, "collapsed", json_true());
        else if (!strcmp(argv[i], "--expand"))
            json_object_set_new(req, "collapsed", json_false());
        else if (argv[i][0] != '-' && id < 0) {
            const char *a = argv[i];
            if (*a == 'p') a++;
            id = atoll(a);
        }
    }
    if (id <= 0) {
        json_decref(req);
        fprintf(stderr, "usage: dlm pkg <id> [--name N] [--folder D] [--comment C] [--priority P] [--collapse|--expand]\n");
        return 2;
    }
    json_object_set_new(req, "id", json_integer(id));
    return send_req(req, NULL, "package updated");
}

static int cmd_clear(void)
{
    return send_req(req_obj("clear_finished"), "removed", "cleared finished:");
}

/* rm a link, or a whole package with -p. */
static int cmd_rm(int argc, char **argv)
{
    int is_pkg;
    int64_t id = arg_id_pkg(argc, argv, &is_pkg);
    if (id <= 0) { fprintf(stderr, "usage: dlm rm <id> [-p]\n"); return 2; }
    json_t *req = req_obj("rm");
    json_object_set_new(req, "id", json_integer(id));
    if (is_pkg) json_object_set_new(req, "package", json_true());
    return send_req(req, NULL, is_pkg ? "package removed" : "removed");
}

/* dlm start [id]: with id, force that item now; without, resume the queue. */
static int cmd_start(int argc, char **argv)
{
    int is_pkg;
    int64_t id = arg_id_pkg(argc, argv, &is_pkg);
    if (id > 0) {
        json_t *req = req_obj("force");
        json_object_set_new(req, "id", json_integer(id));
        if (is_pkg) json_object_set_new(req, "package", json_true());
        return send_req(req, NULL, "started");
    }
    json_t *req = req_obj("set");
    json_object_set_new(req, "autostart", json_true());
    return send_req(req, NULL, "downloads running (autostart on)");
}

static int cmd_stop(void)
{
    json_t *req = req_obj("set");
    json_object_set_new(req, "autostart", json_false());
    return send_req(req, NULL, "downloads stopped (active finish; no new starts)");
}

/* ===================== entry ========================================== */

static int usage(void)
{
    fprintf(stderr,
        "dlm %s\n"
        "  dlm get <url> [-o out] [-d dir] [-c n] [--limit RATE]  download now (no daemon)\n"
        "\n download list / queue:\n"
        "  dlm add <url> [-o out] [-d dir] [-c n]   enqueue directly on the daemon\n"
        "  dlm ls                          list the download queue (by package)\n"
        "  dlm watch                       live status table\n"
        "  dlm pause|resume|cancel <id>    control a download\n"
        "  dlm rm <id> [-p]                remove a link (or a whole package)\n"
        "  dlm start [<id> [-p]]           resume the queue, or force one item now\n"
        "  dlm stop                        stop after current (no new auto-starts)\n"
        "  dlm priority <id> <level> [-p]  highest..lowest / -3..3\n"
        "  dlm enable|disable <id> [-p]    include/exclude from scheduling\n"
        "  dlm auto <id> on|off [-p]       choose whether it auto-downloads\n"
        "  dlm move <id> up|down|top|bottom [-p]    reorder\n"
        "  dlm clear                       remove all finished downloads\n"
        "\n linkgrabber (staging):\n"
        "  dlm grab <url> [-d dir] [-c n]  crawl a URL into the linkgrabber\n"
        "  dlm lg                          list the linkgrabber\n"
        "  dlm confirm [<id> [-p]] [-n]    move to the download list (-n: don't start)\n"
        "  dlm lg-rm <id> [-p] | dlm lg-clear\n"
        "\n packages / settings:\n"
        "  dlm pkg <id> [--name N] [--folder D] [--priority P] [--collapse|--expand]\n"
        "  dlm set [-j <n>] [--limit RATE] max concurrent / global speed (e.g. 2M, off)\n"
        "  dlm ia-login ... | ia-logout | ia-status   archive.org account\n"
        "  dlm restart | shutdown          restart/stop the daemon (after upgrades)\n"
        "  dlm version\n"
        "\n ids: links are plain numbers; packages are shown as [pN] — pass N with -p.\n"
        "  flags in ls: x=disabled  m=manual(no auto)  F=forced  X=offline\n",
        dlm_version());
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    const char *cmd = argv[1];
    int rest = argc - 2;
    char **rv = argv + 2;

    if (!strcmp(cmd, "get")) return cmd_get(rest, rv);
    if (!strcmp(cmd, "add")) return cmd_add(rest, rv);
    if (!strcmp(cmd, "ls") || !strcmp(cmd, "list")) return cmd_ls();
    if (!strcmp(cmd, "watch")) return cmd_watch();
    if (!strcmp(cmd, "pause")) return cmd_id_op("pause", rest, rv);
    if (!strcmp(cmd, "resume")) return cmd_id_op("resume", rest, rv);
    if (!strcmp(cmd, "cancel")) return cmd_id_op("cancel", rest, rv);
    if (!strcmp(cmd, "rm")) return cmd_rm(rest, rv);
    if (!strcmp(cmd, "set")) return cmd_set(rest, rv);
    if (!strcmp(cmd, "resolve")) return cmd_resolve(rest, rv);
    /* linkgrabber */
    if (!strcmp(cmd, "grab")) return cmd_grab(rest, rv);
    if (!strcmp(cmd, "lg") || !strcmp(cmd, "linkgrabber")) return cmd_lg();
    if (!strcmp(cmd, "confirm")) return cmd_confirm(rest, rv);
    if (!strcmp(cmd, "lg-rm")) return cmd_lg_remove(rest, rv);
    if (!strcmp(cmd, "lg-clear")) return cmd_lg_clear();
    /* queue ops */
    if (!strcmp(cmd, "priority") || !strcmp(cmd, "prio")) return cmd_priority(rest, rv);
    if (!strcmp(cmd, "enable")) return cmd_flag("enable", "enabled", rest, rv);
    if (!strcmp(cmd, "disable")) return cmd_flag("disable", "disabled", rest, rv);
    if (!strcmp(cmd, "force")) return cmd_flag("force", "forced start", rest, rv);
    if (!strcmp(cmd, "auto")) return cmd_auto(rest, rv);
    if (!strcmp(cmd, "move")) return cmd_move(rest, rv);
    if (!strcmp(cmd, "pkg") || !strcmp(cmd, "package")) return cmd_pkg(rest, rv);
    if (!strcmp(cmd, "clear")) return cmd_clear();
    if (!strcmp(cmd, "start")) return cmd_start(rest, rv);
    if (!strcmp(cmd, "stop")) return cmd_stop();
    if (!strcmp(cmd, "ia-login")) return cmd_ia_login(rest, rv);
    if (!strcmp(cmd, "ia-logout")) return cmd_ia_logout();
    if (!strcmp(cmd, "ia-status")) return cmd_ia_status();
    if (!strcmp(cmd, "shutdown")) return cmd_shutdown();
    if (!strcmp(cmd, "restart")) return cmd_restart();
    if (!strcmp(cmd, "version")) { printf("%s\n", dlm_version()); return 0; }
    if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") || !strcmp(cmd, "--help")) return usage();
    fprintf(stderr, "dlm: unknown command '%s'\n", cmd);
    return usage();
}
