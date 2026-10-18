#include "app.h"
#include "version.h"
#include <gtksourceview/gtksource.h>
#include <string.h>

/* in preview.c implementiert */
extern GtkWidget *preview_build_for(EditorTab *t);
extern void       preview_reload(EditorTab *t);

/* ============================================================
 * Editor-Komponente: GtkSourceView in Tabs.
 * - Zeilennummern, Syntax-Highlighting, aktuelle Zeile
 * - Speichern (Strg+S) mit stillem Syntax-Check im Hintergrund
 * - Fehler-Markierungen als rote Wellenlinien (PANGO_UNDERLINE_ERROR)
 * - Outline-Ansicht (Klassen/Funktionen)
 * - Split-Tab Live-Preview
 * ============================================================ */

/* ----- Sprache / GtkSourceLanguage-ID anhand Dateiendung ----- */
const char *hc_lang_for_path(const char *path)
{
    if (!path) return NULL;
    const char *dot = strrchr(path, '.');
    if (!dot) return NULL;
    if (!g_ascii_strcasecmp(dot, ".c"))   return "c";
    if (!g_ascii_strcasecmp(dot, ".h"))   return "c";
    if (!g_ascii_strcasecmp(dot, ".cpp") || !g_ascii_strcasecmp(dot, ".cc") ||
        !g_ascii_strcasecmp(dot, ".cxx") || !g_ascii_strcasecmp(dot, ".hpp")) return "cpp";
    if (!g_ascii_strcasecmp(dot, ".py"))  return "python3";
    if (!g_ascii_strcasecmp(dot, ".sh"))  return "sh";
    if (!g_ascii_strcasecmp(dot, ".php")) return "php";
    if (!g_ascii_strcasecmp(dot, ".ui") || !g_ascii_strcasecmp(dot, ".xml")) return "xml";
    if (!g_ascii_strcasecmp(dot, ".html") || !g_ascii_strcasecmp(dot, ".htm")) return "html";
    if (!g_ascii_strcasecmp(dot, ".css")) return "css";
    if (!g_ascii_strcasecmp(dot, ".js"))  return "js";
    if (!g_ascii_strcasecmp(dot, ".json")) return "json";
    if (!g_ascii_strcasecmp(dot, ".md"))  return "markdown";
    return NULL;
}

static const char *scheme_for_theme(App *app)
{
    gboolean dark;
    switch (app->settings.theme) {
        case HC_THEME_LIGHT: dark = FALSE; break;
        case HC_THEME_DARK:  dark = TRUE;  break;
        default: {  /* System: GTK-Vorgabe abfragen. */
            gboolean prefer_dark = TRUE;
            GtkSettings *gs = gtk_settings_get_default();
            if (gs) g_object_get(gs, "gtk-application-prefer-dark-theme", &prefer_dark, NULL);
            dark = prefer_dark;
        }
    }
    return dark ? "Adwaita-dark" : "Adwaita";
}

/* ------------------------------------------------------------ */

EditorTab *editor_current(App *app)
{
    int p = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    if (p < 0) return NULL;
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->notebook), p);
    for (GList *l = app->tabs; l; l = l->next) {
        EditorTab *t = l->data;
        if (t->page == page) return t;
    }
    return NULL;
}

static void update_tab_title(EditorTab *t)
{
    const char *base = t->path ? strrchr(t->path, '/') : NULL;
    base = base ? base + 1 : (t->path ? t->path : "Unbenannt");
    char *txt = g_strdup_printf("%s%s", t->modified ? "● " : "", base);
    gtk_label_set_text(GTK_LABEL(t->tab_title), txt);
    g_free(txt);
}

static void show_notebook(App *app)
{
    gtk_stack_set_visible_child_name(GTK_STACK(app->notebook_stack),
        app->tabs ? "notebook" : "placeholder");
}

