#include "app.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>

/* ============================================================
 * Werkzeuge des KI-Agenten (Tool-Calling-Bridge).
 * Alle Pfade sind strikt auf den Projekt-Root beschränkt.
 * ============================================================ */

/* Pfad relativ zum Projekt auflösen und auf Ausbruch (../) prüfen. */
static char *resolve_in_project(App *app, const char *rel, char **err)
{
    if (!app->proj_root) { *err = g_strdup("Kein Projekt geöffnet."); return NULL; }
    if (!rel || !*rel) rel = ".";
    char *full = g_path_is_absolute(rel)
        ? g_strdup(rel)
        : g_build_filename(app->proj_root, rel, NULL);
    char *canon = g_canonicalize_filename(full, NULL);
    g_free(full);
    char *root_canon = g_canonicalize_filename(app->proj_root, NULL);
    gboolean inside = g_str_has_prefix(canon, root_canon);
    g_free(root_canon);
    if (!inside) {
        *err = g_strdup_printf("Pfad liegt außerhalb des Projekts: %s", rel);
        g_free(canon);
        return NULL;
    }
    return canon;
}

/* Offenen Tab nach Schreibvorgang neu laden. */
static void reload_if_open(App *app, const char *full)
{
    for (GList *l = app->tabs; l; l = l->next) {
        EditorTab *t = l->data;
        if (t->path && !strcmp(t->path, full)) {
            char *nc = NULL;
            if (g_file_get_contents(full, &nc, NULL, NULL)) {
                gtk_text_buffer_set_text(t->buffer, nc, -1);
                t->modified = FALSE;
                g_free(nc);
            }
        }
    }
}

/* ---- JSON-Arg-Helfer ---- */
static char *arg_str(JsonObject *o, const char *key)
{
    if (o && json_object_has_member(o, key)) {
        JsonNode *n = json_object_get_member(o, key);
        if (JSON_NODE_HOLDS_VALUE(n))
            return g_strdup(json_object_get_string_member(o, key));
    }
    return NULL;
}

/* ---- 1) list_directory ---- */
static char *tool_list_directory(App *app, JsonObject *args, gboolean *ok)
{
    char *err = NULL;
    char *rel = arg_str(args, "path");
    char *dir = resolve_in_project(app, rel ? rel : ".", &err);
    g_free(rel);
    if (!dir) { *ok = FALSE; return err; }

    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) { *ok = FALSE; char *e = g_strdup_printf("Kein Verzeichnis: %s", dir); g_free(dir); return e; }

    GString *s = g_string_new("");
    const char *root = app->proj_root;
    char *shown = g_str_has_prefix(dir, root) ? dir + strlen(root) : dir;
    g_string_append_printf(s, "Inhalt von '%s':\n", (*shown ? shown : "/"));
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    const char *n;
    while ((n = g_dir_read_name(d))) {
        if (n[0] == '.') continue;
        char *full = g_build_filename(dir, n, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) g_ptr_array_add(dirs, g_strdup(n));
        else g_ptr_array_add(files, g_strdup(n));
        g_free(full);
    }
    g_dir_close(d);
    g_ptr_array_sort_values(dirs, (GCompareFunc)g_ascii_strcasecmp);
    g_ptr_array_sort_values(files, (GCompareFunc)g_ascii_strcasecmp);
    for (guint i = 0; i < dirs->len; i++)  g_string_append_printf(s, "  [DIR]  %s/\n", (char*)dirs->pdata[i]);
    for (guint i = 0; i < files->len; i++) g_string_append_printf(s, "         %s\n", (char*)files->pdata[i]);
    if (dirs->len == 0 && files->len == 0) g_string_append(s, "  (leer)\n");
    g_ptr_array_free(dirs, TRUE);
    g_ptr_array_free(files, TRUE);
    g_free(dir);
    *ok = TRUE;
    return g_string_free(s, FALSE);
}

