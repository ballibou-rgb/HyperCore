#include "app.h"
#include "version.h"
#include <glib/gstdio.h>
#include <string.h>

/* ============================================================
 * Projektverwaltung:
 *  - Projekt-Baum (links), gesperrt auf den Projekt-Root
 *  - projektname.hcs (Basis-Infos der IDE)
 *  - Datei-Generator (Hello-World-Templates inkl. build.sh)
 *  - Session (.session): offene Tabs merken/wiederherstellen
 * ============================================================ */

/* ---------- eigenes GObject für Baum-Einträge ---------- */
#define HC_TYPE_FILE_ITEM (hc_file_item_get_type())
G_DECLARE_FINAL_TYPE(HcFileItem, hc_file_item, HC, FILE_ITEM, GObject)
struct _HcFileItem { GObject parent; char *path; char *name; gboolean is_dir; };
G_DEFINE_FINAL_TYPE(HcFileItem, hc_file_item, G_TYPE_OBJECT)
static void hc_file_item_finalize(GObject *o) {
    HcFileItem *i = HC_FILE_ITEM(o);
    g_free(i->path); g_free(i->name);
    G_OBJECT_CLASS(hc_file_item_parent_class)->finalize(o);
}
static void hc_file_item_class_init(HcFileItemClass *k) {
    G_OBJECT_CLASS(k)->finalize = hc_file_item_finalize;
}
static void hc_file_item_init(HcFileItem *i) { (void)i; }
static HcFileItem *hc_file_item_new(const char *path, const char *name, gboolean dir) {
    HcFileItem *i = g_object_new(HC_TYPE_FILE_ITEM, NULL);
    i->path = g_strdup(path); i->name = g_strdup(name); i->is_dir = dir;
    return i;
}

/* Verzeichnis einlesen -> sortiertes GListStore (Ordner zuerst). */
static int cmp_items(gconstpointer a, gconstpointer b, gpointer u) {
    (void)u;
    HcFileItem *x = *(HcFileItem**)a, *y = *(HcFileItem**)b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return g_ascii_strcasecmp(x->name, y->name);
}

/* ---- Whitelist-Cache (Tresor): nur .hcs-gelistete Dateien sichtbar ---- */
static GHashTable *s_wl = NULL;      /* rel. Pfad -> 1                     */
static char       *s_wl_root = NULL; /* Projekt-Root zum Kürzen            */

static gboolean wl_has_file(const char *rel)
{
    return s_wl && g_hash_table_contains(s_wl, rel);
}
/* Enthält das Verzeichnis (rel) mindestens eine gelistete Datei? */
static gboolean wl_has_prefix(const char *reldir)
{
    if (!s_wl) return FALSE;
    char *pref = g_strconcat(reldir, "/", NULL);
    GHashTableIter it; gpointer k;
    g_hash_table_iter_init(&it, s_wl);
    gboolean found = FALSE;
    while (g_hash_table_iter_next(&it, &k, NULL)) {
        if (g_str_has_prefix((char*)k, pref)) { found = TRUE; break; }
    }
    g_free(pref);
    return found;
}
static char *rel_of(const char *full)
{
    if (s_wl_root && g_str_has_prefix(full, s_wl_root)) {
        const char *r = full + strlen(s_wl_root);
        while (*r == '/') r++;
        return g_strdup(r);
    }
    return g_path_get_basename(full);
}

static GListModel *dir_to_model(const char *dirpath)
{
    GDir *d = g_dir_open(dirpath, 0, NULL);
    if (!d) return NULL;
    GPtrArray *arr = g_ptr_array_new();
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (name[0] == '.') continue;  /* versteckte Dateien ausblenden  */
        char *full = g_build_filename(dirpath, name, NULL);
        gboolean isdir = g_file_test(full, G_FILE_TEST_IS_DIR);
        char *rel = rel_of(full);
        /* Tresor-Prinzip: Datei nur, wenn in Whitelist; Ordner nur, wenn
         * er mindestens eine gelistete Datei enthält. */
        gboolean show = isdir ? wl_has_prefix(rel) : wl_has_file(rel);
        if (show)
            g_ptr_array_add(arr, hc_file_item_new(full, name, isdir));
        g_free(rel);
        g_free(full);
    }
    g_dir_close(d);
    g_ptr_array_sort_with_data(arr, cmp_items, NULL);

    GListStore *store = g_list_store_new(HC_TYPE_FILE_ITEM);
    for (guint i = 0; i < arr->len; i++) {
        g_list_store_append(store, arr->pdata[i]);
        g_object_unref(arr->pdata[i]);
    }
    g_ptr_array_free(arr, TRUE);
    return G_LIST_MODEL(store);
}

/* Kinder-Modell für TreeListModel (nur für Ordner). */
static GListModel *tree_create_child(gpointer item, gpointer user_data)
{
    (void)user_data;
    HcFileItem *fi = HC_FILE_ITEM(item);
    if (!fi->is_dir) return NULL;
    return dir_to_model(fi->path);
}

/* ---- ListView-Factory ---- */
/* ============================================================
 * Rechtsklick-Kontextmenü im Projektbaum
 * ============================================================ */

/* Kontext der aktuell rechts angeklickten Zeile. Wird im Popover-Callback
 * ausgewertet und danach sofort wieder genullt. */
typedef struct {
    App     *app;
    char    *path;     /* absoluter Pfad der angeklickten Datei/Ordner */
    gboolean is_dir;
} TreeCtxData;

static void tree_ctx_free(gpointer p)
{
    TreeCtxData *ctx = p;
    g_free(ctx->path);
    g_free(ctx);
}

/* ------------------------------------------------------------------
 * GTK4-native Hilfsfunktionen (kein gtk_dialog_run, kein GtkMessageDialog)
 * Alle Dialoge sind GtkWindow-basiert mit Signalen statt synchronem run().
 * ------------------------------------------------------------------ */

/* Zeigt eine einfache Fehlermeldung (GtkAlertDialog, GTK 4.10+). */
static void show_error(GtkWidget *anchor, const char *msg)
{
    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", msg);
    gtk_alert_dialog_show(dlg, GTK_WINDOW(gtk_widget_get_root(anchor)));
    g_object_unref(dlg);
}

