#ifndef HYPERCORE_APP_H
#define HYPERCORE_APP_H

#include <gtk/gtk.h>

/* ============================================================
 * HyperCore Studio – zentrale Zustands-Strukturen.
 *
 * Die gesamte Anwendung teilt sich eine einzige App-Struktur, die an
 * alle Module durchgereicht wird. Jedes Feature (Editor, Terminal,
 * Projekt, Agent, ...) liegt in einer eigenen .c-Datei, deren
 * öffentliche Funktionen hier deklariert sind.
 * ============================================================ */

typedef enum {
    HC_THEME_DARK = 0,
    HC_THEME_LIGHT,
    HC_THEME_SYSTEM
} HcTheme;

typedef struct {
    char   *font_family;   /* z.B. "Monospace"                      */
    int     font_size;     /* Punktgröße                            */
    int     tab_width;     /* Tab-Weite in Leerzeichen              */
    gboolean spaces_for_tab;
    HcTheme theme;

    /* KI-Agent-Verbindung (siehe Einstellungen -> Reiter "Agent"). */
    char   *agent_host;    /* IP / Hostname des Modell-Servers      */
    int     agent_port;    /* Port                                  */
    char   *agent_key;     /* API-Key (Bearer)                      */
    char   *agent_model;   /* Modellname                            */
    char   *embed_model;   /* Embedding-Modell (RAG, z.B. nomic-embed-text) */
    gboolean ghost_enabled;/* Ghost-Text-Autovervollständigung an?  */
    gboolean autosave_enabled; /* Live-Speichern waehrend des Tippens   */
    int     persona;       /* 0=Architekt 1=Planer 2=Coder          */
    int     agent_timeout_sec; /* Request-Timeout in Sekunden (Ollama kann langsam sein) */
    int     agent_max_tokens;  /* max_tokens pro Request (0 = automatisch je Persona)   */
    double  agent_temperature; /* Kreativität 0.0 (deterministisch) … 1.0 (kreativ)     */
    int     agent_ctx_window;  /* Wie viele Nachrichten im Verlauf gesendet werden       */

    /* ---- Datenbank (MariaDB/MySQL pro Projekt) ---- */
    char   *db_root_pass;  /* Root-Passwort (leer = kein Passwort)          */
    char   *db_extra_user; /* Zusaetzlicher DB-User (leer = keiner)         */
    char   *db_extra_pass; /* Passwort fuer extra User                      */
    gboolean db_phpmyadmin;/* phpMyAdmin im Browser oeffnen wenn DB laeuft  */

    /* ---- Git-Remotes ---- */
    char   *github_token;  /* Personal Access Token fuer GitHub             */
    char   *github_user;   /* GitHub-Username                               */
    char   *gitea_url;     /* Gitea-Basis-URL (z.B. https://git.example.com)*/
    char   *gitea_token;   /* Gitea API-Token                               */
    char   *gitea_user;    /* Gitea-Username                                */

    /* Global Identity (Entwickler-Basisdaten) */
    char   *dev_name;
    char   *dev_company;
    char   *dev_email;
    char   *dev_website;
    char   *dev_license;

    /* Strict Compliance Mode */
    gboolean compliance_enabled;
    char   *copyright_header; /* Vorlage für Datei-Header            */
} Settings;

typedef enum {
    HC_SB_LOCAL = 0,  /* direkt auf dem Host                        */
    HC_SB_CHROOT,     /* HyperCore-Sandbox-Chroot                   */
    HC_SB_DOCKER      /* Docker-Container (persistent pro Projekt)  */
} HcSandboxBackend;

/* ============================================================
 * Dienste ("Maschinenraum"): werden beim Öffnen eines Projekts
 * automatisch gestartet und beim Schließen beendet.
 * ============================================================ */
