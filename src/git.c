#include "app.h"
#include "version.h"
#include <git2.h>
#include <string.h>

/* ============================================================
 * Native Source Control via libgit2 (Zero-Dependency, keine
 * externen git-Binaries).
 *  - Auto-Versioning: neue/importierte Projekte werden Git-Repos.
 *  - Time Machine: Sprung auf alten Commit => Read-Only.
 *  - 1-Klick-Return zur Branch-Spitze.
 *  - KI-Commits: Commit-Message aus dem Diff (RAG-Synergie).
 * ============================================================ */

void git_startup(void) { git_libgit2_init(); }

static git_repository *open_repo(App *app)
{
    if (!app->proj_root) return NULL;
    git_repository *repo = NULL;
    if (git_repository_open(&repo, app->proj_root) != 0) return NULL;
    return repo;
}

/* Signatur aus Global Identity. */
static git_signature *make_sig(App *app)
{
    const char *name = (app->settings.dev_name && *app->settings.dev_name)
        ? app->settings.dev_name : "HyperCore Studio";
    const char *mail = (app->settings.dev_email && *app->settings.dev_email)
        ? app->settings.dev_email : "dev@localhost";
    git_signature *sig = NULL;
    git_signature_now(&sig, name, mail);
    return sig;
}

/* alles stagen + committen (auf HEAD). */
static gboolean do_commit(App *app, git_repository *repo, const char *msg)
{
    git_index *index = NULL;
    if (git_repository_index(&index, repo) != 0) return FALSE;
    git_index_add_all(index, NULL, GIT_INDEX_ADD_DEFAULT, NULL, NULL);
    git_index_write(index);

    git_oid tree_oid, commit_oid;
    if (git_index_write_tree(&tree_oid, index) != 0) { git_index_free(index); return FALSE; }
    git_index_free(index);

    git_tree *tree = NULL;
    if (git_tree_lookup(&tree, repo, &tree_oid) != 0) return FALSE;

    git_signature *sig = make_sig(app);

    /* Eltern-Commit = aktueller HEAD, falls vorhanden. */
    git_commit *parent = NULL;
    git_oid parent_oid;
    int has_parent = (git_reference_name_to_id(&parent_oid, repo, "HEAD") == 0);
    if (has_parent) git_commit_lookup(&parent, repo, &parent_oid);

    const git_commit *parents[1];
    int nparents = 0;
    if (parent) { parents[0] = parent; nparents = 1; }

    int rc = git_commit_create(&commit_oid, repo, "HEAD", sig, sig,
        NULL, msg, tree, nparents, parents);

    if (parent) git_commit_free(parent);
    git_signature_free(sig);
    git_tree_free(tree);
    return rc == 0;
}

gboolean git_ensure_repo(App *app)
{
    if (!app->proj_root) return FALSE;
    git_repository *repo = open_repo(app);
    gboolean fresh = FALSE;
    if (!repo) {
        /* Smart Import / Create: initialisieren. */
        if (git_repository_init(&repo, app->proj_root, 0) != 0) return FALSE;
        fresh = TRUE;
    }
    if (fresh) {
        do_commit(app, repo, "Initial commit (HyperCore Smart Import)");
    }
    git_repository_free(repo);
    return TRUE;
}

char *git_branch_label(App *app)
{
    git_repository *repo = open_repo(app);
    if (!repo) return g_strdup("—");
    char *out = NULL;
    if (git_repository_head_detached(repo) == 1) {
        git_oid oid;
        if (git_reference_name_to_id(&oid, repo, "HEAD") == 0) {
            char buf[8] = {0};
            git_oid_tostr(buf, sizeof(buf), &oid);
            out = g_strdup_printf("detached@%s", buf);
        } else out = g_strdup("detached");
    } else {
        git_reference *head = NULL;
        if (git_repository_head(&head, repo) == 0) {
            const char *sh = git_reference_shorthand(head);
            out = g_strdup(sh ? sh : "HEAD");
            git_reference_free(head);
        } else out = g_strdup("HEAD");
    }
    git_repository_free(repo);
    return out;
}

gboolean git_is_detached(App *app)
{
    git_repository *repo = open_repo(app);
    if (!repo) return FALSE;
    gboolean d = (git_repository_head_detached(repo) == 1);
    git_repository_free(repo);
    return d;
}

