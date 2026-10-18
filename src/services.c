#include "app.h"
#include "version.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

/* ============================================================
 * Dienst-Orchestrator ("Maschinenraum").
 *
 * Grundsatz: Ein Projekt zu öffnen heißt, eine komplette, laufende
 * Umgebung hochzufahren – nicht nur einen Dateibaum zu zeichnen.
 * Welche Dienste gebraucht werden, entscheidet der Projekttyp:
 *
 *   Webprojekt (PHP/HTML)  -> Webserver  (+ Datenbank, + Mail-Catcher)
 *   Native GUI (GTK3/GTK4) -> GUI-Emulator (gtk4-broadwayd)
 *   Mischform (C + Web)    -> beides gleichzeitig
 *
 * Jeder Dienst läuft IN der Sandbox (sandbox_exec_argv) und wird nach
 * dem Start per TCP-Health-Check verifiziert – es wird nicht blind
 * gewartet. Erst wenn alle Pflichtdienste laufen, öffnet das Projekt.
 * ============================================================ */

#define HC_HEALTH_INTERVAL_MS 350
#define HC_HEALTH_MAX_TRIES   60     /* ~21 s Geduld pro Dienst */

/* ============================================================
 * Hilfen
 * ============================================================ */

/* Freien TCP-Port ab 'preferred' suchen (max. 200 Schritte). */
int hc_find_free_port(int preferred)
{
    for (int p = preferred; p < preferred + 200; p++) {
        GSocket *s = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
                                  G_SOCKET_PROTOCOL_TCP, NULL);
        if (!s) return preferred;
        GInetAddress *ia = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
        GSocketAddress *sa = g_inet_socket_address_new(ia, p);
        gboolean ok = g_socket_bind(s, sa, TRUE, NULL);
        g_object_unref(sa);
        g_object_unref(ia);
        g_object_unref(s);
        if (ok) return p;
    }
    return preferred;
}

const char *services_state_label(HcServiceState st)
{
    switch (st) {
        case HC_SVC_RUNNING:  return "läuft";
        case HC_SVC_STARTING: return "startet…";
        case HC_SVC_FAILED:   return "fehlgeschlagen";
        case HC_SVC_SKIPPED:  return "nicht benötigt";
        default:              return "gestoppt";
    }
}

static const char *state_dot(HcServiceState st)
{
    switch (st) {
        case HC_SVC_RUNNING:  return "🟢";
        case HC_SVC_STARTING: return "🟡";
        case HC_SVC_FAILED:   return "🔴";
        case HC_SVC_SKIPPED:  return "⚪";
        default:              return "⚪";
    }
}

HcService *services_get(App *app, HcServiceKind kind)
{
    if (kind < 0 || kind >= HC_SVC_KIND_COUNT) return NULL;
    return &app->services[kind];
}

/* Bind-Adresse: in der Sandbox muss auf allen Interfaces gelauscht
 * werden, damit die IDE auf dem Host den Dienst erreicht. */
static const char *bind_addr(App *app)
{
    return (app->sandbox_backend == HC_SB_DOCKER) ? "0.0.0.0" : "127.0.0.1";
}

/* ============================================================
 * Projekttyp-Erkennung
 * ============================================================ */
gboolean services_project_is_web(App *app)
{
    const char *t = app->proj_template ? app->proj_template : "";
    const char *l = app->proj_lang ? app->proj_lang : "";
    if (strstr(t, "Web")) return TRUE;
    /* PHP ist immer web-artig. */
    if (!g_strcmp0(l, "PHP")) return TRUE;
    /* Auch gemischte Projekte (z.B. C mit Web-Oberfläche) erkennen:
     * liegt im Projekt eine index.html/index.php, ist es ein Webprojekt. */
    if (app->proj_root) {
        const char *cand[] = { "index.php", "index.html", "public/index.php",
                               "public/index.html", "web/index.php", NULL };
        for (int i = 0; cand[i]; i++) {
            char *p = g_build_filename(app->proj_root, cand[i], NULL);
            gboolean e = g_file_test(p, G_FILE_TEST_IS_REGULAR);
            g_free(p);
            if (e) return TRUE;
        }
    }
    return FALSE;
}

gboolean services_project_is_native_gui(App *app)
{
    const char *t = app->proj_template ? app->proj_template : "";
    return (strstr(t, "GTK") != NULL);
}

gboolean services_project_wants_db(App *app)
{
    /* Wird im Wizard gesetzt und in der .hcs gespeichert. */
    return project_flag(app, "db", FALSE);
}

/* Datenbankname aus dem Projektnamen (nur [a-z0-9_]). */
static char *db_name_for(App *app)
{
    GString *s = g_string_new("");
    for (const char *p = app->proj_name ? app->proj_name : "projekt"; *p; p++) {
        if (g_ascii_isalnum(*p)) g_string_append_c(s, g_ascii_tolower(*p));
        else g_string_append_c(s, '_');
    }
    if (s->len == 0) g_string_append(s, "projekt");
    return g_string_free(s, FALSE);
}

