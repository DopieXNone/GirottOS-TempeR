/* awake.c – "Keep awake": while it is on, the computer (desktop or laptop)
 * does not go to sleep by itself and the screen does not blank or lock
 * because nobody touched the mouse, as Caffeine-ng does.
 *
 * Every desktop listens to a different request, so all of them are made;
 * none needs root or extra packages:
 *
 *   GtkApplication inhibit       GNOME session, xdg-desktop-portal (KDE,
 *                                GNOME, wlroots portals…): idle + suspend
 *   org.freedesktop.ScreenSaver  KDE, XFCE, LXQt, Cinnamon, MATE,
 *                                hypridle: screen saver / blank / lock
 *   systemd-logind "idle"        logind's own IdleAction and the programs
 *                                that ask logind (block mode)
 *   X11 only                     the screen saver timer is reset every
 *                                50 s (what xdg-screensaver does), and
 *                                xscreensaver, which does not look at
 *                                that timer, is told to stay off
 *
 * Suspending by hand (menu, power button, closing the lid) still works:
 * only the automatic sleep is stopped.  Everything is given back when it
 * is turned off, and also when TempeR quits (the requests belong to its
 * connections and end with them).
 */

#include "tempfm.h"

#include <gio/gunixfdlist.h>

#ifdef TEMPFM_HAVE_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#endif

#define REASON "Keep awake is on in TempeR"

static gboolean on;
static guint    app_cookie;     /* gtk_application_inhibit, 0 = none   */
static guint32  ss_cookie;      /* org.freedesktop.ScreenSaver         */
static gboolean ss_held;
static int      logind_fd = -1; /* closed = logind inhibitor released  */
static guint    x11_timer;
static guint    generation;     /* answers that arrive after an off/on */


/* ── org.freedesktop.ScreenSaver (session bus) ── */

static void
ss_uninhibit(guint32 cookie)
{
    GDBusConnection *s = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!s) return;
    g_dbus_connection_call(s, "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
        "org.freedesktop.ScreenSaver", "UnInhibit", g_variant_new("(u)", cookie),
        NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL, NULL);
    g_object_unref(s);
}

static void
on_ss_inhibit(GObject *src, GAsyncResult *res, gpointer ud)
{
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (!r) return;             /* no such service on this desktop: fine */
    guint32 cookie;
    g_variant_get(r, "(u)", &cookie);
    g_variant_unref(r);
    if (on && GPOINTER_TO_UINT(ud) == generation) {
        ss_cookie = cookie;
        ss_held = TRUE;
    } else {
        ss_uninhibit(cookie);   /* turned off meanwhile */
    }
}


/* ── systemd-logind (system bus) ── */

static void
on_logind_inhibit(GObject *src, GAsyncResult *res, gpointer ud)
{
    GUnixFDList *fds = NULL;
    GVariant *r = g_dbus_connection_call_with_unix_fd_list_finish(
        G_DBUS_CONNECTION(src), &fds, res, NULL);
    if (!r) return;
    gint32 idx = -1;
    g_variant_get(r, "(h)", &idx);
    g_variant_unref(r);
    int fd = fds && idx >= 0 ? g_unix_fd_list_get(fds, idx, NULL) : -1;
    if (fds) g_object_unref(fds);
    if (fd < 0) return;
    if (on && GPOINTER_TO_UINT(ud) == generation && logind_fd < 0)
        logind_fd = fd;
    else
        close(fd);
}


/* ── X11: reset the screen saver timer ── */

#ifdef TEMPFM_HAVE_X11
static gboolean
on_x11_tick(gpointer ud)
{
    (void)ud;
    GdkDisplay *d = gdk_display_get_default();
    if (d && GDK_IS_X11_DISPLAY(d)) {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        Display *xd = gdk_x11_display_get_xdisplay(d);
        G_GNUC_END_IGNORE_DEPRECATIONS
        XResetScreenSaver(xd);
        XFlush(xd);
        /* old X11 desktops (Openbox, i3, Fluxbox…) often run xscreensaver */
        static int has_xss = -1;
        if (has_xss < 0) {
            char *p = g_find_program_in_path("xscreensaver-command");
            has_xss = p != NULL;
            g_free(p);
        }
        if (has_xss) {
            const char *argv[] = { "xscreensaver-command", "-deactivate", NULL };
            g_spawn_async(NULL, (char **)argv, NULL,
                          G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                          G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
        }
    }
    return G_SOURCE_CONTINUE;
}
#endif


/* ── on / off ── */

static void
take(void)
{
    guint gen = ++generation;

    GApplication *app = g_application_get_default();
    if (app && GTK_IS_APPLICATION(app) && g_application_get_is_registered(app))
        app_cookie = gtk_application_inhibit(GTK_APPLICATION(app), NULL,
            GTK_APPLICATION_INHIBIT_IDLE | GTK_APPLICATION_INHIBIT_SUSPEND, REASON);

    GDBusConnection *s = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (s) {
        g_dbus_connection_call(s, "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
            "org.freedesktop.ScreenSaver", "Inhibit", g_variant_new("(ss)", "TempeR", REASON),
            G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 10000, NULL,
            on_ss_inhibit, GUINT_TO_POINTER(gen));
        g_object_unref(s);
    }

    GDBusConnection *sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (sys) {
        g_dbus_connection_call_with_unix_fd_list(sys, "org.freedesktop.login1",
            "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Inhibit",
            g_variant_new("(ssss)", "idle", "TempeR", REASON, "block"),
            G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL,
            on_logind_inhibit, GUINT_TO_POINTER(gen));
        g_object_unref(sys);
    }

#ifdef TEMPFM_HAVE_X11
    GdkDisplay *d = gdk_display_get_default();
    if (d && GDK_IS_X11_DISPLAY(d)) {
        on_x11_tick(NULL);
        x11_timer = g_timeout_add_seconds(50, on_x11_tick, NULL);
    }
#endif
}

static void
give_back(void)
{
    generation++;
    GApplication *app = g_application_get_default();
    if (app_cookie && app && GTK_IS_APPLICATION(app))
        gtk_application_uninhibit(GTK_APPLICATION(app), app_cookie);
    app_cookie = 0;
    if (ss_held) ss_uninhibit(ss_cookie);
    ss_held = FALSE;
    if (logind_fd >= 0) close(logind_fd);
    logind_fd = -1;
    if (x11_timer) g_source_remove(x11_timer);
    x11_timer = 0;
}

void
tempfm_awake_apply(void)
{
    gboolean want = tempfm_settings()->keep_awake;
    if (want == on) return;
    on = want;
    if (on) take();
    else    give_back();
}