/* ---- Fehler-Wellenlinien-Tag ---- */
static GtkTextTag *error_tag(GtkTextBuffer *b)
{
    GtkTextTagTable *tt = gtk_text_buffer_get_tag_table(b);
    GtkTextTag *tag = gtk_text_tag_table_lookup(tt, "hc-error");
    if (!tag) {
        tag = gtk_text_buffer_create_tag(b, "hc-error",
            "underline", PANGO_UNDERLINE_ERROR, NULL);
    }
    return tag;
}

static void clear_error_marks(EditorTab *t)
{
    GtkTextIter a, z;
    gtk_text_buffer_get_bounds(t->buffer, &a, &z);
    gtk_text_buffer_remove_tag(t->buffer, error_tag(t->buffer), &a, &z);
}

static void mark_error_line(EditorTab *t, int line)
{
    if (line < 1) return;
    GtkTextIter a, z;
    gtk_text_buffer_get_iter_at_line(t->buffer, &a, line - 1);
    z = a;
    if (!gtk_text_iter_ends_line(&z))
        gtk_text_iter_forward_to_line_end(&z);
    /* führende Leerzeichen überspringen für saubere Markierung */
    while (!gtk_text_iter_is_end(&a) &&
           (gtk_text_iter_get_char(&a) == ' ' || gtk_text_iter_get_char(&a) == '\t'))
        gtk_text_iter_forward_char(&a);
    gtk_text_buffer_apply_tag(t->buffer, error_tag(t->buffer), &a, &z);
}

/* ============================================================
 * Stiller Syntax-Check beim Speichern
 * ============================================================ */
typedef struct { App *app; char *path; } LintJob;

/* Parst gcc/php/python/bash Fehlermeldungen: "file:line[:col]: msg". */
static void parse_lint_output(App *app, const char *path, const char *out)
{
    EditorTab *t = NULL;
    for (GList *l = app->tabs; l; l = l->next) {
        EditorTab *e = l->data;
        if (e->path && !strcmp(e->path, path)) { t = e; break; }
    }
    /* Alte Wellenlinien weg: sonst bleiben behobene Fehler markiert. */
    if (t) clear_error_marks(t);
    char **lines = g_strsplit(out, "\n", -1);
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    GRegex *re = g_regex_new("(?:^|\\s)([^\\s:]+):(\\d+)(?::(\\d+))?:?\\s*(.*)$",
                             0, 0, NULL);
    for (int i = 0; lines[i]; i++) {
        if (!*lines[i]) continue;
        GMatchInfo *mi = NULL;
        if (g_regex_match(re, lines[i], 0, &mi)) {
            char *f = g_match_info_fetch(mi, 1);
            char *ln = g_match_info_fetch(mi, 2);
            char *co = g_match_info_fetch(mi, 3);
            char *ms = g_match_info_fetch(mi, 4);
            /* Nur Treffer, die zur gespeicherten Datei gehören. */
            if (f && (strstr(f, base) || g_str_has_suffix(path, f))) {
                int line = atoi(ln ? ln : "0");
                int col  = co ? atoi(co) : 0;
                problems_add(app, path, line, col, ms ? ms : "");
                if (t) mark_error_line(t, line);
            }
            g_free(f); g_free(ln); g_free(co); g_free(ms);
        }
        g_match_info_free(mi);
    }
    g_regex_unref(re);
    g_strfreev(lines);
    problems_rebuild_bar(app);
}

static void lint_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    LintJob *j = ud;
    GError *e = NULL;
    GBytes *stdout_buf = NULL, *stderr_buf = NULL;
    g_subprocess_communicate_finish(G_SUBPROCESS(src), res, &stdout_buf, &stderr_buf, &e);
    if (!e) {
        gsize n = 0;
        const char *so = stdout_buf ? g_bytes_get_data(stdout_buf, &n) : "";
        const char *se = stderr_buf ? g_bytes_get_data(stderr_buf, &n) : "";
        char *all = g_strconcat(so ? so : "", "\n", se ? se : "", NULL);
        parse_lint_output(j->app, j->path, all);
        g_free(all);
    } else {
        g_clear_error(&e);
    }
    if (stdout_buf) g_bytes_unref(stdout_buf);
    if (stderr_buf) g_bytes_unref(stderr_buf);
    g_object_unref(src);
    g_free(j->path);
    g_free(j);
}

