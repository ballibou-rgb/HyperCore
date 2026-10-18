#include "app.h"
#include "version.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <gdk/gdkkeysyms.h>
#include <string.h>

/* ============================================================
 * Nativ integrierter KI-Agent.
 *  - Chat-Verlauf + Eingabe in der rechten Sidebar
 *  - Kontext: aktuelle Datei + Projekt-Dateiliste
 *  - Autonomer Modus: schreibt ```file=...```-Blöcke direkt ins Projekt
 *  - Ghost-Text: graue Inline-Vorschläge, Übernahme per Tab
 *
 * Protokoll: OpenAI-kompatibel (POST /v1/chat/completions), wie es
 * llama.cpp, Ollama, LM Studio und vLLM anbieten.
 * ============================================================ */

/* Session wird bei jedem Request neu konfiguriert (Timeout kann sich
 * in den Einstellungen ändern). */
static SoupSession *agent_session(App *app)
{
    static SoupSession *s = NULL;
    if (!s) s = soup_session_new();
    int t = (app && app->settings.agent_timeout_sec >= 10)
            ? app->settings.agent_timeout_sec : 120;
    g_object_set(s, "timeout", (guint)t, NULL);
    return s;
}

static char *agent_url(App *app)
{
    return g_strdup_printf("http://%s:%d/v1/chat/completions",
        app->settings.agent_host ? app->settings.agent_host : "127.0.0.1",
        app->settings.agent_port);
}

static gboolean agent_configured(App *app)
{
    return app->settings.agent_host && *app->settings.agent_host
        && app->settings.agent_port > 0;
}

/* ---- JSON-Body aus einer Liste von HcChatTurn* ---- */
static char *build_body(const char *model, GList *turns, int max_tokens, double temp)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "model");
    json_builder_add_string_value(b, model ? model : "local-model");
    json_builder_set_member_name(b, "messages");
    json_builder_begin_array(b);
    for (GList *l = turns; l; l = l->next) {
        HcChatTurn *t = l->data;
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "role");
        json_builder_add_string_value(b, t->role);
        json_builder_set_member_name(b, "content");
        json_builder_add_string_value(b, t->content);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_set_member_name(b, "max_tokens");
    json_builder_add_int_value(b, max_tokens);
    json_builder_set_member_name(b, "temperature");
    json_builder_add_double_value(b, temp);
    json_builder_set_member_name(b, "stream");
    json_builder_add_boolean_value(b, FALSE);
    json_builder_end_object(b);

    JsonGenerator *g = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(g, root);
    char *out = json_generator_to_data(g, NULL);
    json_node_free(root);
    g_object_unref(g);
    g_object_unref(b);
    return out;
}

/* ---- Antwort-Content extrahieren ---- */
static char *parse_content(const char *data, gsize len, char **err_out)
{
    JsonParser *p = json_parser_new();
    char *result = NULL;
    if (json_parser_load_from_data(p, data, len, NULL)) {
        JsonNode *root = json_parser_get_root(p);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *o = json_node_get_object(root);
            if (json_object_has_member(o, "choices")) {
                JsonArray *ch = json_object_get_array_member(o, "choices");
                if (json_array_get_length(ch) > 0) {
                    JsonObject *c0 = json_array_get_object_element(ch, 0);
                    if (json_object_has_member(c0, "message")) {
                        JsonObject *msg = json_object_get_object_member(c0, "message");
                        if (json_object_has_member(msg, "content"))
                            result = g_strdup(json_object_get_string_member(msg, "content"));
                    } else if (json_object_has_member(c0, "text")) {
                        result = g_strdup(json_object_get_string_member(c0, "text"));
                    }
                }
            } else if (json_object_has_member(o, "error")) {
                JsonNode *en = json_object_get_member(o, "error");
                if (JSON_NODE_HOLDS_OBJECT(en)) {
                    JsonObject *eo = json_node_get_object(en);
                    if (json_object_has_member(eo, "message") && err_out)
                        *err_out = g_strdup(json_object_get_string_member(eo, "message"));
                } else if (err_out) {
                    *err_out = g_strdup("Serverfehler");
                }
            }
        }
    } else if (err_out) {
        *err_out = g_strdup("Ungültige JSON-Antwort vom Server.");
    }
    g_object_unref(p);
    return result;
}

static void turn_free(gpointer p) { HcChatTurn *t = p; g_free(t->role); g_free(t->content); g_free(t); }
static HcChatTurn *turn_new(const char *role, const char *content) {
    HcChatTurn *t = g_new0(HcChatTurn, 1);
    t->role = g_strdup(role); t->content = g_strdup(content);
    return t;
}

/* ============================================================
 * Chat-UI
 * ============================================================ */

/* Animierte Thinking-Blase: drei Punkte mit versetzter CSS-Animation.
 * Gibt die äußere Row zurück (wird von remove_thinking entfernt). */
static GtkWidget *add_thinking_bubble(App *app)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *bubble = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(bubble, "hc-bubble");
    gtk_widget_add_css_class(bubble, "hc-bubble-assistant");
    gtk_widget_add_css_class(bubble, "hc-bubble-thinking");
    gtk_widget_set_halign(bubble, GTK_ALIGN_START);

    /* Drei Punkte – jeder bekommt eine CSS-Klasse mit Delay. */
    const char *dot_classes[] = { "hc-dot-1", "hc-dot-2", "hc-dot-3" };
    for (int i = 0; i < 3; i++) {
        GtkWidget *dot = gtk_label_new("\xe2\x80\xa2");  /* Unicode-Bullet U+2022 */
        gtk_widget_add_css_class(dot, "hc-thinking-dot");
        gtk_widget_add_css_class(dot, dot_classes[i]);
        gtk_box_append(GTK_BOX(bubble), dot);
    }

    gtk_box_append(GTK_BOX(row), bubble);
    gtk_box_append(GTK_BOX(app->agent_log), row);
    return row;   /* row zurückgeben, nicht bubble – remove_thinking entfernt die row */
}
static void scroll_log_bottom(App *app) __attribute__((unused));
static void scroll_log_bottom(App *app)
{
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(app->agent_log_scroll));
    if (adj) gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
}

/* Auto-Scroll: GtkAdjustment meldet "changed" sobald upper sich geändert
 * hat – das ist der Moment NACHDEM GTK das neue Layout berechnet hat.
 * size-allocate feuert zu früh; upper hat dort noch den alten Wert.
 *
 * Wir merken uns ob der User "am Ende" war bevor die neue Bubble kam.
 * Wenn ja → scrollen. Wenn er zurückgescrollt hat → nicht anfassen. */
typedef struct { App *app; gboolean was_at_bottom; } ScrollCtx;

static gboolean do_scroll_bottom(gpointer ud)
{
    ScrollCtx *ctx = ud;
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ctx->app->agent_log_scroll));
    if (adj) {
        double upper = gtk_adjustment_get_upper(adj);
        double page  = gtk_adjustment_get_page_size(adj);
        gtk_adjustment_set_value(adj, upper - page);
    }
    g_free(ctx);
    return G_SOURCE_REMOVE;
}

static void on_adj_changed(GtkAdjustment *adj, gpointer ud)
{
    App *app = ud;
    double upper   = gtk_adjustment_get_upper(adj);
    double page    = gtk_adjustment_get_page_size(adj);
    double current = gtk_adjustment_get_value(adj);
    /* War der User am Ende (Toleranz: eine halbe Seite)? */
    gboolean at_bottom = (current >= upper - page - page * 0.5);
    if (at_bottom) {
        /* Idle-Callback: GtkAdjustment-Callbacks dürfen value nicht direkt
         * setzen – das würde das changed-Signal re-entrieren. */
        ScrollCtx *ctx = g_new0(ScrollCtx, 1);
        ctx->app = app;
        ctx->was_at_bottom = TRUE;
        g_idle_add(do_scroll_bottom, ctx);
    }
}

static GtkWidget *add_bubble(App *app, const char *role, const char *text)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *bubble = gtk_label_new(text);
    gtk_label_set_wrap(GTK_LABEL(bubble), TRUE);
    gtk_label_set_xalign(GTK_LABEL(bubble), 0.0);
    gtk_label_set_selectable(GTK_LABEL(bubble), TRUE);
    gtk_widget_set_hexpand(bubble, TRUE);
    gtk_widget_add_css_class(bubble, "hc-bubble");
    if (!strcmp(role, "user")) {
        gtk_widget_add_css_class(bubble, "hc-bubble-user");
        gtk_widget_set_halign(bubble, GTK_ALIGN_END);
    } else if (!strcmp(role, "system")) {
        gtk_widget_add_css_class(bubble, "hc-bubble-system");
    } else {
        gtk_widget_add_css_class(bubble, "hc-bubble-assistant");
        gtk_widget_set_halign(bubble, GTK_ALIGN_START);
    }
    gtk_box_append(GTK_BOX(row), bubble);
    gtk_box_append(GTK_BOX(app->agent_log), row);
    return bubble;
}

/* ---- System-Prompt mit Projekt-Kontext ---- */
const char *persona_name(int persona)
{
    switch (persona) {
        case 0: return "Architekt";
        case 1: return "Planer";
        default: return "Coder";
    }
}