/* ---- 2) read_file (mit optionalem Zeilenbereich) ---- */
static char *tool_read_file(App *app, JsonObject *args, gboolean *ok)
{
    char *err = NULL;
    char *rel = arg_str(args, "filepath");
    if (!rel) rel = arg_str(args, "path");
    if (!rel) { *ok = FALSE; return g_strdup("Argument 'filepath' fehlt."); }
    char *full = resolve_in_project(app, rel, &err);
    g_free(rel);
    if (!full) { *ok = FALSE; return err; }

    /* Optionaler Zeilenbereich: start_line / end_line (1-basiert). */
    int start_line = 0, end_line = 0;
    if (args) {
        if (json_object_has_member(args, "start_line"))
            start_line = (int)json_object_get_int_member(args, "start_line");
        if (json_object_has_member(args, "end_line"))
            end_line = (int)json_object_get_int_member(args, "end_line");
    }

    char *content = NULL; gsize len = 0;
    if (!g_file_get_contents(full, &content, &len, NULL)) {
        *ok = FALSE; char *e = g_strdup_printf("Datei nicht lesbar: %s", full); g_free(full); return e;
    }
    g_free(full);

    /* Kein Zeilenbereich → ganzen Inhalt liefern (mit Kappung). */
    if (start_line <= 0 && end_line <= 0) {
        const gsize CAP = 20000;
        char *out;
        if (len > CAP) {
            char *head = g_strndup(content, CAP);
            out = g_strdup_printf("%s\n\n...(gekürzt, %zu Bytes gesamt)", head, (size_t)len);
            g_free(head);
        } else {
            out = g_strdup(content);
        }
        g_free(content);
        *ok = TRUE;
        return out;
    }

    /* Zeilenbereich: Zeilen extrahieren, mit Nummern prefix. */
    char **lines = g_strsplit(content, "\n", -1);
    g_free(content);
    int total = (int)g_strv_length(lines);
    if (start_line < 1) start_line = 1;
    if (end_line <= 0 || end_line > total) end_line = total;
    if (start_line > total) start_line = total;

    GString *s = g_string_new("");
    g_string_append_printf(s, "(Zeilen %d–%d von %d)\n", start_line, end_line, total);
    for (int i = start_line - 1; i < end_line && lines[i]; i++)
        g_string_append_printf(s, "%4d  %s\n", i + 1, lines[i]);
    g_strfreev(lines);
    *ok = TRUE;
    return g_string_free(s, FALSE);
}

/* ---- 3) search_project ---- */
static char *tool_search_project(App *app, JsonObject *args, gboolean *ok)
{
    if (!app->proj_root) { *ok = FALSE; return g_strdup("Kein Projekt geöffnet."); }
    char *kw = arg_str(args, "keyword");
    if (!kw) kw = arg_str(args, "query");
    if (!kw || !*kw) { g_free(kw); *ok = FALSE; return g_strdup("Argument 'keyword' fehlt."); }

    char *argv[] = { "grep", "-rnI", "--exclude-dir=.git", "--exclude-dir=bin",
                     "-e", kw, app->proj_root, NULL };
    char *out = NULL, *errout = NULL; int status = 0;
    GError *e = NULL;
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                 &out, &errout, &status, &e);
    g_free(kw); g_free(errout);
    if (e) { char *m = g_strdup_printf("Suche fehlgeschlagen: %s", e->message); g_clear_error(&e); *ok = FALSE; return m; }

    /* Projekt-Root aus den Pfaden kürzen + begrenzen. */
    GString *s = g_string_new("");
    if (!out || !*out) {
        g_string_append(s, "Keine Treffer.");
    } else {
        char **lines = g_strsplit(out, "\n", -1);
        int c = 0;
        for (int i = 0; lines[i] && c < 60; i++) {
            if (!*lines[i]) continue;
            const char *p = lines[i];
            if (g_str_has_prefix(p, app->proj_root)) p += strlen(app->proj_root);
            while (*p == '/') p++;
            g_string_append_printf(s, "%s\n", p);
            c++;
        }
        g_strfreev(lines);
    }
    g_free(out);
    *ok = TRUE;
    return g_string_free(s, FALSE);
}

