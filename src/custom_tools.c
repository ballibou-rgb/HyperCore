#include "app.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>

/* ============================================================
 * Custom Agent-Werkzeuge.
 *
 * Jede .json-Datei in ~/.config/hypercore/tools/ definiert ein
 * Werkzeug das dem LLM als vollwertiges Tool-Schema gemeldet wird.
 *
 * Minimal-Format:
 *   {
 *     "name":        "lint_php",
 *     "description": "PHP-Syntaxfehler im Projekt finden",
 *     "command":     "find . -name '*.php' | xargs php -l 2>&1"
 *   }
 *
 * Mit Parametern:
 *   {
 *     "name":        "db_query",
 *     "description": "SQL-Abfrage auf der Projekt-DB ausführen",
 *     "command":     "mysql -h 127.0.0.1 -P {port} -u root -e {query}",
 *     "args": [
 *       { "name": "query", "description": "SQL-Abfrage (einzeilig)" },
 *       { "name": "port",  "description": "DB-Port",  "optional": true }
 *     ]
 *   }
 *
 * Platzhalter {argname} im command werden durch die Argumentwerte
 * ersetzt bevor der Befehl über run_command in der Sandbox läuft.
 * Fehlende optionale Argumente werden durch "" ersetzt.
 * ============================================================ */

/* ---- Pfade ---- */

char *hc_tools_dir(void)
{
    char *dir = g_build_filename(g_get_user_config_dir(),
                                 "hypercore", "tools", NULL);
    g_mkdir_with_parents(dir, 0700);
    return dir;
}

/* ---- Speicher-Verwaltung ---- */

static void custom_tool_arg_free(gpointer p)
{
    HcCustomToolArg *a = p;
    g_free(a->name);
    g_free(a->description);
    g_free(a);
}

static void custom_tool_free_one(gpointer p)
{
    HcCustomTool *t = p;
    g_free(t->name);
    g_free(t->description);
    g_free(t->command);
    g_free(t->filename);
    g_list_free_full(t->args, custom_tool_arg_free);
    g_free(t);
}

void custom_tools_free(App *app)
{
    g_list_free_full(app->custom_tools, custom_tool_free_one);
    app->custom_tools = NULL;
}

/* ---- JSON laden ---- */

static HcCustomTool *load_tool_file(const char *path)
{
    JsonParser *p = json_parser_new();
    if (!json_parser_load_from_file(p, path, NULL)) {
        g_object_unref(p);
        return NULL;
    }
    JsonNode *root = json_parser_get_root(p);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(p); return NULL;
    }
    JsonObject *o = json_node_get_object(root);

    /* Pflichtfelder. */
    if (!json_object_has_member(o, "name") ||
        !json_object_has_member(o, "description") ||
        !json_object_has_member(o, "command")) {
        g_object_unref(p); return NULL;
    }

    HcCustomTool *t = g_new0(HcCustomTool, 1);
    t->name        = g_strdup(json_object_get_string_member(o, "name"));
    t->description = g_strdup(json_object_get_string_member(o, "description"));
    t->command     = g_strdup(json_object_get_string_member(o, "command"));
    t->filename    = g_strdup(path);

    /* Optionale args-Liste. */
    if (json_object_has_member(o, "args")) {
        JsonArray *arr = json_object_get_array_member(o, "args");
        guint n = json_array_get_length(arr);
        for (guint i = 0; i < n; i++) {
            JsonObject *ao = json_array_get_object_element(arr, i);
            if (!ao || !json_object_has_member(ao, "name")) continue;
            HcCustomToolArg *arg = g_new0(HcCustomToolArg, 1);
            arg->name        = g_strdup(json_object_get_string_member(ao, "name"));
            arg->description = json_object_has_member(ao, "description")
                ? g_strdup(json_object_get_string_member(ao, "description"))
                : g_strdup("");
            arg->optional    = json_object_has_member(ao, "optional")
                ? json_object_get_boolean_member(ao, "optional")
                : FALSE;
            t->args = g_list_append(t->args, arg);
        }
    }

    g_object_unref(p);
    return t;
}

