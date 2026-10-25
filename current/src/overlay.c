/* overlay.c – the on-screen overlay (temperatures, usage, swap…).
 *
 * The overlay is a window of its own, never part of the game: nothing is
 * loaded into other programs and nothing is drawn inside them.
 *
 * To stay above games and full-screen apps it is an X11 "override
 * redirect" window, opened on XWayland when the session is Wayland (GNOME,
 * KDE, Hyprland, Sway… all ship XWayland): window managers and compositors
 * do not manage such windows, so they are always on top, never take the
 * focus, and have no title bar.  Its input region is empty, so every click
 * goes through to the window below.
 *
 * Without XWayland (or with a GTK built without X11) it falls back to a
 * normal small window, which cannot stay above full-screen windows.
 */

#include "tempfm.h"

#ifdef TEMPFM_HAVE_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#endif

static GtkWidget      *ov_win    = NULL;
static GtkWidget      *ov_grid   = NULL;
static GdkDisplay     *ov_disp   = NULL;     /* X11 display, or the default one */
static gboolean        ov_x11    = FALSE;
static gboolean        disp_tried = FALSE;
static GtkCssProvider *ov_css    = NULL;
static guint           ov_listen = 0;

static const char *OV_CSS =
    "window.tempfm-overlay { background: rgba(0,0,0,{alpha}); border-radius: 4px; }"
    "window.tempfm-overlay.solid-edge { border: 1px solid rgba(255,255,255,0.15); }"
    ".ov-grid { padding: 5px 9px; }"
    ".ov-grid label {"
    "  font-family: \"Segoe UI\", \"Noto Sans\", \"DejaVu Sans\", sans-serif;"
    "  font-weight: bold; font-size: {size}px; color: #ffffff;"
    "  font-feature-settings: \"tnum\";"
    "  text-shadow: 1px 1px 2px rgba(0,0,0,0.9);"
    "}"
    ".ov-grid label.ov-name { margin-right: 10px; }"
    ".ov-grid label.ov-cpu  { color: #6cc6ff; }"
    ".ov-grid label.ov-gpu  { color: #7ee07e; }"
    ".ov-grid label.ov-disk { color: #ffcf6e; }"
    ".ov-grid label.ov-ram  { color: #d6a4ff; }"
    ".ov-grid label.ov-swap { color: #c8c8c8; }"
    ".ov-grid label.ov-fans { color: #8fd3d3; }"
    ".ov-grid label.ov-bat  { color: #ffe082; }"
    ".ov-grid label.ov-time { color: #ffffff; }"
    ".ov-grid label.ov-unit { font-weight: normal; color: #d0d0d0; font-size: {small}px; margin-left: 1px; }"
    ".ov-grid label.lvl-warm { color: #ffd54f; }"
    ".ov-grid label.lvl-hot  { color: #ff9e40; }"
    ".ov-grid label.lvl-crit { color: #ff5252; }";


/* ── the display the overlay lives on ── */

static GdkDisplay *
overlay_display(void)
{
    if (disp_tried) return ov_disp;
    disp_tried = TRUE;

    GdkDisplay *def = gdk_display_get_default();
#ifdef TEMPFM_HAVE_X11
    if (def && GDK_IS_X11_DISPLAY(def)) {
        ov_disp = def;
        ov_x11 = TRUE;
        return ov_disp;
    }
    /* Wayland session: open XWayland as a second display */
    const char *x = g_getenv("DISPLAY");
    if (x && *x) {
        GdkDisplay *d = gdk_display_open(x);
        if (d && GDK_IS_X11_DISPLAY(d)) {
            ov_disp = d;
            ov_x11 = TRUE;
            return ov_disp;
        }
        if (d) gdk_display_close(d);
    }
#endif
    ov_disp = def;
    return ov_disp;
}

gboolean
tempfm_overlay_on_top(void)
{
    overlay_display();
    return ov_x11;
}

GListModel *
tempfm_overlay_monitors(void)
{
    GdkDisplay *d = overlay_display();
    return d ? gdk_display_get_monitors(d) : NULL;
}


/* ── content ── */

static void
add_cell(int col, int row, const char *text, const char *css1, const char *css2)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), col == 0 ? 0.0f : 1.0f);
    if (css1) gtk_widget_add_css_class(l, css1);
    if (css2) gtk_widget_add_css_class(l, css2);
    gtk_grid_attach(GTK_GRID(ov_grid), l, col, row, 1, 1);
}

