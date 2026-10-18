#include "app.h"
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

/* ============================================================
 * Git-Remote-Panel: GitHub und Gitea Repository-Browser
 * Push, Clone, Quick-Buttons
 * ============================================================ */

/* Maximale Anzahl Repos die per API geladen werden. */
#define REMOTE_MAX_REPOS 50

typedef struct {
    App       *app;
    GtkWidget *win;
    GtkWidget *list_box;
    GtkWidget *status_lbl;
    GtkWidget *spinner;
    GtkWidget *gh_btn;
    GtkWidget *gt_btn;
    int        source;   /* 0=GitHub 1=Gitea */
} RemoteCtx;

typedef struct {
    char *name;     /* owner/repo */
    char *url;      /* clone URL  */
    char *desc;
    gboolean is_private;
} RepoEntry;

static void repo_entry_free(gpointer p)
{
    RepoEntry *r = p;
    g_free(r->name); g_free(r->url); g_free(r->desc); g_free(r);
}

/* ---- Status-Anzeige ---- */
static void set_status(RemoteCtx *ctx, const char *msg, gboolean spinning)
{
    gtk_label_set_text(GTK_LABEL(ctx->status_lbl), msg ? msg : "");
    gtk_widget_set_visible(ctx->status_lbl, msg && *msg);
    if (spinning) gtk_spinner_start(GTK_SPINNER(ctx->spinner));
    else          gtk_spinner_stop(GTK_SPINNER(ctx->spinner));
    gtk_widget_set_visible(ctx->spinner, spinning);
}

/* ---- Repo-Liste rendern ---- */
static void on_clone_clicked(GtkButton *btn, gpointer ud)
{
    App *app = ud;
    const char *url = g_object_get_data(G_OBJECT(btn), "hc-clone-url");
    if (!url || !app->proj_root) return;

    /* Zielverzeichnis = Eltern des aktuellen Projekts + Repo-Name */
    char *base = g_path_get_basename(url);
    /* .git-Suffix entfernen */
    if (g_str_has_suffix(base, ".git")) base[strlen(base)-4] = '\0';
    char *parent = g_path_get_dirname(app->proj_root);
    char *dest = g_build_filename(parent, base, NULL);
    g_free(parent); g_free(base);

    char *cmd = g_strdup_printf("git clone -- %s %s",
                                g_shell_quote(url), g_shell_quote(dest));
    g_free(dest);

    /* Im Terminal ausfuehren */
    terminal_feed_line(app, cmd);
    char **argv = sandbox_exec_argv(app, cmd);
    g_spawn_async(app->proj_root, argv, NULL,
                  G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    g_strfreev(argv);
    g_free(cmd);
}

static void populate_list(RemoteCtx *ctx, GList *repos)
{
    /* Alte Eintraege leeren */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(ctx->list_box)))
        gtk_box_remove(GTK_BOX(ctx->list_box), child);

    if (!repos) {
        GtkWidget *lbl = gtk_label_new("Keine Repositories gefunden.");
        gtk_widget_add_css_class(lbl, "hc-settings-hint");
        gtk_widget_set_margin_top(lbl, 16);
        gtk_box_append(GTK_BOX(ctx->list_box), lbl);
        return;
    }

    for (GList *l = repos; l; l = l->next) {
        RepoEntry *r = l->data;
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(row, 4);
        gtk_widget_set_margin_end(row, 4);
        gtk_widget_set_margin_top(row, 4);
        gtk_widget_add_css_class(row, "hc-tool-row");

        GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(info, TRUE);

        GtkWidget *name_lbl = gtk_label_new(r->name);
        gtk_widget_add_css_class(name_lbl, "hc-tool-name");
        gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0);

        gtk_box_append(GTK_BOX(info), name_lbl);
        if (r->desc && *r->desc) {
            GtkWidget *desc_lbl = gtk_label_new(r->desc);
            gtk_widget_add_css_class(desc_lbl, "hc-tool-desc");
            gtk_label_set_xalign(GTK_LABEL(desc_lbl), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(desc_lbl), PANGO_ELLIPSIZE_END);
            gtk_box_append(GTK_BOX(info), desc_lbl);
        }

        if (r->is_private) {
            GtkWidget *badge = gtk_label_new("privat");
            gtk_widget_add_css_class(badge, "hc-tool-badge");
            gtk_box_append(GTK_BOX(row), badge);
        }

        /* Clone-Button */
        GtkWidget *clone_btn = gtk_button_new_with_label("Clone");
        gtk_widget_add_css_class(clone_btn, "suggested-action");
        g_object_set_data_full(G_OBJECT(clone_btn), "hc-clone-url",
                               g_strdup(r->url), g_free);
        g_signal_connect(clone_btn, "clicked",
                         G_CALLBACK(on_clone_clicked), ctx->app);

        gtk_box_append(GTK_BOX(row), info);
        gtk_box_append(GTK_BOX(row), clone_btn);
        gtk_box_append(GTK_BOX(ctx->list_box), row);
    }
}

