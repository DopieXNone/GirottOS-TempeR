/* keyboard.c – the Keyboard light window: colour and brightness of a
 * laptop's backlit keyboard, without asusd, OpenRGB or any vendor tool.
 *
 * Shown only on laptops (DMI chassis type, or a battery) whose keyboard
 * has a light the kernel knows (/sys/class/leds/…::kbd_backlight).  As for
 * the fan curves, TempeR never writes to the hardware itself: the
 * temper-fand service does (system D-Bus, polkit
 * com.girottos.temper.keyboard-light), saves the choice and puts it back
 * at every start.
 *
 *   ┌ Keyboard light ─────────────────────────── X ┐
 *   │ Brightness                    ──●──────  2   │
 *   │ Colour                               [████]  │
 *   │                                      Close   │
 *   └──────────────────────────────────────────────┘
 */

#include "tempfm.h"

#define FAN_BUS     "com.girottos.TempeR.FanControl"
#define FAN_PATH    "/com/girottos/TempeR/FanControl"
#define FAN_IFACE   "com.girottos.TempeR.FanControl"
#define LEDS_DIR    "/sys/class/leds"
/* changes are sent once the slider / colour stops moving */
#define SEND_DELAY_MS 250

typedef struct {
    GtkWidget       *win, *scale, *color, *color_row, *status, *body;
    GDBusConnection *sys;
    GCancellable    *cancel;
    guint            send_id;
    gboolean         building, busy, again;
} KbdUI;

static KbdUI *kui = NULL;       /* only one window */


/* ── detection (no root needed: sysfs is readable) ── */

static char *
kbd_led_dir(void)
{
    GDir *d = g_dir_open(LEDS_DIR, 0, NULL);
    if (!d) return NULL;
    char *best = NULL;
    const char *e;
    while ((e = g_dir_read_name(d)) != NULL)
        if (strstr(e, "kbd_backlight") && (!best || strcmp(e, best) < 0)) {
            g_free(best);
            best = g_strdup(e);
        }
    g_dir_close(d);
    char *dir = best ? g_build_filename(LEDS_DIR, best, NULL) : NULL;
    g_free(best);
    return dir;
}

static gboolean
is_laptop(void)
{
    char *t = NULL;
    if (g_file_get_contents("/sys/class/dmi/id/chassis_type", &t, NULL, NULL)) {
        /* portable, laptop, notebook, hand held, sub notebook, tablet,
         * convertible, detachable */
        int c = atoi(t);
        g_free(t);
        if (c == 8 || c == 9 || c == 10 || c == 11 || c == 14 ||
            c == 30 || c == 31 || c == 32)
            return TRUE;
    }
    /* no DMI (ARM…): a battery says the same */
    GDir *d = g_dir_open("/sys/class/power_supply", 0, NULL);
    gboolean bat = FALSE;
    const char *e;
    while (d && (e = g_dir_read_name(d)) != NULL)
        if (g_str_has_prefix(e, "BAT")) bat = TRUE;
    if (d) g_dir_close(d);
    return bat;
}

gboolean
tempfm_keyboard_available(void)
{
    static int cached = -1;
    if (cached < 0) {
        char *dir = is_laptop() ? kbd_led_dir() : NULL;
        cached = dir != NULL;
        g_free(dir);
    }
    return cached;
}


/* ── talking to temper-fand ── */

static void
set_status(const char *msg, gboolean error)
{
    gtk_label_set_text(GTK_LABEL(kui->status), msg ? msg : "");
    if (error) gtk_widget_add_css_class(kui->status, "lvl-crit");
    else       gtk_widget_remove_css_class(kui->status, "lvl-crit");
}

static void
show_error(GError *err)
{
    if (g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
        g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
        g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_SERVICE_NOT_FOUND) ||
        g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
        set_status("The TempeR service (temper-fand) is not installed or too old: "
                   "install TempeR's package to change the keyboard light.", TRUE);
    else if (g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED) ||
             g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_AUTH_FAILED))
        set_status("Not allowed to change the keyboard light.", TRUE);
    else {
        g_dbus_error_strip_remote_error(err);
        set_status(err->message, TRUE);
    }
}

static void send_now(void);

static void
on_sent(GObject *src, GAsyncResult *res, gpointer ud)
{
    (void)ud;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!r) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            kui->busy = FALSE;
            show_error(err);
        }
        g_error_free(err);
        return;
    }
    g_variant_unref(r);
    kui->busy = FALSE;
    set_status(NULL, FALSE);
    /* moved again while this one was on its way */
    if (kui->again) send_now();
}