/* value + small unit, in the same cell */
static void
add_value(int col, int row, const char *value, const char *unit, TempLevel level)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(box, GTK_ALIGN_END);
    gtk_widget_set_margin_start(box, 10);
    GtkWidget *v = gtk_label_new(value);
    if (level != LEVEL_UNKNOWN && level != LEVEL_NORMAL)
        gtk_widget_add_css_class(v, tempfm_level_class(level));
    gtk_widget_set_valign(v, GTK_ALIGN_BASELINE_CENTER);
    gtk_box_append(GTK_BOX(box), v);
    if (unit) {
        GtkWidget *u = gtk_label_new(unit);
        gtk_widget_add_css_class(u, "ov-unit");
        gtk_widget_set_valign(u, GTK_ALIGN_BASELINE_CENTER);
        gtk_box_append(GTK_BOX(box), u);
    }
    gtk_grid_attach(GTK_GRID(ov_grid), box, col, row, 1, 1);
}

static void
add_temp(int col, int row, Sensor *s)
{
    if (!s || !s->valid) {
        add_value(col, row, "\xE2\x80\x94", NULL, LEVEL_UNKNOWN);
        return;
    }
    TempfmSettings *st = tempfm_settings();
    double v = st->unit == UNIT_FAHRENHEIT ? s->value * 9.0 / 5.0 + 32.0 : s->value;
    char *t = g_strdup_printf("%.0f", v);
    add_value(col, row, t, st->unit == UNIT_FAHRENHEIT ? "\xC2\xB0" "F" : "\xC2\xB0" "C",
              tempfm_sensor_level(s));
    g_free(t);
}

static void
add_pct(int col, int row, double pct)
{
    if (pct < 0) {
        add_value(col, row, "\xE2\x80\x94", NULL, LEVEL_UNKNOWN);
        return;
    }
    char *t = g_strdup_printf("%.0f", pct);
    add_value(col, row, t, "%", LEVEL_UNKNOWN);
    g_free(t);
}

static void
add_used(int col, int row, double used_gib, double total_gib)
{
    char *t = g_strdup_printf("%.1f/%.0f", used_gib, total_gib);
    add_value(col, row, t, "GB", LEVEL_UNKNOWN);
    g_free(t);
}

/* hottest main sensor of a category */
static Sensor *
hottest(GPtrArray *devs, SensorCategory c)
{
    Sensor *best = NULL;
    for (guint i = 0; i < devs->len; i++) {
        Device *d = devs->pdata[i];
        if (d->category != c) continue;
        Sensor *s = tempfm_device_main_sensor(d);
        if (s && (!best || (s->valid && (!best->valid || s->value > best->value)))) best = s;
    }
    return best;
}

