/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dlmd — download queue / scheduler implementation (JDownloader-style:
 * linkgrabber staging, packages, priorities, enable/disable, manual start). */
#define _POSIX_C_SOURCE 200809L
#include "queue.h"
#include "dlm/dlm.h"
#include "dlm/tools.h"
#include "compat/compat.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#  include <strings.h>
#  include <unistd.h>
#endif

typedef struct {
    int64_t id;
    char *url;
    char *out_path;
    char *name;            /* display name */
    int connections;
    int delegate;          /* download via yt-dlp instead of the engine */
    int64_t total;
    int64_t downloaded;
    double speed_bps;
    dlm_qstate state;
    char *error;

    /* grouping / scheduling */
    int64_t package_id;
    int priority;
    int enabled;
    int autostart;
    int force;
    dlm_list list;
    char *availability;
    int64_t position;

    /* runtime */
    pthread_t thread;
    int has_thread;
    volatile int cancel;   /* engine cancel flag */
    int pause_requested;   /* cancel was for a pause (resumable) */
    int remove_requested;  /* cancel was for removal */
    int finished;          /* worker returned; awaiting reap/join */
    dlm_queue *q;
} qitem;

typedef struct {
    int64_t id;
    char *name;
    char *folder;
    char *comment;
    char *source_url;      /* URL the user added (host/favicon source), or NULL */
    dlm_list list;
    int priority;
    int collapsed;
    int64_t position;
} pkg;

struct dlm_queue {
    dlm_store *store;
    int max_active;
    int64_t max_speed;     /* global bytes/sec cap; 0 = unlimited */
    int global_autostart;  /* master Start/Stop switch */
    int64_t next_pos;      /* monotonic ordering key for new links/packages */
    pthread_mutex_t mu;
    qitem **items;
    int n, cap;
    pkg **pkgs;
    int npkg, cappkg;
};

const char *dlm_qstate_str(dlm_qstate s)
{
    switch (s) {
    case Q_QUEUED: return "queued";
    case Q_ACTIVE: return "active";
    case Q_PAUSED: return "paused";
    case Q_DONE: return "done";
    case Q_ERROR: return "error";
    }
    return "unknown";
}

static dlm_qstate qstate_from_str(const char *s)
{
    if (!s) return Q_QUEUED;
    if (!strcmp(s, "active")) return Q_QUEUED; /* interrupted -> requeue */
    if (!strcmp(s, "paused")) return Q_PAUSED;
    if (!strcmp(s, "done")) return Q_DONE;
    if (!strcmp(s, "error")) return Q_ERROR;
    return Q_QUEUED;
}

const char *dlm_list_str(dlm_list l)
{
    return l == DLM_LIST_LINKGRABBER ? "linkgrabber" : "download";
}

dlm_list dlm_list_from_str(const char *s)
{
    return (s && !strcmp(s, "linkgrabber")) ? DLM_LIST_LINKGRABBER
                                            : DLM_LIST_DOWNLOAD;
}

/* ---- helpers (assume caller holds mu unless noted) -------------------- */

static char *xstrdup(const char *s) { return s ? strdup(s) : NULL; }

static int clamp_priority(int p)
{
    if (p < DLM_PRIO_LOWEST) return DLM_PRIO_LOWEST;
    if (p > DLM_PRIO_HIGHEST) return DLM_PRIO_HIGHEST;
    return p;
}

static void item_free(qitem *it)
{
    free(it->url);
    free(it->out_path);
    free(it->name);
    free(it->error);
    free(it->availability);
    free(it);
}

static void pkg_free(pkg *p)
{
    free(p->name);
    free(p->folder);
    free(p->comment);
    free(p->source_url);
    free(p);
}

static qitem *find(dlm_queue *q, int64_t id)
{
    for (int i = 0; i < q->n; i++)
        if (q->items[i]->id == id) return q->items[i];
    return NULL;
}

static pkg *find_pkg(dlm_queue *q, int64_t id)
{
    for (int i = 0; i < q->npkg; i++)
        if (q->pkgs[i]->id == id) return q->pkgs[i];
    return NULL;
}

static void push(dlm_queue *q, qitem *it)
{
    if (q->n == q->cap) {
        q->cap = q->cap ? q->cap * 2 : 8;
        q->items = realloc(q->items, (size_t)q->cap * sizeof *q->items);
    }
    q->items[q->n++] = it;
}

static void push_pkg(dlm_queue *q, pkg *p)
{
    if (q->npkg == q->cappkg) {
        q->cappkg = q->cappkg ? q->cappkg * 2 : 8;
        q->pkgs = realloc(q->pkgs, (size_t)q->cappkg * sizeof *q->pkgs);
    }
    q->pkgs[q->npkg++] = p;
}

static void erase_at(dlm_queue *q, int idx)
{
    item_free(q->items[idx]);
    memmove(&q->items[idx], &q->items[idx + 1],
            (size_t)(q->n - idx - 1) * sizeof *q->items);
    q->n--;
}

static void erase_pkg_at(dlm_queue *q, int idx)
{
    pkg_free(q->pkgs[idx]);
    memmove(&q->pkgs[idx], &q->pkgs[idx + 1],
            (size_t)(q->npkg - idx - 1) * sizeof *q->pkgs);
    q->npkg--;
}

/* Delete a package row if it has no links left in it. */
static void drop_if_empty(dlm_queue *q, int64_t package_id)
{
    if (package_id <= 0) return;
    for (int i = 0; i < q->n; i++)
        if (q->items[i]->package_id == package_id) return;
    for (int i = 0; i < q->npkg; i++)
        if (q->pkgs[i]->id == package_id) {
            dlm_store_pkg_delete(q->store, package_id);
            erase_pkg_at(q, i);
            return;
        }
}

/* Ensure the parent directory of `path` exists (the save layout now nests
 * downloads under <dir>/<host>/<title>/, and neither the engine nor yt-dlp
 * creates intermediate directories). */
