#include "app.h"
#include "version.h"
#include <vte/vte.h>
#include <gdk/gdkkeysyms.h>
#include <string.h>

/* ============================================================
 * Unteres Panel: HyperCore-Konsole + Build/Run + Smart-Errors.
 *
 * Dies ist KEINE offene Shell. Es gibt keinen Login-Prozess, kein PTY
 * und damit auch keine Bash-History des Benutzers: das VTE-Widget ist
 * eine reine Ausgabeflaeche, die Eingabe ist abgeschaltet.
 *
 * Bedient wird die Konsole ueber einen eigenen, festen Befehlssatz
 * (build, run, stop, services, restart <dienst>, db, mail, open, ...).
 * Es werden also nur Dinge ausgefuehrt, die die IDE selbst kennt und
 * die in der Sandbox landen – nicht beliebige Systembefehle.
 *
 * Fehler gehen auf Wunsch (Rechtsklick) oder automatisch an den
 * KI-Agenten zur Analyse.
 * ============================================================ */

/* Display-Ausgabe: \n -> \r\n, damit VTE korrekt umbricht. */
static void feed_display(App *app, const char *text)
{
    if (!app->term || !text) return;
    GString *s = g_string_new(NULL);
    for (const char *p = text; *p; p++) {
        if (*p == '\n') g_string_append(s, "\r\n");
        else g_string_append_c(s, *p);
    }
    vte_terminal_feed(VTE_TERMINAL(app->term), s->str, s->len);
    g_string_free(s, TRUE);
}

void terminal_feed_line(App *app, const char *text)
{
    feed_display(app, text);
    feed_display(app, "\n");
}

/* ============================================================
 * Problemleiste (Smart Errors)
 * ============================================================ */
static void problem_free(gpointer p)
{
    HcProblem *pr = p;
    g_free(pr->file); g_free(pr->msg); g_free(pr);
}

void problems_clear(App *app)
{
    g_list_free_full(app->problems, problem_free);
    app->problems = NULL;
}

/* Nur die Probleme einer Datei verwerfen (fuer die Live-Pruefung):
 * sonst loescht jede Pruefung die Fehler aller anderen Dateien mit. */
void problems_clear_file(App *app, const char *file)
{
    GList *keep = NULL;
    for (GList *l = app->problems; l; l = l->next) {
        HcProblem *e = l->data;
        if (e->file && file && !strcmp(e->file, file)) problem_free(e);
        else keep = g_list_append(keep, e);
    }
    g_list_free(app->problems);
    app->problems = keep;
}

void problems_add(App *app, const char *file, int line, int col, const char *msg)
{
    /* Duplikate vermeiden. */
    for (GList *l = app->problems; l; l = l->next) {
        HcProblem *e = l->data;
        if (e->line == line && e->file && file && !strcmp(e->file, file))
            return;
    }
    HcProblem *pr = g_new0(HcProblem, 1);
    pr->file = g_strdup(file);
    pr->line = line; pr->col = col;
    pr->msg = g_strdup(msg ? msg : "");
    app->problems = g_list_append(app->problems, pr);
}

typedef struct { App *app; char *file; int line; } Jump;
static void jump_free(gpointer p, GClosure *c) { (void)c; Jump *j = p; g_free(j->file); g_free(j); }
static void on_problem_clicked(GtkButton *b, gpointer ud)
{
    (void)b; Jump *j = ud;
    editor_jump_to(j->app, j->file, j->line);
}

void problems_rebuild_bar(App *app)
{
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(app->problems_bar)))
        gtk_box_remove(GTK_BOX(app->problems_bar), child);

    guint n = g_list_length(app->problems);
    if (n == 0) {
        gtk_widget_set_visible(app->problems_bar, FALSE);
        return;
    }
    gtk_widget_set_visible(app->problems_bar, TRUE);

    GtkWidget *lbl = gtk_label_new(NULL);
    char *head = g_strdup_printf("⚠ %u %s:", n, n == 1 ? "Fehler" : "Fehler");
    gtk_label_set_text(GTK_LABEL(lbl), head);
    g_free(head);
    gtk_widget_add_css_class(lbl, "hc-problems-head");
    gtk_box_append(GTK_BOX(app->problems_bar), lbl);

    int shown = 0;
    for (GList *l = app->problems; l && shown < 12; l = l->next, shown++) {
        HcProblem *pr = l->data;
        const char *base = strrchr(pr->file, '/');
        base = base ? base + 1 : pr->file;
        char *txt = g_strdup_printf("%s:%d", base, pr->line);
        GtkWidget *btn = gtk_button_new_with_label(txt);
        g_free(txt);
        gtk_widget_add_css_class(btn, "hc-problem-chip");
        gtk_widget_set_tooltip_text(btn, pr->msg);
        Jump *j = g_new0(Jump, 1);
        j->app = app; j->file = g_strdup(pr->file); j->line = pr->line;
        g_signal_connect_data(btn, "clicked", G_CALLBACK(on_problem_clicked),
                              j, jump_free, 0);
        gtk_box_append(GTK_BOX(app->problems_bar), btn);
    }
}