void custom_tools_reload(App *app)
{
    custom_tools_free(app);
    char *dir = hc_tools_dir();
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) { g_free(dir); return; }

    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_suffix(name, ".json")) continue;
        char *full = g_build_filename(dir, name, NULL);
        HcCustomTool *t = load_tool_file(full);
        if (t) app->custom_tools = g_list_append(app->custom_tools, t);
        g_free(full);
    }
    g_dir_close(d);
    g_free(dir);
}

HcCustomTool *custom_tool_find(App *app, const char *name)
{
    for (GList *l = app->custom_tools; l; l = l->next) {
        HcCustomTool *t = l->data;
        if (!g_strcmp0(t->name, name)) return t;
    }
    return NULL;
}

/* ---- Tool-Schema für den LLM-Request ---- */

void custom_tool_append_schema(HcCustomTool *tool, gpointer builder)
{
    JsonBuilder *b = builder;
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "function");
    json_builder_set_member_name(b, "function");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, tool->name);
    /* Beschreibung mit Hinweis "custom" damit der Agent weiß woher es kommt. */
    char *desc = g_strdup_printf("[custom] %s", tool->description);
    json_builder_set_member_name(b, "description");
    json_builder_add_string_value(b, desc);
    g_free(desc);
    json_builder_set_member_name(b, "parameters");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "object");
    json_builder_set_member_name(b, "properties");
    json_builder_begin_object(b);
    /* Argumente als String-Properties. */
    for (GList *l = tool->args; l; l = l->next) {
        HcCustomToolArg *a = l->data;
        json_builder_set_member_name(b, a->name);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "type");
        json_builder_add_string_value(b, "string");
        json_builder_set_member_name(b, "description");
        json_builder_add_string_value(b, a->description);
        json_builder_end_object(b);
    }
    json_builder_end_object(b); /* properties */
    /* required-Array: nur nicht-optionale Args. */
    json_builder_set_member_name(b, "required");
    json_builder_begin_array(b);
    for (GList *l = tool->args; l; l = l->next) {
        HcCustomToolArg *a = l->data;
        if (!a->optional) json_builder_add_string_value(b, a->name);
    }
    json_builder_end_array(b);
    json_builder_end_object(b); /* parameters */
    json_builder_end_object(b); /* function   */
    json_builder_end_object(b); /* tool       */
}

/* ---- Tool ausführen ---- */

char *custom_tool_run(App *app, HcCustomTool *tool,
                      const char *args_json, gboolean *ok)
{
    /* Argumente aus JSON parsen. */
    JsonParser *p = json_parser_new();
    JsonObject *jargs = NULL;
    if (args_json && *args_json &&
        json_parser_load_from_data(p, args_json, -1, NULL)) {
        JsonNode *r = json_parser_get_root(p);
        if (r && JSON_NODE_HOLDS_OBJECT(r)) jargs = json_node_get_object(r);
    }

    /* Platzhalter {name} im Command ersetzen. */
    char *cmd = g_strdup(tool->command);
    for (GList *l = tool->args; l; l = l->next) {
        HcCustomToolArg *a = l->data;
        const char *val = "";
        if (jargs && json_object_has_member(jargs, a->name)) {
            JsonNode *n = json_object_get_member(jargs, a->name);
            if (JSON_NODE_HOLDS_VALUE(n))
                val = json_node_get_string(n);
        }
        /* Sicher ersetzen: Wert shell-quoten um Injection zu verhindern. */
        char *quoted = g_shell_quote(val);
        char *placeholder = g_strdup_printf("{%s}", a->name);
        char *newcmd = NULL;
        /* Alle Vorkommen ersetzen. */
        char **parts = g_strsplit(cmd, placeholder, -1);
        newcmd = g_strjoinv(quoted, parts);
        g_strfreev(parts);
        g_free(placeholder);
        g_free(quoted);
        g_free(cmd);
        cmd = newcmd;
    }
    g_object_unref(p);

    /* Über sandbox_exec_argv ausführen (gleiche Umgebung wie run_command). */
    if (!app->proj_root) {
        g_free(cmd);
        *ok = FALSE;
        return g_strdup("Kein Projekt geöffnet.");
    }

    char **argv = sandbox_exec_argv(app, cmd);
    g_free(cmd);

    char *out = NULL, *err_out = NULL;
    int status = 0; GError *e = NULL;
    gboolean ran = g_spawn_sync(
        app->proj_root, argv, NULL,
        G_SPAWN_SEARCH_PATH,
        NULL, NULL, &out, &err_out, &status, &e);
    g_strfreev(argv);

    if (!ran) {
        char *m = g_strdup_printf("Werkzeug '%s' konnte nicht gestartet werden: %s",
                                   tool->name, e ? e->message : "?");
        g_clear_error(&e); *ok = FALSE; return m;
    }

    GString *s = g_string_new("");
    if (out    && *out)      g_string_append(s, out);
    if (err_out && *err_out) g_string_append(s, err_out);
    g_free(out); g_free(err_out);
    if (s->len == 0) g_string_append(s, "(keine Ausgabe)");

    int code = g_spawn_check_wait_status(status, NULL) ? 0 : status;
    g_string_append_printf(s, "\n[exit %d]", code);

    const gsize CAP = 8000;
    if (s->len > CAP) {
        g_string_truncate(s, CAP);
        g_string_append(s, "\n...(gekürzt)");
    }
    *ok = (code == 0);
    return g_string_free(s, FALSE);
}