/* (brightness, colour) as on the window */
static GVariant *
light_params(void)
{
    guint8 rgb[3] = { 255, 255, 255 };
    if (gtk_widget_get_visible(kui->color_row)) {
        const GdkRGBA *c = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(kui->color));
        rgb[0] = (guint8)CLAMP(c->red   * 255.0 + 0.5, 0, 255);
        rgb[1] = (guint8)CLAMP(c->green * 255.0 + 0.5, 0, 255);
        rgb[2] = (guint8)CLAMP(c->blue  * 255.0 + 0.5, 0, 255);
    }
    int b = (int)gtk_range_get_value(GTK_RANGE(kui->scale));
    return g_variant_new("(i@ay)", b,
                         g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, rgb, 3, 1));
}

static void
send_now(void)
{
    if (kui->busy) {
        kui->again = TRUE;
        return;
    }
    kui->again = FALSE;
    kui->busy = TRUE;
    g_dbus_connection_call(kui->sys, FAN_BUS, FAN_PATH, FAN_IFACE, "SetKeyboard",
        light_params(), NULL, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
        G_MAXINT, kui->cancel, on_sent, NULL);
}

/* the window is gone: nobody to tell */
static void
on_last_sent(GObject *src, GAsyncResult *res, gpointer ud)
{
    (void)ud;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (r) g_variant_unref(r);
}

static gboolean
on_send_timeout(gpointer ud)
{
    (void)ud;
    kui->send_id = 0;
    send_now();
    return G_SOURCE_REMOVE;
}

static void
queue_send(void)
{
    if (kui->building || !kui->sys) return;
    if (kui->send_id) g_source_remove(kui->send_id);
    kui->send_id = g_timeout_add(SEND_DELAY_MS, on_send_timeout, NULL);
}

static void
on_state(GObject *src, GAsyncResult *res, gpointer ud)
{
    (void)ud;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!r) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            show_error(err);
        g_error_free(err);
        return;
    }
    gboolean found, color;
    int bright, max;
    GVariant *rgb;
    g_variant_get(r, "(bbii@ay)", &found, &color, &bright, &max, &rgb);
    g_variant_unref(r);

    if (!found || max <= 0) {
        g_variant_unref(rgb);
        set_status("The service found no keyboard light it can change.", TRUE);
        return;
    }
    kui->building = TRUE;
    gtk_range_set_range(GTK_RANGE(kui->scale), 0, max);
    gtk_scale_clear_marks(GTK_SCALE(kui->scale));
    if (max <= 10)
        for (int i = 0; i <= max; i++)
            gtk_scale_add_mark(GTK_SCALE(kui->scale), i, GTK_POS_BOTTOM, NULL);
    if (bright < 0) {
        /* never set from TempeR: what the keyboard has now */
        char *dir = kbd_led_dir();
        char *t = NULL;
        char *p = dir ? g_build_filename(dir, "brightness", NULL) : NULL;
        bright = p && g_file_get_contents(p, &t, NULL, NULL) ? atoi(t) : max;
        g_free(t);
        g_free(p);
        g_free(dir);
    }
    gtk_range_set_value(GTK_RANGE(kui->scale), bright);
    gsize n = 0;
    const guint8 *c = g_variant_get_fixed_array(rgb, &n, 1);
    if (n == 3) {
        GdkRGBA col = { c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1 };
        gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(kui->color), &col);
    }
    g_variant_unref(rgb);
    gtk_widget_set_visible(kui->color_row, color);
    gtk_widget_set_sensitive(kui->body, TRUE);
    kui->building = FALSE;
    set_status(color ? NULL : "This keyboard has one colour: only its brightness can be changed.",
               FALSE);
}


/* ── window ── */

static char *
format_level(GtkScale *s, double v, gpointer ud)
{
    (void)s; (void)ud;
    return v < 0.5 ? g_strdup("Off") : g_strdup_printf("%.0f", v);
}

static void
on_scale(GtkRange *r, gpointer ud)
{
    (void)r; (void)ud;
    queue_send();
}

static void
on_color(GObject *o, GParamSpec *ps, gpointer ud)
{
    (void)o; (void)ps; (void)ud;
    /* a new colour on a dark keyboard: light it up too */
    if (!kui->building && gtk_range_get_value(GTK_RANGE(kui->scale)) < 0.5) {
        kui->building = TRUE;
        gtk_range_set_value(GTK_RANGE(kui->scale),
            gtk_adjustment_get_upper(gtk_range_get_adjustment(GTK_RANGE(kui->scale))));
        kui->building = FALSE;
    }
    queue_send();
}

static gboolean
on_key(GtkEventControllerKey *c, guint keyval, guint code,
       GdkModifierType mod, gpointer ud)
{
    (void)c; (void)code; (void)mod;
    if (keyval == GDK_KEY_Escape) {
        gtk_window_close(GTK_WINDOW(ud));
        return TRUE;
    }
    return FALSE;
}

/* the last change is still sent: the window is just closed */
static gboolean
on_close_request(GtkWindow *w, gpointer ud)
{
    (void)w; (void)ud;
    if (kui->send_id || kui->again) {
        if (kui->send_id) g_source_remove(kui->send_id);
        kui->send_id = 0;
        kui->again = FALSE;
        if (kui->sys)
            g_dbus_connection_call(kui->sys, FAN_BUS, FAN_PATH, FAN_IFACE, "SetKeyboard",
                light_params(), NULL, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                G_MAXINT, NULL, on_last_sent, NULL);
    }
    return FALSE;
}

