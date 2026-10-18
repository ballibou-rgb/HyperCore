#include "app.h"
#include "version.h"
#include <webkit/webkit.h>
#include <string.h>

/* ============================================================
 * Live-Preview & Emulatoren.
 *
 * Grundsatz: Die Vorschau ist SOFORT da und folgt dem Tippen – nicht
 * erst nach einem Klick und nicht erst nach einem Build.
 *
 *  - GTK XML (.ui): wird bei jedem Tastendruck direkt aus dem Editor-
 *    puffer mit GtkBuilder neu gezeichnet. Kein Speichern, kein Build.
 *    Bei (zwischenzeitlich) ungültigem XML bleibt die letzte gültige
 *    Ansicht stehen und oben erscheint ein Hinweisstreifen – die
 *    Vorschau flackert also nicht bei jedem halbfertigen Tag.
 *  - HTML/CSS/JS: wird direkt aus dem Puffer gerendert (ebenfalls ohne
 *    Speichern).
 *  - PHP: braucht den laufenden Webserver aus services.c; die Datei
 *    wird vom Auto-Save geschrieben und die Ansicht nachgeladen.
 *  - Native GUI (C/GTK, Python): läuft in der Sandbox und streamt ihr
 *    Interface über Broadway in den Emulator-Tab. Der Emulator wartet
 *    auf einen echten Port-Health-Check, statt blind zu laden.
 * ============================================================ */

/* ============================================================
 * Dateityp-Erkennung
 * ============================================================ */
static gboolean path_is_ui(const char *path)
{
    return path && (g_str_has_suffix(path, ".ui") || g_str_has_suffix(path, ".glade"));
}

static gboolean path_is_web(const char *path)
{
    const char *lang = hc_lang_for_path(path);
    if (!lang) return FALSE;
    return (!strcmp(lang, "html") || !strcmp(lang, "php") ||
            !strcmp(lang, "css")  || !strcmp(lang, "js"));
}

gboolean preview_available_for(const char *path)
{
    return path_is_ui(path) || path_is_web(path);
}

/* ============================================================
 * Web-Vorschau
 * ============================================================ */

/* URL der Datei auf dem laufenden Webserver. */
static char *web_uri_for(EditorTab *t)
{
    App *app = t->app;
    char *base = services_web_base_url(app);
    if (!base) return NULL;

    /* Pfad relativ zur Dokumentwurzel des Servers bestimmen. */
    char *rel = NULL;
    if (app->proj_root && g_str_has_prefix(t->path, app->proj_root)) {
        const char *r = t->path + strlen(app->proj_root);
        while (*r == '/') r++;
        /* public/ bzw. web/ ist die Dokumentwurzel -> Präfix entfernen. */
        if (g_str_has_prefix(r, "public/")) r += 7;
        else if (g_str_has_prefix(r, "web/")) r += 4;
        rel = g_strdup(r);
    } else {
        rel = g_path_get_basename(t->path);
    }
    char *uri = g_strdup_printf("%s/%s", base, rel);
    g_free(rel);
    g_free(base);
    return uri;
}

static void set_status(GtkWidget *panel, const char *text, gboolean warn)
{
    GtkWidget *lbl = g_object_get_data(G_OBJECT(panel), "hc-status");
    if (!lbl) return;
    gtk_label_set_text(GTK_LABEL(lbl), text ? text : "");
    gtk_widget_set_visible(lbl, text && *text);
    if (warn) gtk_widget_add_css_class(lbl, "hc-preview-warn");
    else      gtk_widget_remove_css_class(lbl, "hc-preview-warn");
}

