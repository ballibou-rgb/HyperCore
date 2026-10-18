#include "app.h"
#include "version.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

/* ============================================================
 * Globale Einstellungen: Persistenz (GKeyFile) + Dialog.
 * Datei liegt unter ~/.config/hypercore/settings.ini
 * ============================================================ */

char *hc_config_dir(void)
{
    char *dir = g_build_filename(g_get_user_config_dir(), "hypercore", NULL);
    g_mkdir_with_parents(dir, 0700);
    return dir;
}

static char *settings_path(void)
{
    char *dir = hc_config_dir();
    char *p = g_build_filename(dir, "settings.ini", NULL);
    g_free(dir);
    return p;
}

/* Globaler Workspace ~/HyperCore (wird angelegt). */
char *hc_workspace_dir(void)
{
    char *dir = g_build_filename(g_get_home_dir(), HC_WORKSPACE_NAME, NULL);
    g_mkdir_with_parents(dir, 0755);
    return dir;
}

void settings_load(Settings *s)
{
    /* Standardwerte. */
    s->font_family    = g_strdup("Monospace");
    s->font_size      = 11;
    s->tab_width      = 4;
    s->spaces_for_tab = TRUE;
    s->autosave_enabled = TRUE;   /* Live-Betrieb ist der Normalfall */
    s->theme          = HC_THEME_DARK;
    s->agent_host     = g_strdup("127.0.0.1");
    s->agent_port     = 11434;   /* Ollama-Standardport */
    s->agent_key      = g_strdup("");
    s->agent_model    = g_strdup("qwen2.5-coder");
    s->embed_model    = g_strdup("nomic-embed-text");
    s->ghost_enabled      = FALSE;
    s->persona            = 2;        /* Coder */
    s->agent_timeout_sec  = 120;      /* 2 min – Ollama braucht manchmal lang */
    s->agent_max_tokens   = 0;        /* 0 = automatisch je Persona           */
    s->agent_temperature  = 0.3;      /* präzise, aber nicht starr            */
    s->agent_ctx_window   = 32;       /* Nachrichten im Verlauf               */
    /* DB */
    s->db_root_pass   = g_strdup("");
    s->db_extra_user  = g_strdup("");
    s->db_extra_pass  = g_strdup("");
    s->db_phpmyadmin  = FALSE;
    /* Git Remotes */
    s->github_token   = g_strdup("");
    s->github_user    = g_strdup("");
    s->gitea_url      = g_strdup("");
    s->gitea_token    = g_strdup("");
    s->gitea_user     = g_strdup("");
    s->dev_name       = g_strdup("");
    s->dev_company    = g_strdup("");
    s->dev_email      = g_strdup("");
    s->dev_website    = g_strdup("");
    s->dev_license    = g_strdup("MIT");
    s->compliance_enabled = FALSE;
    s->copyright_header   = g_strdup("(c) {year} {company} - Alle Rechte vorbehalten.");

    char *path = settings_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        GError *e = NULL;
        char *v;

        v = g_key_file_get_string(kf, "editor", "font_family", NULL);
        if (v) { g_free(s->font_family); s->font_family = v; }

        int iv = g_key_file_get_integer(kf, "editor", "font_size", &e);
        if (!e) s->font_size = iv; else g_clear_error(&e);

        iv = g_key_file_get_integer(kf, "editor", "tab_width", &e);
        if (!e) s->tab_width = iv; else g_clear_error(&e);

        if (g_key_file_has_key(kf, "editor", "spaces_for_tab", NULL))
            s->spaces_for_tab = g_key_file_get_boolean(kf, "editor", "spaces_for_tab", NULL);
        if (g_key_file_has_key(kf, "editor", "autosave", NULL))
            s->autosave_enabled = g_key_file_get_boolean(kf, "editor", "autosave", NULL);

        iv = g_key_file_get_integer(kf, "editor", "theme", &e);
        if (!e) s->theme = (HcTheme)iv; else g_clear_error(&e);

        v = g_key_file_get_string(kf, "agent", "host", NULL);
        if (v) { g_free(s->agent_host); s->agent_host = v; }

        iv = g_key_file_get_integer(kf, "agent", "port", &e);
        if (!e) s->agent_port = iv; else g_clear_error(&e);

        v = g_key_file_get_string(kf, "agent", "key", NULL);
        if (v) { g_free(s->agent_key); s->agent_key = v; }

        v = g_key_file_get_string(kf, "agent", "model", NULL);
        if (v) { g_free(s->agent_model); s->agent_model = v; }

        if (g_key_file_has_key(kf, "agent", "ghost", NULL))
            s->ghost_enabled = g_key_file_get_boolean(kf, "agent", "ghost", NULL);

        v = g_key_file_get_string(kf, "agent", "embed_model", NULL);
        if (v) { g_free(s->embed_model); s->embed_model = v; }
        iv = g_key_file_get_integer(kf, "agent", "persona", &e);
        if (!e) s->persona = iv; else g_clear_error(&e);
        iv = g_key_file_get_integer(kf, "agent", "timeout_sec", &e);
        if (!e && iv >= 10) s->agent_timeout_sec = iv; else g_clear_error(&e);
        iv = g_key_file_get_integer(kf, "agent", "max_tokens", &e);
        if (!e && iv >= 0) s->agent_max_tokens = iv; else g_clear_error(&e);
        double dv = g_key_file_get_double(kf, "agent", "temperature", &e);
        if (!e && dv >= 0.0 && dv <= 2.0) s->agent_temperature = dv; else g_clear_error(&e);
        iv = g_key_file_get_integer(kf, "agent", "ctx_window", &e);
        if (!e && iv >= 4) s->agent_ctx_window = iv; else g_clear_error(&e);

        #define LOADSTR(sec,key,field) v = g_key_file_get_string(kf, sec, key, NULL); \
            if (v) { g_free(s->field); s->field = v; }
        LOADSTR("db","root_pass",  db_root_pass);
        LOADSTR("db","extra_user", db_extra_user);
        LOADSTR("db","extra_pass", db_extra_pass);
        if (g_key_file_has_key(kf, "db", "phpmyadmin", NULL))
            s->db_phpmyadmin = g_key_file_get_boolean(kf, "db", "phpmyadmin", NULL);
        LOADSTR("git","github_token", github_token);
        LOADSTR("git","github_user",  github_user);
        LOADSTR("git","gitea_url",    gitea_url);
        LOADSTR("git","gitea_token",  gitea_token);
        LOADSTR("git","gitea_user",   gitea_user);
        LOADSTR("identity","name",dev_name);
        LOADSTR("identity","company",dev_company);
        LOADSTR("identity","email",dev_email);
        LOADSTR("identity","website",dev_website);
        LOADSTR("identity","license",dev_license);
        LOADSTR("compliance","header",copyright_header);
        #undef LOADSTR
        if (g_key_file_has_key(kf, "compliance", "enabled", NULL))
            s->compliance_enabled = g_key_file_get_boolean(kf, "compliance", "enabled", NULL);
    }
    g_key_file_free(kf);
    g_free(path);
}