/* ============================================================
 * Einstellungs-Seite "Werkzeuge"
 * ============================================================ */

/* Hilfsfunktion: JSON-Vorlage für ein neues Tool erzeugen. */
static char *make_default_json(const char *name)
{
    return g_strdup_printf(
        "{\n"
        "  \"name\": \"%s\",\n"
        "  \"description\": \"Beschreibung was das Werkzeug tut\",\n"
        "  \"command\": \"echo 'Hallo aus {input}'\",\n"
        "  \"args\": [\n"
        "    {\n"
        "      \"name\": \"input\",\n"
        "      \"description\": \"Eingabe\",\n"
        "      \"optional\": false\n"
        "    }\n"
        "  ]\n"
        "}\n", name);
}

typedef struct { App *app; GtkWidget *list_box; } ToolsPageCtx;

static void refresh_tool_list(GtkWidget *list_box, App *app);

static void on_open_tool_file(GtkButton *btn, gpointer ud)
{
    const char *path = g_object_get_data(G_OBJECT(btn), "hc-tool-path");
    if (!path) return;
    App *app = ud;
    editor_open_file(app, path);
}

static void on_delete_tool(GtkButton *btn, gpointer ud)
{
    ToolsPageCtx *ctx = ud;
    const char *path = g_object_get_data(G_OBJECT(btn), "hc-tool-path");
    if (!path) return;

    GtkAlertDialog *dlg = gtk_alert_dialog_new(
        "Werkzeug-Datei loeschen?");
    /* Inline-Callback über closure: path und ctx sichern. */
    char *path_copy = g_strdup(path);
    /* GtkAlertDialog choose mit Lambda wäre ideal, aber in C brauchen
     * wir eine Closure-Struktur. Wir nutzen g_object_set_data am dlg. */
    g_object_set_data_full(G_OBJECT(dlg), "hc-path", path_copy, g_free);
    g_object_set_data(G_OBJECT(dlg), "hc-ctx", ctx);
    const char *btns[] = { "Abbrechen", "Loeschen", NULL };
    gtk_alert_dialog_set_buttons(dlg, btns);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 1);

    /* Callback als statische Funktion — forward declare nötig. */
    extern void _on_delete_tool_confirmed(GObject*, GAsyncResult*, gpointer);
    gtk_alert_dialog_choose(dlg,
        GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn))),
        NULL, _on_delete_tool_confirmed, ctx);
}

void _on_delete_tool_confirmed(GObject *src, GAsyncResult *res, gpointer ud)
{
    ToolsPageCtx *ctx = ud;
    GtkAlertDialog *dlg = GTK_ALERT_DIALOG(src);
    int btn = gtk_alert_dialog_choose_finish(dlg, res, NULL);
    if (btn == 1) {
        const char *path = g_object_get_data(G_OBJECT(dlg), "hc-path");
        g_remove(path);
        custom_tools_reload(ctx->app);
        refresh_tool_list(ctx->list_box, ctx->app);
    }
    g_object_unref(dlg);
}

