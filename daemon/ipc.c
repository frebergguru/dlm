/* dlmd — JSON-lines request handling. */
#define _POSIX_C_SOURCE 200809L
#include "ipc.h"
#include "dlm/proto.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

static json_t *snap_array(dlm_queue *q)
{
    dlm_qsnap *snap = NULL;
    int n = 0;
    dlm_queue_snapshot(q, &snap, &n);
    json_t *arr = json_array();
    for (int i = 0; i < n; i++) {
        json_t *o = json_object();
        json_object_set_new(o, "id", json_integer(snap[i].id));
        json_object_set_new(o, "url", json_string(snap[i].url ? snap[i].url : ""));
        json_object_set_new(o, "out_path",
                            json_string(snap[i].out_path ? snap[i].out_path : ""));
        if (snap[i].name)
            json_object_set_new(o, "name", json_string(snap[i].name));
        json_object_set_new(o, "connections", json_integer(snap[i].connections));
        json_object_set_new(o, "total", json_integer(snap[i].total));
        json_object_set_new(o, "downloaded", json_integer(snap[i].downloaded));
        json_object_set_new(o, "speed", json_real(snap[i].speed_bps));
        json_object_set_new(o, "state", json_string(dlm_qstate_str(snap[i].state)));
        json_object_set_new(o, "package_id", json_integer(snap[i].package_id));
        json_object_set_new(o, "priority", json_integer(snap[i].priority));
        json_object_set_new(o, "enabled", json_boolean(snap[i].enabled));
        json_object_set_new(o, "autostart", json_boolean(snap[i].autostart));
        json_object_set_new(o, "force", json_boolean(snap[i].force));
        json_object_set_new(o, "list", json_string(dlm_list_str(snap[i].list)));
        json_object_set_new(o, "availability",
                            json_string(snap[i].availability ? snap[i].availability
                                                             : "unknown"));
        json_object_set_new(o, "delegate", json_boolean(snap[i].delegate));
        if (snap[i].error)
            json_object_set_new(o, "error", json_string(snap[i].error));
        json_array_append_new(arr, o);
    }
    dlm_queue_snapshot_free(snap, n);
    return arr;
}

static json_t *pkg_array(dlm_queue *q)
{
    dlm_psnap *snap = NULL;
    int n = 0;
    dlm_queue_packages(q, &snap, &n);
    json_t *arr = json_array();
    for (int i = 0; i < n; i++) {
        json_t *o = json_object();
        json_object_set_new(o, "id", json_integer(snap[i].id));
        json_object_set_new(o, "name", json_string(snap[i].name ? snap[i].name : ""));
        if (snap[i].folder)
            json_object_set_new(o, "folder", json_string(snap[i].folder));
        if (snap[i].comment)
            json_object_set_new(o, "comment", json_string(snap[i].comment));
        json_object_set_new(o, "list", json_string(dlm_list_str(snap[i].list)));
        json_object_set_new(o, "priority", json_integer(snap[i].priority));
        json_object_set_new(o, "collapsed", json_boolean(snap[i].collapsed));
        json_object_set_new(o, "links", json_integer(snap[i].link_count));
        json_array_append_new(arr, o);
    }
    dlm_queue_packages_free(snap, n);
    return arr;
}

static char *dump_and_free(json_t *o)
{
    char *s = json_dumps(o, JSON_COMPACT);
    json_decref(o);
    return s;
}

static char *err_resp(const char *msg)
{
    json_t *o = json_object();
    json_object_set_new(o, "ok", json_false());
    json_object_set_new(o, "error", json_string(msg));
    return dump_and_free(o);
}

static char *ok_count(const char *key, int count)
{
    json_t *o = json_object();
    json_object_set_new(o, "ok", json_true());
    if (key) json_object_set_new(o, key, json_integer(count));
    return dump_and_free(o);
}

static int64_t get_id(json_t *req)
{
    json_t *v = json_object_get(req, "id");
    return json_is_integer(v) ? json_integer_value(v) : -1;
}

static int get_bool(json_t *req, const char *key, int dflt)
{
    json_t *v = json_object_get(req, key);
    if (json_is_boolean(v)) return json_is_true(v) ? 1 : 0;
    return dflt;
}

static int get_int(json_t *req, const char *key, int dflt)
{
    json_t *v = json_object_get(req, key);
    return json_is_integer(v) ? (int)json_integer_value(v) : dflt;
}

static int parse_move(const char *s, dlm_move *out)
{
    if (!s) return -1;
    if (!strcmp(s, "up")) *out = DLM_MOVE_UP;
    else if (!strcmp(s, "down")) *out = DLM_MOVE_DOWN;
    else if (!strcmp(s, "top")) *out = DLM_MOVE_TOP;
    else if (!strcmp(s, "bottom")) *out = DLM_MOVE_BOTTOM;
    else return -1;
    return 0;
}

/* Parse the "links" array of a grab request into a dlm_grab_link vector that
 * borrows strings from the json (valid while `req` lives). Returns count, or -1;
 * the caller frees *out. */
