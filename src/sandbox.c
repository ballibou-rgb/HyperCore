#include "app.h"
#include "version.h"
#include <string.h>

/* ============================================================
 * Sandbox-Schicht + Initialization Cycle (Ladescreen) +
 * Sandbox-Inspektor (Abhängigkeits-Check mit 1-Klick-Install).
 *
 * Backends:
 *   LOCAL : direkte Ausführung auf dem Host (immer verfügbar)
 *   CHROOT: HyperCore-Sandbox-Chroot mit Bind-Mount des Projekts
 *   DOCKER: persistenter Container pro Projekt mit Bind-Mount
 *
 * Wichtig: Die Sandbox wird beim Öffnen eines Projekts EINMAL
 * hochgefahren (sandbox_up_for_project) und bleibt stehen, solange das
 * Projekt offen ist. Nur so können Dienste (Webserver, Datenbank,
 * Mail-Catcher, Broadway) dauerhaft darin laufen. Jeder Build-, Run-
 * und Werkzeug-Aufruf läuft über sandbox_exec_argv() in derselben
 * Sandbox-Instanz.
 * ============================================================ */

#define HC_SANDBOX_CHROOT "/opt/hypercore/sandbox"
#define HC_SANDBOX_IMAGE  "hypercore/sandbox:1"
#define HC_SANDBOX_WORK   "/work"   /* Mountpunkt des Projekts in der Sandbox */

/* ============================================================
 * Backend-Erkennung
 * ============================================================ */

/* Ist das Docker-Image vorhanden und Docker benutzbar? */
static gboolean docker_image_ready(void)
{
    if (!g_find_program_in_path("docker")) return FALSE;
    char *out = NULL;
    int status = 0;
    const char *argv[] = { "docker", "image", "inspect", HC_SANDBOX_IMAGE, NULL };
    gboolean ok = g_spawn_sync(NULL, (char **)argv, NULL,
                               G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                               NULL, NULL, &out, NULL, &status, NULL);
    g_free(out);
    return ok && g_spawn_check_wait_status(status, NULL);
}

void sandbox_init(App *app)
{
    /* Reihenfolge: chroot (stärkste Isolation, von der Paketinstallation
     * angelegt) -> docker (nur wenn das Image wirklich existiert) ->
     * lokal. Es wird NICHT mehr stillschweigend auf LOCAL
     * zurückgefallen: das gewählte Backend wird auch benutzt. */
    if (g_file_test(HC_SANDBOX_CHROOT "/usr/bin", G_FILE_TEST_IS_DIR))
        app->sandbox_backend = HC_SB_CHROOT;
    else if (docker_image_ready())
        app->sandbox_backend = HC_SB_DOCKER;
    else
        app->sandbox_backend = HC_SB_LOCAL;
    app->sandbox_up = FALSE;
}

const char *sandbox_backend_label(App *app)
{
    switch (app->sandbox_backend) {
        case HC_SB_CHROOT: return "Sandbox (chroot)";
        case HC_SB_DOCKER: return "Sandbox (docker)";
        default:           return "Sandbox (lokal)";
    }
}

const char *sandbox_project_path(App *app)
{
    if (app->sandbox_backend == HC_SB_DOCKER) return HC_SANDBOX_WORK;
    /* chroot: das Projekt wird an dieselbe Stelle gemountet, damit Pfade
     * in Compilerausgaben weiterhin stimmen. */
    return app->proj_root ? app->proj_root : ".";
}

/* ============================================================
 * Kommandos in der Sandbox ausführen
 * ============================================================ */

/* Argumentvektor: cmd wird immer über eine Login-Shell im Projekt-
 * verzeichnis der Sandbox ausgeführt. */