/* ------------------------------------------------------------------
 * Eingabe-Dialog: GTK4-Fenster mit Entry + OK/Abbrechen.
 * Asynchron: Ergebnis kommt per Callback cb(name_or_NULL, ud).
 * ------------------------------------------------------------------ */
typedef void (*AskNameCb)(const char *name, gpointer ud);

typedef struct {
    GtkWidget  *win;
    GtkWidget  *entry;
    AskNameCb   cb;
    gpointer    ud;
} AskCtx;

static void ask_ok_clicked(GtkWidget *btn, gpointer p)
{
    (void)btn;
    AskCtx *a = p;
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(a->entry));
    char *result = (txt && *txt) ? g_strdup(txt) : NULL;
    gtk_window_destroy(GTK_WINDOW(a->win));
    a->cb(result, a->ud);
    g_free(result);
    g_free(a);
}

static void ask_cancel_clicked(GtkWidget *btn, gpointer p)
{
    (void)btn;
    AskCtx *a = p;
    gtk_window_destroy(GTK_WINDOW(a->win));
    a->cb(NULL, a->ud);
    g_free(a);
}

static void ask_name_async(GtkWidget *anchor, const char *title,
                           const char *placeholder,
                           AskNameCb cb, gpointer ud)
{
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(win),
        GTK_WINDOW(gtk_widget_get_root(anchor)));
    gtk_window_set_default_size(GTK_WINDOW(win), 320, -1);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 14);
    gtk_widget_set_margin_top(box, 14);
    gtk_widget_set_margin_bottom(box, 14);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_box_append(GTK_BOX(box), entry);

    GtkWidget *btnbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btnbox, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_mnemonic("_Abbrechen");
    GtkWidget *ok     = gtk_button_new_with_mnemonic("_OK");
    gtk_widget_add_css_class(ok, "suggested-action");
    gtk_box_append(GTK_BOX(btnbox), cancel);
    gtk_box_append(GTK_BOX(btnbox), ok);
    gtk_box_append(GTK_BOX(box), btnbox);

    gtk_window_set_child(GTK_WINDOW(win), box);

    AskCtx *a = g_new0(AskCtx, 1);
    a->win   = win;
    a->entry = entry;
    a->cb    = cb;
    a->ud    = ud;

    g_signal_connect(ok,     "clicked", G_CALLBACK(ask_ok_clicked),     a);
    g_signal_connect(cancel, "clicked", G_CALLBACK(ask_cancel_clicked),  a);
    /* Enter im Entry = OK */
    g_signal_connect_swapped(entry, "activate",
                             G_CALLBACK(ask_ok_clicked), a);

    gtk_window_present(GTK_WINDOW(win));
}

/* ------------------------------------------------------------------
 * Bestätigungs-Dialog: GtkAlertDialog (GTK 4.10).
 * Asynchron: Ergebnis per GAsyncReadyCallback.
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------
 * Kontextmenü-Aktionen (alle asynchron via ask_name_async)
 * ------------------------------------------------------------------ */

static char *parent_dir_of(const char *path, gboolean is_dir)
{
    if (is_dir) return g_strdup(path);
    return g_path_get_dirname(path);
}

/* ---- Neue Datei ---- */
typedef struct { App *app; char *dir; GtkWidget *anchor; } NewFileCtx;

static void new_file_got_name(const char *name, gpointer ud)
{
    NewFileCtx *c = ud;
    if (!name) { g_free(c->dir); g_free(c); return; }

    if (strchr(name, '/') || strchr(name, '\\')) {
        show_error(c->anchor, "Der Dateiname darf keine Pfadtrenner enthalten.");
        g_free(c->dir); g_free(c); return;
    }
    char *full = g_build_filename(c->dir, name, NULL);
    if (g_file_test(full, G_FILE_TEST_EXISTS)) {
        show_error(c->anchor, "Eine Datei mit diesem Namen existiert bereits.");
        g_free(full); g_free(c->dir); g_free(c); return;
    }
    g_file_set_contents(full, "", 0, NULL);
    char *rel = rel_of(full);
    project_whitelist_add(c->app, rel);
    g_free(rel);
    editor_open_file(c->app, full);
    g_free(full);
    g_free(c->dir);
    g_free(c);
}

static void ctx_new_file(GtkWidget *btn, gpointer ud)
{
    TreeCtxData *ctx = ud;
    NewFileCtx *c = g_new0(NewFileCtx, 1);
    c->app    = ctx->app;
    c->dir    = parent_dir_of(ctx->path, ctx->is_dir);
    c->anchor = btn;
    ask_name_async(btn, "Neue Datei", "dateiname.c", new_file_got_name, c);
}

/* ---- Neuer Ordner ---- */
typedef struct { App *app; char *dir; GtkWidget *anchor; } NewFolderCtx;

static void new_folder_got_name(const char *name, gpointer ud)
{
    NewFolderCtx *c = ud;
    if (!name) { g_free(c->dir); g_free(c); return; }

    if (strchr(name, '/') || strchr(name, '\\')) {
        show_error(c->anchor, "Der Ordnername darf keine Pfadtrenner enthalten.");
        g_free(c->dir); g_free(c); return;
    }
    char *full = g_build_filename(c->dir, name, NULL);
    if (g_mkdir_with_parents(full, 0755) != 0)
        show_error(c->anchor, "Ordner konnte nicht angelegt werden.");
    else
        project_refresh_tree(c->app);
    g_free(full);
    g_free(c->dir);
    g_free(c);
}

static void ctx_new_folder(GtkWidget *btn, gpointer ud)
{
    TreeCtxData *ctx = ud;
    NewFolderCtx *c = g_new0(NewFolderCtx, 1);
    c->app    = ctx->app;
    c->dir    = parent_dir_of(ctx->path, ctx->is_dir);
    c->anchor = btn;
    ask_name_async(btn, "Neuer Ordner", "ordnername", new_folder_got_name, c);
}

/* ---- Umbenennen ---- */
typedef struct { App *app; char *old_path; gboolean is_dir; GtkWidget *anchor; } RenameCtx;