typedef enum {
    HC_SVC_STOPPED = 0,
    HC_SVC_STARTING,   /* Prozess läuft, Health-Check offen         */
    HC_SVC_RUNNING,    /* Health-Check bestanden                    */
    HC_SVC_FAILED,     /* Start oder Health-Check fehlgeschlagen    */
    HC_SVC_SKIPPED     /* für dieses Projekt nicht benötigt         */
} HcServiceState;

typedef enum {
    HC_SVC_WEB = 0,    /* PHP-/Static-Webserver                     */
    HC_SVC_DB,         /* MariaDB/MySQL (eigenes datadir je Projekt)*/
    HC_SVC_MAIL,       /* SMTP-Catcher (IDE-intern)                 */
    HC_SVC_EMULATOR,   /* gtk4-broadwayd (nativer GUI-Stream)       */
    HC_SVC_KIND_COUNT
} HcServiceKind;

typedef struct {
    HcServiceKind   kind;
    const char     *label;
    HcServiceState  state;
    char           *cmd;        /* Startkommando (in der Sandbox)   */
    int             port;       /* Health-Check-Port (0 = keiner)   */
    char           *detail;     /* Status-/Fehlertext für die UI    */
    GSubprocess    *proc;       /* laufender Prozess (oder NULL)    */
    gboolean        proc_exited; /* von wait_async gesetzt           */
    int             proc_status; /* Exit-Code, wenn proc_exited      */
    char           *logfile;    /* Logdatei des Dienstes            */
    guint           health_timer;
    int             tries;      /* Health-Check-Versuche            */
    gboolean        required;    /* Blocker, wenn er nicht startet   */

    /* Zeile im Dienste-Panel. */
    GtkWidget      *row_dot;
    GtkWidget      *row_detail;
} HcService;

/* Eine abgefangene E-Mail (Mail-Catcher). */
typedef struct {
    char *from;
    char *to;
    char *subject;
    char *body;
    char *received;   /* Zeitstempel                                */
} HcMail;

/* ============================================================
 * Custom Agent-Werkzeuge (aus ~/.config/hypercore/tools/ - .json-Dateien).
 * ============================================================ */
typedef struct {
    char    *name;         /* Parametername                        */
    char    *description;  /* Beschreibung für das LLM             */
    gboolean optional;     /* nicht im required-Array              */
} HcCustomToolArg;

typedef struct {
    char  *name;           /* Werkzeugname (wird dem LLM gemeldet) */
    char  *description;    /* Was das Werkzeug tut                 */
    char  *command;        /* Shell-Vorlage, {arg} wird ersetzt    */
    GList *args;           /* HcCustomToolArg*, kann leer sein     */
    char  *filename;       /* Quelldatei (für Editor-Button)       */
} HcCustomTool;

/* RAG-Chunk (ein indiziertes Stück einer Datei). */
typedef struct {
    char   *file;      /* relativer Pfad                            */
    int     idx;       /* Chunk-Nummer                              */
    char   *text;      /* Chunk-Text                                */
    float  *vec;       /* Embedding-Vektor                          */
    int     dim;       /* Vektordimension                           */
} HcRagChunk;

typedef struct _App App;

/* Ein offener Editor-Tab. */
typedef struct {
    App           *app;
    char          *path;        /* absoluter Pfad oder NULL (unbenannt) */
    GtkWidget     *page;        /* Wurzel-Widget der Seite (im Notebook)*/
    GtkWidget     *view;        /* GtkSourceView                        */
    GtkTextBuffer *buffer;      /* GtkSourceBuffer                      */
    GtkWidget     *tab_title;   /* Label im Reiter                      */
    gboolean       modified;
    gulong         changed_handler;

    /* Live-Preview (Split-Tab), falls aktiv. */
    GtkWidget     *preview;     /* WebKitWebView oder GTK-Vorschau, NULL */
    GtkWidget     *paned;       /* Split-Container oder NULL            */

    /* Live-Betrieb: entprellte Timer fuer Vorschau/Pruefung und Auto-Save. */
    guint          live_timer;   /* Vorschau + Syntaxpruefung aus dem Puffer */
    guint          save_timer;   /* Auto-Save nach dem Tippen                */

    /* Ghost-Text-Autovervollständigung. */
    guint          ghost_timer;
    gboolean       ghost_active;
    int            ghost_len;   /* Zeichenlänge des eingefügten Vorschlags */
    gboolean       ghost_ignore_change; /* Reentrancy-Schutz           */
} EditorTab;

