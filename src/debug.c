#include "app.h"
#include <string.h>

#ifdef HC_DEBUG
/* ============================================================
 * Debug-Fenster (nur bei --debug Build aktiv)
 * Enthaelt:
 *   - System-Prompt-Vorschau (live generiert)
 *   - Editierbarer Override-Prompt (wird vor den echten Prompt prepended)
 *   - RAG-Einstellungen live aendern
 *   - Test-Sendung an Agent
 * ============================================================ */

typedef struct {
    App       *app;
    GtkWidget *prompt_view;   /* generierter System-Prompt (read-only)     */
    GtkWidget *override_view; /* Benutzer-Override Prefix                  */
    GtkWidget *test_entry;    /* Testnachricht                             */
    GtkWidget *temp_spin;     /* Temperature Override fuer Tests           */
    GtkWidget *ctx_spin;      /* Kontext-Fenster Override                  */
} DebugCtx;

/* Generierten System-Prompt live anzeigen. */
static void refresh_prompt(DebugCtx *ctx)
{
    /* build_system_prompt ist nicht public – wir rufen agent_debug_prompt auf. */
    char *prompt = agent_debug_get_system_prompt(ctx->app);
    if (!prompt) prompt = g_strdup("(kein Projekt geladen)");
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ctx->prompt_view));
    gtk_text_buffer_set_text(buf, prompt, -1);
    g_free(prompt);
}

static void on_refresh_clicked(GtkButton *btn, gpointer ud)
{
    (void)btn;
    refresh_prompt((DebugCtx *)ud);
}

static void on_test_send(GtkButton *btn, gpointer ud)
{
    (void)btn;
    DebugCtx *ctx = ud;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(ctx->test_entry));
    if (!text || !*text) return;

    /* Override-Prompt lesen und temporaer setzen. */
    GtkTextBuffer *ob = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ctx->override_view));
    GtkTextIter a, z;
    gtk_text_buffer_get_bounds(ob, &a, &z);
    char *override = gtk_text_buffer_get_text(ob, &a, &z, FALSE);
    ctx->app->debug_prompt_override = (*override) ? g_strdup(override) : NULL;
    g_free(override);

    /* Temperature + Context-Window temporaer ueberschreiben. */
    double saved_temp = ctx->app->settings.agent_temperature;
    int    saved_ctx  = ctx->app->settings.agent_ctx_window;
    ctx->app->settings.agent_temperature =
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(ctx->temp_spin));
    ctx->app->settings.agent_ctx_window =
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ctx->ctx_spin));

    /* Nachricht senden (normaler Agent-Pfad). */
    agent_send_user_message(ctx->app, text);

    /* Werte restaurieren. */
    ctx->app->settings.agent_temperature = saved_temp;
    ctx->app->settings.agent_ctx_window  = saved_ctx;
    g_clear_pointer(&ctx->app->debug_prompt_override, g_free);

    gtk_editable_set_text(GTK_EDITABLE(ctx->test_entry), "");
}

static void on_copy_prompt(GtkButton *btn, gpointer ud)
{
    (void)btn;
    DebugCtx *ctx = ud;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ctx->prompt_view));
    GtkTextIter a, z;
    gtk_text_buffer_get_bounds(buf, &a, &z);
    char *text = gtk_text_buffer_get_text(buf, &a, &z, FALSE);
    GdkClipboard *clip = gtk_widget_get_clipboard(ctx->prompt_view);
    gdk_clipboard_set_text(clip, text);
    g_free(text);
}

