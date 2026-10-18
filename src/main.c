#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include "version.h"
#include "app.h"
#include <stdio.h>
#include <string.h>

/* ============================================================
 * HyperCore Studio – IDE (GTK4). Zusammenbau des Hauptfensters.
 * Baut auf dem Design der "HyperCore"-Vorlage auf: eigene
 * (client-seitige) Titelleiste, dunkles Farbschema, dataNet.ovh.
 * ============================================================ */

/* ---------- Logging in Datei (~/.cache/hypercore/hypercore.log) ---------- */
static FILE *s_logf = NULL;

static GLogWriterOutput hc_log_writer(GLogLevelFlags level,
    const GLogField *fields, gsize n, gpointer ud)
{
    g_log_writer_standard_streams(level, fields, n, ud);  /* weiterhin stderr */
    if (s_logf) {
        const char *msg = NULL, *dom = NULL;
        for (gsize i = 0; i < n; i++) {
            if (!strcmp(fields[i].key, "MESSAGE")) msg = fields[i].value;
            else if (!strcmp(fields[i].key, "GLIB_DOMAIN")) dom = fields[i].value;
        }
        /* Nur Relevantes ins Logfile: Warnungen/Fehler/Meldungen und
         * alle eigenen (hypercore-)Ausgaben; GIO/dconf-Debug ausblenden. */
        GLogLevelFlags lvl = level & G_LOG_LEVEL_MASK;
        gboolean important = (lvl & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL |
                                     G_LOG_LEVEL_WARNING | G_LOG_LEVEL_MESSAGE)) != 0;
        gboolean ours = (dom && g_str_has_prefix(dom, "hypercore"));
        if (msg && (important || ours)) {
            GDateTime *t = g_date_time_new_now_local();
            char *ts = g_date_time_format(t, "%H:%M:%S");
            g_date_time_unref(t);
            fprintf(s_logf, "%s [%s] %s\n", ts, dom ? dom : "hypercore", msg);
            fflush(s_logf);
            g_free(ts);
        }
    }
    return G_LOG_WRITER_HANDLED;
}

static void hc_install_logging(void)
{
    char *dir = g_build_filename(g_get_user_cache_dir(), "hypercore", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_build_filename(dir, "hypercore.log", NULL);
    s_logf = fopen(path, "a");
    g_log_set_writer_func(hc_log_writer, NULL, NULL);
    g_message("=== %s %s gestartet === Log: %s", APP_NAME, APP_VERSION, path);
    g_free(path); g_free(dir);
}

/* ---------- gemeinsame Helfer ---------- */
void hc_mark_window(App *app, GtkWidget *w)
{
    gtk_widget_add_css_class(w, "hc");   /* Basis-Design der Linie */
    gtk_widget_add_css_class(w, "hypercore");
    if (app->settings.theme == HC_THEME_LIGHT)
        gtk_widget_add_css_class(w, "hc-light");
}

GtkWidget *hc_make_drag_area(GtkWidget *child, gboolean expand)
{
    GtkWidget *handle = gtk_window_handle_new();
    gtk_widget_set_hexpand(handle, expand);
    if (!child) {
        child = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(child, expand);
    }
    gtk_window_handle_set_child(GTK_WINDOW_HANDLE(handle), child);
    return handle;
}

void hc_apply_theme(App *app)
{
    gboolean light = (app->settings.theme == HC_THEME_LIGHT);

    /* Alle offenen Fenster der App aktualisieren. */
    GList *wins = gtk_application_get_windows(app->app);
    for (GList *l = wins; l; l = l->next) {
        GtkWidget *w = GTK_WIDGET(l->data);
        if (light) gtk_widget_add_css_class(w, "hc-light");
        else       gtk_widget_remove_css_class(w, "hc-light");
    }

    /* GTK-System-Prefer ebenfalls umschalten, damit Widget-Defaults stimmen. */
    GtkSettings *gs = gtk_settings_get_default();
    if (gs) g_object_set(gs, "gtk-application-prefer-dark-theme", !light, NULL);

    terminal_apply_theme(app);
}

/* ---------- CSS laden (neben Binary, CWD, Installationspfad) ---------- */
/* Stylesheet ist in die Binary einkompiliert (GResource-Monolith). */
static void load_css(App *app)
{
    app->css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(app->css_provider,
        "/ovh/datanet/hypercore/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(app->css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* Icon-Bild aus dem einkompilierten Icon-Set. */
GtkWidget *hc_icon(const char *name)
{
    char *path = g_strdup_printf("/ovh/datanet/hypercore/icons/%s.svg", name);
    GtkWidget *img = gtk_image_new_from_resource(path);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 18);
    g_free(path);
    return img;
}

/* Knopf mit eigenem Icon. */
GtkWidget *hc_icon_button(const char *name, const char *tip)
{
    GtkWidget *b = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(b), hc_icon(name));
    if (tip) gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_add_css_class(b, "flat");
    return b;
}

/* ============================================================
 * Wiederverwendbarer Dialog-Builder
 * Gibt das Fenster-Widget zurueck; content_box_out bekommt die
 * scrollbare innere Box (fuer den Inhalt des Dialogs).
 * ============================================================ */
GtkWidget *hc_make_dialog(App *app, const char *title,
                          int width, int height,
                          GtkWidget **content_box_out)
{
    GtkWidget *win = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(app->window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), width, height);
    hc_mark_window(app, win);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root, "hc-dialog-root");

    /* Titelleiste */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(bar, "hc-dialog-titlebar");
    GtkWidget *lbl = gtk_label_new(title);
    gtk_widget_add_css_class(lbl, "title");
    gtk_widget_set_margin_start(lbl, 14);
    GtkWidget *handle = gtk_window_handle_new();
    gtk_window_handle_set_child(GTK_WINDOW_HANDLE(handle), lbl);
    gtk_widget_set_hexpand(handle, TRUE);
    GtkWidget *wc = gtk_window_controls_new(GTK_PACK_END);
    gtk_window_controls_set_decoration_layout(GTK_WINDOW_CONTROLS(wc), ":close");
    gtk_box_append(GTK_BOX(bar), handle);
    gtk_box_append(GTK_BOX(bar), wc);
    gtk_box_append(GTK_BOX(root), bar);

    if (content_box_out) *content_box_out = root;
    gtk_window_set_child(GTK_WINDOW(win), root);
    return win;
}