/* ---- 4) write_file (nur neue Dateien) ---- */
static char *tool_write_file(App *app, JsonObject *args, gboolean *ok)
{
    char *err = NULL;
    char *rel = arg_str(args, "filepath");
    char *content = arg_str(args, "content");
    if (!rel) { g_free(content); *ok = FALSE; return g_strdup("Argument 'filepath' fehlt."); }
    char *full = resolve_in_project(app, rel, &err);
    if (!full) { g_free(rel); g_free(content); *ok = FALSE; return err; }

    if (g_file_test(full, G_FILE_TEST_EXISTS)) {
        *ok = FALSE;
        char *e = g_strdup_printf(
            "Datei existiert bereits: %s. write_file ist nur fürs Anlegen NEUER "
            "Dateien gedacht – nutze replace_in_file für Änderungen.", rel);
        g_free(rel); g_free(full); g_free(content);
        return e;
    }
    char *dir = g_path_get_dirname(full);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    gboolean w = g_file_set_contents(full, content ? content : "", -1, NULL);
    char *result;
    if (w) {
        /* Datei zur Whitelist hinzufügen – sonst blendet der Tresor-Filter
         * sie im Projektbaum aus. project_whitelist_add ruft intern
         * project_build_tree auf, ein separates refresh_tree entfällt. */
        project_whitelist_add(app, rel);

        /* RAG-Index für die neue Datei aufbauen. */
        hc_set_activity(app, "RAG: indexiere …");
        rag_update_file(app, rel);

        /* Git-Status aktualisieren. */
        git_update_header(app);

        *ok = TRUE;
        result = g_strdup_printf("Neue Datei angelegt: %s", rel);
    } else {
        *ok = FALSE;
        result = g_strdup_printf("Konnte Datei nicht schreiben: %s", rel);
    }
    g_free(rel); g_free(full); g_free(content);
    return result;
}

/* ---- 5) replace_in_file (strikt, 100%-Match) ---- */
static char *tool_replace_in_file(App *app, JsonObject *args, gboolean *ok)
{
    char *err = NULL;
    char *rel = arg_str(args, "filepath");
    char *search = arg_str(args, "search_text");
    char *replace = arg_str(args, "replace_text");
    if (!rel || !search) {
        g_free(rel); g_free(search); g_free(replace);
        *ok = FALSE; return g_strdup("Argumente 'filepath' und 'search_text' erforderlich.");
    }
    char *full = resolve_in_project(app, rel, &err);
    if (!full) { g_free(rel); g_free(search); g_free(replace); *ok = FALSE; return err; }

    char *content = NULL; gsize len = 0;
    if (!g_file_get_contents(full, &content, &len, NULL)) {
        *ok = FALSE; char *e = g_strdup_printf("Datei nicht lesbar: %s", rel);
        g_free(rel); g_free(search); g_free(replace); g_free(full); return e;
    }

    /* Strikt: Suchblock muss exakt (100%) vorkommen. */
    char **parts = g_strsplit(content, search, -1);
    int occ = g_strv_length(parts) - 1;
    if (occ < 1) {
        g_strfreev(parts); g_free(content);
        *ok = FALSE;
        char *e = g_strdup_printf(
            "Abbruch: Suchblock kommt NICHT exakt in %s vor (kein 100%%-Match). "
            "Keine Änderung vorgenommen.", rel);
        g_free(rel); g_free(search); g_free(replace); g_free(full);
        return e;
    }
    char *newc = g_strjoinv(replace ? replace : "", parts);
    g_strfreev(parts);
    gboolean w = g_file_set_contents(full, newc, -1, NULL);
    char *result;
    if (w) {
        reload_if_open(app, full);

        /* RAG-Delta-Update: geänderte Datei neu einbetten. */
        hc_set_activity(app, "RAG: aktualisiere …");
        rag_update_file(app, rel);

        /* Git-Kopf aktualisieren (neue Änderungen). */
        git_update_header(app);

        *ok = TRUE;
        result = g_strdup_printf("%d Vorkommen in %s ersetzt.", occ, rel);
    } else {
        *ok = FALSE;
        result = g_strdup_printf("Schreibfehler bei %s.", rel);
    }
    g_free(newc); g_free(content);
    g_free(rel); g_free(search); g_free(replace); g_free(full);
    return result;
}

/* ---- Dispatcher ---- */
/* ============================================================
 * project_status: der Agent soll die laufende Umgebung kennen, ohne
 * danach fragen zu muessen (Ports, Dienste, DB-Zugang, offene Fehler).
 * ============================================================ */