static const char *persona_directive(int persona)
{
    switch (persona) {
        case 0: return "ROLLE: Architekt. Fokus auf Konzepte, Architektur und "
                       "Entwurfsentscheidungen. Erkläre Strukturen und Abwägungen, "
                       "bevor Code entsteht.";
        case 1: return "ROLLE: Planer. Fokus auf Strukturen, Dateiaufteilung und "
                       "Arbeitsschritte. Liefere klare, nummerierte Pläne.";
        default: return "ROLLE: Coder. Liefere reinen, lauffähigen Code mit minimalen "
                        "Erklärungen. Nutze die Werkzeuge direkt.";
    }
}

/* Forward declaration noetig da die public wrapper-Funktion vor der
 * static-Implementierung steht. */
static char *build_system_prompt(App *app);

/* Oeffentliche Debug-Funktion: gibt den aktuell generierten System-Prompt zurueck. */
char *agent_debug_get_system_prompt(App *app) { return build_system_prompt(app); }

static char *build_system_prompt(App *app)
{
    GString *s = g_string_new(
        "Du bist der in die HyperCore Studio-IDE integrierte Programmier-Assistent. "
        "Antworte knapp und auf Deutsch.\n");
    g_string_append_printf(s, "%s\n\n", persona_directive(app->settings.persona));
    g_string_append(s,
        "## WERKZEUGE\n"
        "Du hast diese Werkzeuge. Benutze sie – schreibe keinen Code in den Chat:\n"
        "  list_directory   – Ordnerinhalt lesen\n"
        "  read_file        – Datei lesen (start_line/end_line optional)\n"
        "  search_project   – Projekt durchsuchen (grep)\n"
        "  project_status   – Dienste, Ports, Fehler\n"
        "  write_file       – NEUE Datei anlegen\n"
        "  replace_in_file  – Block in Datei exakt ersetzen\n"
        "  run_command      – Shell-Befehl (grep, sed, php -l, npm, …)\n"
        "  get_url          – HTTP-GET auf Webserver\n"
        "  run_build        – Projekt bauen\n"
        "\n"
        "## ABSOLUT VERBOTEN\n"
        "- NIEMALS Code, JSON, Dateiinhalt oder Codeblöcke (```) in den Chat schreiben.\n"
        "- NIEMALS write_file oder replace_in_file als Text erklären oder zeigen.\n"
        "- NIEMALS sagen 'Hier ist der Code' oder 'Füge folgendes ein'.\n"
        "- NIEMALS eine Aufgabe beschreiben anstatt sie auszuführen.\n"
        "- Wenn du Code ausgibst anstatt ein Werkzeug zu benutzen, ist das ein FEHLER.\n"
        "\n"
        "## WIE DU ARBEITEST\n"
        "Aufgabe erhalten → sofort Werkzeuge benutzen → fertig → kurze Zusammenfassung.\n"
        "\n"
        "Neue Datei anlegen:\n"
        "  → write_file aufrufen. FERTIG. Keinen Inhalt im Chat zeigen.\n"
        "\n"
        "Datei ändern:\n"
        "  1. read_file aufrufen (PFLICHT vor replace_in_file)\n"
        "  2. replace_in_file mit EXAKT kopiertem Suchblock\n"
        "  3. Kurze Bestätigung im Chat\n"
        "\n"
        "Fehler suchen:\n"
        "  1. project_status → 2. get_url → 3. read_file/run_command\n"
        "  4. replace_in_file → 5. get_url zur Verifikation\n"
        "\n"
        "Nach Code-Änderungen: run_build aufrufen.\n"
        "\n"
        "## BEISPIEL (RICHTIG)\n"
        "Benutzer: 'Erstelle eine README.md'\n"
        "Du: [rufst write_file auf mit filepath='README.md' und content='...']\n"
        "Du: 'README.md wurde angelegt.'\n"
        "\n"
        "## BEISPIEL (FALSCH – TU DAS NICHT)\n"
        "Benutzer: 'Erstelle eine README.md'\n"
        "Du: 'Hier ist der Inhalt für README.md: ```markdown # Titel ...```'\n"
        "→ Das ist VERBOTEN. Benutze write_file.\n\n");

    /* Strict Compliance Mode: versteckter Zwang zu DocBlocks + Header. */
    if (app->settings.compliance_enabled) {
        char year[8]; GDateTime *now = g_date_time_new_now_local();
        g_snprintf(year, sizeof(year), "%d", g_date_time_get_year(now));
        g_date_time_unref(now);
        GString *hdr = g_string_new(app->settings.copyright_header ? app->settings.copyright_header : "");
        g_string_replace(hdr, "{year}", year, 0);
        g_string_replace(hdr, "{company}", app->settings.dev_company ? app->settings.dev_company : "", 0);
        g_string_replace(hdr, "{name}", app->settings.dev_name ? app->settings.dev_name : "", 0);
        g_string_replace(hdr, "{email}", app->settings.dev_email ? app->settings.dev_email : "", 0);
        g_string_replace(hdr, "{license}", app->settings.dev_license ? app->settings.dev_license : "", 0);
        g_string_append_printf(s,
            "STRICT COMPLIANCE MODE (zwingend): Jede von dir erzeugte oder geänderte "
            "Datei MUSS einen Datei-DocBlock am Anfang und folgenden Copyright-Header "
            "enthalten:\n%s\nDokumentiere jede Funktion mit einem DocBlock.\n\n", hdr->str);
        g_string_free(hdr, TRUE);
    }
    /* Custom-Werkzeuge auflisten wenn vorhanden. */
    if (app->custom_tools) {
        g_string_append(s, "Zusätzliche eigene Werkzeuge (custom):\n");
        for (GList *l = app->custom_tools; l; l = l->next) {
            HcCustomTool *t = l->data;
            g_string_append_printf(s, "  %-24s – %s\n", t->name, t->description);
        }
        g_string_append_c(s, '\n');
    }

    /* Global Identity – immer senden, nicht nur im Compliance-Modus.
     * Der Agent soll wissen, für wen er arbeitet. */
    gboolean has_identity = (app->settings.dev_name && *app->settings.dev_name)
                         || (app->settings.dev_company && *app->settings.dev_company);
    if (has_identity) {
        g_string_append(s, "Entwickler-Identität:\n");
        if (app->settings.dev_name    && *app->settings.dev_name)
            g_string_append_printf(s, "  Name:    %s\n", app->settings.dev_name);
        if (app->settings.dev_company && *app->settings.dev_company)
            g_string_append_printf(s, "  Firma:   %s\n", app->settings.dev_company);
        if (app->settings.dev_email   && *app->settings.dev_email)
            g_string_append_printf(s, "  E-Mail:  %s\n", app->settings.dev_email);
        if (app->settings.dev_website && *app->settings.dev_website)
            g_string_append_printf(s, "  Website: %s\n", app->settings.dev_website);
        if (app->settings.dev_license && *app->settings.dev_license)
            g_string_append_printf(s, "  Lizenz:  %s\n", app->settings.dev_license);
        g_string_append_c(s, '\n');
    }

    if (app->proj_name)
        g_string_append_printf(s, "Projekt: %s (Sprache: %s)\n",
            app->proj_name, app->proj_lang ? app->proj_lang : "?");

    /* Dateiliste aus der Whitelist – so kennt der Agent exakt die
     * Dateien, die zum Projekt gehören (Tresor-Prinzip). */
    if (app->proj_root) {
        GList *wl = project_whitelist(app);
        if (wl) {
            g_string_append(s, "Projektdateien (Whitelist):\n");
            int c = 0;
            for (GList *l = wl; l && c < 80; l = l->next, c++)
                g_string_append_printf(s, "  - %s\n", (char *)l->data);
            if (c >= 80) g_string_append(s, "  … (weitere Dateien vorhanden)\n");
            g_list_free_full(wl, g_free);
        }
    }

    /* Aktuelle Datei (bis 6000 Zeichen). */
    EditorTab *t = editor_current(app);
    if (t && t->path) {
        GtkTextIter a, z; gtk_text_buffer_get_bounds(t->buffer, &a, &z);
        char *code = gtk_text_buffer_get_text(t->buffer, &a, &z, FALSE);
        const char *base = strrchr(t->path, '/'); base = base ? base + 1 : t->path;
        g_string_append_printf(s, "\nAktuell geöffnet: %s\n```\n", base);
        if (strlen(code) > 6000) { code[6000] = 0; g_string_append(s, code); g_string_append(s, "\n...(gekürzt)"); }
        else g_string_append(s, code);
        g_string_append(s, "\n```\n");
        g_free(code);
    }
    return g_string_free(s, FALSE);
}

