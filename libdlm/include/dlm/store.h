/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — durable queue persistence (sqlite3).
 *
 * Stores one row per download (a "link", in JDownloader terms) plus one row per
 * "package" — a named group of links sharing a download folder. Both live in one
 * of two lists: the linkgrabber (a staging area where crawled links are reviewed
 * before being confirmed) and the download list (links the scheduler runs). All
 * of this survives a daemon restart. Byte-level resume is handled separately by
 * the engine's journal sidecar; this store tracks the queue item and its coarse
 * state/progress, ordering, priority and grouping.
 */
#ifndef DLM_STORE_H
#define DLM_STORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dlm_store dlm_store;

/* A link row as seen by store_load_all()'s callback. String fields are owned by
 * the store and only valid for the duration of the callback. */
typedef struct {
    int64_t id;
    const char *url;
    const char *out_path;
    int connections;
    int delegate;        /* 1 => download via yt-dlp instead of the engine */
    int64_t total;
    int64_t downloaded;
    const char *state;   /* "queued" | "active" | "paused" | "done" | "error" */
    const char *error;   /* may be NULL */
    int64_t created_at;  /* unix seconds */

    /* JDownloader-style queue/grouping fields (added in schema v2). */
    int64_t package_id;       /* owning package, or 0 if none */
    int priority;             /* -3..3; 0 = default. Higher runs first. */
    int enabled;              /* 0 => skipped by the scheduler */
    int autostart;            /* 0 => never auto-started (manual-only) */
    const char *list;         /* "download" | "linkgrabber" */
    const char *name;         /* display name (may be NULL => derive from path) */
    const char *availability; /* "online" | "offline" | "unknown" */
    int64_t position;         /* manual ordering key within the list */
    int force;                /* 1 => start now, ignoring limits/priority */
} dlm_store_row;

/* A package row (a group of links). */
typedef struct {
    int64_t id;
    const char *name;
    const char *folder;   /* download directory for the package's links */
    const char *comment;  /* may be NULL */
    const char *list;     /* "download" | "linkgrabber" */
    int priority;
    int collapsed;        /* UI hint: 1 => collapsed in the view */
    int64_t position;
    int64_t created_at;
} dlm_store_pkg_row;

typedef void (*dlm_store_row_cb)(void *userdata, const dlm_store_row *row);
typedef void (*dlm_store_pkg_cb)(void *userdata, const dlm_store_pkg_row *row);

/* Open (creating + migrating schema as needed) the store at `path`.
 * Returns NULL on failure. */
dlm_store *dlm_store_open(const char *path);
void dlm_store_close(dlm_store *s);

/* Group a burst of writes into one transaction (one commit/fsync instead of
 * one per row). Use around bulk inserts; pair begin with commit (or rollback on
 * error). Each returns 0 on success, -1 on failure. */
int dlm_store_begin(dlm_store *s);
int dlm_store_commit(dlm_store *s);
int dlm_store_rollback(dlm_store *s);

/* Insert a new queued download-list link with default grouping (no package,
 * default priority, enabled, availability unknown); returns its id or -1. */
int64_t dlm_store_add(dlm_store *s, const char *url, const char *out_path,
                      int connections, int delegate, int64_t created_at);

/* Insert a fully-specified link row (id is ignored / assigned). Used for
 * linkgrabber crawls that carry a name, size, package and availability.
 * Returns the new id (>0) or -1. */
int64_t dlm_store_add_full(dlm_store *s, const dlm_store_row *row);

/* Update coarse progress (total<0 leaves total unchanged). */
int dlm_store_set_progress(dlm_store *s, int64_t id, int64_t total,
                           int64_t downloaded);

/* Update state and optional error string (error may be NULL). */
int dlm_store_set_state(dlm_store *s, int64_t id, const char *state,
                        const char *error);

/* Per-field link updates (used by the queue's reorder/priority/grouping ops). */
int dlm_store_set_priority(dlm_store *s, int64_t id, int priority);
int dlm_store_set_enabled(dlm_store *s, int64_t id, int enabled);
int dlm_store_set_autostart(dlm_store *s, int64_t id, int autostart);
int dlm_store_set_list(dlm_store *s, int64_t id, const char *list);
int dlm_store_set_position(dlm_store *s, int64_t id, int64_t position);
int dlm_store_set_force(dlm_store *s, int64_t id, int force);
int dlm_store_set_package(dlm_store *s, int64_t id, int64_t package_id);

/* Permanently remove a link row. */
int dlm_store_delete(dlm_store *s, int64_t id);

/* Iterate every link row (ordered by position, then id). */
int dlm_store_load_all(dlm_store *s, dlm_store_row_cb cb, void *userdata);

/* ---- packages --------------------------------------------------------- */

/* Insert a new package; returns its id (>0) or -1. */
int64_t dlm_store_pkg_add(dlm_store *s, const char *name, const char *folder,
                          const char *comment, const char *list, int priority,
                          int64_t position, int64_t created_at);

/* Update a package's mutable metadata. NULL string args leave that field. */
int dlm_store_pkg_update(dlm_store *s, int64_t id, const char *name,
                         const char *folder, const char *comment, int priority,
                         int collapsed);
int dlm_store_pkg_set_list(dlm_store *s, int64_t id, const char *list);
int dlm_store_pkg_set_position(dlm_store *s, int64_t id, int64_t position);
int dlm_store_pkg_delete(dlm_store *s, int64_t id);

/* Iterate every package row (ordered by position, then id). */
int dlm_store_pkg_load_all(dlm_store *s, dlm_store_pkg_cb cb, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* DLM_STORE_H */
