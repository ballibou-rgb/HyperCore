#include "app.h"
#include <string.h>

/* ============================================================
 * Globale Suche: grep über den gesamten Projektordner.
 * Ersetzen aus Sicherheitsgründen NUR in der aktuell offenen Datei.
 * ============================================================ */

typedef struct {
    App *app;
    GtkWidget *win;
    GtkWidget *search_entry;
    GtkWidget *replace_entry;
    GtkWidget *results;     /* GtkListBox */
    GtkWidget *status;
} SearchDlg;

typedef struct { App *app; char *file; int line; } Hit;

static void on_hit_activate(GtkListBox *box, GtkListBoxRow *row, gpointer ud)
{
    (void)box;
    SearchDlg *d = ud;
    if (!row) return;
    Hit *h = g_object_get_data(G_OBJECT(row), "hit");
    if (h) {
        editor_jump_to(d->app, h->file, h->line);
    }
}

static void hit_free(gpointer p) { Hit *h = p; g_free(h->file); g_free(h); }

static void clear_results(SearchDlg *d)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(d->results)))
        gtk_list_box_remove(GTK_LIST_BOX(d->results), c);
}

static void grep_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    SearchDlg *d = ud;
    GError *e = NULL;
    GBytes *out = NULL;
    g_subprocess_communicate_finish(G_SUBPROCESS(src), res, &out, NULL, &e);
    g_object_unref(src);
    if (e) { g_clear_error(&e); if (out) g_bytes_unref(out); return; }

    gsize len = 0;
    const char *data = out ? g_bytes_get_data(out, &len) : "";
    char **lines = g_strsplit(data ? data : "", "\n", -1);
    int count = 0;
    for (int i = 0; lines[i]; i++) {
        if (!*lines[i]) continue;
        /* Format: file:line:content */
        char *c1 = strchr(lines[i], ':');
        if (!c1) continue;
        char *c2 = strchr(c1 + 1, ':');
        if (!c2) continue;
        *c1 = 0; *c2 = 0;
        const char *file = lines[i];
        int line = atoi(c1 + 1);
        const char *content = c2 + 1;

        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *b = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start(b, 8); gtk_widget_set_margin_end(b, 8);
        gtk_widget_set_margin_top(b, 4); gtk_widget_set_margin_bottom(b, 4);
        const char *base = strrchr(file, '/'); base = base ? base + 1 : file;
        char *loc = g_strdup_printf("%s:%d", base, line);
        GtkWidget *l1 = gtk_label_new(loc);
        g_free(loc);
        gtk_widget_add_css_class(l1, "hc-search-loc");
        gtk_label_set_xalign(GTK_LABEL(l1), 0.0);
        char *prev = g_strstrip(g_strdup(content));
        GtkWidget *l2 = gtk_label_new(prev);
        g_free(prev);
        gtk_label_set_xalign(GTK_LABEL(l2), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(l2), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(l2, "dim-label");
        gtk_box_append(GTK_BOX(b), l1);
        gtk_box_append(GTK_BOX(b), l2);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), b);

        Hit *h = g_new0(Hit, 1);
        h->app = d->app; h->file = g_strdup(file); h->line = line;
        g_object_set_data_full(G_OBJECT(row), "hit", h, hit_free);
        gtk_list_box_append(GTK_LIST_BOX(d->results), row);
        count++;
    }
    g_strfreev(lines);
    if (out) g_bytes_unref(out);

    char *st = g_strdup_printf("%d Treffer", count);
    gtk_label_set_text(GTK_LABEL(d->status), st);
    g_free(st);
}

static void do_search(SearchDlg *d)
{
    clear_results(d);
    const char *q = gtk_editable_get_text(GTK_EDITABLE(d->search_entry));
    if (!q || !*q) return;
    if (!d->app->proj_root) {
        gtk_label_set_text(GTK_LABEL(d->status), "Kein Projekt geöffnet.");
        return;
    }
    gtk_label_set_text(GTK_LABEL(d->status), "Suche…");

    const char *argv[] = {
        "grep", "-rnI", "--exclude-dir=.git", "--exclude-dir=bin",
        "-e", q, d->app->proj_root, NULL
    };
    GError *e = NULL;
    GSubprocess *proc = g_subprocess_newv(argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &e);
    if (!proc) { g_clear_error(&e); return; }
    g_subprocess_communicate_async(proc, NULL, NULL, grep_done, d);
}