gboolean git_commit_all(App *app, const char *message)
{
    if (app->git_readonly) return FALSE;  /* Zeitreise = Schreibsperre */
    git_repository *repo = open_repo(app);
    if (!repo) return FALSE;
    gboolean ok = do_commit(app, repo, message && *message ? message : "Update");
    git_repository_free(repo);
    git_update_header(app);
    return ok;
}

/* Diff HEAD -> Arbeitsverzeichnis als Text. */
static int diff_line_cb(const git_diff_delta *d, const git_diff_hunk *h,
                        const git_diff_line *l, void *payload)
{
    (void)d; (void)h;
    GString *s = payload;
    if (l->origin == GIT_DIFF_LINE_ADDITION) g_string_append_c(s, '+');
    else if (l->origin == GIT_DIFF_LINE_DELETION) g_string_append_c(s, '-');
    else if (l->origin == GIT_DIFF_LINE_CONTEXT) g_string_append_c(s, ' ');
    g_string_append_len(s, l->content, l->content_len);
    if (s->len > 12000) return -1; /* genug Kontext */
    return 0;
}

char *git_collect_diff(App *app)
{
    git_repository *repo = open_repo(app);
    if (!repo) return NULL;
    git_diff *diff = NULL;

    /* HEAD-Tree gegen Workdir. */
    git_object *head_obj = NULL;
    git_tree *head_tree = NULL;
    if (git_revparse_single(&head_obj, repo, "HEAD^{tree}") == 0)
        head_tree = (git_tree *)head_obj;

    GString *s = g_string_new("");
    if (git_diff_tree_to_workdir_with_index(&diff, repo, head_tree, NULL) == 0) {
        git_diff_print(diff, GIT_DIFF_FORMAT_PATCH, diff_line_cb, s);
        git_diff_free(diff);
    }
    if (head_tree) git_tree_free(head_tree);
    git_repository_free(repo);
    if (s->len == 0) { g_string_free(s, TRUE); return NULL; }
    return g_string_free(s, FALSE);
}

gboolean git_return_latest(App *app)
{
    git_repository *repo = open_repo(app);
    if (!repo) return FALSE;
    gboolean ok = FALSE;
    /* bevorzugt main, sonst master, sonst erster lokaler Branch. */
    const char *cands[] = { "refs/heads/main", "refs/heads/master", NULL };
    git_reference *branch = NULL;
    for (int i = 0; cands[i]; i++)
        if (git_reference_lookup(&branch, repo, cands[i]) == 0) break;
    if (!branch) {
        git_branch_iterator *it = NULL;
        if (git_branch_iterator_new(&it, repo, GIT_BRANCH_LOCAL) == 0) {
            git_branch_t bt;
            git_branch_next(&branch, &bt, it);
            git_branch_iterator_free(it);
        }
    }
    if (branch) {
        const char *name = git_reference_name(branch);
        git_object *target = NULL;
        if (git_reference_peel(&target, branch, GIT_OBJECT_COMMIT) == 0) {
            git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
            co.checkout_strategy = GIT_CHECKOUT_SAFE;
            if (git_checkout_tree(repo, target, &co) == 0) {
                git_repository_set_head(repo, name);
                ok = TRUE;
            }
            git_object_free(target);
        }
        git_reference_free(branch);
    }
    git_repository_free(repo);
    app->git_readonly = FALSE;
    git_update_header(app);
    return ok;
}

/* Checkout eines bestimmten Commits -> Read-Only Zeitreise. */
static gboolean checkout_commit(App *app, const char *sha)
{
    git_repository *repo = open_repo(app);
    if (!repo) return FALSE;
    git_oid oid;
    gboolean ok = FALSE;
    if (git_oid_fromstr(&oid, sha) == 0) {
        git_commit *c = NULL;
        if (git_commit_lookup(&c, repo, &oid) == 0) {
            git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
            co.checkout_strategy = GIT_CHECKOUT_SAFE;
            if (git_checkout_tree(repo, (git_object *)c, &co) == 0) {
                git_repository_set_head_detached(repo, &oid);
                ok = TRUE;
            }
            git_commit_free(c);
        }
    }
    git_repository_free(repo);
    if (ok) { app->git_readonly = TRUE; git_update_header(app); }
    return ok;
}

