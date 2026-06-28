/* dlmd — download queue / scheduler, modelled on JDownloader.
 *
 * Owns two lists of "links" (downloads): the **linkgrabber**, a staging area
 * where crawled links are reviewed, and the **download list**, which the
 * scheduler runs. Links are grouped into **packages** (a named set sharing a
 * download folder). The scheduler runs up to max_active download-list links
 * concurrently — highest priority first, skipping disabled links/packages, with
 * a force flag and a "stop after current" switch — one worker thread per active
 * link driving the libdlm engine. All state is mutex-protected; the daemon's
 * main thread calls dlm_queue_tick() periodically to reap finished workers and
 * start eligible ones, and dlm_queue_snapshot()/_packages() to report status.
 */
#ifndef DLM_QUEUE_H
#define DLM_QUEUE_H

#include "dlm/store.h"
#include <stdint.h>

typedef enum {
    Q_QUEUED = 0,
    Q_ACTIVE,
    Q_PAUSED,
    Q_DONE,
    Q_ERROR
} dlm_qstate;

const char *dlm_qstate_str(dlm_qstate s);

/* JDownloader-style priorities; higher values are scheduled first. */
typedef enum {
    DLM_PRIO_LOWEST = -3,
    DLM_PRIO_LOWER = -2,
    DLM_PRIO_LOW = -1,
    DLM_PRIO_DEFAULT = 0,
    DLM_PRIO_HIGH = 1,
    DLM_PRIO_HIGHER = 2,
    DLM_PRIO_HIGHEST = 3
} dlm_priority;

/* Which list a link/package lives in. */
typedef enum {
    DLM_LIST_DOWNLOAD = 0,
    DLM_LIST_LINKGRABBER = 1
} dlm_list;

const char *dlm_list_str(dlm_list l);
dlm_list dlm_list_from_str(const char *s);

typedef struct dlm_queue dlm_queue;

/* Immutable snapshot of one link, returned by dlm_queue_snapshot(). */
typedef struct {
    int64_t id;
    char *url;
    char *out_path;
    char *name;        /* display name (basename of out_path if unset) */
    int connections;
    int64_t total;
    int64_t downloaded;
    double speed_bps;
    dlm_qstate state;
    char *error;       /* may be NULL */
    int64_t package_id;
    int priority;
    int enabled;
    int autostart;     /* 0 => never started automatically (manual-only) */
    int force;
    dlm_list list;
    char *availability; /* "online" | "offline" | "unknown" */
    int delegate;
} dlm_qsnap;

/* Immutable snapshot of one package. */
typedef struct {
    int64_t id;
    char *name;
    char *folder;
    char *comment;
    dlm_list list;
    int priority;
    int collapsed;
    int link_count;     /* links currently in this package */
} dlm_psnap;

/* One link to add via dlm_queue_grab(): the result of crawling a URL. */
typedef struct {
    const char *url;
    const char *out_path;     /* full output path */
    const char *name;         /* display name (may be NULL) */
    int64_t size;             /* known size, or -1 */
    int connections;
    int delegate;
    const char *availability; /* "online"|"offline"|"unknown"; NULL => online */
} dlm_grab_link;

dlm_queue *dlm_queue_create(dlm_store *store, int max_active);
void dlm_queue_destroy(dlm_queue *q);

/* Add a new download-list link (state queued). Returns id (>0) or -1. out_path
 * may be NULL to derive a filename from the URL. delegate => yt-dlp worker. */
int64_t dlm_queue_add(dlm_queue *q, const char *url, const char *out_path,
                      int connections, int delegate);

/* ---- linkgrabber ------------------------------------------------------ */

/* Stage a crawled set of links as a new package in the linkgrabber. Returns the
 * new package id (>0) or -1. Links are added disabled-for-scheduling by virtue
 * of living in the linkgrabber list; nothing runs until confirmed. */
int64_t dlm_queue_grab(dlm_queue *q, const char *package_name,
                       const char *folder, const dlm_grab_link *links,
                       int count);

/* Move linkgrabber content into the download list (queued). is_package selects
 * whether id is a package (all its links move) or a single link. id<=0 confirms
 * the whole linkgrabber. `start` chooses whether the confirmed links auto-start
 * (1) or land in the download list as manual-only (0, the user starts them
 * later). Returns the number of links moved, or -1 on error. */