static void ensure_parent_dir(const char *path)
{
    if (!path) return;
    const char *slash = strrchr(path, '/');
#if defined(_WIN32)
    const char *bs = strrchr(path, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
#endif
    if (!slash || slash == path) return;        /* no dir component / root */
    size_t len = (size_t)(slash - path);
    char *dir = malloc(len + 1);
    if (!dir) return;
    memcpy(dir, path, len);
    dir[len] = '\0';
    dlm_mkdir_p(dir);
    free(dir);
}

static void cleanup_partial_files(qitem *it)
{
    if (!it->out_path) return;
    size_t n = strlen(it->out_path);
    char *p = malloc(n + 10);
    sprintf(p, "%s.dlmpart", it->out_path);
    remove(p);
    sprintf(p, "%s.dlmjson", it->out_path);
    remove(p);
    free(p);
}

/* ---- worker ----------------------------------------------------------- */

static void progress_cb(void *ud, int64_t done, int64_t total, double bps)
{
    qitem *it = ud;
    pthread_mutex_lock(&it->q->mu);
    it->downloaded = done;
    if (total >= 0) it->total = total;
    it->speed_bps = bps;
    pthread_mutex_unlock(&it->q->mu);
}

/* Container extensions yt-dlp can merge into (so the file keeps its name). */
static const char *merge_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return NULL;
    static const char *ok[] = {"mp4", "mkv", "webm", "ogg", "flv", "mov", NULL};
    for (int i = 0; ok[i]; i++)
        if (!strcasecmp(dot + 1, ok[i])) return dot + 1;
    return NULL;
}

/* yt-dlp prints one of these per progress tick (via --progress-template); we
 * parse it to drive the item's live progress. Fields may be "NA". */
#define DLM_YTDLP_TMPL                                                          \
    "download:dlmprog|%(progress.downloaded_bytes)s|%(progress.total_bytes)s"   \
    "|%(progress.total_bytes_estimate)s|%(progress.speed)s"

static void parse_progress_line(qitem *it, char *line)
{
    if (strncmp(line, "dlmprog|", 8) != 0) return;
    char fdl[32], ftot[32], fest[32], fsp[64];
    if (sscanf(line + 8, "%31[^|]|%31[^|]|%31[^|]|%63[^\r\n]", fdl, ftot, fest,
               fsp) < 1)
        return;
    long long dl = strcmp(fdl, "NA") ? atoll(fdl) : -1;
    long long tot = strcmp(ftot, "NA") ? atoll(ftot) : -1;
    long long est = strcmp(fest, "NA") ? atoll(fest) : -1;
    double sp = strcmp(fsp, "NA") ? atof(fsp) : -1;

    pthread_mutex_lock(&it->q->mu);
    if (dl >= 0) it->downloaded = dl;
    int64_t t = tot >= 0 ? tot : est;
    if (t >= 0) it->total = t;
    if (sp >= 0) it->speed_bps = sp;
    pthread_mutex_unlock(&it->q->mu);
}

/* Download a delegated (stream/series) item via yt-dlp, parsing its progress
 * output so the queue reflects live download status. Returns DLM_OK or an error
 * code; honors the cancel flag by killing the child. */
static dlm_result run_ytdlp(qitem *it, int64_t rate)
{
    const char *argv[28];
    int n = 0;
    argv[n++] = dlm_tool_path("yt-dlp");
    argv[n++] = "--no-warnings";
    argv[n++] = "--no-playlist";
    argv[n++] = "--newline";
    argv[n++] = "--progress";
    argv[n++] = "--progress-template";
    argv[n++] = DLM_YTDLP_TMPL;
    const char *ffdir = dlm_tool_ffmpeg_dir();
    if (ffdir) { argv[n++] = "--ffmpeg-location"; argv[n++] = ffdir; }
    char ratebuf[32];
    if (rate > 0) {
        snprintf(ratebuf, sizeof ratebuf, "%lld", (long long)rate);
        argv[n++] = "--limit-rate";
        argv[n++] = ratebuf;
    }
    const char *mext = merge_ext(it->out_path);
    if (mext) { argv[n++] = "--merge-output-format"; argv[n++] = mext; }
    argv[n++] = "-o";
    argv[n++] = it->out_path;
    argv[n++] = "--";
    argv[n++] = it->url;
    argv[n] = NULL;

    dlm_proc *proc = dlm_proc_spawn(argv, DLM_SPAWN_STDOUT_ERR);
    if (!proc) return DLM_ERR_IO;

    /* read yt-dlp output line by line, parsing progress; the 200ms read timeout
     * lets us react to a cancel request promptly */
    char buf[8192];
    size_t len = 0;
    int killed = 0;
    for (;;) {
        long got = dlm_proc_read(proc, buf + len, sizeof buf - len - 1, 200);
        if (it->cancel && !killed) { dlm_proc_terminate(proc); killed = 1; }
        if (got < 0) continue; /* timeout: re-check cancel */
        if (got == 0) break;   /* EOF */
        len += (size_t)got;
        buf[len] = '\0';
        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', (size_t)(buf + len - start)))) {
            *nl = '\0';
            parse_progress_line(it, start);
            start = nl + 1;
        }
        size_t left = (size_t)(buf + len - start);
        memmove(buf, start, left);
        len = left;
        /* An overlong line with no newline would fill the buffer and make the
         * next read request 0 bytes (misread as EOF). Flush it instead. */
        if (len == sizeof buf - 1) {
            buf[len] = '\0';
            parse_progress_line(it, buf);
            len = 0;
        }
    }

    int ec = -1;
    dlm_proc_wait(proc, &ec);
    dlm_proc_free(proc);
    if (it->cancel) return DLM_ERR_CANCELLED;
    return ec == 0 ? DLM_OK : DLM_ERR_NET;
}