/* live = aus dem Puffer rendern (ohne Speichern), wo das möglich ist. */
static void web_render(EditorTab *t, GtkWidget *panel, gboolean live)
{
    GtkWidget *view = g_object_get_data(G_OBJECT(panel), "hc-webview");
    if (!view) return;
    const char *lang = hc_lang_for_path(t->path);
    gboolean is_php = lang && !strcmp(lang, "php");

    if (!is_php) {
        /* HTML/CSS/JS: direkt den (ggf. ungespeicherten) Puffer rendern.
         * So sieht man jede Änderung sofort, ohne Strg+S. */
        GtkTextIter a, z;
        gtk_text_buffer_get_bounds(t->buffer, &a, &z);
        char *src = gtk_text_buffer_get_text(t->buffer, &a, &z, FALSE);
        char *base = g_strdup_printf("file://%s", t->path);

        if (lang && !strcmp(lang, "html")) {
            webkit_web_view_load_html(WEBKIT_WEB_VIEW(view), src, base);
        } else {
            /* CSS/JS allein ergibt keine Seite: in ein Minimalgerüst
             * einbetten, damit man die Wirkung trotzdem sieht. */
            char *html = (!strcmp(lang, "css"))
                ? g_strdup_printf("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                                  "<style>%s</style></head><body>"
                                  "<h1>Beispiel-Überschrift</h1>"
                                  "<p>Absatz zum Prüfen des Stylesheets.</p>"
                                  "<button>Schaltfläche</button></body></html>", src)
                : g_strdup_printf("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
                                  "</head><body><div id=\"app\"></div>"
                                  "<script>%s</script></body></html>", src);
            webkit_web_view_load_html(WEBKIT_WEB_VIEW(view), html, base);
            g_free(html);
        }
        set_status(panel, live ? "live aus dem Editor" : NULL, FALSE);
        g_free(src);
        g_free(base);
        return;
    }

    /* PHP: muss durch den Server laufen. */
    char *uri = web_uri_for(t);
    if (!uri) {
        HcService *sv = services_get(t->app, HC_SVC_WEB);
        char *msg = g_strdup_printf("Webserver %s – Vorschau sobald er läuft.",
            sv ? services_state_label(sv->state) : "nicht gestartet");
        webkit_web_view_load_html(WEBKIT_WEB_VIEW(view),
            "<html><body style='font:14px sans-serif;padding:24px;color:#888'>"
            "Warte auf den Webserver…</body></html>", NULL);
        set_status(panel, msg, TRUE);
        g_free(msg);
        return;
    }
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(view), uri);
    char *st = g_strdup_printf("%s", uri);
    set_status(panel, st, FALSE);
    g_free(st);
    g_free(uri);
}

/* ============================================================
 * GTK .ui-Vorschau (live, ohne Speichern und ohne Build)
 * ============================================================ */

/* Erstes darstellbares Widget aus einem GtkBuilder holen. */
static GtkWidget *extract_widget(GtkBuilder *b)
{
    GSList *objs = gtk_builder_get_objects(b);
    GtkWidget *shown = NULL;

    /* Bevorzugt ein Fenster: dessen Inhalt ist die eigentliche Oberfläche. */
    for (GSList *l = objs; l && !shown; l = l->next) {
        if (GTK_IS_WINDOW(l->data)) {
            GtkWidget *child = gtk_window_get_child(GTK_WINDOW(l->data));
            if (child) {
                g_object_ref(child);
                gtk_window_set_child(GTK_WINDOW(l->data), NULL);
                shown = child;
            }
        }
    }
    /* Sonst das erste freistehende Widget ohne Eltern. */
    for (GSList *l = objs; l && !shown; l = l->next) {
        if (GTK_IS_WIDGET(l->data) && !GTK_IS_WINDOW(l->data)
            && !gtk_widget_get_parent(GTK_WIDGET(l->data))) {
            shown = GTK_WIDGET(l->data);
            g_object_ref(shown);
        }
    }
    g_slist_free(objs);
    return shown;
}

/* GtkBuilder meldet abgeschnittenes XML nicht als Fehler (selbst ein
 * leerer String liefert TRUE). Fuer eine brauchbare Rueckmeldung beim
 * Tippen pruefen wir die Wohlgeformtheit daher selbst. */
static gboolean xml_well_formed(const char *xml, char **err)
{
    static const GMarkupParser nop = { NULL, NULL, NULL, NULL, NULL };
    GError *e = NULL;
    GMarkupParseContext *ctx =
        g_markup_parse_context_new(&nop, 0, NULL, NULL);
    gboolean ok = g_markup_parse_context_parse(ctx, xml, -1, &e)
               && g_markup_parse_context_end_parse(ctx, &e);
    g_markup_parse_context_free(ctx);
    if (!ok && err) *err = g_strdup(e ? e->message : "ungültiges XML");
    g_clear_error(&e);
    return ok;
}

