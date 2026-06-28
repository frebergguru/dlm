/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libdlm — sqlite-backed queue persistence. */
#define _POSIX_C_SOURCE 200809L
#include "dlm/store.h"
#include "internal.h"

#include <sqlite3.h>

struct dlm_store {
    sqlite3 *db;
};

static const char *SCHEMA =
    "PRAGMA journal_mode=WAL;"
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS downloads("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  url TEXT NOT NULL,"
    "  out_path TEXT NOT NULL,"
    "  connections INTEGER NOT NULL DEFAULT 0,"
    "  delegate INTEGER NOT NULL DEFAULT 0,"
    "  total INTEGER NOT NULL DEFAULT -1,"
    "  downloaded INTEGER NOT NULL DEFAULT 0,"
    "  state TEXT NOT NULL DEFAULT 'queued',"
    "  error TEXT,"
    "  created_at INTEGER NOT NULL DEFAULT 0,"
    "  package_id INTEGER NOT NULL DEFAULT 0,"
    "  priority INTEGER NOT NULL DEFAULT 0,"
    "  enabled INTEGER NOT NULL DEFAULT 1,"
    "  list TEXT NOT NULL DEFAULT 'download',"
    "  name TEXT,"
    "  availability TEXT NOT NULL DEFAULT 'unknown',"
    "  position INTEGER NOT NULL DEFAULT 0,"
    "  force INTEGER NOT NULL DEFAULT 0,"
    "  autostart INTEGER NOT NULL DEFAULT 1"
    ");"
    "CREATE TABLE IF NOT EXISTS packages("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name TEXT NOT NULL DEFAULT '',"
    "  folder TEXT,"
    "  comment TEXT,"
    "  list TEXT NOT NULL DEFAULT 'download',"
    "  priority INTEGER NOT NULL DEFAULT 0,"
    "  collapsed INTEGER NOT NULL DEFAULT 0,"
    "  position INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL DEFAULT 0"
    ");";

/* Best-effort migrations for databases created by older schemas. Each ALTER is
 * run independently and its "duplicate column" error ignored, so upgrading an
 * existing queue.db just adds the missing columns. */
static const char *MIGRATIONS[] = {
    "ALTER TABLE downloads ADD COLUMN delegate INTEGER NOT NULL DEFAULT 0;",
    "ALTER TABLE downloads ADD COLUMN package_id INTEGER NOT NULL DEFAULT 0;",
    "ALTER TABLE downloads ADD COLUMN priority INTEGER NOT NULL DEFAULT 0;",
    "ALTER TABLE downloads ADD COLUMN enabled INTEGER NOT NULL DEFAULT 1;",
    "ALTER TABLE downloads ADD COLUMN list TEXT NOT NULL DEFAULT 'download';",
    "ALTER TABLE downloads ADD COLUMN name TEXT;",
    "ALTER TABLE downloads ADD COLUMN availability TEXT NOT NULL DEFAULT 'unknown';",
    "ALTER TABLE downloads ADD COLUMN position INTEGER NOT NULL DEFAULT 0;",
    "ALTER TABLE downloads ADD COLUMN force INTEGER NOT NULL DEFAULT 0;",
    "ALTER TABLE downloads ADD COLUMN autostart INTEGER NOT NULL DEFAULT 1;",
    NULL,
};

dlm_store *dlm_store_open(const char *path)
{
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        DLM_ERROR("store: cannot open %s: %s", path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    char *err = NULL;
    if (sqlite3_exec(db, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
        DLM_ERROR("store: schema init failed: %s", err ? err : "?");
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    /* migrate older databases; "duplicate column" errors are expected & ignored */
    for (int i = 0; MIGRATIONS[i]; i++)
        sqlite3_exec(db, MIGRATIONS[i], NULL, NULL, NULL);
    dlm_store *s = dlm_xcalloc(1, sizeof *s);
    s->db = db;
    return s;
}

void dlm_store_close(dlm_store *s)
{
    if (!s) return;
    sqlite3_close(s->db);
    free(s);
}

int64_t dlm_store_add(dlm_store *s, const char *url, const char *out_path,
                      int connections, int delegate, int64_t created_at)
{
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
    row.created_at = created_at;
    return dlm_store_add_full(s, &row);
}

int64_t dlm_store_add_full(dlm_store *s, const dlm_store_row *row)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO downloads(url,out_path,connections,delegate,total,"
        "downloaded,state,created_at,package_id,priority,enabled,list,name,"
        "availability,position,force,autostart) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    int i = 1;
    sqlite3_bind_text(st, i++, row->url, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, i++, row->out_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, i++, row->connections);
    sqlite3_bind_int(st, i++, row->delegate);
    sqlite3_bind_int64(st, i++, row->total); /* callers pass -1 for unknown */
    sqlite3_bind_int64(st, i++, row->downloaded);
    sqlite3_bind_text(st, i++, row->state ? row->state : "queued", -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, i++, row->created_at);
    sqlite3_bind_int64(st, i++, row->package_id);
    sqlite3_bind_int(st, i++, row->priority);
    sqlite3_bind_int(st, i++, row->enabled);
    sqlite3_bind_text(st, i++, row->list ? row->list : "download", -1,
                      SQLITE_TRANSIENT);
    if (row->name) sqlite3_bind_text(st, i++, row->name, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, i++);
    sqlite3_bind_text(st, i++, row->availability ? row->availability : "unknown",
                      -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, i++, row->position);
    sqlite3_bind_int(st, i++, row->force);
    sqlite3_bind_int(st, i++, row->autostart);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        DLM_ERROR("store: add failed: %s", sqlite3_errmsg(s->db));
        return -1;
    }
    return sqlite3_last_insert_rowid(s->db);
}

int dlm_store_set_progress(dlm_store *s, int64_t id, int64_t total,
                           int64_t downloaded)
{
    sqlite3_stmt *st = NULL;
    const char *sql = total >= 0
        ? "UPDATE downloads SET total=?,downloaded=? WHERE id=?;"
        : "UPDATE downloads SET downloaded=? WHERE id=?;";
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    int i = 1;
    if (total >= 0) sqlite3_bind_int64(st, i++, total);
    sqlite3_bind_int64(st, i++, downloaded);
    sqlite3_bind_int64(st, i++, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int dlm_store_set_state(dlm_store *s, int64_t id, const char *state,
                        const char *error)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "UPDATE downloads SET state=?,error=? WHERE id=?;";
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, state, -1, SQLITE_TRANSIENT);
    if (error) sqlite3_bind_text(st, 2, error, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, 2);
    sqlite3_bind_int64(st, 3, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Shared helper: run "UPDATE downloads SET <col>=? WHERE id=?" with an int64. */
static int set_int_field(dlm_store *s, const char *sql, int64_t val, int64_t id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, val);
    sqlite3_bind_int64(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int dlm_store_set_priority(dlm_store *s, int64_t id, int priority)
{
    return set_int_field(s, "UPDATE downloads SET priority=? WHERE id=?;",
                         priority, id);
}

int dlm_store_set_enabled(dlm_store *s, int64_t id, int enabled)
{
    return set_int_field(s, "UPDATE downloads SET enabled=? WHERE id=?;",
                         enabled ? 1 : 0, id);
}

int dlm_store_set_autostart(dlm_store *s, int64_t id, int autostart)
{
    return set_int_field(s, "UPDATE downloads SET autostart=? WHERE id=?;",
                         autostart ? 1 : 0, id);
}

int dlm_store_set_position(dlm_store *s, int64_t id, int64_t position)
{
    return set_int_field(s, "UPDATE downloads SET position=? WHERE id=?;",
                         position, id);
}

int dlm_store_set_force(dlm_store *s, int64_t id, int force)
{
    return set_int_field(s, "UPDATE downloads SET force=? WHERE id=?;",
                         force ? 1 : 0, id);
}

int dlm_store_set_package(dlm_store *s, int64_t id, int64_t package_id)
{
    return set_int_field(s, "UPDATE downloads SET package_id=? WHERE id=?;",
                         package_id, id);
}

int dlm_store_set_list(dlm_store *s, int64_t id, const char *list)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, "UPDATE downloads SET list=? WHERE id=?;", -1,
                           &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, list, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int dlm_store_delete(dlm_store *s, int64_t id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, "DELETE FROM downloads WHERE id=?;", -1, &st,
                           NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int dlm_store_load_all(dlm_store *s, dlm_store_row_cb cb, void *userdata)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id,url,out_path,connections,delegate,total,downloaded,state,"
        "error,created_at,package_id,priority,enabled,list,name,availability,"
        "position,force,autostart FROM downloads ORDER BY position,id;";
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        dlm_store_row row;
        row.id = sqlite3_column_int64(st, 0);
        row.url = (const char *)sqlite3_column_text(st, 1);
        row.out_path = (const char *)sqlite3_column_text(st, 2);
        row.connections = sqlite3_column_int(st, 3);
        row.delegate = sqlite3_column_int(st, 4);
        row.total = sqlite3_column_int64(st, 5);
        row.downloaded = sqlite3_column_int64(st, 6);
        row.state = (const char *)sqlite3_column_text(st, 7);
        row.error = (const char *)sqlite3_column_text(st, 8);
        row.created_at = sqlite3_column_int64(st, 9);
        row.package_id = sqlite3_column_int64(st, 10);
        row.priority = sqlite3_column_int(st, 11);
        row.enabled = sqlite3_column_int(st, 12);
        row.list = (const char *)sqlite3_column_text(st, 13);
        row.name = (const char *)sqlite3_column_text(st, 14);
        row.availability = (const char *)sqlite3_column_text(st, 15);
        row.position = sqlite3_column_int64(st, 16);
        row.force = sqlite3_column_int(st, 17);
        row.autostart = sqlite3_column_int(st, 18);
        cb(userdata, &row);
    }
    sqlite3_finalize(st);
    return 0;
}

/* ---- packages --------------------------------------------------------- */

int64_t dlm_store_pkg_add(dlm_store *s, const char *name, const char *folder,
                          const char *comment, const char *list, int priority,
                          int64_t position, int64_t created_at)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO packages(name,folder,comment,list,priority,position,"
        "created_at) VALUES(?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, name ? name : "", -1, SQLITE_TRANSIENT);
    if (folder) sqlite3_bind_text(st, 2, folder, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, 2);
    if (comment) sqlite3_bind_text(st, 3, comment, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, 3);
    sqlite3_bind_text(st, 4, list ? list : "download", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, priority);
    sqlite3_bind_int64(st, 6, position);
    sqlite3_bind_int64(st, 7, created_at);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        DLM_ERROR("store: pkg add failed: %s", sqlite3_errmsg(s->db));
        return -1;
    }
    return sqlite3_last_insert_rowid(s->db);
}

int dlm_store_pkg_update(dlm_store *s, int64_t id, const char *name,
                         const char *folder, const char *comment, int priority,
                         int collapsed)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "UPDATE packages SET "
        "name=COALESCE(?,name),folder=COALESCE(?,folder),"
        "comment=COALESCE(?,comment),priority=?,collapsed=? WHERE id=?;";
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if (name) sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, 1);
    if (folder) sqlite3_bind_text(st, 2, folder, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, 2);
    if (comment) sqlite3_bind_text(st, 3, comment, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, 3);
    sqlite3_bind_int(st, 4, priority);
    sqlite3_bind_int(st, 5, collapsed ? 1 : 0);
    sqlite3_bind_int64(st, 6, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int dlm_store_pkg_set_list(dlm_store *s, int64_t id, const char *list)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, "UPDATE packages SET list=? WHERE id=?;", -1,
                           &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, list, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int dlm_store_pkg_set_position(dlm_store *s, int64_t id, int64_t position)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, "UPDATE packages SET position=? WHERE id=?;",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, position);
    sqlite3_bind_int64(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int dlm_store_pkg_delete(dlm_store *s, int64_t id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s->db, "DELETE FROM packages WHERE id=?;", -1, &st,
                           NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int dlm_store_pkg_load_all(dlm_store *s, dlm_store_pkg_cb cb, void *userdata)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT id,name,folder,comment,list,priority,collapsed,position,"
        "created_at FROM packages ORDER BY position,id;";
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        dlm_store_pkg_row row;
        row.id = sqlite3_column_int64(st, 0);
        row.name = (const char *)sqlite3_column_text(st, 1);
        row.folder = (const char *)sqlite3_column_text(st, 2);
        row.comment = (const char *)sqlite3_column_text(st, 3);
        row.list = (const char *)sqlite3_column_text(st, 4);
        row.priority = sqlite3_column_int(st, 5);
        row.collapsed = sqlite3_column_int(st, 6);
        row.position = sqlite3_column_int64(st, 7);
        row.created_at = sqlite3_column_int64(st, 8);
        cb(userdata, &row);
    }
    sqlite3_finalize(st);
    return 0;
}