static void *worker_main(void *arg)
{
    qitem *it = arg;
    dlm_queue *q = it->q;

    /* split the global rate cap across the configured concurrency */
    pthread_mutex_lock(&q->mu);
    int64_t cap = q->max_speed;
    int slots = q->max_active > 0 ? q->max_active : 1;
    pthread_mutex_unlock(&q->mu);
    int64_t per = cap > 0 ? cap / slots : 0;
    if (cap > 0 && per < 1) per = 1;

    /* create the (possibly nested) destination directory before writing */
    ensure_parent_dir(it->out_path);

    dlm_result r;
    if (it->delegate) {
        r = run_ytdlp(it, per);
    } else {
        dlm_options opt;
        memset(&opt, 0, sizeof opt);
        opt.url = it->url;
        opt.out_path = it->out_path;
        opt.connections = it->connections;
        opt.max_speed = per;
        opt.on_progress = progress_cb;
        opt.userdata = it;
        opt.cancel = &it->cancel;
        r = dlm_download_file(&opt);
    }

    pthread_mutex_lock(&q->mu);
    if (r == DLM_OK) {
        it->state = Q_DONE;
        it->speed_bps = 0;
    } else if (r == DLM_ERR_CANCELLED) {
        /* pause vs remove decided by flags; remove handled in tick */
        it->state = it->remove_requested ? Q_ERROR : Q_PAUSED;
        it->speed_bps = 0;
    } else {
        it->state = Q_ERROR;
        free(it->error);
        it->error = xstrdup(dlm_strerror(r));
        it->speed_bps = 0;
    }
    it->finished = 1;
    pthread_mutex_unlock(&q->mu);
    return NULL;
}

/* ---- lifecycle -------------------------------------------------------- */

static void load_pkg(void *ud, const dlm_store_pkg_row *row)
{
    dlm_queue *q = ud;
    pkg *p = calloc(1, sizeof *p);
    p->id = row->id;
    p->name = xstrdup(row->name);
    p->folder = xstrdup(row->folder);
    p->comment = xstrdup(row->comment);
    p->source_url = xstrdup(row->source_url);
    p->list = dlm_list_from_str(row->list);
    p->priority = row->priority;
    p->collapsed = row->collapsed;
    p->position = row->position;
    if (row->position >= q->next_pos) q->next_pos = row->position + 1;
    push_pkg(q, p);
}

static void load_row(void *ud, const dlm_store_row *row)
{
    dlm_queue *q = ud;
    qitem *it = calloc(1, sizeof *it);
    it->id = row->id;
    it->url = xstrdup(row->url);
    it->out_path = xstrdup(row->out_path);
    it->name = xstrdup(row->name);
    it->connections = row->connections;
    it->delegate = row->delegate;
    it->total = row->total;
    it->downloaded = row->downloaded;
    it->state = qstate_from_str(row->state);
    it->error = xstrdup(row->error);
    it->package_id = row->package_id;
    it->priority = row->priority;
    it->enabled = row->enabled;
    it->autostart = row->autostart;
    it->force = 0; /* force never survives a restart */
    it->list = dlm_list_from_str(row->list);
    it->availability = xstrdup(row->availability);
    it->position = row->position;
    if (row->position >= q->next_pos) q->next_pos = row->position + 1;
    it->q = q;
    push(q, it);
}

dlm_queue *dlm_queue_create(dlm_store *store, int max_active)
{
    dlm_queue *q = calloc(1, sizeof *q);
    q->store = store;
    q->max_active = max_active >= 0 ? max_active : 3;
    q->global_autostart = 1;
    q->next_pos = 1;
    pthread_mutex_init(&q->mu, NULL);
    dlm_store_pkg_load_all(store, load_pkg, q);
    dlm_store_load_all(store, load_row, q);
    /* persist any 'active' rows we just demoted to 'queued', and clear stale
     * force flags */
    for (int i = 0; i < q->n; i++) {
        if (q->items[i]->state == Q_QUEUED &&
            q->items[i]->list == DLM_LIST_DOWNLOAD)
            dlm_store_set_state(store, q->items[i]->id, "queued", NULL);
        dlm_store_set_force(store, q->items[i]->id, 0);
    }
    return q;
}

void dlm_queue_destroy(dlm_queue *q)
{
    if (!q) return;
    dlm_queue_stop_all(q);
    /* join any running workers */
    for (int i = 0; i < q->n; i++) {
        if (q->items[i]->has_thread) {
            pthread_join(q->items[i]->thread, NULL);
            q->items[i]->has_thread = 0;
        }
    }
    for (int i = 0; i < q->n; i++) item_free(q->items[i]);
    for (int i = 0; i < q->npkg; i++) pkg_free(q->pkgs[i]);
    free(q->items);
    free(q->pkgs);
    pthread_mutex_destroy(&q->mu);
    free(q);
}

/* ---- add / linkgrabber ------------------------------------------------ */

/* basename of a URL path with query/fragment stripped, malloc'd. */
static char *derive_name(const char *url)
{
    const char *base = strrchr(url, '/');
    base = base ? base + 1 : url;
    size_t len = strcspn(base, "?#");
    if (len == 0) { base = "download"; len = 8; }
    char *out = malloc(len + 1);
    memcpy(out, base, len);
    out[len] = '\0';
    return out;
}

int64_t dlm_queue_add(dlm_queue *q, const char *url, const char *out_path,
                      int connections, int delegate)
{
    if (!url) return -1;
    char *derived = NULL;
    if (!out_path || !*out_path) {
        derived = derive_name(url);
        out_path = derived;
    }

    pthread_mutex_lock(&q->mu);
    dlm_store_row row;
    memset(&row, 0, sizeof row);
    row.url = url;
    row.out_path = out_path;
    row.connections = connections;
    row.delegate = delegate;
    row.total = -1;
    row.enabled = 1;
    row.autostart = 1;
    row.list = "download";
    row.availability = "unknown";
    row.position = q->next_pos++;
    row.created_at = time(NULL);
    int64_t id = dlm_store_add_full(q->store, &row);
    if (id > 0) {
        qitem *it = calloc(1, sizeof *it);
        it->id = id;
        it->url = xstrdup(url);
        it->out_path = xstrdup(out_path);
        it->connections = connections;
        it->delegate = delegate;
        it->total = -1;
        it->state = Q_QUEUED;
        it->enabled = 1;
        it->autostart = 1;
        it->list = DLM_LIST_DOWNLOAD;
        it->availability = xstrdup("unknown");
        it->position = row.position;
        it->q = q;
        push(q, it);
    }
    pthread_mutex_unlock(&q->mu);
    free(derived);
    return id;
}

