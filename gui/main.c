/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dlm-gui — GTK4 + libadwaita front-end for the dlm download daemon.
 *
 * The GUI is a thin, reactive view over dlmd: it opens a long-lived "subscribe"
 * connection and rebuilds the download list from each progress event, and sends
 * one-shot commands (add/pause/resume/cancel/rm/set) over short connections.
 * All engine/queue logic lives in the daemon; this process only renders and
 * issues commands.
 */
#include "client.h"
#include "dlm/dlm.h"
#include "dlm/extract.h"
#include "dlm/iaauth.h"
#include "dlm/tools.h"
#include "compat/compat.h"

#include <adwaita.h>
#include <errno.h>
#include <jansson.h>
#include <string.h>

/* Watch a (possibly Winsock) socket fd within the GLib main loop. g_unix_fd_add
 * is POSIX-only, so we go through a GIOChannel, which has a Win32-socket
 * constructor. The callback receives the GIOChannel; recover the fd from the
 * App state. */
static guint dlm_gui_add_socket_watch(int fd, GIOCondition cond, GIOFunc fn, gpointer data)
{
#if defined(_WIN32)
    GIOChannel *ch = g_io_channel_win32_new_socket(fd);
#else
    GIOChannel *ch = g_io_channel_unix_new(fd);
#endif
    guint id = g_io_add_watch(ch, cond, fn, data);
    g_io_channel_unref(ch);
    return id;
}

/* libdlm/httpget.c — declared here to avoid pulling in the private header. */
int dlm_http_get_blob(const char *url, const char *const *headers, char **body,
                      size_t *len, long *status);

/* How the linkgrabber groups links. */
enum { GROUP_SITE_PKG = 0, GROUP_SITE, GROUP_PKG };

/* ---- model ------------------------------------------------------------ */

typedef struct {
    long long id;
    char *name;
    char *state;
    long long total, downloaded;
    double speed;
    long long package_id;
    int priority;
    int enabled;
    int autostart;
    int force;
    char *list;            /* "download" | "linkgrabber" */
    char *availability;    /* "online" | "offline" | "unknown" */
    char *url;             /* source URL (for site grouping / favicon) */
} Dl;

typedef struct {
    long long id;
    char *name;
    char *folder;
    char *list;
    int priority;
    int collapsed;
    int links;
} Pkg;

typedef struct {
    AdwApplication *app;
    GtkWindow *win;
    AdwToastOverlay *toast;
    GtkListBox *dlist;      /* download-list view */
    GtkListBox *glist;      /* linkgrabber view */
    AdwViewStack *stack;
    GtkLabel *status;       /* aggregate speed / counts */
    GtkDropDown *filter;    /* All / Active / Queued / Done */
    int sub_fd;
    GString *rx;            /* partial-line buffer for the subscribe stream */
    guint sub_source;       /* g_unix_fd_add id */
    Dl *items;
    int n;
    Pkg *pkgs;
    int npkg;
    int max_active;
    gint64 max_speed;
    int autostart;          /* global Start/Stop state */
    char *download_dir;     /* remembered destination folder */
    GdkClipboard *clip;     /* display clipboard (for the auto-paste monitor) */
    gulong clip_handler;    /* "changed" handler id; 0 when monitoring is off */
    char *clip_last;        /* last URL auto-grabbed, to suppress duplicates */
    int group_mode;         /* GROUP_SITE_PKG | GROUP_SITE | GROUP_PKG */
    char **csites;          /* client-side collapsed site hosts (linkgrabber) */
    int ncsites;
    int open_menus;         /* open row popovers; rebuilds are deferred while >0 */
    int pending_rebuild;    /* a refresh arrived while a menu was open */
} App;

static App g_app;

/* ---- helpers ---------------------------------------------------------- */

static void human(double n, char *buf, size_t len)
{
    static const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int i = 0;
    while (n >= 1024.0 && i < 4) { n /= 1024.0; i++; }
    snprintf(buf, len, "%.1f %s", n, u[i]);
}

static void toast(App *a, const char *msg)
{
    adw_toast_overlay_add_toast(a->toast, adw_toast_new(msg));
}

/* open a short connection, send one command, ignore the reply */
static void send_cmd(const char *json)
{
    int fd = dlm_client_connect();
    if (fd < 0) { toast(&g_app, "Cannot reach daemon"); return; }
    char *resp = dlm_client_rpc(fd, json);
    free(resp);
    dlm_client_close(fd);
}

/* ---- generic, parameterized row/package action ----------------------- */

/* One descriptor wires a popover button (or quick button) to a daemon command,
 * covering every CLI verb: pause/resume/rm, priority, enable/disable, autostart,
 * force, move, confirm, lg_remove, pkg. Optional fields are sent only when set. */
typedef struct {
    char cmd[20];
    long long id;
    int is_pkg;
    int has_level; int level;     /* priority */
    const char *dir;              /* move: up/down/top/bottom (static) */
    int onflag;                   /* autostart "on": -1 none else 0/1 */
    int startflag;                /* confirm "start": -1 none else 0/1 */
    int collapsedflag;            /* pkg "collapsed": -1 none else 0/1 */
    GtkPopover *pop;              /* popped down after firing */
} ActionCtx;

/* Base context with all optional fields disabled (-1) — never brace-init an
 * ActionCtx directly or zeroed onflag/startflag would send on=false. */
static ActionCtx ctx_make(const char *cmd, long long id, int is_pkg)
{
    ActionCtx c;
    memset(&c, 0, sizeof c);
    snprintf(c.cmd, sizeof c.cmd, "%s", cmd);
    c.id = id;
    c.is_pkg = is_pkg;
    c.onflag = -1;
    c.startflag = -1;
    c.collapsedflag = -1;
    return c;
}

static void on_action(GtkButton *b, gpointer u)
{
    (void)b;
    ActionCtx *c = u;
    json_t *r = json_object();
    json_object_set_new(r, "cmd", json_string(c->cmd));
    if (c->id > 0) json_object_set_new(r, "id", json_integer(c->id));
    if (c->is_pkg) json_object_set_new(r, "package", json_true());
    if (c->has_level) json_object_set_new(r, "level", json_integer(c->level));
    if (c->dir) json_object_set_new(r, "dir", json_string(c->dir));
    if (c->onflag >= 0) json_object_set_new(r, "on", json_boolean(c->onflag));
    if (c->startflag >= 0) json_object_set_new(r, "start", json_boolean(c->startflag));
    if (c->collapsedflag >= 0)
        json_object_set_new(r, "collapsed", json_boolean(c->collapsedflag));
    char *s = json_dumps(r, JSON_COMPACT);
    json_decref(r);
    send_cmd(s);
    free(s);
    if (c->pop) gtk_popover_popdown(c->pop);
}

/* Icon button that fires an ActionCtx directly (no popover). */
static GtkWidget *quick_btn(const char *icon, const char *tip, ActionCtx tmpl)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    ActionCtx *c = g_new0(ActionCtx, 1);
    *c = tmpl;
    c->pop = NULL;
    g_object_set_data_full(G_OBJECT(b), "ctx", c, g_free);
    g_signal_connect(b, "clicked", G_CALLBACK(on_action), c);
    return b;
}

/* Append a labelled button to a popover's box, owning a copy of `tmpl`. */
static void pop_add(GtkWidget *box, GtkPopover *pop, const char *label,
                    ActionCtx tmpl)
{
    GtkWidget *btn = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(btn, "flat");
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    GtkWidget *lbl = gtk_button_get_child(GTK_BUTTON(btn));
    if (GTK_IS_LABEL(lbl)) gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    ActionCtx *c = g_new0(ActionCtx, 1);
    *c = tmpl;
    c->pop = pop;
    g_object_set_data_full(G_OBJECT(btn), "ctx", c, g_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_action), c);
    gtk_box_append(GTK_BOX(box), btn);
}

static void pop_sep(GtkWidget *box)
{
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
}

/* Build a "⋮" menu button whose popover contains `box` (caller fills it). */
static void rebuild_list(App *a); /* fwd */

/* While a row menu is open, the live refresh must not tear down the list (it
 * would dismiss the menu). Count open popovers and defer rebuilds until none
 * remain. */
static void on_menu_shown(GtkWidget *pop, gpointer user)
{
    (void)pop;
    ((App *)user)->open_menus++;
}
static void on_menu_closed(GtkPopover *pop, gpointer user)
{
    (void)pop;
    App *a = user;
    if (a->open_menus > 0) a->open_menus--;
    if (a->open_menus == 0 && a->pending_rebuild) {
        a->pending_rebuild = 0;
        rebuild_list(a);
    }
}

static GtkWidget *menu_button(GtkWidget **box_out, GtkPopover **pop_out)
{
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(mb), "view-more-symbolic");
    gtk_widget_add_css_class(mb, "flat");
    GtkWidget *pop = gtk_popover_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top(box, 6); gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6); gtk_widget_set_margin_end(box, 6);
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), pop);
    g_signal_connect(pop, "show", G_CALLBACK(on_menu_shown), &g_app);
    g_signal_connect(pop, "closed", G_CALLBACK(on_menu_closed), &g_app);
    *box_out = box;
    *pop_out = GTK_POPOVER(pop);
    return mb;
}

