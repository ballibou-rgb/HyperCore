#include <gtk/gtk.h>

static int clicks = 0;

static void on_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GtkLabel *counter = GTK_LABEL(user_data);
    char buf[64];
    g_snprintf(buf, sizeof(buf), "%d-mal geklickt.", ++clicks);
    gtk_label_set_text(counter, buf);
}

static void on_activate(GtkApplication *app, gpointer u) {
    (void)u;
    GtkBuilder *b = gtk_builder_new_from_file("src/window.ui");
    GObject *win = gtk_builder_get_object(b, "main_window");
    gtk_window_set_application(GTK_WINDOW(win), app);
    GObject *btn = gtk_builder_get_object(b, "btn");
    GObject *counter = gtk_builder_get_object(b, "counter");
    if (btn && counter)
        g_signal_connect(btn, "clicked", G_CALLBACK(on_clicked), counter);
    gtk_window_present(GTK_WINDOW(win));
    g_object_unref(b);
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.example.hello", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int s = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return s;
}
