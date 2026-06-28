/* Offline test: JDownloader-style queue features — linkgrabber staging,
 * packages, confirm, priorities, enable/disable, per-link & global autostart,
 * force, reordering, and clear-finished.
 *
 * Everything runs with max_active=0 (or with no schedulable item) so no worker
 * threads start and no network is touched — this exercises the queue's state
 * machine, grouping and persistence deterministically. */
#include "queue.h"
#include "dlm/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg)                                                       \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

/* ---- snapshot accessors ----------------------------------------------- */

static int link_count(dlm_queue *q)
{
    dlm_qsnap *s = NULL; int n = 0;
    dlm_queue_snapshot(q, &s, &n);
    dlm_queue_snapshot_free(s, n);
    return n;
}

static int count_in_list(dlm_queue *q, dlm_list list)
{
    dlm_qsnap *s = NULL; int n = 0, c = 0;
    dlm_queue_snapshot(q, &s, &n);
    for (int i = 0; i < n; i++) if (s[i].list == list) c++;
    dlm_queue_snapshot_free(s, n);
    return c;
}

/* Find a link by id and copy the fields the tests care about. Returns 1/0. */
static int link_get(dlm_queue *q, int64_t id, dlm_qsnap *out)
{
    dlm_qsnap *s = NULL; int n = 0, found = 0;
    dlm_queue_snapshot(q, &s, &n);
    for (int i = 0; i < n; i++) {
        if (s[i].id != id) continue;
        out->state = s[i].state;
        out->list = s[i].list;
        out->priority = s[i].priority;
        out->enabled = s[i].enabled;
        out->autostart = s[i].autostart;
        out->force = s[i].force;
        out->package_id = s[i].package_id;
        found = 1;
    }
    dlm_queue_snapshot_free(s, n);
    return found;
}

/* The id at display position `idx` (snapshots are position-ordered). */
static int64_t id_at(dlm_queue *q, int idx)
{
    dlm_qsnap *s = NULL; int n = 0;
    int64_t id = -1;
    dlm_queue_snapshot(q, &s, &n);
    if (idx >= 0 && idx < n) id = s[idx].id;
    dlm_queue_snapshot_free(s, n);
    return id;
}

static int pkg_count(dlm_queue *q, dlm_list list)
{
    dlm_psnap *p = NULL; int n = 0, c = 0;
    dlm_queue_packages(q, &p, &n);
    for (int i = 0; i < n; i++) if (p[i].list == list) c++;
    dlm_queue_packages_free(p, n);
    return c;
}

/* ---- tests ------------------------------------------------------------ */