/* ---- API-Anfragen ---- */
typedef struct { RemoteCtx *ctx; int source; } FetchCtx;

static void on_api_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    FetchCtx *fc = ud;
    RemoteCtx *ctx = fc->ctx;
    int source = fc->source;
    g_free(fc);

    GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, NULL);
    set_status(ctx, NULL, FALSE);

    if (!body) {
        set_status(ctx, "Verbindung fehlgeschlagen.", FALSE);
        return;
    }

    gsize len = 0;
    const char *data = g_bytes_get_data(body, &len);
    GList *repos = NULL;

    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, data, (gssize)len, NULL)) {
        JsonNode *root = json_parser_get_root(p);
        JsonArray *arr = NULL;

        if (JSON_NODE_HOLDS_ARRAY(root)) {
            arr = json_node_get_array(root);
        } else if (JSON_NODE_HOLDS_OBJECT(root)) {
            /* GitHub antwortet bei /user/repos direkt mit Array,
               bei Fehler mit Objekt */
            JsonObject *obj = json_node_get_object(root);
            if (json_object_has_member(obj, "message")) {
                const char *msg = json_object_get_string_member(obj, "message");
                char *err = g_strdup_printf("API-Fehler: %s", msg ? msg : "unbekannt");
                set_status(ctx, err, FALSE);
                g_free(err);
                g_object_unref(p); g_bytes_unref(body);
                return;
            }
        }

        if (arr) {
            guint n = json_array_get_length(arr);
            for (guint i = 0; i < n && i < REMOTE_MAX_REPOS; i++) {
                JsonObject *o = json_array_get_object_element(arr, i);
                if (!o) continue;
                RepoEntry *r = g_new0(RepoEntry, 1);
                /* GitHub-Felder: full_name, clone_url, description, private
                   Gitea-Felder:  full_name, clone_url, description, private */
                const char *fn = json_object_has_member(o,"full_name")
                    ? json_object_get_string_member(o,"full_name") : NULL;
                const char *cu = json_object_has_member(o,"clone_url")
                    ? json_object_get_string_member(o,"clone_url") : NULL;
                const char *dc = json_object_has_member(o,"description")
                    ? json_object_get_string_member(o,"description") : NULL;
                gboolean prv = json_object_has_member(o,"private")
                    ? json_object_get_boolean_member(o,"private") : FALSE;
                r->name = g_strdup(fn ? fn : "?");
                r->url  = g_strdup(cu ? cu : "");
                r->desc = g_strdup(dc ? dc : "");
                r->is_private = prv;
                repos = g_list_append(repos, r);
            }
        }
    }
    g_object_unref(p);
    g_bytes_unref(body);

    (void)source;
    populate_list(ctx, repos);
    g_list_free_full(repos, repo_entry_free);

    char buf[64];
    snprintf(buf, sizeof(buf), "%d Repositories geladen.", g_list_length(repos));
}