/* returns the number of lines shown */
static int
overlay_fill(void)
{
    if (!ov_grid) return 0;
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(ov_grid)) != NULL)
        gtk_grid_remove(GTK_GRID(ov_grid), c);

    TempfmSettings *st = tempfm_settings();
    const gboolean *on = st->ov_items;
    GPtrArray *devs = tempfm_monitor_devices();
    const SystemStats *ss = tempfm_monitor_stats();
    const double GIB = 1024.0 * 1024.0 * 1024.0;
    int row = 0, col;

    /* CPU  55°C  23%  2.1GHz */
    if (on[OV_CPU_TEMP] || on[OV_CPU_USAGE] || on[OV_CPU_CLOCK]) {
        add_cell(0, row, "CPU", "ov-name", "ov-cpu");
        col = 1;
        if (on[OV_CPU_TEMP])  add_temp(col++, row, hottest(devs, CAT_CPU));
        if (on[OV_CPU_USAGE]) add_pct(col++, row, ss->cpu_usage);
        if (on[OV_CPU_CLOCK]) {
            char *t = g_strdup_printf("%.1f", ss->cpu_mhz / 1000.0);
            add_value(col++, row, ss->cpu_mhz > 0 ? t : "\xE2\x80\x94", "GHz", LEVEL_UNKNOWN);
            g_free(t);
        }
        row++;
    }

    /* one line per graphics card */
    if (on[OV_GPU_TEMP] || on[OV_GPU_USAGE] || on[OV_GPU_VRAM]) {
        int n_gpu = 0;
        for (guint i = 0; i < devs->len; i++)
            if (((Device *)devs->pdata[i])->category == CAT_GPU) n_gpu++;
        int k = 0;
        for (guint i = 0; i < devs->len; i++) {
            Device *d = devs->pdata[i];
            if (d->category != CAT_GPU) continue;
            char *name = n_gpu > 1 ? g_strdup_printf("GPU%d", ++k) : g_strdup("GPU");
            add_cell(0, row, name, "ov-name", "ov-gpu");
            g_free(name);
            col = 1;
            if (on[OV_GPU_TEMP])  add_temp(col++, row, tempfm_device_main_sensor(d));
            if (on[OV_GPU_USAGE]) add_pct(col++, row, d->gpu_usage);
            if (on[OV_GPU_VRAM]) {
                if (d->vram_total > 0) add_used(col++, row, d->vram_used / 1024.0, d->vram_total / 1024.0);
                else add_value(col++, row, "\xE2\x80\x94", NULL, LEVEL_UNKNOWN);
            }
            row++;
        }
    }

    if (on[OV_DISK_TEMP] || on[OV_DISK_ACTIVITY]) {
        add_cell(0, row, "Disk", "ov-name", "ov-disk");
        col = 1;
        if (on[OV_DISK_TEMP])     add_temp(col++, row, hottest(devs, CAT_STORAGE));
        if (on[OV_DISK_ACTIVITY]) add_pct(col++, row, ss->disk_busy);
        row++;
    }

    if (on[OV_RAM] && ss->ram_total > 0) {
        add_cell(0, row, "RAM", "ov-name", "ov-ram");
        add_used(1, row, ss->ram_used / GIB, tempfm_ram_gb());
        add_pct(2, row, 100.0 * ss->ram_used / ss->ram_total);
        row++;
    }

    if (on[OV_SWAP]) {
        add_cell(0, row, "Swap", "ov-name", "ov-swap");
        if (ss->swap_total > 0) {
            add_used(1, row, ss->swap_used / GIB, ss->swap_total / GIB);
            add_pct(2, row, 100.0 * ss->swap_used / ss->swap_total);
        } else {
            add_value(1, row, "off", NULL, LEVEL_UNKNOWN);
        }
        row++;
    }

    if (on[OV_FANS]) {
        double top = -1;
        for (guint i = 0; i < devs->len; i++) {
            Device *d = devs->pdata[i];
            if (d->category != CAT_FANS) continue;
            for (guint j = 0; j < d->sensors->len; j++) {
                Sensor *s = d->sensors->pdata[j];
                if (s->valid && s->value > top) top = s->value;
            }
        }
        if (top >= 0) {
            add_cell(0, row, "Fan", "ov-name", "ov-fans");
            char *t = g_strdup_printf("%.0f", top);
            add_value(1, row, t, "RPM", LEVEL_UNKNOWN);
            g_free(t);
            row++;
        }
    }

    if (on[OV_BATTERY] && ss->battery >= 0) {
        add_cell(0, row, "Bat", "ov-name", "ov-bat");
        char *t = g_strdup_printf("%d", ss->battery);
        add_value(1, row, t, ss->charging ? "% \xE2\x9A\xA1" : "%", LEVEL_UNKNOWN);
        g_free(t);
        row++;
    }

    if (on[OV_TIME]) {
        GDateTime *now = g_date_time_new_now_local();
        char *t = g_date_time_format(now, "%H:%M");
        add_cell(0, row, "Time", "ov-name", "ov-time");
        add_value(1, row, t, NULL, LEVEL_UNKNOWN);
        g_free(t);
        g_date_time_unref(now);
        row++;
    }

    /* nothing to show: no empty box on screen */
    gtk_widget_set_visible(ov_win, row > 0);
    return row;
}


/* ── placement ── */

static void
place(void)
{
#ifdef TEMPFM_HAVE_X11
    if (!ov_win || !ov_x11) return;
    GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(ov_win));
    if (!surf) return;

    TempfmSettings *st = tempfm_settings();
    GListModel *mons = gdk_display_get_monitors(ov_disp);
    guint n = g_list_model_get_n_items(mons);
    if (!n) return;
    GdkMonitor *m = g_list_model_get_item(mons, MIN((guint)st->ov_monitor, n - 1));
    GdkRectangle g;
    gdk_monitor_get_geometry(m, &g);
    g_object_unref(m);

    int w = gdk_surface_get_width(surf), h = gdk_surface_get_height(surf);
    const int margin = 12;
    int x = (st->ov_corner == CORNER_TOP_RIGHT || st->ov_corner == CORNER_BOTTOM_RIGHT)
          ? g.x + g.width - w - margin : g.x + margin;
    int y = (st->ov_corner == CORNER_BOTTOM_LEFT || st->ov_corner == CORNER_BOTTOM_RIGHT)
          ? g.y + g.height - h - margin : g.y + margin;
    int scale = gdk_surface_get_scale_factor(surf);

    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    Display *xd = gdk_x11_display_get_xdisplay(ov_disp);
    XMoveWindow(xd, gdk_x11_surface_get_xid(surf), x * scale, y * scale);
    XRaiseWindow(xd, gdk_x11_surface_get_xid(surf));
    XFlush(xd);
    G_GNUC_END_IGNORE_DEPRECATIONS
#endif
}

static void
on_layout(GdkSurface *s, int w, int h, gpointer ud)
{
    (void)s; (void)w; (void)h; (void)ud;
    place();
}

