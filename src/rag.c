#include "app.h"
#include "version.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <math.h>

/* ============================================================
 * Lokale RAG-Engine.
 *  - Chunkt die Whitelist-Dateien des Projekts.
 *  - Bettet Chunks über Ollama (/api/embeddings) ein.
 *  - Retrieval per Cosine-Ähnlichkeit; fehlen Vektoren (Server offline),
 *    greift ein lexikalischer Fallback.
 *  - Delta-Update beim Speichern, Garbage-Collection beim Entfernen.
 * ============================================================ */

static SoupSession *rag_session(void)
{
    static SoupSession *s = NULL;
    if (!s) s = soup_session_new();
    return s;
}

static void chunk_free(gpointer p)
{
    HcRagChunk *c = p;
    g_free(c->file); g_free(c->text); g_free(c->vec); g_free(c);
}

void rag_clear(App *app)
{
    g_list_free_full(app->rag_chunks, chunk_free);
    app->rag_chunks = NULL;
    app->rag_ready = FALSE;
}

int rag_file_chunk_count(App *app, const char *relpath)
{
    int n = 0;
    for (GList *l = app->rag_chunks; l; l = l->next) {
        HcRagChunk *c = l->data;
        if (!g_strcmp0(c->file, relpath)) n++;
    }
    return n;
}

void rag_remove_file(App *app, const char *relpath)
{
    GList *l = app->rag_chunks;
    while (l) {
        GList *next = l->next;
        HcRagChunk *c = l->data;
        if (!g_strcmp0(c->file, relpath)) {
            app->rag_chunks = g_list_delete_link(app->rag_chunks, l);
            chunk_free(c);
        }
        l = next;
    }
}

/* ---- Embedding-HTTP (Ollama /api/embeddings) ---- */
static float *parse_embedding(const char *data, gsize len, int *dim)
{
    *dim = 0;
    float *vec = NULL;
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, data, len, NULL)) {
        JsonNode *root = json_parser_get_root(p);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *o = json_node_get_object(root);
            JsonArray *arr = NULL;
            if (json_object_has_member(o, "embedding"))
                arr = json_object_get_array_member(o, "embedding");
            else if (json_object_has_member(o, "data")) { /* OpenAI-kompat. */
                JsonArray *d = json_object_get_array_member(o, "data");
                if (json_array_get_length(d) > 0) {
                    JsonObject *e0 = json_array_get_object_element(d, 0);
                    if (json_object_has_member(e0, "embedding"))
                        arr = json_object_get_array_member(e0, "embedding");
                }
            }
            if (arr) {
                guint n = json_array_get_length(arr);
                vec = g_new(float, n);
                for (guint i = 0; i < n; i++)
                    vec[i] = (float)json_array_get_double_element(arr, i);
                *dim = (int)n;
            }
        }
    }
    g_object_unref(p);
    return vec;
}

typedef struct {
    App *app;
    void (*cb)(App *app, float *vec, int dim, gpointer ud);
    gpointer ud;
} EmbedCtx;

static void embed_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    EmbedCtx *ctx = ud;
    GBytes *body = soup_session_send_and_read_finish(SOUP_SESSION(src), res, NULL);
    float *vec = NULL; int dim = 0;
    if (body) {
        gsize len = 0; const char *data = g_bytes_get_data(body, &len);
        vec = parse_embedding(data, len, &dim);
        g_bytes_unref(body);
    }
    ctx->cb(ctx->app, vec, dim, ctx->ud);
    g_free(ctx);
}

static void embed_async(App *app, const char *text,
                        void (*cb)(App*, float*, int, gpointer), gpointer ud)
{
    char *url = g_strdup_printf("http://%s:%d/api/embeddings",
        app->settings.agent_host, app->settings.agent_port);
    SoupMessage *msg = soup_message_new("POST", url);
    g_free(url);

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "model");
    json_builder_add_string_value(b, app->settings.embed_model);
    json_builder_set_member_name(b, "prompt");
    json_builder_add_string_value(b, text);
    json_builder_end_object(b);
    JsonGenerator *g = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(g, root);
    char *payload = json_generator_to_data(g, NULL);
    json_node_free(root); g_object_unref(g); g_object_unref(b);

    GBytes *bb = g_bytes_new_take(payload, strlen(payload));
    soup_message_set_request_body_from_bytes(msg, "application/json", bb);
    g_bytes_unref(bb);

    EmbedCtx *ctx = g_new0(EmbedCtx, 1);
    ctx->app = app; ctx->cb = cb; ctx->ud = ud;
    soup_session_send_and_read_async(rag_session(), msg, G_PRIORITY_LOW, NULL, embed_done, ctx);
    g_object_unref(msg);
}