/* Arbeitsverzeichnis der IDE im Projekt (Logs, DB-Daten, php.ini). */
static char *hc_dir(App *app, const char *sub)
{
    char *d = sub ? g_build_filename(app->proj_root, ".hypercore", sub, NULL)
                  : g_build_filename(app->proj_root, ".hypercore", NULL);
    g_mkdir_with_parents(d, 0755);
    return d;
}

/* ============================================================
 * php.ini + sendmail-Shim (Zero-Config Mail-Catcher)
 *
 * Damit mail() aus dem Projekt heraus funktioniert, ohne dass der
 * Nutzer irgendetwas konfiguriert, erzeugt die IDE eine eigene
 * php.ini und einen sendmail-Ersatz, der die Mail per SMTP an den
 * eingebauten Catcher schickt.
 * ============================================================ */
static char *write_php_support_files(App *app)
{
    char *dir = hc_dir(app, NULL);
    char *shim = g_build_filename(dir, "sendmail", NULL);
    char *ini  = g_build_filename(dir, "php.ini", NULL);

    char *shim_body = g_strdup_printf(
        "#!/usr/bin/env python3\n"
        "# HyperCore Studio – sendmail-Ersatz: leitet Mails an den\n"
        "# eingebauten SMTP-Catcher der IDE weiter.\n"
        "import socket, sys\n"
        "raw = sys.stdin.read()\n"
        "to = 'unbekannt@localhost'\n"
        "for line in raw.splitlines():\n"
        "    if line.lower().startswith('to:'):\n"
        "        to = line[3:].strip(); break\n"
        "try:\n"
        "    s = socket.create_connection(('127.0.0.1', %d), 5)\n"
        "    f = s.makefile('rwb')\n"
        "    def rd():  return f.readline()\n"
        "    def wr(x): f.write((x + '\\r\\n').encode()); f.flush()\n"
        "    rd(); wr('HELO hypercore'); rd()\n"
        "    wr('MAIL FROM:<php@hypercore>'); rd()\n"
        "    wr('RCPT TO:<%%s>' %% to); rd()\n"
        "    wr('DATA'); rd()\n"
        "    f.write(raw.encode()); f.write(b'\\r\\n.\\r\\n'); f.flush(); rd()\n"
        "    wr('QUIT')\n"
        "    s.close()\n"
        "except Exception as e:\n"
        "    sys.stderr.write('sendmail-shim: %%s\\n' %% e)\n"
        "    sys.exit(1)\n",
        app->mail_port);
    g_file_set_contents(shim, shim_body, -1, NULL);
    g_chmod(shim, 0755);
    g_free(shim_body);

    /* Pfad, unter dem der Shim IN der Sandbox sichtbar ist. */
    char *shim_in_sb = g_build_filename(sandbox_project_path(app),
                                        ".hypercore", "sendmail", NULL);
    char *ini_body = g_strdup_printf(
        "; HyperCore Studio – automatisch erzeugt, nicht von Hand ändern.\n"
        "display_errors = On\n"
        "error_reporting = E_ALL\n"
        "sendmail_path = %s\n"
        "SMTP = 127.0.0.1\n"
        "smtp_port = %d\n",
        shim_in_sb, app->mail_port);
    g_file_set_contents(ini, ini_body, -1, NULL);
    g_free(ini_body);
    g_free(shim_in_sb);
    g_free(shim);
    g_free(dir);

    /* Rückgabe: php.ini-Pfad innerhalb der Sandbox. */
    return g_build_filename(sandbox_project_path(app), ".hypercore", "php.ini", NULL);
}

/* ============================================================
 * Dienste-Panel (UI)
 * ============================================================ */
typedef struct { App *app; HcServiceKind kind; } RestartJob;

static void on_restart_clicked(GtkButton *b, gpointer ud)
{
    (void)b;
    RestartJob *j = ud;
    services_restart(j->app, j->kind);
}
static void restart_job_free(gpointer p, GClosure *c) { (void)c; g_free(p); }

static gboolean services_clear_activity_cb(gpointer ud)
{
    hc_set_activity((App *)ud, NULL);
    return G_SOURCE_REMOVE;
}