int64_t dlm_queue_grab(dlm_queue *q, const char *package_name,
                       const char *folder, const char *source_url,
                       const dlm_grab_link *links, int count)
{
    if (!links || count <= 0) return -1;
    pthread_mutex_lock(&q->mu);

    /* One transaction for the package + all its links: a single commit/fsync
     * instead of one per row, which matters for large crawls. */
    dlm_store_begin(q->store);

    int64_t pos = q->next_pos++;
    int64_t pkg_id = dlm_store_pkg_add(q->store,
                                       package_name ? package_name : "links",
                                       folder, NULL, "linkgrabber",
                                       DLM_PRIO_DEFAULT, pos, time(NULL),
                                       source_url);
    if (pkg_id <= 0) {
        dlm_store_rollback(q->store);
        pthread_mutex_unlock(&q->mu);
        return -1;
    }
    pkg *p = calloc(1, sizeof *p);
    p->id = pkg_id;
    p->name = xstrdup(package_name ? package_name : "links");
    p->folder = xstrdup(folder);
    p->source_url = xstrdup(source_url);
    p->list = DLM_LIST_LINKGRABBER;
    p->priority = DLM_PRIO_DEFAULT;
    p->position = pos;
    push_pkg(q, p);

    for (int i = 0; i < count; i++) {
        const dlm_grab_link *l = &links[i];
        char *derived = NULL;
        const char *opath = l->out_path;
        if (!opath || !*opath) { derived = derive_name(l->url); opath = derived; }

        dlm_store_row row;
        memset(&row, 0, sizeof row);
        row.url = l->url;
        row.out_path = opath;
        row.connections = l->connections;
        row.delegate = l->delegate;
        row.total = l->size;
        row.enabled = 1;
        row.autostart = 1;
        row.list = "linkgrabber";
        row.name = l->name;
        row.availability = l->availability ? l->availability : "unknown";
        row.package_id = pkg_id;
        row.position = q->next_pos++;
        row.created_at = time(NULL);
        int64_t id = dlm_store_add_full(q->store, &row);
        if (id > 0) {
            qitem *it = calloc(1, sizeof *it);
            it->id = id;
            it->url = xstrdup(l->url);
            it->out_path = xstrdup(opath);
            it->name = xstrdup(l->name);
            it->connections = l->connections;
            it->delegate = l->delegate;
            it->total = l->size ? l->size : -1;
            it->state = Q_QUEUED;
            it->enabled = 1;
            it->autostart = 1;
            it->list = DLM_LIST_LINKGRABBER;
            it->availability = xstrdup(row.availability);
            it->package_id = pkg_id;
            it->position = row.position;
            it->q = q;
            push(q, it);
        }
        free(derived);
    }
    dlm_store_commit(q->store);
    pthread_mutex_unlock(&q->mu);
    return pkg_id;
}

/* Move one linkgrabber link into the download list. Caller holds mu. */
static void confirm_link(dlm_queue *q, qitem *it, int start)
{
    if (it->list != DLM_LIST_LINKGRABBER) return;
    it->list = DLM_LIST_DOWNLOAD;
    it->state = Q_QUEUED;
    it->autostart = start ? 1 : 0;
    dlm_store_set_list(q->store, it->id, "download");
    dlm_store_set_state(q->store, it->id, "queued", NULL);
    dlm_store_set_autostart(q->store, it->id, it->autostart);
}

int dlm_queue_confirm(dlm_queue *q, int64_t id, int is_package, int start)
{
    pthread_mutex_lock(&q->mu);
    int moved = 0;
    if (id <= 0) {
        /* confirm the whole linkgrabber */
        for (int i = 0; i < q->npkg; i++)
            if (q->pkgs[i]->list == DLM_LIST_LINKGRABBER) {
                q->pkgs[i]->list = DLM_LIST_DOWNLOAD;
                dlm_store_pkg_set_list(q->store, q->pkgs[i]->id, "download");
            }
        for (int i = 0; i < q->n; i++)
            if (q->items[i]->list == DLM_LIST_LINKGRABBER) {
                confirm_link(q, q->items[i], start);
                moved++;
            }
    } else if (is_package) {
        pkg *p = find_pkg(q, id);
        if (!p || p->list != DLM_LIST_LINKGRABBER) { pthread_mutex_unlock(&q->mu); return -1; }
        p->list = DLM_LIST_DOWNLOAD;
        dlm_store_pkg_set_list(q->store, p->id, "download");
        for (int i = 0; i < q->n; i++)
            if (q->items[i]->package_id == id &&
                q->items[i]->list == DLM_LIST_LINKGRABBER) {
                confirm_link(q, q->items[i], start);
                moved++;
            }
    } else {
        qitem *it = find(q, id);
        if (!it || it->list != DLM_LIST_LINKGRABBER) { pthread_mutex_unlock(&q->mu); return -1; }
        int64_t was_pkg = it->package_id;
        confirm_link(q, it, start);
        moved = 1;
        /* if its package now has no linkgrabber links left, move the (now
         * empty-in-linkgrabber) package to the download list too */
        if (was_pkg > 0) {
            int remaining = 0;
            for (int i = 0; i < q->n; i++)
                if (q->items[i]->package_id == was_pkg &&
                    q->items[i]->list == DLM_LIST_LINKGRABBER)
                    remaining++;
            if (!remaining) {
                pkg *p = find_pkg(q, was_pkg);
                if (p && p->list == DLM_LIST_LINKGRABBER) {
                    p->list = DLM_LIST_DOWNLOAD;
                    dlm_store_pkg_set_list(q->store, p->id, "download");
                }
            }
        }
    }
    pthread_mutex_unlock(&q->mu);
    return moved;
}

/* Remove a single linkgrabber link (no running worker possible). Caller mu. */
static void lg_erase_link(dlm_queue *q, int64_t id)
{
    for (int i = 0; i < q->n; i++)
        if (q->items[i]->id == id) {
            dlm_store_delete(q->store, id);
            erase_at(q, i);
            return;
        }
}

