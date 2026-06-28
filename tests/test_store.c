/* Offline test: sqlite store CRUD + persistence across reopen. */
#include "dlm/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg)                                                       \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

static int g_rows;
static char g_state[32];
static int64_t g_downloaded;
static int g_delegate2 = -1;

static void count_cb(void *ud, const dlm_store_row *r)
{
    (void)ud;
    g_rows++;
    if (r->id == 1) {
        snprintf(g_state, sizeof g_state, "%s", r->state ? r->state : "");
        g_downloaded = r->downloaded;
    }
    if (r->id == 2) g_delegate2 = r->delegate;
}

int main(void)
{
    const char *path = "/tmp/dlm_test_store.db";
    unlink(path);

    dlm_store *s = dlm_store_open(path);
    CHECK(s != NULL, "open store");

    int64_t id1 = dlm_store_add(s, "http://x/a", "a.bin", 4, 0, 1000);
    int64_t id2 = dlm_store_add(s, "http://x/b", "b.bin", 8, 1, 2000);
    CHECK(id1 == 1 && id2 == 2, "sequential ids");

    dlm_store_set_progress(s, id1, 500, 123);
    dlm_store_set_state(s, id1, "paused", NULL);
    dlm_store_set_state(s, id2, "error", "boom");

    /* reopen to prove durability */
    dlm_store_close(s);
    s = dlm_store_open(path);
    CHECK(s != NULL, "reopen store");

    g_rows = 0;
    dlm_store_load_all(s, count_cb, NULL);
    CHECK(g_rows == 2, "two rows persisted");
    CHECK(strcmp(g_state, "paused") == 0, "state persisted");
    CHECK(g_downloaded == 123, "progress persisted");
    CHECK(g_delegate2 == 1, "delegate flag persisted");

    /* delete */
    dlm_store_delete(s, id1);
    g_rows = 0;
    dlm_store_load_all(s, count_cb, NULL);
    CHECK(g_rows == 1, "row deleted");

    dlm_store_close(s);
    unlink(path);

    if (failures == 0) { printf("test_store: all passed\n"); return 0; }
    fprintf(stderr, "test_store: %d failure(s)\n", failures);
    return 1;
}
