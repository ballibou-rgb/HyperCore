#include "app.h"
#include "version.h"
#include <string.h>

/* ============================================================
 * Projekt-Wizard (Neues Projekt).
 *  Seite 1: Name, Sprache (C/C++/Python/Bash/PHP), Zielordner.
 *  Seite 2: Template (Console/GTK3/GTK4/Web) + System-Check.
 * ============================================================ */

typedef struct {
    App *app;
    GtkWidget *win;
    GtkWidget *stack;
    GtkWidget *name_entry;
    GtkWidget *lang_drop;
    GtkWidget *dir_label;
    char      *parent_dir;
    GtkWidget *tmpl_drop;
    GtkWidget *check_box;   /* Container für System-Check-Zeilen */
    GtkWidget *svc_box;     /* Dienst-Optionen (nur wo sinnvoll)  */
    GtkWidget *db_check;    /* MySQL/MariaDB fuer Webprojekte     */
    GtkWidget *svc_note;    /* Erklaerzeile zu den Diensten       */
    GtkWidget *btn_back;
    GtkWidget *btn_next;
    GtkWidget *btn_finish;
} Wizard;

static const char *LANGS[] = { "C", "C++", "Python", "Bash", "PHP", NULL };

/* ============================================================
 * Statisches C-Routing: welche Templates gehoeren zu welcher Sprache.
 *
 * Vorher war die Template-Liste eine feste Liste fuer alle Sprachen –
 * bei PHP liess sich "GTK4" waehlen, bei "Web" wurde auf GTK-Pakete
 * geprueft. Beides ist jetzt an die Sprache gekoppelt.
 * ============================================================ */
typedef struct {
    const char *lang;
    const char *templates[6];   /* NULL-terminiert */
} LangRoute;

static const LangRoute ROUTES[] = {
    { "C",      { "Console", "GTK4", "GTK3", "Web", NULL } },
    { "C++",    { "Console", "GTK4", "GTK3", NULL } },
    { "Python", { "Console", "GTK4", "Web", NULL } },
    { "Bash",   { "Console", NULL } },
    { "PHP",    { "Web", "Console", NULL } },
};

static const LangRoute *route_for(const char *lang)
{
    for (guint i = 0; i < G_N_ELEMENTS(ROUTES); i++)
        if (!g_strcmp0(ROUTES[i].lang, lang)) return &ROUTES[i];
    return &ROUTES[0];
}

static const char *lang_at(Wizard *w) {
    return LANGS[gtk_drop_down_get_selected(GTK_DROP_DOWN(w->lang_drop))];
}

static const char *tmpl_at(Wizard *w) {
    const LangRoute *r = route_for(lang_at(w));
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(w->tmpl_drop));
    if (sel == GTK_INVALID_LIST_POSITION) sel = 0;
    /* Laenge der aktuellen Liste bestimmen. */
    guint n = 0;
    while (r->templates[n]) n++;
    if (sel >= n) sel = 0;
    return r->templates[sel];
}

static gboolean tmpl_is_web(const char *t)   { return t && strstr(t, "Web") != NULL; }
static gboolean tmpl_is_gtk(const char *t)   { return t && strstr(t, "GTK") != NULL; }

/* ---- System-Check ---- */
/* Prueft IN der Sandbox, nicht auf dem Host: dort wird spaeter gebaut. */
static GtkWidget *check_row(App *app, const char *label, const char *tool)
{
    gboolean have = sandbox_has_tool(app, tool);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *mark = gtk_label_new(have ? "✓" : "✗");
    gtk_widget_add_css_class(mark, have ? "hc-check-ok" : "hc-check-fail");
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_widget_set_hexpand(l, TRUE);
    GtkWidget *loc = gtk_label_new(have ? "vorhanden" : "fehlt in der Sandbox");
    gtk_widget_add_css_class(loc, "dim-label");
    gtk_box_append(GTK_BOX(row), mark);
    gtk_box_append(GTK_BOX(row), l);
    gtk_box_append(GTK_BOX(row), loc);
    return row;
}

/* Template-Liste an die gewaehlte Sprache anpassen. */
static void refresh_templates(Wizard *w)
{
    const LangRoute *r = route_for(lang_at(w));
    GtkStringList *sl = gtk_string_list_new(NULL);
    for (int i = 0; r->templates[i]; i++)
        gtk_string_list_append(sl, r->templates[i]);
    gtk_drop_down_set_model(GTK_DROP_DOWN(w->tmpl_drop), G_LIST_MODEL(sl));
    g_object_unref(sl);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(w->tmpl_drop), 0);
}

