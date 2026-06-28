/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Offline test: queue state machine + persistence across a simulated restart.
 *
 * Uses max_active=0 so no worker threads start (no network); this exercises the
 * add/pause/resume/remove/snapshot logic and proves items survive destroying
 * and recreating the queue from the same store (the daemon-restart path). */
#include "queue.h"
#include "dlm/store.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg)                                                       \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

static dlm_qstate state_of(dlm_queue *q, int64_t id)
{
    dlm_qsnap *s = NULL;
    int n = 0;
    dlm_qstate st = Q_ERROR;
    dlm_queue_snapshot(q, &s, &n);
    for (int i = 0; i < n; i++)
        if (s[i].id == id) st = s[i].state;
    dlm_queue_snapshot_free(s, n);
    return st;
}

static int count(dlm_queue *q)
{
    dlm_qsnap *s = NULL;
    int n = 0;
    dlm_queue_snapshot(q, &s, &n);
    dlm_queue_snapshot_free(s, n);
    return n;
}

int main(void)
{
    const char *path = "/tmp/dlm_test_queue.db";
    unlink(path);

    dlm_store *store = dlm_store_open(path);
    CHECK(store != NULL, "open store");
    dlm_queue *q = dlm_queue_create(store, 0); /* max_active 0: nothing runs */

    int64_t a = dlm_queue_add(q, "http://x/a", "a.bin", 4, 0);
    int64_t b = dlm_queue_add(q, "http://x/b", "b.bin", 4, 0);
    CHECK(a > 0 && b > 0, "add returns ids");
    CHECK(count(q) == 2, "two items");

    dlm_queue_tick(q); /* must not start anything with max_active=0 */
    CHECK(state_of(q, a) == Q_QUEUED, "a queued");

    dlm_queue_pause(q, a);
    CHECK(state_of(q, a) == Q_PAUSED, "a paused");

    dlm_queue_resume(q, a);
    CHECK(state_of(q, a) == Q_QUEUED, "a resumed to queued");

    dlm_queue_remove(q, b);
    CHECK(count(q) == 1, "b removed");

    /* simulate daemon restart: destroy queue, recreate from same store */
    dlm_queue_pause(q, a);
    CHECK(state_of(q, a) == Q_PAUSED, "a paused before restart");
    dlm_queue_destroy(q);

    dlm_queue *q2 = dlm_queue_create(store, 0);
    CHECK(count(q2) == 1, "one item survives restart");
    CHECK(state_of(q2, a) == Q_PAUSED, "paused state survives restart");

    dlm_queue_destroy(q2);
    dlm_store_close(store);
    unlink(path);

    if (failures == 0) { printf("test_queue: all passed\n"); return 0; }
    fprintf(stderr, "test_queue: %d failure(s)\n", failures);
    return 1;
}