void services_panel_refresh(App *app)
{
    if (!app->svc_list) return;

    int running = 0, total = 0;
    for (int i = 0; i < HC_SVC_KIND_COUNT; i++) {
        HcService *sv = &app->services[i];
        if (sv->row_dot) {
            gtk_label_set_text(GTK_LABEL(sv->row_dot), state_dot(sv->state));
            char *d = g_strdup_printf("%s%s%s",
                services_state_label(sv->state),
                sv->detail && *sv->detail ? " · " : "",
                sv->detail && *sv->detail ? sv->detail : "");
            gtk_label_set_text(GTK_LABEL(sv->row_detail), d);
            g_free(d);
        }
        if (sv->state == HC_SVC_SKIPPED) continue;
        total++;
        if (sv->state == HC_SVC_RUNNING) running++;
    }
    if (app->svc_status_lbl) {
        char *s = total ? g_strdup_printf("%d/%d Dienste", running, total)
                        : g_strdup("keine Dienste");
        gtk_label_set_text(GTK_LABEL(app->svc_status_lbl), s);
        g_free(s);
    }

    /* Dienst-Status auch in der zentralen Aktivitätsleiste spiegeln:
     * Solange Dienste starten → Spinner mit Fortschritt.
     * Sobald alle fertig (running+failed+skipped == total) → kurz anzeigen, dann löschen. */
    int starting = 0, failed = 0;
    for (int i = 0; i < HC_SVC_KIND_COUNT; i++) {
        HcService *sv = &app->services[i];
        if (sv->state == HC_SVC_STARTING) starting++;
        if (sv->state == HC_SVC_FAILED)   failed++;
    }
    if (starting > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Dienste: %d/%d laufen …", running, total);
        hc_set_activity(app, buf);
    } else if (total > 0) {
        if (failed > 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "⚠ %d Dienst(e) fehlgeschlagen", failed);
            hc_set_activity(app, buf);
        } else if (running == total) {
            char buf[64];
            snprintf(buf, sizeof(buf), "✔ Alle Dienste laufen (%d)", running);
            hc_set_activity(app, buf);
        }
        /* Status nach 5 s löschen – danach nur noch im Dienste-Panel sichtbar. */
        g_timeout_add_seconds(5, (GSourceFunc)services_clear_activity_cb, app);
    }
}

GtkWidget *services_panel_new(App *app)
{
    GtkWidget *scr = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scr, TRUE);
    app->svc_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(app->svc_list, "hc-svc-list");
    gtk_widget_set_margin_start(app->svc_list, 10);
    gtk_widget_set_margin_end(app->svc_list, 10);
    gtk_widget_set_margin_top(app->svc_list, 8);
    gtk_widget_set_margin_bottom(app->svc_list, 8);

    for (int i = 0; i < HC_SVC_KIND_COUNT; i++) {
        HcService *sv = &app->services[i];
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_add_css_class(row, "hc-svc-row");

        sv->row_dot = gtk_label_new(state_dot(sv->state));
        gtk_box_append(GTK_BOX(row), sv->row_dot);

        GtkWidget *name = gtk_label_new(sv->label);
        gtk_label_set_xalign(GTK_LABEL(name), 0.0);
        gtk_widget_set_size_request(name, 190, -1);
        gtk_widget_add_css_class(name, "hc-svc-name");
        gtk_box_append(GTK_BOX(row), name);

        sv->row_detail = gtk_label_new(services_state_label(sv->state));
        gtk_label_set_xalign(GTK_LABEL(sv->row_detail), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(sv->row_detail), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(sv->row_detail, TRUE);
        gtk_widget_add_css_class(sv->row_detail, "dim-label");
        gtk_box_append(GTK_BOX(row), sv->row_detail);

        GtkWidget *rb = gtk_button_new_with_label("Neu starten");
        gtk_widget_add_css_class(rb, "flat");
        RestartJob *j = g_new0(RestartJob, 1);
        j->app = app; j->kind = (HcServiceKind)i;
        g_signal_connect_data(rb, "clicked", G_CALLBACK(on_restart_clicked),
                              j, restart_job_free, 0);
        gtk_box_append(GTK_BOX(row), rb);

        gtk_box_append(GTK_BOX(app->svc_list), row);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), app->svc_list);
    return scr;
}

/* ============================================================
 * Mail-Catcher (eingebauter SMTP-Server)
 * ============================================================ */
static void mail_free(gpointer p)
{
    HcMail *m = p;
    g_free(m->from); g_free(m->to); g_free(m->subject);
    g_free(m->body); g_free(m->received);
    g_free(m);
}

void services_mails_clear(App *app)
{
    g_list_free_full(app->mails, mail_free);
    app->mails = NULL;
    services_mail_panel_refresh(app);
}

void services_mail_panel_refresh(App *app)
{
    if (!app->mail_list) return;
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(app->mail_list)))
        gtk_box_remove(GTK_BOX(app->mail_list), c);

    if (!app->mails) {
        GtkWidget *l = gtk_label_new("Noch keine Mail abgefangen. "
                                     "mail() aus dem Projekt landet hier.");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(l), 0.0);
        gtk_widget_set_margin_start(l, 4);
        gtk_box_append(GTK_BOX(app->mail_list), l);
        return;
    }
    for (GList *l = app->mails; l; l = l->next) {
        HcMail *m = l->data;
        GtkWidget *exp = gtk_expander_new(NULL);
        char *head = g_strdup_printf("✉  %s  →  %s   (%s)",
            m->subject && *m->subject ? m->subject : "(kein Betreff)",
            m->to ? m->to : "?", m->received ? m->received : "");
        gtk_expander_set_label(GTK_EXPANDER(exp), head);
        g_free(head);
        GtkWidget *body = gtk_label_new(m->body ? m->body : "");
        gtk_label_set_selectable(GTK_LABEL(body), TRUE);
        gtk_label_set_xalign(GTK_LABEL(body), 0.0);
        gtk_label_set_wrap(GTK_LABEL(body), TRUE);
        gtk_widget_add_css_class(body, "hc-mail-body");
        gtk_expander_set_child(GTK_EXPANDER(exp), body);
        gtk_box_append(GTK_BOX(app->mail_list), exp);
    }
}