void debug_window_open(App *app)
{
    if (app->debug_win) {
        gtk_window_present(GTK_WINDOW(app->debug_win));
        return;
    }

    GtkWidget *content = NULL;
    GtkWidget *win = hc_make_dialog(app, "[DEBUG] System-Prompt & Agent-Test",
                                    780, 620, &content);
    app->debug_win = win;
    g_signal_connect_swapped(win, "destroy",
        G_CALLBACK(g_nullify_pointer), &app->debug_win);

    DebugCtx *ctx = g_new0(DebugCtx, 1);
    ctx->app = app;
    g_object_set_data_full(G_OBJECT(win), "hc-dbg", ctx, g_free);

    /* Paned: links System-Prompt, rechts Override + Test */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_paned_set_position(GTK_PANED(paned), 380);
    gtk_box_append(GTK_BOX(content), paned);

    /* ---- Linke Seite: generierter System-Prompt ---- */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(left, 8); gtk_widget_set_margin_start(left, 8);
    gtk_widget_set_margin_end(left, 4); gtk_widget_set_margin_bottom(left, 8);

    GtkWidget *lhead = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *llbl = gtk_label_new("Generierter System-Prompt");
    gtk_widget_add_css_class(llbl, "hc-wizard-head");
    gtk_widget_set_hexpand(llbl, TRUE);
    GtkWidget *refresh_btn = gtk_button_new_with_label("Aktualisieren");
    GtkWidget *copy_btn = gtk_button_new_with_label("Kopieren");
    gtk_box_append(GTK_BOX(lhead), llbl);
    gtk_box_append(GTK_BOX(lhead), refresh_btn);
    gtk_box_append(GTK_BOX(lhead), copy_btn);
    gtk_box_append(GTK_BOX(left), lhead);

    GtkWidget *lscr = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(lscr, TRUE);
    ctx->prompt_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(ctx->prompt_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(ctx->prompt_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ctx->prompt_view), GTK_WRAP_WORD);
    gtk_widget_set_margin_start(ctx->prompt_view, 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(lscr), ctx->prompt_view);
    gtk_box_append(GTK_BOX(left), lscr);

    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_refresh_clicked), ctx);
    g_signal_connect(copy_btn,    "clicked", G_CALLBACK(on_copy_prompt), ctx);

    gtk_paned_set_start_child(GTK_PANED(paned), left);

    /* ---- Rechte Seite: Override + Test ---- */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(right, 8); gtk_widget_set_margin_start(right, 4);
    gtk_widget_set_margin_end(right, 8); gtk_widget_set_margin_bottom(right, 8);

    GtkWidget *olbl = gtk_label_new("Prompt-Prefix (wird vor den Prompt gestellt)");
    gtk_widget_add_css_class(olbl, "hc-wizard-head");
    gtk_label_set_xalign(GTK_LABEL(olbl), 0.0);
    gtk_box_append(GTK_BOX(right), olbl);

    GtkWidget *ohint = gtk_label_new(
        "Hier kannst du Anweisungen testen bevor sie fest eingebaut werden.\n"
        "Der Text wird nur beim naechsten Test-Send verwendet.");
    gtk_widget_add_css_class(ohint, "hc-settings-hint");
    gtk_label_set_xalign(GTK_LABEL(ohint), 0.0);
    gtk_label_set_wrap(GTK_LABEL(ohint), TRUE);
    gtk_box_append(GTK_BOX(right), ohint);

    GtkWidget *oscr = gtk_scrolled_window_new();
    gtk_widget_set_size_request(oscr, -1, 200);
    ctx->override_view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(ctx->override_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ctx->override_view), GTK_WRAP_WORD);
    gtk_widget_set_margin_start(ctx->override_view, 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(oscr), ctx->override_view);
    gtk_box_append(GTK_BOX(right), oscr);

    /* Parameter-Overrides */
    GtkWidget *params = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(params, 4);

    GtkWidget *temp_lbl = gtk_label_new("Temperature:");
    ctx->temp_spin = gtk_spin_button_new_with_range(0.0, 2.0, 0.05);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ctx->temp_spin), 2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ctx->temp_spin),
                              app->settings.agent_temperature);
    GtkWidget *ctx_lbl = gtk_label_new("Kontext:");
    ctx->ctx_spin = gtk_spin_button_new_with_range(4, 200, 4);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ctx->ctx_spin),
                              app->settings.agent_ctx_window);
    gtk_box_append(GTK_BOX(params), temp_lbl);
    gtk_box_append(GTK_BOX(params), ctx->temp_spin);
    gtk_box_append(GTK_BOX(params), ctx_lbl);
    gtk_box_append(GTK_BOX(params), ctx->ctx_spin);
    gtk_box_append(GTK_BOX(right), params);

    /* Test-Nachricht */
    GtkWidget *tlbl = gtk_label_new("Test-Nachricht senden:");
    gtk_widget_add_css_class(tlbl, "hc-settings-hint");
    gtk_label_set_xalign(GTK_LABEL(tlbl), 0.0);
    gtk_box_append(GTK_BOX(right), tlbl);

    GtkWidget *trow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ctx->test_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->test_entry),
                                   "Nachricht an den Agent…");
    gtk_widget_set_hexpand(ctx->test_entry, TRUE);
    g_signal_connect_swapped(ctx->test_entry, "activate",
                             G_CALLBACK(on_test_send), ctx);
    GtkWidget *send_btn = gtk_button_new_with_label("Senden");
    gtk_widget_add_css_class(send_btn, "suggested-action");
    g_signal_connect(send_btn, "clicked", G_CALLBACK(on_test_send), ctx);
    gtk_box_append(GTK_BOX(trow), ctx->test_entry);
    gtk_box_append(GTK_BOX(trow), send_btn);
    gtk_box_append(GTK_BOX(right), trow);

    gtk_paned_set_end_child(GTK_PANED(paned), right);

    /* Footer */
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(foot, "hc-dialog-footer");
    GtkWidget *warn = gtk_label_new("⚠  DEBUG-BUILD – nicht fuer Produktion");
    gtk_widget_add_css_class(warn, "hc-check-fail");
    gtk_widget_set_hexpand(warn, TRUE);
    GtkWidget *close_btn = gtk_button_new_with_label("Schliessen");
    g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(gtk_window_destroy), win);
    gtk_box_append(GTK_BOX(foot), warn);
    gtk_box_append(GTK_BOX(foot), close_btn);
    gtk_box_append(GTK_BOX(content), foot);

    /* Prompt initial laden */
    refresh_prompt(ctx);
    gtk_window_present(GTK_WINDOW(win));
}

#endif /* HC_DEBUG */