static void
load_css(void)
{
    TempfmSettings *st = tempfm_settings();
    static const int sizes[] = { 11, 13, 17 };
    int size = sizes[CLAMP(st->ov_size, 0, 2)];

    char alpha[16], sz[8], small[8];
    g_ascii_formatd(alpha, sizeof alpha, "%.2f", st->ov_opacity / 100.0);
    g_snprintf(sz, sizeof sz, "%d", size);
    g_snprintf(small, sizeof small, "%d", size - 2);

    GString *css = g_string_new(OV_CSS);
    g_string_replace(css, "{alpha}", alpha, 0);
    g_string_replace(css, "{size}",  sz,    0);
    g_string_replace(css, "{small}", small, 0);

    /* reloading restyles the overlay: only when something changed */
    static char *loaded = NULL;
    if (ov_css && !g_strcmp0(loaded, css->str)) {
        g_string_free(css, TRUE);
        return;
    }
    g_free(loaded);
    loaded = g_strdup(css->str);
    if (!ov_css) {
        ov_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(ov_disp,
            GTK_STYLE_PROVIDER(ov_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    gtk_css_provider_load_from_string(ov_css, css->str);
    g_string_free(css, TRUE);
}

static void
on_monitor(gboolean rebuilt, gpointer ud)
{
    (void)rebuilt; (void)ud;
    overlay_fill();
}

/* The fallback window can be closed by the compositor (Alt+F4): forget
 * it, so nothing uses a destroyed window. */
static void
on_overlay_destroy(GtkWidget *w, gpointer ud)
{
    (void)ud;
    if (w != ov_win) return;
    tempfm_monitor_unlisten(ov_listen);
    ov_listen = 0;
    ov_win  = NULL;
    ov_grid = NULL;
}

static void
overlay_create(void)
{
    GdkDisplay *d = overlay_display();
    if (!d) return;
    load_css();

    ov_win = gtk_window_new();
    gtk_window_set_display(GTK_WINDOW(ov_win), d);
    gtk_window_set_title(GTK_WINDOW(ov_win), "TempeR overlay");
    gtk_window_set_decorated(GTK_WINDOW(ov_win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(ov_win), FALSE);
    gtk_widget_add_css_class(ov_win, "tempfm-overlay");
    g_signal_connect(ov_win, "destroy", G_CALLBACK(on_overlay_destroy), NULL);
    gtk_widget_set_can_focus(ov_win, FALSE);
    gtk_widget_set_can_target(ov_win, FALSE);

    ov_grid = gtk_grid_new();
    gtk_widget_add_css_class(ov_grid, "ov-grid");
    gtk_grid_set_row_spacing(GTK_GRID(ov_grid), 0);
    if (ov_x11) {
        gtk_window_set_child(GTK_WINDOW(ov_win), ov_grid);
    } else {
        /* a normal window: let it be dragged around */
        GtkWidget *handle = gtk_window_handle_new();
        gtk_window_handle_set_child(GTK_WINDOW_HANDLE(handle), ov_grid);
        gtk_window_set_child(GTK_WINDOW(ov_win), handle);
        gtk_widget_set_can_target(ov_win, TRUE);
    }

    gtk_widget_realize(ov_win);
    GdkSurface *surf = gtk_native_get_surface(GTK_NATIVE(ov_win));

#ifdef TEMPFM_HAVE_X11
    if (ov_x11) {
        /* unmanaged: above every window, never focused, no title bar */
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        XSetWindowAttributes at = { .override_redirect = True };
        XChangeWindowAttributes(gdk_x11_display_get_xdisplay(d),
            gdk_x11_surface_get_xid(surf), CWOverrideRedirect, &at);
        G_GNUC_END_IGNORE_DEPRECATIONS

        /* click-through */
        cairo_region_t *empty = cairo_region_create();
        gdk_surface_set_input_region(surf, empty);
        cairo_region_destroy(empty);
        g_signal_connect(surf, "layout", G_CALLBACK(on_layout), NULL);
    }
#endif
    (void)surf;

    if (overlay_fill() > 0) {
        gtk_window_present(GTK_WINDOW(ov_win));
        place();
    }
    ov_listen = tempfm_monitor_listen(on_monitor, NULL);
}

static void
overlay_destroy(void)
{
    if (!ov_win) return;
    gtk_window_destroy(GTK_WINDOW(ov_win));     /* on_overlay_destroy clears */
}

void
tempfm_overlay_apply(void)
{
    TempfmSettings *st = tempfm_settings();
    if (!st->ov_enabled) {
        overlay_destroy();
        return;
    }
    if (!ov_win) {
        overlay_create();
        return;
    }
    load_css();
    overlay_fill();
    place();
}