/* Prueferkommando fuer eine Sprache. check_path ist die Datei, die
 * tatsaechlich geprueft wird (beim Live-Check eine Pufferkopie). */
static char *syntax_check_cmd(const char *lang, const char *check_path)
{
    char *q = g_shell_quote(check_path);
    char *cmd = NULL;
    if (!strcmp(lang, "c"))            cmd = g_strdup_printf("gcc -fsyntax-only -w %s", q);
    else if (!strcmp(lang, "cpp"))     cmd = g_strdup_printf("g++ -fsyntax-only -w %s", q);
    else if (!strcmp(lang, "python3")) cmd = g_strdup_printf("python3 -m py_compile %s", q);
    else if (!strcmp(lang, "sh"))      cmd = g_strdup_printf("bash -n %s", q);
    else if (!strcmp(lang, "php"))     cmd = g_strdup_printf("php -l %s", q);
    g_free(q);
    return cmd;
}

/* Gemeinsamer Kern: prueft check_path, meldet Fehler aber unter
 * report_path (der echten Datei im Editor). */
static void run_check_on(App *app, const char *report_path,
                         const char *check_path)
{
    const char *lang = hc_lang_for_path(report_path);
    if (!lang) return;
    char *cmd = syntax_check_cmd(lang, check_path);
    if (!cmd) return;

    /* Die Pruefung laeuft in derselben Sandbox wie der Build – sonst
     * meldet der Editor Fehler, die der Compiler nie sieht (und umgekehrt). */
    char **argv = sandbox_exec_argv(app, cmd);
    g_free(cmd);

    /* Nur die Probleme DIESER Datei verwerfen, nicht die ganze Liste. */
    problems_clear_file(app, report_path);

    GError *e = NULL;
    GSubprocess *proc = g_subprocess_newv((const char * const *)argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &e);
    g_strfreev(argv);
    if (!proc) { g_clear_error(&e); return; }

    LintJob *j = g_new0(LintJob, 1);
    j->app = app;
    j->path = g_strdup(report_path);
    g_subprocess_communicate_async(proc, NULL, NULL, lint_done, j);
}

/* Pruefung der gespeicherten Datei. */
static void run_syntax_check(App *app, const char *path)
{
    run_check_on(app, path, path);
}

/* Live-Pruefung: schreibt den Puffer in eine Schattendatei und prueft
 * diese. So werden Fehler markiert, WAEHREND getippt wird – ohne dass
 * die echte Datei dafuer gespeichert werden muss. */