/* ---- autonomer Modus: ```lang file=...``` Blöcke schreiben ---- */
static int apply_autonomous(App *app, const char *content)
{
    GRegex *re = g_regex_new(
        "```[^\\n]*\\bfile=([^\\s`\\n]+)[^\\n]*\\n(.*?)```",
        G_REGEX_DOTALL, 0, NULL);
    GMatchInfo *mi = NULL;
    int written = 0;
    g_regex_match(re, content, 0, &mi);
    while (mi && g_match_info_matches(mi)) {
        char *rel = g_match_info_fetch(mi, 1);
        char *code = g_match_info_fetch(mi, 2);
        if (rel && code && app->proj_root) {
            char *full = g_build_filename(app->proj_root, rel, NULL);
            char *dir = g_path_get_dirname(full);
            g_mkdir_with_parents(dir, 0755);
            if (g_file_set_contents(full, code, -1, NULL)) {
                written++;
                char *info = g_strdup_printf("📝 geschrieben: %s", rel);
                add_bubble(app, "system", info);
                g_free(info);
                /* offenen Tab neu laden */
                for (GList *l = app->tabs; l; l = l->next) {
                    EditorTab *tt = l->data;
                    if (tt->path && !strcmp(tt->path, full)) {
                        char *nc = NULL;
                        if (g_file_get_contents(full, &nc, NULL, NULL)) {
                            gtk_text_buffer_set_text(tt->buffer, nc, -1);
                            tt->modified = FALSE;
                            g_free(nc);
                        }
                    }
                }
            }
            g_free(dir); g_free(full);
        }
        g_free(rel); g_free(code);
        g_match_info_next(mi, NULL);
    }
    g_match_info_free(mi);
    g_regex_unref(re);
    if (written) project_refresh_tree(app);
    return written;
}

/* ============================================================
 * Tool-Calling: Werkzeug-Schema im Request
 * ============================================================ */
static void prop_str(JsonBuilder *b, const char *name, const char *desc)
{
    json_builder_set_member_name(b, name);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");        json_builder_add_string_value(b, "string");
    json_builder_set_member_name(b, "description"); json_builder_add_string_value(b, desc);
    json_builder_end_object(b);
}
static void prop_int(JsonBuilder *b, const char *name, const char *desc)
{
    json_builder_set_member_name(b, name);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");        json_builder_add_string_value(b, "integer");
    json_builder_set_member_name(b, "description"); json_builder_add_string_value(b, desc);
    json_builder_end_object(b);
}
static void begin_fn(JsonBuilder *b, const char *name, const char *desc)
{
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");     json_builder_add_string_value(b, "function");
    json_builder_set_member_name(b, "function"); json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");        json_builder_add_string_value(b, name);
    json_builder_set_member_name(b, "description"); json_builder_add_string_value(b, desc);
    json_builder_set_member_name(b, "parameters");  json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");       json_builder_add_string_value(b, "object");
    json_builder_set_member_name(b, "properties"); json_builder_begin_object(b);
}
static void end_fn(JsonBuilder *b, const char **req, int n)
{
    json_builder_end_object(b);                 /* properties */
    json_builder_set_member_name(b, "required");
    json_builder_begin_array(b);
    for (int i = 0; i < n; i++) json_builder_add_string_value(b, req[i]);
    json_builder_end_array(b);
    json_builder_end_object(b);                 /* parameters */
    json_builder_end_object(b);                 /* function   */
    json_builder_end_object(b);                 /* tool       */
}

static void append_tools(JsonBuilder *b, App *app)
{
    json_builder_set_member_name(b, "tools");
    json_builder_begin_array(b);

    begin_fn(b, "list_directory", "Listet die Ordnerstruktur eines Projektverzeichnisses auf.");
    prop_str(b, "path", "Relativer Pfad im Projekt. Standard: '.' (Projektwurzel).");
    { const char *r[] = {}; end_fn(b, r, 0); }

    begin_fn(b, "read_file",
        "Lädt den Inhalt einer Datei. Mit start_line/end_line kann ein Zeilenbereich "
        "gelesen werden – effizienter als die ganze Datei bei großen Dateien.");
    prop_str(b, "filepath", "Relativer Pfad der zu lesenden Datei.");
    prop_int(b, "start_line", "Erste Zeile (1-basiert). Optional.");
    prop_int(b, "end_line",   "Letzte Zeile (1-basiert). Optional.");
    { const char *r[] = {"filepath"}; end_fn(b, r, 1); }

    begin_fn(b, "search_project", "Durchsucht das gesamte Projekt nach einem Stichwort (Variablen, Klassen, Text).");
    prop_str(b, "keyword", "Suchbegriff.");
    { const char *r[] = {"keyword"}; end_fn(b, r, 1); }

    begin_fn(b, "project_status",
        "Liefert den Zustand der laufenden Umgebung: Projekttyp, Sandbox-Backend, "
        "welche Dienste laufen (Webserver, Datenbank, Mail-Catcher, GUI-Emulator), "
        "deren Ports und Adressen, die Zugangsdaten der Projekt-Datenbank sowie "
        "die aktuell offenen Compiler-/Syntaxfehler.");
    { const char *r[] = {}; end_fn(b, r, 0); }

    begin_fn(b, "write_file", "Legt eine NEUE Datei an. Schlägt fehl, wenn die Datei bereits existiert.");
    prop_str(b, "filepath", "Relativer Pfad der neuen Datei.");
    prop_str(b, "content", "Vollständiger Inhalt der neuen Datei.");
    { const char *r[] = {"filepath", "content"}; end_fn(b, r, 2); }

    begin_fn(b, "replace_in_file", "Ersetzt einen exakten Textblock in einer bestehenden Datei. Bricht strikt ab, wenn der Suchblock nicht zu 100% vorkommt.");
    prop_str(b, "filepath", "Relativer Pfad der Datei.");
    prop_str(b, "search_text", "Der exakt zu findende Textblock.");
    prop_str(b, "replace_text", "Der Ersatztext.");
    { const char *r[] = {"filepath", "search_text", "replace_text"}; end_fn(b, r, 3); }

    begin_fn(b, "run_command",
        "Führt einen beliebigen Shell-Befehl im Projektordner in der Sandbox aus. "
        "Nutze es für: grep/sed/awk/find zum Suchen und Ersetzen; php -l / "
        "python3 -m py_compile / node --check zur Syntaxprüfung; curl zum Testen "
        "von HTTP-Endpunkten; ls/stat/diff zur Analyse; composer/npm/pip zum "
        "Installieren. stdout und stderr kommen zusammen zurück. "
        "Bevorzuge run_command gegenüber mehreren einzelnen read_file-Aufrufen.");
    prop_str(b, "command", "Shell-Befehl. Wird in der Projekt-Sandbox ausgeführt.");
    { const char *r[] = {"command"}; end_fn(b, r, 1); }

    begin_fn(b, "get_url",
        "HTTP-GET auf eine URL – typischerweise den laufenden Webserver des Projekts. "
        "Gibt HTTP-Status und die ersten 6000 Zeichen des Response-Body zurück. "
        "Nutze es um: Seiten auf Fehler zu prüfen (500, 404), HTML-Ausgabe zu lesen, "
        "PHP-Fehlerseiten zu entdecken, API-Endpoints zu testen. "
        "Relative Pfade (z.B. '/login.php') werden automatisch gegen die "
        "Webserver-URL des Projekts aufgelöst.");
    prop_str(b, "url", "Absolute URL (http://…) oder relativer Pfad (/seite.php).");
    { const char *r[] = {"url"}; end_fn(b, r, 1); }

    begin_fn(b, "run_build",
        "Löst den Build des Projekts aus und gibt Compiler-Ausgabe + Fehler zurück. "
        "Nutze es nach Änderungen um sofort zu prüfen ob der Code compiliert. "
        "Fehler werden automatisch in die IDE-Fehlerleiste eingetragen.");
    { const char *r[] = {}; end_fn(b, r, 0); }

    /* Custom-Werkzeuge aus ~/.config/hypercore/tools/ anhängen. */
    for (GList *l = app->custom_tools; l; l = l->next)
        custom_tool_append_schema((HcCustomTool *)l->data, b);

    json_builder_end_array(b);
    json_builder_set_member_name(b, "tool_choice");
    json_builder_add_string_value(b, "auto");
}

/* Rolle einer Nachricht lesen. */
static const char *msg_role(JsonNode *n)
{
    if (!n || !JSON_NODE_HOLDS_OBJECT(n)) return "";
    JsonObject *o = json_node_get_object(n);
    return json_object_has_member(o, "role")
        ? json_object_get_string_member(o, "role") : "";
}

/* Smart Context: nicht der ganze Verlauf, aber genug, dass Werkzeug-
 * ergebnisse erhalten bleiben. Index, ab dem gesendet wird. */
static guint context_start(GPtrArray *msgs, int window)
{
    if (window <= 0) window = 32;
    if ((int)msgs->len <= window) return 0;
    guint start = msgs->len - (guint)window;
    /* Nie mitten in einem tool_calls/tool-Paar anfangen: eine
     * "tool"-Nachricht ohne den zugehoerigen Assistenz-Aufruf davor
     * weisen OpenAI-kompatible Server zurueck. */
    while (start < msgs->len &&
           !g_strcmp0(msg_role(g_ptr_array_index(msgs, start)), "tool"))
        start++;
    return start;
}