int dlm_queue_lg_remove(dlm_queue *q, int64_t id, int is_package)
{
    pthread_mutex_lock(&q->mu);
    int removed = 0;
    if (id <= 0) {
        for (int i = q->n - 1; i >= 0; i--)
            if (q->items[i]->list == DLM_LIST_LINKGRABBER) {
                dlm_store_delete(q->store, q->items[i]->id);
                erase_at(q, i);
                removed++;
            }
        for (int i = q->npkg - 1; i >= 0; i--)
            if (q->pkgs[i]->list == DLM_LIST_LINKGRABBER) {
                dlm_store_pkg_delete(q->store, q->pkgs[i]->id);
                erase_pkg_at(q, i);
            }
    } else if (is_package) {
        pkg *p = find_pkg(q, id);
        if (!p || p->list != DLM_LIST_LINKGRABBER) { pthread_mutex_unlock(&q->mu); return -1; }
        for (int i = q->n - 1; i >= 0; i--)
            if (q->items[i]->package_id == id &&
                q->items[i]->list == DLM_LIST_LINKGRABBER) {
                dlm_store_delete(q->store, q->items[i]->id);
                erase_at(q, i);
                removed++;
            }
        for (int i = 0; i < q->npkg; i++)
            if (q->pkgs[i]->id == id) { dlm_store_pkg_delete(q->store, id); erase_pkg_at(q, i); break; }
    } else {
        qitem *it = find(q, id);
        if (!it || it->list != DLM_LIST_LINKGRABBER) { pthread_mutex_unlock(&q->mu); return -1; }
        int64_t pkg_id = it->package_id;
        lg_erase_link(q, id);
        removed = 1;
        drop_if_empty(q, pkg_id);
    }
    pthread_mutex_unlock(&q->mu);
    return removed;
}

/* ---- per-link operations ---------------------------------------------- */

int dlm_queue_pause(dlm_queue *q, int64_t id)
{
    pthread_mutex_lock(&q->mu);
    qitem *it = find(q, id);
    int rc = -1;
    if (it) {
        rc = 0;
        it->force = 0;
        if (it->state == Q_ACTIVE) {
            it->pause_requested = 1;
            it->cancel = 1; /* worker will exit to paused */
        } else if (it->state == Q_QUEUED) {
            it->state = Q_PAUSED;
            dlm_store_set_state(q->store, id, "paused", NULL);
        }
    }
    pthread_mutex_unlock(&q->mu);
    return rc;
}

int dlm_queue_resume(dlm_queue *q, int64_t id)
{
    pthread_mutex_lock(&q->mu);
    qitem *it = find(q, id);
    int rc = -1;
    if (it && (it->state == Q_PAUSED || it->state == Q_ERROR)) {
        it->state = Q_QUEUED;
        it->cancel = 0;
        it->pause_requested = 0;
        free(it->error);
        it->error = NULL;
        dlm_store_set_state(q->store, id, "queued", NULL);
        rc = 0;
    }
    pthread_mutex_unlock(&q->mu);
    return rc;
}

int dlm_queue_remove(dlm_queue *q, int64_t id)
{
    pthread_mutex_lock(&q->mu);
    qitem *it = find(q, id);
    int rc = -1;
    if (it) {
        rc = 0;
        int64_t pkg_id = it->package_id;
        if (it->has_thread) {
            /* A worker is still attached (running, or finished but not yet
             * reaped). Defer to the tick, which joins before erasing. */
            it->remove_requested = 1;
            it->cancel = 1;
        } else {
            cleanup_partial_files(it);
            dlm_store_delete(q->store, id);
            for (int i = 0; i < q->n; i++)
                if (q->items[i] == it) { erase_at(q, i); break; }
            drop_if_empty(q, pkg_id);
        }
    }
    pthread_mutex_unlock(&q->mu);
    return rc;
}