/* ============================================================
 * Header-Anzeige (Branch + "Zurück zu Latest")
 * ============================================================ */
void git_update_header(App *app)
{
    if (!app->git_branch_lbl) return;
    char *br = git_branch_label(app);
    gboolean detached = app->git_readonly || git_is_detached(app);
    char *txt = g_strdup_printf("%s %s", detached ? "⏱" : "⎇", br);
    gtk_label_set_text(GTK_LABEL(app->git_branch_lbl), txt);
    g_free(txt); g_free(br);
    if (app->git_latest_btn)
        gtk_widget_set_visible(app->git_latest_btn, detached);
    /* Schreibsperre visuell andeuten. */
    if (detached) gtk_widget_add_css_class(app->window, "hc-readonly");
    else          gtk_widget_remove_css_class(app->window, "hc-readonly");
}

/* ============================================================
 * Commit-Dialog mit KI-Message
 * ============================================================ */
typedef struct { App *app; GtkWidget *win; GtkWidget *view; } CommitDlg;

static void ai_msg_ready(App *app, char *text, gpointer ud)
{
    CommitDlg *d = ud;
    (void)app;
    if (text && *text) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->view));
        gtk_text_buffer_set_text(buf, g_strstrip(text), -1);
    }
    g_free(text);
}

static void on_ai_message(GtkButton *b, gpointer ud)
{
    (void)b;
    CommitDlg *d = ud;
    char *diff = git_collect_diff(d->app);
    if (!diff) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->view));
        gtk_text_buffer_set_text(buf, "Keine Änderungen gegenüber HEAD.", -1);
        return;
    }
    char *prompt = g_strdup_printf(
        "Erzeuge eine knappe, präzise deutsche Git-Commit-Message (eine "
        "Betreffzeile, max. 72 Zeichen, optional kurzer Body) für folgendes "
        "Diff. Gib NUR die Commit-Message aus:\n\n%s", diff);
    agent_oneshot(d->app,
        "Du bist ein Git-Assistent und schreibst exzellente Commit-Messages.",
        prompt, ai_msg_ready, d);
    g_free(prompt); g_free(diff);
}

static void on_do_commit(GtkButton *b, gpointer ud)
{
    (void)b;
    CommitDlg *d = ud;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->view));
    GtkTextIter a, z; gtk_text_buffer_get_bounds(buf, &a, &z);
    char *msg = gtk_text_buffer_get_text(buf, &a, &z, FALSE);
    if (git_commit_all(d->app, msg))
        terminal_feed_line(d->app, "[Git] Commit erstellt.");
    else
        terminal_feed_line(d->app, "[Git] Commit fehlgeschlagen (ggf. Zeitreise aktiv).");
    g_free(msg);
    gtk_window_destroy(GTK_WINDOW(d->win));
}

void git_commit_dialog(App *app)
{
    if (!app->proj_root) return;
    if (app->git_readonly) {
        terminal_feed_line(app, "[Git] Zeitreise aktiv - zum Committen erst 'Zurueck zu Latest'.");
        return;
    }
    editor_save_all(app);
    CommitDlg *d = g_new0(CommitDlg, 1);
    d->app = app;
    GtkWidget *root_outer = NULL;
    GtkWidget *win = hc_make_dialog(app, "Commit", 520, 320, &root_outer);
    d->win = win;
    g_object_set_data_full(G_OBJECT(win), "d", d, g_free);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_vexpand(root, TRUE);
    gtk_widget_set_margin_top(root, 12); gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12); gtk_widget_set_margin_end(root, 12);
    gtk_box_append(GTK_BOX(root_outer), root);

    GtkWidget *scr = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scr, TRUE);
    d->view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(d->view), GTK_WRAP_WORD);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), d->view);
    gtk_box_append(GTK_BOX(root), scr);

    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *ai = gtk_button_new_with_label("KI-Message generieren");
    g_signal_connect(ai, "clicked", G_CALLBACK(on_ai_message), d);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    GtkWidget *ok = gtk_button_new_with_label("Commit");
    gtk_widget_add_css_class(ok, "suggested-action");
    g_signal_connect(ok, "clicked", G_CALLBACK(on_do_commit), d);
    gtk_box_append(GTK_BOX(foot), ai);
    gtk_box_append(GTK_BOX(foot), spacer);
    gtk_box_append(GTK_BOX(foot), ok);
    gtk_box_append(GTK_BOX(root), foot);
    gtk_window_present(GTK_WINDOW(win));
}