/* Vom SMTP-Thread in den Hauptthread übergeben. */
typedef struct { App *app; HcMail *mail; } MailDrop;

static gboolean mail_arrived(gpointer ud)
{
    MailDrop *d = ud;
    d->app->mails = g_list_prepend(d->app->mails, d->mail);
    services_mail_panel_refresh(d->app);
    char *m = g_strdup_printf("\x1b[95m[Mail] abgefangen: %s → %s\x1b[0m",
        d->mail->subject ? d->mail->subject : "(kein Betreff)",
        d->mail->to ? d->mail->to : "?");
    terminal_feed_line(d->app, m);
    g_free(m);
    g_free(d);
    return G_SOURCE_REMOVE;
}

/* Minimaler SMTP-Dialog. Läuft im Thread (GThreadedSocketService). */
static gboolean smtp_run(GThreadedSocketService *svc, GSocketConnection *conn,
                         GObject *src, gpointer ud)
{
    (void)svc; (void)src;
    App *app = ud;
    GOutputStream *os = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    GInputStream  *is = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GDataInputStream *dis = g_data_input_stream_new(is);
    g_data_input_stream_set_newline_type(dis, G_DATA_STREAM_NEWLINE_TYPE_ANY);

    #define SAY(txt) g_output_stream_write_all(os, txt "\r\n", strlen(txt) + 2, NULL, NULL, NULL)
    SAY("220 hypercore ESMTP Mail-Catcher");

    char *from = NULL, *to = NULL;
    GString *data = g_string_new("");
    gboolean in_data = FALSE, done = FALSE;

    while (!done) {
        char *line = g_data_input_stream_read_line(dis, NULL, NULL, NULL);
        if (!line) break;

        if (in_data) {
            if (!g_strcmp0(line, ".")) {
                in_data = FALSE;
                SAY("250 OK: Mail angenommen");
            } else {
                /* Transparenz-Punkt nach RFC 5321 entfernen. */
                const char *l = (line[0] == '.' && line[1]) ? line + 1 : line;
                g_string_append(data, l);
                g_string_append_c(data, '\n');
            }
            g_free(line);
            continue;
        }

        if (g_ascii_strncasecmp(line, "EHLO", 4) == 0) {
            SAY("250-hypercore");
            SAY("250 HELP");
        } else if (g_ascii_strncasecmp(line, "HELO", 4) == 0) {
            SAY("250 hypercore");
        } else if (g_ascii_strncasecmp(line, "MAIL FROM:", 10) == 0) {
            g_free(from); from = g_strstrip(g_strdup(line + 10));
            SAY("250 OK");
        } else if (g_ascii_strncasecmp(line, "RCPT TO:", 8) == 0) {
            g_free(to); to = g_strstrip(g_strdup(line + 8));
            SAY("250 OK");
        } else if (g_ascii_strncasecmp(line, "DATA", 4) == 0) {
            in_data = TRUE;
            SAY("354 Ende mit einer Zeile, die nur einen Punkt enthält");
        } else if (g_ascii_strncasecmp(line, "RSET", 4) == 0) {
            g_string_truncate(data, 0);
            SAY("250 OK");
        } else if (g_ascii_strncasecmp(line, "QUIT", 4) == 0) {
            SAY("221 Tschuess");
            done = TRUE;
        } else {
            SAY("250 OK");
        }
        g_free(line);
    }
    #undef SAY

    if (data->len > 0) {
        HcMail *m = g_new0(HcMail, 1);
        m->from = from ? g_strdup(from) : g_strdup("?");
        m->to   = to   ? g_strdup(to)   : g_strdup("?");
        m->body = g_strdup(data->str);
        /* Betreff aus den Kopfzeilen ziehen. */
        char **lines = g_strsplit(data->str, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (g_ascii_strncasecmp(lines[i], "Subject:", 8) == 0) {
                m->subject = g_strstrip(g_strdup(lines[i] + 8));
                break;
            }
            if (!*lines[i]) break;   /* Kopf zu Ende */
        }
        g_strfreev(lines);
        GDateTime *now = g_date_time_new_now_local();
        m->received = g_date_time_format(now, "%H:%M:%S");
        g_date_time_unref(now);

        MailDrop *d = g_new0(MailDrop, 1);
        d->app = app; d->mail = m;
        g_idle_add(mail_arrived, d);
    }
    g_string_free(data, TRUE);
    g_free(from); g_free(to);
    g_object_unref(dis);
    return FALSE;   /* Verbindung schließen */
}

/* Mail-Catcher läuft IN der IDE (kein externes Paket nötig). */
static gboolean mail_catcher_start(App *app, char **err)
{
    if (app->mail_service) return TRUE;   /* läuft schon */
    GThreadedSocketService *svc =
        G_THREADED_SOCKET_SERVICE(g_threaded_socket_service_new(8));
    GError *e = NULL;
    /* Auf allen Interfaces lauschen, damit auch die Sandbox zustellen kann. */
    if (!g_socket_listener_add_inet_port(G_SOCKET_LISTENER(svc),
                                         app->mail_port, NULL, &e)) {
        if (err) *err = g_strdup(e ? e->message : "Port belegt");
        g_clear_error(&e);
        g_object_unref(svc);
        return FALSE;
    }
    g_signal_connect(svc, "run", G_CALLBACK(smtp_run), app);
    g_socket_service_start(G_SOCKET_SERVICE(svc));
    app->mail_service = G_SOCKET_SERVICE(svc);
    return TRUE;
}