/* ---- Chunking ---- */
static void add_chunks_for_file(App *app, const char *relpath)
{
    char *full = g_build_filename(app->proj_root, relpath, NULL);
    char *content = NULL; gsize len = 0;
    if (g_file_get_contents(full, &content, &len, NULL)) {
        /* nur Textdateien bis 200k */
        if (len < 200000) {
            const gsize CHUNK = 1200;
            int idx = 0;
            for (gsize off = 0; off < len; off += CHUNK) {
                gsize n = MIN(CHUNK, len - off);
                HcRagChunk *c = g_new0(HcRagChunk, 1);
                c->file = g_strdup(relpath);
                c->idx = idx++;
                c->text = g_strndup(content + off, n);
                c->vec = NULL; c->dim = 0;
                app->rag_chunks = g_list_append(app->rag_chunks, c);
            }
        }
        g_free(content);
    }
    g_free(full);
}

/* ---- sequentielles Einbetten aller Chunks ohne Vektor ---- */
static void embed_next(App *app);

static void embed_chunk_done(App *app, float *vec, int dim, gpointer ud)
{
    HcRagChunk *c = ud;
    if (vec) {
        c->vec = vec; c->dim = dim;
        embed_next(app);          /* nur bei Erfolg weitermachen        */
    } else {
        /* Embedding fehlgeschlagen (Server offline / Modell fehlt).
         * NICHT denselben Chunk erneut versuchen -> das wäre eine
         * Endlosschleife. Indexierung beenden; es greift der
         * lexikalische Fallback beim Retrieval. */
        c->dim = -1;              /* "versucht, fehlgeschlagen"          */
        app->rag_ready = TRUE;
        hc_set_activity(app, NULL);
        g_message("[rag] Embedding nicht möglich (Ollama erreichbar?) – "
                  "nutze lexikalischen Fallback.");
    }
}

static void embed_next(App *app)
{
    /* Wie viele Chunks sind noch ohne Vektor (und ohne Fehlversuch)? */
    int pending = 0, total = 0;
    for (GList *l = app->rag_chunks; l; l = l->next) {
        HcRagChunk *c = l->data;
        total++;
        if (!c->vec && c->dim != -1) pending++;
    }

    if (pending > 0) {
        /* Status live anzeigen. */
        char buf[64];
        snprintf(buf, sizeof(buf), "RAG: %d/%d Chunks …", total - pending, total);
        hc_set_activity(app, buf);

        /* Nächsten Chunk einbetten. */
        for (GList *l = app->rag_chunks; l; l = l->next) {
            HcRagChunk *c = l->data;
            if (!c->vec && c->dim != -1) {
                embed_async(app, c->text, embed_chunk_done, c);
                return;
            }
        }
    }

    app->rag_ready = TRUE;  /* alle verarbeitet */
    hc_set_activity(app, NULL);  /* Spinner aus */
}

void rag_index_project(App *app, gboolean rebuild)
{
    if (!app->proj_root) return;
    if (rebuild) rag_clear(app);

    GList *wl = project_whitelist(app);
    for (GList *l = wl; l; l = l->next) {
        const char *rel = l->data;
        if (rag_file_chunk_count(app, rel) == 0)
            add_chunks_for_file(app, rel);
    }
    g_list_free_full(wl, g_free);
    app->rag_ready = FALSE;
    embed_next(app);  /* Hintergrund-Einbettung starten */
}

void rag_update_file(App *app, const char *relpath)
{
    rag_remove_file(app, relpath);
    add_chunks_for_file(app, relpath);
    embed_next(app);
}

/* ---- Retrieval ---- */
static float cosine(const float *a, const float *b, int n)
{
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < n; i++) { dot += a[i]*b[i]; na += a[i]*a[i]; nb += b[i]*b[i]; }
    if (na == 0 || nb == 0) return 0;
    return (float)(dot / (sqrt(na) * sqrt(nb)));
}