static char *tool_project_status(App *app, JsonObject *args, gboolean *ok)
{
    (void)args;
    if (!app->proj_root) { *ok = FALSE; return g_strdup("Kein Projekt geöffnet."); }

    GString *s = g_string_new("");
    g_string_append_printf(s, "Projekt: %s\n", app->proj_name ? app->proj_name : "?");
    g_string_append_printf(s, "Pfad: %s\n", app->proj_root);
    g_string_append_printf(s, "Sprache: %s\n", app->proj_lang ? app->proj_lang : "?");
    g_string_append_printf(s, "Template: %s\n",
                           app->proj_template ? app->proj_template : "?");
    g_string_append_printf(s, "Umgebung: %s\n", sandbox_backend_label(app));
    g_string_append_printf(s, "Build-Kommando: %s\n",
                           app->proj_build ? app->proj_build : "(keines)");
    g_string_append_printf(s, "Run-Kommando: %s\n",
                           app->proj_run ? app->proj_run : "(keines)");
    g_string_append_printf(s, "Projekttyp: %s%s\n",
        services_project_is_web(app) ? "Web " : "",
        services_project_is_native_gui(app) ? "native-GUI" : "");

    g_string_append(s, "\nDienste:\n");
    for (int i = 0; i < HC_SVC_KIND_COUNT; i++) {
        HcService *sv = services_get(app, (HcServiceKind)i);
        g_string_append_printf(s, "  %-22s %-16s %s\n", sv->label,
            services_state_label(sv->state), sv->detail ? sv->detail : "");
    }

    char *url = services_web_base_url(app);
    if (url) {
        g_string_append_printf(s, "\nWeb-Adresse: %s\n", url);
        g_free(url);
    }
    HcService *db = services_get(app, HC_SVC_DB);
    if (db->state == HC_SVC_RUNNING) {
        g_string_append_printf(s,
            "Datenbank: host=127.0.0.1 port=%d user=root passwort=(leer)\n",
            app->db_port);
    }
    HcService *ml = services_get(app, HC_SVC_MAIL);
    if (ml->state == HC_SVC_RUNNING) {
        g_string_append_printf(s,
            "Mail-Catcher: SMTP 127.0.0.1:%d, %u Mail(s) abgefangen\n",
            app->mail_port, g_list_length(app->mails));
    }

    if (app->problems) {
        g_string_append(s, "\nOffene Fehler:\n");
        for (GList *l = app->problems; l; l = l->next) {
            HcProblem *pr = l->data;
            g_string_append_printf(s, "  %s:%d:%d: %s\n",
                pr->file ? pr->file : "?", pr->line, pr->col,
                pr->msg ? pr->msg : "");
        }
    } else {
        g_string_append(s, "\nKeine offenen Fehler.\n");
    }

    *ok = TRUE;
    return g_string_free(s, FALSE);
}

/* ---- 7) run_command ---- */
/* Führt einen beliebigen Shell-Befehl in der Projekt-Sandbox aus.
 * Timeout: 30 s. stdout + stderr werden zusammengeführt zurückgegeben.
 * Sicherheit: der Befehl läuft über sandbox_exec_argv – also im selben
 * Backend (local/chroot/docker) wie Build und Run. Kein Ausbruch über
 * cd ../../ o.ä., weil die Sandbox das CWD auf den Projektordner setzt. */
static char *tool_run_command(App *app, JsonObject *args, gboolean *ok)
{
    if (!app->proj_root) { *ok = FALSE; return g_strdup("Kein Projekt geöffnet."); }
    char *cmd = arg_str(args, "command");
    if (!cmd || !*cmd) { g_free(cmd); *ok = FALSE; return g_strdup("Argument 'command' fehlt."); }

    char **argv = sandbox_exec_argv(app, cmd);
    g_free(cmd);

    char *out = NULL, *err_out = NULL;
    int status = 0;
    GError *e = NULL;
    gboolean ran = g_spawn_sync(
        app->proj_root,          /* CWD = Projektordner                    */
        argv, NULL,
        G_SPAWN_SEARCH_PATH,
        NULL, NULL,
        &out, &err_out,
        &status, &e);
    g_strfreev(argv);

    if (!ran) {
        char *m = g_strdup_printf("Befehl konnte nicht gestartet werden: %s",
                                   e ? e->message : "?");
        g_clear_error(&e); *ok = FALSE; return m;
    }

    /* stdout + stderr zusammenführen, auf 8000 Zeichen kappen. */
    GString *s = g_string_new("");
    if (out    && *out)     g_string_append(s, out);
    if (err_out && *err_out) g_string_append(s, err_out);
    g_free(out); g_free(err_out);

    if (s->len == 0) g_string_append(s, "(keine Ausgabe)");

    /* Exit-Code anhängen damit der Agent Erfolg/Fehler erkennt. */
    int code = g_spawn_check_wait_status(status, NULL) ? 0 : status;
    g_string_append_printf(s, "\n[exit %d]", code);

    /* Kappen */
    const gsize CAP = 8000;
    if (s->len > CAP) {
        g_string_truncate(s, CAP);
        g_string_append(s, "\n...(Ausgabe gekürzt)");
    }

    *ok = (code == 0);
    return g_string_free(s, FALSE);
}