/* Body aus einer Nachrichtenliste (JsonNode*-Objekte) + Werkzeuge. */
static char *build_agent_body(App *app, GPtrArray *msgs)
{
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "model");
    json_builder_add_string_value(b, app->settings.agent_model ? app->settings.agent_model : "local-model");
    json_builder_set_member_name(b, "messages");
    json_builder_begin_array(b);
    guint start = context_start(msgs, app->settings.agent_ctx_window);
    for (guint i = 0; i < msgs->len; i++) {
        /* Systemnachrichten (Prompt, RAG-Kontext) immer mitsenden. */
        if (i < start && g_strcmp0(msg_role(g_ptr_array_index(msgs, i)), "system") != 0)
            continue;
        json_builder_add_value(b, json_node_copy(g_ptr_array_index(msgs, i)));
    }
    json_builder_end_array(b);
    append_tools(b, app);
    /* max_tokens: 0 = automatisch je Persona, sonst Settings-Wert. */
    int max_tok = app->settings.agent_max_tokens > 0
        ? app->settings.agent_max_tokens
        : (app->settings.persona == 2 ? 4096 : 2048);
    json_builder_set_member_name(b, "max_tokens");  json_builder_add_int_value(b, max_tok);
    json_builder_set_member_name(b, "temperature"); json_builder_add_double_value(b, app->settings.agent_temperature);
    json_builder_set_member_name(b, "stream");       json_builder_add_boolean_value(b, FALSE);
    json_builder_end_object(b);

    JsonGenerator *g = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(g, root);
    char *out = json_generator_to_data(g, NULL);
    json_node_free(root);
    g_object_unref(g);
    g_object_unref(b);
    return out;
}

/* Message-Knoten {role, content}. */
static JsonNode *mk_msg(const char *role, const char *content)
{
    JsonObject *o = json_object_new();
    json_object_set_string_member(o, "role", role);
    json_object_set_string_member(o, "content", content ? content : "");
    JsonNode *n = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(n, o);
    return n;
}

#define AGENT_MAX_ITER 12

typedef struct {
    App        *app;
    GPtrArray  *msgs;       /* Sichtfenster fuer diesen Request (geliehen) */
    int         iter;
    GtkWidget  *thinking;
    gboolean    used_tools; /* hat der Agent in diesem Lauf gearbeitet?   */
    int         nudged;     /* wie oft wurde schon nachgefasst? (max 3)  */
    /* Read-before-Write Guard: Dateipfade die in diesem Lauf mit
     * read_file gelesen wurden. replace_in_file/patch_file pruefen
     * ob die Datei schon gelesen wurde; wenn nicht, wird ein
     * automatisches read_file injiziert. */
    GHashTable *read_files;  /* gchar* → gboolean, owned */
} AgentRun;

static void agent_run_free(AgentRun *r)
{
    if (r->read_files) g_hash_table_destroy(r->read_files);
    /* msgs zeigt auf app->agent_msgs und wird dort verwaltet. */
    g_free(r);
}

static void agent_post(AgentRun *run);  /* fwd */

static void remove_thinking(AgentRun *run)
{
    if (run->thinking) {
        /* run->thinking ist jetzt die row selbst (von add_thinking_bubble). */
        gtk_box_remove(GTK_BOX(run->app->agent_log), run->thinking);
        run->thinking = NULL;
    }
}

/* Enthaelt die Antwort einen Codeblock, also Code, der im Chat landet
 * statt in einer Datei? */
static gboolean looks_like_chat_code(const char *txt)
{
    if (!txt) return FALSE;

    /* 1. Klassischer Codeblock mit Backticks. */
    const char *f = strstr(txt, "```");
    if (f) {
        const char *end = strstr(f + 3, "```");
        if (end && (end - f) > 60) return TRUE;
    }

    /* 2. Agent gibt JSON eines Tool-Calls als Text aus.
     * Erkennungszeichen: {"name": "write_file" oder "replace_in_file"
     * als roher Text – das bedeutet das Modell hat den Tool-Call
     * textuell geschrieben statt ihn auszuführen. */
    if (strstr(txt, "\"write_file\"")     ||
        strstr(txt, "\"replace_in_file\"") ||
        strstr(txt, "\"filepath\"")        ||
        strstr(txt, "\"search_text\"")     ||
        strstr(txt, "\"run_command\""))
        return TRUE;

    /* 3. Dateiinhalt als Text: mehrzeiliger Block mit Einrueckung,
     * typisch für "hier ist der Code"-Ausgaben ohne Backticks. */
    int newlines = 0;
    for (const char *p = txt; *p; p++)
        if (*p == '\n') { newlines++; if (newlines >= 8) return TRUE; }

    return FALSE;
}

static void finish_run(AgentRun *run, const char *final_content)
{
    App *app = run->app;
    remove_thinking(run);

    /* Nachfassen: Der Agent hat kein einziges Werkzeug benutzt, aber Code
     * in den Chat geschrieben. Genau das soll er nicht. Einmal
     * unmissverstaendlich auffordern und erneut senden – danach wird die
     * Antwort so akzeptiert, wie sie ist (kein Endlos-Pingpong). */
    /* Mehrfach-Nudge: bis zu 3 Versuche bevor wir aufgeben.
     * nudged zählt wie oft wir schon nachgefasst haben. */
    if (!run->used_tools && run->nudged < 3 && looks_like_chat_code(final_content)) {
        run->nudged++;
        char hint[256];
        snprintf(hint, sizeof(hint),
            "⟳ Agent hat Text statt Werkzeug benutzt – Korrektur %d/3", run->nudged);
        add_bubble(app, "system", hint);

        /* Eskalierende Direktheit je nach Versuch */
        const char *correction;
        if (run->nudged == 1)
            correction =
                "FEHLER: Du hast Text in den Chat geschrieben statt ein Werkzeug aufzurufen. "
                "Das ist nicht erlaubt. "
                "JETZT: Rufe write_file oder replace_in_file auf um die Aufgabe zu erledigen. "
                "Schreibe keinen Code, kein JSON, keine Codeblöcke in den Chat.";
        else if (run->nudged == 2)
            correction =
                "FEHLER WIEDERHOLT: Kein Text in den Chat – NUR Werkzeuge benutzen. "
                "Führe die Aufgabe aus: write_file(filepath=..., content=...) "
                "oder replace_in_file(filepath=..., search_text=..., replace_text=...). "
                "Keine Erklärungen, kein Code, keine Blöcke. Nur den Tool-Call.";
        else
            correction =
                "LETZTER VERSUCH: Benutze JETZT sofort write_file oder replace_in_file. "
                "Jede andere Antwort ist falsch. Nur Tool-Aufruf, nichts sonst.";

        g_ptr_array_add(app->agent_msgs, mk_msg("user", correction));
        run->thinking = add_thinking_bubble(app);
        agent_post(run);
        return;
    }

    if (app->agent_activity) gtk_label_set_text(GTK_LABEL(app->agent_activity), "");
    if (final_content && *final_content) {
        add_bubble(app, "assistant", final_content);
        /* Antwort im Verlauf behalten, damit der Agent sich erinnert. */
        g_ptr_array_add(app->agent_msgs, mk_msg("assistant", final_content));
        g_free(app->last_assistant);
        app->last_assistant = g_strdup(final_content);
        /* Codeblock-Fallback nur noch im autonomen Modus und nur, wenn der
         * Agent gar keine Werkzeuge benutzt hat. */
        if (!run->used_tools && app->agent_auto_toggle &&
            gtk_check_button_get_active(GTK_CHECK_BUTTON(app->agent_auto_toggle)))
            apply_autonomous(app, final_content);
    } else {
        add_bubble(app, "system", "(Agent hat keine Textantwort geliefert.)");
    }
    app->agent_busy = FALSE;
    hc_set_activity(app, NULL);
    agent_run_free(run);
}