/* ============================================================
 * Live-Aktivitätsstatus (alle Module schreiben hierher)
 * ============================================================ */
void hc_set_activity(App *app, const char *text)
{
    if (!app->st_activity || !app->st_spinner) return;

    if (text && *text) {
        gtk_label_set_text(GTK_LABEL(app->st_activity), text);
        gtk_widget_set_visible(app->st_activity, TRUE);
        gtk_widget_set_visible(app->st_spinner,  TRUE);
        gtk_spinner_start(GTK_SPINNER(app->st_spinner));
    } else {
        gtk_label_set_text(GTK_LABEL(app->st_activity), "");
        gtk_widget_set_visible(app->st_activity, FALSE);
        gtk_widget_set_visible(app->st_spinner,  FALSE);
        gtk_spinner_stop(GTK_SPINNER(app->st_spinner));
    }
}

/* ============================================================
 * Aktionen
 * ============================================================ */
static void act_new_project(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; wizard_run((App *)ud); }

static void on_folder_opened(GObject *src, GAsyncResult *res, gpointer ud)
{
    App *app = ud;
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f) {
        char *path = g_file_get_path(f);
        if (path) { project_open(app, path); g_free(path); }
        g_object_unref(f);
    }
}
static void act_open_folder(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    App *app = ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Projektordner öffnen");
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(app->window), NULL, on_folder_opened, app);
    g_object_unref(dlg);
}

static void act_save(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; editor_save_current((App *)ud); }
static void act_search(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; search_open_dialog((App *)ud); }
static void act_settings(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; settings_open_dialog((App *)ud); }
static void act_build(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; terminal_run_build((App *)ud); }
/* Run leitet selbst an die richtige Stelle: GUI-Projekte in den
 * Emulator, Webprojekte in die Live-Ansicht, alles andere ins Terminal.
 * Der Nutzer soll nicht wissen muessen, welcher Knopf fuer welchen
 * Projekttyp der richtige ist. */
static void act_run(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    App *app = ud;
    if (services_project_is_native_gui(app)) {
        HcService *sv = services_get(app, HC_SVC_EMULATOR);
        if (sv && sv->state == HC_SVC_RUNNING) { preview_run_broadway(app); return; }
    }
    if (services_project_is_web(app)) {
        EditorTab *t = editor_current(app);
        if (t) preview_auto_open(t);
        char *url = services_web_base_url(app);
        if (url) {
            char *m = g_strdup_printf("\x1b[92m▶ Webprojekt laeuft: %s\x1b[0m", url);
            terminal_feed_line(app, m);
            g_free(m); g_free(url);
            return;
        }
    }
    terminal_run_run(app);
}
static void act_emulator(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; preview_run_broadway((App *)ud); }      /* optional: Broadway  */
static void act_preview(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; editor_toggle_preview((App *)ud); }
static void act_snapshot(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; preview_snapshot_current((App *)ud); }
static void act_sandbox(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; inspector_show_status((App *)ud); }
static void act_rag(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; rag_manager_open((App *)ud); }
static void act_rag_rebuild(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; rag_index_project((App *)ud, TRUE); }
static void act_git_commit(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; git_commit_dialog((App *)ud); }
static void act_git_time(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; git_open_timemachine((App *)ud); }
static void act_git_latest(GSimpleAction *a, GVariant *p, gpointer ud)
{ (void)a; (void)p; git_return_latest((App *)ud); project_refresh_tree((App*)ud); }
static void act_toggle_term(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    App *app = ud;
    gboolean on = gtk_revealer_get_reveal_child(GTK_REVEALER(app->term_revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->term_revealer), !on);
}
static void act_quit(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    App *app = ud;
    session_save(app);
    g_application_quit(G_APPLICATION(app->app));
}