static void ui_render(EditorTab *t, GtkWidget *panel)
{
    GtkWidget *holder = g_object_get_data(G_OBJECT(panel), "hc-uiholder");
    if (!holder) return;

    GtkTextIter a, z;
    gtk_text_buffer_get_bounds(t->buffer, &a, &z);
    char *xml = gtk_text_buffer_get_text(t->buffer, &a, &z, FALSE);

    /* Halbfertiges XML: letzte gueltige Ansicht behalten, Grund anzeigen.
     * Waehrend des Tippens ist das der Normalfall – die Vorschau darf
     * dabei nicht staendig auf eine Fehlermeldung umspringen. */
    char *xerr = NULL;
    if (!xml_well_formed(xml, &xerr)) {
        gboolean had_good = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(panel), "hc-had-good"));
        char *msg = g_strdup_printf("XML unvollständig: %s", xerr);
        set_status(panel, msg, TRUE);
        g_free(msg);
        g_free(xerr);
        if (!had_good) {
            GtkWidget *old = gtk_widget_get_first_child(holder);
            if (old) gtk_box_remove(GTK_BOX(holder), old);
            GtkWidget *ph = gtk_label_new("Noch keine darstellbare Oberfläche.");
            gtk_widget_add_css_class(ph, "dim-label");
            gtk_widget_set_margin_start(ph, 14);
            gtk_widget_set_margin_top(ph, 14);
            gtk_box_append(GTK_BOX(holder), ph);
        }
        g_free(xml);
        return;
    }

    GtkBuilder *b = gtk_builder_new();
    GError *e = NULL;
    GtkWidget *shown = NULL;
    if (gtk_builder_add_from_string(b, xml, -1, &e))
        shown = extract_widget(b);

    if (shown) {
        /* Erfolg: alte Ansicht ersetzen. */
        GtkWidget *old = gtk_widget_get_first_child(holder);
        if (old) gtk_box_remove(GTK_BOX(holder), old);
        gtk_widget_set_hexpand(shown, TRUE);
        gtk_widget_set_vexpand(shown, TRUE);
        gtk_box_append(GTK_BOX(holder), shown);
        g_object_unref(shown);
        set_status(panel, "live aus dem Editor", FALSE);
        g_object_set_data(G_OBJECT(panel), "hc-had-good", GINT_TO_POINTER(1));
    } else {
        /* XML (noch) ungültig. Beim Tippen ist das der Normalfall:
         * letzte gültige Ansicht stehen lassen, nur Hinweis zeigen. */
        gboolean had_good = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(panel), "hc-had-good"));
        char *msg = g_strdup_printf("XML unvollständig: %s",
                                    e ? e->message : "kein darstellbares Widget");
        set_status(panel, msg, TRUE);
        g_free(msg);
        if (!had_good) {
            GtkWidget *old = gtk_widget_get_first_child(holder);
            if (old) gtk_box_remove(GTK_BOX(holder), old);
            GtkWidget *ph = gtk_label_new("Noch keine darstellbare Oberfläche.");
            gtk_widget_add_css_class(ph, "dim-label");
            gtk_widget_set_margin_start(ph, 14);
            gtk_widget_set_margin_top(ph, 14);
            gtk_box_append(GTK_BOX(holder), ph);
        }
    }
    if (e) g_clear_error(&e);
    g_object_unref(b);
    g_free(xml);
}

/* ============================================================
 * Öffentlich: neu laden / live aktualisieren
 * ============================================================ */
void preview_reload(EditorTab *t)
{
    if (!t || !t->preview) return;
    if (path_is_ui(t->path)) ui_render(t, t->preview);
    else                     web_render(t, t->preview, FALSE);
}

void preview_live_update(EditorTab *t)
{
    if (!t || !t->preview) return;
    if (path_is_ui(t->path)) {
        ui_render(t, t->preview);          /* kein Speichern nötig */
    } else {
        web_render(t, t->preview, TRUE);   /* HTML/CSS/JS live; PHP via Server */
    }
}

static void on_reload_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    preview_reload((EditorTab *)ud);
}