int dlm_queue_pkg_remove(dlm_queue *q, int64_t package_id)
{
    pthread_mutex_lock(&q->mu);
    pkg *p = find_pkg(q, package_id);
    if (!p) { pthread_mutex_unlock(&q->mu); return -1; }
    /* remove links; defer any with a live worker to the tick */
    for (int i = q->n - 1; i >= 0; i--) {
        qitem *it = q->items[i];
        if (it->package_id != package_id) continue;
        if (it->has_thread) {
            it->remove_requested = 1;
            it->cancel = 1;
        } else {
            cleanup_partial_files(it);
            dlm_store_delete(q->store, it->id);
            erase_at(q, i);
        }
    }
    /* delete the package row now only if nothing is left pending a reap */
    drop_if_empty(q, package_id);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

/* Apply a per-link mutation to one link or every link in a package. */
typedef void (*link_apply_fn)(dlm_queue *q, qitem *it, int arg);

static int apply_to(dlm_queue *q, int64_t id, int is_package, int arg,
                    link_apply_fn fn)
{
    pthread_mutex_lock(&q->mu);
    int rc = -1;
    if (is_package) {
        if (find_pkg(q, id)) {
            for (int i = 0; i < q->n; i++)
                if (q->items[i]->package_id == id) { fn(q, q->items[i], arg); rc = 0; }
            if (rc != 0) rc = 0; /* an empty package still "succeeds" */
        }
    } else {
        qitem *it = find(q, id);
        if (it) { fn(q, it, arg); rc = 0; }
    }
    pthread_mutex_unlock(&q->mu);
    return rc;
}

static void apply_priority(dlm_queue *q, qitem *it, int prio)
{
    it->priority = clamp_priority(prio);
    dlm_store_set_priority(q->store, it->id, it->priority);
}

static void apply_enabled(dlm_queue *q, qitem *it, int en)
{
    it->enabled = en ? 1 : 0;
    dlm_store_set_enabled(q->store, it->id, it->enabled);
}

static void apply_autostart(dlm_queue *q, qitem *it, int on)
{
    it->autostart = on ? 1 : 0;
    dlm_store_set_autostart(q->store, it->id, it->autostart);
}

static void apply_force(dlm_queue *q, qitem *it, int unused)
{
    (void)unused;
    if (it->list != DLM_LIST_DOWNLOAD) return;
    it->enabled = 1;
    it->force = 1;
    dlm_store_set_enabled(q->store, it->id, 1);
    dlm_store_set_force(q->store, it->id, 1);
    /* only re-queue (and persist that) a stopped item; forcing an already
     * active one must not write a bogus "queued" state over the live worker. */
    if (it->state == Q_PAUSED || it->state == Q_ERROR) {
        it->state = Q_QUEUED;
        it->cancel = 0;
        free(it->error);
        it->error = NULL;
        dlm_store_set_state(q->store, it->id, "queued", NULL);
    }
}

int dlm_queue_set_priority(dlm_queue *q, int64_t id, int is_package, int prio)
{
    /* a package also remembers its own priority for display/new links */
    if (is_package) {
        pthread_mutex_lock(&q->mu);
        pkg *p = find_pkg(q, id);
        if (p) { p->priority = clamp_priority(prio);
                 dlm_store_pkg_update(q->store, id, NULL, NULL, NULL, p->priority,
                                      p->collapsed); }
        pthread_mutex_unlock(&q->mu);
    }
    return apply_to(q, id, is_package, prio, apply_priority);
}

int dlm_queue_set_enabled(dlm_queue *q, int64_t id, int is_package, int enabled)
{
    return apply_to(q, id, is_package, enabled, apply_enabled);
}

int dlm_queue_set_autostart(dlm_queue *q, int64_t id, int is_package, int on)
{
    return apply_to(q, id, is_package, on, apply_autostart);
}

int dlm_queue_force(dlm_queue *q, int64_t id, int is_package)
{
    return apply_to(q, id, is_package, 0, apply_force);
}

/* ---- reordering ------------------------------------------------------- */

/* Swap the ordering positions of two links/packages and persist. */
static void swap_link_pos(dlm_queue *q, qitem *a, qitem *b)
{
    int64_t t = a->position; a->position = b->position; b->position = t;
    dlm_store_set_position(q->store, a->id, a->position);
    dlm_store_set_position(q->store, b->id, b->position);
}

int dlm_queue_move(dlm_queue *q, int64_t id, int is_package, dlm_move dir)
{
    pthread_mutex_lock(&q->mu);
    int rc = -1;
    if (!is_package) {
        qitem *it = find(q, id);
        if (it) {
            /* scope: same list + same package */
            qitem *neighbor = NULL; /* for up/down */
            int64_t best = 0;       /* for top/bottom */
            int have = 0;
            for (int i = 0; i < q->n; i++) {
                qitem *o = q->items[i];
                if (o == it || o->list != it->list ||
                    o->package_id != it->package_id)
                    continue;
                if (dir == DLM_MOVE_UP && o->position < it->position) {
                    if (!neighbor || o->position > neighbor->position) neighbor = o;
                } else if (dir == DLM_MOVE_DOWN && o->position > it->position) {
                    if (!neighbor || o->position < neighbor->position) neighbor = o;
                } else if (dir == DLM_MOVE_TOP || dir == DLM_MOVE_BOTTOM) {
                    if (!have) { best = o->position; have = 1; }
                    else if (dir == DLM_MOVE_TOP && o->position < best) best = o->position;
                    else if (dir == DLM_MOVE_BOTTOM && o->position > best) best = o->position;
                }
            }
            rc = 0;
            if ((dir == DLM_MOVE_UP || dir == DLM_MOVE_DOWN) && neighbor) {
                swap_link_pos(q, it, neighbor);
            } else if (dir == DLM_MOVE_TOP && have) {
                it->position = best - 1;
                dlm_store_set_position(q->store, it->id, it->position);
            } else if (dir == DLM_MOVE_BOTTOM && have) {
                it->position = best + 1;
                dlm_store_set_position(q->store, it->id, it->position);
            }
        }
    } else {
        pkg *p = find_pkg(q, id);
        if (p) {
            pkg *neighbor = NULL;
            int64_t best = 0; int have = 0;
            for (int i = 0; i < q->npkg; i++) {
                pkg *o = q->pkgs[i];
                if (o == p || o->list != p->list) continue;
                if (dir == DLM_MOVE_UP && o->position < p->position) {
                    if (!neighbor || o->position > neighbor->position) neighbor = o;
                } else if (dir == DLM_MOVE_DOWN && o->position > p->position) {
                    if (!neighbor || o->position < neighbor->position) neighbor = o;
                } else if (dir == DLM_MOVE_TOP || dir == DLM_MOVE_BOTTOM) {
                    if (!have) { best = o->position; have = 1; }
                    else if (dir == DLM_MOVE_TOP && o->position < best) best = o->position;
                    else if (dir == DLM_MOVE_BOTTOM && o->position > best) best = o->position;
                }
            }
            rc = 0;
            if ((dir == DLM_MOVE_UP || dir == DLM_MOVE_DOWN) && neighbor) {
                int64_t t = p->position; p->position = neighbor->position;
                neighbor->position = t;
                dlm_store_pkg_set_position(q->store, p->id, p->position);
                dlm_store_pkg_set_position(q->store, neighbor->id, neighbor->position);
            } else if (dir == DLM_MOVE_TOP && have) {
                p->position = best - 1;
                dlm_store_pkg_set_position(q->store, p->id, p->position);
            } else if (dir == DLM_MOVE_BOTTOM && have) {
                p->position = best + 1;
                dlm_store_pkg_set_position(q->store, p->id, p->position);
            }
        }
    }
    pthread_mutex_unlock(&q->mu);
    return rc;
}

/* ---- packages --------------------------------------------------------- */

int dlm_queue_pkg_update(dlm_queue *q, int64_t id, const char *name,
                         const char *folder, const char *comment, int priority,
                         int collapsed)
{
    pthread_mutex_lock(&q->mu);
    pkg *p = find_pkg(q, id);
    int rc = -1;
    if (p) {
        if (name) { free(p->name); p->name = xstrdup(name); }
        if (folder) { free(p->folder); p->folder = xstrdup(folder); }
        if (comment) { free(p->comment); p->comment = xstrdup(comment); }
        if (priority >= DLM_PRIO_LOWEST) p->priority = clamp_priority(priority);
        if (collapsed >= 0) p->collapsed = collapsed ? 1 : 0;
        dlm_store_pkg_update(q->store, id, name, folder, comment, p->priority,
                             p->collapsed);
        rc = 0;
    }
    pthread_mutex_unlock(&q->mu);
    return rc;
}

/* ---- global settings -------------------------------------------------- */

void dlm_queue_set_max_active(dlm_queue *q, int max_active)
{
    pthread_mutex_lock(&q->mu);
    if (max_active >= 0) q->max_active = max_active;
    pthread_mutex_unlock(&q->mu);
}

int dlm_queue_get_max_active(dlm_queue *q)
{
    pthread_mutex_lock(&q->mu);
    int m = q->max_active;
    pthread_mutex_unlock(&q->mu);
    return m;
}

void dlm_queue_set_max_speed(dlm_queue *q, int64_t bytes_per_sec)
{
    pthread_mutex_lock(&q->mu);
    q->max_speed = bytes_per_sec > 0 ? bytes_per_sec : 0;
    pthread_mutex_unlock(&q->mu);
}

int64_t dlm_queue_get_max_speed(dlm_queue *q)
{
    pthread_mutex_lock(&q->mu);
    int64_t s = q->max_speed;
    pthread_mutex_unlock(&q->mu);
    return s;
}

void dlm_queue_set_global_autostart(dlm_queue *q, int on)
{
    pthread_mutex_lock(&q->mu);
    q->global_autostart = on ? 1 : 0;
    pthread_mutex_unlock(&q->mu);
}

int dlm_queue_get_global_autostart(dlm_queue *q)
{
    pthread_mutex_lock(&q->mu);
    int v = q->global_autostart;
    pthread_mutex_unlock(&q->mu);
    return v;
}

int dlm_queue_clear_finished(dlm_queue *q)
{
    pthread_mutex_lock(&q->mu);
    int removed = 0;
    for (int i = q->n - 1; i >= 0; i--) {
        qitem *it = q->items[i];
        if (it->state == Q_DONE && !it->has_thread) {
            int64_t pkg_id = it->package_id;
            dlm_store_delete(q->store, it->id);
            erase_at(q, i);
            drop_if_empty(q, pkg_id);
            removed++;
        }
    }
    pthread_mutex_unlock(&q->mu);
    return removed;
}

/* ---- scheduler tick --------------------------------------------------- */

/* Start a queued download-list link in a worker thread. Caller holds mu. */
static int start_item(dlm_queue *q, qitem *it)
{
    it->state = Q_ACTIVE;
    it->cancel = 0;
    it->pause_requested = 0;
    dlm_store_set_state(q->store, it->id, "active", NULL);
    if (pthread_create(&it->thread, NULL, worker_main, it) == 0) {
        it->has_thread = 1;
        return 1;
    }
    it->state = Q_ERROR;
    free(it->error);
    it->error = xstrdup("failed to start worker thread");
    dlm_store_set_state(q->store, it->id, "error", it->error);
    return 0;
}

/* True if a link may be auto-started by the scheduler right now. Links crawled
 * as "offline" are skipped so they don't churn through Q_ERROR; an explicit
 * force still starts them (the force loop doesn't consult this). */
static int eligible(qitem *it)
{
    return it->list == DLM_LIST_DOWNLOAD && it->state == Q_QUEUED &&
           !it->has_thread && it->enabled && it->autostart &&
           (!it->availability || strcmp(it->availability, "offline") != 0);
}

void dlm_queue_tick(dlm_queue *q)
{
    pthread_mutex_lock(&q->mu);

    /* reap finished workers */
    for (int i = 0; i < q->n;) {
        qitem *it = q->items[i];
        if (it->has_thread && it->finished) {
            pthread_join(it->thread, NULL);
            it->has_thread = 0;
            it->finished = 0;
            /* A finished worker leaves the item in DONE/ERROR/PAUSED. If it is
             * Q_QUEUED here, a force/resume command landed between the worker
             * finishing and this reap — keep its force flag so it still starts. */
            if (it->state != Q_QUEUED) {
                it->force = 0;
                dlm_store_set_force(q->store, it->id, 0);
            }
            dlm_store_set_progress(q->store, it->id, it->total, it->downloaded);
            if (it->remove_requested) {
                int64_t pkg_id = it->package_id;
                cleanup_partial_files(it);
                dlm_store_delete(q->store, it->id);
                erase_at(q, i);
                drop_if_empty(q, pkg_id);
                continue;
            }
            dlm_store_set_state(q->store, it->id, dlm_qstate_str(it->state),
                                it->error);
        }
        i++;
    }

    /* count active */
    int active = 0;
    for (int i = 0; i < q->n; i++)
        if (q->items[i]->has_thread) active++;

    /* forced links start unconditionally (ignore max_active / autostart) */
    for (int i = 0; i < q->n; i++) {
        qitem *it = q->items[i];
        if (it->force && it->list == DLM_LIST_DOWNLOAD &&
            it->state == Q_QUEUED && !it->has_thread && it->enabled) {
            if (start_item(q, it)) active++;
        }
    }

    /* start eligible links by priority (desc), then position (asc), up to
     * max_active — unless the global autostart switch is off */
    if (q->global_autostart) {
        while (active < q->max_active) {
            qitem *best = NULL;
            for (int i = 0; i < q->n; i++) {
                qitem *it = q->items[i];
                if (!eligible(it)) continue;
                if (!best || it->priority > best->priority ||
                    (it->priority == best->priority && it->position < best->position))
                    best = it;
            }
            if (!best) break;
            if (start_item(q, best)) active++;
            else break; /* thread spawn failed; avoid spinning */
        }
    }

    /* periodically persist progress of active items */
    for (int i = 0; i < q->n; i++)
        if (q->items[i]->has_thread)
            dlm_store_set_progress(q->store, q->items[i]->id,
                                   q->items[i]->total, q->items[i]->downloaded);

    pthread_mutex_unlock(&q->mu);
}

int dlm_queue_has_pending_work(dlm_queue *q)
{
    pthread_mutex_lock(&q->mu);
    int pending = 0;

    /* a running worker means progress to persist and an eventual reap */
    int active = 0;
    for (int i = 0; i < q->n; i++)
        if (q->items[i]->has_thread) { active++; pending = 1; }

    /* a forced link starts unconditionally on the next tick */
    for (int i = 0; i < q->n && !pending; i++) {
        qitem *it = q->items[i];
        if (it->force && it->list == DLM_LIST_DOWNLOAD &&
            it->state == Q_QUEUED && !it->has_thread && it->enabled)
            pending = 1;
    }

    /* an eligible link can be auto-started if a slot is free */
    if (!pending && q->global_autostart && active < q->max_active)
        for (int i = 0; i < q->n && !pending; i++)
            if (eligible(q->items[i])) pending = 1;

    pthread_mutex_unlock(&q->mu);
    return pending;
}

/* ---- snapshot / introspection ----------------------------------------- */

/* qsort comparators ordering by position then id (stable display order). */
static int cmp_item_pos(const void *a, const void *b)
{
    const qitem *x = *(qitem *const *)a, *y = *(qitem *const *)b;
    if (x->position != y->position) return x->position < y->position ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id ? 1 : 0;
}

static int cmp_pkg_pos(const void *a, const void *b)
{
    const pkg *x = *(pkg *const *)a, *y = *(pkg *const *)b;
    if (x->position != y->position) return x->position < y->position ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id ? 1 : 0;
}

/* (package id -> output slot) pair, sorted by id so per-package link counts can
 * be tallied in one O(n log p) pass instead of an O(n*p) nested scan. */
typedef struct { int64_t id; int slot; } pkg_idslot;
static int cmp_idslot(const void *a, const void *b)
{
    const pkg_idslot *x = a, *y = b;
    return x->id < y->id ? -1 : x->id > y->id ? 1 : 0;
}

int dlm_queue_snapshot(dlm_queue *q, dlm_qsnap **out, int *count)
{
    pthread_mutex_lock(&q->mu);
    dlm_qsnap *arr = calloc((size_t)(q->n ? q->n : 1), sizeof *arr);
    qitem **ord = malloc((size_t)(q->n ? q->n : 1) * sizeof *ord);
    for (int i = 0; i < q->n; i++) ord[i] = q->items[i];
    qsort(ord, (size_t)q->n, sizeof *ord, cmp_item_pos);
    for (int i = 0; i < q->n; i++) {
        qitem *it = ord[i];
        arr[i].id = it->id;
        arr[i].url = xstrdup(it->url);
        arr[i].out_path = xstrdup(it->out_path);
        arr[i].name = xstrdup(it->name);
        arr[i].connections = it->connections;
        arr[i].total = it->total;
        arr[i].downloaded = it->downloaded;
        arr[i].speed_bps = it->speed_bps;
        arr[i].state = it->state;
        arr[i].error = xstrdup(it->error);
        arr[i].package_id = it->package_id;
        arr[i].priority = it->priority;
        arr[i].enabled = it->enabled;
        arr[i].autostart = it->autostart;
        arr[i].force = it->force;
        arr[i].list = it->list;
        arr[i].availability = xstrdup(it->availability);
        arr[i].delegate = it->delegate;
    }
    free(ord);
    *count = q->n;
    *out = arr;
    pthread_mutex_unlock(&q->mu);
    return 0;
}

void dlm_queue_snapshot_free(dlm_qsnap *snap, int count)
{
    if (!snap) return;
    for (int i = 0; i < count; i++) {
        free(snap[i].url);
        free(snap[i].out_path);
        free(snap[i].name);
        free(snap[i].error);
        free(snap[i].availability);
    }
    free(snap);
}

int dlm_queue_packages(dlm_queue *q, dlm_psnap **out, int *count)
{
    pthread_mutex_lock(&q->mu);
    dlm_psnap *arr = calloc((size_t)(q->npkg ? q->npkg : 1), sizeof *arr);
    pkg **ord = malloc((size_t)(q->npkg ? q->npkg : 1) * sizeof *ord);
    for (int i = 0; i < q->npkg; i++) ord[i] = q->pkgs[i];
    qsort(ord, (size_t)q->npkg, sizeof *ord, cmp_pkg_pos);
    pkg_idslot *map = malloc((size_t)(q->npkg ? q->npkg : 1) * sizeof *map);
    for (int i = 0; i < q->npkg; i++) {
        pkg *p = ord[i];
        arr[i].id = p->id;
        arr[i].name = xstrdup(p->name);
        arr[i].folder = xstrdup(p->folder);
        arr[i].comment = xstrdup(p->comment);
        arr[i].source_url = xstrdup(p->source_url);
        arr[i].list = p->list;
        arr[i].priority = p->priority;
        arr[i].collapsed = p->collapsed;
        arr[i].link_count = 0;
        map[i].id = p->id;
        map[i].slot = i;
    }
    /* Tally links per package in one pass: sort the id->slot map, then binary
     * search each item's package_id (O(n log p) vs the old O(n*p)). */
    qsort(map, (size_t)q->npkg, sizeof *map, cmp_idslot);
    for (int j = 0; j < q->n; j++) {
        int64_t pid = q->items[j]->package_id;
        int lo = 0, hi = q->npkg - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (map[mid].id < pid) lo = mid + 1;
            else if (map[mid].id > pid) hi = mid - 1;
            else { arr[map[mid].slot].link_count++; break; }
        }
    }
    free(map);
    free(ord);
    *count = q->npkg;
    *out = arr;
    pthread_mutex_unlock(&q->mu);
    return 0;
}

void dlm_queue_packages_free(dlm_psnap *snap, int count)
{
    if (!snap) return;
    for (int i = 0; i < count; i++) {
        free(snap[i].name);
        free(snap[i].folder);
        free(snap[i].comment);
        free(snap[i].source_url);
    }
    free(snap);
}

int dlm_queue_active_count(dlm_queue *q)
{
    pthread_mutex_lock(&q->mu);
    int c = 0;
    for (int i = 0; i < q->n; i++)
        if (q->items[i]->has_thread) c++;
    pthread_mutex_unlock(&q->mu);
    return c;
}

void dlm_queue_stop_all(dlm_queue *q)
{
    pthread_mutex_lock(&q->mu);
    for (int i = 0; i < q->n; i++) {
        if (q->items[i]->has_thread) {
            q->items[i]->pause_requested = 1;
            q->items[i]->cancel = 1;
        }
    }
    pthread_mutex_unlock(&q->mu);
}