static void rename_got_name(const char *name, gpointer ud)
{
    RenameCtx *c = ud;
    if (!name) { g_free(c->old_path); g_free(c); return; }

    char *parent = g_path_get_dirname(c->old_path);
    char *new_full = g_build_filename(parent, name, NULL);
    g_free(parent);

    if (g_file_test(new_full, G_FILE_TEST_EXISTS)) {
        show_error(c->anchor, "Ein Eintrag mit diesem Namen existiert bereits.");
        g_free(new_full); g_free(c->old_path); g_free(c); return;
    }
    if (g_rename(c->old_path, new_full) != 0) {
        show_error(c->anchor, "Umbenennen fehlgeschlagen.");
        g_free(new_full); g_free(c->old_path); g_free(c); return;
    }
    if (!c->is_dir) {
        char *old_rel = rel_of(c->old_path);
        char *new_rel = rel_of(new_full);
        project_whitelist_remove(c->app, old_rel);
        project_whitelist_add(c->app, new_rel);
        g_free(old_rel); g_free(new_rel);
    } else {
        char *old_rel = rel_of(c->old_path);
        char *new_rel = rel_of(new_full);
        char *old_pref = g_strconcat(old_rel, "/", NULL);
        GList *wl = project_whitelist(c->app);
        for (GList *l = wl; l; l = l->next) {
            if (g_str_has_prefix((char*)l->data, old_pref)) {
                char *ne = g_strconcat(new_rel, "/",
                    (char*)l->data + strlen(old_pref), NULL);
                project_whitelist_remove(c->app, l->data);
                project_whitelist_add(c->app, ne);
                g_free(ne);
            }
        }
        g_list_free_full(wl, g_free);
        g_free(old_pref); g_free(old_rel); g_free(new_rel);
    }
    g_free(new_full); g_free(c->old_path); g_free(c);
}

static void ctx_rename(GtkWidget *btn, gpointer ud)
{
    TreeCtxData *ctx = ud;
    RenameCtx *c = g_new0(RenameCtx, 1);
    c->app      = ctx->app;
    c->old_path = g_strdup(ctx->path);
    c->is_dir   = ctx->is_dir;
    c->anchor   = btn;
    char *base = g_path_get_basename(ctx->path);
    ask_name_async(btn, "Umbenennen", base, rename_got_name, c);
    g_free(base);
}

/* ---- Löschen ---- */
typedef struct { App *app; char *path; gboolean is_dir; } DeleteCtx;

static void delete_confirmed(GObject *src, GAsyncResult *res, gpointer ud)
{
    DeleteCtx *c = ud;
    int btn = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    g_object_unref(src);
    if (btn != 1) { g_free(c->path); g_free(c); return; }  /* 0=Abbrechen */

    if (!c->is_dir) {
        char *rel = rel_of(c->path);
        project_whitelist_remove(c->app, rel);
        g_free(rel);
        g_remove(c->path);
    } else {
        char *rel_dir = rel_of(c->path);
        char *pref    = g_strconcat(rel_dir, "/", NULL);
        GList *wl = project_whitelist(c->app);
        for (GList *l = wl; l; l = l->next)
            if (g_str_has_prefix((char*)l->data, pref))
                project_whitelist_remove(c->app, l->data);
        g_list_free_full(wl, g_free);
        g_free(pref); g_free(rel_dir);
        char *quoted = g_shell_quote(c->path);
        char *cmd    = g_strdup_printf("rm -rf -- %s", quoted);
        g_free(quoted);
        g_spawn_command_line_async(cmd, NULL);
        g_free(cmd);
        project_refresh_tree(c->app);
    }
    g_free(c->path); g_free(c);
}

static void ctx_delete(GtkWidget *btn, gpointer ud)
{
    TreeCtxData *ctx = ud;
    char *base = g_path_get_basename(ctx->path);
    char *msg  = ctx->is_dir
        ? g_strdup_printf("Ordner \"%s\" und seinen gesamten Inhalt loeschen?", base)
        : g_strdup_printf("Datei \"%s\" unwiderruflich loeschen?", base);
    g_free(base);

    GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", msg);
    g_free(msg);
    const char *btns[] = { "Abbrechen", "Loeschen", NULL };
    gtk_alert_dialog_set_buttons(dlg, btns);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 1);

    DeleteCtx *c = g_new0(DeleteCtx, 1);
    c->app    = ctx->app;
    c->path   = g_strdup(ctx->path);
    c->is_dir = ctx->is_dir;

    gtk_alert_dialog_choose(dlg,
        GTK_WINDOW(gtk_widget_get_root(btn)),
        NULL, delete_confirmed, c);
    /* dlg wird in delete_confirmed via g_object_unref freigegeben */
}

/* ------------------------------------------------------------------
 * Popover aufbauen und anzeigen
 * ------------------------------------------------------------------ */
static void show_tree_context_menu(App *app, GtkWidget *row_widget,
                                   const char *path, gboolean is_dir,
                                   double x, double y)
{
    TreeCtxData *ctx = g_new0(TreeCtxData, 1);
    ctx->app    = app;
    ctx->path   = g_strdup(path);
    ctx->is_dir = is_dir;

    GtkWidget *popover = gtk_popover_new();
    gtk_widget_set_parent(popover, row_widget);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);

    /* Position relativ zum angeklickten Punkt. */
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top(vbox, 4);
    gtk_widget_set_margin_bottom(vbox, 4);
    gtk_widget_set_margin_start(vbox, 4);
    gtk_widget_set_margin_end(vbox, 4);

    struct { const char *label; GCallback cb; } items[] = {
        { "📄  Neue Datei",    G_CALLBACK(ctx_new_file)   },
        { "📁  Neuer Ordner",  G_CALLBACK(ctx_new_folder) },
        { "✏️  Umbenennen",    G_CALLBACK(ctx_rename)     },
        { "🗑  Löschen",       G_CALLBACK(ctx_delete)     },
    };
    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        GtkWidget *btn = gtk_button_new_with_label(items[i].label);
        gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
        gtk_widget_set_halign(btn, GTK_ALIGN_START);
        g_signal_connect(btn, "clicked", items[i].cb, ctx);
        /* Popover schließen, nachdem die Aktion ausgelöst wurde. */
        g_signal_connect_swapped(btn, "clicked",
                                 G_CALLBACK(gtk_popover_popdown), popover);
        gtk_box_append(GTK_BOX(vbox), btn);
    }
    gtk_popover_set_child(GTK_POPOVER(popover), vbox);

    /* ctx-Lifetime an den Popover binden. */
    g_object_set_data_full(G_OBJECT(popover), "hc-ctx", ctx,
                           (GDestroyNotify)tree_ctx_free);

    gtk_popover_popup(GTK_POPOVER(popover));
}