/* lexikalischer Score (Fallback). */
static int lex_score(const char *text, char **terms, int nterms)
{
    int score = 0;
    char *low = g_ascii_strdown(text, -1);
    for (int i = 0; i < nterms; i++)
        if (terms[i] && *terms[i] && strstr(low, terms[i])) score++;
    g_free(low);
    return score;
}

typedef struct { HcRagChunk *c; float score; } Ranked;
static int cmp_ranked(const void *a, const void *b)
{
    float d = ((const Ranked*)b)->score - ((const Ranked*)a)->score;
    return (d > 0) - (d < 0);
}

static char *build_context_from_ranked(Ranked *r, int n, int k)
{
    GString *s = g_string_new("");
    int used = 0;
    for (int i = 0; i < n && used < k; i++) {
        if (r[i].score <= 0) break;
        HcRagChunk *c = r[i].c;
        g_string_append_printf(s, "### %s (Teil %d)\n%s\n\n", c->file, c->idx, c->text);
        used++;
    }
    if (used == 0) { g_string_free(s, TRUE); return NULL; }
    return g_string_free(s, FALSE);
}

typedef struct { App *app; char *query; HcRagCb cb; gpointer ud; } QueryCtx;

static char *retrieve_lexical(App *app, const char *query)
{
    guint n = g_list_length(app->rag_chunks);
    if (n == 0) return NULL;
    char *ql = g_ascii_strdown(query, -1);
    char **terms = g_strsplit_set(ql, " \t\n.,;:()[]{}\"'", -1);
    int nt = g_strv_length(terms);
    Ranked *r = g_new0(Ranked, n);
    int i = 0;
    for (GList *l = app->rag_chunks; l; l = l->next, i++) {
        r[i].c = l->data;
        r[i].score = (float)lex_score(r[i].c->text, terms, nt);
    }
    qsort(r, n, sizeof(Ranked), cmp_ranked);
    char *ctx = build_context_from_ranked(r, n, 4);
    g_free(r); g_strfreev(terms); g_free(ql);
    return ctx;
}

static void query_embed_done(App *app, float *vec, int dim, gpointer ud)
{
    QueryCtx *q = ud;
    char *ctx = NULL;
    if (vec && dim > 0) {
        guint n = g_list_length(app->rag_chunks);
        Ranked *r = g_new0(Ranked, n ? n : 1);
        int i = 0, have_vec = 0;
        for (GList *l = app->rag_chunks; l; l = l->next, i++) {
            HcRagChunk *c = l->data;
            r[i].c = c;
            if (c->vec && c->dim == dim) { r[i].score = cosine(vec, c->vec, dim); have_vec++; }
            else r[i].score = -1;
        }
        if (have_vec > 0) {
            qsort(r, n, sizeof(Ranked), cmp_ranked);
            ctx = build_context_from_ranked(r, n, 4);
        }
        g_free(r); g_free(vec);
    }
    if (!ctx) ctx = retrieve_lexical(app, q->query);  /* Fallback */
    q->cb(app, ctx, q->ud);
    g_free(q->query); g_free(q);
}

void rag_query_async(App *app, const char *query, HcRagCb cb, gpointer ud)
{
    if (!app->rag_chunks) { cb(app, NULL, ud); return; }
    QueryCtx *q = g_new0(QueryCtx, 1);
    q->app = app; q->query = g_strdup(query); q->cb = cb; q->ud = ud;
    /* Query einbetten; bei Fehler wird im Callback lexikalisch gesucht. */
    embed_async(app, query, query_embed_done, q);
}

/* ============================================================
 * Visueller RAG-Manager
 * ============================================================ */
static void on_rebuild(GtkButton *b, gpointer ud)
{
    (void)b; App *app = ud;
    rag_index_project(app, TRUE);
    rag_manager_open(app);  /* neu zeichnen */
    GtkWidget *w = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_WINDOW);
    if (w) gtk_window_destroy(GTK_WINDOW(w));
}

/* ---- Chunk-Vorschau-Fenster ---- */
typedef struct { App *app; char *filepath; int chunk_idx; } ChunkCtx;