static void on_open_external(GtkButton *b, gpointer ud)
{
    (void)b;
    EditorTab *t = ud;
    char *uri = web_uri_for(t);
    if (uri) {
        GtkUriLauncher *l = gtk_uri_launcher_new(uri);
        gtk_uri_launcher_launch(l, GTK_WINDOW(t->app->window), NULL, NULL, NULL);
        g_object_unref(l);
        g_free(uri);
    }
}

/* ============================================================
 * Navigation-Policy: Links im HTML-Preview abfangen
 * ============================================================ */
static gboolean on_preview_decide_policy(WebKitWebView        *view,
                                         WebKitPolicyDecision *decision,
                                         WebKitPolicyDecisionType dtype,
                                         gpointer              ud)
{
    /* Nur Navigations-Requests interessieren uns (nicht neue Fenster,
     * nicht Ressourcen-Loads wie Bilder/CSS). */
    if (dtype != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
        return FALSE;   /* WebKit-Default anwenden */

    EditorTab *t = ud;
    App *app = t->app;

    WebKitNavigationPolicyDecision *nav =
        WEBKIT_NAVIGATION_POLICY_DECISION(decision);
    WebKitNavigationAction *action =
        webkit_navigation_policy_decision_get_navigation_action(nav);
    WebKitURIRequest *req = webkit_navigation_action_get_request(action);
    const char *uri = webkit_uri_request_get_uri(req);
    if (!uri) return FALSE;

    /* about:, data:, javascript: – WebKit intern verarbeiten lassen. */
    if (g_str_has_prefix(uri, "about:") ||
        g_str_has_prefix(uri, "data:")  ||
        g_str_has_prefix(uri, "javascript:")) {
        return FALSE;
    }

    /* http(s):// – im System-Browser öffnen, Preview bleibt stehen. */
    if (g_str_has_prefix(uri, "http://") ||
        g_str_has_prefix(uri, "https://")) {
        GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
        gtk_uri_launcher_launch(launcher,
            GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(view))),
            NULL, NULL, NULL);
        g_object_unref(launcher);
        webkit_policy_decision_ignore(decision);
        return TRUE;
    }

    /* file:// – prüfen ob die Datei im Projekt liegt. */
    if (g_str_has_prefix(uri, "file://")) {
        /* URI → lokaler Pfad (Fragment-Anker abschneiden). */
        char *path = g_filename_from_uri(uri, NULL, NULL);
        if (!path) {
            /* Pfad nicht auflösbar → ignorieren, kein Absturz. */
            webkit_policy_decision_ignore(decision);
            return TRUE;
        }

        /* Fragment (#anchor) stört g_filename_from_uri nicht, aber
         * der Pfad kann ein #-Suffix enthalten wenn der Browser ihn
         * selbst zusammensetzt. Sicherheitshalber abschneiden. */
        char *hash = strchr(path, '#');
        if (hash) *hash = '\0';

        /* Liegt die Datei im Projektordner und ist sie eine bekannte
         * Web-Datei? → im Editor öffnen + Preview umschalten. */
        gboolean in_project = app->proj_root &&
                              g_str_has_prefix(path, app->proj_root);
        gboolean exists = g_file_test(path, G_FILE_TEST_IS_REGULAR);

        if (in_project && exists && path_is_web(path)) {
            /* Datei im Editor öffnen – preview_auto_open kümmert sich
             * dann um das Umschalten der Live-Vorschau. */
            EditorTab *target = editor_open_file(app, path);
            (void)target;
            webkit_policy_decision_ignore(decision);
            g_free(path);
            return TRUE;
        }

        /* Datei existiert, ist aber kein Web-Typ (z.B. ein Download-
         * Link auf eine PDF): im Dateimanager öffnen. */
        if (exists) {
            GFile *gf = g_file_new_for_path(path);
            char *file_uri = g_file_get_uri(gf);
            GtkUriLauncher *launcher = gtk_uri_launcher_new(file_uri);
            gtk_uri_launcher_launch(launcher,
                GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(view))),
                NULL, NULL, NULL);
            g_object_unref(launcher);
            g_free(file_uri);
            g_object_unref(gf);
            webkit_policy_decision_ignore(decision);
            g_free(path);
            return TRUE;
        }

        /* Datei nicht gefunden: Status-Hinweis setzen, Preview bleibt. */
        GtkWidget *panel = gtk_widget_get_parent(GTK_WIDGET(view));
        if (panel) set_status(panel, "Link-Ziel nicht gefunden.", TRUE);
        webkit_policy_decision_ignore(decision);
        g_free(path);
        return TRUE;
    }

    /* Alles andere: WebKit entscheiden lassen. */
    return FALSE;
}