/* ============================================================
 * Build (asynchron, mit Smart-Error-Parsing)
 * ============================================================ */
typedef struct {
    App *app;
    GDataInputStream *dis;
    GRegex *re;
} BuildCtx;

/* Timeout-Callback zum Löschen des Aktivitäts-Status. */
static gboolean clear_activity_cb(gpointer ud)
{
    hc_set_activity((App *)ud, NULL);
    return G_SOURCE_REMOVE;
}

static void build_read_line(BuildCtx *ctx);

static void on_build_line(GObject *src, GAsyncResult *res, gpointer ud)
{
    BuildCtx *ctx = ud;
    gsize len = 0; GError *e = NULL;
    char *line = g_data_input_stream_read_line_finish(
        G_DATA_INPUT_STREAM(src), res, &len, &e);
    if (!line) {
        /* EOF oder Fehler -> fertig. */
        App *app = ctx->app;
        problems_rebuild_bar(app);
        if (app->problems) {
            guint nerr = g_list_length(app->problems);
            char *m = g_strdup_printf(
                "\x1b[31m✘ Build mit %u Fehler(n).\x1b[0m", nerr);
            terminal_feed_line(app, m);
            g_free(m);
            char abuf[48];
            snprintf(abuf, sizeof(abuf), "✘ Build: %u Fehler", nerr);
            hc_set_activity(app, abuf);
            /* Fehler nach 8 s automatisch aus der Leiste räumen. */
            g_timeout_add_seconds(8, clear_activity_cb, app);
            /* Fehler gehen ohne Zutun an den Agenten – genau dafuer ist
             * er da. Rechtsklick waere der Umweg. */
            terminal_send_errors_to_agent(app);
        } else {
            terminal_feed_line(app, "\x1b[90m✔ Build fertig.\x1b[0m");
            hc_set_activity(app, "✔ Build erfolgreich");
            g_timeout_add_seconds(4, clear_activity_cb, app);
        }
        g_clear_error(&e);
        g_object_unref(ctx->dis);
        g_regex_unref(ctx->re);
        g_free(ctx);
        return;
    }
    /* anzeigen */
    feed_display(ctx->app, line);
    feed_display(ctx->app, "\n");

    /* parsen: datei:zeile[:spalte]: meldung */
    GMatchInfo *mi = NULL;
    if (g_regex_match(ctx->re, line, 0, &mi)) {
        char *f = g_match_info_fetch(mi, 1);
        char *ln = g_match_info_fetch(mi, 2);
        char *co = g_match_info_fetch(mi, 3);
        char *ms = g_match_info_fetch(mi, 4);
        if (f && ln) {
            char *abs = g_path_is_absolute(f)
                ? g_strdup(f)
                : g_build_filename(ctx->app->proj_root ? ctx->app->proj_root : ".", f, NULL);
            problems_add(ctx->app, abs, atoi(ln), co ? atoi(co) : 0, ms ? ms : "");
            g_free(abs);
        }
        g_free(f); g_free(ln); g_free(co); g_free(ms);
    }
    g_match_info_free(mi);
    g_free(line);

    build_read_line(ctx);
}

static void build_read_line(BuildCtx *ctx)
{
    g_data_input_stream_read_line_async(ctx->dis, G_PRIORITY_DEFAULT, NULL,
                                        on_build_line, ctx);
}