/* Add the shared "Priority ▸" and "Move ▸" entries to a popover box. */
static void pop_add_priority(GtkWidget *box, GtkPopover *pop, long long id, int is_pkg)
{
    static const struct { const char *label; int level; } P[] = {
        {"Priority: Highest", 3}, {"Priority: High", 1},
        {"Priority: Default", 0}, {"Priority: Low", -1},
        {"Priority: Lowest", -3},
    };
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
        ActionCtx c = ctx_make("priority", id, is_pkg);
        c.has_level = 1;
        c.level = P[i].level;
        pop_add(box, pop, P[i].label, c);
    }
}

static void pop_add_move(GtkWidget *box, GtkPopover *pop, long long id, int is_pkg)
{
    static const char *D[] = {"up", "down", "top", "bottom"};
    static const char *L[] = {"Move up", "Move down", "Move to top", "Move to bottom"};
    for (size_t i = 0; i < 4; i++) {
        ActionCtx c = ctx_make("move", id, is_pkg);
        c.dir = D[i];
        pop_add(box, pop, L[i], c);
    }
}

/* ---- rendering -------------------------------------------------------- */

static const char *current_filter(App *a)
{
    guint sel = gtk_drop_down_get_selected(a->filter);
    switch (sel) {
    case 1: return "active";
    case 2: return "queued";
    case 3: return "done";
    default: return NULL; /* all */
    }
}

static void on_pkg_edit_clicked(GtkButton *b, gpointer u); /* fwd */
static void rebuild_list(App *a);                          /* fwd */

/* ---- site grouping: host / name / favicon ---------------------------- */

/* Extract the bare host from a URL into `buf` (scheme, userinfo, port and path
 * stripped; leading "www." dropped). Empty for schemeless URLs like magnet:. */
static void host_of(const char *url, char *buf, size_t len)
{
    if (len) buf[0] = '\0';
    if (!url || !len) return;
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    const char *at = strchr(p, '@');
    if (at && (!slash || at < slash)) p = at + 1;      /* drop user:pass@ */
    size_t i = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#' && i < len - 1)
        buf[i++] = (char)g_ascii_tolower((guchar)*p++);
    buf[i] = '\0';
    if (g_str_has_prefix(buf, "www.")) memmove(buf, buf + 4, strlen(buf + 4) + 1);
}

/* A human-friendly display name for a host (falls back to the host itself). */
static const char *site_label(const char *host)
{
    if (!host || !*host) return "Other links";
    if (strstr(host, "archive.org")) return "Internet Archive";
    if (strstr(host, "youtube") || strstr(host, "youtu.be") ||
        strstr(host, "ytimg") || strstr(host, "googlevideo")) return "YouTube";
    if (strstr(host, "vimeo")) return "Vimeo";
    if (strstr(host, "soundcloud")) return "SoundCloud";
    if (!strcmp(host, "magnet")) return "Magnet links";
    return host;
}

/* ---- favicon fetch + cache (async, off the UI thread) ---------------- */

/* host -> state: GINT 1 = fetch in flight, 2 = gave up (no usable icon). */
static GHashTable *favicon_state;

static char *favicon_cache_path(const char *host)
{
    char safe[256];
    size_t i = 0;
    for (const char *p = host; *p && i < sizeof safe - 1; p++)
        safe[i++] = (g_ascii_isalnum(*p) || *p == '.' || *p == '-') ? *p : '_';
    safe[i] = '\0';
    char *dir = g_build_filename(g_get_user_cache_dir(), "dlm", "favicons", NULL);
    char *path = g_strdup_printf("%s/%s.ico", dir, safe);
    g_free(dir);
    return path;
}

/* Worker thread: fetch https://host/favicon.ico into the cache file. */
static void favicon_worker(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    const char *host = data;
    char url[512];
    snprintf(url, sizeof url, "https://%s/favicon.ico", host);
    char *body = NULL; size_t len = 0; long st = 0;
    gboolean ok = FALSE;
    if (dlm_http_get_blob(url, NULL, &body, &len, &st) == 0 &&
        st >= 200 && st < 300 && len > 0) {
        char *path = favicon_cache_path(host);
        char *dir = g_path_get_dirname(path);
        g_mkdir_with_parents(dir, 0700);
        ok = g_file_set_contents(path, body, (gssize)len, NULL);
        g_free(dir);
        g_free(path);
    }
    g_free(body);
    g_task_return_boolean(task, ok);
}

/* Try to load the cached icon into `img`; returns FALSE if missing/unusable. */
static gboolean favicon_load(GtkImage *img, const char *host)
{
    char *path = favicon_cache_path(host);
    gboolean ok = FALSE;
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        /* GdkPixbuf handles .ico (and scaling) more reliably than GdkTexture */
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size(path, 24, 24, NULL);
        if (pb) {
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
            GdkTexture *tex = gdk_texture_new_for_pixbuf(pb);
G_GNUC_END_IGNORE_DEPRECATIONS
            gtk_image_set_from_paintable(img, GDK_PAINTABLE(tex));
            g_object_unref(tex);
            g_object_unref(pb);
            ok = TRUE;
        }
    }
    g_free(path);
    return ok;
}

static void favicon_fetched(GObject *src, GAsyncResult *res, gpointer user)
{
    (void)src;
    GtkImage *img = GTK_IMAGE(user);            /* held with a ref until now */
    const char *host = g_object_get_data(G_OBJECT(img), "host");
    gboolean ok = g_task_propagate_boolean(G_TASK(res), NULL);
    if (ok && favicon_load(img, host))
        g_hash_table_remove(favicon_state, host);                 /* cached now */
    else
        g_hash_table_replace(favicon_state, g_strdup(host), GINT_TO_POINTER(2));
    g_object_unref(img);
}

/* A 24px site icon: cached favicon if available, otherwise a generic globe
 * with a one-shot background fetch kicked off for next time. */