static void on_delete_chunk(GtkButton *btn, gpointer ud)
{
    ChunkCtx *ctx = ud;
    (void)btn;
    /* Chunks der Datei aus dem Index entfernen. */
    int removed = 0;
    for (GList *l = ctx->app->rag_chunks; l; ) {
        HcRagChunk *c = l->data;
        GList *next = l->next;
        if (c->file && !g_strcmp0(c->file, ctx->filepath)
            && (ctx->chunk_idx < 0 || removed == ctx->chunk_idx)) {
            ctx->app->rag_chunks = g_list_remove_link(ctx->app->rag_chunks, l);
            g_list_free(l);
            g_free(c->file); g_free(c->text); g_free(c->vec); g_free(c);
            removed++;
            if (ctx->chunk_idx >= 0) break;
        }
        l = next;
    }
    /* Fenster schließen */
    GtkRoot *w = gtk_widget_get_root(GTK_WIDGET(btn));
    if (w) gtk_window_destroy(GTK_WINDOW(w));
    hc_set_activity(ctx->app, removed > 0 ? "RAG: Eintrag entfernt." : NULL);
}

static void show_chunk_detail(App *app, HcRagChunk *c, int idx)
{
    GtkWidget *content = NULL;
    char *title = g_strdup_printf("Chunk #%d – %s", idx,
                                  c->file ? c->file : "?");
    GtkWidget *win = hc_make_dialog(app, title, 560, 440, &content);
    g_free(title);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(box, 10); gtk_widget_set_margin_bottom(box, 4);
    gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_vexpand(box, TRUE);
    gtk_box_append(GTK_BOX(content), box);

    /* Metadaten */
    char *meta = g_strdup_printf("Datei: %s\nVektor: %s (%d Dimensionen)",
        c->file ? c->file : "?",
        c->vec ? "vorhanden" : "fehlt",
        c->dim);
    GtkWidget *meta_lbl = gtk_label_new(meta);
    g_free(meta);
    gtk_label_set_xalign(GTK_LABEL(meta_lbl), 0.0);
    gtk_widget_add_css_class(meta_lbl, "hc-settings-hint");
    gtk_label_set_selectable(GTK_LABEL(meta_lbl), TRUE);
    gtk_box_append(GTK_BOX(box), meta_lbl);

    /* Text-Inhalt */
    GtkWidget *scr = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scr, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    gtk_widget_set_margin_start(tv, 8); gtk_widget_set_margin_end(tv, 8);
    gtk_widget_set_margin_top(tv, 6);
    if (c->text) gtk_text_buffer_set_text(
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), c->text, -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), tv);
    gtk_box_append(GTK_BOX(box), scr);

    /* Footer */
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(foot, "hc-dialog-footer");
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    GtkWidget *del_btn = gtk_button_new_with_label("Diesen Chunk loeschen");
    gtk_widget_add_css_class(del_btn, "destructive-action");
    GtkWidget *close_btn = gtk_button_new_with_label("Schliessen");
    g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(gtk_window_destroy), win);

    ChunkCtx *ctx = g_new0(ChunkCtx, 1);
    ctx->app = app; ctx->filepath = g_strdup(c->file ? c->file : ""); ctx->chunk_idx = idx;
    g_object_set_data_full(G_OBJECT(del_btn), "hc-ctx", ctx, (GDestroyNotify)g_free);
    g_signal_connect(del_btn, "clicked", G_CALLBACK(on_delete_chunk), ctx);

    gtk_box_append(GTK_BOX(foot), spacer);
    gtk_box_append(GTK_BOX(foot), del_btn);
    gtk_box_append(GTK_BOX(foot), close_btn);
    gtk_box_append(GTK_BOX(content), foot);
    gtk_window_present(GTK_WINDOW(win));
}

/* Closure-Struct fuer den "Ansehen"-Button eines einzelnen Chunks. */
typedef struct { App *app; HcRagChunk *chunk; int idx; } ChunkViewCB;

static void on_open_chunk_btn(GtkButton *btn, gpointer ud)
{
    (void)btn;
    ChunkViewCB *cb = ud;
    show_chunk_detail(cb->app, cb->chunk, cb->idx);
}

/* ---- Datei-Zeile anklicken → Chunks anzeigen ---- */
typedef struct { App *app; char *filepath; GtkWidget *expander_box; gboolean expanded; } FileRowCtx;