void terminal_run_build(App *app)
{
    if (!app->proj_root) {
        terminal_feed_line(app, "\x1b[33mKein Projekt geöffnet.\x1b[0m");
        return;
    }
    editor_save_all(app);  /* immer den aktuellen Stand bauen            */
    const char *cmd = app->proj_build ? app->proj_build : "bash build.sh";
    problems_clear(app);
    problems_rebuild_bar(app);

    /* Terminal sichtbar machen. */
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->term_revealer), TRUE);

    terminal_feed_line(app, "\x1b[96m⚙ Baue Projekt …\x1b[0m");
    hc_set_activity(app, "Build läuft …");

    /* Build laeuft in derselben Sandbox wie Run und die Live-Pruefung. */
    char **argv = sandbox_exec_argv(app, cmd);
    GSubprocessLauncher *l = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE);
    g_subprocess_launcher_set_cwd(l, app->proj_root);
    GError *e = NULL;
    GSubprocess *proc = g_subprocess_launcher_spawnv(l, (const char * const *)argv, &e);
    g_object_unref(l);
    g_strfreev(argv);
    if (!proc) {
        char *m = g_strdup_printf("\x1b[31mBuild-Start fehlgeschlagen: %s\x1b[0m",
                                   e ? e->message : "?");
        terminal_feed_line(app, m);
        g_free(m); g_clear_error(&e);
        return;
    }

    BuildCtx *ctx = g_new0(BuildCtx, 1);
    ctx->app = app;
    GInputStream *out = g_subprocess_get_stdout_pipe(proc);
    ctx->dis = g_data_input_stream_new(out);
    ctx->re = g_regex_new("^([^\\s:]+):(\\d+)(?::(\\d+))?:\\s*(.*)$", 0, 0, NULL);
    /* proc an den Stream-Lebenszyklus koppeln. */
    g_object_set_data_full(G_OBJECT(ctx->dis), "proc", proc, g_object_unref);
    build_read_line(ctx);
}

/* ============================================================
 * Run (asynchron, saubere Ausgabe – kein cd/Rohbefehl sichtbar)
 * ============================================================ */
typedef struct {
    App *app;
    GDataInputStream *dis;
    GSubprocess *proc;
    GString *buf;   /* gesammelte Ausgabe (für Crash-Analyse) */
} RunCtx;

static void run_read_line(RunCtx *ctx);

static void on_run_line(GObject *src, GAsyncResult *res, gpointer ud)
{
    RunCtx *ctx = ud;
    gsize len = 0;
    char *line = g_data_input_stream_read_line_finish(
        G_DATA_INPUT_STREAM(src), res, &len, NULL);
    if (!line) {
        /* Programm beendet: Exit-Code ermitteln. */
        g_subprocess_wait(ctx->proc, NULL, NULL);
        int code = g_subprocess_get_if_exited(ctx->proc)
            ? g_subprocess_get_exit_status(ctx->proc) : -1;
        if (code == 0)
            terminal_feed_line(ctx->app, "\x1b[90m✔ Programm beendet.\x1b[0m");
        else {
            char *m = g_strdup_printf("\x1b[31m✘ Programm beendet mit Fehler (Code %d).\x1b[0m", code);
            terminal_feed_line(ctx->app, m);
            g_free(m);
            /* Auto-Crash-Hook: Ausgabe zur Analyse an den Agenten. */
            if (ctx->buf->len > 0)
                agent_report_crash(ctx->app, ctx->buf->str);
        }
        g_object_unref(ctx->dis);
        g_string_free(ctx->buf, TRUE);
        g_free(ctx);
        return;
    }
    feed_display(ctx->app, line);
    feed_display(ctx->app, "\n");
    if (ctx->buf->len < 8000) {
        g_string_append(ctx->buf, line);
        g_string_append_c(ctx->buf, '\n');
    }
    g_free(line);
    run_read_line(ctx);
}

static void run_read_line(RunCtx *ctx)
{
    g_data_input_stream_read_line_async(ctx->dis, G_PRIORITY_DEFAULT, NULL,
                                        on_run_line, ctx);
}