static GtkWidget *make_site_icon(const char *host)
{
    GtkWidget *img = gtk_image_new_from_icon_name("emblem-web-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(img), 24);
    if (!host || !*host) return img;

    if (favicon_load(GTK_IMAGE(img), host)) return img;

    if (!favicon_state)
        favicon_state = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    /* skip if a fetch is already running or we previously gave up */
    if (g_hash_table_contains(favicon_state, host)) return img;
    g_hash_table_replace(favicon_state, g_strdup(host), GINT_TO_POINTER(1));

    g_object_set_data_full(G_OBJECT(img), "host", g_strdup(host), g_free);
    GTask *t = g_task_new(NULL, NULL, favicon_fetched, g_object_ref(img));
    g_task_set_task_data(t, g_strdup(host), g_free);
    g_task_run_in_thread(t, favicon_worker);
    g_object_unref(t);
    return img;
}

/* ---- client-side site collapse + per-site actions -------------------- */

static int site_collapsed(App *a, const char *host)
{
    for (int i = 0; i < a->ncsites; i++)
        if (!strcmp(a->csites[i], host)) return 1;
    return 0;
}

static void site_collapse_toggle(App *a, const char *host)
{
    for (int i = 0; i < a->ncsites; i++)
        if (!strcmp(a->csites[i], host)) {            /* expand: drop it */
            g_free(a->csites[i]);
            a->csites[i] = a->csites[--a->ncsites];
            return;
        }
    a->csites = g_realloc(a->csites, (size_t)(a->ncsites + 1) * sizeof *a->csites);
    a->csites[a->ncsites++] = g_strdup(host);          /* collapse: add it */
}

static void on_site_collapse(GtkButton *b, gpointer user)
{
    App *a = user;
    const char *host = g_object_get_data(G_OBJECT(b), "host");
    site_collapse_toggle(a, host ? host : "");
    rebuild_list(a);
}

/* Confirm every linkgrabber link whose host matches, into the queue. */
static void on_site_confirm(GtkButton *b, gpointer user)
{
    App *a = user;
    const char *host = g_object_get_data(G_OBJECT(b), "host");
    if (!host) return;
    int n = 0;
    for (int i = 0; i < a->n; i++) {
        Dl *d = &a->items[i];
        if (!d->list || strcmp(d->list, "linkgrabber")) continue;
        char h[256]; host_of(d->url, h, sizeof h);
        if (strcmp(h, host)) continue;
        char cmd[128];
        snprintf(cmd, sizeof cmd,
                 "{\"cmd\":\"confirm\",\"id\":%lld,\"start\":true}", d->id);
        send_cmd(cmd);
        n++;
    }
    char msg[96];
    snprintf(msg, sizeof msg, "Confirmed %d link%s from %s", n,
             n == 1 ? "" : "s", site_label(host));
    toast(a, msg);
}

/* A site group header: favicon, friendly name, link count, collapse + confirm. */
static GtkWidget *make_site_header(App *a, const char *host, int count,
                                   int linkgrabber)
{
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(outer, 6);
    gtk_widget_set_margin_bottom(outer, 6);
    gtk_widget_set_margin_start(outer, 8);
    gtk_widget_set_margin_end(outer, 12);
    gtk_widget_add_css_class(outer, "toolbar");

    int collapsed = site_collapsed(a, host);
    GtkWidget *ct = gtk_button_new_from_icon_name(
        collapsed ? "pan-end-symbolic" : "pan-down-symbolic");
    gtk_widget_add_css_class(ct, "flat");
    gtk_widget_set_tooltip_text(ct, collapsed ? "Expand" : "Collapse");
    g_object_set_data_full(G_OBJECT(ct), "host", g_strdup(host), g_free);
    g_signal_connect(ct, "clicked", G_CALLBACK(on_site_collapse), a);
    gtk_box_append(GTK_BOX(outer), ct);

    gtk_box_append(GTK_BOX(outer), make_site_icon(host));

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vbox, TRUE);
    GtkWidget *name = gtk_label_new(site_label(host));
    gtk_label_set_xalign(GTK_LABEL(name), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(name, "heading");
    gtk_box_append(GTK_BOX(vbox), name);
    char sub[96];
    snprintf(sub, sizeof sub, "%s · %d link%s",
             host && *host ? host : "—", count, count == 1 ? "" : "s");
    GtkWidget *subl = gtk_label_new(sub);
    gtk_label_set_xalign(GTK_LABEL(subl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(subl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(subl, "dim-label");
    gtk_widget_add_css_class(subl, "caption");
    gtk_box_append(GTK_BOX(vbox), subl);
    gtk_box_append(GTK_BOX(outer), vbox);

    if (linkgrabber) {
        GtkWidget *confirm = gtk_button_new_from_icon_name("object-select-symbolic");
        gtk_widget_add_css_class(confirm, "flat");
        gtk_widget_set_valign(confirm, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(confirm, "Confirm all links from this site");
        g_object_set_data_full(G_OBJECT(confirm), "host", g_strdup(host), g_free);
        g_signal_connect(confirm, "clicked", G_CALLBACK(on_site_confirm), a);
        gtk_box_append(GTK_BOX(outer), confirm);
    }

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), outer);
    return row;
}

/* One download/linkgrabber link row, with all per-link queue actions. */
static GtkWidget *make_row(Dl *d, int linkgrabber)
{
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(outer, 8);
    gtk_widget_set_margin_bottom(outer, 8);
    gtk_widget_set_margin_start(outer, linkgrabber ? 12 : 24); /* indent under pkg */
    gtk_widget_set_margin_end(outer, 12);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(vbox, TRUE);

    GtkWidget *name = gtk_label_new(d->name ? d->name : "");
    gtk_label_set_xalign(GTK_LABEL(name), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(name, "heading");
    gtk_box_append(GTK_BOX(vbox), name);

    if (!linkgrabber) {
        GtkWidget *bar = gtk_progress_bar_new();
        double frac = d->total > 0 ? (double)d->downloaded / (double)d->total : 0.0;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), frac);
        if (d->state && !strcmp(d->state, "active"))
            gtk_widget_add_css_class(bar, "accent");
        gtk_box_append(GTK_BOX(vbox), bar);
    }

    /* flags string: priority + disabled/manual/forced/offline */
    char flags[96] = "";
    const char *pt = "";
    switch (d->priority) {
    case 3: pt = "Highest"; break; case 2: pt = "Higher"; break;
    case 1: pt = "High"; break; case -1: pt = "Low"; break;
    case -2: pt = "Lower"; break; case -3: pt = "Lowest"; break;
    }
    if (*pt) { strncat(flags, " · prio ", sizeof flags - strlen(flags) - 1);
               strncat(flags, pt, sizeof flags - strlen(flags) - 1); }
    if (!d->enabled) strncat(flags, " · disabled", sizeof flags - strlen(flags) - 1);
    if (!d->autostart) strncat(flags, " · manual", sizeof flags - strlen(flags) - 1);
    if (d->force) strncat(flags, " · forced", sizeof flags - strlen(flags) - 1);

    char db[24], tb[24], sb[24], meta[256];
    const char *state = d->state ? d->state : "?";
    human((double)d->downloaded, db, sizeof db);
    human((double)d->total, tb, sizeof tb);
    human(d->speed, sb, sizeof sb);
    if (linkgrabber) {
        const char *av = d->availability ? d->availability : "unknown";
        snprintf(meta, sizeof meta, "%s · %s%s", av,
                 d->total > 0 ? tb : "size unknown", flags);
    } else if (d->total > 0 && d->speed > 1 && d->downloaded < d->total) {
        long eta = (long)((double)(d->total - d->downloaded) / d->speed);
        snprintf(meta, sizeof meta, "%s · %s / %s · %s/s · ETA %ld:%02ld%s",
                 state, db, tb, sb, eta / 60, eta % 60, flags);
    } else {
        snprintf(meta, sizeof meta, "%s · %s / %s%s", state, db,
                 d->total > 0 ? tb : "?", flags);
    }
    GtkWidget *ml = gtk_label_new(meta);
    gtk_label_set_xalign(GTK_LABEL(ml), 0.0);
    gtk_widget_add_css_class(ml, "dim-label");
    gtk_widget_add_css_class(ml, "caption");
    gtk_box_append(GTK_BOX(vbox), ml);
    gtk_box_append(GTK_BOX(outer), vbox);

    /* right: quick buttons + ⋮ menu + remove */
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(btns, GTK_ALIGN_CENTER);
    long long id = d->id;

    GtkWidget *box, *mb;
    GtkPopover *pop;

    if (linkgrabber) {
        ActionCtx cf = ctx_make("confirm", id, 0); cf.startflag = 1;
        gtk_box_append(GTK_BOX(btns),
                       quick_btn("object-select-symbolic", "Confirm (start)", cf));
        mb = menu_button(&box, &pop);
        ActionCtx cn = ctx_make("confirm", id, 0); cn.startflag = 0;
        pop_add(box, pop, "Confirm (don't start)", cn);
        pop_sep(box);
        pop_add_move(box, pop, id, 0);
        gtk_box_append(GTK_BOX(btns), mb);
        ActionCtx rm = ctx_make("lg_remove", id, 0);
        gtk_box_append(GTK_BOX(btns),
                       quick_btn("user-trash-symbolic", "Remove from linkgrabber", rm));
    } else {
        if (d->state && !strcmp(d->state, "active")) {
            gtk_box_append(GTK_BOX(btns),
                quick_btn("media-playback-pause-symbolic", "Pause", ctx_make("pause", id, 0)));
        } else if (d->state && (!strcmp(d->state, "paused") || !strcmp(d->state, "error"))) {
            gtk_box_append(GTK_BOX(btns),
                quick_btn("media-playback-start-symbolic", "Resume", ctx_make("resume", id, 0)));
        } else if (d->state && !strcmp(d->state, "queued")) {
            gtk_box_append(GTK_BOX(btns),
                quick_btn("media-playback-start-symbolic", "Start now", ctx_make("force", id, 0)));
        }
        mb = menu_button(&box, &pop);
        pop_add(box, pop, "Start now (force)", ctx_make("force", id, 0));
        if (d->enabled) pop_add(box, pop, "Disable", ctx_make("disable", id, 0));
        else pop_add(box, pop, "Enable", ctx_make("enable", id, 0));
        { ActionCtx c = ctx_make("autostart", id, 0); c.onflag = d->autostart ? 0 : 1;
          pop_add(box, pop, d->autostart ? "Auto-download: off" : "Auto-download: on", c); }
        pop_sep(box);
        pop_add_priority(box, pop, id, 0);
        pop_sep(box);
        pop_add_move(box, pop, id, 0);
        gtk_box_append(GTK_BOX(btns), mb);
        gtk_box_append(GTK_BOX(btns),
                       quick_btn("user-trash-symbolic", "Remove", ctx_make("rm", id, 0)));
    }

    gtk_box_append(GTK_BOX(outer), btns);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), outer);
    return row;
}