static void act_about(GSimpleAction *a, GVariant *p, gpointer ud)
{
    (void)a; (void)p;
    App *app = ud;

    GtkWidget *win = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(app->window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 560, 620);
    hc_mark_window(app, win);

    /* Root: eigene Titelleiste + Inhalt */
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root, "hc-dialog-root");

    /* Titelleiste */
    GtkWidget *titlebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(titlebar, "hc-dialog-titlebar");
    GtkWidget *wc = gtk_window_controls_new(GTK_PACK_END);
    gtk_window_controls_set_decoration_layout(GTK_WINDOW_CONTROLS(wc), ":close");
    GtkWidget *drag = gtk_label_new("Über " APP_NAME);
    gtk_widget_add_css_class(drag, "title");
    gtk_widget_set_hexpand(drag, TRUE);
    gtk_widget_set_margin_start(drag, 14);
    GtkWindowHandle *handle = (GtkWindowHandle *)gtk_window_handle_new();
    gtk_window_handle_set_child(handle, drag);
    gtk_widget_set_hexpand(GTK_WIDGET(handle), TRUE);
    gtk_box_append(GTK_BOX(titlebar), GTK_WIDGET(handle));
    gtk_box_append(GTK_BOX(titlebar), wc);
    gtk_box_append(GTK_BOX(root), titlebar);

    /* Scrollbarer Inhaltsbereich */
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(body, "hc-about-body");

    /* Logo – volle Breite, gross */
    GtkWidget *logo_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(logo_box, "hc-about-logo");
    gtk_widget_set_halign(logo_box, GTK_ALIGN_CENTER);
    GtkWidget *logo = gtk_image_new_from_resource("/ovh/datanet/hypercore/icons/logo.png");
    gtk_image_set_pixel_size(GTK_IMAGE(logo), 280);
    gtk_widget_set_halign(logo, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(logo_box), logo);
    gtk_box_append(GTK_BOX(body), logo_box);

    /* App-Name */
    GtkWidget *h = gtk_label_new(APP_NAME);
    gtk_widget_add_css_class(h, "hc-about-title");
    gtk_label_set_xalign(GTK_LABEL(h), 0.5);
    gtk_widget_set_margin_bottom(h, 4);
    gtk_box_append(GTK_BOX(body), h);

    /* Version */
    char ver[160];
    snprintf(ver, sizeof(ver), "Version %s  ·  Build %s", APP_VERSION, APP_BUILD_VERSION);
    GtkWidget *v = gtk_label_new(ver);
    gtk_widget_add_css_class(v, "hc-about-subtitle");
    gtk_label_set_xalign(GTK_LABEL(v), 0.5);
    gtk_widget_set_margin_bottom(v, 20);
    gtk_box_append(GTK_BOX(body), v);

    /* Trennlinie */
    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_bottom(sep1, 16);
    gtk_box_append(GTK_BOX(body), sep1);

    /* Beschreibung */
    GtkWidget *d = gtk_label_new(
        "Integrierte Entwicklungsumgebung (GTK4) der DATANET.OVH LTD, "
        "aufgebaut auf dem HyperCore-Fundament. Editor mit Syntax-Highlighting, "
        "Projektverwaltung, Terminal, Live-Preview und nativem KI-Agenten.");
    gtk_label_set_wrap(GTK_LABEL(d), TRUE);
    gtk_label_set_xalign(GTK_LABEL(d), 0.0);
    gtk_widget_add_css_class(d, "hc-about-desc");
    gtk_widget_set_margin_bottom(d, 16);
    gtk_box_append(GTK_BOX(body), d);

    /* Firma */
    GtkWidget *sep_firm = gtk_label_new("UNTERNEHMEN");
    gtk_widget_add_css_class(sep_firm, "hc-about-section");
    gtk_label_set_xalign(GTK_LABEL(sep_firm), 0.0);
    gtk_box_append(GTK_BOX(body), sep_firm);

    GtkWidget *reg = gtk_label_new(APP_COMPANY_REG);
    gtk_label_set_xalign(GTK_LABEL(reg), 0.0);
    gtk_widget_add_css_class(reg, "hc-about-key");
    gtk_widget_set_margin_bottom(reg, 8);
    gtk_box_append(GTK_BOX(body), reg);

    /* Links */
    GtkWidget *links = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_set_margin_bottom(links, 16);
    char *web = g_strdup_printf("<a href=\"%s\">%s</a>", APP_COMPANY_URL, APP_COMPANY_URL);
    GtkWidget *lw = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lw), web); g_free(web);
    char *mail = g_strdup_printf("<a href=\"mailto:%s\">%s</a>", APP_COMPANY_EMAIL, APP_COMPANY_EMAIL);
    GtkWidget *lm = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lm), mail); g_free(mail);
    gtk_box_append(GTK_BOX(links), lw);
    gtk_box_append(GTK_BOX(links), lm);
    gtk_box_append(GTK_BOX(body), links);

    GtkWidget *c = gtk_label_new(APP_COPYRIGHT);
    gtk_label_set_xalign(GTK_LABEL(c), 0.0);
    gtk_widget_add_css_class(c, "hc-about-key");
    gtk_widget_set_margin_bottom(c, 16);
    gtk_box_append(GTK_BOX(body), c);

    /* Lizenz */
    GtkWidget *sep_lic = gtk_label_new("LIZENZ");
    gtk_widget_add_css_class(sep_lic, "hc-about-section");
    gtk_label_set_xalign(GTK_LABEL(sep_lic), 0.0);
    gtk_box_append(GTK_BOX(body), sep_lic);

    GtkWidget *licscroll = gtk_scrolled_window_new();
    gtk_widget_set_size_request(licscroll, -1, 130);
    gtk_widget_set_margin_top(licscroll, 8);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(licscroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *lic = gtk_label_new(
        APP_NAME " - Endbenutzer-Lizenzvereinbarung (Auszug)\n\n"
        "Diese Software wird von der " APP_COMPANY " bereitgestellt. Die Nutzung "
        "erfolgt gemass der mitgelieferten Lizenzbedingungen. Alle Marken- und "
        "Namensrechte verbleiben bei der " APP_COMPANY ".\n\n"
        "Die Software wird ohne Maengelgewaehr (\"as is\") bereitgestellt; eine "
        "Haftung fuer mittelbare Schaeden ist im gesetzlich zulaessigen Rahmen "
        "ausgeschlossen. Komponenten von Drittanbietern (GTK, GtkSourceView, "
        "VTE, WebKitGTK, json-glib, libsoup) unterliegen ihren jeweiligen "
        "Open-Source-Lizenzen.\n\n"
        "Vollstaendiger Lizenztext: siehe /usr/share/doc/hypercore-studio/.");
    gtk_label_set_wrap(GTK_LABEL(lic), TRUE);
    gtk_label_set_xalign(GTK_LABEL(lic), 0.0);
    gtk_label_set_yalign(GTK_LABEL(lic), 0.0);
    gtk_widget_add_css_class(lic, "hc-about-desc");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(licscroll), lic);
    gtk_box_append(GTK_BOX(body), licscroll);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), body);
    gtk_box_append(GTK_BOX(root), scroll);
    gtk_window_set_child(GTK_WINDOW(win), root);
    gtk_window_present(GTK_WINDOW(win));
}