static void mail_catcher_stop(App *app)
{
    if (!app->mail_service) return;
    g_socket_service_stop(app->mail_service);
    g_socket_listener_close(G_SOCKET_LISTENER(app->mail_service));
    g_clear_object(&app->mail_service);
}

/* ============================================================
 * Health-Check: TCP-Verbindung zum Dienst-Port
 * ============================================================ */
typedef struct {
    App          *app;
    HcServiceKind kind;
    HcServicesCb  cb;        /* nur beim Sammelstart gesetzt */
    gpointer      cb_ud;
} HealthCtx;

static void start_next_or_done(App *app, HcServicesCb cb, gpointer ud);

/* Wird aufgerufen, sobald ein Dienst endgültig läuft oder scheitert. */
static void service_settled(HealthCtx *h)
{
    App *app = h->app;
    services_panel_refresh(app);
    if (h->cb) start_next_or_done(app, h->cb, h->cb_ud);
    g_free(h);
}

static gboolean health_tick(gpointer ud);

/* Prozessende sauber verfolgen. g_subprocess_get_if_exited() darf erst
 * NACH einem wait aufgerufen werden – sonst schlaegt eine GLib-Assertion
 * zu und der Zustand ist unbrauchbar. */
static void on_service_exit(GObject *src, GAsyncResult *res, gpointer ud)
{
    HcService *sv = ud;
    GSubprocess *p = G_SUBPROCESS(src);
    g_subprocess_wait_finish(p, res, NULL);
    /* Nur fuer den noch aktuellen Prozess Buch fuehren: ein zwischen-
     * zeitlicher Neustart hat sv->proc schon ersetzt. */
    if (sv->proc == p) {
        sv->proc_exited = TRUE;
        sv->proc_status = g_subprocess_get_if_exited(p)
            ? g_subprocess_get_exit_status(p) : -1;
    }
    g_object_unref(p);
}

static void on_health_connect(GObject *src, GAsyncResult *res, gpointer ud)
{
    HealthCtx *h = ud;
    App *app = h->app;
    HcService *sv = &app->services[h->kind];
    GError *e = NULL;
    GSocketConnection *c = g_socket_client_connect_finish(G_SOCKET_CLIENT(src), res, &e);
    g_object_unref(src);

    if (c) {
        g_io_stream_close(G_IO_STREAM(c), NULL, NULL);
        g_object_unref(c);
        sv->state = HC_SVC_RUNNING;
        g_free(sv->detail);
        sv->detail = g_strdup_printf("Port %d", sv->port);
        sv->health_timer = 0;
        g_clear_error(&e);
        service_settled(h);
        return;
    }
    g_clear_error(&e);

    /* Prozess schon gestorben? Dann gar nicht weiter pollen, sondern
     * sofort mit Hinweis auf die Logdatei scheitern. */
    if (sv->proc_exited) {
        sv->state = HC_SVC_FAILED;
        g_free(sv->detail);
        sv->detail = g_strdup_printf("Prozess beendet (Code %d) – Log: %s",
            sv->proc_status, sv->logfile ? sv->logfile : "?");
        sv->health_timer = 0;
        service_settled(h);
        return;
    }

    if (++sv->tries >= HC_HEALTH_MAX_TRIES) {
        sv->state = HC_SVC_FAILED;
        g_free(sv->detail);
        sv->detail = g_strdup_printf("Port %d antwortet nicht – Log: %s",
                                     sv->port, sv->logfile ? sv->logfile : "?");
        sv->health_timer = 0;
        service_settled(h);
        return;
    }
    sv->health_timer = g_timeout_add(HC_HEALTH_INTERVAL_MS, health_tick, h);
}

static gboolean health_tick(gpointer ud)
{
    HealthCtx *h = ud;
    App *app = h->app;
    HcService *sv = &app->services[h->kind];
    sv->health_timer = 0;

    GSocketClient *cl = g_socket_client_new();
    g_socket_client_set_timeout(cl, 2);
    char *addr = g_strdup_printf("127.0.0.1:%d", sv->port);
    g_socket_client_connect_to_host_async(cl, addr, sv->port, NULL,
                                          on_health_connect, h);
    g_free(addr);
    return G_SOURCE_REMOVE;
}

/* ============================================================
 * Dienst starten
 * ============================================================ */