static void
on_destroy(GtkWidget *w, gpointer ud)
{
    (void)w; (void)ud;
    if (!kui) return;
    if (kui->send_id) g_source_remove(kui->send_id);
    g_cancellable_cancel(kui->cancel);
    g_object_unref(kui->cancel);
    if (kui->sys) g_object_unref(kui->sys);
    g_free(kui);
    kui = NULL;
}

void
tempfm_open_keyboard(void)
{
    if (kui) {
        gtk_window_present(GTK_WINDOW(kui->win));
        return;
    }
    kui = g_new0(KbdUI, 1);
    kui->cancel = g_cancellable_new();

    GtkWidget *win = gtk_window_new();
    kui->win = win;
    GApplication *app = g_application_get_default();
    if (app && GTK_IS_APPLICATION(app))
        gtk_window_set_application(GTK_WINDOW(win), GTK_APPLICATION(app));
    gtk_widget_add_css_class(win, "props");
    gtk_window_set_title(GTK_WINDOW(win), "Keyboard light");
    gtk_window_set_default_size(GTK_WINDOW(win), 420, -1);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    tempfm_window_titlebar(win, "Keyboard light");
    g_signal_connect(win, "close-request", G_CALLBACK(on_close_request), NULL);
    g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), NULL);

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), win);
    gtk_widget_add_controller(win, kc);

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(page, 12);
    gtk_widget_set_margin_end(page, 12);
    gtk_widget_set_margin_top(page, 4);
    gtk_widget_set_margin_bottom(page, 12);
    gtk_window_set_child(GTK_WINDOW(win), page);

    kui->body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(page), kui->body);
    gtk_box_append(GTK_BOX(kui->body), tempfm_group_title("Keyboard light"));
    GtkWidget *card = tempfm_card_new();
    kui->scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 3, 1);
    gtk_scale_set_digits(GTK_SCALE(kui->scale), 0);
    gtk_range_set_round_digits(GTK_RANGE(kui->scale), 0);
    gtk_scale_set_draw_value(GTK_SCALE(kui->scale), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(kui->scale), GTK_POS_LEFT);
    gtk_scale_set_format_value_func(GTK_SCALE(kui->scale), format_level, NULL, NULL);
    gtk_widget_set_size_request(kui->scale, 200, -1);
    g_signal_connect(kui->scale, "value-changed", G_CALLBACK(on_scale), NULL);
    tempfm_card_add_widget(card, "Brightness", kui->scale);

    GtkColorDialog *cd = gtk_color_dialog_new();
    gtk_color_dialog_set_with_alpha(cd, FALSE);
    gtk_color_dialog_set_title(cd, "Keyboard colour");
    kui->color = gtk_color_dialog_button_new(cd);
    g_signal_connect(kui->color, "notify::rgba", G_CALLBACK(on_color), NULL);
    kui->color_row = tempfm_card_add_widget(card, "Colour", kui->color);
    gtk_widget_set_visible(kui->color_row, FALSE);
    gtk_box_append(GTK_BOX(kui->body), card);

    GtkWidget *note = gtk_label_new("Set straight through the kernel, no keyboard "
                                    "program needed. It stays the same after a restart.");
    gtk_widget_add_css_class(note, "prop-subtitle");
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_widget_set_margin_top(note, 4);
    gtk_box_append(GTK_BOX(kui->body), note);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(btns, 10);
    kui->status = gtk_label_new("Connecting to the TempeR service\xE2\x80\xA6");
    gtk_widget_add_css_class(kui->status, "prop-subtitle");
    gtk_label_set_wrap(GTK_LABEL(kui->status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(kui->status), 0);
    gtk_widget_set_hexpand(kui->status, TRUE);
    gtk_box_append(GTK_BOX(btns), kui->status);
    GtkWidget *close = gtk_button_new_with_label("Close");
    gtk_widget_add_css_class(close, "dlg-btn");
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_close), win);
    gtk_box_append(GTK_BOX(btns), close);
    gtk_box_append(GTK_BOX(page), btns);

    gtk_widget_set_sensitive(kui->body, FALSE);

    GError *err = NULL;
    kui->sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
    if (kui->sys) {
        g_dbus_connection_call(kui->sys, FAN_BUS, FAN_PATH, FAN_IFACE, "GetKeyboard", NULL,
            G_VARIANT_TYPE("(bbiiay)"), G_DBUS_CALL_FLAGS_NONE, 10000,
            kui->cancel, on_state, NULL);
    } else {
        set_status(err->message, TRUE);
        g_error_free(err);
    }
    gtk_window_present(GTK_WINDOW(win));
}