/* ---- 8) get_url ---- */
/* HTTP-GET auf eine URL – typischerweise der laufende Webserver des
 * Projekts. Gibt Status-Code + die ersten 6000 Zeichen des Body zurück.
 * Nützlich um zu prüfen ob eine Seite 200 liefert, Fehlermeldungen
 * enthält, oder ob Links/Formulare wie erwartet reagieren. */
static char *tool_get_url(App *app, JsonObject *args, gboolean *ok)
{
    char *url = arg_str(args, "url");
    if (!url || !*url) { g_free(url); *ok = FALSE; return g_strdup("Argument 'url' fehlt."); }

    /* Relative URLs gegen die Web-Basis-URL auflösen. */
    char *full_url;
    if (g_str_has_prefix(url, "http://") || g_str_has_prefix(url, "https://")) {
        full_url = g_strdup(url);
    } else {
        char *base = services_web_base_url(app);
        if (!base) {
            g_free(url);
            *ok = FALSE;
            return g_strdup("Kein Webserver läuft. Starte den Webserver zuerst (project_status).");
        }
        /* Führenden / sicherstellen. */
        full_url = g_strdup_printf("%s%s%s", base,
                                   (*url == '/') ? "" : "/", url);
        g_free(base);
    }
    g_free(url);

    /* curl benutzen – immer verfügbar, kein extra libsoup-Request der
     * den Agent-Session-Timeout beeinflusst. */
    char *argv[] = {
        "curl", "-s", "-S", "-L",
        "--max-time", "10",
        "--write-out", "\n[HTTP %{http_code}]",
        full_url, NULL
    };
    char *out = NULL, *err_out = NULL;
    int status = 0; GError *e = NULL;
    gboolean ran = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                NULL, NULL, &out, &err_out, &status, &e);
    g_free(full_url);

    if (!ran) {
        char *m = g_strdup_printf("curl fehlgeschlagen: %s", e ? e->message : "?");
        g_clear_error(&e); g_free(out); g_free(err_out); *ok = FALSE; return m;
    }

    GString *s = g_string_new("");
    if (out && *out) g_string_append(s, out);
    if (err_out && *err_out) { g_string_append(s, "\n[stderr] "); g_string_append(s, err_out); }
    g_free(out); g_free(err_out);

    const gsize CAP = 6000;
    if (s->len > CAP) {
        g_string_truncate(s, CAP);
        g_string_append(s, "\n...(Body gekürzt)");
    }
    if (s->len == 0) g_string_append(s, "(leere Antwort)");

    *ok = TRUE;
    return g_string_free(s, FALSE);
}

/* ---- 9) run_build ---- */
/* Löst einen Build aus und wartet auf das Ergebnis. Gibt Compiler-
 * Ausgabe + Fehleranzahl zurück. Nützlich wenn der Agent nach einer
 * Änderung sofort prüfen will ob der Code compiliert. */