/* ============================================================
 * Vorschau-Panel bauen
 * ============================================================ */
GtkWidget *preview_build_for(EditorTab *t)
{
    gboolean ui  = path_is_ui(t->path);
    gboolean web = path_is_web(t->path);
    if (!ui && !web) return NULL;

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(panel, "hc-preview");

    /* Kopfleiste: Titel, Live-Status, Aktionen. */
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(head, "hc-preview-head");
    gtk_widget_set_margin_start(head, 8); gtk_widget_set_margin_end(head, 6);
    gtk_widget_set_margin_top(head, 4); gtk_widget_set_margin_bottom(head, 4);

    GtkWidget *title = gtk_label_new(ui ? "GTK-Vorschau · live" : "Live-Browser");
    gtk_widget_add_css_class(title, "hc-preview-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_append(GTK_BOX(head), title);

    GtkWidget *status = gtk_label_new("");
    gtk_widget_add_css_class(status, "hc-preview-status");
    gtk_label_set_ellipsize(GTK_LABEL(status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(status), 0.0);
    gtk_widget_set_hexpand(status, TRUE);
    gtk_box_append(GTK_BOX(head), status);
    g_object_set_data(G_OBJECT(panel), "hc-status", status);

    if (web) {
        GtkWidget *ext = gtk_button_new_from_icon_name("web-browser-symbolic");
        gtk_widget_set_tooltip_text(ext, "Im echten Browser öffnen");
        gtk_widget_add_css_class(ext, "flat");
        g_signal_connect(ext, "clicked", G_CALLBACK(on_open_external), t);
        gtk_box_append(GTK_BOX(head), ext);
    }
    GtkWidget *reload = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(reload, "Vorschau neu laden");
    gtk_widget_add_css_class(reload, "flat");
    g_signal_connect(reload, "clicked", G_CALLBACK(on_reload_clicked), t);
    gtk_box_append(GTK_BOX(head), reload);
    gtk_box_append(GTK_BOX(panel), head);

    if (web) {
        GtkWidget *view = webkit_web_view_new();
        gtk_widget_set_vexpand(view, TRUE);
        gtk_widget_set_hexpand(view, TRUE);
        gtk_box_append(GTK_BOX(panel), view);
        g_object_set_data(G_OBJECT(panel), "hc-webview", view);

        /* Navigation-Policy: Links und Formular-Submits abfangen.
         * - about:blank und data:-URIs: immer erlauben (load_html intern).
         * - http(s)://: im System-Browser öffnen, Preview nicht verlassen.
         * - file://-Links ins Projekt: zugehörige Datei im Editor öffnen
         *   und Preview auf die neue Datei umschalten.
         * - Alles andere (z.B. javascript:): erlauben (läuft inline). */
        g_signal_connect(view, "decide-policy",
                         G_CALLBACK(on_preview_decide_policy), t);

        web_render(t, panel, FALSE);
    } else {
        GtkWidget *scr = gtk_scrolled_window_new();
        gtk_widget_set_vexpand(scr, TRUE);
        GtkWidget *holder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(holder, "hc-ui-holder");
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), holder);
        gtk_box_append(GTK_BOX(panel), scr);
        g_object_set_data(G_OBJECT(panel), "hc-uiholder", holder);
        ui_render(t, panel);
    }
    return panel;
}

/* Vorschau automatisch anzeigen, sobald eine Datei geöffnet wird, die
 * eine hat. Der Nutzer soll nichts "starten" müssen. */
void preview_auto_open(EditorTab *t)
{
    if (!t || !t->path || t->preview) return;
    if (!preview_available_for(t->path)) return;
    editor_set_preview(t, TRUE);
}

/* ============================================================
 * Nativer Emulator (Broadway)
 * ============================================================ */
gboolean preview_is_native_project(App *app)
{
    return services_project_is_native_gui(app);
}