static void fetch_repos(RemoteCtx *ctx, int source)
{
    ctx->source = source;
    App *app = ctx->app;

    const char *token = NULL;
    char *url_str = NULL;

    if (source == 0) {
        /* GitHub */
        token = app->settings.github_token;
        if (!token || !*token) { set_status(ctx, "Kein GitHub-Token konfiguriert.", FALSE); return; }
        url_str = g_strdup("https://api.github.com/user/repos?per_page=50&sort=updated");
    } else {
        /* Gitea */
        const char *base = app->settings.gitea_url;
        token = app->settings.gitea_token;
        if (!base || !*base) { set_status(ctx, "Keine Gitea-URL konfiguriert.", FALSE); return; }
        if (!token || !*token) { set_status(ctx, "Kein Gitea-Token konfiguriert.", FALSE); return; }
        url_str = g_strdup_printf("%s/api/v1/repos/search?limit=50&token=%s", base, token);
    }

    set_status(ctx, "Lade Repositories …", TRUE);

    SoupMessage *msg = soup_message_new(SOUP_METHOD_GET, url_str);
    g_free(url_str);
    if (!msg) { set_status(ctx, "Ungueltige URL.", FALSE); return; }

    /* Auth-Header */
    if (source == 0) {
        char *auth = g_strdup_printf("Bearer %s", token);
        soup_message_headers_append(soup_message_get_request_headers(msg),
                                    "Authorization", auth);
        g_free(auth);
        soup_message_headers_append(soup_message_get_request_headers(msg),
                                    "X-GitHub-Api-Version", "2022-11-28");
    }
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Accept", "application/json");
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "User-Agent", "HyperCore-Studio/1.0");

    FetchCtx *fc = g_new0(FetchCtx, 1);
    fc->ctx = ctx; fc->source = source;

    static SoupSession *sess = NULL;
    if (!sess) sess = soup_session_new();
    soup_session_send_and_read_async(sess, msg, G_PRIORITY_DEFAULT,
                                     NULL, on_api_done, fc);
    g_object_unref(msg);
}

static void on_load_github(GtkButton *btn, gpointer ud)
{ (void)btn; fetch_repos((RemoteCtx*)ud, 0); }

static void on_load_gitea(GtkButton *btn, gpointer ud)
{ (void)btn; fetch_repos((RemoteCtx*)ud, 1); }