char **sandbox_exec_argv(App *app, const char *cmd)
{
    const char *work = sandbox_project_path(app);
    char *qwork = g_shell_quote(work);
    char *inner = g_strdup_printf("cd %s 2>/dev/null; %s", qwork, cmd);
    g_free(qwork);
    GPtrArray *a = g_ptr_array_new();

    switch (app->sandbox_backend) {
        case HC_SB_DOCKER:
            if (app->sandbox_container) {
                g_ptr_array_add(a, g_strdup("docker"));
                g_ptr_array_add(a, g_strdup("exec"));
                g_ptr_array_add(a, g_strdup("-i"));
                g_ptr_array_add(a, g_strdup("-w"));
                g_ptr_array_add(a, g_strdup(HC_SANDBOX_WORK));
                g_ptr_array_add(a, g_strdup(app->sandbox_container));
                g_ptr_array_add(a, g_strdup("/bin/bash"));
                g_ptr_array_add(a, g_strdup("-lc"));
                g_ptr_array_add(a, g_strdup(cmd));
                break;
            }
            /* Container nicht hochgefahren -> lokal ausführen. */
            /* fallthrough */
        case HC_SB_CHROOT:
            if (app->sandbox_backend == HC_SB_CHROOT) {
                g_ptr_array_add(a, g_strdup("pkexec"));
                g_ptr_array_add(a, g_strdup("chroot"));
                g_ptr_array_add(a, g_strdup(HC_SANDBOX_CHROOT));
                g_ptr_array_add(a, g_strdup("/bin/bash"));
                g_ptr_array_add(a, g_strdup("-lc"));
                g_ptr_array_add(a, g_strdup(inner));
                break;
            }
            /* fallthrough */
        default:
            /* Lokal: das cd ins Projektverzeichnis MUSS mit, sonst laufen
             * Dienste wie `php -S -t .` im Verzeichnis der IDE statt im
             * Projekt (Folge: 404 auf jede Datei). */
            g_ptr_array_add(a, g_strdup("/bin/bash"));
            g_ptr_array_add(a, g_strdup("-lc"));
            g_ptr_array_add(a, g_strdup(inner));
            break;
    }
    g_ptr_array_add(a, NULL);
    g_free(inner);
    return (char **)g_ptr_array_free(a, FALSE);
}

/* Rückwärtskompatible String-Variante (wird dort genutzt, wo ein
 * einzelner Shell-String gebraucht wird, z.B. in der Konsole). */
char *sandbox_wrap_command(App *app, const char *cmd)
{
    switch (app->sandbox_backend) {
        case HC_SB_DOCKER:
            if (!app->sandbox_container) return g_strdup(cmd);
            {
                char *q = g_shell_quote(cmd);
                char *r = g_strdup_printf("docker exec -i -w %s %s /bin/bash -lc %s",
                                          HC_SANDBOX_WORK, app->sandbox_container, q);
                g_free(q);
                return r;
            }
        case HC_SB_CHROOT: {
            char *q = g_shell_quote(cmd);
            char *r = g_strdup_printf("pkexec chroot %s /bin/bash -lc %s",
                                      HC_SANDBOX_CHROOT, q);
            g_free(q);
            return r;
        }
        default:
            return g_strdup(cmd);
    }
}

gboolean sandbox_has_tool(App *app, const char *tool)
{
    char *cmd = g_strdup_printf("command -v %s >/dev/null 2>&1", tool);
    char **argv = sandbox_exec_argv(app, cmd);
    int status = 0;
    gboolean ok = g_spawn_sync(NULL, argv, NULL,
                               G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL
                               | G_SPAWN_STDERR_TO_DEV_NULL,
                               NULL, NULL, NULL, NULL, &status, NULL);
    g_strfreev(argv);
    g_free(cmd);
    return ok && g_spawn_check_wait_status(status, NULL);
}

/* ============================================================
 * Sandbox hoch-/herunterfahren
 * ============================================================ */

static char *container_name_for(const char *root)
{
    /* Stabiler, eindeutiger Name pro Projektpfad. */
    char *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, root, -1);
    char *name = g_strdup_printf("hypercore-sbx-%.12s", sum);
    g_free(sum);
    return name;
}

static void docker_rm_quiet(const char *name)
{
    const char *argv[] = { "docker", "rm", "-f", name, NULL };
    g_spawn_sync(NULL, (char **)argv, NULL,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL
                 | G_SPAWN_STDERR_TO_DEV_NULL,
                 NULL, NULL, NULL, NULL, NULL, NULL);
}