struct _App {
    GtkApplication *app;
    GtkWidget      *window;
    Settings        settings;
    GtkCssProvider *css_provider;

    /* ---- aktuelles Projekt ---- */
    char           *proj_root;   /* absoluter Projektpfad oder NULL     */
    char           *proj_name;
    char           *proj_lang;   /* "C", "C++", "Python", "Bash", "PHP" */
    char           *proj_template;
    char           *proj_build;  /* Build-Kommando (Hardcoded-Toolchain)*/
    char           *proj_run;    /* Run-Kommando                        */

    /* ---- Hauptlayout ---- */
    GtkWidget      *tree_view;      /* Projekt-Baum (GtkListView)        */
    GtkWidget      *notebook;       /* Editor-Reiter                     */
    GtkWidget      *placeholder;    /* Platzhalter bei 0 Tabs            */
    GtkWidget      *notebook_stack; /* wechselt Platzhalter <-> Notebook */

    /* ---- Terminal (unten) ---- */
    GtkWidget      *term;           /* VTE-Terminal                      */
    GtkWidget      *term_revealer;  /* ein-/ausklappbar                  */
    GtkWidget      *problems_bar;   /* Leiste mit klickbaren Fehlern     */
    GList          *problems;       /* HcProblem*                        */

    /* ---- rechte Sidebar ---- */
    GtkWidget      *right_revealer;
    GtkWidget      *right_stack;     /* "outline" / "agent"              */
    GtkWidget      *outline_list;    /* GtkListBox                       */
    GtkWidget      *agent_log;       /* Chat-Verlauf (Box)               */
    GtkWidget      *agent_log_scroll;
    GtkWidget      *agent_entry;
    GtkWidget      *agent_auto_toggle;

    /* ---- Statusleiste ---- */
    GtkWidget      *st_project;
    GtkWidget      *st_fileinfo;
    GtkWidget      *st_activity;  /* Live-Aktivitäts-Label (Mitte)           */
    GtkWidget      *st_spinner;   /* GtkSpinner neben dem Activity-Label     */
    GtkWidget      *st_version;

    GList          *tabs;            /* EditorTab*                        */
    GList          *agent_history;   /* HcChatTurn* (Kontext)             */
    gboolean        agent_busy;

    /* ---- v2: Sandbox / Inspektor / Ladescreen ---- */
    GtkWidget      *overlay;          /* GtkOverlay über dem Arbeitsbereich */
    GtkWidget      *loading_box;      /* abgedunkeltes Overlay              */
    GtkWidget      *loading_title;
    GtkWidget      *loading_detail;
    GtkWidget      *loading_progress;
    GtkWidget      *loading_actions;  /* Box für Inspektor-Buttons          */
    GtkWidget      *sandbox_status_btn;
    int             sandbox_backend;  /* HcSandboxBackend                   */
    char           *sandbox_container; /* Docker-Containername (oder NULL)  */
    gboolean        sandbox_up;        /* Sandbox läuft und ist nutzbar     */

    /* ---- Dienste ("Maschinenraum") ---- */
    HcService       services[HC_SVC_KIND_COUNT];
    GtkWidget      *svc_list;         /* Box mit einer Zeile je Dienst      */
    GtkWidget      *svc_status_lbl;   /* Kurzstatus in der Titelleiste      */
    int             web_port;         /* aktiver Webserver-Port             */
    int             db_port;          /* aktiver Datenbank-Port             */
    int             mail_port;        /* SMTP-Catcher-Port                  */
    int             broadway_port;    /* Broadway-HTTP-Port                 */
    int             broadway_display; /* Broadway-Displaynummer             */