/* GtkGestureClick-Handler (Sekundärtaste = Rechtsklick). */
static void on_tree_right_click(GtkGestureClick *gesture, int n_press,
                                double x, double y, gpointer ud)
{
    (void)n_press;
    App *app = ud;
    GtkWidget *row_widget = gtk_event_controller_get_widget(
        GTK_EVENT_CONTROLLER(gesture));

    const char *path  = g_object_get_data(G_OBJECT(row_widget), "hc-path");
    gboolean    isdir = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(row_widget), "hc-isdir"));

    if (!path) return;
    show_tree_context_menu(app, row_widget, path, isdir, x, y);
}

/* ============================================================
 * ListView-Factory
 * ============================================================ */
static void factory_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer u)
{
    App *app = u;
    (void)f;
    GtkWidget *expander = gtk_tree_expander_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new();
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
    gtk_list_item_set_child(li, expander);

    /* Rechtsklick-Erkennung pro Zeile. */
    GtkGesture *rclick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
    g_signal_connect(rclick, "pressed", G_CALLBACK(on_tree_right_click), app);
    gtk_widget_add_controller(expander, GTK_EVENT_CONTROLLER(rclick));
}

static void factory_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer u)
{
    (void)f; (void)u;
    GtkTreeListRow *row = gtk_list_item_get_item(li);
    HcFileItem *fi = gtk_tree_list_row_get_item(row);
    GtkWidget *expander = gtk_list_item_get_child(li);
    gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);
    GtkWidget *box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander));
    GtkWidget *icon = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_last_child(box);
    gtk_image_set_from_icon_name(GTK_IMAGE(icon),
        fi->is_dir ? "folder-symbolic" : "text-x-generic-symbolic");
    gtk_label_set_text(GTK_LABEL(label), fi->name);
    g_object_set_data_full(G_OBJECT(expander), "hc-path",
                           g_strdup(fi->path), g_free);
    g_object_set_data(G_OBJECT(expander), "hc-isdir",
                      GINT_TO_POINTER(fi->is_dir));
    g_object_unref(fi);
}

static void on_tree_activate(GtkListView *lv, guint pos, gpointer ud)
{
    App *app = ud;
    GtkSelectionModel *sel = gtk_list_view_get_model(lv);
    GtkTreeListRow *row = g_list_model_get_item(G_LIST_MODEL(sel), pos);
    if (!row) return;
    HcFileItem *fi = gtk_tree_list_row_get_item(row);
    if (fi->is_dir) {
        gboolean ex = gtk_tree_list_row_get_expanded(row);
        gtk_tree_list_row_set_expanded(row, !ex);
    } else {
        editor_open_file(app, fi->path);
    }
    g_object_unref(fi);
    g_object_unref(row);
}

void project_setup_tree_view(App *app)
{
    g_signal_connect(app->tree_view, "activate",
                     G_CALLBACK(on_tree_activate), app);
}

/* Whitelist-Cache aus der .hcs neu aufbauen. */
static void load_whitelist_cache(App *app)
{
    if (s_wl) g_hash_table_destroy(s_wl);
    s_wl = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_clear_pointer(&s_wl_root, g_free);
    s_wl_root = g_strdup(app->proj_root);
    GList *wl = project_whitelist(app);
    for (GList *l = wl; l; l = l->next)
        g_hash_table_add(s_wl, g_strdup(l->data));
    g_list_free_full(wl, g_free);
}

void project_build_tree(App *app)
{
    if (!app->proj_root) return;
    load_whitelist_cache(app);
    GListModel *root = dir_to_model(app->proj_root);
    if (!root) return;
    GtkTreeListModel *tlm = gtk_tree_list_model_new(
        root, FALSE, FALSE, tree_create_child, app, NULL);
    GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(tlm));

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(factory_setup), app);
    g_signal_connect(factory, "bind",  G_CALLBACK(factory_bind),  NULL);

    gtk_list_view_set_model(GTK_LIST_VIEW(app->tree_view), GTK_SELECTION_MODEL(sel));
    gtk_list_view_set_factory(GTK_LIST_VIEW(app->tree_view), factory);
    g_object_unref(sel);
    g_object_unref(factory);
}

void project_refresh_tree(App *app) { project_build_tree(app); }

/* ============================================================
 * .hcs lesen/schreiben
 * ============================================================ */
static char *ght_path(const char *root, const char *name)
{
    char *fname = g_strconcat(name, HC_PROJECT_EXT, NULL);
    char *p = g_build_filename(root, fname, NULL);
    g_free(fname);
    return p;
}

/* Pfad der *.hcs im Verzeichnis (oder NULL). */
static char *find_ght(const char *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return NULL;
    const char *n; char *found = NULL;
    while ((n = g_dir_read_name(d))) {
        if (g_str_has_suffix(n, HC_PROJECT_EXT)) {
            found = g_build_filename(dir, n, NULL);
            break;
        }
    }
    g_dir_close(d);
    return found;
}

gboolean project_dir_has_ght(const char *dir)
{
    char *g = find_ght(dir);
    gboolean has = (g != NULL);
    g_free(g);
    return has;
}

/* ---- .hcs Whitelist (Tresor) ---- */
GList *project_whitelist(App *app)
{
    GList *out = NULL;
    if (!app->proj_root) return NULL;
    char *hcs = find_ght(app->proj_root);
    if (!hcs) return NULL;
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, hcs, G_KEY_FILE_NONE, NULL)) {
        gsize n = 0;
        char **list = g_key_file_get_string_list(kf, "files", "whitelist", &n, NULL);
        for (gsize i = 0; list && i < n; i++)
            out = g_list_append(out, g_strdup(list[i]));
        g_strfreev(list);
    }
    g_key_file_free(kf);
    g_free(hcs);
    return out;
}