static void emu_set_status(App *app, const char *text)
{
    if (app->emu_status) gtk_label_set_text(GTK_LABEL(app->emu_status), text ? text : "");
}

void preview_reset(App *app)
{
    if (app->emu_poll) { g_source_remove(app->emu_poll); app->emu_poll = 0; }
    if (app->emu_app) {
        g_subprocess_force_exit(app->emu_app);
        g_clear_object(&app->emu_app);
    }
    if (app->emu_page && app->notebook) {
        int p = gtk_notebook_page_num(GTK_NOTEBOOK(app->notebook), app->emu_page);
        if (p >= 0) gtk_notebook_remove_page(GTK_NOTEBOOK(app->notebook), p);
    }
    app->emu_page = NULL;
    app->emu_view = NULL;
    app->emu_status = NULL;
    app->emu_tries = 0;
}

static void emu_open_tab(App *app)
{
    if (app->emu_page) {
        int p = gtk_notebook_page_num(GTK_NOTEBOOK(app->notebook), app->emu_page);
        if (p >= 0) gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), p);
        return;
    }
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(page, "hc-preview");

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(head, "hc-preview-head");
    gtk_widget_set_margin_start(head, 8); gtk_widget_set_margin_top(head, 4);
    gtk_widget_set_margin_bottom(head, 4); gtk_widget_set_margin_end(head, 6);
    GtkWidget *tl = gtk_label_new("Emulator · Sandbox");
    gtk_widget_add_css_class(tl, "hc-preview-title");
    gtk_label_set_xalign(GTK_LABEL(tl), 0.0);
    gtk_box_append(GTK_BOX(head), tl);
    app->emu_status = gtk_label_new("");
    gtk_widget_add_css_class(app->emu_status, "hc-preview-status");
    gtk_label_set_xalign(GTK_LABEL(app->emu_status), 0.0);
    gtk_widget_set_hexpand(app->emu_status, TRUE);
    gtk_box_append(GTK_BOX(head), app->emu_status);
    gtk_box_append(GTK_BOX(page), head);

    app->emu_view = webkit_web_view_new();
    gtk_widget_set_vexpand(app->emu_view, TRUE);
    gtk_box_append(GTK_BOX(page), app->emu_view);

    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), page,
                             gtk_label_new("▶ Emulator"));
    app->emu_page = page;
    gtk_stack_set_visible_child_name(GTK_STACK(app->notebook_stack), "notebook");
    int p = gtk_notebook_page_num(GTK_NOTEBOOK(app->notebook), page);
    if (p >= 0) gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), p);
}

/* Warten, bis der Broadway-Server wirklich antwortet, erst dann laden.
 * Vorher sah man ein schwarzes Fenster, weil blind geladen wurde. */
static void emu_probe(gpointer ud);

static void on_emu_connect(GObject *src, GAsyncResult *res, gpointer ud)
{
    App *app = ud;
    GSocketConnection *c = g_socket_client_connect_finish(G_SOCKET_CLIENT(src), res, NULL);
    g_object_unref(src);

    if (c) {
        g_io_stream_close(G_IO_STREAM(c), NULL, NULL);
        g_object_unref(c);
        char *uri = g_strdup_printf("http://127.0.0.1:%d/", app->broadway_port);
        if (app->emu_view)
            webkit_web_view_load_uri(WEBKIT_WEB_VIEW(app->emu_view), uri);
        char *st = g_strdup_printf("verbunden · %s", uri);
        emu_set_status(app, st);
        g_free(st); g_free(uri);
        return;
    }
    if (++app->emu_tries > 40) {
        emu_set_status(app, "Emulator antwortet nicht – siehe Dienste-Reiter.");
        if (app->emu_view)
            webkit_web_view_load_html(WEBKIT_WEB_VIEW(app->emu_view),
                "<html><body style='font:14px sans-serif;padding:24px;color:#888'>"
                "Der GUI-Emulator ist nicht erreichbar.<br><br>"
                "Prüfe im Maschinenraum den Reiter <b>Dienste</b>: dort steht, "
                "ob <i>gtk4-broadwayd</i> gestartet wurde."
                "</body></html>", NULL);
        return;
    }
    char *st = g_strdup_printf("warte auf Emulator … (%d)", app->emu_tries);
    emu_set_status(app, st);
    g_free(st);
    app->emu_poll = g_timeout_add_once(350, (GSourceOnceFunc)emu_probe, app);
}