static void on_agent_resp(GObject *src, GAsyncResult *res, gpointer ud)
{
    AgentRun *run = ud;
    App *app = run->app;
    GError *e = NULL;
    GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &e);

    if (!body) {
        remove_thinking(run);
        char *m = g_strdup_printf("⚠ Verbindung fehlgeschlagen: %s\n(Agent-Server unter %s:%d erreichbar?)",
            e ? e->message : "?", app->settings.agent_host, app->settings.agent_port);
        add_bubble(app, "system", m);
        g_free(m); g_clear_error(&e);
        app->agent_busy = FALSE;
        hc_set_activity(app, NULL);
        agent_run_free(run);
        return;
    }

    gsize len = 0;
    const char *data = g_bytes_get_data(body, &len);
    JsonParser *p = json_parser_new();
    gboolean parsed = json_parser_load_from_data(p, data, len, NULL);
    JsonObject *msgobj = NULL, *rootobj = NULL;
    if (parsed) {
        JsonNode *root = json_parser_get_root(p);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            rootobj = json_node_get_object(root);
            if (json_object_has_member(rootobj, "choices")) {
                JsonArray *ch = json_object_get_array_member(rootobj, "choices");
                if (json_array_get_length(ch) > 0)
                    msgobj = json_array_get_object_element(ch, 0);
                if (msgobj && json_object_has_member(msgobj, "message"))
                    msgobj = json_object_get_object_member(msgobj, "message");
            }
        }
    }

    if (!msgobj) {
        /* evtl. Fehlerobjekt */
        char *err = NULL;
        char *c = parse_content(data, len, &err);
        remove_thinking(run);
        char *m = g_strdup_printf("⚠ %s", err ? err : (c ? c : "Keine verwertbare Antwort."));
        add_bubble(app, "system", m);
        g_free(m); g_free(err); g_free(c);
        g_object_unref(p); g_bytes_unref(body);
        app->agent_busy = FALSE;
        hc_set_activity(app, NULL);
        agent_run_free(run);
        return;
    }

    /* content (String oder null) */
    const char *content = NULL;
    if (json_object_has_member(msgobj, "content")) {
        JsonNode *cn = json_object_get_member(msgobj, "content");
        if (JSON_NODE_HOLDS_VALUE(cn) && json_node_get_value_type(cn) == G_TYPE_STRING)
            content = json_node_get_string(cn);
    }
    /* tool_calls? */
    JsonArray *tcs = NULL;
    if (json_object_has_member(msgobj, "tool_calls")) {
        JsonNode *tn = json_object_get_member(msgobj, "tool_calls");
        if (JSON_NODE_HOLDS_ARRAY(tn)) tcs = json_node_get_array(tn);
    }

    if (tcs && json_array_get_length(tcs) > 0 && run->iter < AGENT_MAX_ITER) {
        remove_thinking(run);
        run->used_tools = TRUE;

        /* Assistenz-Nachricht (mit tool_calls) sauber nachbilden + anhängen. */
        JsonObject *amsg = json_object_new();
        json_object_set_string_member(amsg, "role", "assistant");
        if (content) json_object_set_string_member(amsg, "content", content);
        else         json_object_set_null_member(amsg, "content");
        json_object_set_member(amsg, "tool_calls",
            json_node_copy(json_object_get_member(msgobj, "tool_calls")));
        JsonNode *an = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(an, amsg);
        g_ptr_array_add(run->msgs, an);

        /* jeden Tool-Aufruf ausführen + Ergebnis anhängen. */
        guint n = json_array_get_length(tcs);
        for (guint i = 0; i < n; i++) {
            JsonObject *tc = json_array_get_object_element(tcs, i);
            const char *id = json_object_has_member(tc, "id")
                ? json_object_get_string_member(tc, "id") : "";
            JsonObject *fn = json_object_has_member(tc, "function")
                ? json_object_get_object_member(tc, "function") : NULL;
            const char *fname = fn && json_object_has_member(fn, "name")
                ? json_object_get_string_member(fn, "name") : "?";
            char *args_str = NULL;
            if (fn && json_object_has_member(fn, "arguments")) {
                JsonNode *argn = json_object_get_member(fn, "arguments");
                if (JSON_NODE_HOLDS_VALUE(argn))
                    args_str = g_strdup(json_node_get_string(argn));
                else {
                    JsonGenerator *gg = json_generator_new();
                    json_generator_set_root(gg, argn);
                    args_str = json_generator_to_data(gg, NULL);
                    g_object_unref(gg);
                }
            }

            char *preview = args_str ? g_strndup(args_str, 80) : g_strdup("");
            char *bub = g_strdup_printf("🔧 %s(%s)", fname, preview);
            add_bubble(app, "system", bub);
            if (app->agent_activity) {
                char *act = g_strdup_printf("%s …", fname);
                gtk_label_set_text(GTK_LABEL(app->agent_activity), act);
                g_free(act);
            }
            /* Werkzeugname auch in die zentrale Statusleiste. */
            {
                char abuf[64];
                snprintf(abuf, sizeof(abuf), "🔧 Agent: %s …", fname);
                hc_set_activity(app, abuf);
            }
            g_free(bub); g_free(preview);

            /* ---- Read-before-Write Guard ---- */
            /* Wenn replace_in_file oder write_file aufgerufen wird und die
             * Datei noch nicht in diesem Lauf gelesen wurde, wird stattdessen
             * automatisch read_file ausgefuehrt und eine Korrektur-Nachricht
             * an das Modell gesendet. */
            gboolean is_write = (!g_strcmp0(fname, "replace_in_file") ||
                                 !g_strcmp0(fname, "write_file"));
            if (is_write && args_str && *args_str) {
                /* Filepath aus args_json extrahieren. */
                const char *fp = NULL;
                JsonParser *jp = json_parser_new();
                if (json_parser_load_from_data(jp, args_str, -1, NULL)) {
                    JsonNode *jr = json_parser_get_root(jp);
                    if (jr && JSON_NODE_HOLDS_OBJECT(jr)) {
                        JsonObject *jo = json_node_get_object(jr);
                        if (json_object_has_member(jo, "filepath"))
                            fp = json_object_get_string_member(jo, "filepath");
                        else if (json_object_has_member(jo, "path"))
                            fp = json_object_get_string_member(jo, "path");
                    }
                }
                if (fp && *fp) {
                    if (!run->read_files)
                        run->read_files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
                    if (!g_hash_table_contains(run->read_files, fp) &&
                        !g_strcmp0(fname, "replace_in_file")) {
                        /* Datei noch nicht gelesen – auto read_file ausloesen. */
                        char *auto_args = g_strdup_printf("{\"filepath\":\"%s\"}", fp);
                        gboolean rfok = FALSE;
                        char *rf_result = agent_tools_dispatch(app, "read_file", auto_args, &rfok);
                        g_free(auto_args);
                        /* read_file-Ergebnis in den Kontext eintragen. */
                        JsonObject *auto_msg = json_object_new();
                        json_object_set_string_member(auto_msg, "role", "tool");
                        json_object_set_string_member(auto_msg, "tool_call_id", id);
                        json_object_set_string_member(auto_msg, "name", "read_file");
                        char *rf_content = g_strdup_printf(
                            "[AUTO-READ vor replace_in_file]\n%s", rf_result);
                        json_object_set_string_member(auto_msg, "content", rf_content);
                        g_free(rf_content); g_free(rf_result);
                        JsonNode *auto_node = json_node_new(JSON_NODE_OBJECT);
                        json_node_take_object(auto_node, auto_msg);
                        g_ptr_array_add(run->msgs, auto_node);
                        /* Korrektur-Hinweis an das Modell. */
                        JsonObject *hint_msg = json_object_new();
                        json_object_set_string_member(hint_msg, "role", "user");
                        json_object_set_string_member(hint_msg, "content",
                            "SYSTEM: replace_in_file wurde aufgerufen ohne die Datei "
                            "vorher zu lesen. Der Dateiinhalt wurde automatisch geladen "
                            "(siehe auto-read oben). Bitte replace_in_file ERNEUT "
                            "aufrufen mit einem Suchblock der EXAKT so im Dateiinhalt "
                            "vorkommt. Kein Raten, kein Umformulieren.");
                        JsonNode *hint_node = json_node_new(JSON_NODE_OBJECT);
                        json_node_take_object(hint_node, hint_msg);
                        g_ptr_array_add(run->msgs, hint_node);
                        g_hash_table_insert(run->read_files, g_strdup(fp), GINT_TO_POINTER(1));
                        /* Tool-Call als Fehler markieren und weiter. */
                        add_bubble(app, "system",
                            "   ⚠ Auto-Read: Datei wurde automatisch geladen. "
                            "replace_in_file wird im naechsten Schritt erneut versucht.");
                        g_object_unref(jp);
                        goto next_iter;
                    }
                }
                if (fp && !run->read_files)
                    run->read_files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
                if (fp) g_hash_table_insert(run->read_files, g_strdup(fp), GINT_TO_POINTER(1));
                g_object_unref(jp);
            }
            /* read_file: Datei als gelesen markieren. */
            if (!g_strcmp0(fname, "read_file") && args_str) {
                JsonParser *jp2 = json_parser_new();
                if (json_parser_load_from_data(jp2, args_str, -1, NULL)) {
                    JsonNode *jr2 = json_parser_get_root(jp2);
                    if (jr2 && JSON_NODE_HOLDS_OBJECT(jr2)) {
                        JsonObject *jo2 = json_node_get_object(jr2);
                        const char *fp2 = NULL;
                        if (json_object_has_member(jo2, "filepath"))
                            fp2 = json_object_get_string_member(jo2, "filepath");
                        else if (json_object_has_member(jo2, "path"))
                            fp2 = json_object_get_string_member(jo2, "path");
                        if (fp2 && *fp2) {
                            if (!run->read_files)
                                run->read_files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
                            g_hash_table_insert(run->read_files, g_strdup(fp2), GINT_TO_POINTER(1));
                        }
                    }
                }
                g_object_unref(jp2);
            }
            /* Nach fehlgeschlagenem replace_in_file: Hinweis injizieren. */

            gboolean ok = FALSE;
            char *result = agent_tools_dispatch(app, fname, args_str, &ok);
            g_free(args_str);

            if (!ok && !g_strcmp0(fname, "replace_in_file")) {
                /* Fehler beim Ersetzen: Korrekturhinweis an Modell. */
                JsonObject *fix_msg = json_object_new();
                json_object_set_string_member(fix_msg, "role", "user");
                json_object_set_string_member(fix_msg, "content",
                    "SYSTEM: replace_in_file ist fehlgeschlagen weil der Suchblock "
                    "nicht exakt im Dateiinhalt vorkommt. PFLICHT: Lies die Datei "
                    "JETZT mit read_file (mit start_line/end_line um den relevanten "
                    "Bereich zu sehen), dann verwende den EXAKTEN Text aus der Datei "
                    "als search_text. Niemals den Suchblock aus dem Gedaechtnis "
                    "rekonstruieren.");
                JsonNode *fix_node = json_node_new(JSON_NODE_OBJECT);
                json_node_take_object(fix_node, fix_msg);
                g_ptr_array_add(run->msgs, fix_node);
                /* Datei aus read_files entfernen damit sie beim naechsten
                 * replace_in_file-Versuch automatisch neu gelesen wird. */
                if (run->read_files) {
                    /* Filepath nochmal extrahieren. */
                    JsonParser *jp3 = json_parser_new();
                    if (json_parser_load_from_data(jp3, args_str ? args_str : "{}", -1, NULL)) {
                        JsonNode *jr3 = json_parser_get_root(jp3);
                        if (jr3 && JSON_NODE_HOLDS_OBJECT(jr3)) {
                            JsonObject *jo3 = json_node_get_object(jr3);
                            const char *fp3 = json_object_has_member(jo3,"filepath")
                                ? json_object_get_string_member(jo3,"filepath") : NULL;
                            if (fp3) g_hash_table_remove(run->read_files, fp3);
                        }
                    }
                    g_object_unref(jp3);
                }
            }

            /* kurzes Ergebnis anzeigen */
            char *rp = g_strndup(result, 200);
            char *rb = g_strdup_printf("%s%s", ok ? "   ↳ " : "   ⚠ ", rp);
            add_bubble(app, "system", rb);
            g_free(rb); g_free(rp);

            /* Tool-Nachricht anhängen. */
            JsonObject *tmsg = json_object_new();
            json_object_set_string_member(tmsg, "role", "tool");
            if (*id) json_object_set_string_member(tmsg, "tool_call_id", id);
            json_object_set_string_member(tmsg, "name", fname);
            json_object_set_string_member(tmsg, "content", result);
            JsonNode *tnode = json_node_new(JSON_NODE_OBJECT);
            json_node_take_object(tnode, tmsg);
            g_ptr_array_add(run->msgs, tnode);
            g_free(result);
        }

        next_iter:
        g_object_unref(p);
        g_bytes_unref(body);

        /* neue "denkt"-Blase + naechste Runde. */
        run->thinking = add_thinking_bubble(app);
        run->iter++;
        agent_post(run);
        return;
    }

    /* Keine (weiteren) Tool-Aufrufe -> finale Antwort. */
    char *final = content ? g_strdup(content) : NULL;
    g_object_unref(p);
    g_bytes_unref(body);
    finish_run(run, final);
    g_free(final);
}