static void service_stop_one(App *app, HcServiceKind kind)
{
    HcService *sv = &app->services[kind];
    if (sv->health_timer) { g_source_remove(sv->health_timer); sv->health_timer = 0; }
    if (sv->proc) {
        g_subprocess_force_exit(sv->proc);
        g_clear_object(&sv->proc);
    }
    if (kind == HC_SVC_MAIL) mail_catcher_stop(app);
    /* Bei Docker läuft der Dienst im Container – dort gezielt beenden. */
    if (app->sandbox_backend == HC_SB_DOCKER && app->sandbox_container && sv->cmd) {
        /* Der Container wird beim Projektwechsel ohnehin entfernt;
         * ein pkill hält den Zustand zwischendurch sauber. */
        char *qcmd = g_shell_quote(sv->cmd);
        char *kill = g_strdup_printf("pkill -f %s 2>/dev/null || true", qcmd);
        g_free(qcmd);
        char **argv = sandbox_exec_argv(app, kill);
        g_spawn_sync(NULL, argv, NULL,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL
                     | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, NULL, NULL, NULL, NULL);
        g_strfreev(argv);
        g_free(kill);
    }
    sv->state = HC_SVC_STOPPED;
    sv->tries = 0;
}

/* Startet den Prozess eines Dienstes und beginnt den Health-Check. */
static void service_launch(App *app, HcServiceKind kind,
                           HcServicesCb cb, gpointer cb_ud)
{
    HcService *sv = &app->services[kind];
    HealthCtx *h = g_new0(HealthCtx, 1);
    h->app = app; h->kind = kind; h->cb = cb; h->cb_ud = cb_ud;

    sv->tries = 0;
    sv->state = HC_SVC_STARTING;
    sv->proc_exited = FALSE;
    sv->proc_status = 0;
    g_free(sv->detail);
    sv->detail = NULL;
    services_panel_refresh(app);

    /* Der Mail-Catcher ist IDE-intern, kein externer Prozess. */
    if (kind == HC_SVC_MAIL) {
        char *err = NULL;
        if (mail_catcher_start(app, &err)) {
            sv->state = HC_SVC_RUNNING;
            sv->detail = g_strdup_printf("SMTP auf Port %d", app->mail_port);
        } else {
            sv->state = HC_SVC_FAILED;
            sv->detail = err ? err : g_strdup("Start fehlgeschlagen");
        }
        service_settled(h);
        return;
    }

    if (!sv->cmd) {
        sv->state = HC_SVC_SKIPPED;
        service_settled(h);
        return;
    }

    /* Ausgabe in eine Logdatei, damit Fehler nachlesbar sind
     * (und im Panel referenziert werden können). */
    g_free(sv->logfile);
    char *logdir = hc_dir(app, "logs");
    sv->logfile = g_build_filename(logdir,
        kind == HC_SVC_WEB ? "web.log" :
        kind == HC_SVC_DB  ? "db.log"  : "emulator.log", NULL);
    g_free(logdir);

    char *qlog = g_shell_quote(sv->logfile);
    char *full = g_strdup_printf("%s >>%s 2>&1", sv->cmd, qlog);
    g_free(qlog);
    char **argv = sandbox_exec_argv(app, full);
    g_free(full);

    GError *e = NULL;
    sv->proc = g_subprocess_newv((const char * const *)argv,
                                 G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                                 | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &e);
    g_strfreev(argv);

    if (!sv->proc) {
        sv->state = HC_SVC_FAILED;
        sv->detail = g_strdup(e ? e->message : "Start fehlgeschlagen");
        g_clear_error(&e);
        service_settled(h);
        return;
    }
    /* Prozessende beobachten (eigene Referenz fuer den Callback). */
    g_subprocess_wait_async(g_object_ref(sv->proc), NULL, on_service_exit, sv);

    /* Health-Check beginnt nach kurzer Anlaufzeit. */
    sv->health_timer = g_timeout_add(400, health_tick, h);
}

/* ============================================================
 * Kommandos je Dienst zusammenbauen
 * ============================================================ */
static void build_web_cmd(App *app)
{
    HcService *sv = &app->services[HC_SVC_WEB];
    g_clear_pointer(&sv->cmd, g_free);
    sv->port = app->web_port;
    sv->required = TRUE;

    if (!services_project_is_web(app)) {
        sv->state = HC_SVC_SKIPPED;
        sv->required = FALSE;
        return;
    }
    /* Dokumentwurzel: public/ oder web/, sonst Projektwurzel. */
    const char *root = ".";
    if (app->proj_root) {
        char *p = g_build_filename(app->proj_root, "public", NULL);
        if (g_file_test(p, G_FILE_TEST_IS_DIR)) root = "public";
        g_free(p);
        if (!g_strcmp0(root, ".")) {
            p = g_build_filename(app->proj_root, "web", NULL);
            if (g_file_test(p, G_FILE_TEST_IS_DIR)) root = "web";
            g_free(p);
        }
    }

    if (!g_strcmp0(app->proj_lang, "PHP")) {
        char *ini = write_php_support_files(app);
        char *qini = g_shell_quote(ini);
        char *db = db_name_for(app);
        /* Zugangsdaten als Umgebungsvariablen: der Port wird pro Sitzung
         * frei gewaehlt, darf also nicht im Projektcode festgenagelt
         * werden. Das Template liest sie mit getenv(). */
        sv->cmd = g_strdup_printf(
            "HC_DB_HOST=127.0.0.1 HC_DB_PORT=%d HC_DB_USER=root HC_DB_PASS= "
            "HC_DB_NAME=%s HC_MAIL_PORT=%d "
            "exec php -c %s -S %s:%d -t %s",
            app->db_port, db, app->mail_port,
            qini, bind_addr(app), app->web_port, root);
        g_free(db);
        g_free(qini);
        g_free(ini);
    } else {
        /* Statische Web-Oberfläche (auch für C-Projekte mit Web-UI). */
        sv->cmd = g_strdup_printf("exec python3 -m http.server %d --bind %s --directory %s",
            app->web_port, bind_addr(app), root);
    }
}