void terminal_run_run(App *app)
{
    if (!app->proj_root) {
        terminal_feed_line(app, "\x1b[33mKein Projekt geöffnet.\x1b[0m");
        return;
    }
    editor_save_all(app);
    const char *cmd = app->proj_run ? app->proj_run : "./bin/app";
    gtk_revealer_set_reveal_child(GTK_REVEALER(app->term_revealer), TRUE);

    terminal_feed_line(app, "\x1b[92m▶ Starte Programm …\x1b[0m");

    /* In der Sandbox + im Projektverzeichnis ausführen. */
    char **argv = sandbox_exec_argv(app, cmd);
    GSubprocessLauncher *l = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE);
    g_subprocess_launcher_set_cwd(l, app->proj_root);
    GError *e = NULL;
    GSubprocess *proc = g_subprocess_launcher_spawnv(l, (const char * const *)argv, &e);
    g_object_unref(l);
    g_strfreev(argv);
    if (!proc) {
        char *m = g_strdup_printf("\x1b[31mStart fehlgeschlagen: %s\x1b[0m",
                                   e ? e->message : "?");
        terminal_feed_line(app, m);
        g_free(m); g_clear_error(&e);
        return;
    }

    RunCtx *ctx = g_new0(RunCtx, 1);
    ctx->app = app;
    ctx->proc = proc;
    ctx->buf = g_string_new("");
    GInputStream *out = g_subprocess_get_stdout_pipe(proc);
    ctx->dis = g_data_input_stream_new(out);
    g_object_set_data_full(G_OBJECT(ctx->dis), "proc", g_object_ref(proc), g_object_unref);
    run_read_line(ctx);
}

/* ============================================================
 * Ausgabeflaeche + HyperCore-Befehlszeile
 * ============================================================ */
static void set_vte_colors(App *app)
{
    gboolean dark = (app->settings.theme != HC_THEME_LIGHT);
    GdkRGBA fg, bg;
    if (dark) {
        gdk_rgba_parse(&fg, "#e7eaef");
        gdk_rgba_parse(&bg, "#14171c");
    } else {
        gdk_rgba_parse(&fg, "#1d1d1f");
        gdk_rgba_parse(&bg, "#fbfbfd");
    }
    vte_terminal_set_colors(VTE_TERMINAL(app->term), &fg, &bg, NULL, 0);
}

void terminal_apply_theme(App *app)
{
    if (app->term) set_vte_colors(app);
}

/* Frueher: cd in die interaktive Shell. Jetzt nur noch eine Notiz in
 * der Ausgabe – es gibt keine Shell mehr, in die man cd schicken kann. */
void terminal_cd(App *app, const char *path)
{
    if (!app->term || !path) return;
    char *m = g_strdup_printf("\x1b[90mArbeitsverzeichnis: %s\x1b[0m", path);
    terminal_feed_line(app, m);
    g_free(m);
}

/* ============================================================
 * Fehler an den Agenten geben
 * ============================================================ */

/* Sammelt die aktuell bekannten Probleme als Text. */
static char *problems_as_text(App *app)
{
    if (!app->problems) return NULL;
    GString *s = g_string_new("");
    for (GList *l = app->problems; l; l = l->next) {
        HcProblem *pr = l->data;
        const char *base = pr->file ? strrchr(pr->file, '/') : NULL;
        base = base ? base + 1 : (pr->file ? pr->file : "?");
        g_string_append_printf(s, "%s:%d:%d: %s\n",
                               base, pr->line, pr->col, pr->msg ? pr->msg : "");
    }
    return g_string_free(s, FALSE);
}

void terminal_send_errors_to_agent(App *app)
{
    char *txt = problems_as_text(app);
    if (!txt || !*txt) {
        terminal_feed_line(app, "\x1b[90m[Agent] Keine Fehler zum Senden.\x1b[0m");
        g_free(txt);
        return;
    }
    terminal_feed_line(app, "\x1b[95m[Agent] Fehler zur Analyse uebergeben.\x1b[0m");
    agent_report_crash(app, txt);
    g_free(txt);
}

/* ============================================================
 * Eigener Befehlssatz der HyperCore-Konsole
 * ============================================================ */
static void console_help(App *app)
{
    terminal_feed_line(app, "\x1b[96mHyperCore-Konsole – Befehle\x1b[0m");
    terminal_feed_line(app, "  build              Projekt bauen");
    terminal_feed_line(app, "  run                Projekt starten (typgerecht)");
    terminal_feed_line(app, "  emulator           Anwendung im GUI-Emulator starten");
    terminal_feed_line(app, "  services           Status aller Dienste anzeigen");
    terminal_feed_line(app, "  restart <dienst>   web | db | mail | emulator");
    terminal_feed_line(app, "  url                Adresse des Webservers");
    terminal_feed_line(app, "  db                 Zugangsdaten der Projekt-Datenbank");
    terminal_feed_line(app, "  mail               Abgefangene Mails zaehlen");
    terminal_feed_line(app, "  open <datei>       Datei im Editor oeffnen");
    terminal_feed_line(app, "  snapshot           GUI-Snapshot aufnehmen");
    terminal_feed_line(app, "  errors             Fehler an den KI-Agenten senden");
    terminal_feed_line(app, "  sandbox            Sandbox-Status / Inspektor");
    terminal_feed_line(app, "  clear              Ausgabe leeren");
    terminal_feed_line(app, "  help               diese Liste");
}