/* A package header row with collapse + package-wide actions. */
static GtkWidget *make_pkg_header(App *a, Pkg *p, int linkgrabber)
{
    (void)a;
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(outer, 6);
    gtk_widget_set_margin_bottom(outer, 6);
    gtk_widget_set_margin_start(outer, 8);
    gtk_widget_set_margin_end(outer, 12);
    gtk_widget_add_css_class(outer, "toolbar");

    /* collapse/expand toggle */
    ActionCtx ce = ctx_make("pkg", p->id, 0);
    ce.collapsedflag = p->collapsed ? 0 : 1;
    gtk_box_append(GTK_BOX(outer),
        quick_btn(p->collapsed ? "pan-end-symbolic" : "pan-down-symbolic",
                  p->collapsed ? "Expand" : "Collapse", ce));

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vbox, TRUE);
    GtkWidget *name = gtk_label_new(p->name ? p->name : "package");
    gtk_label_set_xalign(GTK_LABEL(name), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(name, "heading");
    gtk_box_append(GTK_BOX(vbox), name);
    char sub[400];
    snprintf(sub, sizeof sub, "%d link%s%s%s", p->links, p->links == 1 ? "" : "s",
             p->folder ? " · " : "", p->folder ? p->folder : "");
    GtkWidget *subl = gtk_label_new(sub);
    gtk_label_set_xalign(GTK_LABEL(subl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(subl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(subl, "dim-label");
    gtk_widget_add_css_class(subl, "caption");
    gtk_box_append(GTK_BOX(vbox), subl);
    gtk_box_append(GTK_BOX(outer), vbox);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(btns, GTK_ALIGN_CENTER);
    long long id = p->id;

    if (linkgrabber) {
        ActionCtx cf = ctx_make("confirm", id, 1); cf.startflag = 1;
        gtk_box_append(GTK_BOX(btns),
                       quick_btn("object-select-symbolic", "Confirm package", cf));
    }

    GtkWidget *box;
    GtkPopover *pop;
    GtkWidget *mb = menu_button(&box, &pop);
    if (linkgrabber) {
        ActionCtx c1 = ctx_make("confirm", id, 1); c1.startflag = 1;
        pop_add(box, pop, "Confirm (start)", c1);
        ActionCtx c0 = ctx_make("confirm", id, 1); c0.startflag = 0;
        pop_add(box, pop, "Confirm (don't start)", c0);
        pop_sep(box);
        pop_add_move(box, pop, id, 1);
        pop_sep(box);
        /* Edit (name/folder/priority) via dialog */
        GtkWidget *edit = gtk_button_new_with_label("Edit package…");
        gtk_widget_add_css_class(edit, "flat");
        /* Heap-allocate the id so the full 64 bits survive; packing it into a
         * gpointer would truncate on 32-bit targets where pointers are 4 bytes. */
        gint64 *pidp = g_new(gint64, 1);
        *pidp = id;
        g_object_set_data_full(G_OBJECT(edit), "pid", pidp, g_free);
        g_signal_connect(edit, "clicked", G_CALLBACK(on_pkg_edit_clicked), &g_app);
        gtk_box_append(GTK_BOX(box), edit);
        pop_add(box, pop, "Remove package", ctx_make("lg_remove", id, 1));
    } else {
        pop_add(box, pop, "Start all (force)", ctx_make("force", id, 1));
        pop_add(box, pop, "Enable all", ctx_make("enable", id, 1));
        pop_add(box, pop, "Disable all", ctx_make("disable", id, 1));
        { ActionCtx con = ctx_make("autostart", id, 1); con.onflag = 1;
          pop_add(box, pop, "Auto-download: on", con);
          ActionCtx cof = ctx_make("autostart", id, 1); cof.onflag = 0;
          pop_add(box, pop, "Auto-download: off", cof); }
        pop_sep(box);
        pop_add_priority(box, pop, id, 1);
        pop_sep(box);
        pop_add_move(box, pop, id, 1);
        pop_sep(box);
        GtkWidget *edit = gtk_button_new_with_label("Edit package…");
        gtk_widget_add_css_class(edit, "flat");
        /* Heap-allocate the id so the full 64 bits survive; packing it into a
         * gpointer would truncate on 32-bit targets where pointers are 4 bytes. */
        gint64 *pidp = g_new(gint64, 1);
        *pidp = id;
        g_object_set_data_full(G_OBJECT(edit), "pid", pidp, g_free);
        g_signal_connect(edit, "clicked", G_CALLBACK(on_pkg_edit_clicked), &g_app);
        gtk_box_append(GTK_BOX(box), edit);
        pop_add(box, pop, "Remove package", ctx_make("rm", id, 1));
    }
    gtk_box_append(GTK_BOX(btns), mb);

    gtk_box_append(GTK_BOX(outer), btns);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), outer);
    return row;
}

/* True if `d` belongs to a package that lives in `view`. */
static int item_has_pkg(App *a, Dl *d, const char *view)
{
    for (int pi = 0; pi < a->npkg; pi++)
        if (a->pkgs[pi].id == d->package_id && a->pkgs[pi].list &&
            !strcmp(a->pkgs[pi].list, view)) return 1;
    return 0;
}

/* Host of a package = host of its first link in `view` (packages are
 * single-site in practice, since each grab stages one source URL). */
static void pkg_host(App *a, Pkg *p, const char *view, char *buf, size_t len)
{
    if (len) buf[0] = '\0';
    for (int i = 0; i < a->n; i++) {
        Dl *d = &a->items[i];
        if (d->package_id == p->id && d->list && !strcmp(d->list, view)) {
            host_of(d->url, buf, len);
            return;
        }
    }
}

/* Classic package grouping (used for the downloads view and "Packages" mode). */
static int render_pkg_view(App *a, GtkListBox *list, const char *view,
                           const char *filter, int linkgrabber)
{
    int shown = 0;
    /* packaged links (packages already arrive position-sorted) */
    for (int pi = 0; pi < a->npkg; pi++) {
        Pkg *p = &a->pkgs[pi];
        if (!p->list || strcmp(p->list, view)) continue;
        gtk_list_box_append(list, make_pkg_header(a, p, linkgrabber));
        shown++;
        if (p->collapsed) continue;
        for (int i = 0; i < a->n; i++) {
            Dl *d = &a->items[i];
            if (d->package_id != p->id || !d->list || strcmp(d->list, view)) continue;
            if (!linkgrabber && filter && (!d->state || strcmp(d->state, filter))) continue;
            gtk_list_box_append(list, make_row(d, linkgrabber));
        }
    }
    /* loose links (no package in this view) */
    for (int i = 0; i < a->n; i++) {
        Dl *d = &a->items[i];
        if (!d->list || strcmp(d->list, view)) continue;
        if (item_has_pkg(a, d, view)) continue;
        if (!linkgrabber && filter && (!d->state || strcmp(d->state, filter))) continue;
        gtk_list_box_append(list, make_row(d, linkgrabber));
        shown++;
    }
    return shown;
}

/* True if `d` passes the active state filter (downloads view only). */
static int item_passes(Dl *d, const char *filter, int linkgrabber)
{
    if (linkgrabber || !filter) return 1;
    return d->state && !strcmp(d->state, filter);
}

/* Site grouping for either view. When `nested`, packages are shown under their
 * site header; otherwise links are listed directly under the site. */
static int render_site_view(App *a, GtkListBox *list, const char *view,
                            const char *filter, int linkgrabber, int nested)
{
    /* unique hosts, in first-appearance order (only over visible links) */
    GPtrArray *hosts = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0; i < a->n; i++) {
        Dl *d = &a->items[i];
        if (!d->list || strcmp(d->list, view)) continue;
        if (!item_passes(d, filter, linkgrabber)) continue;
        char h[256]; host_of(d->url, h, sizeof h);
        int seen = 0;
        for (guint k = 0; k < hosts->len; k++)
            if (!strcmp(g_ptr_array_index(hosts, k), h)) { seen = 1; break; }
        if (!seen) g_ptr_array_add(hosts, g_strdup(h));
    }

    int shown = 0;
    for (guint hi = 0; hi < hosts->len; hi++) {
        const char *host = g_ptr_array_index(hosts, hi);
        int count = 0;
        for (int i = 0; i < a->n; i++) {
            Dl *d = &a->items[i];
            if (!d->list || strcmp(d->list, view)) continue;
            if (!item_passes(d, filter, linkgrabber)) continue;
            char h[256]; host_of(d->url, h, sizeof h);
            if (!strcmp(h, host)) count++;
        }
        gtk_list_box_append(list, make_site_header(a, host, count, linkgrabber));
        shown++;
        if (site_collapsed(a, host)) continue;

        if (nested) {
            /* packages of this host */
            for (int pi = 0; pi < a->npkg; pi++) {
                Pkg *p = &a->pkgs[pi];
                if (!p->list || strcmp(p->list, view)) continue;
                char ph[256]; pkg_host(a, p, view, ph, sizeof ph);
                if (strcmp(ph, host)) continue;
                gtk_list_box_append(list, make_pkg_header(a, p, linkgrabber));
                if (p->collapsed) continue;
                for (int i = 0; i < a->n; i++) {
                    Dl *d = &a->items[i];
                    if (d->package_id != p->id || !d->list || strcmp(d->list, view))
                        continue;
                    if (!item_passes(d, filter, linkgrabber)) continue;
                    gtk_list_box_append(list, make_row(d, linkgrabber));
                }
            }
            /* loose links of this host (no package) */
            for (int i = 0; i < a->n; i++) {
                Dl *d = &a->items[i];
                if (!d->list || strcmp(d->list, view)) continue;
                if (item_has_pkg(a, d, view)) continue;
                if (!item_passes(d, filter, linkgrabber)) continue;
                char h[256]; host_of(d->url, h, sizeof h);
                if (strcmp(h, host)) continue;
                gtk_list_box_append(list, make_row(d, linkgrabber));
            }
        } else {
            /* all links of this host, regardless of package */
            for (int i = 0; i < a->n; i++) {
                Dl *d = &a->items[i];
                if (!d->list || strcmp(d->list, view)) continue;
                if (!item_passes(d, filter, linkgrabber)) continue;
                char h[256]; host_of(d->url, h, sizeof h);
                if (strcmp(h, host)) continue;
                gtk_list_box_append(list, make_row(d, linkgrabber));
            }
        }
    }
    g_ptr_array_free(hosts, TRUE);
    return shown;
}