void editor_live_check(EditorTab *t)
{
    App *app = t->app;
    if (!t->path || !app->proj_root) return;
    const char *lang = hc_lang_for_path(t->path);
    if (!lang) return;
    if (!syntax_check_cmd(lang, "x")) return;   /* Sprache nicht pruefbar */

    char *dir = g_build_filename(app->proj_root, ".hypercore", "live", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *base = g_path_get_basename(t->path);
    char *shadow = g_build_filename(dir, base, NULL);
    g_free(dir); g_free(base);

    GtkTextIter a, z;
    gtk_text_buffer_get_bounds(t->buffer, &a, &z);
    char *text = gtk_text_buffer_get_text(t->buffer, &a, &z, FALSE);
    gboolean ok = g_file_set_contents(shadow, text, -1, NULL);
    g_free(text);
    if (!ok) { g_free(shadow); return; }

    /* Pfad der Schattendatei innerhalb der Sandbox. */
    char *base2 = g_path_get_basename(t->path);
    char *in_sb = g_build_filename(sandbox_project_path(app), ".hypercore",
                                   "live", base2, NULL);
    g_free(base2);
    run_check_on(app, t->path, in_sb);
    g_free(in_sb);
    g_free(shadow);
}

/* ============================================================
 * Speichern
 * ============================================================ */
void editor_save_tab(EditorTab *t)
{
    if (!t || !t->path) return;
    agent_cancel_ghost(t);  /* Ghost-Text vor dem Speichern entfernen   */

    GtkTextIter a, z;
    gtk_text_buffer_get_bounds(t->buffer, &a, &z);
    char *text = gtk_text_buffer_get_text(t->buffer, &a, &z, FALSE);
    GError *e = NULL;
    if (g_file_set_contents(t->path, text, -1, &e)) {
        t->modified = FALSE;
        update_tab_title(t);
        clear_error_marks(t);
        run_syntax_check(t->app, t->path);
        editor_update_status(t->app);
        if (t->preview) preview_reload(t);  /* Live-Preview aktualisieren */
        /* RAG Smart Delta-Update: nur diese Datei neu indizieren. */
        if (t->app->proj_root && g_str_has_prefix(t->path, t->app->proj_root)) {
            const char *rel = t->path + strlen(t->app->proj_root);
            while (*rel == '/') rel++;
            rag_update_file(t->app, rel);
        }
    } else {
        g_warning("[editor] Speichern fehlgeschlagen: %s", e->message);
        g_clear_error(&e);
    }
    g_free(text);
}

void editor_save_current(App *app) { editor_save_tab(editor_current(app)); }

/* Alle geänderten Tabs hart speichern (vor Build/Run). */
void editor_save_all(App *app)
{
    for (GList *l = app->tabs; l; l = l->next) {
        EditorTab *t = l->data;
        if (t->path && t->modified) editor_save_tab(t);
    }
}

/* ============================================================
 * Status / Outline
 * ============================================================ */
void editor_update_status(App *app)
{
    EditorTab *t = editor_current(app);
    if (!t) {
        gtk_label_set_text(GTK_LABEL(app->st_fileinfo), "—");
        return;
    }
    int lines = gtk_text_buffer_get_line_count(t->buffer);
    GtkTextIter a, z; gtk_text_buffer_get_bounds(t->buffer, &a, &z);
    int chars = gtk_text_iter_get_offset(&z);
    const char *base = t->path ? strrchr(t->path, '/') : NULL;
    base = base ? base + 1 : (t->path ? t->path : "Unbenannt");
    char *s = g_strdup_printf("%s   ·   %d Zeilen   ·   %d Zeichen",
                              base, lines, chars);
    gtk_label_set_text(GTK_LABEL(app->st_fileinfo), s);
    g_free(s);
}

typedef struct { App *app; int line; } OutlineItem;

static void on_outline_row(GtkListBox *box, GtkListBoxRow *row, gpointer ud)
{
    (void)box;
    App *app = ud;
    if (!row) return;
    OutlineItem *oi = g_object_get_data(G_OBJECT(row), "oi");
    EditorTab *t = editor_current(app);
    if (oi && t) editor_goto_line(t, oi->line);
}

void editor_connect_outline(App *app)
{
    g_signal_connect(app->outline_list, "row-activated",
                     G_CALLBACK(on_outline_row), app);
}

void editor_refresh_outline(App *app)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(app->outline_list)))
        gtk_list_box_remove(GTK_LIST_BOX(app->outline_list), child);

    EditorTab *t = editor_current(app);
    if (!t) return;
    const char *lang = hc_lang_for_path(t->path);
    if (!lang) return;

    GtkTextIter a, z; gtk_text_buffer_get_bounds(t->buffer, &a, &z);
    char *text = gtk_text_buffer_get_text(t->buffer, &a, &z, FALSE);
    char **lines = g_strsplit(text, "\n", -1);

    /* Pro Sprache ein Muster: Gruppe 1 = Art(optional), 2 = Name. */
    const char *pat = NULL;
    if (!strcmp(lang, "python3"))
        pat = "^\\s*(def|class)\\s+(\\w+)";
    else if (!strcmp(lang, "php"))
        pat = "^\\s*(?:public\\s+|private\\s+|protected\\s+|static\\s+)*(function|class)\\s+(\\w+)";
    else if (!strcmp(lang, "sh"))
        pat = "^\\s*(?:function\\s+)?(\\w+)\\s*\\(\\)";
    else /* c / cpp */
        pat = "^[A-Za-z_][\\w \\t\\*&]*?\\b(\\w+)\\s*\\([^;]*\\)\\s*\\{?\\s*$|^\\s*(?:struct|class|enum)\\s+(\\w+)";

    GRegex *re = g_regex_new(pat, 0, 0, NULL);
    int count = 0;
    for (int i = 0; lines[i]; i++) {
        GMatchInfo *mi = NULL;
        if (re && g_regex_match(re, lines[i], 0, &mi)) {
            /* Namen aus der ersten nicht-leeren Namensgruppe nehmen. */
            char *name = NULL; const char *kind = "";
            int ngroups = g_match_info_get_match_count(mi);
            if (!strcmp(lang, "python3") || !strcmp(lang, "php")) {
                kind = g_match_info_fetch(mi, 1);
                name = g_match_info_fetch(mi, 2);
            } else if (!strcmp(lang, "sh")) {
                name = g_match_info_fetch(mi, 1);
            } else {
                for (int g = 1; g < ngroups; g++) {
                    char *v = g_match_info_fetch(mi, g);
                    if (v && *v) { name = v; break; }
                    g_free(v);
                }
            }
            /* Kontroll-Schlüsselwörter aussortieren. */
            if (name && *name &&
                strcmp(name,"if") && strcmp(name,"for") && strcmp(name,"while") &&
                strcmp(name,"switch") && strcmp(name,"return") && strcmp(name,"else") &&
                strcmp(name,"sizeof") && strcmp(name,"do")) {
                GtkWidget *row = gtk_list_box_row_new();
                GtkWidget *b = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                gtk_widget_set_margin_start(b, 10); gtk_widget_set_margin_end(b, 10);
                gtk_widget_set_margin_top(b, 3); gtk_widget_set_margin_bottom(b, 3);
                GtkWidget *ic = gtk_label_new(
                    (kind && strstr(kind,"class")) ? "◆" :
                    (kind && strstr(kind,"struct")) ? "◇" : "ƒ");
                gtk_widget_add_css_class(ic, "hc-outline-icon");
                GtkWidget *nm = gtk_label_new(name);
                gtk_label_set_xalign(GTK_LABEL(nm), 0.0);
                gtk_widget_set_hexpand(nm, TRUE);
                gtk_box_append(GTK_BOX(b), ic);
                gtk_box_append(GTK_BOX(b), nm);
                gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), b);
                OutlineItem *oi = g_new0(OutlineItem, 1);
                oi->app = app; oi->line = i + 1;
                g_object_set_data_full(G_OBJECT(row), "oi", oi, g_free);
                gtk_list_box_append(GTK_LIST_BOX(app->outline_list), row);
                count++;
            }
            if (name) g_free(name);
        }
        g_match_info_free(mi);
    }
    if (re) g_regex_unref(re);
    if (count == 0) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *l = gtk_label_new("Keine Symbole gefunden");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_widget_set_margin_start(l, 10); gtk_widget_set_margin_top(l, 6);
        gtk_label_set_xalign(GTK_LABEL(l), 0.0);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), l);
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->outline_list), GTK_SELECTION_NONE);
        gtk_list_box_append(GTK_LIST_BOX(app->outline_list), row);
    } else {
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->outline_list), GTK_SELECTION_SINGLE);
    }
    g_strfreev(lines);
    g_free(text);
}