static void console_services(App *app)
{
    terminal_feed_line(app, "\x1b[96mDienste\x1b[0m");
    for (int i = 0; i < HC_SVC_KIND_COUNT; i++) {
        HcService *sv = services_get(app, (HcServiceKind)i);
        char *line = g_strdup_printf("  %-22s %-16s %s",
            sv->label, services_state_label(sv->state),
            sv->detail ? sv->detail : "");
        terminal_feed_line(app, line);
        g_free(line);
    }
}

static void console_exec(App *app, const char *input)
{
    char *line = g_strstrip(g_strdup(input));
    if (!*line) { g_free(line); return; }

    /* Eingabe mitschreiben, damit die Konsole nachvollziehbar bleibt. */
    char *echo = g_strdup_printf("\x1b[1;38;5;75mhypercore\x1b[0m$ %s", line);
    terminal_feed_line(app, echo);
    g_free(echo);

    char **tok = g_strsplit_set(line, " \t", 2);
    const char *cmd = tok[0] ? tok[0] : "";
    const char *arg = tok[1] ? g_strstrip(tok[1]) : NULL;

    if (!g_strcmp0(cmd, "help") || !g_strcmp0(cmd, "?")) {
        console_help(app);
    } else if (!g_strcmp0(cmd, "clear")) {
        vte_terminal_reset(VTE_TERMINAL(app->term), TRUE, TRUE);
    } else if (!g_strcmp0(cmd, "build")) {
        terminal_run_build(app);
    } else if (!g_strcmp0(cmd, "run")) {
        g_action_group_activate_action(G_ACTION_GROUP(app->app), "run", NULL);
    } else if (!g_strcmp0(cmd, "emulator")) {
        preview_run_broadway(app);
    } else if (!g_strcmp0(cmd, "services")) {
        console_services(app);
    } else if (!g_strcmp0(cmd, "sandbox")) {
        inspector_show_status(app);
    } else if (!g_strcmp0(cmd, "snapshot")) {
        preview_snapshot_current(app);
    } else if (!g_strcmp0(cmd, "errors")) {
        terminal_send_errors_to_agent(app);
    } else if (!g_strcmp0(cmd, "url")) {
        char *u = services_web_base_url(app);
        if (u) { terminal_feed_line(app, u); g_free(u); }
        else terminal_feed_line(app, "\x1b[33mKein Webserver aktiv.\x1b[0m");
    } else if (!g_strcmp0(cmd, "db")) {
        HcService *sv = services_get(app, HC_SVC_DB);
        if (sv->state == HC_SVC_RUNNING) {
            char *m = g_strdup_printf(
                "Host 127.0.0.1   Port %d   Benutzer root   Passwort (leer)",
                app->db_port);
            terminal_feed_line(app, m);
            g_free(m);
            if (sv->detail) terminal_feed_line(app, sv->detail);
        } else {
            terminal_feed_line(app, "\x1b[33mKeine Datenbank aktiv "
                "(im Wizard oder in der .hcs aktivieren).\x1b[0m");
        }
    } else if (!g_strcmp0(cmd, "mail")) {
        char *m = g_strdup_printf("%u Mail(s) abgefangen – Reiter \"Mail\".",
                                  g_list_length(app->mails));
        terminal_feed_line(app, m);
        g_free(m);
    } else if (!g_strcmp0(cmd, "restart")) {
        if (!arg) { terminal_feed_line(app, "restart web|db|mail|emulator"); }
        else if (!g_strcmp0(arg, "web"))      services_restart(app, HC_SVC_WEB);
        else if (!g_strcmp0(arg, "db"))       services_restart(app, HC_SVC_DB);
        else if (!g_strcmp0(arg, "mail"))     services_restart(app, HC_SVC_MAIL);
        else if (!g_strcmp0(arg, "emulator")) services_restart(app, HC_SVC_EMULATOR);
        else terminal_feed_line(app, "Unbekannter Dienst.");
    } else if (!g_strcmp0(cmd, "open")) {
        if (!arg) terminal_feed_line(app, "open <datei>");
        else if (app->proj_root) {
            char *full = g_path_is_absolute(arg) ? g_strdup(arg)
                       : g_build_filename(app->proj_root, arg, NULL);
            if (g_file_test(full, G_FILE_TEST_IS_REGULAR)) editor_open_file(app, full);
            else terminal_feed_line(app, "\x1b[33mDatei nicht gefunden.\x1b[0m");
            g_free(full);
        }
    } else {
        char *m = g_strdup_printf(
            "\x1b[33mUnbekannter Befehl: %s   (\"help\" zeigt alle)\x1b[0m", cmd);
        terminal_feed_line(app, m);
        g_free(m);
    }
    g_strfreev(tok);
    g_free(line);
}