static void on_dark_toggle(GtkToggleButton *btn, gpointer ud)
{
    App *app = ud;
    app->settings.theme = gtk_toggle_button_get_active(btn) ? HC_THEME_DARK : HC_THEME_LIGHT;
    settings_save(&app->settings);
    hc_apply_theme(app);
    editor_apply_settings_all(app);
}

/* ---- Outline/Agent-Umschaltung ---- */
static void on_show_outline(GtkButton *b, gpointer ud)
{ (void)b; App *app = ud;
  gtk_revealer_set_reveal_child(GTK_REVEALER(app->right_revealer), TRUE);
  gtk_stack_set_visible_child_name(GTK_STACK(app->right_stack), "outline");
  editor_refresh_outline(app); }
static void on_show_agent(GtkButton *b, gpointer ud)
{ (void)b; App *app = ud;
  gtk_revealer_set_reveal_child(GTK_REVEALER(app->right_revealer), TRUE);
  gtk_stack_set_visible_child_name(GTK_STACK(app->right_stack), "agent"); }
static void on_toggle_sidebar(GtkButton *b, gpointer ud)
{ (void)b; App *app = ud;
  gboolean on = gtk_revealer_get_reveal_child(GTK_REVEALER(app->right_revealer));
  gtk_revealer_set_reveal_child(GTK_REVEALER(app->right_revealer), !on); }

/* ============================================================
 * Titelleiste (eigene CSD) mit Menü + Dark-Toggle
 * ============================================================ */
static GMenu *build_menu(void)
{
    GMenu *root = g_menu_new();

    GMenu *file = g_menu_new();
    g_menu_append(file, "Neues Projekt…", "app.new_project");
    g_menu_append(file, "Ordner öffnen…", "app.open_folder");
    g_menu_append(file, "Speichern", "app.save");
    GMenu *fq = g_menu_new();
    g_menu_append(fq, "Beenden", "app.quit");
    g_menu_append_section(file, NULL, G_MENU_MODEL(fq));
    g_object_unref(fq);
    g_menu_append_submenu(root, "Datei", G_MENU_MODEL(file));
    g_object_unref(file);

    GMenu *edit = g_menu_new();
    g_menu_append(edit, "Suchen & Ersetzen…", "app.search");
    g_menu_append(edit, "Einstellungen…", "app.settings");
    g_menu_append_submenu(root, "Bearbeiten", G_MENU_MODEL(edit));
    g_object_unref(edit);

    GMenu *git = g_menu_new();
    g_menu_append(git, "Commit… (KI-Message)", "app.git_commit");
    g_menu_append(git, "Time Machine…", "app.git_time");
    g_menu_append(git, "Zurück zu Latest", "app.git_latest");
    g_menu_append_submenu(root, "Git", G_MENU_MODEL(git));
    g_object_unref(git);

    GMenu *tools = g_menu_new();
    g_menu_append(tools, "Build", "app.build");
    g_menu_append(tools, "Ausführen", "app.run");
    g_menu_append(tools, "Im Emulator starten (Broadway)", "app.emulator");
    g_menu_append(tools, "GUI-Snapshot", "app.snapshot");
    g_menu_append(tools, "Live-Preview umschalten", "app.preview");
    g_menu_append(tools, "Maschinenraum umschalten", "app.toggle_term");
    GMenu *tr = g_menu_new();
    g_menu_append(tr, "Sandbox-Inspektor…", "app.sandbox");
    g_menu_append(tr, "RAG-Manager…", "app.rag");
    g_menu_append(tr, "RAG neu aufbauen", "app.rag_rebuild");
    g_menu_append_section(tools, NULL, G_MENU_MODEL(tr));
    g_object_unref(tr);
    g_menu_append_submenu(root, "Werkzeuge", G_MENU_MODEL(tools));
    g_object_unref(tools);

    /* ---- Hilfe-Menü ---- */
    GMenu *help = g_menu_new();
    g_menu_append(help, "Über " APP_NAME, "app.about");
    #ifdef HC_DEBUG
    g_menu_append(help, "[DEBUG] System-Prompt & Agent-Test", "app.debug_win");
    #endif
    g_menu_append_submenu(root, "Hilfe", G_MENU_MODEL(help));
    g_object_unref(help);

    return root;
}