/* ============================================================
 * Navigation
 * ============================================================ */
void editor_goto_line(EditorTab *t, int line)
{
    if (!t || line < 1) return;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(t->buffer, &it, line - 1);
    gtk_text_buffer_place_cursor(t->buffer, &it);
    GtkTextMark *m = gtk_text_buffer_get_insert(t->buffer);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(t->view), m, 0.2, TRUE, 0.0, 0.3);
    gtk_widget_grab_focus(t->view);
}

void editor_jump_to(App *app, const char *file, int line)
{
    EditorTab *t = editor_open_file(app, file);
    if (t) editor_goto_line(t, line);
}

/* ============================================================
 * Einstellungen auf alle Views anwenden (Font, Tab, Scheme)
 * ============================================================ */
static GtkCssProvider *s_font_provider = NULL;

void editor_apply_settings_all(App *app)
{
    if (!s_font_provider) {
        s_font_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
            GTK_STYLE_PROVIDER(s_font_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    char *css = g_strdup_printf(
        ".hc-sourceview, .hc-sourceview text { font-family: \"%s\", monospace; font-size: %dpt; }",
        app->settings.font_family, app->settings.font_size);
    gtk_css_provider_load_from_string(s_font_provider, css);
    g_free(css);

    GtkSourceStyleSchemeManager *sm = gtk_source_style_scheme_manager_get_default();
    GtkSourceStyleScheme *scheme =
        gtk_source_style_scheme_manager_get_scheme(sm, scheme_for_theme(app));

    for (GList *l = app->tabs; l; l = l->next) {
        EditorTab *t = l->data;
        gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(t->view), app->settings.tab_width);
        gtk_source_view_set_insert_spaces_instead_of_tabs(
            GTK_SOURCE_VIEW(t->view), app->settings.spaces_for_tab);
        if (scheme && GTK_SOURCE_IS_BUFFER(t->buffer))
            gtk_source_buffer_set_style_scheme(GTK_SOURCE_BUFFER(t->buffer), scheme);
    }
}

/* ============================================================
 * Tab schließen
 * ============================================================ */
void editor_close_tab(EditorTab *t)
{
    if (!t) return;
    App *app = t->app;
    agent_cancel_ghost(t);
    if (t->ghost_timer) { g_source_remove(t->ghost_timer); t->ghost_timer = 0; }
    /* Live-Timer abbestellen, sonst feuern sie in einen freigegebenen Tab. */
    if (t->live_timer) { g_source_remove(t->live_timer); t->live_timer = 0; }
    if (t->save_timer) { g_source_remove(t->save_timer); t->save_timer = 0; }
    int p = gtk_notebook_page_num(GTK_NOTEBOOK(app->notebook), t->page);
    app->tabs = g_list_remove(app->tabs, t);
    if (p >= 0) gtk_notebook_remove_page(GTK_NOTEBOOK(app->notebook), p);
    g_free(t->path);
    g_free(t);
    show_notebook(app);
    editor_update_status(app);
    editor_refresh_outline(app);
}

/* ============================================================
 * Buffer-Änderungen
 * ============================================================ */
static guint s_outline_timer = 0;
static App  *s_outline_app = NULL;
static gboolean outline_timeout(gpointer ud) {
    (void)ud; s_outline_timer = 0;
    if (s_outline_app) editor_refresh_outline(s_outline_app);
    return G_SOURCE_REMOVE;
}

/* ---- Live-Betrieb: Vorschau + Fehlerpruefung + Auto-Save ----
 *
 * Tippen loest zwei entprellte Schritte aus:
 *   nach 250 ms  -> Vorschau neu zeichnen und Syntax live pruefen
 *                   (beides direkt aus dem Puffer, ohne Speichern)
 *   nach 900 ms  -> Auto-Save; damit wird die Datei auch fuer alles
 *                   aktuell, was sie vom Datentraeger liest (PHP-Server,
 *                   Build, RAG-Index).
 */
#define HC_LIVE_DELAY_MS 250
#define HC_SAVE_DELAY_MS 900

static void live_tick(gpointer ud)
{
    EditorTab *t = ud;
    t->live_timer = 0;
    preview_live_update(t);   /* .ui/HTML direkt aus dem Puffer */
    editor_live_check(t);     /* Wellenlinien live              */
}

static void save_tick(gpointer ud)
{
    EditorTab *t = ud;
    t->save_timer = 0;
    if (!t->app->settings.autosave_enabled) return;
    /* Zeitreise (Git) sperrt das Schreiben – dann nicht automatisch
     * speichern, sonst wird die Historie unbemerkt veraendert. */
    if (t->app->git_readonly) return;
    if (t->path && t->modified) editor_save_tab(t);
}

static void on_buffer_changed(GtkTextBuffer *b, gpointer ud)
{
    (void)b;
    EditorTab *t = ud;
    if (t->ghost_ignore_change) return;  /* eigene Ghost-Einfügung       */
    agent_cancel_ghost(t);               /* Tipp-Änderung verwirft Ghost */
    if (!t->modified) { t->modified = TRUE; update_tab_title(t); }
    editor_update_status(t->app);

    /* Outline entprellt aktualisieren. */
    s_outline_app = t->app;
    if (s_outline_timer) g_source_remove(s_outline_timer);
    s_outline_timer = g_timeout_add(500, outline_timeout, NULL);

    /* Live-Vorschau + Live-Pruefung (ohne Speichern). */
    if (t->live_timer) g_source_remove(t->live_timer);
    t->live_timer = g_timeout_add_once(HC_LIVE_DELAY_MS, live_tick, t);

    /* Auto-Save (nur wenn aktiviert und nicht schreibgesperrt). */
    if (t->save_timer) g_source_remove(t->save_timer);
    t->save_timer = g_timeout_add_once(HC_SAVE_DELAY_MS, save_tick, t);

    /* Ghost-Text neu anfragen (debounced in agent.c). */
    if (t->app->settings.ghost_enabled)
        agent_request_ghost(t);
}

static void on_tab_close_clicked(GtkButton *btn, gpointer ud)
{
    (void)btn;
    editor_close_tab((EditorTab *)ud);
}

/* ============================================================
 * Datei öffnen / neuen Tab anlegen
 * ============================================================ */
EditorTab *editor_open_file(App *app, const char *path)
{
    char *abspath = NULL;
    if (path) {
        if (g_path_is_absolute(path)) abspath = g_strdup(path);
        else abspath = g_build_filename(app->proj_root ? app->proj_root : ".", path, NULL);
    }

    /* Bereits offen? -> fokussieren. */
    if (abspath) {
        for (GList *l = app->tabs; l; l = l->next) {
            EditorTab *e = l->data;
            if (e->path && !strcmp(e->path, abspath)) {
                int p = gtk_notebook_page_num(GTK_NOTEBOOK(app->notebook), e->page);
                gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), p);
                g_free(abspath);
                return e;
            }
        }
    }

    EditorTab *t = g_new0(EditorTab, 1);
    t->app = app;
    t->path = abspath;

    GtkSourceBuffer *buf = gtk_source_buffer_new(NULL);
    t->buffer = GTK_TEXT_BUFFER(buf);

    /* Inhalt laden. */
    if (abspath) {
        char *content = NULL; gsize len = 0;
        if (g_file_get_contents(abspath, &content, &len, NULL)) {
            gtk_text_buffer_set_text(t->buffer, content, len);
            g_free(content);
        }
    }

    /* Sprache. */
    const char *langid = hc_lang_for_path(abspath);
    if (langid) {
        GtkSourceLanguageManager *lm = gtk_source_language_manager_get_default();
        GtkSourceLanguage *lang = gtk_source_language_manager_get_language(lm, langid);
        if (lang) gtk_source_buffer_set_language(buf, lang);
    }

    t->view = gtk_source_view_new_with_buffer(buf);
    gtk_widget_add_css_class(t->view, "hc-sourceview");
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(t->view), TRUE);
    gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(t->view), TRUE);
    gtk_source_view_set_auto_indent(GTK_SOURCE_VIEW(t->view), TRUE);
    gtk_source_view_set_show_line_marks(GTK_SOURCE_VIEW(t->view), TRUE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(t->view), TRUE);
    gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(t->view), app->settings.tab_width);
    gtk_source_view_set_insert_spaces_instead_of_tabs(
        GTK_SOURCE_VIEW(t->view), app->settings.spaces_for_tab);

    GtkSourceStyleSchemeManager *sm = gtk_source_style_scheme_manager_get_default();
    GtkSourceStyleScheme *scheme =
        gtk_source_style_scheme_manager_get_scheme(sm, scheme_for_theme(app));
    if (scheme) gtk_source_buffer_set_style_scheme(buf, scheme);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(scroller, TRUE);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), t->view);

    /* page: ein Paned, damit später Live-Preview rechts angedockt wird. */
    t->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(t->paned), scroller);
    gtk_paned_set_resize_start_child(GTK_PANED(t->paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(t->paned), FALSE);
    t->page = t->paned;

    /* Reiter-Widget: Titel + Schließen. */
    GtkWidget *tabbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(tabbox, "hc-tab");
    t->tab_title = gtk_label_new("");
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(close, "flat");
    gtk_widget_add_css_class(close, "hc-tab-close");
    gtk_button_set_has_frame(GTK_BUTTON(close), FALSE);
    g_signal_connect(close, "clicked", G_CALLBACK(on_tab_close_clicked), t);
    gtk_box_append(GTK_BOX(tabbox), t->tab_title);
    gtk_box_append(GTK_BOX(tabbox), close);

    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), t->page, tabbox);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(app->notebook), t->page, TRUE);
    app->tabs = g_list_append(app->tabs, t);
    update_tab_title(t);

    t->changed_handler =
        g_signal_connect(t->buffer, "changed", G_CALLBACK(on_buffer_changed), t);
    agent_attach_ghost(t);  /* Tab übernimmt Ghost-Text-Vorschlag          */

    show_notebook(app);
    int p = gtk_notebook_page_num(GTK_NOTEBOOK(app->notebook), t->page);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), p);

    editor_update_status(app);
    editor_refresh_outline(app);

    /* Live-Ansicht ist sofort da: Wer eine .ui- oder Web-Datei oeffnet,
     * soll die Vorschau sehen, ohne erst etwas "starten" zu muessen. */
    preview_auto_open(t);
    return t;
}