static void whitelist_write(App *app, GList *entries)
{
    char *hcs = find_ght(app->proj_root);
    if (!hcs) return;
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, hcs, G_KEY_FILE_KEEP_COMMENTS, NULL);
    guint n = g_list_length(entries);
    const char **arr = g_new0(const char *, n);
    guint i = 0;
    for (GList *l = entries; l; l = l->next) arr[i++] = l->data;
    g_key_file_set_string_list(kf, "files", "whitelist", arr, n);
    g_free(arr);
    g_key_file_save_to_file(kf, hcs, NULL);
    g_key_file_free(kf);
    g_free(hcs);
}

void project_whitelist_add(App *app, const char *relpath)
{
    GList *wl = project_whitelist(app);
    for (GList *l = wl; l; l = l->next)
        if (!g_strcmp0(l->data, relpath)) { g_list_free_full(wl, g_free); return; }
    wl = g_list_append(wl, g_strdup(relpath));
    whitelist_write(app, wl);
    g_list_free_full(wl, g_free);
    project_build_tree(app);
}

void project_whitelist_remove(App *app, const char *relpath)
{
    GList *wl = project_whitelist(app);
    GList *keep = NULL;
    for (GList *l = wl; l; l = l->next)
        if (g_strcmp0(l->data, relpath)) keep = g_list_append(keep, g_strdup(l->data));
    whitelist_write(app, keep);
    g_list_free_full(wl, g_free);
    g_list_free_full(keep, g_free);
    rag_remove_file(app, relpath);   /* RAG-Garbage-Collection              */
    project_build_tree(app);
}

/* Hauptdatei laut .hcs (absolut) oder NULL. */
char *project_main_file(App *app)
{
    if (!app->proj_root) return NULL;
    char *ght = find_ght(app->proj_root);
    if (!ght) return NULL;
    char *result = NULL;
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, ght, G_KEY_FILE_NONE, NULL)) {
        char *mf = g_key_file_get_string(kf, "project", "main_file", NULL);
        if (mf && *mf) {
            result = g_path_is_absolute(mf) ? g_strdup(mf)
                   : g_build_filename(app->proj_root, mf, NULL);
            g_free(mf);
        }
    }
    g_key_file_free(kf);
    g_free(ght);
    return result;
}

/* zuletzt geöffnetes Projekt merken / lesen. */
static void project_remember(const char *root)
{
    char *dir = hc_config_dir();
    char *p = g_build_filename(dir, "last_project", NULL);
    g_file_set_contents(p, root, -1, NULL);
    g_free(p); g_free(dir);
}

char *project_last_path(void)
{
    char *dir = hc_config_dir();
    char *p = g_build_filename(dir, "last_project", NULL);
    char *content = NULL;
    if (g_file_get_contents(p, &content, NULL, NULL))
        g_strstrip(content);
    g_free(p); g_free(dir);
    if (content && !*content) { g_free(content); content = NULL; }
    return content;
}

/* Sucht eine *.hcs im Root und lädt die Projekt-Metadaten. */
static void load_ght(App *app)
{
    char *found = find_ght(app->proj_root);
    if (!found) return;

    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, found, G_KEY_FILE_NONE, NULL)) {
        g_clear_pointer(&app->proj_name, g_free);
        g_clear_pointer(&app->proj_lang, g_free);
        g_clear_pointer(&app->proj_template, g_free);
        g_clear_pointer(&app->proj_build, g_free);
        g_clear_pointer(&app->proj_run, g_free);
        app->proj_name = g_key_file_get_string(kf, "project", "name", NULL);
        app->proj_lang = g_key_file_get_string(kf, "project", "lang", NULL);
        app->proj_template = g_key_file_get_string(kf, "project", "template", NULL);
        app->proj_build = g_key_file_get_string(kf, "build", "build_cmd", NULL);
        app->proj_run = g_key_file_get_string(kf, "build", "run_cmd", NULL);
    }
    g_key_file_free(kf);
    g_free(found);
}

/* Liest einen Schalter aus dem Abschnitt [services] der .hcs.
 * Dort hinterlegt der Wizard, welche Dienste das Projekt braucht
 * (z.B. db=true fuer MySQL). */
gboolean project_flag(App *app, const char *key, gboolean def)
{
    if (!app->proj_root) return def;
    char *found = find_ght(app->proj_root);
    if (!found) return def;
    gboolean val = def;
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, found, G_KEY_FILE_NONE, NULL)) {
        GError *e = NULL;
        gboolean v = g_key_file_get_boolean(kf, "services", key, &e);
        if (!e) val = v;
        g_clear_error(&e);
    }
    g_key_file_free(kf);
    g_free(found);
    return val;
}

/* Schreibt einen Dienst-Schalter zurueck in die .hcs. */
void project_set_flag(App *app, const char *key, gboolean val)
{
    if (!app->proj_root) return;
    char *found = find_ght(app->proj_root);
    if (!found) return;
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, found, G_KEY_FILE_NONE, NULL)) {
        g_key_file_set_boolean(kf, "services", key, val);
        g_key_file_save_to_file(kf, found, NULL);
    }
    g_key_file_free(kf);
    g_free(found);
}

/* ============================================================
 * Projekt öffnen
 * ============================================================ */
void project_open(App *app, const char *root)
{
    project_close(app);
    /* Emulator-Tab des Vorgaengerprojekts entfernen – sonst zeigt er
     * weiterhin den alten (toten) Stream. */
    preview_reset(app);
    agent_reset_context(app);
    app->proj_root = g_strdup(root);
    char *base = g_path_get_basename(root);
    app->proj_name = g_strdup(base);
    g_free(base);
    app->proj_lang = g_strdup("—");

    load_ght(app);
    project_build_tree(app);

    char *s = g_strdup_printf("%s   (%s)", app->proj_name,
                              app->proj_lang ? app->proj_lang : "—");
    gtk_label_set_text(GTK_LABEL(app->st_project), s);
    g_free(s);

    project_remember(root);
    terminal_cd(app, root);   /* Konsole ins Projektverzeichnis           */

    /* RAG-Index im Hintergrund (nahtlos im Ladescreen) aufbauen. */
    rag_index_project(app, FALSE);

    /* Initialization Cycle: Ladescreen + Sandbox-Inspektor.
     * Bei grünem Status öffnet der Inspektor project_finish_open(). */
    inspector_open_for_project(app);
}