static void emu_probe(gpointer ud)
{
    App *app = ud;
    app->emu_poll = 0;
    GSocketClient *cl = g_socket_client_new();
    g_socket_client_set_timeout(cl, 2);
    char *addr = g_strdup_printf("127.0.0.1:%d", app->broadway_port);
    g_socket_client_connect_to_host_async(cl, addr, app->broadway_port, NULL,
                                          on_emu_connect, app);
    g_free(addr);
}

/* Ausgabe der Emulator-Anwendung ins Terminal + Crash-Hook. */
typedef struct {
    App *app;
    GDataInputStream *dis;
    GString *buf;
} EmuOut;

static void emu_read(EmuOut *eo);

static void on_emu_line(GObject *src, GAsyncResult *res, gpointer ud)
{
    EmuOut *eo = ud;
    char *line = g_data_input_stream_read_line_finish(
        G_DATA_INPUT_STREAM(src), res, NULL, NULL);
    if (!line) {
        App *app = eo->app;
        if (app->emu_app) {
            g_subprocess_wait(app->emu_app, NULL, NULL);
            int code = g_subprocess_get_if_exited(app->emu_app)
                ? g_subprocess_get_exit_status(app->emu_app) : -1;
            if (code != 0) {
                char *m = g_strdup_printf(
                    "\x1b[31m✘ Emulator-Anwendung beendet (Code %d).\x1b[0m", code);
                terminal_feed_line(app, m);
                g_free(m);
                /* Auto-Crash-Hook: Stacktrace zur Analyse an den Agenten. */
                if (eo->buf->len > 0) agent_report_crash(app, eo->buf->str);
            } else {
                terminal_feed_line(app, "\x1b[90m✔ Emulator-Anwendung beendet.\x1b[0m");
            }
            emu_set_status(app, "Anwendung beendet.");
        }
        g_object_unref(eo->dis);
        g_string_free(eo->buf, TRUE);
        g_free(eo);
        return;
    }
    terminal_feed_line(eo->app, line);
    if (eo->buf->len < 8000) {
        g_string_append(eo->buf, line);
        g_string_append_c(eo->buf, '\n');
    }
    g_free(line);
    emu_read(eo);
}

static void emu_read(EmuOut *eo)
{
    g_data_input_stream_read_line_async(eo->dis, G_PRIORITY_DEFAULT, NULL,
                                        on_emu_line, eo);
}

void preview_run_broadway(App *app)
{
    if (!app->proj_root) {
        terminal_feed_line(app, "\x1b[33mKein Projekt geöffnet.\x1b[0m");
        return;
    }
    /* Nur native GUI-Projekte gehören in den Emulator. Vorher wurde auch
     * ein PHP-Projekt hineingeschickt – daher das weiße Fenster. */
    if (!services_project_is_native_gui(app)) {
        if (services_project_is_web(app)) {
            terminal_feed_line(app,
                "\x1b[33m[Emulator] Webprojekt: die Live-Ansicht ist der "
                "Live-Browser (Strg+P), nicht der GUI-Emulator.\x1b[0m");
            EditorTab *t = editor_current(app);
            if (t) preview_auto_open(t);
        } else {
            terminal_feed_line(app,
                "\x1b[33m[Emulator] Kein GUI-Projekt – starte normal.\x1b[0m");
            terminal_run_run(app);
        }
        return;
    }

    HcService *sv = services_get(app, HC_SVC_EMULATOR);
    if (!sv || sv->state != HC_SVC_RUNNING) {
        terminal_feed_line(app,
            "\x1b[33m[Emulator] Broadway-Server läuft nicht – starte Anwendung "
            "nativ. (Siehe Maschinenraum -> Dienste)\x1b[0m");
        terminal_run_run(app);
        return;
    }

    editor_save_all(app);
    emu_open_tab(app);
    emu_set_status(app, "starte Anwendung in der Sandbox…");
    app->emu_tries = 0;

    /* Laufende Vorgängerinstanz beenden. */
    if (app->emu_app) {
        g_subprocess_force_exit(app->emu_app);
        g_clear_object(&app->emu_app);
    }

    const char *runcmd = app->proj_run ? app->proj_run : "./bin/app";
    char *cmd = g_strdup_printf(
        "GDK_BACKEND=broadway BROADWAY_DISPLAY=:%d %s",
        app->broadway_display, runcmd);
    char **argv = sandbox_exec_argv(app, cmd);

    GError *e = NULL;
    app->emu_app = g_subprocess_newv((const char * const *)argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE, &e);
    g_strfreev(argv);
    g_free(cmd);

    if (!app->emu_app) {
        char *m = g_strdup_printf("\x1b[31m[Emulator] Start fehlgeschlagen: %s\x1b[0m",
                                  e ? e->message : "?");
        terminal_feed_line(app, m);
        emu_set_status(app, "Start fehlgeschlagen.");
        g_free(m); g_clear_error(&e);
        return;
    }

    EmuOut *eo = g_new0(EmuOut, 1);
    eo->app = app;
    eo->buf = g_string_new("");
    eo->dis = g_data_input_stream_new(g_subprocess_get_stdout_pipe(app->emu_app));
    emu_read(eo);

    gtk_revealer_set_reveal_child(GTK_REVEALER(app->term_revealer), TRUE);
    emu_probe(app);
}