    /* ---- Mail-Catcher ---- */
    GList          *mails;            /* HcMail*                            */
    GtkWidget      *mail_list;        /* Box im unteren Panel               */
    GSocketService *mail_service;

    /* ---- Emulator-Tab (nativer GUI-Stream) ---- */
    GtkWidget      *emu_page;         /* Seite im Notebook (oder NULL)      */
    GtkWidget      *emu_view;         /* WebKitWebView                      */
    GtkWidget      *emu_status;       /* Statuszeile im Emulator            */
    GSubprocess    *emu_app;          /* laufende Anwendung im Emulator     */
    guint           emu_poll;         /* Health-Poll-Timer                  */
    int             emu_tries;

    /* ---- v2: Agent ---- */
    GtkWidget      *agent_persona_drop;
    GtkWidget      *agent_root_stack; /* "online" (Chat) / "offline"        */
    GtkWidget      *agent_activity;   /* Tool-Transparenz-Zeile             */
    char           *last_assistant;   /* Kurzzeitgedächtnis (Smart Context) */
    /* Laufender Gespraechsverlauf inkl. Werkzeugaufrufe und -ergebnisse.
     * Ohne ihn vergisst der Agent nach jeder Nutzerrunde, was er gerade
     * gelesen oder geaendert hat. JsonNode* (Objekte), owned. */
    GPtrArray      *agent_msgs;

    /* ---- Debug-Modus (nur mit HC_DEBUG=1 Build) ---- */
    GtkWidget      *debug_win;        /* Debug-Fenster (NULL wenn nicht offen) */
    char           *debug_prompt_override; /* Prefix vor System-Prompt (Debug) */

    /* ---- Git Quick-Buttons ---- */
    GtkWidget      *git_push_btn;     /* Toolbar-Schnellbutton Push            */
    GtkWidget      *git_remote_btn;   /* Toolbar-Schnellbutton Remote-Repos    */

    /* ---- Custom Werkzeuge ---- */
    GList          *custom_tools;     /* HcCustomTool* (aus tools/-Verzeichnis) */

    /* ---- v2: RAG ---- */
    GList          *rag_chunks;       /* HcRagChunk*                        */
    gboolean        rag_ready;

    /* ---- v2: Git (libgit2) ---- */
    GtkWidget      *git_branch_lbl;   /* Branch-Anzeige in Titelleiste      */
    GtkWidget      *git_latest_btn;   /* "Zurück zu Latest"                 */
    gboolean        git_readonly;     /* Zeitreise aktiv (Schreibsperre)    */
};

/* Ein erkannter, anklickbarer Compiler-Fehler. */
typedef struct {
    char *file;   /* absoluter Pfad                                      */
    int   line;
    int   col;
    char *msg;
} HcProblem;

/* Ein Chat-Beitrag (Agent-Kontext). */
typedef struct {
    char *role;    /* "user" / "assistant" / "system"                   */
    char *content;
} HcChatTurn;

/* ---------- settings.c ---------- */
void      settings_load(Settings *s);
void      settings_save(const Settings *s);
void      settings_free(Settings *s);
void      settings_open_dialog(App *app);
char     *hc_config_dir(void);

/* ---------- theme.c (in main.c) ---------- */
void      hc_apply_theme(App *app);

/* ---------- project.c ---------- */
void      project_open(App *app, const char *root);
void      project_finish_open(App *app);  /* nach Inspektor: Dateien öffnen */
gboolean  project_dir_has_ght(const char *dir);
char     *project_last_path(void);     /* zuletzt geöffnetes Projekt      */
char     *project_main_file(App *app);  /* Hauptdatei laut .hcs, sonst NULL */
gboolean  project_create_files(const char *root, const char *name,
                               const char *lang, const char *template_name,
                               gboolean want_db, GError **error);