static void render_into(App *a, GtkListBox *list, const char *view,
                        const char *filter)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))))
        gtk_list_box_remove(list, child);

    int linkgrabber = !strcmp(view, "linkgrabber");
    int shown;
    if (a->group_mode != GROUP_PKG)
        shown = render_site_view(a, list, view, filter, linkgrabber,
                                 a->group_mode == GROUP_SITE_PKG);
    else
        shown = render_pkg_view(a, list, view, filter, linkgrabber);

    if (shown == 0) {
        GtkWidget *empty = adw_status_page_new();
        adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty),
            linkgrabber ? "edit-find-symbolic" : "folder-download-symbolic");
        adw_status_page_set_title(ADW_STATUS_PAGE(empty),
            linkgrabber ? "Linkgrabber empty" : "No downloads");
        adw_status_page_set_description(ADW_STATUS_PAGE(empty),
            linkgrabber ? "Use + → “Review in linkgrabber” to stage links."
                        : "Click + to add a URL or archive.org item.");
        GtkWidget *r = gtk_list_box_row_new();
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r), FALSE);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(r), empty);
        gtk_list_box_append(list, r);
    }
}

static void rebuild_list(App *a)
{
    /* Don't rebuild rows while a menu is open — it would dismiss the popover.
     * The status bar below still updates; the rows refresh once it closes. */
    if (a->open_menus > 0) {
        a->pending_rebuild = 1;
    } else {
        render_into(a, a->dlist, "download", current_filter(a));
        render_into(a, a->glist, "linkgrabber", NULL);
    }

    double total_speed = 0;
    int dl = 0, lg = 0;
    for (int i = 0; i < a->n; i++) {
        Dl *d = &a->items[i];
        if (d->list && !strcmp(d->list, "linkgrabber")) { lg++; continue; }
        dl++;
        if (d->state && !strcmp(d->state, "active")) total_speed += d->speed;
    }
    char sb[24], line[160];
    human(total_speed, sb, sizeof sb);
    snprintf(line, sizeof line, "%d download%s · %s/s · %s", dl,
             dl == 1 ? "" : "s", sb, a->autostart ? "running" : "stopped");
    if (lg) {
        char extra[40];
        snprintf(extra, sizeof extra, " · %d in linkgrabber", lg);
        strncat(line, extra, sizeof line - strlen(line) - 1);
    }
    gtk_label_set_text(a->status, line);
}

/* ---- snapshot parsing ------------------------------------------------- */

static void free_items(App *a)
{
    for (int i = 0; i < a->n; i++) {
        g_free(a->items[i].name);
        g_free(a->items[i].state);
        g_free(a->items[i].list);
        g_free(a->items[i].availability);
        g_free(a->items[i].url);
    }
    free(a->items);
    a->items = NULL;
    a->n = 0;
}

static void free_pkgs(App *a)
{
    for (int i = 0; i < a->npkg; i++) {
        g_free(a->pkgs[i].name);
        g_free(a->pkgs[i].folder);
        g_free(a->pkgs[i].list);
    }
    free(a->pkgs);
    a->pkgs = NULL;
    a->npkg = 0;
}

static void apply_packages(App *a, json_t *arr)
{
    free_pkgs(a);
    if (!json_is_array(arr)) return;
    a->npkg = (int)json_array_size(arr);
    a->pkgs = a->npkg ? calloc((size_t)a->npkg, sizeof(Pkg)) : NULL;
    if (!a->pkgs) { a->npkg = 0; return; }
    size_t i;
    json_t *o;
    json_array_foreach(arr, i, o) {
        Pkg *p = &a->pkgs[i];
        p->id = json_integer_value(json_object_get(o, "id"));
        p->name = g_strdup(json_string_value(json_object_get(o, "name")));
        p->folder = g_strdup(json_string_value(json_object_get(o, "folder")));
        p->list = g_strdup(json_string_value(json_object_get(o, "list")));
        p->priority = json_integer_value(json_object_get(o, "priority"));
        p->collapsed = json_is_true(json_object_get(o, "collapsed"));
        p->links = json_integer_value(json_object_get(o, "links"));
    }
}

static void apply_downloads(App *a, json_t *arr)
{
    free_items(a);
    if (!json_is_array(arr)) return;
    a->n = (int)json_array_size(arr);
    a->items = a->n ? calloc((size_t)a->n, sizeof(Dl)) : NULL;
    if (!a->items) { a->n = 0; return; }
    size_t i;
    json_t *o;
    json_array_foreach(arr, i, o) {
        Dl *d = &a->items[i];
        d->id = json_integer_value(json_object_get(o, "id"));
        const char *nm = json_string_value(json_object_get(o, "name"));
        const char *path = json_string_value(json_object_get(o, "out_path"));
        const char *base = nm && *nm ? nm : (path ? path : "");
        const char *slash = strrchr(base, '/');
        d->name = g_strdup(slash ? slash + 1 : base);
        d->state = g_strdup(json_string_value(json_object_get(o, "state")));
        d->total = json_integer_value(json_object_get(o, "total"));
        d->downloaded = json_integer_value(json_object_get(o, "downloaded"));
        d->speed = json_real_value(json_object_get(o, "speed"));
        d->package_id = json_integer_value(json_object_get(o, "package_id"));
        d->priority = json_integer_value(json_object_get(o, "priority"));
        d->enabled = json_is_true(json_object_get(o, "enabled"));
        d->autostart = json_is_true(json_object_get(o, "autostart"));
        d->force = json_is_true(json_object_get(o, "force"));
        d->list = g_strdup(json_string_value(json_object_get(o, "list")));
        d->availability = g_strdup(json_string_value(json_object_get(o, "availability")));
        d->url = g_strdup(json_string_value(json_object_get(o, "url")));
    }
}

static void handle_line(App *a, const char *line)
{
    json_error_t e;
    json_t *root = json_loads(line, 0, &e);
    if (!root) return;
    json_t *as = json_object_get(root, "autostart");
    if (json_is_boolean(as)) a->autostart = json_is_true(as) ? 1 : 0;
    json_t *ma = json_object_get(root, "max_active");
    if (json_is_integer(ma)) a->max_active = (int)json_integer_value(ma);
    json_t *ms = json_object_get(root, "max_speed");
    if (json_is_integer(ms)) a->max_speed = json_integer_value(ms);
    /* packages must be parsed before downloads so grouping is available */
    apply_packages(a, json_object_get(root, "packages"));
    json_t *dls = json_object_get(root, "downloads");
    if (json_is_array(dls)) { apply_downloads(a, dls); rebuild_list(a); }
    json_decref(root);
}

/* ---- subscribe stream ------------------------------------------------- */

static void schedule_reconnect(App *a);

static gboolean on_socket_readable(GIOChannel *src, GIOCondition cond, gpointer user)
{
    (void)src;
    App *a = user;
    if (cond & (G_IO_HUP | G_IO_ERR)) goto closed;

    char buf[8192];
    long got = dlm_sock_read((dlm_sock_t)a->sub_fd, buf, sizeof buf);
    if (got <= 0) goto closed;
    g_string_append_len(a->rx, buf, got);

    char *nl;
    while ((nl = memchr(a->rx->str, '\n', a->rx->len))) {
        *nl = '\0';
        handle_line(a, a->rx->str);
        size_t consumed = (size_t)(nl - a->rx->str) + 1;
        g_string_erase(a->rx, 0, consumed);
    }
    return G_SOURCE_CONTINUE;

closed:
    a->sub_source = 0;
    if (a->sub_fd >= 0) { dlm_client_close(a->sub_fd); a->sub_fd = -1; }
    schedule_reconnect(a);
    return G_SOURCE_REMOVE;
}

static void connect_subscribe(App *a)
{
    a->sub_fd = dlm_client_connect();
    if (a->sub_fd < 0) { schedule_reconnect(a); return; }
    const char *sub = "{\"cmd\":\"subscribe\"}\n";
    size_t slen = strlen(sub), off = 0;
    int werr = 0;
    while (off < slen) { /* full-write: a short write must not desync the stream */
        long w = dlm_sock_write((dlm_sock_t)a->sub_fd, sub + off, slen - off);
        if (w < 0) {
            if (dlm_sock_was_intr()) continue;
            werr = 1;
            break;
        }
        off += (size_t)w;
    }
    if (werr) {
        dlm_client_close(a->sub_fd);
        a->sub_fd = -1;
        schedule_reconnect(a);
        return;
    }
    g_string_truncate(a->rx, 0);
    a->sub_source = dlm_gui_add_socket_watch(a->sub_fd, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                             on_socket_readable, a);
}

static gboolean reconnect_cb(gpointer u)
{
    connect_subscribe((App *)u);
    return G_SOURCE_REMOVE;
}

static void schedule_reconnect(App *a)
{
    g_timeout_add_seconds(2, reconnect_cb, a);
}

/* ---- add dialog ------------------------------------------------------- */

/* Resolve a URL into tasks and enqueue each into `dir` on the daemon. Runs in
 * the click handler (a brief synchronous yt-dlp/IA resolve). */
static void do_add(const char *url, const char *dir)
{
    if (!url || !*url) return;
    if (!dir || !*dir) dir = ".";

    dlm_extract_result res;
    if (dlm_extract(url, &res) != DLM_OK || res.count == 0) {
        toast(&g_app, "Could not resolve URL");
        return;
    }
    g_mkdir_with_parents(dir, 0755);
    int added = 0;
    for (int i = 0; i < res.count; i++) {
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, res.tasks[i].filename);
        json_t *r = json_object();
        json_object_set_new(r, "cmd", json_string("add"));
        json_object_set_new(r, "url", json_string(res.tasks[i].url));
        json_object_set_new(r, "out", json_string(path));
        if (res.tasks[i].delegate) json_object_set_new(r, "delegate", json_true());
        char *s = json_dumps(r, JSON_COMPACT);
        json_decref(r);
        send_cmd(s);
        free(s);
        added++;
    }
    char msg[96];
    snprintf(msg, sizeof msg, "Added %d %s (%s) to %s", added,
             added == 1 ? "download" : "files", res.source, dir);
    toast(&g_app, msg);
    dlm_extract_result_free(&res);
}