static void agent_post(AgentRun *run)
{
    App *app = run->app;
    char *body = build_agent_body(app, run->msgs);
    char *url = agent_url(app);
    SoupMessage *msg = soup_message_new("POST", url);
    g_free(url);
    if (app->settings.agent_key && *app->settings.agent_key) {
        char *auth = g_strdup_printf("Bearer %s", app->settings.agent_key);
        soup_message_headers_append(soup_message_get_request_headers(msg),
                                    "Authorization", auth);
        g_free(auth);
    }
    GBytes *bb = g_bytes_new_take(body, strlen(body));
    soup_message_set_request_body_from_bytes(msg, "application/json", bb);
    g_bytes_unref(bb);
    soup_session_send_and_read_async(agent_session(app), msg, G_PRIORITY_DEFAULT, NULL,
                                     on_agent_resp, run);
    g_object_unref(msg);
}

/* Zweite Stufe des Sendens: nach RAG-Retrieval zusammenbauen + posten.
 * Smart Context Management: bewusst KEIN voller Verlauf, sondern nur
 *  System-Prompt, RAG-Kontext, letzte Agenten-Antwort (Kurzzeitgedächtnis)
 *  und die aktuelle Nutzer-Eingabe. */
typedef struct { App *app; char *text; GtkWidget *think; } SendCtx;

static void send_with_context(App *app, char *rag_ctx, gpointer ud)
{
    SendCtx *sc = ud;

    /* Der Verlauf lebt in app->agent_msgs und bleibt ueber Nutzerrunden
     * hinweg bestehen – inklusive Werkzeugaufrufe und deren Ergebnisse.
     * Sonst weiss der Agent in der naechsten Runde nicht mehr, was er
     * gerade gelesen oder geschrieben hat. */
    if (!app->agent_msgs)
        app->agent_msgs = g_ptr_array_new_with_free_func((GDestroyNotify)json_node_free);

    /* Index 0 ist immer der aktuelle System-Prompt (Projekt, offene
     * Datei und Persona aendern sich waehrend der Sitzung). */
    char *sysp_raw = build_system_prompt(app);
    char *sysp;
    #ifdef HC_DEBUG
    if (app->debug_prompt_override && *app->debug_prompt_override) {
        sysp = g_strconcat(app->debug_prompt_override, "\n\n", sysp_raw, NULL);
        g_free(sysp_raw);
    } else sysp = sysp_raw;
    #else
    sysp = sysp_raw;
    #endif
    if (app->agent_msgs->len == 0) {
        g_ptr_array_add(app->agent_msgs, mk_msg("system", sysp));
    } else {
        json_node_free(g_ptr_array_index(app->agent_msgs, 0));
        g_ptr_array_index(app->agent_msgs, 0) = mk_msg("system", sysp);
    }
    g_free(sysp);

    /* RAG-Kontext: alte system-Nachrichten mit RAG-Inhalt ZUERST entfernen,
     * damit sich der Kontext nicht unbegrenzt aufschichtet. Index 0 ist
     * immer der System-Prompt – der bleibt, alle anderen "system"-Nachrichten
     * mit RAG-Prefix werden gelöscht. */
    for (guint i = app->agent_msgs->len; i > 1; i--) {
        JsonNode *node = g_ptr_array_index(app->agent_msgs, i - 1);
        if (!g_strcmp0(msg_role(node), "system")) {
            JsonObject *o = json_node_get_object(node);
            const char *c = json_object_has_member(o, "content")
                ? json_object_get_string_member(o, "content") : "";
            if (g_str_has_prefix(c, "RAG-Kontext") ||
                g_str_has_prefix(c, "Letzte Agenten-Antwort")) {
                json_node_free(node);
                g_ptr_array_remove_index(app->agent_msgs, i - 1);
            }
        }
    }

    /* Letzte Agenten-Antwort als Kurzzeitgedächtnis einbauen (sofern
     * vorhanden). Ohne das "vergisst" der Agent was er gerade getan hat. */
    if (app->last_assistant && *app->last_assistant) {
        char *la = g_strdup_printf(
            "Letzte Agenten-Antwort (Kurzzeitgedächtnis):\n%s",
            app->last_assistant);
        g_ptr_array_add(app->agent_msgs, mk_msg("system", la));
        g_free(la);
    }

    if (rag_ctx && *rag_ctx) {
        char *rc = g_strdup_printf(
            "RAG-Kontext (relevante Projektausschnitte):\n%s", rag_ctx);
        g_ptr_array_add(app->agent_msgs, mk_msg("system", rc));
        g_free(rc);
    }
    g_free(rag_ctx);

    g_ptr_array_add(app->agent_msgs, mk_msg("user", sc->text));

    AgentRun *run = g_new0(AgentRun, 1);
    run->app = app;
    run->msgs = app->agent_msgs;
    run->iter = 0;
    run->thinking = sc->think;
    agent_post(run);

    g_free(sc->text);
    g_free(sc);
}

/* ---- Einmalige LLM-Anfrage ohne Werkzeuge (z.B. Commit-Message) ---- */
typedef struct { App *app; HcOneshotCb cb; gpointer ud; } OneshotCtx;

static void oneshot_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    OneshotCtx *o = ud;
    GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, NULL);
    char *text = NULL;
    if (body) {
        gsize len = 0; const char *data = g_bytes_get_data(body, &len);
        text = parse_content(data, len, NULL);
        g_bytes_unref(body);
    }
    o->cb(o->app, text, o->ud);
    g_free(o);
}

void agent_oneshot(App *app, const char *system, const char *user,
                   HcOneshotCb cb, gpointer ud)
{
    if (!agent_configured(app)) { cb(app, NULL, ud); return; }
    GList *turns = NULL;
    if (system) turns = g_list_append(turns, turn_new("system", system));
    turns = g_list_append(turns, turn_new("user", user));
    char *body = build_body(app->settings.agent_model, turns, 256, 0.2);
    g_list_free_full(turns, turn_free);

    char *url = agent_url(app);
    SoupMessage *msg = soup_message_new("POST", url);
    g_free(url);
    if (app->settings.agent_key && *app->settings.agent_key) {
        char *auth = g_strdup_printf("Bearer %s", app->settings.agent_key);
        soup_message_headers_append(soup_message_get_request_headers(msg),
                                    "Authorization", auth);
        g_free(auth);
    }
    GBytes *bb = g_bytes_new_take(body, strlen(body));
    soup_message_set_request_body_from_bytes(msg, "application/json", bb);
    g_bytes_unref(bb);
    OneshotCtx *o = g_new0(OneshotCtx, 1);
    o->app = app; o->cb = cb; o->ud = ud;
    soup_session_send_and_read_async(agent_session(app), msg, G_PRIORITY_DEFAULT, NULL,
                                     oneshot_done, o);
    g_object_unref(msg);
}

/* Neues Projekt = neuer Kontext. Sonst schleppt der Agent Dateien und
 * Werkzeugergebnisse des Vorgaengerprojekts mit. */
void agent_reset_context(App *app)
{
    if (app->agent_msgs) {
        g_ptr_array_free(app->agent_msgs, TRUE);
        app->agent_msgs = NULL;
    }
    g_clear_pointer(&app->last_assistant, g_free);
}