void      project_refresh_tree(App *app);
void      project_build_tree(App *app);
void      project_setup_tree_view(App *app); /* "activate" einmalig verbinden */
void      session_save(App *app);
void      session_restore(App *app);
void      project_close(App *app);

/* ---------- wizard.c ---------- */
void      wizard_run(App *app);

/* ---------- editor.c ---------- */
EditorTab *editor_open_file(App *app, const char *path);
EditorTab *editor_current(App *app);
void       editor_save_current(App *app);
void       editor_save_all(App *app);    /* hart, ohne Nachfrage           */
void       editor_save_tab(EditorTab *t);
void       editor_goto_line(EditorTab *t, int line);
void       editor_jump_to(App *app, const char *file, int line);
void       editor_close_tab(EditorTab *t);
void       editor_update_status(App *app);
void       editor_apply_settings_all(App *app);
void       editor_refresh_outline(App *app);
void       editor_connect_outline(App *app); /* row-activated verbinden */
void       editor_toggle_preview(App *app);
/* Vorschau fuer einen bestimmten Tab ein-/ausschalten. */
void       editor_set_preview(EditorTab *t, gboolean on);
/* Syntaxpruefung direkt auf dem Editor-Puffer (ohne Speichern), damit
 * Fehler live markiert werden. */
void       editor_live_check(EditorTab *t);

/* ---------- search.c ---------- */
void       search_open_dialog(App *app);

/* ---------- terminal.c ---------- */
GtkWidget *terminal_new(App *app);
void       terminal_apply_theme(App *app);
void       terminal_cd(App *app, const char *path); /* Notiz in die Konsole */
void       terminal_send_errors_to_agent(App *app);

/* ---------- sandbox.c (Sandbox + Inspektor + Ladescreen) ---------- */
void       sandbox_init(App *app);
const char*sandbox_backend_label(App *app);
void       sandbox_build_overlay(App *app);       /* Overlay-Widgets anlegen */
void       inspector_open_for_project(App *app);  /* Ladescreen + Dep-Check  */
void       inspector_show_status(App *app);       /* manuell (Toolbar)       */
/* Kommando so umschreiben, dass es in der Sandbox läuft (Backend-abhängig). */
char      *sandbox_wrap_command(App *app, const char *cmd);
/* Sandbox für das aktuelle Projekt hochfahren / herunterfahren.
 * Bei DOCKER wird ein persistenter Container mit Bind-Mount gestartet,
 * damit die Dienste (Webserver, DB, ...) darin dauerhaft laufen. */
gboolean   sandbox_up_for_project(App *app);
void       sandbox_down(App *app);
/* Argumentvektor zum Ausführen von cmd in der Sandbox. Rückgabe muss mit
 * g_strfreev() freigegeben werden. */
char     **sandbox_exec_argv(App *app, const char *cmd);
/* Prüft asynchron, ob ein Werkzeug IN der Sandbox vorhanden ist. */
gboolean   sandbox_has_tool(App *app, const char *tool);
/* Pfad innerhalb der Sandbox, der dem Projektordner entspricht. */
const char*sandbox_project_path(App *app);

/* ---------- services.c (Dienst-Orchestrator) ---------- */
typedef void (*HcServicesCb)(App *app, gboolean all_ok, gpointer ud);
/* Freien TCP-Port ab 'preferred' suchen (für parallele Projekte). */
int        hc_find_free_port(int preferred);
void       services_init(App *app);
GtkWidget *services_panel_new(App *app);
/* Startet alle für das Projekt nötigen Dienste und ruft cb auf, sobald
 * alle entweder laufen, übersprungen oder endgültig fehlgeschlagen sind. */