void settings_save(const Settings *s)
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string (kf, "editor", "font_family", s->font_family);
    g_key_file_set_integer(kf, "editor", "font_size", s->font_size);
    g_key_file_set_integer(kf, "editor", "tab_width", s->tab_width);
    g_key_file_set_boolean(kf, "editor", "spaces_for_tab", s->spaces_for_tab);
    g_key_file_set_boolean(kf, "editor", "autosave", s->autosave_enabled);
    g_key_file_set_integer(kf, "editor", "theme", (int)s->theme);
    g_key_file_set_string (kf, "agent", "host", s->agent_host);
    g_key_file_set_integer(kf, "agent", "port", s->agent_port);
    g_key_file_set_string (kf, "agent", "key", s->agent_key);
    g_key_file_set_string (kf, "agent", "model", s->agent_model);
    g_key_file_set_string (kf, "agent", "embed_model", s->embed_model);
    g_key_file_set_boolean(kf, "agent", "ghost",       s->ghost_enabled);
    g_key_file_set_integer(kf, "agent", "persona",     s->persona);
    g_key_file_set_integer(kf, "agent", "timeout_sec",  s->agent_timeout_sec);
    g_key_file_set_integer(kf, "agent", "max_tokens",   s->agent_max_tokens);
    g_key_file_set_double (kf, "agent", "temperature",  s->agent_temperature);
    g_key_file_set_integer(kf, "agent", "ctx_window",   s->agent_ctx_window);
    g_key_file_set_string (kf, "db", "root_pass",    s->db_root_pass   ? s->db_root_pass   : "");
    g_key_file_set_string (kf, "db", "extra_user",   s->db_extra_user  ? s->db_extra_user  : "");
    g_key_file_set_string (kf, "db", "extra_pass",   s->db_extra_pass  ? s->db_extra_pass  : "");
    g_key_file_set_boolean(kf, "db", "phpmyadmin",   s->db_phpmyadmin);
    g_key_file_set_string (kf, "git", "github_token", s->github_token  ? s->github_token   : "");
    g_key_file_set_string (kf, "git", "github_user",  s->github_user   ? s->github_user    : "");
    g_key_file_set_string (kf, "git", "gitea_url",    s->gitea_url     ? s->gitea_url      : "");
    g_key_file_set_string (kf, "git", "gitea_token",  s->gitea_token   ? s->gitea_token    : "");
    g_key_file_set_string (kf, "git", "gitea_user",   s->gitea_user    ? s->gitea_user     : "");
    g_key_file_set_string (kf, "identity", "name", s->dev_name);
    g_key_file_set_string (kf, "identity", "company", s->dev_company);
    g_key_file_set_string (kf, "identity", "email", s->dev_email);
    g_key_file_set_string (kf, "identity", "website", s->dev_website);
    g_key_file_set_string (kf, "identity", "license", s->dev_license);
    g_key_file_set_boolean(kf, "compliance", "enabled", s->compliance_enabled);
    g_key_file_set_string (kf, "compliance", "header", s->copyright_header);

    char *path = settings_path();
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path);
    g_key_file_free(kf);
}