static void refresh_tool_list(GtkWidget *list_box, App *app)
{
    /* Alle Kinder entfernen. */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(list_box)))
        gtk_box_remove(GTK_BOX(list_box), child);

    if (!app->custom_tools) {
        GtkWidget *lbl = gtk_label_new("Noch keine Custom-Werkzeuge.\n"
            "Klicke '+' um ein neues anzulegen.");
        gtk_widget_set_margin_top(lbl, 16);
        gtk_widget_add_css_class(lbl, "hc-settings-hint");
        gtk_box_append(GTK_BOX(list_box), lbl);
        return;
    }

    for (GList *l = app->custom_tools; l; l = l->next) {
        HcCustomTool *t = l->data;

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(row, 4);
        gtk_widget_set_margin_end(row, 4);
        gtk_widget_set_margin_top(row, 4);
        gtk_widget_add_css_class(row, "hc-tool-row");

        /* Icon + Name + Beschreibung. */
        GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(info, TRUE);
        GtkWidget *name_lbl = gtk_label_new(t->name);
        gtk_widget_add_css_class(name_lbl, "hc-tool-name");
        gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);
        GtkWidget *desc_lbl = gtk_label_new(t->description);
        gtk_widget_add_css_class(desc_lbl, "hc-settings-hint");
        gtk_widget_set_halign(desc_lbl, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(desc_lbl), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(info), name_lbl);
        gtk_box_append(GTK_BOX(info), desc_lbl);

        /* Arg-Anzahl als kleines Badge. */
        int nargs = (int)g_list_length(t->args);
        if (nargs > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d Arg%s", nargs, nargs == 1 ? "" : "s");
            GtkWidget *badge = gtk_label_new(buf);
            gtk_widget_add_css_class(badge, "hc-tool-badge");
            gtk_box_append(GTK_BOX(row), badge);
        }

        /* Bearbeiten-Button (öffnet die JSON im Editor). */
        GtkWidget *edit_btn = gtk_button_new_from_icon_name("document-edit-symbolic");
        gtk_widget_set_tooltip_text(edit_btn, "JSON im Editor öffnen");
        gtk_widget_add_css_class(edit_btn, "flat");
        g_object_set_data_full(G_OBJECT(edit_btn), "hc-tool-path",
                               g_strdup(t->filename), g_free);
        g_signal_connect(edit_btn, "clicked", G_CALLBACK(on_open_tool_file), app);

        /* Löschen-Button. */
        ToolsPageCtx *ctx = g_object_get_data(G_OBJECT(list_box), "hc-ctx");
        GtkWidget *del_btn = gtk_button_new_from_icon_name("edit-delete-symbolic");
        gtk_widget_set_tooltip_text(del_btn, "Werkzeug entfernen");
        gtk_widget_add_css_class(del_btn, "flat");
        gtk_widget_add_css_class(del_btn, "destructive-action");
        g_object_set_data_full(G_OBJECT(del_btn), "hc-tool-path",
                               g_strdup(t->filename), g_free);
        g_signal_connect(del_btn, "clicked", G_CALLBACK(on_delete_tool), ctx);

        gtk_box_append(GTK_BOX(row), info);
        gtk_box_append(GTK_BOX(row), edit_btn);
        gtk_box_append(GTK_BOX(row), del_btn);
        gtk_box_append(GTK_BOX(list_box), row);
    }
}

static void on_add_tool(GtkButton *btn, gpointer ud)
{
    ToolsPageCtx *ctx = ud;
    App *app = ctx->app;

    /* Einzigartigen Namen generieren. */
    char *dir = hc_tools_dir();
    int n = (int)g_list_length(app->custom_tools) + 1;
    char *filename = NULL;
    char *path = NULL;
    do {
        g_free(filename); g_free(path);
        filename = g_strdup_printf("tool_%02d.json", n++);
        path = g_build_filename(dir, filename, NULL);
    } while (g_file_test(path, G_FILE_TEST_EXISTS));
    g_free(dir);

    /* Vorlage schreiben. */
    char *slug = g_strndup(filename, strlen(filename) - 5); /* ohne .json */
    char *content = make_default_json(slug);
    g_free(slug);
    g_file_set_contents(path, content, -1, NULL);
    g_free(content);

    /* Neu laden + im Editor öffnen. */
    custom_tools_reload(app);
    refresh_tool_list(ctx->list_box, app);
    editor_open_file(app, path);
    g_free(path);
    g_free(filename);

    (void)btn;
}