/* ============================================================
 * Snapshot-Tool: GUI-Snapshot als PNG + .hcs-Whitelist
 * ============================================================ */
void preview_snapshot_current(App *app)
{
    if (!app->proj_root) {
        terminal_feed_line(app, "[Snapshot] Kein Projekt geöffnet.");
        return;
    }
    /* Ziel: Emulator, sonst aktuelle Preview, sonst Editor, sonst Fenster. */
    GtkWidget *target = NULL;
    EditorTab *t = editor_current(app);
    if (app->emu_view && gtk_widget_get_mapped(app->emu_view)) target = app->emu_view;
    else if (t && t->preview) target = t->preview;
    else if (t && t->page)    target = t->page;
    else                      target = app->window;
    if (!target) return;

    int w = gtk_widget_get_width(target);
    int h = gtk_widget_get_height(target);
    if (w < 2 || h < 2) { terminal_feed_line(app, "[Snapshot] Nichts zum Aufnehmen."); return; }

    GdkPaintable *paint = gtk_widget_paintable_new(target);
    GtkSnapshot *snap = gtk_snapshot_new();
    gdk_paintable_snapshot(paint, GDK_SNAPSHOT(snap), w, h);
    GskRenderNode *node = gtk_snapshot_free_to_node(snap);
    if (!node) { g_object_unref(paint); terminal_feed_line(app, "[Snapshot] Fehlgeschlagen."); return; }

    GtkNative *native = gtk_widget_get_native(target);
    GskRenderer *renderer = native ? gtk_native_get_renderer(native) : NULL;
    GdkTexture *tex = NULL;
    if (renderer) tex = gsk_renderer_render_texture(renderer, node, NULL);
    gsk_render_node_unref(node);
    g_object_unref(paint);

    if (!tex) { terminal_feed_line(app, "[Snapshot] Kein Renderer verfügbar."); return; }

    char *dir = g_build_filename(app->proj_root, "snapshots", NULL);
    g_mkdir_with_parents(dir, 0755);
    GDateTime *now = g_date_time_new_now_local();
    char *stamp = g_date_time_format(now, "%Y%m%d_%H%M%S");
    g_date_time_unref(now);
    char *rel = g_strdup_printf("snapshots/snap_%s.png", stamp);
    char *full = g_build_filename(app->proj_root, rel, NULL);
    gboolean ok = gdk_texture_save_to_png(tex, full);
    g_object_unref(tex);

    if (ok) {
        project_whitelist_add(app, rel);  /* in den Tresor aufnehmen */
        char *m = g_strdup_printf("[Snapshot] gespeichert: %s", rel);
        terminal_feed_line(app, m);
        g_free(m);
    } else {
        terminal_feed_line(app, "[Snapshot] Speichern fehlgeschlagen.");
    }
    g_free(dir); g_free(stamp); g_free(rel); g_free(full);
}