/* ============================================================
 * Time Machine (Verlauf)
 * ============================================================ */
typedef struct { App *app; char sha[41]; } Jump;
static void on_timejump(GtkButton *b, gpointer ud)
{
    Jump *j = ud;
    if (checkout_commit(j->app, j->sha)) {
        terminal_feed_line(j->app, "[Git] Zeitreise (Read-Only) aktiv.");
        project_refresh_tree(j->app);
        GtkWidget *w = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_WINDOW);
        if (w) gtk_window_destroy(GTK_WINDOW(w));
    }
}
static void jump_free(gpointer p, GClosure *c) { (void)c; g_free(p); }

void git_open_timemachine(App *app)
{
    if (!app->proj_root) return;
    git_repository *repo = open_repo(app);
    if (!repo) { terminal_feed_line(app, "[Git] Kein Repository."); return; }

    GtkWidget *root_outer = NULL;
    GtkWidget *win = hc_make_dialog(app, "Time Machine", 560, 480, &root_outer);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_vexpand(root, TRUE);
    gtk_widget_set_margin_top(root, 12); gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12); gtk_widget_set_margin_end(root, 12);
    gtk_box_append(GTK_BOX(root_outer), root);

    GtkWidget *scr = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scr, TRUE);
    GtkWidget *list = gtk_list_box_new();
    gtk_widget_add_css_class(list, "hc-search-results");

    git_revwalk *walk = NULL;
    if (git_revwalk_new(&walk, repo) == 0) {
        git_revwalk_sorting(walk, GIT_SORT_TIME);
        if (git_revwalk_push_head(walk) == 0) {
            git_oid oid; int count = 0;
            while (git_revwalk_next(&oid, walk) == 0 && count < 80) {
                git_commit *c = NULL;
                if (git_commit_lookup(&c, repo, &oid) == 0) {
                    const char *summary = git_commit_summary(c);
                    char shabuf[41] = {0};
                    git_oid_tostr(shabuf, sizeof(shabuf), &oid);
                    char shortbuf[8] = {0};
                    git_oid_tostr(shortbuf, sizeof(shortbuf), &oid);

                    GtkWidget *rowb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                    gtk_widget_set_margin_start(rowb, 10); gtk_widget_set_margin_end(rowb, 10);
                    gtk_widget_set_margin_top(rowb, 5); gtk_widget_set_margin_bottom(rowb, 5);
                    char *meta = g_strdup_printf("%s  %s", shortbuf, summary ? summary : "");
                    GtkWidget *l = gtk_label_new(meta);
                    g_free(meta);
                    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
                    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
                    gtk_widget_set_hexpand(l, TRUE);
                    GtkWidget *go = gtk_button_new_with_label("Zeitreise");
                    Jump *j = g_new0(Jump, 1);
                    j->app = app;
                    g_strlcpy(j->sha, shabuf, sizeof(j->sha));
                    g_signal_connect_data(go, "clicked", G_CALLBACK(on_timejump), j, jump_free, 0);
                    gtk_box_append(GTK_BOX(rowb), l);
                    gtk_box_append(GTK_BOX(rowb), go);
                    GtkWidget *row = gtk_list_box_row_new();
                    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), rowb);
                    gtk_list_box_append(GTK_LIST_BOX(list), row);
                    git_commit_free(c);
                    count++;
                }
            }
        }
        git_revwalk_free(walk);
    }
    git_repository_free(repo);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), list);
    gtk_box_append(GTK_BOX(root), scr);

    GtkWidget *latest = gtk_button_new_with_label("Zurück zu Latest");
    gtk_widget_add_css_class(latest, "suggested-action");
    g_signal_connect_swapped(latest, "clicked", G_CALLBACK(git_return_latest), app);
    g_signal_connect_swapped(latest, "clicked", G_CALLBACK(gtk_window_destroy), win);
    gtk_box_append(GTK_BOX(root), latest);

    gtk_window_present(GTK_WINDOW(win));
}