static void on_reload_tools(GtkButton *btn, gpointer ud)
{
    ToolsPageCtx *ctx = ud;
    (void)btn;
    custom_tools_reload(ctx->app);
    refresh_tool_list(ctx->list_box, ctx->app);
}

static void on_open_tools_dir(GtkButton *btn, gpointer ud)
{
    (void)ud;
    char *dir = hc_tools_dir();
    char *uri = g_strdup_printf("file://%s", dir);
    GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
    gtk_uri_launcher_launch(launcher,
        GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn))),
        NULL, NULL, NULL);
    g_object_unref(launcher);
    g_free(uri);
    g_free(dir);
}

/* Eingebaute Werkzeuge (nicht editierbar, nur zur Anzeige). */
static const struct { const char *name; const char *desc; } BUILTIN_TOOLS[] = {
    { "list_directory",  "Ordnerinhalt eines Projektverzeichnisses auflisten" },
    { "read_file",       "Dateiinhalt lesen (optional: start_line/end_line)" },
    { "write_file",      "Neue Datei anlegen und Inhalt schreiben" },
    { "replace_in_file", "Textblock in bestehender Datei exakt ersetzen" },
    { "search_project",  "Projekt per grep nach Begriffen durchsuchen" },
    { "project_status",  "Dienste, Ports, Datenbank und offene Fehler abfragen" },
    { "run_command",     "Beliebigen Shell-Befehl in der Sandbox ausfuehren" },
    { "get_url",         "HTTP-GET auf den laufenden Webserver senden" },
    { "run_build",       "Projekt bauen und Compiler-Ausgabe zurueckgeben" },
    { NULL, NULL }
};