static void build_db_cmd(App *app)
{
    HcService *sv = &app->services[HC_SVC_DB];
    g_clear_pointer(&sv->cmd, g_free);
    sv->port = app->db_port;

    if (!services_project_wants_db(app)) {
        sv->state = HC_SVC_SKIPPED;
        sv->required = FALSE;
        return;
    }
    sv->required = TRUE;

    /* Eigenes datadir je Projekt -> keine globale DB-Installation nötig,
     * das Projekt bleibt in sich geschlossen und wegwerfbar. */
    char *data_host = hc_dir(app, "mysql");
    g_free(data_host);
    char *data = g_build_filename(sandbox_project_path(app),
                                  ".hypercore", "mysql", NULL);

    /* Socket und PID-Datei NICHT ins Projekt legen: Unix-Socket-Pfade
     * sind auf 107 Zeichen begrenzt, und ein tief verschachtelter
     * Projektpfad sprengt das Limit (mariadbd bricht dann ab). Deshalb
     * ein kurzer, aus dem Projektpfad abgeleiteter Name unter /tmp. */
    char *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA256,
                                              app->proj_root, -1);
    char *sock = g_strdup_printf("/tmp/hcs-%.10s.sock", sum);
    char *pid  = g_strdup_printf("/tmp/hcs-%.10s.pid", sum);
    g_free(sum);

    /* Erststart: datadir initialisieren. Danach Server starten.
     * --user: mariadbd verweigert den Start als root ohne diese Angabe. */
    char *qdata = g_shell_quote(data);
    char *qsock = g_shell_quote(sock);
    char *qpid  = g_shell_quote(pid);
    sv->cmd = g_strdup_printf(
        "set -e; "
        "if [ ! -d %s/mysql ]; then "
        "  (mariadb-install-db --datadir=%s --user=\"$(id -un)\" "
        "     --auth-root-authentication-method=normal "
        "   || mysql_install_db --datadir=%s --user=\"$(id -un)\") >/dev/null; "
        "fi; "
        "exec mariadbd --datadir=%s --socket=%s --pid-file=%s "
        "  --port=%d --bind-address=%s --user=\"$(id -un)\" "
        "  --skip-name-resolve",
        qdata, qdata, qdata, qdata, qsock, qpid,
        app->db_port, bind_addr(app));
    g_free(qdata); g_free(qsock); g_free(qpid);

    g_free(data); g_free(sock); g_free(pid);
}

static void build_emu_cmd(App *app)
{
    HcService *sv = &app->services[HC_SVC_EMULATOR];
    g_clear_pointer(&sv->cmd, g_free);
    sv->port = app->broadway_port;

    if (!services_project_is_native_gui(app)) {
        sv->state = HC_SVC_SKIPPED;
        sv->required = FALSE;
        return;
    }
    /* Der Emulator ist kein Blocker: ohne ihn läuft die App nativ weiter. */
    sv->required = FALSE;

    /* Auf Debian/Ubuntu heißt der GTK4-Broadway-Server gtk4-broadwayd.
     * Älteres GTK3 bringt broadwayd mit – beides zulassen. */
    sv->cmd = g_strdup_printf(
        "if command -v gtk4-broadwayd >/dev/null 2>&1; then "
        "  exec gtk4-broadwayd :%d; "
        "elif command -v broadwayd >/dev/null 2>&1; then "
        "  exec broadwayd :%d; "
        "else echo 'gtk4-broadwayd nicht installiert' >&2; exit 127; fi",
        app->broadway_display, app->broadway_display);
}

static void build_mail_cmd(App *app)
{
    HcService *sv = &app->services[HC_SVC_MAIL];
    sv->port = app->mail_port;
    sv->required = FALSE;
    /* Mail-Catcher nur für Webprojekte sinnvoll. */
    if (!services_project_is_web(app)) {
        sv->state = HC_SVC_SKIPPED;
        return;
    }
    g_clear_pointer(&sv->cmd, g_free);
    sv->cmd = g_strdup("(intern)");
}

/* ============================================================
 * Nach dem DB-Start: Projekt-Datenbank anlegen
 * ============================================================ */
static void db_create_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    App *app = ud;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, NULL);
    g_object_unref(src);
    HcService *sv = &app->services[HC_SVC_DB];
    if (sv->state == HC_SVC_RUNNING) {
        char *db = db_name_for(app);
        g_free(sv->detail);
        sv->detail = g_strdup_printf("Port %d · Datenbank '%s' · root ohne Passwort",
                                     sv->port, db);
        char *m = g_strdup_printf(
            "\x1b[96m[DB] bereit: mysql://root@127.0.0.1:%d/%s (ohne Passwort)\x1b[0m",
            sv->port, db);
        terminal_feed_line(app, m);
        g_free(m);
        g_free(db);
        services_panel_refresh(app);
    }
}