int dlm_queue_confirm(dlm_queue *q, int64_t id, int is_package, int start);

/* Remove linkgrabber content (a link, or a package and its links). id<=0 clears
 * the whole linkgrabber. Only affects the linkgrabber list. Returns links
 * removed, or -1. */
int dlm_queue_lg_remove(dlm_queue *q, int64_t id, int is_package);

/* ---- per-link queue operations ---------------------------------------- */

int dlm_queue_pause(dlm_queue *q, int64_t id);
int dlm_queue_resume(dlm_queue *q, int64_t id);
/* Stop and permanently remove the link, deleting partial files. */
int dlm_queue_remove(dlm_queue *q, int64_t id);
/* Remove a package and all of its links (from whichever list it is in). */
int dlm_queue_pkg_remove(dlm_queue *q, int64_t package_id);

/* The following take is_package: when nonzero, id names a package and the
 * operation cascades to every link in it; otherwise id names a single link. */

/* Set priority (-3..3). */
int dlm_queue_set_priority(dlm_queue *q, int64_t id, int is_package, int priority);
/* Enable/disable (disabled links are skipped by the scheduler). */
int dlm_queue_set_enabled(dlm_queue *q, int64_t id, int is_package, int enabled);
/* Choose whether links auto-download. When off, a link stays queued but the
 * scheduler never starts it on its own; the user starts it manually (force). */
int dlm_queue_set_autostart(dlm_queue *q, int64_t id, int is_package, int on);
/* Force to start now, ignoring max_active / priority / the autostart switches. */
int dlm_queue_force(dlm_queue *q, int64_t id, int is_package);

typedef enum { DLM_MOVE_UP, DLM_MOVE_DOWN, DLM_MOVE_TOP, DLM_MOVE_BOTTOM } dlm_move;
/* Reorder a link within its package, or a package within its list. */
int dlm_queue_move(dlm_queue *q, int64_t id, int is_package, dlm_move dir);

/* ---- packages --------------------------------------------------------- */

/* Update a package's metadata (NULL leaves a string field unchanged;
 * priority<-3 leaves priority; collapsed<0 leaves collapsed). */
int dlm_queue_pkg_update(dlm_queue *q, int64_t id, const char *name,
                         const char *folder, const char *comment, int priority,
                         int collapsed);

/* ---- global settings -------------------------------------------------- */

void dlm_queue_set_max_active(dlm_queue *q, int max_active);
int dlm_queue_get_max_active(dlm_queue *q);

/* Global download rate cap in bytes/sec (0 = unlimited), shared across the
 * active downloads. Takes effect for downloads started after the change. */
void dlm_queue_set_max_speed(dlm_queue *q, int64_t bytes_per_sec);
int64_t dlm_queue_get_max_speed(dlm_queue *q);

/* Global autostart switch (the master Start/Stop). When off, the scheduler
 * starts no new links automatically — active ones finish ("stop after current")
 * and the user starts the rest manually. Default on. */
void dlm_queue_set_global_autostart(dlm_queue *q, int on);
int dlm_queue_get_global_autostart(dlm_queue *q);

/* Remove every finished (done) link from the queue, plus any package thereby
 * left empty. Returns the number of links removed. */
int dlm_queue_clear_finished(dlm_queue *q);

/* Reap finished workers and start eligible items. Call regularly (e.g. 200ms). */
void dlm_queue_tick(dlm_queue *q);

/* Allocate a snapshot array of all links. Free with dlm_queue_snapshot_free. */
int dlm_queue_snapshot(dlm_queue *q, dlm_qsnap **out, int *count);
void dlm_queue_snapshot_free(dlm_qsnap *snap, int count);

/* Allocate a snapshot array of all packages. Free with dlm_queue_packages_free. */
int dlm_queue_packages(dlm_queue *q, dlm_psnap **out, int *count);
void dlm_queue_packages_free(dlm_psnap *snap, int count);

/* True if any item is currently active (used for clean shutdown). */
int dlm_queue_active_count(dlm_queue *q);

/* Request all active workers to stop (pause) for shutdown. */
void dlm_queue_stop_all(dlm_queue *q);

#endif /* DLM_QUEUE_H */