gboolean sandbox_up_for_project(App *app)
{
    if (!app->proj_root) return FALSE;
    if (app->sandbox_up) return TRUE;

    /* Ports vorab festlegen: bei DOCKER müssen sie beim Containerstart
     * veröffentlicht werden, damit die IDE (auf dem Host) die Dienste
     * erreichen kann. */
    app->web_port       = hc_find_free_port(8080);
    app->db_port        = hc_find_free_port(13306);
    app->mail_port      = hc_find_free_port(11025);
    /* Broadway legt seinen HTTP-Port fest auf 8080+Display – der Port
     * kann also nicht frei gewählt werden, sondern bestimmt das Display. */
    app->broadway_port    = hc_find_free_port(8085);
    app->broadway_display = app->broadway_port - 8080;

    if (app->sandbox_backend == HC_SB_DOCKER) {
        g_clear_pointer(&app->sandbox_container, g_free);
        app->sandbox_container = container_name_for(app->proj_root);
        docker_rm_quiet(app->sandbox_container);   /* Reste aufräumen */

        char *pweb  = g_strdup_printf("127.0.0.1:%d:%d", app->web_port, app->web_port);
        char *pdb   = g_strdup_printf("127.0.0.1:%d:%d", app->db_port, app->db_port);
        char *pmail = g_strdup_printf("127.0.0.1:%d:%d", app->mail_port, app->mail_port);
        char *pbw   = g_strdup_printf("127.0.0.1:%d:%d", app->broadway_port, app->broadway_port);
        char *mount = g_strdup_printf("%s:%s", app->proj_root, HC_SANDBOX_WORK);

        /* Persistenter Container: schläft, damit docker exec jederzeit
         * Befehle und Dienste darin starten kann (Bind-Mount = Tresor). */
        const char *argv[] = {
            "docker", "run", "-d", "--rm",
            "--name", app->sandbox_container,
            "-v", mount,
            "-w", HC_SANDBOX_WORK,
            "-p", pweb, "-p", pdb, "-p", pmail, "-p", pbw,
            HC_SANDBOX_IMAGE,
            "sleep", "infinity", NULL
        };
        int status = 0;
        gboolean ok = g_spawn_sync(NULL, (char **)argv, NULL,
                                   G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL
                                   | G_SPAWN_STDERR_TO_DEV_NULL,
                                   NULL, NULL, NULL, NULL, &status, NULL);
        g_free(pweb); g_free(pdb); g_free(pmail); g_free(pbw); g_free(mount);

        if (!ok || !g_spawn_check_wait_status(status, NULL)) {
            terminal_feed_line(app,
                "\x1b[33m[Sandbox] Docker-Container konnte nicht gestartet werden – "
                "weiter im lokalen Modus.\x1b[0m");
            g_clear_pointer(&app->sandbox_container, g_free);
            app->sandbox_backend = HC_SB_LOCAL;
        }
    }

    app->sandbox_up = TRUE;
    char *m = g_strdup_printf("\x1b[96m[Sandbox] %s bereit (Web:%d  DB:%d  Mail:%d  Emulator:%d)\x1b[0m",
                              sandbox_backend_label(app), app->web_port,
                              app->db_port, app->mail_port, app->broadway_port);
    terminal_feed_line(app, m);
    g_free(m);
    return TRUE;
}

void sandbox_down(App *app)
{
    if (app->sandbox_backend == HC_SB_DOCKER && app->sandbox_container) {
        docker_rm_quiet(app->sandbox_container);
        g_clear_pointer(&app->sandbox_container, g_free);
    }
    app->sandbox_up = FALSE;
}

/* ============================================================
 * Abhängigkeits-Check – dynamisch nach Sprache UND Template,
 * geprüft IN der Sandbox (nicht auf dem Host).
 * ============================================================ */
typedef struct {
    const char *label;
    const char *tool;
    const char *pkg;
    gboolean    required;
} Dep;

static GArray *required_deps(App *app)
{
    GArray *a = g_array_new(FALSE, FALSE, sizeof(Dep));
    const char *lang = app->proj_lang ? app->proj_lang : "";
    Dep d;
    #define ADD(L,T,P,R) do{ d.label=L; d.tool=T; d.pkg=P; d.required=R; g_array_append_val(a,d);}while(0)

    /* --- Toolchain der Sprache (Blocker) --- */
    if (!g_strcmp0(lang,"C"))      ADD("C-Compiler (gcc)","gcc","gcc",TRUE);
    if (!g_strcmp0(lang,"C++"))    ADD("C++-Compiler (g++)","g++","g++",TRUE);
    if (!g_strcmp0(lang,"Python")) ADD("Python 3","python3","python3",TRUE);
    if (!g_strcmp0(lang,"Bash"))   ADD("Bash","bash","bash",TRUE);
    if (!g_strcmp0(lang,"PHP"))    ADD("PHP","php","php-cli",TRUE);

    /* --- Dienste nach Projekttyp (nur was wirklich gebraucht wird) --- */
    if (services_project_is_web(app)) {
        /* Webprojekt: Server ist Pflicht, sonst gibt es keine Live-Ansicht. */
        if (g_strcmp0(lang,"PHP") != 0)
            ADD("Webserver (python3 -m http.server)","python3","python3",TRUE);
        if (services_project_wants_db(app)) {
            ADD("Datenbank-Server (MariaDB)","mariadbd","mariadb-server",TRUE);
            ADD("Datenbank-Client","mariadb","mariadb-client",FALSE);
        }
    }
    if (services_project_is_native_gui(app)) {
        ADD("pkg-config","pkg-config","pkg-config",TRUE);
        /* Der GUI-Emulator braucht den Broadway-Server von GTK4.
         * Auf Debian/Ubuntu heißt das Programm gtk4-broadwayd. */
        ADD("GUI-Emulator (gtk4-broadwayd)","gtk4-broadwayd","libgtk-4-bin",FALSE);
    }
    ADD("make","make","make",FALSE);
    #undef ADD
    return a;
}