/* Dienst-Optionen: nur anzeigen, was zum Projekttyp passt. */
static void refresh_services(Wizard *w)
{
    const char *tmpl = tmpl_at(w);
    gboolean web = tmpl_is_web(tmpl);

    gtk_widget_set_visible(w->svc_box, web);
    if (!web) return;

    gtk_label_set_text(GTK_LABEL(w->svc_note),
        "Beim Oeffnen des Projekts startet die IDE automatisch: Webserver "
        "und Mail-Catcher (mail() wird abgefangen). Die Datenbank laeuft mit "
        "eigenem Datenverzeichnis im Projekt.");
}

/* System-Check: genau die Werkzeuge pruefen, die dieses Projekt
 * wirklich braucht – und zwar in der Sandbox, in der spaeter auch
 * gebaut und ausgefuehrt wird. */
static void refresh_checks(Wizard *w)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(w->check_box)))
        gtk_box_remove(GTK_BOX(w->check_box), c);

    const char *lang = lang_at(w);
    const char *tmpl = tmpl_at(w);
    App *app = w->app;

    #define ROW(label, tool) \
        gtk_box_append(GTK_BOX(w->check_box), check_row(app, label, tool))

    /* Toolchain der Sprache. */
    if (!strcmp(lang, "C"))      ROW("gcc (C-Compiler)", "gcc");
    if (!strcmp(lang, "C++"))    ROW("g++ (C++-Compiler)", "g++");
    if (!strcmp(lang, "Python")) ROW("python3", "python3");
    if (!strcmp(lang, "Bash"))   ROW("bash", "bash");
    if (!strcmp(lang, "PHP"))    ROW("php", "php");

    /* Nur bei GTK-Templates nach GTK-Werkzeugen fragen. */
    if (tmpl_is_gtk(tmpl)) {
        ROW("pkg-config", "pkg-config");
        ROW("GUI-Emulator (gtk4-broadwayd)", "gtk4-broadwayd");
    }

    /* Nur bei Webprojekten nach Web-/DB-Werkzeugen fragen. */
    if (tmpl_is_web(tmpl)) {
        if (g_strcmp0(lang, "PHP") != 0)
            ROW("Webserver (python3)", "python3");
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(w->db_check))) {
            ROW("Datenbank-Server (mariadbd)", "mariadbd");
            ROW("Datenbank-Client (mariadb)", "mariadb");
        }
    }

    ROW("make (Build-Tool)", "make");
    #undef ROW
}

/* Beide Listen zusammen aktualisieren (Template-Wechsel). */
static void refresh_page2(Wizard *w)
{
    refresh_services(w);
    refresh_checks(w);
}

/* Sprache gewechselt: Templates neu aufbauen, dann Seite 2 neu. */
static void on_lang_changed(Wizard *w)
{
    refresh_templates(w);
    refresh_page2(w);
}

/* ---- Zielordner wählen ---- */
static void on_folder_chosen(GObject *src, GAsyncResult *res, gpointer ud)
{
    Wizard *w = ud;
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f) {
        g_free(w->parent_dir);
        w->parent_dir = g_file_get_path(f);
        gtk_label_set_text(GTK_LABEL(w->dir_label), w->parent_dir);
        g_object_unref(f);
    }
}
static void on_choose_dir(GtkButton *b, gpointer ud)
{
    (void)b;
    Wizard *w = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Übergeordneten Ordner wählen");
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(w->win), NULL, on_folder_chosen, w);
    g_object_unref(dlg);
}

/* ---- Navigation ---- */
static void goto_page(Wizard *w, int page)
{
    gtk_stack_set_visible_child_name(GTK_STACK(w->stack), page == 0 ? "p1" : "p2");
    gtk_widget_set_visible(w->btn_back, page == 1);
    gtk_widget_set_visible(w->btn_next, page == 0);
    gtk_widget_set_visible(w->btn_finish, page == 1);
    if (page == 1) refresh_page2(w);
}