void       services_start_for_project(App *app, HcServicesCb cb, gpointer ud);
void       services_stop_all(App *app);
void       services_restart(App *app, HcServiceKind kind);
HcService *services_get(App *app, HcServiceKind kind);
void       services_panel_refresh(App *app);
const char*services_state_label(HcServiceState st);
/* Basis-URL des laufenden Webservers, z.B. "http://127.0.0.1:8080" –
 * NULL, wenn kein Webserver läuft. */
char      *services_web_base_url(App *app);
gboolean   services_project_is_web(App *app);
gboolean   services_project_is_native_gui(App *app);
gboolean   services_project_wants_db(App *app);
/* Mail-Catcher */
void       services_mail_panel_refresh(App *app);
void       services_mails_clear(App *app);

/* ---------- rag.c (lokale RAG-Engine, Ollama-Embeddings) ---------- */
typedef void (*HcRagCb)(App *app, char *context, gpointer ud); /* context: owned/NULL */
void       rag_index_project(App *app, gboolean rebuild);
void       rag_update_file(App *app, const char *relpath);
void       rag_remove_file(App *app, const char *relpath);
void       rag_query_async(App *app, const char *query, HcRagCb cb, gpointer ud);
void       rag_manager_open(App *app);
void       rag_clear(App *app);
int        rag_file_chunk_count(App *app, const char *relpath);

/* ---------- workspace / identity (settings.c) ---------- */
char      *hc_workspace_dir(void);   /* ~/HyperCore (angelegt)             */

/* ---------- .hcs Whitelist (project.c) ---------- */
/* Dienst-Schalter aus/in der .hcs ([services] db=true, ...). */
gboolean   project_flag(App *app, const char *key, gboolean def);
void       project_set_flag(App *app, const char *key, gboolean val);
void       project_whitelist_add(App *app, const char *relpath);
void       project_whitelist_remove(App *app, const char *relpath);
GList     *project_whitelist(App *app);   /* Liste aus char* (rel. Pfade)  */

/* ---------- snapshot + emulator (preview.c) ---------- */
void       preview_snapshot_current(App *app);
void       preview_run_broadway(App *app);  /* nativer Emulator (Broadway)  */
gboolean   preview_is_native_project(App *app);
GtkWidget *preview_build_for(EditorTab *t);
void       preview_reload(EditorTab *t);
/* Live-Aktualisierung aus dem Editor-Puffer – OHNE Speichern.
 * GTK .ui wird direkt neu gezeichnet, HTML direkt neu gerendert. */
void       preview_live_update(EditorTab *t);
/* Vorschau automatisch öffnen, wenn der Dateityp eine hat. */
void       preview_auto_open(EditorTab *t);
gboolean   preview_available_for(const char *path);
/* Emulator-Tab zurücksetzen (Projektwechsel). */
void       preview_reset(App *app);

/* ---------- crash-hook (agent.c) ---------- */
void       agent_report_crash(App *app, const char *stderr_text);
const char*persona_name(int persona);
/* Einmalige LLM-Anfrage (ein Request, ohne Werkzeuge) -> Text im Callback. */
typedef void (*HcOneshotCb)(App *app, char *text, gpointer ud);
void       agent_oneshot(App *app, const char *system, const char *user,
                         HcOneshotCb cb, gpointer ud);

/* ---------- git.c (Native Source Control, libgit2) ---------- */
void       git_startup(void);                 /* libgit2 init (einmalig)    */
gboolean   git_ensure_repo(App *app);         /* open/init + Auto-Commit    */
char      *git_branch_label(App *app);         /* z.B. "main" / "detached@ab12" */
gboolean   git_is_detached(App *app);
gboolean   git_commit_all(App *app, const char *message);
gboolean   git_return_latest(App *app);        /* zurück auf Branch-Spitze   */
char      *git_collect_diff(App *app);         /* HEAD->workdir als Text     */
void       git_update_header(App *app);        /* Branch-Anzeige + Buttons   */
void       git_open_timemachine(App *app);     /* Verlauf / Zeitreise        */
void       git_commit_dialog(App *app);        /* Commit (mit KI-Message)    */