GtkWidget *custom_tools_settings_page(App *app)
{
    /* outer_page wird ans Notebook gehängt und ZURÜCKGEGEBEN.
     * Alles andere wird daran gehängt — kein page=inner Trick mehr. */
    GtkWidget *outer_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *page = outer_page;

    /* ---- Abschnitt: Eingebaute Werkzeuge ---- */
    GtkWidget *builtin_head = gtk_label_new("EINGEBAUTE WERKZEUGE");
    gtk_widget_add_css_class(builtin_head, "hc-about-section");
    gtk_label_set_xalign(GTK_LABEL(builtin_head), 0.0);
    gtk_widget_set_margin_top(builtin_head, 14);
    gtk_widget_set_margin_bottom(builtin_head, 8);
    gtk_widget_set_margin_start(builtin_head, 16);
    gtk_box_append(GTK_BOX(page), builtin_head);

    GtkWidget *builtin_hint = gtk_label_new(
        "Diese Werkzeuge sind fest eingebaut und stehen dem Agenten immer zur Verfuegung.");
    gtk_widget_add_css_class(builtin_hint, "hc-settings-hint");
    gtk_label_set_xalign(GTK_LABEL(builtin_hint), 0.0);
    gtk_label_set_wrap(GTK_LABEL(builtin_hint), TRUE);
    gtk_widget_set_margin_start(builtin_hint, 16);
    gtk_widget_set_margin_end(builtin_hint, 16);
    gtk_widget_set_margin_bottom(builtin_hint, 8);
    gtk_box_append(GTK_BOX(page), builtin_hint);

    GtkWidget *builtin_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(builtin_list, 16);
    gtk_widget_set_margin_end(builtin_list, 16);

    for (int i = 0; BUILTIN_TOOLS[i].name; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(row, "hc-tool-row");
        gtk_widget_add_css_class(row, "hc-builtin");

        GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(info, TRUE);

        GtkWidget *name_lbl = gtk_label_new(BUILTIN_TOOLS[i].name);
        gtk_widget_add_css_class(name_lbl, "hc-tool-name");
        gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0);
        gtk_widget_set_margin_start(name_lbl, 4);

        GtkWidget *desc_lbl = gtk_label_new(BUILTIN_TOOLS[i].desc);
        gtk_widget_add_css_class(desc_lbl, "hc-tool-desc");
        gtk_label_set_xalign(GTK_LABEL(desc_lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(desc_lbl), PANGO_ELLIPSIZE_END);
        gtk_widget_set_margin_start(desc_lbl, 4);

        GtkWidget *badge = gtk_label_new("eingebaut");
        gtk_widget_add_css_class(badge, "hc-tool-badge-builtin");

        gtk_box_append(GTK_BOX(info), name_lbl);
        gtk_box_append(GTK_BOX(info), desc_lbl);
        gtk_box_append(GTK_BOX(row), info);
        gtk_box_append(GTK_BOX(row), badge);
        gtk_box_append(GTK_BOX(builtin_list), row);
    }
    gtk_box_append(GTK_BOX(page), builtin_list);

    /* ---- Abschnitt: Custom-Werkzeuge ---- */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep, 14);
    gtk_widget_set_margin_bottom(sep, 10);
    gtk_widget_set_margin_start(sep, 16);
    gtk_widget_set_margin_end(sep, 16);
    gtk_box_append(GTK_BOX(page), sep);

    GtkWidget *custom_head = gtk_label_new("EIGENE WERKZEUGE");
    gtk_widget_add_css_class(custom_head, "hc-about-section");
    gtk_label_set_xalign(GTK_LABEL(custom_head), 0.0);
    gtk_widget_set_margin_start(custom_head, 16);
    gtk_widget_set_margin_bottom(custom_head, 8);
    gtk_box_append(GTK_BOX(page), custom_head);

    GtkWidget *hint = gtk_label_new(
        "Eigene Werkzeuge: .json-Datei in ~/.config/hypercore/tools/\n"
        "Platzhalter {argname} im command werden durch Argument-Werte ersetzt.");
    gtk_widget_add_css_class(hint, "hc-settings-hint");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_margin_start(hint, 16);
    gtk_widget_set_margin_end(hint, 16);
    gtk_widget_set_margin_bottom(hint, 8);
    gtk_box_append(GTK_BOX(page), hint);

    /* Toolbar und Liste folgen unten unveraendert */
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(inner, 16);
    gtk_widget_set_margin_end(inner, 16);
    gtk_widget_set_margin_bottom(inner, 16);
    gtk_widget_set_vexpand(inner, TRUE);
    gtk_box_append(GTK_BOX(outer_page), inner);

    /* Toolbar: +, Neu laden, Ordner öffnen. */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    GtkWidget *add_btn = gtk_button_new_with_label("+ Neu");
    gtk_widget_add_css_class(add_btn, "suggested-action");

    GtkWidget *reload_btn = gtk_button_new_with_label("Neu laden");

    GtkWidget *folder_btn = gtk_button_new_with_label("Ordner öffnen");

    gtk_box_append(GTK_BOX(toolbar), add_btn);
    gtk_box_append(GTK_BOX(toolbar), reload_btn);
    gtk_box_append(GTK_BOX(toolbar), folder_btn);
    gtk_box_append(GTK_BOX(inner), toolbar);

    /* Scrollable Liste. */
    GtkWidget *scr = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scr, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkWidget *list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), list_box);
    gtk_box_append(GTK_BOX(inner), scr);

    /* Kontext für Callbacks. */
    ToolsPageCtx *ctx = g_new0(ToolsPageCtx, 1);
    ctx->app = app;
    ctx->list_box = list_box;
    g_object_set_data_full(G_OBJECT(list_box), "hc-ctx", ctx, g_free);

    g_signal_connect(add_btn,    "clicked", G_CALLBACK(on_add_tool),       ctx);
    g_signal_connect(reload_btn, "clicked", G_CALLBACK(on_reload_tools),   ctx);
    g_signal_connect(folder_btn, "clicked", G_CALLBACK(on_open_tools_dir), app);

    /* Initiale Liste befüllen. */
    refresh_tool_list(list_box, app);

    return outer_page;
}