static void on_next(GtkButton *b, gpointer ud)
{
    (void)b;
    Wizard *w = ud;
    const char *name = gtk_editable_get_text(GTK_EDITABLE(w->name_entry));
    if (!name || !*name) {
        gtk_widget_grab_focus(w->name_entry);
        return;
    }
    goto_page(w, 1);
}
static void on_back(GtkButton *b, gpointer ud) { (void)b; goto_page((Wizard *)ud, 0); }

static void on_finish(GtkButton *b, gpointer ud)
{
    (void)b;
    Wizard *w = ud;
    const char *name = gtk_editable_get_text(GTK_EDITABLE(w->name_entry));
    const char *lang = lang_at(w);
    const char *tmpl = tmpl_at(w);

    gboolean want_db = tmpl_is_web(tmpl)
        && gtk_check_button_get_active(GTK_CHECK_BUTTON(w->db_check));

    char *root = g_build_filename(w->parent_dir, name, NULL);
    GError *e = NULL;
    if (project_create_files(root, name, lang, tmpl, want_db, &e)) {
        GtkWidget *win = w->win;
        App *app = w->app;
        gtk_window_destroy(GTK_WINDOW(win));
        project_open(app, root);
        /* erste Quelldatei öffnen, falls vorhanden. */
    } else {
        GtkAlertDialog *dlg = gtk_alert_dialog_new("Projekt konnte nicht angelegt werden");
        gtk_alert_dialog_set_detail(dlg, e ? e->message : "Unbekannter Fehler");
        gtk_alert_dialog_show(dlg, GTK_WINDOW(w->win));
        g_object_unref(dlg);
        g_clear_error(&e);
    }
    g_free(root);
}

static GtkWidget *field_row(const char *label, GtkWidget *control)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_set_size_request(l, 120, -1);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_widget_add_css_class(l, "hc-settings-key");
    gtk_box_append(GTK_BOX(row), l);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_box_append(GTK_BOX(row), control);
    return row;
}

static void wizard_free(gpointer ud) { Wizard *w = ud; g_free(w->parent_dir); g_free(w); }