/* ---------- tools.c (Agent-Werkzeuge) ---------- */
/* Führt ein lokales Werkzeug aus. args_json ist das JSON-Argumentobjekt
 * als String. Rückgabe: neuer Ergebnis-String (immer != NULL). *ok wird
 * auf FALSE gesetzt, wenn das Werkzeug einen Fehler meldet. */
char      *agent_tools_dispatch(App *app, const char *name,
                                const char *args_json, gboolean *ok);
void       terminal_run_build(App *app);
void       terminal_run_run(App *app);
void       terminal_feed_line(App *app, const char *text);
void       problems_clear(App *app);
void       problems_clear_file(App *app, const char *file);
void       problems_add(App *app, const char *file, int line, int col, const char *msg);
void       problems_rebuild_bar(App *app);

/* ---------- git_remote.c ---------- */
void git_remote_panel_open(App *app);   /* GitHub/Gitea-Panel                */
void git_push_dialog(App *app);         /* Push-Dialog mit Remote-Auswahl    */

/* ---------- debug.c ---------- */
#ifdef HC_DEBUG
void debug_window_open(App *app);       /* Debug-Fenster mit Prompt-Editor   */
#endif

/* ---------- custom_tools.c ---------- */
/* Verzeichnis ~/.config/hypercore/tools/ (wird angelegt). */
char        *hc_tools_dir(void);
/* Alle .json-Dateien im Tools-Verzeichnis laden. */
void         custom_tools_reload(App *app);
void         custom_tools_free(App *app);
/* Ein Custom Tool als OpenAI-Tool-Schema in den JsonBuilder schreiben. */
void         custom_tool_append_schema(HcCustomTool *tool, gpointer builder);
/* Ein Custom Tool ausführen (args_json = JSON-Objekt der Argumente). */
char        *custom_tool_run(App *app, HcCustomTool *tool,
                             const char *args_json, gboolean *ok);
/* Tool nach Name suchen. */
HcCustomTool *custom_tool_find(App *app, const char *name);
/* Einstellungs-Dialog-Seite. */
GtkWidget   *custom_tools_settings_page(App *app);

/* ---------- agent.c ---------- */
GtkWidget *agent_panel_new(App *app);
char      *agent_debug_get_system_prompt(App *app); /* nur fuer debug.c */
void       agent_send_user_message(App *app, const char *text);
void       agent_reset_context(App *app);
void       agent_refresh_availability(App *app); /* Graceful Degradation    */
void       agent_request_ghost(EditorTab *t);
void       agent_cancel_ghost(EditorTab *t);
void       agent_attach_ghost(EditorTab *t); /* Tastatur-Controller (Tab) */

/* ---------- utils (main.c) ---------- */
void       hc_mark_window(App *app, GtkWidget *w);
GtkWidget *hc_make_drag_area(GtkWidget *child, gboolean expand);
GtkWidget *hc_icon(const char *name);                 /* Icon aus GResource */
GtkWidget *hc_icon_button(const char *name, const char *tip);
const char *hc_lang_for_path(const char *path); /* GtkSourceLanguage-ID */

/* Erzeugt ein dekorationsfreies HyperCore-Dialog-Fenster mit eigener
 * Titelleiste. Gibt die innere content-Box zurueck (child wird eingefuegt).
 * Das Fenster wird mit hc_mark_window markiert.
 * title_out (optional) → Zeiger auf das Titel-Label fuer dynamische Updates. */
GtkWidget *hc_make_dialog(App *app, const char *title,
                           int width, int height,
                           GtkWidget **content_box_out);

/* Zentrale Statuszeilen-Funktion: text=NULL löscht den Eintrag.
 * Spinner läuft solange text != NULL und hört auf wenn NULL gesetzt wird.
 * Darf aus allen Modulen aufgerufen werden, läuft immer im Main-Thread. */
void       hc_set_activity(App *app, const char *text);

#endif
