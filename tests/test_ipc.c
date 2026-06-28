/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Offline test: the JSON-lines IPC dispatch (dlm_ipc_handle) against a real
 * queue, with no socket or separate process. Verifies the request->queue->
 * response path for every command. */
#include "ipc.h"
#include "queue.h"
#include "dlm/store.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg)                                                       \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

/* run one request, return parsed json (caller decrefs) */
static json_t *call(dlm_queue *q, const char *req, int *sub, int *shut)
{
    int s = 0, sh = 0;
    char *resp = dlm_ipc_handle(q, req, &s, &sh);
    if (sub) *sub = s;
    if (shut) *shut = sh;
    json_error_t e;
    json_t *root = resp ? json_loads(resp, 0, &e) : NULL;
    free(resp);
    return root;
}

static int is_ok(json_t *r) { return r && json_is_true(json_object_get(r, "ok")); }

static int g_delegate_rows;
static void count_delegate(void *ud, const dlm_store_row *row)
{
    (void)ud;
    if (row->delegate) g_delegate_rows++;
}

static int dl_count(json_t *r)
{
    json_t *a = json_object_get(r, "downloads");
    return json_is_array(a) ? (int)json_array_size(a) : -1;
}

static const char *first_state(json_t *r)
{
    json_t *a = json_object_get(r, "downloads");
    if (!json_is_array(a) || json_array_size(a) == 0) return "";
    return json_string_value(json_object_get(json_array_get(a, 0), "state"));
}