void wizard_run(App *app)
{
    Wizard *w = g_new0(Wizard, 1);
    w->app = app;
    w->parent_dir = g_strdup(g_get_home_dir());

    GtkWidget *root = NULL;
    GtkWidget *win = hc_make_dialog(app, "Neues Projekt", 560, 480, &root);
    w->win = win;
    g_object_set_data_full(G_OBJECT(win), "wiz", w, wizard_free);

    w->stack = gtk_stack_new();
    gtk_widget_set_vexpand(w->stack, TRUE);
    gtk_stack_set_transition_type(GTK_STACK(w->stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);

    /* ---- Seite 1 ---- */
    GtkWidget *p1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_top(p1, 20); gtk_widget_set_margin_bottom(p1, 20);
    gtk_widget_set_margin_start(p1, 24); gtk_widget_set_margin_end(p1, 24);
    GtkWidget *h1 = gtk_label_new("1 · Projekt & Sprache");
    gtk_widget_add_css_class(h1, "hc-wizard-head");
    gtk_label_set_xalign(GTK_LABEL(h1), 0.0);
    gtk_box_append(GTK_BOX(p1), h1);

    w->name_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(w->name_entry), "z.B. mein_projekt");
    gtk_box_append(GTK_BOX(p1), field_row("Projektname", w->name_entry));

    w->lang_drop = gtk_drop_down_new_from_strings(LANGS);
    /* Sprachwechsel baut die Template-Liste neu auf. */
    g_signal_connect_swapped(w->lang_drop, "notify::selected",
                             G_CALLBACK(on_lang_changed), w);
    gtk_box_append(GTK_BOX(p1), field_row("Sprache", w->lang_drop));

    GtkWidget *dirrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    w->dir_label = gtk_label_new(w->parent_dir);
    gtk_label_set_ellipsize(GTK_LABEL(w->dir_label), PANGO_ELLIPSIZE_START);
    gtk_label_set_xalign(GTK_LABEL(w->dir_label), 0.0);
    gtk_widget_set_hexpand(w->dir_label, TRUE);
    gtk_widget_add_css_class(w->dir_label, "dim-label");
    GtkWidget *choose = gtk_button_new_with_label("Wählen…");
    g_signal_connect(choose, "clicked", G_CALLBACK(on_choose_dir), w);
    gtk_box_append(GTK_BOX(dirrow), w->dir_label);
    gtk_box_append(GTK_BOX(dirrow), choose);
    gtk_box_append(GTK_BOX(p1), field_row("Zielordner", dirrow));

    gtk_stack_add_named(GTK_STACK(w->stack), p1, "p1");

    /* ---- Seite 2 ---- */
    GtkWidget *p2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_top(p2, 20); gtk_widget_set_margin_bottom(p2, 20);
    gtk_widget_set_margin_start(p2, 24); gtk_widget_set_margin_end(p2, 24);
    GtkWidget *h2 = gtk_label_new("2 · Template & System-Check");
    gtk_widget_add_css_class(h2, "hc-wizard-head");
    gtk_label_set_xalign(GTK_LABEL(h2), 0.0);
    gtk_box_append(GTK_BOX(p2), h2);

    /* Modell kommt aus dem Routing und wird bei Sprachwechsel ersetzt. */
    w->tmpl_drop = gtk_drop_down_new(NULL, NULL);
    g_signal_connect_swapped(w->tmpl_drop, "notify::selected",
                             G_CALLBACK(refresh_page2), w);
    gtk_box_append(GTK_BOX(p2), field_row("Template", w->tmpl_drop));

    /* ---- Dienst-Optionen (erscheinen nur bei Webprojekten) ---- */
    w->svc_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(w->svc_box, 6);
    GtkWidget *svch = gtk_label_new("Dienste für dieses Projekt:");
    gtk_label_set_xalign(GTK_LABEL(svch), 0.0);
    gtk_box_append(GTK_BOX(w->svc_box), svch);

    w->db_check = gtk_check_button_new_with_label(
        "MySQL / MariaDB-Datenbank (eigenes Datenverzeichnis im Projekt)");
    /* Bei einem Webprojekt ist eine Datenbank der Normalfall. */
    gtk_check_button_set_active(GTK_CHECK_BUTTON(w->db_check), TRUE);
    g_signal_connect_swapped(w->db_check, "toggled",
                             G_CALLBACK(refresh_checks), w);
    gtk_box_append(GTK_BOX(w->svc_box), w->db_check);

    w->svc_note = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(w->svc_note), 0.0);
    gtk_label_set_wrap(GTK_LABEL(w->svc_note), TRUE);
    gtk_widget_add_css_class(w->svc_note, "dim-label");
    gtk_box_append(GTK_BOX(w->svc_box), w->svc_note);

    gtk_box_append(GTK_BOX(p2), w->svc_box);

    GtkWidget *csep = gtk_label_new("Verfügbare Werkzeuge (in der Sandbox):");
    gtk_label_set_xalign(GTK_LABEL(csep), 0.0);
    gtk_widget_set_margin_top(csep, 6);
    gtk_box_append(GTK_BOX(p2), csep);

    w->check_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(w->check_box, "hc-check-box");
    gtk_box_append(GTK_BOX(p2), w->check_box);

    gtk_stack_add_named(GTK_STACK(w->stack), p2, "p2");

    gtk_box_append(GTK_BOX(root), w->stack);

    /* ---- Fussleiste ---- */
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(foot, "hc-dialog-footer");
    GtkWidget *cancel = gtk_button_new_with_label("Abbrechen");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), win);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    w->btn_back = gtk_button_new_with_label("Zurück");
    g_signal_connect(w->btn_back, "clicked", G_CALLBACK(on_back), w);
    w->btn_next = gtk_button_new_with_label("Weiter");
    gtk_widget_add_css_class(w->btn_next, "suggested-action");
    g_signal_connect(w->btn_next, "clicked", G_CALLBACK(on_next), w);
    w->btn_finish = gtk_button_new_with_label("Erstellen");
    gtk_widget_add_css_class(w->btn_finish, "suggested-action");
    g_signal_connect(w->btn_finish, "clicked", G_CALLBACK(on_finish), w);

    gtk_box_append(GTK_BOX(foot), cancel);
    gtk_box_append(GTK_BOX(foot), spacer);
    gtk_box_append(GTK_BOX(foot), w->btn_back);
    gtk_box_append(GTK_BOX(foot), w->btn_next);
    gtk_box_append(GTK_BOX(foot), w->btn_finish);
    gtk_box_append(GTK_BOX(root), foot);

    gtk_window_set_child(GTK_WINDOW(win), root);
    /* Template-Liste passend zur voreingestellten Sprache aufbauen. */
    refresh_templates(w);
    goto_page(w, 0);
    gtk_window_present(GTK_WINDOW(win));
}