/* ============================================================
 * Ladescreen-Overlay
 * ============================================================ */
void sandbox_build_overlay(App *app)
{
    app->loading_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_add_css_class(app->loading_box, "hc-loading");
    gtk_widget_set_halign(app->loading_box, GTK_ALIGN_FILL);
    gtk_widget_set_valign(app->loading_box, GTK_ALIGN_FILL);

    GtkWidget *center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_halign(center, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(center, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(center, TRUE);

    GtkWidget *logo = gtk_image_new_from_resource("/ovh/datanet/hypercore/icons/app-icon.png");
    gtk_image_set_pixel_size(GTK_IMAGE(logo), 72);
    gtk_box_append(GTK_BOX(center), logo);

    app->loading_title = gtk_label_new("Waking up Sandbox…");
    gtk_widget_add_css_class(app->loading_title, "hc-loading-title");
    gtk_box_append(GTK_BOX(center), app->loading_title);

    app->loading_detail = gtk_label_new("Prüfe Abhängigkeiten…");
    gtk_widget_add_css_class(app->loading_detail, "hc-loading-detail");
    gtk_label_set_justify(GTK_LABEL(app->loading_detail), GTK_JUSTIFY_CENTER);
    gtk_label_set_wrap(GTK_LABEL(app->loading_detail), TRUE);
    gtk_box_append(GTK_BOX(center), app->loading_detail);

    app->loading_progress = gtk_progress_bar_new();
    gtk_widget_set_size_request(app->loading_progress, 300, -1);
    gtk_box_append(GTK_BOX(center), app->loading_progress);

    app->loading_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(app->loading_actions, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(center), app->loading_actions);

    gtk_box_append(GTK_BOX(app->loading_box), center);
    gtk_widget_set_visible(app->loading_box, FALSE);
}

static void clear_actions(App *app)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(app->loading_actions)))
        gtk_box_remove(GTK_BOX(app->loading_actions), c);
}

/* Pulsierende Progressbar. */
static gboolean pulse_cb(gpointer ud)
{
    App *app = ud;
    if (!gtk_widget_get_visible(app->loading_box)) return G_SOURCE_REMOVE;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(app->loading_progress));
    return G_SOURCE_CONTINUE;
}

static void inspector_run_check(App *app, gboolean manual);

/* ============================================================
 * Phase 2: Sandbox hochfahren + Dienste starten
 * ============================================================ */

/* "Trotzdem fortfahren", nachdem ein Dienst nicht startete. */
static void on_continue_after_fail(GtkButton *b, gpointer ud)
{
    (void)b;
    App *app = ud;
    gtk_widget_set_visible(app->loading_box, FALSE);
    project_finish_open(app);
}