int main(void)
{
    const char *path = "/tmp/dlm_test_ipc.db";
    unlink(path);
    dlm_store *store = dlm_store_open(path);
    dlm_queue *q = dlm_queue_create(store, 0); /* nothing auto-starts */

    json_t *r;

    r = call(q, "{\"cmd\":\"ping\"}", NULL, NULL);
    CHECK(is_ok(r), "ping ok");
    CHECK(r && strcmp(json_string_value(json_object_get(r, "pong")), "dlmd") == 0,
          "ping pong");
    json_decref(r);

    r = call(q, "not valid json", NULL, NULL);
    CHECK(r && !is_ok(r), "invalid json rejected");
    json_decref(r);

    r = call(q, "{\"cmd\":\"bogus\"}", NULL, NULL);
    CHECK(r && !is_ok(r), "unknown cmd rejected");
    json_decref(r);

    r = call(q, "{\"cmd\":\"add\"}", NULL, NULL);
    CHECK(r && !is_ok(r), "add without url rejected");
    json_decref(r);

    r = call(q, "{\"cmd\":\"add\",\"url\":\"http://x/a\",\"out\":\"a.bin\",\"connections\":4}",
             NULL, NULL);
    CHECK(is_ok(r), "add ok");
    int64_t id = r ? json_integer_value(json_object_get(r, "id")) : -1;
    CHECK(id == 1, "add id == 1");
    json_decref(r);

    r = call(q, "{\"cmd\":\"list\"}", NULL, NULL);
    CHECK(dl_count(r) == 1, "list has 1");
    CHECK(strcmp(first_state(r), "queued") == 0, "state queued");
    json_decref(r);

    r = call(q, "{\"cmd\":\"pause\",\"id\":1}", NULL, NULL);
    CHECK(is_ok(r), "pause ok");
    json_decref(r);
    r = call(q, "{\"cmd\":\"list\"}", NULL, NULL);
    CHECK(strcmp(first_state(r), "paused") == 0, "now paused");
    json_decref(r);

    r = call(q, "{\"cmd\":\"resume\",\"id\":1}", NULL, NULL);
    CHECK(is_ok(r), "resume ok");
    json_decref(r);
    r = call(q, "{\"cmd\":\"list\"}", NULL, NULL);
    CHECK(strcmp(first_state(r), "queued") == 0, "queued again");
    json_decref(r);

    r = call(q, "{\"cmd\":\"set\",\"max_active\":5}", NULL, NULL);
    CHECK(is_ok(r) && json_integer_value(json_object_get(r, "max_active")) == 5,
          "set max_active");
    json_decref(r);

    int sub = 0, shut = 0;
    r = call(q, "{\"cmd\":\"subscribe\"}", &sub, NULL);
    CHECK(is_ok(r) && sub == 1, "subscribe sets flag");
    json_decref(r);

    r = call(q, "{\"cmd\":\"shutdown\"}", NULL, &shut);
    CHECK(is_ok(r) && shut == 1, "shutdown sets flag");
    json_decref(r);

    /* a delegated (stream/series) add must persist delegate=1 in the store */
    r = call(q, "{\"cmd\":\"add\",\"url\":\"nrk:ABC\",\"out\":\"ep.mp4\",\"delegate\":true}",
             NULL, NULL);
    CHECK(is_ok(r), "delegate add ok");
    json_decref(r);

    r = call(q, "{\"cmd\":\"rm\",\"id\":1}", NULL, NULL);
    CHECK(is_ok(r), "rm ok");
    json_decref(r);
    r = call(q, "{\"cmd\":\"list\"}", NULL, NULL);
    CHECK(dl_count(r) == 1, "delegate item remains after removing the first");
    json_decref(r);

    g_delegate_rows = 0;
    dlm_store_load_all(store, count_delegate, NULL);
    CHECK(g_delegate_rows == 1, "delegate flag persisted to store");

    /* ---- linkgrabber + queue commands over the wire ---- */

    /* grab two links into the linkgrabber as a package */
    r = call(q,
             "{\"cmd\":\"grab\",\"name\":\"item\",\"folder\":\"/d\",\"links\":["
             "{\"url\":\"http://x/p\",\"out\":\"/d/p\",\"size\":10},"
             "{\"url\":\"http://x/q\",\"out\":\"/d/q\",\"size\":20}]}",
             NULL, NULL);
    CHECK(is_ok(r), "grab ok");
    int64_t gpkg = r ? json_integer_value(json_object_get(r, "package_id")) : -1;
    CHECK(gpkg > 0, "grab returns package_id");
    json_decref(r);

    /* list now carries a packages array */
    r = call(q, "{\"cmd\":\"list\"}", NULL, NULL);
    CHECK(json_is_array(json_object_get(r, "packages")), "list has packages");
    CHECK(json_array_size(json_object_get(r, "packages")) >= 1, "one+ package");
    json_decref(r);

    /* confirm the whole linkgrabber into the download list (manual) */
    r = call(q, "{\"cmd\":\"confirm\",\"start\":false}", NULL, NULL);
    CHECK(is_ok(r) && json_integer_value(json_object_get(r, "moved")) == 2,
          "confirm moved two links");
    json_decref(r);

    /* priority / enable / disable / autostart / move / force all dispatch ok */
    {
        char buf[128];
        snprintf(buf, sizeof buf,
                 "{\"cmd\":\"priority\",\"id\":%lld,\"package\":true,\"level\":3}",
                 (long long)gpkg);
        r = call(q, buf, NULL, NULL);
        CHECK(is_ok(r), "priority on package ok");
        json_decref(r);
    }
    r = call(q, "{\"cmd\":\"disable\",\"id\":3}", NULL, NULL);
    CHECK(is_ok(r), "disable ok");
    json_decref(r);
    r = call(q, "{\"cmd\":\"enable\",\"id\":3}", NULL, NULL);
    CHECK(is_ok(r), "enable ok");
    json_decref(r);
    r = call(q, "{\"cmd\":\"autostart\",\"id\":3,\"on\":false}", NULL, NULL);
    CHECK(is_ok(r), "autostart ok");
    json_decref(r);
    r = call(q, "{\"cmd\":\"move\",\"id\":3,\"dir\":\"down\"}", NULL, NULL);
    CHECK(is_ok(r), "move ok");
    json_decref(r);
    r = call(q, "{\"cmd\":\"move\",\"id\":3,\"dir\":\"sideways\"}", NULL, NULL);
    CHECK(r && !is_ok(r), "bad move dir rejected");
    json_decref(r);

    /* global autostart toggle via set */
    r = call(q, "{\"cmd\":\"set\",\"autostart\":false}", NULL, NULL);
    CHECK(is_ok(r) && json_is_false(json_object_get(r, "autostart")),
          "set autostart=false");
    json_decref(r);

    /* clear_finished returns a count (nothing finished here) */
    r = call(q, "{\"cmd\":\"clear_finished\"}", NULL, NULL);
    CHECK(is_ok(r) && json_integer_value(json_object_get(r, "removed")) == 0,
          "clear_finished ok, none removed");
    json_decref(r);

    /* progress event shape */
    char *ev = dlm_ipc_progress_event(q);
    json_error_t e;
    json_t *evj = json_loads(ev, 0, &e);
    free(ev);
    CHECK(evj && strcmp(json_string_value(json_object_get(evj, "event")),
                        "progress") == 0, "progress event shape");
    json_decref(evj);

    dlm_queue_destroy(q);
    dlm_store_close(store);
    unlink(path);

    if (failures == 0) { printf("test_ipc: all passed\n"); return 0; }
    fprintf(stderr, "test_ipc: %d failure(s)\n", failures);
    return 1;
}