/* ============================================================
 * Live-Preview (Split-Tab) – Umschalten
 * Implementierung des Vorschau-Inhalts in preview.c
 * ============================================================ */
/* Teiler mittig setzen. Direkt nach dem Anlegen ist die Breite noch 0,
 * deshalb wird die Position nachgezogen, sobald das Widget eine hat –
 * sonst klappt die Vorschau auf Breite 0 zusammen (sah wie "keine
 * Vorschau" aus). */
static void place_split(gpointer ud)
{
    EditorTab *t = ud;
    if (!t->paned || !t->preview) return;
    int w = gtk_widget_get_width(t->paned);
    gtk_paned_set_position(GTK_PANED(t->paned), w > 200 ? w / 2 : 520);
}

void editor_set_preview(EditorTab *t, gboolean on)
{
    if (!t) return;
    if (!on) {
        if (t->preview) {
            gtk_paned_set_end_child(GTK_PANED(t->paned), NULL);
            t->preview = NULL;
        }
        return;
    }
    if (t->preview) return;
    GtkWidget *pv = preview_build_for(t);
    if (!pv) {
        terminal_feed_line(t->app,
            "[Preview] Für diesen Dateityp ist keine Vorschau verfügbar.");
        return;
    }
    t->preview = pv;
    gtk_paned_set_end_child(GTK_PANED(t->paned), pv);
    gtk_paned_set_resize_end_child(GTK_PANED(t->paned), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(t->paned), FALSE);
    place_split(t);
    g_idle_add_once(place_split, t);   /* nach dem ersten Layout nachziehen */
}

void editor_toggle_preview(App *app)
{
    EditorTab *t = editor_current(app);
    if (!t) return;
    editor_set_preview(t, t->preview == NULL);
}