void settings_free(Settings *s)
{
    g_clear_pointer(&s->font_family, g_free);
    g_clear_pointer(&s->agent_host, g_free);
    g_clear_pointer(&s->agent_key, g_free);
    g_clear_pointer(&s->agent_model, g_free);
    g_clear_pointer(&s->embed_model, g_free);
    g_clear_pointer(&s->dev_name, g_free);
    g_clear_pointer(&s->dev_company, g_free);
    g_clear_pointer(&s->dev_email, g_free);
    g_clear_pointer(&s->dev_website, g_free);
    g_clear_pointer(&s->dev_license, g_free);
    g_clear_pointer(&s->copyright_header, g_free);
}

/* ---------------- Dialog ---------------- */

typedef struct {
    App *app;
    GtkWidget *win;
    GtkWidget *font_btn;
    GtkWidget *tab_spin;
    GtkWidget *spaces_sw;
    GtkWidget *theme_drop;
    GtkWidget *host_entry;
    GtkWidget *port_spin;
    GtkWidget *key_entry;
    GtkWidget *model_entry;
    GtkWidget *model_btn;
    GtkWidget *embed_entry;
    GtkWidget *persona_drop;
    GtkWidget *ghost_sw;
    GtkWidget *timeout_spin;
    GtkWidget *max_tokens_spin;
    GtkWidget *temperature_spin;
    GtkWidget *ctx_window_spin;
    GtkWidget *autosave_sw;
    /* DB */
    GtkWidget *db_root_pass_entry;
    GtkWidget *db_extra_user_entry;
    GtkWidget *db_extra_pass_entry;
    GtkWidget *db_phpmyadmin_sw;
    /* Git Remotes */
    GtkWidget *github_token_entry;
    GtkWidget *github_user_entry;
    GtkWidget *gitea_url_entry;
    GtkWidget *gitea_token_entry;
    GtkWidget *gitea_user_entry;
    /* Identity */
    GtkWidget *id_name, *id_company, *id_email, *id_website, *id_license;
    /* Compliance */
    GtkWidget *comp_sw, *comp_header;
} SettingsDlg;

/* GTK4 ignoriert min-width/min-height auf GtkSwitch aus CSS.
 * Groesse muss per set_size_request erzwungen werden. */
static GtkWidget *make_switch(gboolean active)
{
    GtkWidget *sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), active);
    gtk_widget_set_size_request(sw, 44, 24);
    gtk_widget_set_halign(sw, GTK_ALIGN_START);
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
    return sw;
}

static GtkWidget *labeled_row(const char *text, GtkWidget *control)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_widget_set_size_request(l, 140, -1);
    gtk_widget_add_css_class(l, "hc-settings-key");
    gtk_box_append(GTK_BOX(row), l);
    gtk_widget_set_hexpand(control, TRUE);
    gtk_widget_set_halign(control, GTK_ALIGN_FILL);
    gtk_box_append(GTK_BOX(row), control);
    return row;
}