/* If `text` (clipboard contents) starts with a supported URL scheme, return a
 * freshly-allocated, trimmed copy of the first whitespace-delimited token;
 * otherwise NULL. Keeps the heavy extractor off non-link clipboard contents. */
static char *detect_url(const char *text)
{
    if (!text) return NULL;
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
    static const char *const schemes[] = {
        "http://", "https://", "ftp://", "ftps://", "magnet:", NULL };
    int ok = 0;
    for (int i = 0; schemes[i]; i++)
        if (g_ascii_strncasecmp(text, schemes[i], strlen(schemes[i])) == 0) { ok = 1; break; }
    if (!ok) return NULL;
    size_t len = strcspn(text, " \t\r\n");  /* first token only */
    if (len == 0) return NULL;
    return g_strndup(text, len);
}

/* Resolve a URL and stage the tasks into the linkgrabber as one package. */
static void do_grab(const char *url, const char *dir)
{
    if (!url || !*url) return;
    if (!dir || !*dir) dir = ".";
    dlm_extract_result res;
    if (dlm_extract(url, &res) != DLM_OK || res.count == 0) {
        toast(&g_app, "Could not resolve URL");
        return;
    }
    /* package name: URL basename, else extractor source */
    char pkgname[256];
    const char *b = strrchr(url, '/');
    b = b ? b + 1 : url;
    size_t bl = strcspn(b, "?#");
    if (bl == 0 || bl >= sizeof pkgname) snprintf(pkgname, sizeof pkgname, "%s", res.source);
    else { memcpy(pkgname, b, bl); pkgname[bl] = '\0'; }

    json_t *r = json_object();
    json_object_set_new(r, "cmd", json_string("grab"));
    json_object_set_new(r, "name", json_string(pkgname));
    json_object_set_new(r, "folder", json_string(dir));
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
        if (t->delegate) json_object_set_new(l, "delegate", json_true());
        json_object_set_new(l, "availability",
                            json_string(t->size >= 0 ? "online" : "unknown"));
        json_array_append_new(links, l);
    }
    json_object_set_new(r, "links", links);
    char *s = json_dumps(r, JSON_COMPACT);
    json_decref(r);
    send_cmd(s);
    free(s);
    char msg[128];
    snprintf(msg, sizeof msg, "Staged %d %s (%s) in the linkgrabber", res.count,
             res.count == 1 ? "link" : "links", res.source);
    toast(&g_app, msg);
    dlm_extract_result_free(&res);
}

typedef struct { GtkEditable *url, *dir; GtkSwitch *grab; } AddEntries;

static void on_add_response(AdwAlertDialog *d, const char *response, gpointer user)
{
    (void)d;
    AddEntries *e = user;
    if (!g_strcmp0(response, "add")) {
        const char *dir = gtk_editable_get_text(e->dir);
        /* remember the chosen folder for next time */
        if (dir && *dir) { g_free(g_app.download_dir); g_app.download_dir = g_strdup(dir); }
        if (gtk_switch_get_active(e->grab))
            do_grab(gtk_editable_get_text(e->url), g_app.download_dir);
        else
            do_add(gtk_editable_get_text(e->url), g_app.download_dir);
    }
    g_free(e);
}

/* Clipboard read finished: if the dialog's URL field is still empty and the
 * clipboard holds a link, drop it in so the user can just hit Add. */
static void on_add_clip_ready(GObject *src, GAsyncResult *res, gpointer user)
{
    GtkEditable *url = user;            /* held with a ref until this fires */
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    char *link = detect_url(text);
    if (link && (!gtk_editable_get_text(url) || !*gtk_editable_get_text(url)))
        gtk_editable_set_text(url, link);
    g_free(link);
    g_free(text);
    g_object_unref(url);
}

static void on_add_clicked(GtkButton *b, gpointer user)
{
    (void)b;
    App *a = user;
    AdwDialog *d = adw_alert_dialog_new("Add download", NULL);
    adw_alert_dialog_set_body(ADW_ALERT_DIALOG(d),
        "Paste a direct URL, a yt-dlp-supported page/series, or an archive.org item.");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *url = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(url), "https://…");
    gtk_widget_set_size_request(url, 380, -1);
    /* auto-paste: prefill from the clipboard if it holds a link */
    GdkClipboard *clip = gtk_widget_get_clipboard(GTK_WIDGET(a->win));
    gdk_clipboard_read_text_async(clip, NULL, on_add_clip_ready,
                                  g_object_ref(GTK_EDITABLE(url)));
    GtkWidget *dir = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(dir), "Download folder");
    gtk_editable_set_text(GTK_EDITABLE(dir), a->download_dir);
    gtk_box_append(GTK_BOX(box), url);
    gtk_box_append(GTK_BOX(box), dir);

    /* review-in-linkgrabber toggle (JDownloader-style staging) */
    GtkWidget *grow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(grow, 4);
    GtkWidget *glabel = gtk_label_new("Review in linkgrabber first");
    gtk_widget_set_hexpand(glabel, TRUE);
    gtk_label_set_xalign(GTK_LABEL(glabel), 0.0);
    GtkWidget *gsw = gtk_switch_new();
    gtk_widget_set_valign(gsw, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(grow), glabel);
    gtk_box_append(GTK_BOX(grow), gsw);
    gtk_box_append(GTK_BOX(box), grow);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(d), box);

    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d),
        "cancel", "Cancel", "add", "Add", NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "add",
        ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "add");

    AddEntries *e = g_new0(AddEntries, 1);
    e->url = GTK_EDITABLE(url);
    e->dir = GTK_EDITABLE(dir);
    e->grab = GTK_SWITCH(gsw);
    g_signal_connect(d, "response", G_CALLBACK(on_add_response), e);
    adw_dialog_present(d, GTK_WIDGET(a->win));
}

/* ---- package edit dialog --------------------------------------------- */

typedef struct { long long id; GtkEditable *name, *folder; } PkgEntries;

static void on_pkg_edit_response(AdwAlertDialog *d, const char *response, gpointer user)
{
    (void)d;
    PkgEntries *e = user;
    if (!g_strcmp0(response, "save")) {
        json_t *r = json_object();
        json_object_set_new(r, "cmd", json_string("pkg"));
        json_object_set_new(r, "id", json_integer(e->id));
        const char *nm = gtk_editable_get_text(e->name);
        const char *fo = gtk_editable_get_text(e->folder);
        if (nm && *nm) json_object_set_new(r, "name", json_string(nm));
        if (fo && *fo) json_object_set_new(r, "folder", json_string(fo));
        char *s = json_dumps(r, JSON_COMPACT);
        json_decref(r);
        send_cmd(s);
        free(s);
        toast(&g_app, "Package updated");
    }
    g_free(e);
}

static void on_pkg_edit_clicked(GtkButton *b, gpointer user)
{
    App *a = user;
    const gint64 *pidp = g_object_get_data(G_OBJECT(b), "pid");
    long long id = pidp ? (long long)*pidp : 0;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));

    /* current values from the model */
    const char *cur_name = "", *cur_folder = "";
    for (int i = 0; i < a->npkg; i++)
        if (a->pkgs[i].id == id) {
            cur_name = a->pkgs[i].name ? a->pkgs[i].name : "";
            cur_folder = a->pkgs[i].folder ? a->pkgs[i].folder : "";
        }

    AdwDialog *d = adw_alert_dialog_new("Edit package", NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(name), "Package name");
    gtk_editable_set_text(GTK_EDITABLE(name), cur_name);
    GtkWidget *folder = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(folder), "Download folder");
    gtk_editable_set_text(GTK_EDITABLE(folder), cur_folder);
    gtk_box_append(GTK_BOX(box), name);
    gtk_box_append(GTK_BOX(box), folder);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(d), box);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d),
        "cancel", "Cancel", "save", "Save", NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "save",
        ADW_RESPONSE_SUGGESTED);

    PkgEntries *e = g_new0(PkgEntries, 1);
    e->id = id;
    e->name = GTK_EDITABLE(name);
    e->folder = GTK_EDITABLE(folder);
    g_signal_connect(d, "response", G_CALLBACK(on_pkg_edit_response), e);
    adw_dialog_present(d, GTK_WIDGET(a->win));
}

/* ---- archive.org account dialog -------------------------------------- */

/* Mirrors the CLI's ia-login: S3 keys, a pasted cookie, or email+password. */
typedef struct {
    GtkEditable *access, *secret, *cookie, *email, *password;
} IaEntries;