void agent_send_user_message(App *app, const char *text)
{
    if (!text || !*text) return;
    if (!agent_configured(app)) {
        agent_refresh_availability(app);
        return;
    }
    if (app->agent_busy) return;
    app->agent_busy = TRUE;
    hc_set_activity(app, "Agent denkt …");

    add_bubble(app, "user", text);

    GtkWidget *think = add_thinking_bubble(app);

    SendCtx *sc = g_new0(SendCtx, 1);
    sc->app = app; sc->text = g_strdup(text); sc->think = think;
    /* RAG-Kontext abrufen (asynchron), dann senden. */
    rag_query_async(app, text, send_with_context, sc);
}

/* ============================================================
 * Ghost-Text
 * ============================================================ */
static void set_ghost_tag(GtkTextBuffer *b)
{
    GtkTextTagTable *tt = gtk_text_buffer_get_tag_table(b);
    if (!gtk_text_tag_table_lookup(tt, "hc-ghost"))
        gtk_text_buffer_create_tag(b, "hc-ghost",
            "foreground", "#7d8694", "style", PANGO_STYLE_ITALIC, NULL);
}

void agent_cancel_ghost(EditorTab *t)
{
    if (!t || !t->ghost_active) return;
    GtkTextMark *ms = gtk_text_buffer_get_mark(t->buffer, "hc_ghost_start");
    GtkTextMark *me = gtk_text_buffer_get_mark(t->buffer, "hc_ghost_end");
    if (ms && me) {
        GtkTextIter a, z;
        gtk_text_buffer_get_iter_at_mark(t->buffer, &a, ms);
        gtk_text_buffer_get_iter_at_mark(t->buffer, &z, me);
        t->ghost_ignore_change = TRUE;
        gtk_text_buffer_delete(t->buffer, &a, &z);
        t->ghost_ignore_change = FALSE;
        gtk_text_buffer_delete_mark(t->buffer, ms);
        gtk_text_buffer_delete_mark(t->buffer, me);
    }
    t->ghost_active = FALSE;
    t->ghost_len = 0;
}

static gboolean accept_ghost(EditorTab *t)
{
    if (!t->ghost_active) return FALSE;
    GtkTextMark *ms = gtk_text_buffer_get_mark(t->buffer, "hc_ghost_start");
    GtkTextMark *me = gtk_text_buffer_get_mark(t->buffer, "hc_ghost_end");
    if (ms && me) {
        GtkTextIter a, z;
        gtk_text_buffer_get_iter_at_mark(t->buffer, &a, ms);
        gtk_text_buffer_get_iter_at_mark(t->buffer, &z, me);
        /* Ghost-Stil entfernen -> Text wird "echt". */
        GtkTextTag *tag = gtk_text_tag_table_lookup(
            gtk_text_buffer_get_tag_table(t->buffer), "hc-ghost");
        if (tag) gtk_text_buffer_remove_tag(t->buffer, tag, &a, &z);
        gtk_text_buffer_place_cursor(t->buffer, &z);
        gtk_text_buffer_delete_mark(t->buffer, ms);
        gtk_text_buffer_delete_mark(t->buffer, me);
        if (!t->modified) { t->modified = TRUE; }
    }
    t->ghost_active = FALSE;
    t->ghost_len = 0;
    return TRUE;
}

typedef struct { EditorTab *t; int req_offset; } GhostCtx;

static void on_ghost_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    GhostCtx *ctx = ud;
    EditorTab *t = ctx->t;
    GError *e = NULL;
    GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &e);
    if (!body) { g_clear_error(&e); g_free(ctx); return; }

    gsize len = 0;
    const char *data = g_bytes_get_data(body, &len);
    char *content = parse_content(data, len, NULL);
    g_bytes_unref(body);
    if (!content || !*content) { g_free(content); g_free(ctx); return; }

    /* Nur einfügen, wenn der Cursor seit der Anfrage nicht bewegt wurde
     * und kein Ghost schon aktiv ist. */
    GtkTextIter cur;
    gtk_text_buffer_get_iter_at_mark(t->buffer, &cur,
        gtk_text_buffer_get_insert(t->buffer));
    if (t->ghost_active || gtk_text_iter_get_offset(&cur) != ctx->req_offset) {
        g_free(content); g_free(ctx); return;
    }

    /* Nur die erste(n) Zeile(n) als Vorschlag, maximal ~160 Zeichen. */
    char *suggestion = content;
    /* führende Codefences/Backticks entfernen, falls das Modell welche liefert */
    while (*suggestion == '`' || *suggestion == '\n') suggestion++;
    char trimmed[200];
    g_strlcpy(trimmed, suggestion, sizeof(trimmed));
    char *fence = strstr(trimmed, "```");
    if (fence) *fence = 0;
    if (!*trimmed) { g_free(content); g_free(ctx); return; }

    set_ghost_tag(t->buffer);
    t->ghost_ignore_change = TRUE;
    GtkTextIter ins;
    gtk_text_buffer_get_iter_at_mark(t->buffer, &ins,
        gtk_text_buffer_get_insert(t->buffer));
    GtkTextMark *start = gtk_text_buffer_create_mark(t->buffer, "hc_ghost_start", &ins, TRUE);
    gtk_text_buffer_insert(t->buffer, &ins, trimmed, -1);
    GtkTextMark *end = gtk_text_buffer_create_mark(t->buffer, "hc_ghost_end", &ins, FALSE);
    GtkTextIter a, z;
    gtk_text_buffer_get_iter_at_mark(t->buffer, &a, start);
    gtk_text_buffer_get_iter_at_mark(t->buffer, &z, end);
    gtk_text_buffer_apply_tag_by_name(t->buffer, "hc-ghost", &a, &z);
    /* Cursor zurück vor den Ghost-Text. */
    gtk_text_buffer_get_iter_at_mark(t->buffer, &a, start);
    gtk_text_buffer_place_cursor(t->buffer, &a);
    t->ghost_ignore_change = FALSE;
    t->ghost_active = TRUE;
    t->ghost_len = g_utf8_strlen(trimmed, -1);

    g_free(content);
    g_free(ctx);
}

static gboolean ghost_fire(gpointer ud)
{
    EditorTab *t = ud;
    t->ghost_timer = 0;
    App *app = t->app;
    if (!app->settings.ghost_enabled || !agent_configured(app)) return G_SOURCE_REMOVE;
    if (t->ghost_active) return G_SOURCE_REMOVE;

    /* Präfix (vor Cursor) bis 1500 Zeichen. */
    GtkTextIter cur, startb;
    gtk_text_buffer_get_iter_at_mark(t->buffer, &cur,
        gtk_text_buffer_get_insert(t->buffer));
    int off = gtk_text_iter_get_offset(&cur);
    gtk_text_buffer_get_start_iter(t->buffer, &startb);
    char *prefix = gtk_text_buffer_get_text(t->buffer, &startb, &cur, FALSE);
    if (strlen(prefix) > 1500) {
        char *tail = g_strdup(prefix + strlen(prefix) - 1500);
        g_free(prefix); prefix = tail;
    }

    const char *langid = hc_lang_for_path(t->path);
    char *sys = g_strdup_printf(
        "Du vervollständigst Quellcode (%s). Gib AUSSCHLIESSLICH die direkte "
        "Fortsetzung ab der Cursorposition aus – kein Markdown, keine Erklärung, "
        "keine Wiederholung des vorhandenen Codes. Höchstens wenige Zeilen.",
        langid ? langid : "Text");

    GList *turns = NULL;
    turns = g_list_append(turns, turn_new("system", sys));
    turns = g_list_append(turns, turn_new("user", prefix));
    g_free(sys); g_free(prefix);

    char *reqbody = build_body(app->settings.agent_model, turns, 48, 0.1);
    g_list_free_full(turns, turn_free);

    char *url = agent_url(app);
    SoupMessage *msg = soup_message_new("POST", url);
    g_free(url);
    if (app->settings.agent_key && *app->settings.agent_key) {
        char *auth = g_strdup_printf("Bearer %s", app->settings.agent_key);
        soup_message_headers_append(soup_message_get_request_headers(msg),
                                    "Authorization", auth);
        g_free(auth);
    }
    GBytes *bb = g_bytes_new_take(reqbody, strlen(reqbody));
    soup_message_set_request_body_from_bytes(msg, "application/json", bb);
    g_bytes_unref(bb);

    GhostCtx *ctx = g_new0(GhostCtx, 1);
    ctx->t = t; ctx->req_offset = off;
    soup_session_send_and_read_async(agent_session(t->app), msg, G_PRIORITY_LOW, NULL,
                                     on_ghost_done, ctx);
    g_object_unref(msg);
    return G_SOURCE_REMOVE;
}

void agent_request_ghost(EditorTab *t)
{
    if (!t) return;
    if (t->ghost_timer) { g_source_remove(t->ghost_timer); t->ghost_timer = 0; }
    t->ghost_timer = g_timeout_add(700, ghost_fire, t);
}

/* ---- Tastatur-Controller am View (Tab = übernehmen) ---- */
static gboolean on_view_key(GtkEventControllerKey *c, guint keyval,
                            guint keycode, GdkModifierType state, gpointer ud)
{
    (void)c; (void)keycode; (void)state;
    EditorTab *t = ud;
    if (t->ghost_active) {
        if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) {
            accept_ghost(t);
            return TRUE;  /* Tab verbrauchen */
        }
        if (keyval == GDK_KEY_Escape) { agent_cancel_ghost(t); return TRUE; }
        /* jede andere Taste verwirft den Vorschlag, Taste wirkt normal. */
        agent_cancel_ghost(t);
    }
    return FALSE;
}