static void on_settings_apply(GtkButton *btn, gpointer ud)
{
    (void)btn;
    SettingsDlg *d = ud;
    Settings *s = &d->app->settings;

    /* Schriftart. */
    PangoFontDescription *fd =
        gtk_font_dialog_button_get_font_desc(GTK_FONT_DIALOG_BUTTON(d->font_btn));
    if (fd) {
        const char *fam = pango_font_description_get_family(fd);
        int size = pango_font_description_get_size(fd) / PANGO_SCALE;
        if (fam && *fam) { g_free(s->font_family); s->font_family = g_strdup(fam); }
        if (size > 0) s->font_size = size;
    }

    s->tab_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->tab_spin));
    s->spaces_for_tab = gtk_switch_get_active(GTK_SWITCH(d->spaces_sw));
    s->autosave_enabled = gtk_switch_get_active(GTK_SWITCH(d->autosave_sw));
    s->theme = (HcTheme)gtk_drop_down_get_selected(GTK_DROP_DOWN(d->theme_drop));

    g_free(s->agent_host);
    s->agent_host = g_strdup(gtk_editable_get_text(GTK_EDITABLE(d->host_entry)));
    s->agent_port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->port_spin));
    g_free(s->agent_key);
    s->agent_key = g_strdup(gtk_editable_get_text(GTK_EDITABLE(d->key_entry)));
    g_free(s->agent_model);
    s->agent_model = g_strdup(gtk_editable_get_text(GTK_EDITABLE(d->model_entry)));
    s->ghost_enabled      = gtk_switch_get_active(GTK_SWITCH(d->ghost_sw));
    s->agent_timeout_sec  = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->timeout_spin));
    s->agent_max_tokens   = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->max_tokens_spin));
    s->agent_temperature  = gtk_spin_button_get_value(GTK_SPIN_BUTTON(d->temperature_spin));
    s->agent_ctx_window   = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->ctx_window_spin));

    g_free(s->embed_model);
    s->embed_model = g_strdup(gtk_editable_get_text(GTK_EDITABLE(d->embed_entry)));
    s->persona = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->persona_drop));

    #define SETSTR(field,w) g_free(s->field); s->field = g_strdup(gtk_editable_get_text(GTK_EDITABLE(w)))
    SETSTR(db_root_pass,  d->db_root_pass_entry);
    SETSTR(db_extra_user, d->db_extra_user_entry);
    SETSTR(db_extra_pass, d->db_extra_pass_entry);
    s->db_phpmyadmin = gtk_switch_get_active(GTK_SWITCH(d->db_phpmyadmin_sw));
    SETSTR(github_token, d->github_token_entry);
    SETSTR(github_user,  d->github_user_entry);
    SETSTR(gitea_url,    d->gitea_url_entry);
    SETSTR(gitea_token,  d->gitea_token_entry);
    SETSTR(gitea_user,   d->gitea_user_entry);
    SETSTR(dev_name, d->id_name);
    SETSTR(dev_company, d->id_company);
    SETSTR(dev_email, d->id_email);
    SETSTR(dev_website, d->id_website);
    SETSTR(dev_license, d->id_license);
    #undef SETSTR
    s->compliance_enabled = gtk_switch_get_active(GTK_SWITCH(d->comp_sw));
    g_free(s->copyright_header);
    s->copyright_header = g_strdup(gtk_editable_get_text(GTK_EDITABLE(d->comp_header)));

    settings_save(s);
    hc_apply_theme(d->app);
    editor_apply_settings_all(d->app);
    agent_refresh_availability(d->app);  /* Graceful Degradation neu bewerten */

    gtk_window_destroy(GTK_WINDOW(d->win));
}

/* ---- Modelle vom Server laden (/v1/models) ---- */
static void pick_model(GtkButton *b, gpointer ud)
{
    SettingsDlg *d = ud;
    const char *id = gtk_button_get_label(b);
    if (id) gtk_editable_set_text(GTK_EDITABLE(d->model_entry), id);
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

static void show_model_popover(SettingsDlg *d, GList *ids, const char *errmsg)
{
    GtkWidget *pop = gtk_popover_new();
    gtk_widget_set_parent(pop, d->model_btn);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top(box, 4); gtk_widget_set_margin_bottom(box, 4);
    gtk_widget_set_margin_start(box, 4); gtk_widget_set_margin_end(box, 4);
    if (errmsg) {
        GtkWidget *l = gtk_label_new(errmsg);
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_widget_set_size_request(l, 220, -1);
        gtk_box_append(GTK_BOX(box), l);
    } else {
        for (GList *l = ids; l; l = l->next) {
            GtkWidget *mb = gtk_button_new_with_label((char *)l->data);
            gtk_button_set_has_frame(GTK_BUTTON(mb), FALSE);
            GtkWidget *lab = gtk_button_get_child(GTK_BUTTON(mb));
            if (GTK_IS_LABEL(lab)) gtk_label_set_xalign(GTK_LABEL(lab), 0.0);
            g_signal_connect(mb, "clicked", G_CALLBACK(pick_model), d);
            gtk_box_append(GTK_BOX(box), mb);
        }
    }
    GtkWidget *scr = gtk_scrolled_window_new();
    gtk_widget_set_size_request(scr, 240, 220);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), box);
    gtk_popover_set_child(GTK_POPOVER(pop), scr);
    gtk_popover_popup(GTK_POPOVER(pop));
}

static void models_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    SettingsDlg *d = ud;
    GError *e = NULL;
    GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, &e);
    gtk_widget_set_sensitive(d->model_btn, TRUE);
    if (!body) {
        char *m = g_strdup_printf("Keine Verbindung: %s", e ? e->message : "?");
        show_model_popover(d, NULL, m);
        g_free(m); g_clear_error(&e);
        return;
    }
    gsize len = 0; const char *data = g_bytes_get_data(body, &len);
    GList *ids = NULL;
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, data, len, NULL)) {
        JsonNode *root = json_parser_get_root(p);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *o = json_node_get_object(root);
            if (json_object_has_member(o, "data")) {
                JsonArray *arr = json_object_get_array_member(o, "data");
                guint n = json_array_get_length(arr);
                for (guint i = 0; i < n; i++) {
                    JsonObject *m = json_array_get_object_element(arr, i);
                    if (m && json_object_has_member(m, "id"))
                        ids = g_list_append(ids, g_strdup(json_object_get_string_member(m, "id")));
                }
            }
        }
    }
    g_object_unref(p);
    g_bytes_unref(body);
    if (ids) { show_model_popover(d, ids, NULL); g_list_free_full(ids, g_free); }
    else show_model_popover(d, NULL, "Keine Modelle gefunden (Antwort ohne 'data').");
}