static GtkWidget *build_titlebar(App *app)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(bar, "hc-titlebar");
    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 4);

    GtkWidget *title = gtk_label_new(APP_NAME);
    gtk_widget_add_css_class(title, "title");
    gtk_box_append(GTK_BOX(bar), hc_make_drag_area(title, FALSE));

    GtkWidget *menubar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(build_menu()));
    gtk_widget_add_css_class(menubar, "hc-menubar");
    gtk_box_append(GTK_BOX(bar), menubar);

    gtk_box_append(GTK_BOX(bar), hc_make_drag_area(NULL, TRUE));

    /* Git: "Zurück zu Latest" (nur bei Zeitreise sichtbar) + Branch. */
    app->git_latest_btn = gtk_button_new_with_label("⏱ Zurück zu Latest");
    gtk_widget_add_css_class(app->git_latest_btn, "hc-latest-btn");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(app->git_latest_btn), "app.git_latest");
    gtk_widget_set_visible(app->git_latest_btn, FALSE);
    gtk_box_append(GTK_BOX(bar), app->git_latest_btn);

    app->git_branch_lbl = gtk_label_new("⎇ —");
    gtk_widget_add_css_class(app->git_branch_lbl, "hc-branch");
    gtk_widget_set_tooltip_text(app->git_branch_lbl, "Aktiver Git-Branch");
    gtk_box_append(GTK_BOX(bar), app->git_branch_lbl);

    /* Git-Schnellbuttons (nur sichtbar wenn GitHub oder Gitea konfiguriert) */
    app->git_push_btn = hc_icon_button("go-up-symbolic", "Push (origin HEAD)");
    gtk_widget_add_css_class(app->git_push_btn, "flat");
    g_signal_connect_swapped(app->git_push_btn, "clicked",
                             G_CALLBACK(git_push_dialog), app);
    gtk_widget_set_visible(app->git_push_btn,
        (app->settings.github_token && *app->settings.github_token) ||
        (app->settings.gitea_token  && *app->settings.gitea_token));
    gtk_box_append(GTK_BOX(bar), app->git_push_btn);

    app->git_remote_btn = hc_icon_button("network-server-symbolic", "Remote Repositories");
    gtk_widget_add_css_class(app->git_remote_btn, "flat");
    g_signal_connect_swapped(app->git_remote_btn, "clicked",
                             G_CALLBACK(git_remote_panel_open), app);
    gtk_widget_set_visible(app->git_remote_btn,
        (app->settings.github_token && *app->settings.github_token) ||
        (app->settings.gitea_token  && *app->settings.gitea_token));
    gtk_box_append(GTK_BOX(bar), app->git_remote_btn);

    /* Dark-Mode-Toggle. */
    GtkWidget *dark = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(dark), "weather-clear-night-symbolic");
    gtk_widget_set_tooltip_text(dark, "Dunkel / Hell");
    gtk_widget_add_css_class(dark, "flat");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dark),
        app->settings.theme != HC_THEME_LIGHT);
    g_signal_connect(dark, "toggled", G_CALLBACK(on_dark_toggle), app);
    gtk_box_append(GTK_BOX(bar), dark);

    GtkWidget *controls = gtk_window_controls_new(GTK_PACK_END);
    gtk_window_controls_set_decoration_layout(GTK_WINDOW_CONTROLS(controls),
        "minimize,maximize,close");
    gtk_box_append(GTK_BOX(bar), controls);
    return bar;
}

/* ============================================================
 * Toolbar
 * ============================================================ */
static GtkWidget *tool_btn(const char *icon, const char *tip, const char *action)
{
    GtkWidget *b = hc_icon_button(icon, tip);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    return b;
}
static GtkWidget *sep(void) {
    GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(s, "hc-toolsep");
    return s;
}
static GtkWidget *labeled_btn(const char *icon, const char *label,
                              const char *tip, const char *action, gboolean accent)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *bc = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_append(GTK_BOX(bc), hc_icon(icon));
    gtk_box_append(GTK_BOX(bc), gtk_label_new(label));
    gtk_button_set_child(GTK_BUTTON(b), bc);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    gtk_widget_add_css_class(b, accent ? "hc-run" : "flat");
    return b;
}

static GtkWidget *build_toolbar(App *app)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(bar, "hc-toolbar");
    gtk_widget_set_margin_start(bar, 8); gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 5); gtk_widget_set_margin_bottom(bar, 5);

    gtk_box_append(GTK_BOX(bar), tool_btn("new-project", "Neues Projekt (Strg+N)", "app.new_project"));
    gtk_box_append(GTK_BOX(bar), tool_btn("open", "Ordner öffnen (Strg+O)", "app.open_folder"));
    gtk_box_append(GTK_BOX(bar), tool_btn("save", "Speichern (Strg+S)", "app.save"));
    gtk_box_append(GTK_BOX(bar), sep());

    gtk_box_append(GTK_BOX(bar), labeled_btn("build", "Build", "Build (Strg+B)", "app.build", FALSE));
    gtk_box_append(GTK_BOX(bar), labeled_btn("run", "Run", "Ausführen (F5)", "app.run", TRUE));

    gtk_box_append(GTK_BOX(bar), sep());
    gtk_box_append(GTK_BOX(bar), tool_btn("search", "Suchen (Strg+F)", "app.search"));
    gtk_box_append(GTK_BOX(bar), tool_btn("preview", "Live-Preview (Strg+P)", "app.preview"));
    gtk_box_append(GTK_BOX(bar), tool_btn("snapshot", "GUI-Snapshot", "app.snapshot"));

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(bar), spacer);

    /* Sandbox-Status (klickbar -> Inspektor). */
    app->sandbox_status_btn = hc_icon_button("sandbox", "Sandbox-Status");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(app->sandbox_status_btn), "app.sandbox");
    gtk_box_append(GTK_BOX(bar), app->sandbox_status_btn);

    gtk_box_append(GTK_BOX(bar), tool_btn("terminal", "Maschinenraum (Strg+`)", "app.toggle_term"));
    GtkWidget *side = hc_icon_button("sidebar", "Sidebar umschalten");
    g_signal_connect(side, "clicked", G_CALLBACK(on_toggle_sidebar), app);
    gtk_box_append(GTK_BOX(bar), side);

    return bar;
}

/* ============================================================
 * Rechte Sidebar (Outline / Agent)
 * ============================================================ */