/* Letzter Schritt: Ladescreen aus, Projektdateien öffnen. */
static void services_ready(App *app, gboolean all_ok, gpointer ud)
{
    (void)ud;
    if (!all_ok) {
        /* Ein Pflichtdienst fehlt: Ladescreen bleibt als Inspektor stehen. */
        gtk_label_set_text(GTK_LABEL(app->loading_title),
                           "Sandbox-Inspektor: Dienst konnte nicht starten");
        GString *s = g_string_new("");
        for (int i = 0; i < HC_SVC_KIND_COUNT; i++) {
            HcService *sv = &app->services[i];
            if (sv->state == HC_SVC_SKIPPED) continue;
            const char *dot = sv->state == HC_SVC_RUNNING ? "🟢"
                            : sv->state == HC_SVC_FAILED  ? "🔴" : "🟡";
            g_string_append_printf(s, "%s  %s — %s\n", dot, sv->label,
                                   sv->detail ? sv->detail : services_state_label(sv->state));
        }
        gtk_label_set_text(GTK_LABEL(app->loading_detail), s->str);
        g_string_free(s, TRUE);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->loading_progress), 0.0);
        clear_actions(app);

        GtkWidget *again = gtk_button_new_with_label("Erneut versuchen");
        g_signal_connect_swapped(again, "clicked", G_CALLBACK(inspector_open_for_project), app);
        gtk_box_append(GTK_BOX(app->loading_actions), again);
        GtkWidget *cont = gtk_button_new_with_label("Trotzdem fortfahren");
        g_signal_connect(cont, "clicked", G_CALLBACK(on_continue_after_fail), app);
        gtk_box_append(GTK_BOX(app->loading_actions), cont);
        return;
    }

    gtk_label_set_text(GTK_LABEL(app->loading_title), "Sandbox bereit");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->loading_progress), 1.0);
    gtk_widget_set_visible(app->loading_box, FALSE);
    project_finish_open(app);
}

static void start_sandbox_and_services(gpointer ud)
{
    App *app = ud;
    gtk_label_set_text(GTK_LABEL(app->loading_title), "Starte Sandbox…");
    gtk_label_set_text(GTK_LABEL(app->loading_detail), sandbox_backend_label(app));

    sandbox_up_for_project(app);

    gtk_label_set_text(GTK_LABEL(app->loading_title), "Starte Dienste…");
    services_start_for_project(app, services_ready, NULL);
}

/* ============================================================
 * Phase 1: Abhängigkeits-Check
 * ============================================================ */
typedef struct { App *app; char *cmd; } InstallJob;

static void install_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    InstallJob *j = ud;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, NULL);
    g_object_unref(src);
    inspector_run_check(j->app, FALSE);   /* nach Installation erneut prüfen */
    g_free(j->cmd); g_free(j);
}

static void on_install_clicked(GtkButton *b, gpointer ud)
{
    InstallJob *j = ud;
    (void)b;
    gtk_label_set_text(GTK_LABEL(j->app->loading_detail), "Installiere Pakete…");
    clear_actions(j->app);
    /* Installation läuft IN der Sandbox (dort fehlen die Pakete). */
    char **argv = sandbox_exec_argv(j->app, j->cmd);
    GError *e = NULL;
    GSubprocess *p = g_subprocess_newv((const char * const *)argv,
                                       G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                                       | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &e);
    g_strfreev(argv);
    if (!p) {
        gtk_label_set_text(GTK_LABEL(j->app->loading_detail),
            "Installation konnte nicht gestartet werden.");
        g_clear_error(&e);
        return;
    }
    g_subprocess_wait_async(p, NULL, install_done, j);
}

static void on_continue_anyway(GtkButton *b, gpointer ud)
{
    (void)b;
    App *app = ud;
    /* Trotz fehlender Pakete weiter: Sandbox + Dienste versuchen. */
    start_sandbox_and_services(app);
}

static void on_close_status(GtkButton *b, gpointer ud)
{
    (void)b;
    App *app = ud;
    gtk_widget_set_visible(app->loading_box, FALSE);
}

typedef struct { App *app; gboolean manual; } CheckCtx;