static void on_fetch_models(GtkButton *b, gpointer ud)
{
    (void)b;
    SettingsDlg *d = ud;
    const char *host = gtk_editable_get_text(GTK_EDITABLE(d->host_entry));
    int port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->port_spin));
    const char *key = gtk_editable_get_text(GTK_EDITABLE(d->key_entry));
    if (!host || !*host) return;
    gtk_widget_set_sensitive(d->model_btn, FALSE);
    char *url = g_strdup_printf("http://%s:%d/v1/models", host, port);
    static SoupSession *sess = NULL;
    if (!sess) sess = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", url);
    g_free(url);
    if (key && *key) {
        char *auth = g_strdup_printf("Bearer %s", key);
        soup_message_headers_append(soup_message_get_request_headers(msg),
                                    "Authorization", auth);
        g_free(auth);
    }
    soup_session_send_and_read_async(sess, msg, G_PRIORITY_DEFAULT, NULL, models_done, d);
    g_object_unref(msg);
}

void settings_open_dialog(App *app)
{
    SettingsDlg *d = g_new0(SettingsDlg, 1);
    d->app = app;

    GtkWidget *win = gtk_window_new();
    d->win = win;
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(app->window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 580, 500);
    hc_mark_window(app, win);
    g_object_set_data_full(G_OBJECT(win), "dlg", d, g_free);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root, "hc-dialog-root");

    /* Eigene Titelleiste */
    GtkWidget *titlebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(titlebar, "hc-dialog-titlebar");
    GtkWidget *wc = gtk_window_controls_new(GTK_PACK_END);
    gtk_window_controls_set_decoration_layout(GTK_WINDOW_CONTROLS(wc), ":close");
    GtkWidget *tbl = gtk_label_new("Einstellungen");
    gtk_widget_add_css_class(tbl, "title");
    gtk_widget_set_margin_start(tbl, 14);
    GtkWindowHandle *handle = (GtkWindowHandle *)gtk_window_handle_new();
    gtk_window_handle_set_child(handle, tbl);
    gtk_widget_set_hexpand(GTK_WIDGET(handle), TRUE);
    gtk_box_append(GTK_BOX(titlebar), GTK_WIDGET(handle));
    gtk_box_append(GTK_BOX(titlebar), wc);
    gtk_box_append(GTK_BOX(root), titlebar);

    GtkWidget *nb = gtk_notebook_new();
    gtk_widget_set_vexpand(nb, TRUE);

    /* ---- Reiter "Editor" ---- */
    GtkWidget *ed = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(ed, 16); gtk_widget_set_margin_bottom(ed, 16);
    gtk_widget_set_margin_start(ed, 16); gtk_widget_set_margin_end(ed, 16);

    GtkFontDialog *fdlg = gtk_font_dialog_new();
    d->font_btn = gtk_font_dialog_button_new(fdlg);
    PangoFontDescription *cur = pango_font_description_new();
    pango_font_description_set_family(cur, app->settings.font_family);
    pango_font_description_set_size(cur, app->settings.font_size * PANGO_SCALE);
    gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(d->font_btn), cur);
    pango_font_description_free(cur);
    gtk_box_append(GTK_BOX(ed), labeled_row("Schriftart / -größe", d->font_btn));

    d->tab_spin = gtk_spin_button_new_with_range(1, 16, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->tab_spin), app->settings.tab_width);
    gtk_box_append(GTK_BOX(ed), labeled_row("Tab-Weite", d->tab_spin));

    d->spaces_sw = make_switch(app->settings.spaces_for_tab);

    /* Live-Betrieb: beim Tippen automatisch speichern, damit Vorschau,
     * Webserver und Build immer den aktuellen Stand sehen. */
    d->autosave_sw = make_switch(app->settings.autosave_enabled);
    gtk_widget_set_halign(d->spaces_sw, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(ed), labeled_row("Tabs als Leerzeichen", d->spaces_sw));
    gtk_box_append(GTK_BOX(ed), labeled_row("Live speichern (Auto-Save)", d->autosave_sw));

    const char *themes[] = { "Dunkel", "Hell", "System", NULL };
    d->theme_drop = gtk_drop_down_new_from_strings(themes);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(d->theme_drop), (guint)app->settings.theme);
    gtk_box_append(GTK_BOX(ed), labeled_row("Theme", d->theme_drop));

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), ed, gtk_label_new("Editor"));

    /* ---- Reiter "Agent" ---- */
    GtkWidget *ag = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(ag, 16); gtk_widget_set_margin_bottom(ag, 16);
    gtk_widget_set_margin_start(ag, 16); gtk_widget_set_margin_end(ag, 16);

    GtkWidget *hint = gtk_label_new(
        "Verbindung zum KI-Modell-Server (OpenAI-kompatibel: /v1/chat/completions).\n"
        "Funktioniert mit llama.cpp, Ollama, LM Studio, vLLM u.ä.");
    gtk_widget_add_css_class(hint, "hc-settings-hint");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_box_append(GTK_BOX(ag), hint);

    d->host_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->host_entry), app->settings.agent_host);
    gtk_box_append(GTK_BOX(ag), labeled_row("IP / Host", d->host_entry));

    d->port_spin = gtk_spin_button_new_with_range(1, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->port_spin), app->settings.agent_port);
    gtk_box_append(GTK_BOX(ag), labeled_row("Port", d->port_spin));

    d->key_entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(d->key_entry), TRUE);
    gtk_editable_set_text(GTK_EDITABLE(d->key_entry), app->settings.agent_key);
    gtk_box_append(GTK_BOX(ag), labeled_row("API-Key", d->key_entry));

    d->model_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->model_entry), app->settings.agent_model);
    gtk_widget_set_hexpand(d->model_entry, TRUE);
    GtkWidget *modelrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    d->model_btn = gtk_button_new_with_label("Laden");
    gtk_widget_set_tooltip_text(d->model_btn, "Modelle vom Server abrufen (/v1/models)");
    g_signal_connect(d->model_btn, "clicked", G_CALLBACK(on_fetch_models), d);
    gtk_box_append(GTK_BOX(modelrow), d->model_entry);
    gtk_box_append(GTK_BOX(modelrow), d->model_btn);
    gtk_box_append(GTK_BOX(ag), labeled_row("Modell", modelrow));

    d->embed_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->embed_entry), app->settings.embed_model);
    gtk_box_append(GTK_BOX(ag), labeled_row("Embedding-Modell (RAG)", d->embed_entry));

    const char *personas[] = { "Architekt", "Planer", "Coder", NULL };
    d->persona_drop = gtk_drop_down_new_from_strings(personas);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(d->persona_drop), app->settings.persona);
    gtk_box_append(GTK_BOX(ag), labeled_row("Standard-Rolle", d->persona_drop));

    d->ghost_sw = make_switch(app->settings.ghost_enabled);
    gtk_box_append(GTK_BOX(ag), labeled_row("Ghost-Text aktiv", d->ghost_sw));

    /* Timeout */
    d->timeout_spin = gtk_spin_button_new_with_range(10, 600, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->timeout_spin),
                              app->settings.agent_timeout_sec);
    gtk_widget_set_tooltip_text(d->timeout_spin,
        "Wie lange auf eine Antwort gewartet wird (Sekunden). "
        "Ollama mit grossen Modellen braucht manchmal 2-3 Minuten.");
    GtkWidget *timeout_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *timeout_lbl = gtk_label_new("Sekunden");
    gtk_widget_set_halign(timeout_lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(timeout_box), d->timeout_spin);
    gtk_box_append(GTK_BOX(timeout_box), timeout_lbl);
    gtk_box_append(GTK_BOX(ag), labeled_row("Request-Timeout", timeout_box));

    /* Max Tokens */
    d->max_tokens_spin = gtk_spin_button_new_with_range(0, 32768, 256);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->max_tokens_spin),
                              app->settings.agent_max_tokens);
    gtk_widget_set_tooltip_text(d->max_tokens_spin,
        "Maximale Tokens pro Antwort. 0 = automatisch (2048 Architekt/Planer, "
        "4096 Coder). Höher = längere Antworten, mehr Speicher.");
    {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *lbl = gtk_label_new("Tokens  (0 = auto)");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(box), d->max_tokens_spin);
        gtk_box_append(GTK_BOX(box), lbl);
        gtk_box_append(GTK_BOX(ag), labeled_row("Max Tokens", box));
    }

    /* Temperature */
    d->temperature_spin = gtk_spin_button_new_with_range(0.0, 2.0, 0.05);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(d->temperature_spin), 2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->temperature_spin),
                              app->settings.agent_temperature);
    gtk_widget_set_tooltip_text(d->temperature_spin,
        "0.0 = deterministisch (immer gleiche Antwort), "
        "0.3 = präzise Empfehlung für Code, "
        "0.7 = kreativ, "
        "1.0+ = sehr kreativ/unvorhersehbar.");
    {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        /* Beschriftungen für die Extremwerte */
        GtkWidget *lo = gtk_label_new("0 = deterministisch");
        GtkWidget *hi = gtk_label_new("1+ = kreativ");
        gtk_widget_add_css_class(lo, "hc-settings-hint");
        gtk_widget_add_css_class(hi, "hc-settings-hint");
        gtk_box_append(GTK_BOX(box), d->temperature_spin);
        gtk_box_append(GTK_BOX(box), lo);
        gtk_box_append(GTK_BOX(box), hi);
        gtk_box_append(GTK_BOX(ag), labeled_row("Temperature", box));
    }

    /* Kontext-Fenster */
    d->ctx_window_spin = gtk_spin_button_new_with_range(4, 200, 4);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->ctx_window_spin),
                              app->settings.agent_ctx_window);
    gtk_widget_set_tooltip_text(d->ctx_window_spin,
        "Wie viele Nachrichten aus dem Verlauf gesendet werden. "
        "Höher = Agent 'erinnert' sich länger, aber mehr Tokens pro Request. "
        "32 ist ein guter Kompromiss. System-Nachrichten werden immer gesendet.");
    {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *lbl = gtk_label_new("Nachrichten");
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(box), d->ctx_window_spin);
        gtk_box_append(GTK_BOX(box), lbl);
        gtk_box_append(GTK_BOX(ag), labeled_row("Kontext-Fenster", box));
    }

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), ag, gtk_label_new("Agent"));

    /* ---- Reiter "Identität" (Global Identity) ---- */
    GtkWidget *id = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(id, 16); gtk_widget_set_margin_bottom(id, 16);
    gtk_widget_set_margin_start(id, 16); gtk_widget_set_margin_end(id, 16);
    GtkWidget *idhint = gtk_label_new(
        "Entwickler-Basisdaten – fließen in Templates, Datei-Header und den Agenten ein.");
    gtk_widget_add_css_class(idhint, "hc-settings-hint");
    gtk_label_set_xalign(GTK_LABEL(idhint), 0.0);
    gtk_label_set_wrap(GTK_LABEL(idhint), TRUE);
    gtk_box_append(GTK_BOX(id), idhint);
    #define IDROW(lbl,field,var) var = gtk_entry_new(); \
        gtk_editable_set_text(GTK_EDITABLE(var), app->settings.field); \
        gtk_box_append(GTK_BOX(id), labeled_row(lbl, var));
    IDROW("Name", dev_name, d->id_name);
    IDROW("Firma", dev_company, d->id_company);
    IDROW("E-Mail", dev_email, d->id_email);
    IDROW("Website", dev_website, d->id_website);
    IDROW("Standard-Lizenz", dev_license, d->id_license);
    #undef IDROW
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), id, gtk_label_new("Identität"));

    /* ---- Reiter "Compliance" ---- */
    GtkWidget *co = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(co, 16); gtk_widget_set_margin_bottom(co, 16);
    gtk_widget_set_margin_start(co, 16); gtk_widget_set_margin_end(co, 16);
    GtkWidget *cohint = gtk_label_new(
        "Strict Compliance Mode: zwingt den KI-Agenten, bei jeder Generierung "
        "DocBlocks und den definierten Copyright-Header zu verwenden.\n"
        "Platzhalter: {year}, {company}, {name}, {email}, {license}.");
    gtk_widget_add_css_class(cohint, "hc-settings-hint");
    gtk_label_set_xalign(GTK_LABEL(cohint), 0.0);
    gtk_label_set_wrap(GTK_LABEL(cohint), TRUE);
    gtk_box_append(GTK_BOX(co), cohint);
    d->comp_sw = make_switch(app->settings.compliance_enabled);
    gtk_box_append(GTK_BOX(co), labeled_row("Compliance erzwingen", d->comp_sw));
    d->comp_header = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->comp_header), app->settings.copyright_header);
    gtk_box_append(GTK_BOX(co), labeled_row("Copyright-Header", d->comp_header));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), co, gtk_label_new("Compliance"));

    /* ---- Reiter "Datenbank" ---- */
    GtkWidget *db = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(db, 14); gtk_widget_set_margin_bottom(db, 14);
    gtk_widget_set_margin_start(db, 14); gtk_widget_set_margin_end(db, 14);

    GtkWidget *db_hint = gtk_label_new(
        "MariaDB/MySQL laeuft pro Projekt in einer eigenen Sandbox-Instanz.\n"
        "Root-Zugang ist ohne Passwort aktiv. Weitere Einstellungen optional.");
    gtk_widget_add_css_class(db_hint, "hc-settings-hint");
    gtk_label_set_xalign(GTK_LABEL(db_hint), 0.0);
    gtk_label_set_wrap(GTK_LABEL(db_hint), TRUE);
    gtk_box_append(GTK_BOX(db), db_hint);

    d->db_root_pass_entry = gtk_entry_new();
    gtk_entry_set_input_purpose(GTK_ENTRY(d->db_root_pass_entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_visibility(GTK_ENTRY(d->db_root_pass_entry), FALSE);
    gtk_editable_set_text(GTK_EDITABLE(d->db_root_pass_entry), app->settings.db_root_pass ? app->settings.db_root_pass : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->db_root_pass_entry), "(leer = kein Passwort)");
    gtk_box_append(GTK_BOX(db), labeled_row("Root-Passwort", d->db_root_pass_entry));

    d->db_extra_user_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->db_extra_user_entry), app->settings.db_extra_user ? app->settings.db_extra_user : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->db_extra_user_entry), "z.B. webapp");
    gtk_box_append(GTK_BOX(db), labeled_row("Extra-Benutzer", d->db_extra_user_entry));

    d->db_extra_pass_entry = gtk_entry_new();
    gtk_entry_set_input_purpose(GTK_ENTRY(d->db_extra_pass_entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_visibility(GTK_ENTRY(d->db_extra_pass_entry), FALSE);
    gtk_editable_set_text(GTK_EDITABLE(d->db_extra_pass_entry), app->settings.db_extra_pass ? app->settings.db_extra_pass : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->db_extra_pass_entry), "Passwort fuer Extra-Benutzer");
    gtk_box_append(GTK_BOX(db), labeled_row("Extra-Passwort", d->db_extra_pass_entry));

    d->db_phpmyadmin_sw = make_switch(app->settings.db_phpmyadmin);
    gtk_widget_set_tooltip_text(d->db_phpmyadmin_sw,
        "Oeffnet phpMyAdmin im Browser wenn die Datenbank laeuft "
        "(phpMyAdmin muss in der Sandbox installiert sein).");
    gtk_box_append(GTK_BOX(db), labeled_row("phpMyAdmin-Link", d->db_phpmyadmin_sw));

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), db, gtk_label_new("Datenbank"));

    /* ---- Reiter "Git-Remotes" ---- */
    GtkWidget *gr = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(gr, 14); gtk_widget_set_margin_bottom(gr, 14);
    gtk_widget_set_margin_start(gr, 14); gtk_widget_set_margin_end(gr, 14);

    /* GitHub */
    GtkWidget *gh_head = gtk_label_new("GITHUB");
    gtk_widget_add_css_class(gh_head, "hc-about-section");
    gtk_label_set_xalign(GTK_LABEL(gh_head), 0.0);
    gtk_box_append(GTK_BOX(gr), gh_head);

    d->github_user_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->github_user_entry), app->settings.github_user ? app->settings.github_user : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->github_user_entry), "octocat");
    gtk_box_append(GTK_BOX(gr), labeled_row("Username", d->github_user_entry));

    d->github_token_entry = gtk_entry_new();
    gtk_entry_set_input_purpose(GTK_ENTRY(d->github_token_entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_visibility(GTK_ENTRY(d->github_token_entry), FALSE);
    gtk_editable_set_text(GTK_EDITABLE(d->github_token_entry), app->settings.github_token ? app->settings.github_token : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->github_token_entry), "ghp_xxxxxxxxxxxx");
    gtk_widget_set_tooltip_text(d->github_token_entry,
        "GitHub Personal Access Token. Benoetigt Rechte: repo, read:user");
    gtk_box_append(GTK_BOX(gr), labeled_row("Access Token", d->github_token_entry));

    /* Gitea */
    GtkWidget *gt_head = gtk_label_new("GITEA / EIGENE INSTANZ");
    gtk_widget_add_css_class(gt_head, "hc-about-section");
    gtk_label_set_xalign(GTK_LABEL(gt_head), 0.0);
    gtk_widget_set_margin_top(gt_head, 10);
    gtk_box_append(GTK_BOX(gr), gt_head);

    d->gitea_url_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->gitea_url_entry), app->settings.gitea_url ? app->settings.gitea_url : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->gitea_url_entry), "https://git.example.com");
    gtk_box_append(GTK_BOX(gr), labeled_row("Gitea-URL", d->gitea_url_entry));

    d->gitea_user_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->gitea_user_entry), app->settings.gitea_user ? app->settings.gitea_user : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->gitea_user_entry), "username");
    gtk_box_append(GTK_BOX(gr), labeled_row("Username", d->gitea_user_entry));

    d->gitea_token_entry = gtk_entry_new();
    gtk_entry_set_input_purpose(GTK_ENTRY(d->gitea_token_entry), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_visibility(GTK_ENTRY(d->gitea_token_entry), FALSE);
    gtk_editable_set_text(GTK_EDITABLE(d->gitea_token_entry), app->settings.gitea_token ? app->settings.gitea_token : "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->gitea_token_entry), "API-Token");
    gtk_box_append(GTK_BOX(gr), labeled_row("API-Token", d->gitea_token_entry));

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), gr, gtk_label_new("Git-Remotes"));

    /* ---- Reiter "Werkzeuge" (Custom Agent Tools) ---- */
    GtkWidget *tools_page = custom_tools_settings_page(app);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), tools_page, gtk_label_new("Werkzeuge"));

    gtk_box_append(GTK_BOX(root), nb);

    /* ---- Fussleiste ---- */
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(foot, "hc-dialog-footer");
    gtk_widget_set_halign(foot, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Abbrechen");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), win);
    GtkWidget *ok = gtk_button_new_with_label("Speichern");
    gtk_widget_add_css_class(ok, "suggested-action");
    g_signal_connect(ok, "clicked", G_CALLBACK(on_settings_apply), d);
    gtk_box_append(GTK_BOX(foot), cancel);
    gtk_box_append(GTK_BOX(foot), ok);
    gtk_box_append(GTK_BOX(root), foot);

    gtk_window_set_child(GTK_WINDOW(win), root);
    gtk_window_present(GTK_WINDOW(win));
}