/* Wird vom Inspektor aufgerufen, sobald die Sandbox bereit ist. */
void project_finish_open(App *app)
{
    /* Native Source Control: Repo sicherstellen (Auto-Versioning /
     * Smart Import), danach Branch-Anzeige aktualisieren. */
    git_ensure_repo(app);
    app->git_readonly = FALSE;
    git_update_header(app);

    session_restore(app);
    if (!app->tabs) {
        char *mf = project_main_file(app);
        if (mf && g_file_test(mf, G_FILE_TEST_IS_REGULAR))
            editor_open_file(app, mf);
        g_free(mf);
    }
    editor_update_status(app);
}

void project_close(App *app)
{
    session_save(app);
    /* Maschinenraum abschalten: erst die Dienste, dann die Sandbox.
     * Sonst laufen Webserver/Datenbank als Waisen weiter. */
    services_stop_all(app);
    sandbox_down(app);
    /* alle Tabs schließen */
    while (app->tabs) editor_close_tab((EditorTab *)app->tabs->data);
    g_clear_pointer(&app->proj_root, g_free);
    g_clear_pointer(&app->proj_name, g_free);
    g_clear_pointer(&app->proj_lang, g_free);
    g_clear_pointer(&app->proj_template, g_free);
    g_clear_pointer(&app->proj_build, g_free);
    g_clear_pointer(&app->proj_run, g_free);
}

/* ============================================================
 * Session: offene Tabs merken & wiederherstellen
 * ============================================================ */
void session_save(App *app)
{
    if (!app->proj_root) return;
    char *path = g_build_filename(app->proj_root, HC_SESSION_FILE, NULL);
    GString *s = g_string_new("");
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    int idx = 0, curidx = 0;
    for (GList *l = app->tabs; l; l = l->next, idx++) {
        EditorTab *t = l->data;
        if (!t->path) continue;
        int p = gtk_notebook_page_num(GTK_NOTEBOOK(app->notebook), t->page);
        if (p == cur) curidx = idx;
        g_string_append_printf(s, "%s\n", t->path);
    }
    g_string_append_printf(s, "#current=%d\n", curidx);
    g_file_set_contents(path, s->str, -1, NULL);
    g_string_free(s, TRUE);
    g_free(path);
}

void session_restore(App *app)
{
    if (!app->proj_root) return;
    char *path = g_build_filename(app->proj_root, HC_SESSION_FILE, NULL);
    char *content = NULL;
    if (g_file_get_contents(path, &content, NULL, NULL)) {
        char **lines = g_strsplit(content, "\n", -1);
        int current = 0;
        for (int i = 0; lines[i]; i++) {
            if (!*lines[i]) continue;
            if (g_str_has_prefix(lines[i], "#current=")) {
                current = atoi(lines[i] + 9);
                continue;
            }
            if (g_file_test(lines[i], G_FILE_TEST_IS_REGULAR))
                editor_open_file(app, lines[i]);
        }
        if (app->tabs && current < (int)g_list_length(app->tabs))
            gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), current);
        g_strfreev(lines);
        g_free(content);
    }
    g_free(path);
}

/* ============================================================
 * Datei-Generator: Hello-World-Templates
 * ============================================================ */
typedef struct { const char *rel; const char *content; } GenFile;

static gboolean write_rel(const char *root, const char *rel, const char *content, GError **err)
{
    char *full = g_build_filename(root, rel, NULL);
    char *dir = g_path_get_dirname(full);
    g_mkdir_with_parents(dir, 0755);
    gboolean ok = g_file_set_contents(full, content, -1, err);
    g_free(dir); g_free(full);
    return ok;
}