static char *tool_run_build(App *app, JsonObject *args, gboolean *ok)
{
    (void)args;
    if (!app->proj_root) { *ok = FALSE; return g_strdup("Kein Projekt geöffnet."); }
    if (!app->proj_build) { *ok = FALSE; return g_strdup("Kein Build-Kommando konfiguriert."); }

    editor_save_all(app);

    char **argv = sandbox_exec_argv(app, app->proj_build);
    char *out = NULL, *err_out = NULL;
    int status = 0; GError *e = NULL;
    gboolean ran = g_spawn_sync(
        app->proj_root, argv, NULL,
        G_SPAWN_SEARCH_PATH,
        NULL, NULL, &out, &err_out, &status, &e);
    g_strfreev(argv);

    if (!ran) {
        char *m = g_strdup_printf("Build-Start fehlgeschlagen: %s", e ? e->message : "?");
        g_clear_error(&e); *ok = FALSE; return m;
    }

    GString *s = g_string_new("");
    if (out    && *out)     g_string_append(s, out);
    if (err_out && *err_out) g_string_append(s, err_out);
    g_free(out); g_free(err_out);

    int code = g_spawn_check_wait_status(status, NULL) ? 0 : status;
    g_string_append_printf(s, "\n[exit %d]", code);

    const gsize CAP = 8000;
    if (s->len > CAP) { g_string_truncate(s, CAP); g_string_append(s, "\n...(gekürzt)"); }
    if (s->len == 0) g_string_append(s, "(keine Ausgabe)");

    /* Probleme neu parsen und in die IDE-Fehlerleiste eintragen. */
    problems_clear(app);
    char **lines = g_strsplit(s->str, "\n", -1);
    GRegex *re = g_regex_new("^([^\\s:]+):(\\d+)(?::(\\d+))?:\\s*(.*)$", 0, 0, NULL);
    for (int i = 0; lines[i]; i++) {
        GMatchInfo *mi = NULL;
        if (g_regex_match(re, lines[i], 0, &mi)) {
            char *f  = g_match_info_fetch(mi, 1);
            char *ln = g_match_info_fetch(mi, 2);
            char *co = g_match_info_fetch(mi, 3);
            char *ms = g_match_info_fetch(mi, 4);
            if (f && ln) {
                char *abs = g_path_is_absolute(f)
                    ? g_strdup(f)
                    : g_build_filename(app->proj_root, f, NULL);
                problems_add(app, abs, atoi(ln), co ? atoi(co) : 0, ms ? ms : "");
                g_free(abs);
            }
            g_free(f); g_free(ln); g_free(co); g_free(ms);
        }
        g_match_info_free(mi);
    }
    g_strfreev(lines);
    g_regex_unref(re);
    problems_rebuild_bar(app);

    guint nerr = g_list_length(app->problems);
    g_string_append_printf(s, "\n\nBuild: %s (%u Fehler)",
                           nerr == 0 ? "ERFOLGREICH" : "FEHLGESCHLAGEN", nerr);

    *ok = (code == 0);
    return g_string_free(s, FALSE);
}

char *agent_tools_dispatch(App *app, const char *name,
                           const char *args_json, gboolean *ok)
{
    *ok = FALSE;
    JsonObject *args = NULL;
    JsonParser *p = json_parser_new();
    if (args_json && *args_json &&
        json_parser_load_from_data(p, args_json, -1, NULL)) {
        JsonNode *r = json_parser_get_root(p);
        if (r && JSON_NODE_HOLDS_OBJECT(r)) args = json_node_get_object(r);
    }

    char *result;
    if      (!g_strcmp0(name, "list_directory"))  result = tool_list_directory(app, args, ok);
    else if (!g_strcmp0(name, "read_file"))       result = tool_read_file(app, args, ok);
    else if (!g_strcmp0(name, "search_project"))  result = tool_search_project(app, args, ok);
    else if (!g_strcmp0(name, "write_file"))      result = tool_write_file(app, args, ok);
    else if (!g_strcmp0(name, "replace_in_file")) result = tool_replace_in_file(app, args, ok);
    else if (!g_strcmp0(name, "project_status"))  result = tool_project_status(app, args, ok);
    else if (!g_strcmp0(name, "run_command"))      result = tool_run_command(app, args, ok);
    else if (!g_strcmp0(name, "get_url"))          result = tool_get_url(app, args, ok);
    else if (!g_strcmp0(name, "run_build"))        result = tool_run_build(app, args, ok);
    else {
        /* Custom-Werkzeug? */
        HcCustomTool *ct = custom_tool_find(app, name);
        if (ct) {
            result = custom_tool_run(app, ct, args_json, ok);
        } else {
            result = g_strdup_printf("Unbekanntes Werkzeug: %s", name ? name : "?");
        }
    }

    g_object_unref(p);
    return result;
}