/* ---- Befehlszeile: eigene History, NICHT die der Bash ---- */
static GList *s_hist = NULL;      /* char*, neueste zuerst */
static int    s_hist_pos = -1;

static void on_console_activate(GtkEntry *e, gpointer ud)
{
    App *app = ud;
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(e));
    if (!txt || !*txt) return;
    s_hist = g_list_prepend(s_hist, g_strdup(txt));
    s_hist_pos = -1;
    console_exec(app, txt);
    gtk_editable_set_text(GTK_EDITABLE(e), "");
}

static gboolean on_console_key(GtkEventControllerKey *c, guint keyval, guint kc,
                               GdkModifierType st, gpointer ud)
{
    (void)c; (void)kc; (void)st;
    GtkWidget *entry = ud;
    if (keyval == GDK_KEY_Up) {
        if (s_hist && s_hist_pos + 1 < (int)g_list_length(s_hist)) {
            s_hist_pos++;
            gtk_editable_set_text(GTK_EDITABLE(entry),
                                  (char *)g_list_nth_data(s_hist, s_hist_pos));
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Down) {
        if (s_hist_pos > 0) {
            s_hist_pos--;
            gtk_editable_set_text(GTK_EDITABLE(entry),
                                  (char *)g_list_nth_data(s_hist, s_hist_pos));
        } else {
            s_hist_pos = -1;
            gtk_editable_set_text(GTK_EDITABLE(entry), "");
        }
        return TRUE;
    }
    return FALSE;
}

/* ============================================================
 * Kopieren + Kontextmenue
 * ============================================================ */
static gboolean term_key(GtkEventControllerKey *c, guint keyval, guint kc,
                         GdkModifierType state, gpointer ud)
{
    (void)c; (void)kc;
    App *app = ud;
    gboolean ctrl_shift = (state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK);
    if (ctrl_shift && (keyval == GDK_KEY_C || keyval == GDK_KEY_c)) {
        vte_terminal_copy_clipboard_format(VTE_TERMINAL(app->term), VTE_FORMAT_TEXT);
        return TRUE;
    }
    if ((state & GDK_CONTROL_MASK) && keyval == GDK_KEY_Insert) {
        vte_terminal_copy_clipboard_format(VTE_TERMINAL(app->term), VTE_FORMAT_TEXT);
        return TRUE;
    }
    return FALSE;
}

static void popdown(GtkWidget *w)
{
    GtkWidget *pop = gtk_widget_get_ancestor(w, GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

static void mi_copy(GtkButton *b, gpointer ud)
{
    App *app = ud;
    vte_terminal_copy_clipboard_format(VTE_TERMINAL(app->term), VTE_FORMAT_TEXT);
    popdown(GTK_WIDGET(b));
}

/* Rechtsklick: markierten Text direkt an den Agenten. */
static void mi_sel_to_agent(GtkButton *b, gpointer ud)
{
    App *app = ud;
    char *sel = vte_terminal_get_text_selected(VTE_TERMINAL(app->term), VTE_FORMAT_TEXT);
    popdown(GTK_WIDGET(b));
    if (sel && *g_strstrip(sel)) {
        terminal_feed_line(app, "\x1b[95m[Agent] Auswahl zur Analyse uebergeben.\x1b[0m");
        agent_report_crash(app, sel);
    } else {
        terminal_feed_line(app, "\x1b[90m[Agent] Nichts markiert.\x1b[0m");
    }
    g_free(sel);
}

static void mi_errors_to_agent(GtkButton *b, gpointer ud)
{
    App *app = ud;
    popdown(GTK_WIDGET(b));
    terminal_send_errors_to_agent(app);
}

static void mi_clear(GtkButton *b, gpointer ud)
{
    App *app = ud;
    popdown(GTK_WIDGET(b));
    vte_terminal_reset(VTE_TERMINAL(app->term), TRUE, TRUE);
}

static GtkWidget *menu_item(const char *label, GCallback cb, App *app)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_button_set_has_frame(GTK_BUTTON(b), FALSE);
    gtk_widget_add_css_class(b, "hc-menu-item");
    GtkWidget *child = gtk_button_get_child(GTK_BUTTON(b));
    if (GTK_IS_LABEL(child)) gtk_label_set_xalign(GTK_LABEL(child), 0.0);
    g_signal_connect(b, "clicked", cb, app);
    return b;
}

static void term_right_click(GtkGestureClick *g, int n, double x, double y, gpointer ud)
{
    (void)n;
    App *app = ud;
    GtkWidget *pop = gtk_popover_new();
    gtk_widget_set_parent(pop, app->term);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    GdkRectangle r = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(box), menu_item("Kopieren", G_CALLBACK(mi_copy), app));
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box),
        menu_item("Auswahl an Agenten senden", G_CALLBACK(mi_sel_to_agent), app));
    gtk_box_append(GTK_BOX(box),
        menu_item("Alle Fehler an Agenten senden", G_CALLBACK(mi_errors_to_agent), app));
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), menu_item("Ausgabe leeren", G_CALLBACK(mi_clear), app));

    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_popover_popup(GTK_POPOVER(pop));
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
}