static GtkWidget *build_right_sidebar(App *app)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "hc-sidebar");
    gtk_widget_set_size_request(box, 300, -1);

    /* Umschalt-Leiste. */
    GtkWidget *sw = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(sw, "hc-sidebar-switch");
    gtk_widget_set_margin_start(sw, 6); gtk_widget_set_margin_end(sw, 6);
    gtk_widget_set_margin_top(sw, 6); gtk_widget_set_margin_bottom(sw, 2);
    GtkWidget *bo = gtk_button_new();
    GtkWidget *boc = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(boc), gtk_image_new_from_icon_name("view-list-symbolic"));
    gtk_box_append(GTK_BOX(boc), gtk_label_new("Outline"));
    gtk_button_set_child(GTK_BUTTON(bo), boc);
    gtk_widget_add_css_class(bo, "flat");
    gtk_widget_set_hexpand(bo, TRUE);
    g_signal_connect(bo, "clicked", G_CALLBACK(on_show_outline), app);
    GtkWidget *ba = gtk_button_new();
    GtkWidget *bac = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(bac), gtk_image_new_from_icon_name("chat-symbolic"));
    gtk_box_append(GTK_BOX(bac), gtk_label_new("KI-Agent"));
    gtk_button_set_child(GTK_BUTTON(ba), bac);
    gtk_widget_add_css_class(ba, "flat");
    gtk_widget_set_hexpand(ba, TRUE);
    g_signal_connect(ba, "clicked", G_CALLBACK(on_show_agent), app);
    gtk_box_append(GTK_BOX(sw), bo);
    gtk_box_append(GTK_BOX(sw), ba);
    gtk_box_append(GTK_BOX(box), sw);

    app->right_stack = gtk_stack_new();
    gtk_widget_set_vexpand(app->right_stack, TRUE);

    /* Outline. */
    GtkWidget *oscroll = gtk_scrolled_window_new();
    app->outline_list = gtk_list_box_new();
    gtk_widget_add_css_class(app->outline_list, "hc-outline");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(oscroll), app->outline_list);
    gtk_stack_add_named(GTK_STACK(app->right_stack), oscroll, "outline");

    /* Agent. */
    GtkWidget *agent = agent_panel_new(app);
    gtk_stack_add_named(GTK_STACK(app->right_stack), agent, "agent");

    gtk_box_append(GTK_BOX(box), app->right_stack);
    return box;
}

/* ============================================================
 * Hauptfenster
 * ============================================================ */
static gboolean on_close(GtkWindow *w, gpointer ud)
{ (void)w; session_save((App *)ud); return FALSE; }