static void on_file_row_clicked(GtkButton *btn, gpointer ud)
{
    FileRowCtx *frc = ud;
    (void)btn;
    if (frc->expanded) {
        /* Zuklappen */
        GtkWidget *child;
        while ((child = gtk_widget_get_last_child(frc->expander_box))) {
            /* erstes Kind ist der Name-Label – das bleibt */
            if (child == gtk_widget_get_first_child(frc->expander_box)) break;
            gtk_box_remove(GTK_BOX(frc->expander_box), child);
        }
        frc->expanded = FALSE;
        return;
    }
    frc->expanded = TRUE;
    /* Chunks dieser Datei sammeln */
    int idx = 0;
    for (GList *l = frc->app->rag_chunks; l; l = l->next, idx++) {
        HcRagChunk *c = l->data;
        if (!c->file || g_strcmp0(c->file, frc->filepath) != 0) continue;

        int ci = idx;
        GtkWidget *chunk_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(chunk_row, 20);
        gtk_widget_set_margin_top(chunk_row, 2);
        gtk_widget_set_margin_bottom(chunk_row, 2);

        /* Vorschau (erste 60 Zeichen) */
        char *preview = c->text ? g_strndup(c->text, 60) : g_strdup("(leer)");
        /* Zeilenumbrüche ersetzen */
        for (char *p = preview; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        char *chunk_lbl_txt = g_strdup_printf("#%d  %s…", ci, preview);
        g_free(preview);
        GtkWidget *chunk_lbl = gtk_label_new(chunk_lbl_txt);
        g_free(chunk_lbl_txt);
        gtk_label_set_xalign(GTK_LABEL(chunk_lbl), 0.0);
        gtk_widget_set_hexpand(chunk_lbl, TRUE);
        gtk_widget_add_css_class(chunk_lbl, "hc-settings-hint");

        GtkWidget *vec_badge = gtk_label_new(c->vec ? "✔ vec" : "✘ vec");
        gtk_widget_add_css_class(vec_badge, c->vec ? "hc-check-ok" : "hc-check-fail");

        GtkWidget *open_btn = gtk_button_new_with_label("Ansehen");
        gtk_widget_add_css_class(open_btn, "flat");
        /* Closure fuer den Chunk – wird beim Klick ausgewertet. */
        ChunkViewCB *cb = g_new0(ChunkViewCB, 1);
        cb->app = frc->app; cb->chunk = c; cb->idx = ci;
        g_object_set_data_full(G_OBJECT(open_btn), "hc-cb", cb, g_free);
        g_signal_connect(open_btn, "clicked",
                         G_CALLBACK(on_open_chunk_btn), cb);

        gtk_box_append(GTK_BOX(chunk_row), chunk_lbl);
        gtk_box_append(GTK_BOX(chunk_row), vec_badge);
        gtk_box_append(GTK_BOX(chunk_row), open_btn);
        gtk_box_append(GTK_BOX(frc->expander_box), chunk_row);
    }
}

void rag_manager_open(App *app)
{
    GtkWidget *root = NULL;
    GtkWidget *win = hc_make_dialog(app, "RAG-Manager", 600, 520, &root);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(outer, TRUE);
    gtk_box_append(GTK_BOX(root), outer);

    /* Status-Kopf */
    GtkWidget *head_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(head_box, 10); gtk_widget_set_margin_bottom(head_box, 6);
    gtk_widget_set_margin_start(head_box, 14); gtk_widget_set_margin_end(head_box, 14);

    guint total_chunks = g_list_length(app->rag_chunks);
    guint vec_chunks = 0;
    for (GList *l = app->rag_chunks; l; l = l->next) {
        HcRagChunk *c = l->data; if (c->vec) vec_chunks++;
    }
    char *head_txt = g_strdup_printf(
        "%u Chunks  ·  %u eingebettet  ·  %s",
        total_chunks, vec_chunks, app->rag_ready ? "bereit" : "wird aufgebaut…");
    GtkWidget *hl = gtk_label_new(head_txt);
    g_free(head_txt);
    gtk_label_set_xalign(GTK_LABEL(hl), 0.0);
    gtk_widget_add_css_class(hl, "hc-wizard-head");
    gtk_widget_set_hexpand(hl, TRUE);
    gtk_box_append(GTK_BOX(head_box), hl);

    GtkWidget *rb = gtk_button_new_with_label("Rebuild");
    gtk_widget_add_css_class(rb, "suggested-action");
    g_signal_connect(rb, "clicked", G_CALLBACK(on_rebuild), app);
    gtk_box_append(GTK_BOX(head_box), rb);
    gtk_box_append(GTK_BOX(outer), head_box);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(outer), sep);

    /* Datei-Liste mit aufklappbaren Chunks */
    GtkWidget *scr = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scr),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scr, TRUE);

    GtkWidget *list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(list_box, 8);
    gtk_widget_set_margin_end(list_box, 8);
    gtk_widget_set_margin_top(list_box, 6);

    GList *wl = project_whitelist(app);
    for (GList *l = wl; l; l = l->next) {
        const char *rel = l->data;
        int cnt = rag_file_chunk_count(app, rel);

        /* Aufklappbare Datei-Zeile */
        GtkWidget *expander_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_margin_bottom(expander_box, 2);

        GtkWidget *header_btn = gtk_button_new();
        gtk_widget_add_css_class(header_btn, "flat");
        GtkWidget *header_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(header_row, 4);
        gtk_widget_set_margin_end(header_row, 4);
        gtk_widget_set_margin_top(header_row, 5);
        gtk_widget_set_margin_bottom(header_row, 5);

        GtkWidget *arrow = gtk_label_new("▶");
        gtk_widget_add_css_class(arrow, "hc-settings-hint");

        GtkWidget *name_lbl = gtk_label_new(rel);
        gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0);
        gtk_widget_set_hexpand(name_lbl, TRUE);

        char *cnt_str = g_strdup_printf("%d Chunks", cnt);
        GtkWidget *cnt_lbl = gtk_label_new(cnt_str);
        g_free(cnt_str);
        gtk_widget_add_css_class(cnt_lbl, cnt > 0 ? "hc-check-ok" : "hc-check-fail");

        /* Alle Chunks der Datei loeschen */
        GtkWidget *del_all_btn = gtk_button_new_from_icon_name("edit-delete-symbolic");
        gtk_widget_add_css_class(del_all_btn, "flat");
        gtk_widget_set_tooltip_text(del_all_btn, "Alle Chunks dieser Datei loeschen");
        ChunkCtx *dctx = g_new0(ChunkCtx, 1);
        dctx->app = app; dctx->filepath = g_strdup(rel); dctx->chunk_idx = -1;
        g_object_set_data_full(G_OBJECT(del_all_btn), "hc-ctx", dctx, (GDestroyNotify)g_free);
        g_signal_connect(del_all_btn, "clicked", G_CALLBACK(on_delete_chunk), dctx);

        gtk_box_append(GTK_BOX(header_row), arrow);
        gtk_box_append(GTK_BOX(header_row), name_lbl);
        gtk_box_append(GTK_BOX(header_row), cnt_lbl);
        gtk_box_append(GTK_BOX(header_row), del_all_btn);
        gtk_button_set_child(GTK_BUTTON(header_btn), header_row);
        gtk_box_append(GTK_BOX(expander_box), header_btn);

        /* Chunk-Zeilen werden beim Klick dynamisch eingefuegt */
        FileRowCtx *frc = g_new0(FileRowCtx, 1);
        frc->app = app;
        frc->filepath = g_strdup(rel);
        frc->expander_box = expander_box;
        frc->expanded = FALSE;
        g_object_set_data_full(G_OBJECT(header_btn), "hc-frc", frc, (GDestroyNotify)g_free);
        g_signal_connect(header_btn, "clicked", G_CALLBACK(on_file_row_clicked), frc);

        gtk_box_append(GTK_BOX(list_box), expander_box);
    }
    g_list_free_full(wl, g_free);

    if (!app->rag_chunks) {
        GtkWidget *empty = gtk_label_new("Keine RAG-Eintraege vorhanden.\nOeffne ein Projekt um den Index aufzubauen.");
        gtk_label_set_wrap(GTK_LABEL(empty), TRUE);
        gtk_widget_add_css_class(empty, "hc-settings-hint");
        gtk_widget_set_margin_top(empty, 20);
        gtk_box_append(GTK_BOX(list_box), empty);
    }

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scr), list_box);
    gtk_box_append(GTK_BOX(outer), scr);

    gtk_window_present(GTK_WINDOW(win));
}