static void db_create_database(App *app)
{
    char *db = db_name_for(app);
    char *cmd = g_strdup_printf(
        "mariadb --protocol=TCP -h 127.0.0.1 -P %d -u root "
        "-e \"CREATE DATABASE IF NOT EXISTS \\`%s\\` "
        "CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;\" 2>/dev/null "
        "|| mysql --protocol=TCP -h 127.0.0.1 -P %d -u root "
        "-e \"CREATE DATABASE IF NOT EXISTS \\`%s\\`;\" 2>/dev/null || true",
        app->db_port, db, app->db_port, db);
    char **argv = sandbox_exec_argv(app, cmd);
    GSubprocess *p = g_subprocess_newv((const char * const *)argv,
                                       G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                                       | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    g_strfreev(argv);
    g_free(cmd); g_free(db);
    if (p) g_subprocess_wait_async(p, NULL, db_create_done, app);
}

/* ============================================================
 * Sammelstart: Dienste der Reihe nach hochfahren
 * ============================================================ */
typedef struct { HcServicesCb cb; gpointer ud; int next; } StartSeq;
static StartSeq s_seq;

static void start_next_or_done(App *app, HcServicesCb cb, gpointer ud)
{
    /* Dienst, der gerade fertig wurde, nachbearbeiten. */
    if (s_seq.next > 0) {
        HcServiceKind prev = (HcServiceKind)(s_seq.next - 1);
        if (prev == HC_SVC_DB && app->services[prev].state == HC_SVC_RUNNING)
            db_create_database(app);
    }

    while (s_seq.next < HC_SVC_KIND_COUNT) {
        HcServiceKind k = (HcServiceKind)s_seq.next++;
        HcService *sv = &app->services[k];
        if (sv->state == HC_SVC_SKIPPED) continue;
        if (app->loading_detail) {
            char *t = g_strdup_printf("Starte %s …", sv->label);
            gtk_label_set_text(GTK_LABEL(app->loading_detail), t);
            g_free(t);
        }
        if (app->loading_progress)
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->loading_progress),
                0.35 + 0.65 * ((double)s_seq.next / HC_SVC_KIND_COUNT));
        service_launch(app, k, cb, ud);
        return;
    }

    /* Alle durch: Bilanz ziehen. */
    gboolean all_ok = TRUE;
    for (int i = 0; i < HC_SVC_KIND_COUNT; i++) {
        HcService *sv = &app->services[i];
        if (sv->required && sv->state != HC_SVC_RUNNING) all_ok = FALSE;
    }
    services_panel_refresh(app);
    if (cb) cb(app, all_ok, ud);
}

void services_start_for_project(App *app, HcServicesCb cb, gpointer ud)
{
    /* Zustand zurücksetzen. */
    for (int i = 0; i < HC_SVC_KIND_COUNT; i++) {
        service_stop_one(app, (HcServiceKind)i);
        app->services[i].state = HC_SVC_STOPPED;
    }
    build_web_cmd(app);
    build_db_cmd(app);
    build_mail_cmd(app);
    build_emu_cmd(app);

    s_seq.cb = cb; s_seq.ud = ud; s_seq.next = 0;
    start_next_or_done(app, cb, ud);
}

void services_restart(App *app, HcServiceKind kind)
{
    if (!app->proj_root) return;
    service_stop_one(app, kind);
    switch (kind) {
        case HC_SVC_WEB:      build_web_cmd(app);  break;
        case HC_SVC_DB:       build_db_cmd(app);   break;
        case HC_SVC_MAIL:     build_mail_cmd(app); break;
        case HC_SVC_EMULATOR: build_emu_cmd(app);  break;
        default: break;
    }
    if (app->services[kind].state == HC_SVC_SKIPPED) {
        services_panel_refresh(app);
        return;
    }
    service_launch(app, kind, NULL, NULL);
}

void services_stop_all(App *app)
{
    for (int i = 0; i < HC_SVC_KIND_COUNT; i++)
        service_stop_one(app, (HcServiceKind)i);
    services_mails_clear(app);
    services_panel_refresh(app);
}

char *services_web_base_url(App *app)
{
    HcService *sv = &app->services[HC_SVC_WEB];
    if (sv->state != HC_SVC_RUNNING) return NULL;
    return g_strdup_printf("http://127.0.0.1:%d", app->web_port);
}

void services_init(App *app)
{
    static const char *labels[HC_SVC_KIND_COUNT] = {
        "Webserver", "Datenbank (MariaDB)", "Mail-Catcher", "GUI-Emulator"
    };
    for (int i = 0; i < HC_SVC_KIND_COUNT; i++) {
        HcService *sv = &app->services[i];
        sv->kind  = (HcServiceKind)i;
        sv->label = labels[i];
        sv->state = HC_SVC_STOPPED;
    }
}