static void on_activate(GtkApplication *gapp, gpointer ud)
{
    App *app = ud;
    /* bei erneuter Aktivierung nur Fenster zeigen */
    if (app->window) { gtk_window_present(GTK_WINDOW(app->window)); return; }

    settings_load(&app->settings);
    custom_tools_reload(app);   /* ~/.config/hypercore/tools/ laden */
    load_css(app);
    sandbox_init(app);
    services_init(app);

    gtk_source_init();

    GtkWidget *window = gtk_application_window_new(gapp);
    app->window = window;
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 760);
    gtk_window_set_title(GTK_WINDOW(window), APP_NAME);
    hc_mark_window(app, window);
    gtk_window_set_titlebar(GTK_WINDOW(window), build_titlebar(app));
    gtk_window_set_icon_name(GTK_WINDOW(window), "hypercore-studio");

    /* ---- Aktionen registrieren ---- */
    static const struct { const char *name; GCallback cb; } acts[] = {
        { "new_project", G_CALLBACK(act_new_project) },
        { "open_folder", G_CALLBACK(act_open_folder) },
        { "save",        G_CALLBACK(act_save) },
        { "search",      G_CALLBACK(act_search) },
        { "settings",    G_CALLBACK(act_settings) },
        { "build",       G_CALLBACK(act_build) },
        { "run",         G_CALLBACK(act_run) },
        { "preview",     G_CALLBACK(act_preview) },
        { "emulator",    G_CALLBACK(act_emulator) },
        { "snapshot",    G_CALLBACK(act_snapshot) },
        { "sandbox",     G_CALLBACK(act_sandbox) },
        { "rag",         G_CALLBACK(act_rag) },
        { "rag_rebuild", G_CALLBACK(act_rag_rebuild) },
        { "git_commit",  G_CALLBACK(act_git_commit) },
        { "git_time",    G_CALLBACK(act_git_time) },
        { "git_latest",  G_CALLBACK(act_git_latest) },
        { "toggle_term", G_CALLBACK(act_toggle_term) },
        { "about",       G_CALLBACK(act_about) },
        #ifdef HC_DEBUG
        { "debug_win",   G_CALLBACK(+[](GSimpleAction*a,GVariant*p,gpointer ud){ (void)a;(void)p; debug_window_open((App*)ud); }) },
        #endif
        { "quit",        G_CALLBACK(act_quit) },
    };
    for (guint i = 0; i < G_N_ELEMENTS(acts); i++) {
        GSimpleAction *a = g_simple_action_new(acts[i].name, NULL);
        g_signal_connect(a, "activate", acts[i].cb, app);
        g_action_map_add_action(G_ACTION_MAP(gapp), G_ACTION(a));
    }
    struct { const char *action; const char *accel; } ac[] = {
        { "app.save", "<Control>s" }, { "app.search", "<Control>f" },
        { "app.build", "<Control>b" }, { "app.run", "F5" },
        { "app.new_project", "<Control>n" }, { "app.open_folder", "<Control>o" },
        { "app.toggle_term", "<Control>grave" }, { "app.preview", "<Control>p" },
        { "app.git_commit", "<Control>k" }, { "app.snapshot", "<Control><Shift>s" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(ac); i++) {
        const char *accels[] = { ac[i].accel, NULL };
        gtk_application_set_accels_for_action(gapp, ac[i].action, accels);
    }

    /* ---- Root-Layout ---- */
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root, "hc-root");

    gtk_box_append(GTK_BOX(root), build_toolbar(app));

    /* äußeres Paned: Baum | Rest */
    GtkWidget *outer = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(outer, TRUE);

    /* Projekt-Baum links. */
    GtkWidget *treebox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(treebox, "hc-treepanel");
    gtk_widget_set_size_request(treebox, 220, -1);
    GtkWidget *treehdr = gtk_label_new("PROJEKT");
    gtk_widget_add_css_class(treehdr, "hc-panel-head");
    gtk_label_set_xalign(GTK_LABEL(treehdr), 0.0);
    gtk_widget_set_margin_start(treehdr, 10); gtk_widget_set_margin_top(treehdr, 8);
    gtk_widget_set_margin_bottom(treehdr, 4);
    gtk_box_append(GTK_BOX(treebox), treehdr);
    GtkWidget *tscroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(tscroll, TRUE);
    app->tree_view = gtk_list_view_new(NULL, NULL);
    gtk_widget_add_css_class(app->tree_view, "hc-tree");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tscroll), app->tree_view);
    gtk_box_append(GTK_BOX(treebox), tscroll);
    gtk_paned_set_start_child(GTK_PANED(outer), treebox);
    gtk_paned_set_resize_start_child(GTK_PANED(outer), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(outer), FALSE);

    /* vertikales Paned: Mitte / Terminal */
    GtkWidget *vpaned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);

    /* horizontales Paned: Editor | Sidebar */
    GtkWidget *mid = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    /* Editor-Bereich: Stack Platzhalter<->Notebook */
    app->notebook_stack = gtk_stack_new();
    gtk_widget_set_hexpand(app->notebook_stack, TRUE);
    gtk_widget_set_vexpand(app->notebook_stack, TRUE);
    app->placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(app->placeholder, "hc-placeholder");
    gtk_widget_set_valign(app->placeholder, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(app->placeholder, GTK_ALIGN_CENTER);
    GtkWidget *ph_icon = gtk_image_new_from_icon_name("text-editor-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(ph_icon), 64);
    gtk_widget_add_css_class(ph_icon, "dim-label");
    GtkWidget *ph_lbl = gtk_label_new("Keine Datei geöffnet");
    gtk_widget_add_css_class(ph_lbl, "dim-label");
    GtkWidget *ph_hint = gtk_label_new("Datei im Projektbaum wählen oder neues Projekt anlegen");
    gtk_widget_add_css_class(ph_hint, "dim-label");
    gtk_box_append(GTK_BOX(app->placeholder), ph_icon);
    gtk_box_append(GTK_BOX(app->placeholder), ph_lbl);
    gtk_box_append(GTK_BOX(app->placeholder), ph_hint);
    gtk_stack_add_named(GTK_STACK(app->notebook_stack), app->placeholder, "placeholder");

    app->notebook = gtk_notebook_new();
    gtk_widget_add_css_class(app->notebook, "hc-notebook");
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(app->notebook), TRUE);
    g_signal_connect_swapped(app->notebook, "switch-page",
                             G_CALLBACK(editor_update_status), app);
    g_signal_connect_swapped(app->notebook, "switch-page",
                             G_CALLBACK(editor_refresh_outline), app);
    gtk_stack_add_named(GTK_STACK(app->notebook_stack), app->notebook, "notebook");
    gtk_paned_set_start_child(GTK_PANED(mid), app->notebook_stack);
    gtk_paned_set_resize_start_child(GTK_PANED(mid), TRUE);

    /* rechte Sidebar im Revealer. */
    app->right_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(app->right_revealer),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_revealer_set_child(GTK_REVEALER(app->right_revealer), build_right_sidebar(app));
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->right_revealer), TRUE);
    gtk_paned_set_end_child(GTK_PANED(mid), app->right_revealer);
    gtk_paned_set_resize_end_child(GTK_PANED(mid), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(mid), FALSE);

    gtk_paned_set_start_child(GTK_PANED(vpaned), mid);
    gtk_paned_set_resize_start_child(GTK_PANED(vpaned), TRUE);

    /* Terminal unten im Revealer. */
    app->term_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(app->term_revealer),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    GtkWidget *termbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(termbox, "hc-termbox");
    gtk_widget_set_size_request(termbox, -1, 200);
    /* Problemleiste (Smart Errors). */
    app->problems_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(app->problems_bar, "hc-problems");
    gtk_widget_set_visible(app->problems_bar, FALSE);
    gtk_box_append(GTK_BOX(termbox), app->problems_bar);
    /* Maschinenraum als Reiter: Konsole, Dienste, Mail-Catcher.
     * So ist jederzeit sichtbar, was im Hintergrund wirklich laeuft. */
    GtkWidget *mach = gtk_notebook_new();
    gtk_widget_add_css_class(mach, "hc-machine");
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(mach), GTK_POS_TOP);
    gtk_widget_set_vexpand(mach, TRUE);

    gtk_notebook_append_page(GTK_NOTEBOOK(mach), terminal_new(app),
                             gtk_label_new("Konsole"));

    gtk_notebook_append_page(GTK_NOTEBOOK(mach), services_panel_new(app),
                             gtk_label_new("Dienste"));

    /* Mail-Catcher-Reiter. */
    GtkWidget *mailbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *mailhdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(mailhdr, 10); gtk_widget_set_margin_top(mailhdr, 6);
    gtk_widget_set_margin_end(mailhdr, 10);
    GtkWidget *mailinfo = gtk_label_new("Abgefangene Mails (mail() aus dem Projekt)");
    gtk_label_set_xalign(GTK_LABEL(mailinfo), 0.0);
    gtk_widget_set_hexpand(mailinfo, TRUE);
    gtk_widget_add_css_class(mailinfo, "dim-label");
    gtk_box_append(GTK_BOX(mailhdr), mailinfo);
    GtkWidget *mailclr = gtk_button_new_with_label("Leeren");
    gtk_widget_add_css_class(mailclr, "flat");
    g_signal_connect_swapped(mailclr, "clicked", G_CALLBACK(services_mails_clear), app);
    gtk_box_append(GTK_BOX(mailhdr), mailclr);
    gtk_box_append(GTK_BOX(mailbox), mailhdr);
    GtkWidget *mailscr = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(mailscr, TRUE);
    app->mail_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(app->mail_list, 10);
    gtk_widget_set_margin_end(app->mail_list, 10);
    gtk_widget_set_margin_bottom(app->mail_list, 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(mailscr), app->mail_list);
    gtk_box_append(GTK_BOX(mailbox), mailscr);
    gtk_notebook_append_page(GTK_NOTEBOOK(mach), mailbox, gtk_label_new("Mail"));
    services_mail_panel_refresh(app);

    gtk_box_append(GTK_BOX(termbox), mach);
    gtk_revealer_set_child(GTK_REVEALER(app->term_revealer), termbox);
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->term_revealer), TRUE);
    gtk_paned_set_end_child(GTK_PANED(vpaned), app->term_revealer);
    gtk_paned_set_resize_end_child(GTK_PANED(vpaned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(vpaned), FALSE);

    gtk_paned_set_end_child(GTK_PANED(outer), vpaned);
    gtk_paned_set_resize_end_child(GTK_PANED(outer), TRUE);

    gtk_box_append(GTK_BOX(root), outer);

    /* ---- Statusleiste ---- */
    GtkWidget *sb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(sb, "hc-statusbar");

    /* Links: Projektname */
    app->st_project = gtk_label_new("Kein Projekt");
    gtk_widget_set_halign(app->st_project, GTK_ALIGN_START);
    gtk_widget_set_margin_start(app->st_project, 8);

    /* Mitte links: Dateiinfo */
    GtkWidget *sp1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(sp1, FALSE);
    gtk_widget_set_size_request(sp1, 20, -1);
    app->st_fileinfo = gtk_label_new("—");

    /* Mitte: Live-Aktivitäts-Bereich (Spinner + Label) –
     * expandiert und zentriert sich zwischen Dateiinfo und Version. */
    GtkWidget *act_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_hexpand(act_box, TRUE);
    gtk_widget_set_halign(act_box, GTK_ALIGN_CENTER);
    app->st_spinner  = gtk_spinner_new();
    gtk_widget_set_visible(app->st_spinner, FALSE);
    app->st_activity = gtk_label_new("");
    gtk_widget_add_css_class(app->st_activity, "hc-activity");
    gtk_widget_set_visible(app->st_activity, FALSE);
    gtk_box_append(GTK_BOX(act_box), app->st_spinner);
    gtk_box_append(GTK_BOX(act_box), app->st_activity);

    /* Rechts: Version */
    GtkWidget *sp2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(sp2, 20, -1);
    char vbuf[96];
    snprintf(vbuf, sizeof(vbuf), "%s  ·  v%s", APP_NAME, APP_VERSION);
    app->st_version = gtk_label_new(vbuf);
    gtk_widget_set_halign(app->st_version, GTK_ALIGN_END);
    gtk_widget_set_margin_end(app->st_version, 8);

    gtk_box_append(GTK_BOX(sb), app->st_project);
    gtk_box_append(GTK_BOX(sb), sp1);
    gtk_box_append(GTK_BOX(sb), app->st_fileinfo);
    gtk_box_append(GTK_BOX(sb), act_box);
    gtk_box_append(GTK_BOX(sb), sp2);
    gtk_box_append(GTK_BOX(sb), app->st_version);
    gtk_box_append(GTK_BOX(root), sb);

    /* Ladescreen-Overlay über dem Arbeitsbereich. */
    app->overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(app->overlay), root);
    sandbox_build_overlay(app);
    gtk_overlay_add_overlay(GTK_OVERLAY(app->overlay), app->loading_box);

    gtk_window_set_child(GTK_WINDOW(window), app->overlay);
    g_signal_connect(window, "close-request", G_CALLBACK(on_close), app);

    /* Aktivierungs-Signale der Module anschließen. */
    editor_connect_outline(app);
    project_setup_tree_view(app);

    hc_apply_theme(app);
    editor_apply_settings_all(app);
    gtk_widget_set_visible(window, TRUE);

    /* Startprojekt bestimmen: Argument > zuletzt geöffnetes Projekt.
     * Gibt es kein gültiges Projekt (kein .hcs), startet der Wizard. */
    const char *arg = g_object_get_data(G_OBJECT(gapp), "open-arg");
    char *candidate = NULL;
    if (arg && g_file_test(arg, G_FILE_TEST_IS_DIR))
        candidate = g_strdup(arg);
    else {
        char *last = project_last_path();
        if (last && g_file_test(last, G_FILE_TEST_IS_DIR)) candidate = last;
        else g_free(last);
    }

    if (candidate && project_dir_has_ght(candidate))
        project_open(app, candidate);
    else if (!g_getenv("HC_AGENT_TEST"))
        wizard_run(app);   /* kein Projekt -> immer Wizard               */
    g_free(candidate);

    /* Test-Hook (nur mit gesetzter Umgebungsvariable): einmalig eine
     * Agenten-Anfrage absetzen, um den Tool-Calling-Loop zu prüfen. */
    if (g_getenv("HC_AGENT_TEST")) {
        const char *arg2 = g_object_get_data(G_OBJECT(gapp), "open-arg");
        if (arg2 && g_file_test(arg2, G_FILE_TEST_IS_DIR) && !app->proj_root)
            project_open(app, arg2);
        agent_send_user_message(app, "Lies src/main.c und fasse den Code kurz zusammen.");
    }

    /* Terminal initial eingeklappt lassen? Nein – sichtbar für sofortige Nutzung. */
}

int main(int argc, char **argv)
{
    App *app = g_new0(App, 1);
    hc_install_logging();
    git_startup();  /* libgit2 initialisieren */
    app->app = gtk_application_new(APP_ID, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app->app, "activate", G_CALLBACK(on_activate), app);
    if (argc > 1)
        g_object_set_data_full(G_OBJECT(app->app), "open-arg",
                               g_strdup(argv[1]), g_free);
    int status = g_application_run(G_APPLICATION(app->app), 1, argv);
    settings_free(&app->settings);
    g_object_unref(app->app);
    g_free(app);
    return status;
}