static void on_ia_response(AdwAlertDialog *d, const char *response, gpointer user)
{
    (void)d;
    IaEntries *e = user;
    if (!g_strcmp0(response, "save")) {
        const char *ak = gtk_editable_get_text(e->access);
        const char *sk = gtk_editable_get_text(e->secret);
        const char *ck = gtk_editable_get_text(e->cookie);
        const char *em = gtk_editable_get_text(e->email);
        const char *pw = gtk_editable_get_text(e->password);
        if (*ak && *sk) {
            dlm_ia_save_s3(ak, sk);
            toast(&g_app, "archive.org: signed in (S3 keys)");
        } else if (*em && *pw) {
            char *err = NULL;
            if (dlm_ia_login_password(em, pw, &err) == 0)
                toast(&g_app, "archive.org: signed in");
            else { toast(&g_app, err ? err : "login failed"); g_free(err); }
        } else if (*ck) {
            dlm_ia_save_cookie(ck);
            toast(&g_app, "archive.org: signed in (cookie)");
        } else {
            toast(&g_app, "Enter S3 keys, a cookie, or email + password");
        }
    } else if (!g_strcmp0(response, "logout")) {
        dlm_ia_logout();
        toast(&g_app, "archive.org: signed out");
    }
    g_free(e);
}

static GtkWidget *labeled(GtkWidget *box, const char *label)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_widget_add_css_class(l, "dim-label");
    gtk_widget_add_css_class(l, "caption");
    gtk_box_append(GTK_BOX(box), l);
    GtkWidget *e = gtk_entry_new();
    gtk_box_append(GTK_BOX(box), e);
    return e;
}

static void on_ia_clicked(GtkButton *b, gpointer user)
{
    (void)b;
    App *a = user;
    ia_credentials c;
    dlm_ia_load(&c);

    AdwDialog *d = adw_alert_dialog_new("Internet Archive account", NULL);
    char body[128];
    snprintf(body, sizeof body,
             "Current: %s.\nSign in with S3 keys (recommended), a cookie, or "
             "email + password.", dlm_ia_mode_str(c.mode));
    adw_alert_dialog_set_body(ADW_ALERT_DIALOG(d), body);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *access = labeled(box, "S3 access key");
    GtkWidget *secret = labeled(box, "S3 secret key");
    gtk_entry_set_visibility(GTK_ENTRY(secret), FALSE);
    GtkWidget *cookie = labeled(box, "or session cookie");
    GtkWidget *email = labeled(box, "or email");
    GtkWidget *password = labeled(box, "and password");
    gtk_entry_set_visibility(GTK_ENTRY(password), FALSE);
    if (c.mode == IA_AUTH_S3 && c.access)
        gtk_editable_set_text(GTK_EDITABLE(access), c.access);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(d), box);

    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d),
        "cancel", "Cancel", "logout", "Sign out", "save", "Sign in", NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "save",
        ADW_RESPONSE_SUGGESTED);

    IaEntries *e = g_new0(IaEntries, 1);
    e->access = GTK_EDITABLE(access);
    e->secret = GTK_EDITABLE(secret);
    e->cookie = GTK_EDITABLE(cookie);
    e->email = GTK_EDITABLE(email);
    e->password = GTK_EDITABLE(password);
    g_signal_connect(d, "response", G_CALLBACK(on_ia_response), e);
    adw_dialog_present(d, GTK_WIDGET(a->win));
    dlm_ia_credentials_free(&c);
}

static void on_filter_changed(GtkDropDown *dd, GParamSpec *ps, gpointer user)
{
    (void)dd; (void)ps;
    rebuild_list((App *)user);
}

/* linkgrabber grouping: 0=Site+packages, 1=Site, 2=Packages (see enum). */
static void on_group_changed(GtkDropDown *dd, GParamSpec *ps, gpointer user)
{
    (void)ps;
    App *a = user;
    a->group_mode = (int)gtk_drop_down_get_selected(dd);
    rebuild_list(a);
}

/* ---- settings (max concurrent + global speed limit) ------------------ */

typedef struct { GtkEditable *jobs, *limit; GtkWidget *auto_tools; } SettingsEntries;

static void on_settings_response(AdwAlertDialog *d, const char *response, gpointer user)
{
    (void)d;
    SettingsEntries *e = user;
    if (!g_strcmp0(response, "save")) {
        const char *jt = gtk_editable_get_text(e->jobs);
        const char *lt = gtk_editable_get_text(e->limit);
        json_t *r = json_object();
        json_object_set_new(r, "cmd", json_string("set"));
        if (jt && *jt) json_object_set_new(r, "max_active", json_integer(atoi(jt)));
        /* empty / "off" / "0" clears the cap */
        int64_t bps = (lt && *lt && g_strcmp0(lt, "off")) ? dlm_parse_rate(lt) : 0;
        json_object_set_new(r, "max_speed", json_integer(bps));
        json_object_set_new(r, "auto_tools",
            json_boolean(gtk_switch_get_active(GTK_SWITCH(e->auto_tools))));
        char *s = json_dumps(r, JSON_COMPACT);
        json_decref(r);
        send_cmd(s);
        free(s);
        char msg[64], rb[32];
        dlm_format_rate(bps, rb, sizeof rb);
        snprintf(msg, sizeof msg, "Speed limit: %s", rb);
        toast(&g_app, msg);
    }
    g_free(e);
}

/* Send a one-shot tools_update request to the daemon. */
static void on_tools_update_clicked(GtkButton *b, gpointer user)
{
    (void)b; (void)user;
    send_cmd("{\"cmd\":\"tools_update\"}");
    toast(&g_app, "Checking yt-dlp for updates\xE2\x80\xA6");
}

/* Query the daemon's current settings, including auto-tools state and the
 * installed yt-dlp version (ytver may be set to "" if unknown). */
static void query_settings(int *max_active, gint64 *max_speed, int *auto_tools,
                           char *ytver, size_t verlen)
{
    *max_active = 0;
    *max_speed = 0;
    *auto_tools = 1;
    if (ytver && verlen) ytver[0] = '\0';
    int fd = dlm_client_connect();
    if (fd < 0) return;
    char *resp = dlm_client_rpc(fd, "{\"cmd\":\"list\"}");
    dlm_client_close(fd);
    if (!resp) return;
    json_error_t e;
    json_t *root = json_loads(resp, 0, &e);
    free(resp);
    if (!root) return;
    *max_active = (int)json_integer_value(json_object_get(root, "max_active"));
    *max_speed = json_integer_value(json_object_get(root, "max_speed"));
    json_t *tools = json_object_get(root, "tools");
    if (json_is_object(tools)) {
        json_t *au = json_object_get(tools, "auto");
        if (json_is_boolean(au)) *auto_tools = json_is_true(au) ? 1 : 0;
        json_t *yt = json_object_get(tools, "yt-dlp");
        const char *v = yt ? json_string_value(json_object_get(yt, "version")) : NULL;
        if (v && ytver && verlen) snprintf(ytver, verlen, "%s", v);
    }
    json_decref(root);
}

static void on_settings_clicked(GtkButton *b, gpointer user)
{
    (void)b;
    App *a = user;
    AdwDialog *d = adw_alert_dialog_new("Settings", NULL);

    int cur_jobs = 0, cur_auto = 1;
    gint64 cur_speed = 0;
    char ytver[64] = "";
    query_settings(&cur_jobs, &cur_speed, &cur_auto, ytver, sizeof ytver);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *jobs = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(jobs), "Max concurrent downloads (e.g. 3)");
    GtkWidget *limit = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(limit), "Speed limit (e.g. 2M, 500k, off)");
    /* prefill with the daemon's current settings */
    if (cur_jobs > 0) {
        char jb[16];
        snprintf(jb, sizeof jb, "%d", cur_jobs);
        gtk_editable_set_text(GTK_EDITABLE(jobs), jb);
    }
    if (cur_speed > 0) {
        char lb[24];
        snprintf(lb, sizeof lb, "%lld", (long long)cur_speed);
        gtk_editable_set_text(GTK_EDITABLE(limit), lb);
    }
    gtk_box_append(GTK_BOX(box), jobs);
    gtk_box_append(GTK_BOX(box), limit);

    /* --- auto-managed tools row --- */
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    GtkWidget *trow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *tlabel = gtk_label_new("Auto-download & update yt-dlp / ffmpeg");
    gtk_widget_set_hexpand(tlabel, TRUE);
    gtk_label_set_xalign(GTK_LABEL(tlabel), 0.0);
    GtkWidget *autosw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(autosw), cur_auto);
    gtk_widget_set_valign(autosw, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(trow), tlabel);
    gtk_box_append(GTK_BOX(trow), autosw);
    gtk_box_append(GTK_BOX(box), trow);

    GtkWidget *vrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    char vbuf[96];
    snprintf(vbuf, sizeof vbuf, "yt-dlp version: %s", ytver[0] ? ytver : "(not installed)");
    GtkWidget *vlabel = gtk_label_new(vbuf);
    gtk_widget_set_hexpand(vlabel, TRUE);
    gtk_label_set_xalign(GTK_LABEL(vlabel), 0.0);
    GtkWidget *upd = gtk_button_new_with_label("Update now");
    g_signal_connect(upd, "clicked", G_CALLBACK(on_tools_update_clicked), a);
    gtk_box_append(GTK_BOX(vrow), vlabel);
    gtk_box_append(GTK_BOX(vrow), upd);
    gtk_box_append(GTK_BOX(box), vrow);

    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(d), box);

    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d),
        "cancel", "Cancel", "save", "Apply", NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "save",
        ADW_RESPONSE_SUGGESTED);

    SettingsEntries *e = g_new0(SettingsEntries, 1);
    e->jobs = GTK_EDITABLE(jobs);
    e->limit = GTK_EDITABLE(limit);
    e->auto_tools = autosw;
    g_signal_connect(d, "response", G_CALLBACK(on_settings_response), e);
    adw_dialog_present(d, GTK_WIDGET(a->win));
}