void agent_attach_ghost(EditorTab *t)
{
    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_view_key), t);
    gtk_widget_add_controller(t->view, kc);
}

/* ============================================================
 * Panel bauen
 * ============================================================ */
static void on_send(GtkWidget *w, gpointer ud)
{
    (void)w;
    App *app = ud;
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(app->agent_entry));
    if (txt && *txt) {
        char *copy = g_strdup(txt);
        gtk_editable_set_text(GTK_EDITABLE(app->agent_entry), "");
        agent_send_user_message(app, copy);
        g_free(copy);
    }
}

static void on_auto_toggled(GtkCheckButton *b, gpointer ud)
{
    App *app = ud;
    if (!gtk_check_button_get_active(b)) return;
    /* Rückfrage "Autonom arbeiten?" beim Einschalten. */
    GtkAlertDialog *dlg = gtk_alert_dialog_new(
        "Autonomen Modus aktivieren?");
    gtk_alert_dialog_set_detail(dlg,
        "Der Agent darf dann von ihm gelieferte Dateien (```file=…```) "
        "OHNE weitere Rückfrage direkt ins Projekt schreiben und offene "
        "Dateien überschreiben.");
    const char *btns[] = { "Abbrechen", "Aktivieren", NULL };
    gtk_alert_dialog_set_buttons(dlg, btns);
    gtk_alert_dialog_set_cancel_button(dlg, 0);
    gtk_alert_dialog_set_default_button(dlg, 1);
    /* Synchron einfach halten: bei Abbrechen Toggle zurücksetzen via choose. */
    gtk_alert_dialog_choose(dlg, GTK_WINDOW(app->window), NULL,
        (GAsyncReadyCallback)NULL, NULL);
    /* Hinweis im Chat. */
    add_bubble(app, "system",
        "Autonomer Modus aktiv: gelieferte ```file=…```-Blöcke werden direkt gespeichert.");
    g_object_unref(dlg);
}

static void on_persona_changed(GtkDropDown *d, GParamSpec *ps, gpointer ud)
{
    (void)ps;
    App *app = ud;
    app->settings.persona = gtk_drop_down_get_selected(d);
    settings_save(&app->settings);
}

static void on_open_settings_link(GtkButton *b, gpointer ud)
{ (void)b; settings_open_dialog((App*)ud); }

/* Graceful Degradation: Chat ein-/ausblenden je nach Konfiguration. */
void agent_refresh_availability(App *app)
{
    if (!app->agent_root_stack) return;
    gtk_stack_set_visible_child_name(GTK_STACK(app->agent_root_stack),
        agent_configured(app) ? "online" : "offline");
}

GtkWidget *agent_panel_new(App *app)
{
    app->agent_root_stack = gtk_stack_new();
    gtk_widget_set_vexpand(app->agent_root_stack, TRUE);

    /* ---- ONLINE: voller Chat ---- */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "hc-agent-panel");

    /* Persona-Auswahl oben. */
    GtkWidget *prow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(prow, 8); gtk_widget_set_margin_end(prow, 8);
    gtk_widget_set_margin_top(prow, 6);
    GtkWidget *plabel = gtk_label_new("Rolle:");
    gtk_widget_add_css_class(plabel, "dim-label");
    const char *personas[] = { "Architekt", "Planer", "Coder", NULL };
    app->agent_persona_drop = gtk_drop_down_new_from_strings(personas);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app->agent_persona_drop), app->settings.persona);
    gtk_widget_set_hexpand(app->agent_persona_drop, TRUE);
    g_signal_connect(app->agent_persona_drop, "notify::selected",
                     G_CALLBACK(on_persona_changed), app);
    gtk_box_append(GTK_BOX(prow), plabel);
    gtk_box_append(GTK_BOX(prow), app->agent_persona_drop);
    gtk_box_append(GTK_BOX(box), prow);

    /* Verlauf. */
    app->agent_log = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(app->agent_log, "hc-agent-log");
    gtk_widget_set_margin_start(app->agent_log, 8);
    gtk_widget_set_margin_end(app->agent_log, 8);
    gtk_widget_set_margin_top(app->agent_log, 8);
    gtk_widget_set_margin_bottom(app->agent_log, 8);
    app->agent_log_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(app->agent_log_scroll), app->agent_log);
    gtk_widget_set_vexpand(app->agent_log_scroll, TRUE);
    gtk_box_append(GTK_BOX(box), app->agent_log_scroll);

    /* Auto-Scroll: GtkAdjustment "changed" feuert nachdem GTK upper
     * aktualisiert hat – der einzige zuverlässige Zeitpunkt zum Scrollen. */
    GtkAdjustment *log_adj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(app->agent_log_scroll));
    g_signal_connect(log_adj, "changed", G_CALLBACK(on_adj_changed), app);

    add_bubble(app, "system", "KI-Agent bereit.");

    /* Tool-Transparenz-Zeile. */
    app->agent_activity = gtk_label_new("");
    gtk_widget_add_css_class(app->agent_activity, "hc-agent-activity");
    gtk_label_set_xalign(GTK_LABEL(app->agent_activity), 0.0);
    gtk_widget_set_margin_start(app->agent_activity, 10);
    gtk_box_append(GTK_BOX(box), app->agent_activity);

    /* Autonom-Schalter. */
    app->agent_auto_toggle = gtk_check_button_new_with_label("Autonom arbeiten");
    gtk_widget_add_css_class(app->agent_auto_toggle, "hc-agent-auto");
    gtk_widget_set_margin_start(app->agent_auto_toggle, 8);
    gtk_widget_set_margin_end(app->agent_auto_toggle, 8);
    g_signal_connect(app->agent_auto_toggle, "toggled", G_CALLBACK(on_auto_toggled), app);
    gtk_box_append(GTK_BOX(box), app->agent_auto_toggle);

    /* Eingabe. */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(row, "hc-agent-input");
    gtk_widget_set_margin_start(row, 8); gtk_widget_set_margin_end(row, 8);
    gtk_widget_set_margin_top(row, 6); gtk_widget_set_margin_bottom(row, 8);
    app->agent_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->agent_entry), "Frage an den Agenten…");
    gtk_widget_set_hexpand(app->agent_entry, TRUE);
    g_signal_connect(app->agent_entry, "activate", G_CALLBACK(on_send), app);
    GtkWidget *send = hc_icon_button("agent", "Senden");
    gtk_widget_add_css_class(send, "suggested-action");
    g_signal_connect(send, "clicked", G_CALLBACK(on_send), app);
    gtk_box_append(GTK_BOX(row), app->agent_entry);
    gtk_box_append(GTK_BOX(row), send);
    gtk_box_append(GTK_BOX(box), row);

    gtk_stack_add_named(GTK_STACK(app->agent_root_stack), box, "online");

    /* ---- OFFLINE: flacher Placeholder ---- */
    GtkWidget *off = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(off, "hc-agent-offline");
    gtk_widget_set_valign(off, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(off, GTK_ALIGN_CENTER);
    GtkWidget *oi = hc_icon("agent");
    gtk_image_set_pixel_size(GTK_IMAGE(oi), 48);
    gtk_widget_set_opacity(oi, 0.4);
    GtkWidget *ol = gtk_label_new("Agent Offline");
    gtk_widget_add_css_class(ol, "hc-offline-title");
    GtkWidget *oh = gtk_label_new("Keine KI konfiguriert.");
    gtk_widget_add_css_class(oh, "dim-label");
    GtkWidget *obtn = gtk_button_new_with_label("Einstellungen öffnen");
    gtk_widget_add_css_class(obtn, "suggested-action");
    g_signal_connect(obtn, "clicked", G_CALLBACK(on_open_settings_link), app);
    gtk_box_append(GTK_BOX(off), oi);
    gtk_box_append(GTK_BOX(off), ol);
    gtk_box_append(GTK_BOX(off), oh);
    gtk_box_append(GTK_BOX(off), obtn);
    gtk_stack_add_named(GTK_STACK(app->agent_root_stack), off, "offline");

    agent_refresh_availability(app);
    return app->agent_root_stack;
}

/* ============================================================
 * Auto-Crash-Hook: Stacktrace still an den Agenten geben.
 * ============================================================ */
void agent_report_crash(App *app, const char *stderr_text)
{
    if (!stderr_text || !*stderr_text) return;
    if (!agent_configured(app)) return;
    /* Rechte Sidebar auf Agent schalten, damit die Analyse sichtbar ist. */
    if (app->right_stack)
        gtk_stack_set_visible_child_name(GTK_STACK(app->right_stack), "agent");
    char *msg = g_strdup_printf(
        "Die Sandbox-Anwendung ist abgestürzt. Analysiere den folgenden "
        "stderr-Stacktrace, nenne die wahrscheinliche Ursache und schlage "
        "einen konkreten Fix vor:\n\n%s", stderr_text);
    add_bubble(app, "system", "⚠ Absturz erkannt – Stacktrace zur Analyse übergeben.");
    agent_send_user_message(app, msg);
    g_free(msg);
}