static int parse_grab_links(json_t *arr, dlm_grab_link **out)
{
    if (!json_is_array(arr)) return -1;
    size_t n = json_array_size(arr);
    if (n == 0) return -1;
    dlm_grab_link *links = calloc(n, sizeof *links);
    size_t i;
    json_t *o;
    json_array_foreach(arr, i, o) {
        links[i].url = json_string_value(json_object_get(o, "url"));
        links[i].out_path = json_string_value(json_object_get(o, "out"));
        links[i].name = json_string_value(json_object_get(o, "name"));
        json_t *sz = json_object_get(o, "size");
        links[i].size = json_is_integer(sz) ? json_integer_value(sz) : -1;
        links[i].connections = get_int(o, "connections", 0);
        links[i].delegate = get_bool(o, "delegate", 0);
        links[i].availability = json_string_value(json_object_get(o, "availability"));
    }
    *out = links;
    return (int)n;
}

char *dlm_ipc_handle(dlm_queue *q, const char *line, int *want_subscribe,
                     int *want_shutdown)
{
    json_error_t jerr;
    json_t *req = json_loads(line, 0, &jerr);
    if (!req) return err_resp("invalid json");

    const char *cmd = json_string_value(json_object_get(req, "cmd"));
    if (!cmd) { json_decref(req); return err_resp("missing cmd"); }

    char *out = NULL;

    if (!strcmp(cmd, "add")) {
        const char *url = json_string_value(json_object_get(req, "url"));
        const char *opath = json_string_value(json_object_get(req, "out"));
        int conns = get_int(req, "connections", 0);
        int delegate = get_bool(req, "delegate", 0);
        if (!url) {
            out = err_resp("add: missing url");
        } else {
            int64_t id = dlm_queue_add(q, url, opath, conns, delegate);
            if (id < 0) {
                out = err_resp("add: failed");
            } else {
                json_t *o = json_object();
                json_object_set_new(o, "ok", json_true());
                json_object_set_new(o, "id", json_integer(id));
                out = dump_and_free(o);
            }
        }
    } else if (!strcmp(cmd, "grab")) {
        const char *name = json_string_value(json_object_get(req, "name"));
        const char *folder = json_string_value(json_object_get(req, "folder"));
        dlm_grab_link *links = NULL;
        int n = parse_grab_links(json_object_get(req, "links"), &links);
        if (n < 0) {
            out = err_resp("grab: missing/empty links");
        } else {
            int64_t pkg = dlm_queue_grab(q, name, folder, links, n);
            free(links);
            if (pkg < 0) out = err_resp("grab: failed");
            else {
                json_t *o = json_object();
                json_object_set_new(o, "ok", json_true());
                json_object_set_new(o, "package_id", json_integer(pkg));
                out = dump_and_free(o);
            }
        }
    } else if (!strcmp(cmd, "list")) {
        json_t *o = json_object();
        json_object_set_new(o, "ok", json_true());
        json_object_set_new(o, "max_active",
                            json_integer(dlm_queue_get_max_active(q)));
        json_object_set_new(o, "max_speed",
                            json_integer(dlm_queue_get_max_speed(q)));
        json_object_set_new(o, "autostart",
                            json_boolean(dlm_queue_get_global_autostart(q)));
        json_object_set_new(o, "packages", pkg_array(q));
        json_object_set_new(o, "downloads", snap_array(q));
        out = dump_and_free(o);
    } else if (!strcmp(cmd, "confirm")) {
        int n = dlm_queue_confirm(q, get_id(req), get_bool(req, "package", 0),
                                  get_bool(req, "start", 1));
        out = n < 0 ? err_resp("confirm: no such linkgrabber item")
                    : ok_count("moved", n);
    } else if (!strcmp(cmd, "lg_remove") || !strcmp(cmd, "lg_clear")) {
        /* lg_clear is lg_remove with no id (whole linkgrabber) */
        int64_t id = !strcmp(cmd, "lg_clear") ? -1 : get_id(req);
        int n = dlm_queue_lg_remove(q, id, get_bool(req, "package", 0));
        out = n < 0 ? err_resp("lg_remove: no such linkgrabber item")
                    : ok_count("removed", n);
    } else if (!strcmp(cmd, "pause") || !strcmp(cmd, "cancel")) {
        /* cancel == pause (stop, keep resumable) */
        int rc = dlm_queue_pause(q, get_id(req));
        out = rc == 0 ? ok_count(NULL, 0) : err_resp("no such download");
    } else if (!strcmp(cmd, "resume")) {
        int rc = dlm_queue_resume(q, get_id(req));
        out = rc == 0 ? ok_count(NULL, 0)
                      : err_resp("cannot resume (not paused/error)");
    } else if (!strcmp(cmd, "rm")) {
        int rc = get_bool(req, "package", 0)
                     ? dlm_queue_pkg_remove(q, get_id(req))
                     : dlm_queue_remove(q, get_id(req));
        out = rc == 0 ? ok_count(NULL, 0) : err_resp("no such download");
    } else if (!strcmp(cmd, "priority")) {
        int rc = dlm_queue_set_priority(q, get_id(req), get_bool(req, "package", 0),
                                        get_int(req, "level", 0));
        out = rc == 0 ? ok_count(NULL, 0) : err_resp("no such item");
    } else if (!strcmp(cmd, "enable") || !strcmp(cmd, "disable")) {
        int rc = dlm_queue_set_enabled(q, get_id(req), get_bool(req, "package", 0),
                                       !strcmp(cmd, "enable"));
        out = rc == 0 ? ok_count(NULL, 0) : err_resp("no such item");
    } else if (!strcmp(cmd, "autostart")) {
        int rc = dlm_queue_set_autostart(q, get_id(req), get_bool(req, "package", 0),
                                         get_bool(req, "on", 1));
        out = rc == 0 ? ok_count(NULL, 0) : err_resp("no such item");
    } else if (!strcmp(cmd, "force")) {
        int rc = dlm_queue_force(q, get_id(req), get_bool(req, "package", 0));
        out = rc == 0 ? ok_count(NULL, 0) : err_resp("no such item");
    } else if (!strcmp(cmd, "move")) {
        dlm_move dir;
        if (parse_move(json_string_value(json_object_get(req, "dir")), &dir) != 0)
            out = err_resp("move: dir must be up|down|top|bottom");
        else {
            int rc = dlm_queue_move(q, get_id(req), get_bool(req, "package", 0), dir);
            out = rc == 0 ? ok_count(NULL, 0) : err_resp("no such item");
        }
    } else if (!strcmp(cmd, "pkg")) {
        int rc = dlm_queue_pkg_update(
            q, get_id(req), json_string_value(json_object_get(req, "name")),
            json_string_value(json_object_get(req, "folder")),
            json_string_value(json_object_get(req, "comment")),
            get_int(req, "priority", DLM_PRIO_LOWEST - 1),
            json_object_get(req, "collapsed")
                ? get_bool(req, "collapsed", 0)
                : -1);
        out = rc == 0 ? ok_count(NULL, 0) : err_resp("no such package");
    } else if (!strcmp(cmd, "clear_finished")) {
        out = ok_count("removed", dlm_queue_clear_finished(q));
    } else if (!strcmp(cmd, "set")) {
        json_t *m = json_object_get(req, "max_active");
        if (json_is_integer(m))
            dlm_queue_set_max_active(q, (int)json_integer_value(m));
        json_t *sp = json_object_get(req, "max_speed");
        if (json_is_integer(sp))
            dlm_queue_set_max_speed(q, json_integer_value(sp));
        json_t *as = json_object_get(req, "autostart");
        if (json_is_boolean(as))
            dlm_queue_set_global_autostart(q, json_is_true(as) ? 1 : 0);
        json_t *o = json_object();
        json_object_set_new(o, "ok", json_true());
        json_object_set_new(o, "max_active",
                            json_integer(dlm_queue_get_max_active(q)));
        json_object_set_new(o, "max_speed",
                            json_integer(dlm_queue_get_max_speed(q)));
        json_object_set_new(o, "autostart",
                            json_boolean(dlm_queue_get_global_autostart(q)));
        out = dump_and_free(o);
    } else if (!strcmp(cmd, "subscribe")) {
        if (want_subscribe) *want_subscribe = 1;
        json_t *o = json_object();
        json_object_set_new(o, "ok", json_true());
        json_object_set_new(o, "max_active",
                            json_integer(dlm_queue_get_max_active(q)));
        json_object_set_new(o, "max_speed",
                            json_integer(dlm_queue_get_max_speed(q)));
        json_object_set_new(o, "autostart",
                            json_boolean(dlm_queue_get_global_autostart(q)));
        json_object_set_new(o, "packages", pkg_array(q));
        json_object_set_new(o, "downloads", snap_array(q));
        out = dump_and_free(o);
    } else if (!strcmp(cmd, "ping")) {
        out = dump_and_free(json_pack("{s:b,s:s,s:i}", "ok", 1, "pong", "dlmd",
                                      "proto", DLM_PROTO_VERSION));
    } else if (!strcmp(cmd, "shutdown")) {
        if (want_shutdown) *want_shutdown = 1;
        out = dump_and_free(json_pack("{s:b}", "ok", 1));
    } else {
        out = err_resp("unknown cmd");
    }

    json_decref(req);
    return out;
}

char *dlm_ipc_progress_event(dlm_queue *q)
{
    json_t *o = json_object();
    json_object_set_new(o, "event", json_string("progress"));
    json_object_set_new(o, "max_active", json_integer(dlm_queue_get_max_active(q)));
    json_object_set_new(o, "max_speed", json_integer(dlm_queue_get_max_speed(q)));
    json_object_set_new(o, "autostart",
                        json_boolean(dlm_queue_get_global_autostart(q)));
    json_object_set_new(o, "packages", pkg_array(q));
    json_object_set_new(o, "downloads", snap_array(q));
    return dump_and_free(o);
}