static void on_search_clicked(GtkWidget *w, gpointer ud) { (void)w; do_search((SearchDlg *)ud); }

static void on_replace_clicked(GtkWidget *w, gpointer ud)
{
    (void)w;
    SearchDlg *d = ud;
    EditorTab *t = editor_current(d->app);
    const char *q = gtk_editable_get_text(GTK_EDITABLE(d->search_entry));
    const char *r = gtk_editable_get_text(GTK_EDITABLE(d->replace_entry));
    if (!t || !q || !*q) {
        gtk_label_set_text(GTK_LABEL(d->status), "Keine Datei offen / kein Suchbegriff.");
        return;
    }
    GtkTextIter a, z; gtk_text_buffer_get_bounds(t->buffer, &a, &z);
    char *text = gtk_text_buffer_get_text(t->buffer, &a, &z, FALSE);
    char **parts = g_strsplit(text, q, -1);
    int occ = g_strv_length(parts) - 1;
    char *out = g_strjoinv(r ? r : "", parts);
    g_strfreev(parts);
    gtk_text_buffer_set_text(t->buffer, out, -1);
    g_free(out); g_free(text);
    char *st = g_strdup_printf("%d Ersetzung(en) in aktueller Datei.", occ);
    gtk_label_set_text(GTK_LABEL(d->status), st);
    g_free(st);
}

void search_open_dialog(App *app)
{
    SearchDlg *d = g_new0(SearchDlg, 1);
    d->app = app;

    GtkWidget *root_outer = NULL;
    GtkWidget *win = hc_make_dialog(app, "Suchen & Ersetzen", 560, 500, &root_outer);
    d->win = win;
    g_object_set_data_full(G_OBJECT(win), "dlg", d, g_free);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_vexpand(root, TRUE);
    gtk_widget_set_margin_start(root, 12); gtk_widget_set_margin_end(root, 12);
    gtk_widget_set_margin_top(root, 12); gtk_widget_set_margin_bottom(root, 12);
    gtk_box_append(GTK_BOX(root_outer), root);

    GtkWidget *r1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    d->search_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->search_entry), "Im Projekt suchen (grep)…");
    gtk_widget_set_hexpand(d->search_entry, TRUE);
    g_signal_connect(d->search_entry, "activate", G_CALLBACK(on_search_clicked), d);
    GtkWidget *sbtn = gtk_button_new_with_label("Suchen");
    gtk_widget_add_css_class(sbtn, "suggested-action");
    g_signal_connect(sbtn, "clicked", G_CALLBACK(on_search_clicked), d);
    gtk_box_append(GTK_BOX(r1), d->search_entry);
    gtk_box_append(GTK_BOX(r1), sbtn);
    gtk_box_append(GTK_BOX(root), r1);

    GtkWidget *r2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    d->replace_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->replace_entry), "Ersetzen durch… (nur aktuelle Datei)");
    gtk_widget_set_hexpand(d->replace_entry, TRUE);
    GtkWidget *rbtn = gtk_button_new_with_label("In Datei ersetzen");
    g_signal_connect(rbtn, "clicked", G_CALLBACK(on_replace_clicked), d);
    gtk_box_append(GTK_BOX(r2), d->replace_entry);
    gtk_box_append(GTK_BOX(r2), rbtn);
    gtk_box_append(GTK_BOX(root), r2);

    GtkWidget *scr = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scr, TRUE);
    d->results = gtk_list_box_new();
    gtk_widget_add_css_class(d->results, "hc-search-results");
    g_signal_connect(d->results, "row-activated", G_CALLBACK(on_hit_activate), d);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), d->results);
    gtk_box_append(GTK_BOX(root), scr);

    d->status = gtk_label_new("");
    gtk_widget_add_css_class(d->status, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(d->status), 0.0);
    gtk_box_append(GTK_BOX(root), d->status);

    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(d->search_entry);
}