int main(void)
{
    const char *path = "/tmp/dlm_test_package.db";
    unlink(path);

    dlm_store *store = dlm_store_open(path);
    CHECK(store != NULL, "open store");
    dlm_queue *q = dlm_queue_create(store, 0);

    /* --- grab: stage three links into the linkgrabber as one package --- */
    dlm_grab_link links[3] = {
        {"http://x/a.bin", "/d/a.bin", "a.bin", 100, 4, 0, "online"},
        {"http://x/b.bin", "/d/b.bin", "b.bin", 200, 4, 0, "online"},
        {"http://x/c.bin", "/d/c.bin", "c.bin", -1, 4, 0, "unknown"},
    };
    int64_t pkgid = dlm_queue_grab(q, "myitem", "/d", links, 3);
    CHECK(pkgid > 0, "grab returns package id");
    CHECK(link_count(q) == 3, "three links staged");
    CHECK(count_in_list(q, DLM_LIST_LINKGRABBER) == 3, "all in linkgrabber");
    CHECK(count_in_list(q, DLM_LIST_DOWNLOAD) == 0, "none in download list");
    CHECK(pkg_count(q, DLM_LIST_LINKGRABBER) == 1, "one linkgrabber package");

    /* linkgrabber links never auto-start, even with capacity */
    dlm_queue_set_max_active(q, 4);
    dlm_queue_tick(q);
    CHECK(dlm_queue_active_count(q) == 0, "linkgrabber items do not start");
    dlm_queue_set_max_active(q, 0);

    /* link ids in display order */
    int64_t a = id_at(q, 0), b = id_at(q, 1), c = id_at(q, 2);
    CHECK(a > 0 && b > 0 && c > 0, "have three link ids");

    /* --- priority + persistence of fields --- */
    CHECK(dlm_queue_set_priority(q, a, 0, DLM_PRIO_HIGHEST) == 0, "set prio a");
    dlm_queue_set_enabled(q, b, 0, 0);                 /* disable b */
    dlm_qsnap g;
    CHECK(link_get(q, a, &g) && g.priority == DLM_PRIO_HIGHEST, "a is highest");
    CHECK(link_get(q, b, &g) && g.enabled == 0, "b disabled");

    /* --- reorder: move c to the top of its package --- */
    CHECK(dlm_queue_move(q, c, 0, DLM_MOVE_TOP) == 0, "move c top");
    CHECK(id_at(q, 0) == c, "c now displays first");

    /* --- confirm one package to the download list (manual, no auto-start) --- */
    int moved = dlm_queue_confirm(q, pkgid, 1, 0 /* start=0 => manual */);
    CHECK(moved == 3, "confirm moved three links");
    CHECK(count_in_list(q, DLM_LIST_DOWNLOAD) == 3, "now in download list");
    CHECK(count_in_list(q, DLM_LIST_LINKGRABBER) == 0, "linkgrabber empty");
    CHECK(pkg_count(q, DLM_LIST_DOWNLOAD) == 1, "package moved too");

    /* manual (autostart off) items must not auto-start even with capacity and
     * the global switch on */
    CHECK(link_get(q, a, &g) && g.autostart == 0, "confirmed manual => autostart off");
    dlm_queue_set_max_active(q, 4);
    dlm_queue_tick(q);
    CHECK(dlm_queue_active_count(q) == 0, "manual items do not auto-start");

    /* global autostart switch off also blocks starts */
    dlm_queue_set_global_autostart(q, 0);
    CHECK(dlm_queue_get_global_autostart(q) == 0, "global autostart off");
    dlm_queue_set_max_active(q, 0);

    /* --- force sets the flag immediately (we don't tick: would download) --- */
    CHECK(dlm_queue_force(q, a, 0) == 0, "force a");
    CHECK(link_get(q, a, &g) && g.force == 1, "a is forced");

    /* --- persistence across a simulated daemon restart --- */
    dlm_queue_destroy(q);
    dlm_queue *q2 = dlm_queue_create(store, 0);
    CHECK(link_count(q2) == 3, "links survive restart");
    CHECK(pkg_count(q2, DLM_LIST_DOWNLOAD) == 1, "package survives restart");
    CHECK(link_get(q2, a, &g) && g.priority == DLM_PRIO_HIGHEST,
          "priority survives restart");
    CHECK(link_get(q2, b, &g) && g.enabled == 0, "disabled survives restart");
    CHECK(link_get(q2, a, &g) && g.autostart == 0, "manual survives restart");
    CHECK(link_get(q2, a, &g) && g.force == 0, "force is cleared on restart");
    CHECK(id_at(q2, 0) == c, "display order survives restart");

    /* --- clear_finished: seed a done link via the store, reload, clear --- */
    dlm_queue_destroy(q2);
    int64_t done_id = dlm_store_add(store, "http://x/done", "done.bin", 1, 0, 0);
    dlm_store_set_state(store, done_id, "done", NULL);
    dlm_queue *q3 = dlm_queue_create(store, 0);
    CHECK(link_count(q3) == 4, "four links incl. the done one");
    int cleared = dlm_queue_clear_finished(q3);
    CHECK(cleared == 1, "cleared exactly one finished link");
    CHECK(link_count(q3) == 3, "three links remain after clear");
    CHECK(link_get(q3, done_id, &g) == 0, "done link is gone");

    /* --- lg_remove on the download list must not touch download items --- */
    int r = dlm_queue_lg_remove(q3, -1, 0); /* clear linkgrabber (empty) */
    CHECK(r == 0, "lg_clear removes nothing (linkgrabber empty)");
    CHECK(link_count(q3) == 3, "download links untouched by lg_clear");

    /* --- remove a whole package --- */
    CHECK(dlm_queue_pkg_remove(q3, pkgid) == 0, "pkg_remove ok");
    CHECK(count_in_list(q3, DLM_LIST_DOWNLOAD) == 0, "package links removed");
    CHECK(pkg_count(q3, DLM_LIST_DOWNLOAD) == 0, "package row removed");

    dlm_queue_destroy(q3);
    dlm_store_close(store);
    unlink(path);

    if (failures == 0) { printf("test_package: all passed\n"); return 0; }
    fprintf(stderr, "test_package: %d failure(s)\n", failures);
    return 1;
}
