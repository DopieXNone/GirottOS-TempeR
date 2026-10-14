/* main.c – TempeR entry point.
 *
 * Unlike ExploreR, TempeR runs on both Wayland and X11: GDK tries the
 * Wayland backend first and falls back to X11 (also under XWayland, or
 * when GDK_BACKEND=x11 is set).  Nothing here depends on a particular
 * desktop environment or compositor.
 */

#include "tempfm.h"

/* GTK's Wayland frame clock prints "Duplicate call to
 * gdk_frame_timings_presented() with different values" when the
 * compositor's presentation-time feedback for a frame differs from what
 * GTK recorded (common on Mutter).  It is a GTK diagnostic, not an error
 * in this program, and nothing breaks: keep just that one message out of
 * the terminal (same filter as ExploreR). */
static GLogWriterOutput
log_writer(GLogLevelFlags level, const GLogField *fields, gsize n_fields,
           gpointer ud)
{
    if (level & G_LOG_LEVEL_WARNING) {
        const char *domain = NULL, *message = NULL;
        for (gsize i = 0; i < n_fields; i++) {
            if (!strcmp(fields[i].key, "GLIB_DOMAIN"))
                domain = fields[i].value;
            else if (!strcmp(fields[i].key, "MESSAGE"))
                message = fields[i].value;
        }
        if (domain && !strcmp(domain, "Gdk") && message &&
            strstr(message, "gdk_frame_timings_presented"))
            return G_LOG_WRITER_HANDLED;
    }
    return g_log_writer_default(level, fields, n_fields, ud);
}

static void
on_startup(GApplication *app, gpointer ud)
{
    (void)ud;
    tempfm_monitor_start();
    tempfm_tray_start(app);
}

static void
on_activate(GApplication *app, gpointer ud)
{
    (void)ud;
    /* single instance: a second launch just shows the existing window,
     * also when it is hidden in the tray */
    GList *wins = gtk_application_get_windows(GTK_APPLICATION(app));
    for (GList *l = wins; l; l = l->next)
        if (g_object_get_data(l->data, "tempfm-app")) {
            tempfm_gui_show();
            return;
        }
    tempfm_window_new(GTK_APPLICATION(app));
    tempfm_overlay_apply();
}

static void
on_shutdown(GApplication *app, gpointer ud)
{
    (void)app; (void)ud;
    tempfm_monitor_stop();
}

int
main(int argc, char **argv)
{
    /* Wayland first, X11 as fallback.  An explicit GDK_BACKEND from the
     * user still wins, because GDK honours it within this list. */
    gdk_set_allowed_backends("wayland,x11");
    g_log_set_writer_func(log_writer, NULL, NULL);

    if (!gtk_init_check()) {
        g_printerr(
            "TempeR needs a graphical session (Wayland or X11).\n"
            "No display could be opened: check that WAYLAND_DISPLAY or "
            "DISPLAY is set and that the compositor / X server is running.\n");
        return 1;
    }

    GtkApplication *app = gtk_application_new("com.girottos.TempeR",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "startup",  G_CALLBACK(on_startup),  NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);

    tempfm_settings();          /* load settings.ini */
    tempfm_gui_apply_theme();

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