/* ---- Push-Dialog ---- */
static void on_do_push(GtkButton *btn, gpointer ud)
{
    (void)btn;
    App *app = ud;
    if (!app->proj_root) return;
    /* Standard git push origin HEAD */
    terminal_feed_line(app, "[Git] git push origin HEAD …");
    char **argv = sandbox_exec_argv(app, "git push origin HEAD");
    g_spawn_async(app->proj_root, argv, NULL,
                  G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    g_strfreev(argv);
}

void git_push_dialog(App *app)
{
    if (!app->proj_root) {
        terminal_feed_line(app, "[Git] Kein Projekt geöffnet.");
        return;
    }
    GtkWidget *content = NULL;
    GtkWidget *win = hc_make_dialog(app, "Push", 420, 220, &content);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 14); gtk_widget_set_margin_bottom(box, 14);
    gtk_widget_set_margin_start(box, 14); gtk_widget_set_margin_end(box, 14);
    gtk_widget_set_vexpand(box, TRUE);
    gtk_box_append(GTK_BOX(content), box);

    GtkWidget *info = gtk_label_new(
        "Pusht den aktuellen Branch auf 'origin'.\n"
        "Remote muss konfiguriert sein (git remote add origin <url>).");
    gtk_label_set_wrap(GTK_LABEL(info), TRUE);
    gtk_label_set_xalign(GTK_LABEL(info), 0.0);
    gtk_widget_add_css_class(info, "hc-settings-hint");
    gtk_box_append(GTK_BOX(box), info);

    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(foot, "hc-dialog-footer");
    gtk_widget_set_halign(foot, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Abbrechen");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), win);
    GtkWidget *push_btn = gtk_button_new_with_label("Push");
    gtk_widget_add_css_class(push_btn, "suggested-action");
    g_signal_connect(push_btn, "clicked", G_CALLBACK(on_do_push), app);
    g_signal_connect_swapped(push_btn, "clicked", G_CALLBACK(gtk_window_destroy), win);
    gtk_box_append(GTK_BOX(foot), cancel);
    gtk_box_append(GTK_BOX(foot), push_btn);
    gtk_box_append(GTK_BOX(content), foot);

    gtk_window_present(GTK_WINDOW(win));
}

/* ---- Repository-Browser ---- */
void git_remote_panel_open(App *app)
{
    GtkWidget *content = NULL;
    GtkWidget *win = hc_make_dialog(app, "Git Remotes", 640, 540, &content);

    RemoteCtx *ctx = g_new0(RemoteCtx, 1);
    ctx->app = app;
    ctx->win = win;
    g_object_set_data_full(G_OBJECT(win), "hc-rctx", ctx, g_free);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(outer, TRUE);
    gtk_box_append(GTK_BOX(content), outer);

    /* Toolbar */
    GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(tb, 10); gtk_widget_set_margin_bottom(tb, 8);
    gtk_widget_set_margin_start(tb, 14); gtk_widget_set_margin_end(tb, 14);

    ctx->gh_btn = gtk_button_new_with_label("GitHub laden");
    gtk_widget_add_css_class(ctx->gh_btn, "suggested-action");
    gboolean gh_ok = app->settings.github_token && *app->settings.github_token;
    gtk_widget_set_sensitive(ctx->gh_btn, gh_ok);
    gtk_widget_set_tooltip_text(ctx->gh_btn, gh_ok ? "GitHub-Repositories laden"
                                                   : "GitHub-Token in Einstellungen > Git-Remotes eintragen");
    g_signal_connect(ctx->gh_btn, "clicked", G_CALLBACK(on_load_github), ctx);

    ctx->gt_btn = gtk_button_new_with_label("Gitea laden");
    gboolean gt_ok = app->settings.gitea_url && *app->settings.gitea_url
                  && app->settings.gitea_token && *app->settings.gitea_token;
    gtk_widget_set_sensitive(ctx->gt_btn, gt_ok);
    gtk_widget_set_tooltip_text(ctx->gt_btn, gt_ok ? "Gitea-Repositories laden"
                                                   : "Gitea-URL und Token in Einstellungen > Git-Remotes eintragen");
    g_signal_connect(ctx->gt_btn, "clicked", G_CALLBACK(on_load_gitea), ctx);

    ctx->spinner = gtk_spinner_new();
    gtk_widget_set_visible(ctx->spinner, FALSE);

    ctx->status_lbl = gtk_label_new("");
    gtk_widget_add_css_class(ctx->status_lbl, "hc-settings-hint");
    gtk_widget_set_hexpand(ctx->status_lbl, TRUE);
    gtk_widget_set_halign(ctx->status_lbl, GTK_ALIGN_START);
    gtk_widget_set_visible(ctx->status_lbl, FALSE);

    GtkWidget *push_btn = gtk_button_new_with_label("Push…");
    gtk_widget_set_tooltip_text(push_btn, "Aktuellen Branch pushen");
    g_signal_connect_swapped(push_btn, "clicked",
                             G_CALLBACK(git_push_dialog), app);

    gtk_box_append(GTK_BOX(tb), ctx->gh_btn);
    gtk_box_append(GTK_BOX(tb), ctx->gt_btn);
    gtk_box_append(GTK_BOX(tb), ctx->spinner);
    gtk_box_append(GTK_BOX(tb), ctx->status_lbl);
    gtk_box_append(GTK_BOX(tb), push_btn);
    gtk_box_append(GTK_BOX(outer), tb);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(outer), sep);

    /* Scrollbare Liste */
    GtkWidget *scr = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scr, TRUE);
    ctx->list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(ctx->list_box, 10);
    gtk_widget_set_margin_end(ctx->list_box, 10);
    gtk_widget_set_margin_top(ctx->list_box, 8);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), ctx->list_box);
    gtk_box_append(GTK_BOX(outer), scr);

    /* Hinweis wenn nichts geladen */
    GtkWidget *hint = gtk_label_new("GitHub oder Gitea oben laden.");
    gtk_widget_add_css_class(hint, "hc-settings-hint");
    gtk_widget_set_margin_top(hint, 20);
    gtk_box_append(GTK_BOX(ctx->list_box), hint);

    gtk_window_present(GTK_WINDOW(win));
}