/* ---- global queue controls ------------------------------------------- */

static void on_global_start(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    send_cmd("{\"cmd\":\"set\",\"autostart\":true}");
    toast(&g_app, "Downloads running");
}
static void on_global_stop(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    send_cmd("{\"cmd\":\"set\",\"autostart\":false}");
    toast(&g_app, "Stopping after current downloads");
}
static void on_clear_finished(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    send_cmd("{\"cmd\":\"clear_finished\"}");
}
static void on_confirm_all(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    send_cmd("{\"cmd\":\"confirm\",\"start\":true}");
    toast(&g_app, "Linkgrabber confirmed into the queue");
}

/* ---- clipboard monitor (JDownloader-style auto-paste) ---------------- */

/* Clipboard read finished while monitoring: stage any newly-copied link. */
static void on_monitor_clip_ready(GObject *src, GAsyncResult *res, gpointer user)
{
    App *a = user;
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    char *link = detect_url(text);
    /* skip non-links and the same URL fired twice (some apps re-set on focus) */
    if (link && g_strcmp0(link, a->clip_last) != 0) {
        g_free(a->clip_last);
        a->clip_last = g_strdup(link);
        do_grab(link, a->download_dir);
    }
    g_free(link);
    g_free(text);
}

static void on_clip_changed(GdkClipboard *clip, gpointer user)
{
    App *a = user;
    gdk_clipboard_read_text_async(clip, NULL, on_monitor_clip_ready, a);
}

static void on_clip_toggle(GtkToggleButton *btn, gpointer user)
{
    App *a = user;
    if (!a->clip) a->clip = gtk_widget_get_clipboard(GTK_WIDGET(a->win));
    if (gtk_toggle_button_get_active(btn)) {
        if (!a->clip_handler)
            a->clip_handler = g_signal_connect(a->clip, "changed",
                                               G_CALLBACK(on_clip_changed), a);
        toast(a, "Auto-paste on: copied links go to the linkgrabber");
    } else {
        if (a->clip_handler) {
            g_signal_handler_disconnect(a->clip, a->clip_handler);
            a->clip_handler = 0;
        }
        toast(a, "Auto-paste off");
    }
}

/* ---- activate / main -------------------------------------------------- */

/* A boxed list box inside a scroller (one per view). */
static GtkWidget *make_scrolled_list(GtkListBox **out)
{
    GtkWidget *list = gtk_list_box_new();
    *out = GTK_LIST_BOX(list);
    gtk_list_box_set_selection_mode(*out, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    gtk_widget_set_valign(list, GTK_ALIGN_START);
    gtk_widget_set_margin_top(list, 12);
    gtk_widget_set_margin_bottom(list, 12);
    gtk_widget_set_margin_start(list, 12);
    gtk_widget_set_margin_end(list, 12);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
    gtk_widget_set_vexpand(scroll, TRUE);
    return scroll;
}

static void on_activate(GtkApplication *app, gpointer user)
{
    App *a = user;

    GtkWidget *win = adw_application_window_new(app);
    a->win = GTK_WINDOW(win);
    gtk_window_set_title(a->win, "dlm");
    gtk_window_set_default_size(a->win, 960, 600);

    GtkWidget *header = adw_header_bar_new();

    GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(add, "Add download (or stage in linkgrabber)");
    gtk_widget_add_css_class(add, "suggested-action");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_clicked), a);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), add);

    /* Start / Stop the queue (global autostart) + clear finished */
    GtkWidget *start = gtk_button_new_from_icon_name("media-playback-start-symbolic");
    gtk_widget_set_tooltip_text(start, "Start downloads");
    g_signal_connect(start, "clicked", G_CALLBACK(on_global_start), a);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), start);
    GtkWidget *stop = gtk_button_new_from_icon_name("media-playback-stop-symbolic");
    gtk_widget_set_tooltip_text(stop, "Stop after current downloads");
    g_signal_connect(stop, "clicked", G_CALLBACK(on_global_stop), a);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), stop);

    /* centre: switch between the download list and the linkgrabber */
    GtkWidget *stack = adw_view_stack_new();
    a->stack = ADW_VIEW_STACK(stack);
    GtkWidget *dl_view = make_scrolled_list(&a->dlist);
    GtkWidget *lg_view = make_scrolled_list(&a->glist);
    adw_view_stack_add_titled_with_icon(a->stack, dl_view, "downloads",
                                        "Downloads", "folder-download-symbolic");
    adw_view_stack_add_titled_with_icon(a->stack, lg_view, "linkgrabber",
                                        "Linkgrabber", "edit-find-symbolic");
    GtkWidget *switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), a->stack);
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), switcher);

    const char *const filters[] = {"All", "Active", "Queued", "Done", NULL};
    GtkWidget *filter = gtk_drop_down_new_from_strings(filters);
    a->filter = GTK_DROP_DOWN(filter);
    g_signal_connect(filter, "notify::selected", G_CALLBACK(on_filter_changed), a);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), filter);

    /* grouping mode (order must match the GROUP_* enum) */
    const char *const groups[] = {"Site + packages", "Site", "Packages", NULL};
    GtkWidget *group = gtk_drop_down_new_from_strings(groups);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(group), a->group_mode);
    gtk_widget_set_tooltip_text(group, "Group downloads & linkgrabber by site or package");
    g_signal_connect(group, "notify::selected", G_CALLBACK(on_group_changed), a);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), group);

    GtkWidget *clear = gtk_button_new_from_icon_name("edit-clear-all-symbolic");
    gtk_widget_set_tooltip_text(clear, "Clear finished downloads");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_finished), a);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), clear);

    GtkWidget *confirm = gtk_button_new_from_icon_name("object-select-symbolic");
    gtk_widget_set_tooltip_text(confirm, "Confirm all linkgrabber links into the queue");
    g_signal_connect(confirm, "clicked", G_CALLBACK(on_confirm_all), a);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), confirm);

    /* auto-paste monitor: copied links are staged into the linkgrabber */
    GtkWidget *clipmon = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(clipmon), "edit-paste-symbolic");
    gtk_widget_set_tooltip_text(clipmon,
        "Auto-paste: watch the clipboard and stage copied links in the linkgrabber");
    g_signal_connect(clipmon, "toggled", G_CALLBACK(on_clip_toggle), a);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), clipmon);

    GtkWidget *ia = gtk_button_new_from_icon_name("avatar-default-symbolic");
    gtk_widget_set_tooltip_text(ia, "archive.org account");
    g_signal_connect(ia, "clicked", G_CALLBACK(on_ia_clicked), a);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), ia);

    GtkWidget *settings = gtk_button_new_from_icon_name("emblem-system-symbolic");
    gtk_widget_set_tooltip_text(settings, "Settings (concurrency, speed limit)");
    g_signal_connect(settings, "clicked", G_CALLBACK(on_settings_clicked), a);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), settings);

    GtkWidget *toast_overlay = adw_toast_overlay_new();
    a->toast = ADW_TOAST_OVERLAY(toast_overlay);
    adw_toast_overlay_set_child(a->toast, stack);

    /* status bar */
    GtkWidget *statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(statusbar, 4);
    gtk_widget_set_margin_bottom(statusbar, 4);
    gtk_widget_set_margin_start(statusbar, 12);
    gtk_widget_set_margin_end(statusbar, 12);
    GtkWidget *status = gtk_label_new("Connecting…");
    a->status = GTK_LABEL(status);
    gtk_widget_add_css_class(status, "dim-label");
    gtk_box_append(GTK_BOX(statusbar), status);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(content), toast_overlay);
    gtk_box_append(GTK_BOX(content), statusbar);

    GtkWidget *tv = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), content);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), tv);

    gtk_window_present(a->win);
    connect_subscribe(a);
}

int main(int argc, char **argv)
{
    memset(&g_app, 0, sizeof g_app);
    g_app.sub_fd = -1;
    g_app.rx = g_string_new(NULL);

    /* default download folder: $DLM_DOWNLOAD_DIR, else XDG Downloads, else ~ */
    const char *envd = g_getenv("DLM_DOWNLOAD_DIR");
    if (envd && *envd)
        g_app.download_dir = g_strdup(envd);
    else {
        const char *xdg = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
        g_app.download_dir = g_strdup(xdg ? xdg : g_get_home_dir());
    }

    g_app.app = adw_application_new("org.dlm.Gui", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(g_app.app, "activate", G_CALLBACK(on_activate), &g_app);
    int status = g_application_run(G_APPLICATION(g_app.app), argc, argv);
    g_object_unref(g_app.app);
    g_string_free(g_app.rx, TRUE);
    g_free(g_app.clip_last);
    for (int i = 0; i < g_app.ncsites; i++) g_free(g_app.csites[i]);
    g_free(g_app.csites);
    free_items(&g_app);
    free_pkgs(&g_app);
    return status;
}