gboolean project_create_files(const char *root, const char *name,
                              const char *lang, const char *template_name,
                              gboolean want_db, GError **error)
{
    if (g_mkdir_with_parents(root, 0755) != 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                    "Konnte Projektordner nicht anlegen: %s", root);
        return FALSE;
    }

    const char *src = NULL, *build = NULL;
    const char *build_cmd = "bash build.sh";
    const char *run_cmd = "./bin/app";
    const char *scheck = "";
    const char *srcfile = "src/main.c";
    gboolean is_web = (template_name && !strcmp(template_name, "Web"));
    gboolean is_gtk4 = (template_name && !strcmp(template_name, "GTK4"));

    if (!strcmp(lang, "C") && is_gtk4) {
        srcfile = "src/main.c";
        /* Die Oberfläche liegt in einer eigenen .ui-Datei (GtkBuilder),
         * damit sie sofort in der Live-Preview getestet werden kann. */
        const char *ui =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<interface>\n"
        "  <object class=\"GtkWindow\" id=\"main_window\">\n"
        "    <property name=\"title\">Hallo HyperCore Studio</property>\n"
        "    <property name=\"default-width\">440</property>\n"
        "    <property name=\"default-height\">260</property>\n"
        "    <child>\n"
        "      <object class=\"GtkBox\">\n"
        "        <property name=\"orientation\">vertical</property>\n"
        "        <property name=\"spacing\">14</property>\n"
        "        <property name=\"margin-top\">28</property>\n"
        "        <property name=\"margin-bottom\">28</property>\n"
        "        <property name=\"margin-start\">28</property>\n"
        "        <property name=\"margin-end\">28</property>\n"
        "        <property name=\"valign\">center</property>\n"
        "        <child>\n"
        "          <object class=\"GtkLabel\" id=\"title\">\n"
        "            <property name=\"label\">Hallo Welt aus GTK4!</property>\n"
        "            <style><class name=\"title-1\"/></style>\n"
        "          </object>\n"
        "        </child>\n"
        "        <child>\n"
        "          <object class=\"GtkLabel\" id=\"counter\">\n"
        "            <property name=\"label\">Noch nicht geklickt.</property>\n"
        "          </object>\n"
        "        </child>\n"
        "        <child>\n"
        "          <object class=\"GtkButton\" id=\"btn\">\n"
        "            <property name=\"label\">Klick mich</property>\n"
        "            <property name=\"halign\">center</property>\n"
        "          </object>\n"
        "        </child>\n"
        "      </object>\n"
        "    </child>\n"
        "  </object>\n"
        "</interface>\n";
        if (!write_rel(root, "src/window.ui", ui, error)) return FALSE;

        src =
        "#include <gtk/gtk.h>\n"
        "#include <unistd.h>\n\n"
        "static int clicks = 0;\n\n"
        "/* window.ui robust finden: neben der Binary, sonst ./src/window.ui. */\n"
        "static char *find_ui(void) {\n"
        "    char exe[4096];\n"
        "    ssize_t n = readlink(\"/proc/self/exe\", exe, sizeof(exe) - 1);\n"
        "    if (n > 0) {\n"
        "        exe[n] = 0;\n"
        "        char *dir = g_path_get_dirname(exe);\n"
        "        char *p = g_build_filename(dir, \"window.ui\", NULL);\n"
        "        g_free(dir);\n"
        "        if (g_file_test(p, G_FILE_TEST_EXISTS)) return p;\n"
        "        g_free(p);\n"
        "    }\n"
        "    return g_strdup(\"src/window.ui\");\n"
        "}\n\n"
        "static void on_clicked(GtkButton *btn, gpointer user_data) {\n"
        "    GtkLabel *counter = GTK_LABEL(user_data);\n"
        "    char buf[64];\n"
        "    g_snprintf(buf, sizeof(buf), \"%d-mal geklickt.\", ++clicks);\n"
        "    gtk_label_set_text(counter, buf);\n"
        "}\n\n"
        "static void on_activate(GtkApplication *app, gpointer u) {\n"
        "    (void)u;\n"
        "    char *ui = find_ui();\n"
        "    GtkBuilder *b = gtk_builder_new_from_file(ui);\n"
        "    g_free(ui);\n"
        "    GObject *win = gtk_builder_get_object(b, \"main_window\");\n"
        "    gtk_window_set_application(GTK_WINDOW(win), app);\n"
        "    GObject *btn = gtk_builder_get_object(b, \"btn\");\n"
        "    GObject *counter = gtk_builder_get_object(b, \"counter\");\n"
        "    if (btn && counter)\n"
        "        g_signal_connect(btn, \"clicked\", G_CALLBACK(on_clicked), counter);\n"
        "    gtk_window_present(GTK_WINDOW(win));\n"
        "    g_object_unref(b);\n"
        "}\n\n"
        "int main(int argc, char **argv) {\n"
        "    GtkApplication *app = gtk_application_new(\"com.example.hello\", G_APPLICATION_DEFAULT_FLAGS);\n"
        "    g_signal_connect(app, \"activate\", G_CALLBACK(on_activate), NULL);\n"
        "    int s = g_application_run(G_APPLICATION(app), argc, argv);\n"
        "    g_object_unref(app);\n"
        "    return s;\n"
        "}\n";
        build =
        "#!/usr/bin/env bash\nset -e\nmkdir -p bin\n"
        "gcc -Wall -o bin/app src/*.c $(pkg-config --cflags --libs gtk4)\n"
        "cp -f src/window.ui bin/window.ui\n"
        "echo 'Build OK -> bin/app'\n";
    }
    else if (!strcmp(lang, "C")) {
        src =
        "#include <stdio.h>\n\n"
        "int main(void) {\n"
        "    printf(\"Hallo Welt!\\n\");\n"
        "    return 0;\n"
        "}\n";
        build =
        "#!/usr/bin/env bash\nset -e\nmkdir -p bin\n"
        "gcc -Wall -o bin/app src/*.c\n"
        "echo 'Build OK -> bin/app'\n";
    }
    else if (!strcmp(lang, "C++")) {
        srcfile = "src/main.cpp";
        src =
        "#include <iostream>\n\n"
        "int main() {\n"
        "    std::cout << \"Hallo Welt!\" << std::endl;\n"
        "    return 0;\n"
        "}\n";
        build =
        "#!/usr/bin/env bash\nset -e\nmkdir -p bin\n"
        "g++ -Wall -o bin/app src/*.cpp\n"
        "echo 'Build OK -> bin/app'\n";
    }
    else if (!strcmp(lang, "Python")) {
        srcfile = "src/main.py";
        src = "def main():\n    print(\"Hallo Welt!\")\n\n\nif __name__ == \"__main__\":\n    main()\n";
        build = "#!/usr/bin/env bash\nset -e\npython3 -m py_compile src/main.py\necho 'Syntax OK'\n";
        build_cmd = "bash build.sh";
        run_cmd = "python3 src/main.py";
    }
    else if (!strcmp(lang, "Bash")) {
        srcfile = "src/main.sh";
        src = "#!/usr/bin/env bash\necho \"Hallo Welt!\"\n";
        build = "#!/usr/bin/env bash\nset -e\nbash -n src/main.sh\necho 'Syntax OK'\n";
        run_cmd = "bash src/main.sh";
    }
    else if (!strcmp(lang, "PHP")) {
        if (is_web && want_db) {
            /* Web + Datenbank: das Template zeigt sofort eine echte
             * Verbindung. Zugangsdaten kommen aus der Umgebung, weil die
             * IDE den Port pro Sitzung frei waehlt. */
            srcfile = "index.php";
            src =
            "<?php\n"
            "/* Zugang zur Projekt-Datenbank. Host/Port/Name stellt die IDE\n"
            " * ueber Umgebungsvariablen bereit – nichts fest eintragen. */\n"
            "$host = getenv('HC_DB_HOST') ?: '127.0.0.1';\n"
            "$port = (int)(getenv('HC_DB_PORT') ?: 3306);\n"
            "$user = getenv('HC_DB_USER') ?: 'root';\n"
            "$pass = getenv('HC_DB_PASS') ?: '';\n"
            "$name = getenv('HC_DB_NAME') ?: 'projekt';\n\n"
            "$db = @new mysqli($host, $user, $pass, $name, $port);\n"
            "?>\n"
            "<!DOCTYPE html>\n<html lang=\"de\">\n<head>\n  <meta charset=\"utf-8\">\n"
            "  <title>Hallo HyperCore Studio</title>\n"
            "  <style>body{font:15px system-ui;margin:40px;max-width:46em}\n"
            "  code{background:#eee;padding:2px 5px;border-radius:3px}</style>\n"
            "</head>\n<body>\n"
            "  <h1>Hallo Welt!</h1>\n"
            "  <p>Erzeugt von der HyperCore Studio-IDE.</p>\n"
            "<?php if ($db->connect_errno): ?>\n"
            "  <p><strong>Datenbank nicht erreichbar:</strong>\n"
            "     <?= htmlspecialchars($db->connect_error) ?></p>\n"
            "<?php else: ?>\n"
            "  <p>Datenbank <code><?= htmlspecialchars($name) ?></code> verbunden\n"
            "     (Server <?= htmlspecialchars($db->server_info) ?>).</p>\n"
            "<?php\n"
            "  /* Beispiel-Tabelle anlegen und befuellen. */\n"
            "  $db->query('CREATE TABLE IF NOT EXISTS notizen (\n"
            "       id INT AUTO_INCREMENT PRIMARY KEY,\n"
            "       text VARCHAR(255) NOT NULL,\n"
            "       erstellt TIMESTAMP DEFAULT CURRENT_TIMESTAMP)');\n"
            "  if (($db->query('SELECT COUNT(*) c FROM notizen')->fetch_assoc()['c'] ?? 0) == 0)\n"
            "      $db->query(\"INSERT INTO notizen (text) VALUES ('Erste Notiz')\");\n"
            "  $rows = $db->query('SELECT id, text, erstellt FROM notizen ORDER BY id');\n"
            "?>\n"
            "  <h2>Notizen</h2>\n  <ul>\n"
            "<?php while ($r = $rows->fetch_assoc()): ?>\n"
            "    <li>#<?= (int)$r['id'] ?> <?= htmlspecialchars($r['text']) ?>\n"
            "        <small>(<?= $r['erstellt'] ?>)</small></li>\n"
            "<?php endwhile; ?>\n  </ul>\n"
            "<?php endif; ?>\n"
            "</body>\n</html>\n";
            build = "#!/usr/bin/env bash\nset -e\nphp -l index.php\necho 'Syntax OK'\n";
            run_cmd = "php index.php";
        } else if (is_web) {
            srcfile = "index.php";
            src =
            "<!DOCTYPE html>\n<html lang=\"de\">\n<head>\n  <meta charset=\"utf-8\">\n"
            "  <title>Hallo HyperCore Studio</title>\n</head>\n<body>\n"
            "  <h1><?php echo \"Hallo Welt!\"; ?></h1>\n"
            "  <p>Erzeugt von der HyperCore Studio-IDE.</p>\n</body>\n</html>\n";
            build = "#!/usr/bin/env bash\nset -e\nphp -l index.php\necho 'Syntax OK'\n";
            /* Der Webserver wird von der IDE verwaltet (services.c),
             * nicht vom Run-Kommando gestartet. */
            run_cmd = "php index.php";
        } else {
            srcfile = "src/index.php";
            src = "<?php\n\necho \"Hallo Welt!\\n\";\n";
            build = "#!/usr/bin/env bash\nset -e\nphp -l src/index.php\necho 'Syntax OK'\n";
            run_cmd = "php src/index.php";
        }
    }
    else {
        srcfile = "src/main.txt";
        src = "Hallo Welt!\n";
        build = "#!/usr/bin/env bash\necho 'Kein Build definiert.'\n";
        run_cmd = "cat src/main.txt";
    }
    (void)scheck;

    if (!write_rel(root, srcfile, src, error)) return FALSE;
    if (!write_rel(root, "build.sh", build, error)) return FALSE;

    /* build.sh ausführbar machen. */
    char *bs = g_build_filename(root, "build.sh", NULL);
    g_chmod(bs, 0755);
    g_free(bs);

    /* Arbeitsordner der IDE nicht versionieren (DB-Daten, Logs, php.ini). */
    write_rel(root, ".gitignore",
        "# HyperCore Studio\n.hypercore/\nbin/\nsnapshots/\n", NULL);

    /* README */
    char *readme = g_strdup_printf(
        "# %s\n\nErzeugt mit der HyperCore Studio-IDE (%s).\n\n"
        "Sprache: %s  ·  Template: %s\n\n"
        "## Bauen\n\n    bash build.sh\n", name, APP_COMPANY, lang, template_name);
    write_rel(root, "README.md", readme, NULL);
    g_free(readme);

    /* .hcs schreiben */
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "project", "name", name);
    g_key_file_set_string(kf, "project", "lang", lang);
    g_key_file_set_string(kf, "project", "template", template_name);
    g_key_file_set_string(kf, "project", "main_file", srcfile);
    g_key_file_set_string(kf, "project", "generator", APP_NAME " " APP_VERSION);
    g_key_file_set_string(kf, "build", "build_cmd", build_cmd);
    g_key_file_set_string(kf, "build", "run_cmd", run_cmd);
    /* Welche Dienste das Projekt braucht – liest services.c beim Oeffnen. */
    g_key_file_set_boolean(kf, "services", "db", want_db);
    g_key_file_set_boolean(kf, "services", "web", is_web);

    /* Whitelist (Tresor): nur diese Dateien sind der IDE bekannt. */
    GPtrArray *wl = g_ptr_array_new();
    g_ptr_array_add(wl, (gpointer)srcfile);
    g_ptr_array_add(wl, (gpointer)"build.sh");
    g_ptr_array_add(wl, (gpointer)"README.md");
    if (is_gtk4 && !g_strcmp0(lang, "C"))
        g_ptr_array_add(wl, (gpointer)"src/window.ui");
    g_key_file_set_string_list(kf, "files", "whitelist",
        (const char * const *)wl->pdata, wl->len);
    g_ptr_array_free(wl, TRUE);

    char *gp = ght_path(root, name);
    gboolean ok = g_key_file_save_to_file(kf, gp, error);
    g_free(gp);
    g_key_file_free(kf);
    return ok;
}