static void do_check(gpointer ud)
{
    CheckCtx *cc = ud;
    App *app = cc->app;
    gboolean manual = cc->manual;
    g_free(cc);

    GArray *deps = required_deps(app);
    GString *detail = g_string_new("");
    GPtrArray *missing_pkgs = g_ptr_array_new_with_free_func(g_free);
    int blockers = 0;

    g_string_append_printf(detail, "%s\n\n", sandbox_backend_label(app));

    for (guint i = 0; i < deps->len; i++) {
        Dep *d = &g_array_index(deps, Dep, i);
        gboolean ok = sandbox_has_tool(app, d->tool);
        g_string_append_printf(detail, "%s  %s\n",
            ok ? "🟢" : (d->required ? "🔴" : "🟡"), d->label);
        if (!ok) {
            gboolean dup = FALSE;
            for (guint k = 0; k < missing_pkgs->len; k++)
                if (!g_strcmp0(missing_pkgs->pdata[k], d->pkg)) dup = TRUE;
            if (!dup) g_ptr_array_add(missing_pkgs, g_strdup(d->pkg));
            if (d->required) blockers++;
        }
    }
    g_array_free(deps, TRUE);

    gtk_label_set_text(GTK_LABEL(app->loading_detail), detail->str);
    g_string_free(detail, TRUE);
    clear_actions(app);

    if (manual) {
        /* Reiner Statusaufruf aus der Toolbar: nichts starten. */
        gtk_label_set_text(GTK_LABEL(app->loading_title),
            blockers ? "Sandbox-Inspektor: Abhängigkeiten fehlen" : "Sandbox bereit");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->loading_progress),
                                      blockers ? 0.0 : 1.0);
        if (blockers > 0 && missing_pkgs->len > 0) {
            GString *pkgs = g_string_new("");
            for (guint i = 0; i < missing_pkgs->len; i++)
                g_string_append_printf(pkgs, "%s ", (char *)missing_pkgs->pdata[i]);
            char *cmd = g_strdup_printf("apt-get install -y %s", pkgs->str);
            g_string_free(pkgs, TRUE);
            GtkWidget *inst = gtk_button_new_with_label("Sandbox vervollständigen (1 Klick)");
            gtk_widget_add_css_class(inst, "hc-install-btn");
            InstallJob *j = g_new0(InstallJob, 1);
            j->app = app; j->cmd = cmd;
            g_signal_connect(inst, "clicked", G_CALLBACK(on_install_clicked), j);
            gtk_box_append(GTK_BOX(app->loading_actions), inst);
        }
        GtkWidget *cl = gtk_button_new_with_label("Schließen");
        g_signal_connect(cl, "clicked", G_CALLBACK(on_close_status), app);
        gtk_box_append(GTK_BOX(app->loading_actions), cl);
        g_ptr_array_free(missing_pkgs, TRUE);
        return;
    }

    if (blockers > 0) {
        /* ROT: Blocker – zum Inspektor werden, Install anbieten. */
        gtk_label_set_text(GTK_LABEL(app->loading_title),
                           "Sandbox-Inspektor: Abhängigkeiten fehlen");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->loading_progress), 0.0);

        GString *pkgs = g_string_new("");
        for (guint i = 0; i < missing_pkgs->len; i++)
            g_string_append_printf(pkgs, "%s ", (char *)missing_pkgs->pdata[i]);
        char *cmd = g_strdup_printf("apt-get install -y %s", pkgs->str);
        g_string_free(pkgs, TRUE);

        GtkWidget *inst = gtk_button_new_with_label("Sandbox vervollständigen (1 Klick)");
        gtk_widget_add_css_class(inst, "hc-install-btn");
        InstallJob *j = g_new0(InstallJob, 1);
        j->app = app; j->cmd = cmd;
        g_signal_connect(inst, "clicked", G_CALLBACK(on_install_clicked), j);
        gtk_box_append(GTK_BOX(app->loading_actions), inst);

        GtkWidget *cont = gtk_button_new_with_label("Trotzdem fortfahren");
        g_signal_connect(cont, "clicked", G_CALLBACK(on_continue_anyway), app);
        gtk_box_append(GTK_BOX(app->loading_actions), cont);
    } else {
        /* GRÜN: weiter zu Sandbox + Diensten. */
        gtk_label_set_text(GTK_LABEL(app->loading_title), "Abhängigkeiten vollständig");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->loading_progress), 0.35);
        g_timeout_add_once(300, start_sandbox_and_services, app);
    }
    g_ptr_array_free(missing_pkgs, TRUE);
}

static void inspector_run_check(App *app, gboolean manual)
{
    gtk_widget_set_visible(app->loading_box, TRUE);
    gtk_label_set_text(GTK_LABEL(app->loading_title), "Waking up Sandbox…");
    gtk_label_set_text(GTK_LABEL(app->loading_detail), "Prüfe Abhängigkeiten…");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app->loading_progress), 0.0);
    clear_actions(app);
    g_timeout_add(90, pulse_cb, app);
    CheckCtx *cc = g_new0(CheckCtx, 1);
    cc->app = app; cc->manual = manual;
    g_timeout_add_once(400, do_check, cc);
}

void inspector_open_for_project(App *app)
{
    inspector_run_check(app, FALSE);
}

void inspector_show_status(App *app)
{
    inspector_run_check(app, TRUE);
}