/* ============================================================
 * Aufbau
 * ============================================================ */
GtkWidget *terminal_new(App *app)
{
    app->term = vte_terminal_new();
    gtk_widget_add_css_class(app->term, "hc-terminal");
    gtk_widget_set_vexpand(app->term, TRUE);
    gtk_widget_set_hexpand(app->term, TRUE);
    vte_terminal_set_scrollback_lines(VTE_TERMINAL(app->term), 10000);
    vte_terminal_set_mouse_autohide(VTE_TERMINAL(app->term), TRUE);
    /* Reine Ausgabeflaeche: kein Cursor, keine Tastatureingabe und vor
     * allem kein Shell-Prozess, dessen History man durchblaettern kann. */
    vte_terminal_set_cursor_blink_mode(VTE_TERMINAL(app->term), VTE_CURSOR_BLINK_OFF);
    vte_terminal_set_input_enabled(VTE_TERMINAL(app->term), FALSE);
    set_vte_colors(app);

    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(term_key), app);
    gtk_widget_add_controller(app->term, kc);

    GtkGesture *rc_g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc_g), GDK_BUTTON_SECONDARY);
    g_signal_connect(rc_g, "pressed", G_CALLBACK(term_right_click), app);
    gtk_widget_add_controller(app->term, GTK_EVENT_CONTROLLER(rc_g));

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), app->term);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(box), scroller);

    /* HyperCore-Befehlszeile. */
    GtkWidget *cmdrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(cmdrow, "hc-cmdrow");
    gtk_widget_set_margin_start(cmdrow, 8); gtk_widget_set_margin_end(cmdrow, 8);
    gtk_widget_set_margin_top(cmdrow, 4); gtk_widget_set_margin_bottom(cmdrow, 6);
    GtkWidget *prompt = gtk_label_new("hypercore$");
    gtk_widget_add_css_class(prompt, "hc-cmd-prompt");
    gtk_box_append(GTK_BOX(cmdrow), prompt);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry),
        "IDE-Befehl – \"help\" zeigt alle (keine System-Shell)");
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_widget_add_css_class(entry, "hc-cmd-entry");
    g_signal_connect(entry, "activate", G_CALLBACK(on_console_activate), app);
    GtkEventController *ek = gtk_event_controller_key_new();
    g_signal_connect(ek, "key-pressed", G_CALLBACK(on_console_key), entry);
    gtk_widget_add_controller(entry, ek);
    gtk_box_append(GTK_BOX(cmdrow), entry);

    GtkWidget *hb = gtk_button_new_with_label("help");
    gtk_widget_add_css_class(hb, "flat");
    g_signal_connect_swapped(hb, "clicked", G_CALLBACK(console_help), app);
    gtk_box_append(GTK_BOX(cmdrow), hb);

    gtk_box_append(GTK_BOX(box), cmdrow);

    terminal_feed_line(app,
        "\x1b[96mHyperCore-Konsole\x1b[0m  –  eigene Umgebung, keine System-Shell. "
        "\"help\" zeigt die Befehle.");
    return box;
}
