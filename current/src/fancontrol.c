/* fancontrol.c – the Fan control window: fan curves as in ASUS Armoury
 * Crate.  Every fan gets 8 points "at this temperature, this speed" that
 * are dragged on a graph; the fan follows the line between them.
 *
 * TempeR itself never writes to the hardware: the curves are handed to
 * the temper-fand service (system D-Bus, com.girottos.TempeR.FanControl),
 * which runs as root, checks with polkit that the user may change them,
 * applies them, keeps them applied and keeps the other fan control
 * programs out of the way while they are on (see daemon/temper-fand.c).
 *
 *   ┌ Fan control ─────────────────────────────────────────── X ┐
 *   │ Use TempeR's fan curves                              [on] │
 *   │ [ CPU fan ][ GPU fan ]  Silent Standard Turbo … My presets│
 *   │  100% ┤                                     ●───          │
 *   │       │                            ●───●╱                 │
 *   │       │              ●────●───●╱                          │
 *   │    0% ┼──●────●╱──────┬─────────────────────── °C         │
 *   │ Point 4: 60 °C → 40 %            CPU 62 °C · 3900 RPM     │
 *   │                                  Reset   Apply   Close    │
 *   └───────────────────────────────────────────────────────────┘
 *
 * TempeR's presets can't be changed; "My presets → New preset…" makes a
 * copy of one of them, that is edited on the graph and saved again from
 * the same menu (presets.ini, next to settings.ini).
 */

#include "tempfm.h"

#include <math.h>

#define FAN_BUS     "com.girottos.TempeR.FanControl"
#define FAN_PATH    "/com/girottos/TempeR/FanControl"
#define FAN_IFACE   "com.girottos.TempeR.FanControl"

#define N_POINTS    8
#define T_MIN       10
#define T_MAX       110
/* the service never lets the fans go below 50 % from 85 °C up */
#define SAFE_TEMP   85
#define SAFE_PCT    50

/* graph margins, px */
#define G_LEFT      42
#define G_RIGHT     14
#define G_TOP       12
#define G_BOTTOM    26
#define HIT_RADIUS  12

typedef struct {
    char   *id, *label, *follows;
    guint8  temp[N_POINTS], pwm[N_POINTS];          /* on the service    */
    guint8  def_temp[N_POINTS], def_pwm[N_POINTS];  /* its default       */
    double  et[N_POINTS], ep[N_POINTS];             /* edited: °C and %  */
} FanCurve;

typedef struct {
    char   *name;
    double  t[N_POINTS], p[N_POINTS];               /* °C and %          */
} UserPreset;

typedef struct {
    GtkWidget    *win, *sw, *paused, *tabs, *graph, *readout, *now, *status;
    GtkWidget    *btn_apply, *btn_reset, *body, *mine_box;
    GPtrArray    *user;         /* UserPreset*: the user's own presets */
    int           active_user;  /* the one last loaded, -1 none */
    GPtrArray    *fans;         /* FanCurve* */
    int           current;      /* fan shown on the graph */
    int           drag, hover, selected;
    double        drag_x, drag_y;
    gboolean      enabled, loaded, building;
    guint         listen_id, sub_id;
    GDBusConnection *sys;
    GCancellable *cancel;
} FanUI;

static FanUI *fui = NULL;       /* only one window */

static void request_state(void);
static void sync_widgets(void);


/* ── conversions ── */

static double pwm_to_pct(guint8 p) { return p * 100.0 / 255.0; }
static guint8 pct_to_pwm(double p) { return (guint8)CLAMP(lround(p * 255.0 / 100.0), 0, 255); }

static void
fan_curve_free(gpointer p)
{
    FanCurve *f = p;
    g_free(f->id);
    g_free(f->label);
    g_free(f->follows);
    g_free(f);
}

static void
edit_from(FanCurve *f, const guint8 *t, const guint8 *p)
{
    for (int i = 0; i < N_POINTS; i++) {
        f->et[i] = t[i];
        f->ep[i] = round(pwm_to_pct(p[i]));
    }
}

static gboolean
fan_dirty(FanCurve *f)
{
    for (int i = 0; i < N_POINTS; i++)
        if (lround(f->et[i]) != f->temp[i] || pct_to_pwm(f->ep[i]) != f->pwm[i])
            return TRUE;
    return FALSE;
}

static gboolean
any_dirty(void)
{
    for (guint i = 0; fui->fans && i < fui->fans->len; i++)
        if (fan_dirty(fui->fans->pdata[i])) return TRUE;
    return FALSE;
}

static FanCurve *
cur_fan(void)
{
    if (!fui->fans || !fui->fans->len) return NULL;
    return fui->fans->pdata[CLAMP(fui->current, 0, (int)fui->fans->len - 1)];
}

/* the same rules as the service, so what is drawn is what is applied */
static void
fix_curve(FanCurve *f)
{
    for (int i = 0; i < N_POINTS; i++) {
        f->et[i] = CLAMP(round(f->et[i]), T_MIN, T_MAX - (N_POINTS - 1 - i));
        if (i > 0 && f->et[i] <= f->et[i - 1]) f->et[i] = f->et[i - 1] + 1;
        f->ep[i] = CLAMP(round(f->ep[i]), 0, 100);
        if (f->et[i] >= SAFE_TEMP || i == N_POINTS - 1) f->ep[i] = MAX(f->ep[i], SAFE_PCT);
        if (i > 0 && f->ep[i] < f->ep[i - 1]) f->ep[i] = f->ep[i - 1];
    }
}

/* Drags point i.  Its temperature stays between its neighbours; its speed
 * is free and pushes the points after it up (or the ones before it down),
 * so the line never goes down while it gets hotter. */
static void
set_point(FanCurve *f, int i, double t, double p)
{
    double lo = i == 0 ? T_MIN : f->et[i - 1] + 1;
    double hi = i == N_POINTS - 1 ? T_MAX : f->et[i + 1] - 1;
    t = CLAMP(round(t), lo, hi);
    p = CLAMP(round(p), 0, 100);
    if (t >= SAFE_TEMP || i == N_POINTS - 1) p = MAX(p, SAFE_PCT);
    f->et[i] = t;
    f->ep[i] = p;
    for (int k = i + 1; k < N_POINTS; k++) if (f->ep[k] < p) f->ep[k] = p;
    for (int k = i - 1; k >= 0; k--)       if (f->ep[k] > p) f->ep[k] = p;
    fix_curve(f);
}


/* ── presets (applied to every fan) ── */

typedef struct {
    const char *name, *tip;
    double t[N_POINTS], p[N_POINTS];
} Preset;

static const Preset PRESETS[] = {
    { "Silent",     "Quiet: the fans stay slow until the computer gets hot",
      { 40, 50, 60, 70, 75, 80, 85, 95 },  {  0,  5, 15, 25, 35, 45, 55,  80 } },
    { "Standard",   "Balanced between noise and temperature",
      { 30, 40, 50, 60, 70, 80, 90, 100 }, { 10, 20, 30, 40, 50, 65, 80, 100 } },
    { "Turbo",      "Cool: the fans speed up early",
      { 30, 40, 50, 60, 70, 80, 90, 100 }, { 25, 35, 45, 60, 75, 90, 100, 100 } },
    { "Full speed", "Always at 100 %",
      { 30, 40, 50, 60, 70, 80, 90, 100 }, { 100, 100, 100, 100, 100, 100, 100, 100 } },
};


/* ── the user's own presets ── */

static void
user_preset_free(gpointer p)
{
    UserPreset *u = p;
    g_free(u->name);
    g_free(u);
}

static char *
presets_path(void)
{
    char *dir = g_path_get_dirname(tempfm_settings_path());
    char *p = g_build_filename(dir, "presets.ini", NULL);
    g_free(dir);
    return p;
}

static GPtrArray *
presets_load(void)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(user_preset_free);
    char *path = presets_path();
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        char **groups = g_key_file_get_groups(kf, NULL);
        for (int i = 0; groups[i]; i++) {
            gsize nt = 0, np = 0;
            int *t = g_key_file_get_integer_list(kf, groups[i], "temperatures", &nt, NULL);
            int *pc = g_key_file_get_integer_list(kf, groups[i], "speeds", &np, NULL);
            if (t && pc && nt == N_POINTS && np == N_POINTS) {
                UserPreset *u = g_new0(UserPreset, 1);
                u->name = g_strdup(groups[i]);
                for (int k = 0; k < N_POINTS; k++) { u->t[k] = t[k]; u->p[k] = pc[k]; }
                g_ptr_array_add(a, u);
            }
            g_free(t);
            g_free(pc);
        }
        g_strfreev(groups);
    }
    g_key_file_free(kf);
    g_free(path);
    return a;
}

static gboolean
presets_save(void)
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_comment(kf, NULL, NULL,
        " TempeR: your own fan curve presets.\n"
        " temperatures in \xC2\xB0" "C, speeds in %, 8 points each.", NULL);
    for (guint i = 0; i < fui->user->len; i++) {
        UserPreset *u = fui->user->pdata[i];
        int t[N_POINTS], p[N_POINTS];
        for (int k = 0; k < N_POINTS; k++) { t[k] = (int)lround(u->t[k]); p[k] = (int)lround(u->p[k]); }
        g_key_file_set_integer_list(kf, u->name, "temperatures", t, N_POINTS);
        g_key_file_set_integer_list(kf, u->name, "speeds", p, N_POINTS);
    }
    char *path = presets_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    GError *err = NULL;
    gboolean ok = g_key_file_save_to_file(kf, path, &err);
    if (!ok) {
        g_printerr("TempeR: cannot save %s: %s\n", path, err->message);
        g_error_free(err);
    }
    g_free(dir);
    g_free(path);
    g_key_file_free(kf);
    return ok;
}


/* ── current readings, for the graph ── */

static Sensor *
hottest_of(SensorCategory c)
{
    GPtrArray *devs = tempfm_monitor_devices();
    Sensor *best = NULL;
    for (guint i = 0; i < devs->len; i++) {
        Device *d = devs->pdata[i];
        if (d->category != c) continue;
        Sensor *s = tempfm_device_main_sensor(d);
        if (s && s->valid && (!best || s->value > best->value)) best = s;
    }
    return best;
}

/* the temperature this fan follows, < -100 when unknown */
static double
followed_temp(FanCurve *f)
{
    Sensor *s = NULL;
    if (strstr(f->follows, "GPU"))         s = hottest_of(CAT_GPU);
    else if (strstr(f->follows, "System")) s = hottest_of(CAT_SYSTEM);
    else                                   s = hottest_of(CAT_CPU);
    return s ? s->value : -1000;
}

/* The fan's own RPM sensor: "nct6798-pwm2" is fan2 of the nct6798 chip,
 * "asus-cpu/gpu/mid" are fan1/2/3 of the "asus" chip.  By name only when
 * neither fits. */
static double
fan_rpm(FanCurve *f)
{
    char drv[64] = "";
    int n = 0;
    const char *dash = strrchr(f->id, '-');
    if (dash && sscanf(dash, "-pwm%d", &n) == 1) {
        g_strlcpy(drv, f->id, MIN(sizeof drv, (gsize)(dash - f->id) + 1));
    } else if (g_str_has_prefix(f->id, "asus-")) {
        g_strlcpy(drv, "asus", sizeof drv);
        n = !strcmp(f->id, "asus-cpu") ? 1 : !strcmp(f->id, "asus-gpu") ? 2 : 3;
    }
    char want[32];
    g_snprintf(want, sizeof want, "/fan%d_input", n);

    GPtrArray *devs = tempfm_monitor_devices();
    Sensor *by_label = NULL;
    for (guint i = 0; i < devs->len; i++) {
        Device *d = devs->pdata[i];
        if (d->category != CAT_FANS) continue;
        for (guint k = 0; k < d->sensors->len; k++) {
            Sensor *s = d->sensors->pdata[k];
            if (!s->valid) continue;
            if (n && !g_strcmp0(d->source, drv) && s->input && g_str_has_suffix(s->input, want))
                return s->value;
            if (!by_label && !g_ascii_strcasecmp(s->label, f->label)) by_label = s;
        }
    }
    return by_label ? by_label->value : -1;
}

static double
to_unit(double c)
{
    return tempfm_settings()->unit == UNIT_FAHRENHEIT ? c * 9.0 / 5.0 + 32.0 : c;
}


/* ════════════════════════════════════════════════════════════
 * GRAPH
 * ════════════════════════════════════════════════════════════ */

typedef struct { double x0, y0, w, h; } Box;

static Box
graph_box(GtkWidget *w)
{
    Box b = { G_LEFT, G_TOP,
              MAX(gtk_widget_get_width(w) - G_LEFT - G_RIGHT, 10),
              MAX(gtk_widget_get_height(w) - G_TOP - G_BOTTOM, 10) };
    return b;
}

static double gx(const Box *b, double t) { return b->x0 + (t - T_MIN) / (double)(T_MAX - T_MIN) * b->w; }
static double gy(const Box *b, double p) { return b->y0 + b->h - p / 100.0 * b->h; }
static double gt(const Box *b, double x) { return T_MIN + (x - b->x0) / b->w * (T_MAX - T_MIN); }
static double gp(const Box *b, double y) { return (b->y0 + b->h - y) / b->h * 100.0; }

static void
accent_color(GdkRGBA *c)
{
    gdk_rgba_parse(c, tempfm_settings()->theme == THEME_PURPLE ? "#6d4ed6" : "#0078d7");
}

static void
draw_text(cairo_t *cr, GtkWidget *w, const char *txt, double x, double y,
          double xalign, double yalign, const GdkRGBA *color)
{
    PangoLayout *l = gtk_widget_create_pango_layout(w, txt);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 8");
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    int tw, th;
    pango_layout_get_pixel_size(l, &tw, &th);
    gdk_cairo_set_source_rgba(cr, color);
    cairo_move_to(cr, x - tw * xalign, y - th * yalign);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
}

static void
draw_graph(GtkDrawingArea *da, cairo_t *cr, int width, int height, gpointer ud)
{
    (void)width; (void)height; (void)ud;
    GtkWidget *w = GTK_WIDGET(da);
    Box b = graph_box(w);
    GdkRGBA fg, accent, dim, grid;
    gtk_widget_get_color(w, &fg);
    accent_color(&accent);
    dim = fg;  dim.alpha = 0.65;
    grid = fg; grid.alpha = 0.10;

    /* safety zone: below 50 % from 85 °C the service refuses to go */
    cairo_set_source_rgba(cr, 0.91, 0.07, 0.14, 0.15);
    cairo_rectangle(cr, gx(&b, SAFE_TEMP), gy(&b, SAFE_PCT),
                    gx(&b, T_MAX) - gx(&b, SAFE_TEMP), gy(&b, 0) - gy(&b, SAFE_PCT));
    cairo_fill(cr);

    /* grid and axis labels */
    cairo_set_line_width(cr, 1);
    for (int t = T_MIN; t <= T_MAX; t += 10) {
        double x = round(gx(&b, t)) + 0.5;
        gdk_cairo_set_source_rgba(cr, &grid);
        cairo_move_to(cr, x, b.y0);
        cairo_line_to(cr, x, b.y0 + b.h);
        cairo_stroke(cr);
        char s[16];
        g_snprintf(s, sizeof s, "%.0f", to_unit(t));
        draw_text(cr, w, s, x, b.y0 + b.h + 4, 0.5, 0, &dim);
    }
    for (int p = 0; p <= 100; p += 10) {
        double y = round(gy(&b, p)) + 0.5;
        gdk_cairo_set_source_rgba(cr, &grid);
        cairo_move_to(cr, b.x0, y);
        cairo_line_to(cr, b.x0 + b.w, y);
        cairo_stroke(cr);
        if (p % 20 == 0) {
            char s[16];
            g_snprintf(s, sizeof s, "%d %%", p);
            draw_text(cr, w, s, b.x0 - 6, y, 1, 0.5, &dim);
        }
    }
    draw_text(cr, w, tempfm_settings()->unit == UNIT_FAHRENHEIT ? "\xC2\xB0" "F" : "\xC2\xB0" "C",
              b.x0 - 12, b.y0 + b.h + 4, 1, 0, &dim);

    FanCurve *f = cur_fan();
    if (!f) return;

    /* current temperature of what this fan follows */
    double now = followed_temp(f);
    if (now > T_MIN && now < T_MAX) {
        double x = round(gx(&b, now)) + 0.5;
        GdkRGBA hot;
        gdk_rgba_parse(&hot, "#f7630c");
        gdk_cairo_set_source_rgba(cr, &hot);
        double dash[] = { 4, 3 };
        cairo_set_dash(cr, dash, 2, 0);
        cairo_move_to(cr, x, b.y0);
        cairo_line_to(cr, x, b.y0 + b.h);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
        char *t = tempfm_format_temp(now);
        draw_text(cr, w, t, x + 4, b.y0, 0, 0, &hot);
        g_free(t);
    }

    /* the curve: flat before the first point and after the last one */
    cairo_move_to(cr, gx(&b, T_MIN), gy(&b, f->ep[0]));
    for (int i = 0; i < N_POINTS; i++)
        cairo_line_to(cr, gx(&b, f->et[i]), gy(&b, f->ep[i]));
    cairo_line_to(cr, gx(&b, T_MAX), gy(&b, f->ep[N_POINTS - 1]));
    cairo_path_t *line = cairo_copy_path(cr);
    cairo_line_to(cr, gx(&b, T_MAX), gy(&b, 0));
    cairo_line_to(cr, gx(&b, T_MIN), gy(&b, 0));
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue, 0.16);
    cairo_fill(cr);
    cairo_append_path(cr, line);
    cairo_path_destroy(line);
    gdk_cairo_set_source_rgba(cr, &accent);
    cairo_set_line_width(cr, 2);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);

    /* the points */
    for (int i = 0; i < N_POINTS; i++) {
        gboolean big = i == fui->drag || i == fui->hover || i == fui->selected;
        double x = gx(&b, f->et[i]), y = gy(&b, f->ep[i]);
        cairo_arc(cr, x, y, big ? 6.5 : 5, 0, 2 * G_PI);
        if (big) gdk_cairo_set_source_rgba(cr, &accent);
        else     cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill_preserve(cr);
        gdk_cairo_set_source_rgba(cr, big ? &fg : &accent);
        cairo_set_line_width(cr, big ? 1.5 : 2);
        cairo_stroke(cr);
    }
}

static int
point_at(double x, double y)
{
    FanCurve *f = cur_fan();
    if (!f) return -1;
    Box b = graph_box(fui->graph);
    int best = -1;
    double bd = HIT_RADIUS * HIT_RADIUS;
    for (int i = 0; i < N_POINTS; i++) {
        double dx = gx(&b, f->et[i]) - x, dy = gy(&b, f->ep[i]) - y;
        double d = dx * dx + dy * dy;
        if (d <= bd) { bd = d; best = i; }
    }
    return best;
}

static void
update_readout(void)
{
    FanCurve *f = cur_fan();
    int i = fui->drag >= 0 ? fui->drag : fui->hover >= 0 ? fui->hover : fui->selected;
    if (!f) {
        gtk_label_set_text(GTK_LABEL(fui->readout), "");
        gtk_label_set_text(GTK_LABEL(fui->now), "");
        return;
    }
    if (i >= 0) {
        char *t = tempfm_format_temp(f->et[i]);
        char *s = g_strdup_printf("Point %d:  %s  \xE2\x86\x92  %.0f %%", i + 1, t, f->ep[i]);
        gtk_label_set_text(GTK_LABEL(fui->readout), s);
        g_free(s);
        g_free(t);
    } else {
        /* "Drag the points. The CPU fan follows the CPU temperature." */
        char *s = g_strdup_printf("Drag the points. The %s follows the %s.",
                                  f->label, f->follows);
        gtk_label_set_text(GTK_LABEL(fui->readout), s);
        g_free(s);
    }

    GString *n = g_string_new(NULL);
    double t = followed_temp(f), rpm = fan_rpm(f);
    if (t > -100) {
        char *ts = tempfm_format_temp(t);
        g_string_append_printf(n, "Now %s", ts);
        g_free(ts);
    }
    if (rpm >= 0)
        g_string_append_printf(n, "%s%.0f RPM", n->len ? "  \xC2\xB7  " : "", rpm);
    gtk_label_set_text(GTK_LABEL(fui->now), n->str);
    g_string_free(n, TRUE);
}

static void
on_drag_begin(GtkGestureDrag *g, double x, double y, gpointer ud)
{
    (void)ud;
    int i = point_at(x, y);
    if (i < 0) {
        gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }
    fui->drag = fui->selected = i;
    fui->drag_x = x;
    fui->drag_y = y;
    gtk_widget_queue_draw(fui->graph);
    update_readout();
}

static void
on_drag_update(GtkGestureDrag *g, double dx, double dy, gpointer ud)
{
    (void)g; (void)ud;
    FanCurve *f = cur_fan();
    if (!f || fui->drag < 0) return;
    Box b = graph_box(fui->graph);
    set_point(f, fui->drag, gt(&b, fui->drag_x + dx), gp(&b, fui->drag_y + dy));
    gtk_widget_queue_draw(fui->graph);
    update_readout();
    sync_widgets();
}

static void
on_drag_end(GtkGestureDrag *g, double dx, double dy, gpointer ud)
{
    (void)g; (void)dx; (void)dy; (void)ud;
    fui->drag = -1;
    gtk_widget_queue_draw(fui->graph);
    update_readout();
    sync_widgets();
}

static void
on_motion(GtkEventControllerMotion *m, double x, double y, gpointer ud)
{
    (void)m; (void)ud;
    int i = point_at(x, y);
    gtk_widget_set_cursor_from_name(fui->graph, i >= 0 ? "grab" : NULL);
    if (i != fui->hover) {
        fui->hover = i;
        gtk_widget_queue_draw(fui->graph);
        update_readout();
    }
}

static void
on_leave(GtkEventControllerMotion *m, gpointer ud)
{
    (void)m; (void)ud;
    if (fui->hover >= 0) {
        fui->hover = -1;
        gtk_widget_queue_draw(fui->graph);
        update_readout();
    }
}


/* ════════════════════════════════════════════════════════════
 * TALKING TO temper-fand
 * ════════════════════════════════════════════════════════════ */

static void
set_status(const char *msg, gboolean error)
{
    gtk_label_set_text(GTK_LABEL(fui->status), msg ? msg : "");
    if (error) gtk_widget_add_css_class(fui->status, "lvl-crit");
    else       gtk_widget_remove_css_class(fui->status, "lvl-crit");
}

static void
show_error(GError *err)
{
    if (g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
        g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
        g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_SERVICE_NOT_FOUND))
        set_status("The fan control service (temper-fand) is not installed: "
                   "install TempeR's package to change the fan curves.", TRUE);
    else if (g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED) ||
             g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_AUTH_FAILED))
        set_status("Not allowed to change the fan curves.", TRUE);
    else {
        g_dbus_error_strip_remote_error(err);
        set_status(err->message, TRUE);
    }
}

static void on_tab(GtkToggleButton *b, gpointer ud);

static void
fill_tabs(void)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(fui->tabs)) != NULL)
        gtk_box_remove(GTK_BOX(fui->tabs), c);
    GtkWidget *first = NULL;
    for (guint i = 0; i < fui->fans->len; i++) {
        FanCurve *f = fui->fans->pdata[i];
        GtkWidget *b = gtk_toggle_button_new_with_label(f->label);
        gtk_widget_add_css_class(b, "cmd-btn");
        gtk_widget_set_tooltip_text(b, f->follows);
        if (first) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(first));
        else       first = b;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), (int)i == fui->current);
        g_object_set_data(G_OBJECT(b), "fan", GINT_TO_POINTER((int)i));
        g_signal_connect(b, "toggled", G_CALLBACK(on_tab), NULL);
        gtk_box_append(GTK_BOX(fui->tabs), b);
    }
}

/* r: the answer of GetState */
static void
apply_state(GVariant *r)
{
    gboolean was_dirty = fui->loaded && any_dirty();
    GVariantIter *fit, *pit;
    g_variant_get(r, "(ba(sssayayayay)as)", &fui->enabled, &fit, &pit);

    GPtrArray *old = fui->fans;
    fui->fans = g_ptr_array_new_with_free_func(fan_curve_free);
    const char *id, *label, *follows;
    GVariant *t, *p, *dt, *dp;
    while (g_variant_iter_next(fit, "(&s&s&s@ay@ay@ay@ay)", &id, &label, &follows,
                               &t, &p, &dt, &dp)) {
        FanCurve *f = g_new0(FanCurve, 1);
        f->id = g_strdup(id);
        f->label = g_strdup(label);
        f->follows = g_strdup(follows);
        gsize n;
        const guint8 *a;
        a = g_variant_get_fixed_array(t, &n, 1);  if (n == N_POINTS) memcpy(f->temp, a, N_POINTS);
        a = g_variant_get_fixed_array(p, &n, 1);  if (n == N_POINTS) memcpy(f->pwm, a, N_POINTS);
        a = g_variant_get_fixed_array(dt, &n, 1); if (n == N_POINTS) memcpy(f->def_temp, a, N_POINTS);
        a = g_variant_get_fixed_array(dp, &n, 1); if (n == N_POINTS) memcpy(f->def_pwm, a, N_POINTS);
        edit_from(f, f->temp, f->pwm);
        /* keep what the user is editing and has not applied yet */
        for (guint k = 0; was_dirty && old && k < old->len; k++) {
            FanCurve *o = old->pdata[k];
            if (!strcmp(o->id, f->id)) {
                memcpy(f->et, o->et, sizeof f->et);
                memcpy(f->ep, o->ep, sizeof f->ep);
            }
        }
        g_ptr_array_add(fui->fans, f);
        g_variant_unref(t); g_variant_unref(p);
        g_variant_unref(dt); g_variant_unref(dp);
    }
    g_variant_iter_free(fit);

    GString *ps = g_string_new(NULL);
    const char *name;
    while (g_variant_iter_next(pit, "&s", &name))
        g_string_append_printf(ps, "%s%s", ps->len ? ", " : "", name);
    g_variant_iter_free(pit);

    gboolean same_fans = old && old->len == fui->fans->len;
    if (old) g_ptr_array_unref(old);
    fui->loaded = TRUE;
    if (!same_fans) {
        fui->current = 0;
        fill_tabs();
    }

    fui->building = TRUE;
    gtk_switch_set_active(GTK_SWITCH(fui->sw), fui->enabled);
    fui->building = FALSE;

    char *note;
    if (!fui->fans->len)
        note = g_strdup("No fan on this computer can be controlled.");
    else if (!fui->enabled)
        note = g_strdup("Off: the fans follow the computer's own settings. "
                        "While TempeR's curves are on, other fan control programs are paused.");
    else if (ps->len)
        note = g_strdup_printf("On. Paused while the curves are on: %s.", ps->str);
    else
        note = g_strdup("On. TempeR keeps these curves applied, also after a power "
                        "profile change or a resume.");
    gtk_label_set_text(GTK_LABEL(fui->paused), note);
    g_free(note);
    g_string_free(ps, TRUE);

    gtk_widget_set_sensitive(fui->body, fui->fans->len > 0);
    gtk_widget_set_sensitive(fui->sw, fui->fans->len > 0);
    sync_widgets();
    gtk_widget_queue_draw(fui->graph);
    update_readout();
}

static void
on_state(GObject *src, GAsyncResult *res, gpointer ud)
{
    (void)ud;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!r) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            show_error(err);
            gtk_widget_set_sensitive(fui->body, FALSE);
        }
        g_error_free(err);
        return;
    }
    apply_state(r);
    g_variant_unref(r);
}

static void
request_state(void)
{
    if (!fui || !fui->sys) return;
    g_dbus_connection_call(fui->sys, FAN_BUS, FAN_PATH, FAN_IFACE, "GetState", NULL,
        G_VARIANT_TYPE("(ba(sssayayayay)as)"), G_DBUS_CALL_FLAGS_NONE, 10000,
        fui->cancel, on_state, NULL);
}

static void
on_changed_signal(GDBusConnection *c, const char *sender, const char *path,
                  const char *iface, const char *signal, GVariant *params, gpointer ud)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)signal; (void)params; (void)ud;
    request_state();
}

/* a write finished: "next" is the method to call after it, if any */
static void
on_written(GObject *src, GAsyncResult *res, gpointer ud)
{
    char *next = ud;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!r) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            show_error(err);
            request_state();            /* the switch shows the truth again */
        }
        g_error_free(err);
        g_free(next);
        return;
    }
    g_variant_unref(r);
    if (next && !strcmp(next, "enable")) {
        g_dbus_connection_call(fui->sys, FAN_BUS, FAN_PATH, FAN_IFACE, "SetEnabled",
            g_variant_new("(b)", TRUE), NULL,
            G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT,
            fui->cancel, on_written, NULL);
    } else {
        set_status(NULL, FALSE);
        request_state();
    }
    g_free(next);
}

static void
send_curves(gboolean then_enable)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a(sayay)"));
    for (guint i = 0; i < fui->fans->len; i++) {
        FanCurve *f = fui->fans->pdata[i];
        guint8 t[N_POINTS], p[N_POINTS];
        for (int k = 0; k < N_POINTS; k++) {
            t[k] = (guint8)lround(f->et[k]);
            p[k] = pct_to_pwm(f->ep[k]);
        }
        g_variant_builder_add(&b, "(s@ay@ay)", f->id,
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, t, N_POINTS, 1),
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, p, N_POINTS, 1));
    }
    set_status("Applying\xE2\x80\xA6", FALSE);
    g_dbus_connection_call(fui->sys, FAN_BUS, FAN_PATH, FAN_IFACE, "SetCurves",
        g_variant_new("(a(sayay))", &b), NULL,
        G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT,
        fui->cancel, on_written, then_enable ? g_strdup("enable") : NULL);
}


/* ════════════════════════════════════════════════════════════
 * WINDOW
 * ════════════════════════════════════════════════════════════ */

static void
sync_widgets(void)
{
    gboolean dirty = fui->loaded && any_dirty();
    gtk_widget_set_sensitive(fui->btn_apply, dirty || (fui->loaded && !fui->enabled));
    gtk_button_set_label(GTK_BUTTON(fui->btn_apply),
                         fui->enabled ? "Apply" : "Apply and turn on");
    if (dirty && !gtk_widget_has_css_class(fui->status, "lvl-crit"))
        set_status("Changes not applied yet.", FALSE);
    else if (!dirty && !gtk_widget_has_css_class(fui->status, "lvl-crit"))
        set_status(NULL, FALSE);
}

static void
on_tab(GtkToggleButton *b, gpointer ud)
{
    (void)ud;
    if (!gtk_toggle_button_get_active(b)) return;
    fui->current = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "fan"));
    fui->selected = fui->hover = fui->drag = -1;
    gtk_widget_queue_draw(fui->graph);
    update_readout();
}

static void
on_preset(GtkButton *b, gpointer ud)
{
    (void)ud;
    int k = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "preset"));
    fui->active_user = -1;
    for (guint i = 0; fui->fans && i < fui->fans->len; i++) {
        FanCurve *f = fui->fans->pdata[i];
        if (k < 0) {
            edit_from(f, f->def_temp, f->def_pwm);
        } else {
            memcpy(f->et, PRESETS[k].t, sizeof f->et);
            memcpy(f->ep, PRESETS[k].p, sizeof f->ep);
        }
        fix_curve(f);
    }
    fui->selected = -1;
    gtk_widget_queue_draw(fui->graph);
    update_readout();
    sync_widgets();
}

/* one curve on every fan, as TempeR's presets do */
static void
load_curve(const double *t, const double *p)
{
    for (guint i = 0; fui->fans && i < fui->fans->len; i++) {
        FanCurve *f = fui->fans->pdata[i];
        memcpy(f->et, t, sizeof f->et);
        memcpy(f->ep, p, sizeof f->ep);
        fix_curve(f);
    }
    fui->selected = -1;
    gtk_widget_queue_draw(fui->graph);
    update_readout();
    sync_widgets();
}

static void fill_mine_menu(void);

static void
popdown(GtkWidget *w)
{
    GtkWidget *pop = gtk_widget_get_ancestor(w, GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

static void
on_user_preset(GtkButton *b, gpointer ud)
{
    (void)ud;
    popdown(GTK_WIDGET(b));
    int k = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "preset"));
    UserPreset *u = fui->user->pdata[k];
    fui->active_user = k;
    load_curve(u->t, u->p);
    fill_mine_menu();
}

static void
on_user_save(GtkButton *b, gpointer ud)
{
    (void)ud;
    popdown(GTK_WIDGET(b));
    FanCurve *f = cur_fan();
    if (!f || fui->active_user < 0) return;
    UserPreset *u = fui->user->pdata[fui->active_user];
    memcpy(u->t, f->et, sizeof u->t);
    memcpy(u->p, f->ep, sizeof u->p);
    if (presets_save()) {
        char *m = g_strdup_printf("Preset \xE2\x80\x9C%s\xE2\x80\x9D saved.", u->name);
        set_status(m, FALSE);
        g_free(m);
    } else {
        set_status("The preset could not be saved.", TRUE);
    }
}

static void
on_user_delete(GtkButton *b, gpointer ud)
{
    (void)ud;
    popdown(GTK_WIDGET(b));
    int k = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "preset"));
    g_ptr_array_remove_index(fui->user, k);
    if (fui->active_user == k)     fui->active_user = -1;
    else if (fui->active_user > k) fui->active_user--;
    presets_save();
    fill_mine_menu();
}

/* ── "New preset" dialog: a name and the TempeR preset it starts from ── */

typedef struct {
    GtkWidget *win, *name, *base, *error;
} NewPresetUI;

static void
on_new_create(GtkButton *b, gpointer ud)
{
    (void)b;
    NewPresetUI *np = ud;
    char *name = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(np->name))));
    const char *problem = NULL;
    if (!*name)
        problem = "Give the preset a name.";
    else if (strpbrk(name, "[]\n"))
        problem = "The name can't contain [ or ].";
    for (guint i = 0; !problem && i < G_N_ELEMENTS(PRESETS); i++)
        if (!g_ascii_strcasecmp(name, PRESETS[i].name) || !g_ascii_strcasecmp(name, "Default"))
            problem = "That is the name of one of TempeR's presets.";
    for (guint i = 0; !problem && i < fui->user->len; i++)
        if (!g_ascii_strcasecmp(name, ((UserPreset *)fui->user->pdata[i])->name))
            problem = "You already have a preset with this name.";
    if (problem) {
        gtk_label_set_text(GTK_LABEL(np->error), problem);
        g_free(name);
        return;
    }

    const Preset *base = &PRESETS[gtk_drop_down_get_selected(GTK_DROP_DOWN(np->base))];
    UserPreset *u = g_new0(UserPreset, 1);
    u->name = name;
    memcpy(u->t, base->t, sizeof u->t);
    memcpy(u->p, base->p, sizeof u->p);
    g_ptr_array_add(fui->user, u);
    fui->active_user = (int)fui->user->len - 1;
    presets_save();
    load_curve(u->t, u->p);
    fill_mine_menu();
    char *m = g_strdup_printf("Preset \xE2\x80\x9C%s\xE2\x80\x9D made from %s: drag the "
                              "points, then My presets \xE2\x86\x92 Save.", u->name, base->name);
    set_status(m, FALSE);
    g_free(m);
    gtk_window_destroy(GTK_WINDOW(np->win));
}

static void
on_new_destroy(GtkWidget *w, gpointer ud)
{
    (void)w;
    g_free(ud);
}

static void
on_user_new(GtkButton *b, gpointer ud)
{
    (void)ud;
    popdown(GTK_WIDGET(b));
    NewPresetUI *np = g_new0(NewPresetUI, 1);
    GtkWidget *win = gtk_window_new();
    np->win = win;
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(fui->win));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_widget_add_css_class(win, "props");
    gtk_window_set_title(GTK_WINDOW(win), "New preset");
    tempfm_window_titlebar(win, "New preset");
    g_signal_connect(win, "destroy", G_CALLBACK(on_new_destroy), np);

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(page, 12);
    gtk_widget_set_margin_end(page, 12);
    gtk_widget_set_margin_top(page, 4);
    gtk_widget_set_margin_bottom(page, 12);
    gtk_window_set_child(GTK_WINDOW(win), page);

    gtk_box_append(GTK_BOX(page), tempfm_group_title("Your preset"));
    GtkWidget *card = tempfm_card_new();
    np->name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(np->name), "My curve");
    gtk_widget_set_size_request(np->name, 170, -1);
    tempfm_card_add_widget(card, "Name", np->name);
    const char *names[G_N_ELEMENTS(PRESETS) + 1];
    for (guint i = 0; i < G_N_ELEMENTS(PRESETS); i++) names[i] = PRESETS[i].name;
    names[G_N_ELEMENTS(PRESETS)] = NULL;
    np->base = gtk_drop_down_new_from_strings(names);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(np->base), 1);     /* Standard */
    gtk_widget_set_size_request(np->base, 170, -1);
    tempfm_card_add_widget(card, "Start from", np->base);
    gtk_box_append(GTK_BOX(page), card);

    GtkWidget *note = gtk_label_new("TempeR's presets stay as they are: yours starts as a "
                                    "copy, that you can change on the graph.");
    gtk_widget_add_css_class(note, "prop-subtitle");
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 46);
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_widget_set_margin_top(note, 4);
    gtk_box_append(GTK_BOX(page), note);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(btns, 10);
    np->error = gtk_label_new("");
    gtk_widget_add_css_class(np->error, "prop-subtitle");
    gtk_widget_add_css_class(np->error, "lvl-crit");
    gtk_label_set_wrap(GTK_LABEL(np->error), TRUE);
    gtk_label_set_xalign(GTK_LABEL(np->error), 0);
    gtk_widget_set_hexpand(np->error, TRUE);
    gtk_box_append(GTK_BOX(btns), np->error);
    GtkWidget *create = gtk_button_new_with_label("Create");
    gtk_widget_add_css_class(create, "dlg-btn-accent");
    g_signal_connect(create, "clicked", G_CALLBACK(on_new_create), np);
    gtk_box_append(GTK_BOX(btns), create);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(cancel, "dlg-btn");
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), win);
    gtk_box_append(GTK_BOX(btns), cancel);
    gtk_box_append(GTK_BOX(page), btns);

    /* Enter in the name = Create */
    g_signal_connect_swapped(np->name, "activate", G_CALLBACK(gtk_widget_activate), create);
    gtk_window_present(GTK_WINDOW(win));
}

static GtkWidget *
menu_entry(const char *text, GCallback cb, int preset)
{
    GtkWidget *b = gtk_button_new_with_label(text);
    gtk_widget_add_css_class(b, "context-item");
    GtkWidget *l = gtk_button_get_child(GTK_BUTTON(b));
    if (GTK_IS_LABEL(l)) gtk_label_set_xalign(GTK_LABEL(l), 0);
    g_object_set_data(G_OBJECT(b), "preset", GINT_TO_POINTER(preset));
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

static void
fill_mine_menu(void)
{
    GtkWidget *box = fui->mine_box, *c;
    while ((c = gtk_widget_get_first_child(box)) != NULL)
        gtk_box_remove(GTK_BOX(box), c);

    if (!fui->user->len) {
        GtkWidget *l = gtk_label_new("You have no presets yet");
        gtk_widget_add_css_class(l, "menu-title");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_box_append(GTK_BOX(box), l);
    }
    for (guint i = 0; i < fui->user->len; i++) {
        UserPreset *u = fui->user->pdata[i];
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        char *txt = (int)i == fui->active_user
            ? g_strdup_printf("\xE2\x9C\x93 %s", u->name) : g_strdup(u->name);
        GtkWidget *load = menu_entry(txt, G_CALLBACK(on_user_preset), (int)i);
        g_free(txt);
        gtk_widget_set_hexpand(load, TRUE);
        gtk_widget_set_tooltip_text(load, "Put this preset on the graph");
        gtk_box_append(GTK_BOX(row), load);
        GtkWidget *del = gtk_button_new_with_label("\xE2\x9C\x95");
        gtk_widget_add_css_class(del, "cmd-btn");
        gtk_widget_set_tooltip_text(del, "Delete this preset");
        g_object_set_data(G_OBJECT(del), "preset", GINT_TO_POINTER((int)i));
        g_signal_connect(del, "clicked", G_CALLBACK(on_user_delete), NULL);
        gtk_box_append(GTK_BOX(row), del);
        gtk_box_append(GTK_BOX(box), row);
    }
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    if (fui->active_user >= 0) {
        char *txt = g_strdup_printf("Save the graph to \xE2\x80\x9C%s\xE2\x80\x9D",
                                    ((UserPreset *)fui->user->pdata[fui->active_user])->name);
        gtk_box_append(GTK_BOX(box), menu_entry(txt, G_CALLBACK(on_user_save), fui->active_user));
        g_free(txt);
    }
    gtk_box_append(GTK_BOX(box), menu_entry("New preset\xE2\x80\xA6", G_CALLBACK(on_user_new), -1));
}

static void
on_switch(GObject *sw, GParamSpec *ps, gpointer ud)
{
    (void)ps; (void)ud;
    if (fui->building) return;
    set_status(NULL, FALSE);
    if (gtk_switch_get_active(GTK_SWITCH(sw))) {
        send_curves(TRUE);                      /* what is on the graph */
    } else {
        g_dbus_connection_call(fui->sys, FAN_BUS, FAN_PATH, FAN_IFACE, "SetEnabled",
            g_variant_new("(b)", FALSE), NULL,
            G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT,
            fui->cancel, on_written, NULL);
    }
}

static void
on_apply(GtkButton *b, gpointer ud)
{
    (void)b; (void)ud;
    set_status(NULL, FALSE);
    send_curves(!fui->enabled);
}

static void
on_reset(GtkButton *b, gpointer ud)
{
    (void)b; (void)ud;
    set_status(NULL, FALSE);
    /* forget the edits: the service sends its default curves back */
    for (guint i = 0; fui->fans && i < fui->fans->len; i++) {
        FanCurve *f = fui->fans->pdata[i];
        edit_from(f, f->temp, f->pwm);
    }
    g_dbus_connection_call(fui->sys, FAN_BUS, FAN_PATH, FAN_IFACE, "ResetCurves",
        NULL, NULL, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT,
        fui->cancel, on_written, NULL);
}

static void
on_monitor(gboolean rebuilt, gpointer ud)
{
    (void)rebuilt; (void)ud;
    if (!fui || !gtk_widget_get_visible(fui->win)) return;
    gtk_widget_queue_draw(fui->graph);
    update_readout();
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

static void
on_destroy(GtkWidget *w, gpointer ud)
{
    (void)w; (void)ud;
    if (!fui) return;
    g_cancellable_cancel(fui->cancel);
    g_object_unref(fui->cancel);
    tempfm_monitor_unlisten(fui->listen_id);
    if (fui->sys) {
        if (fui->sub_id) g_dbus_connection_signal_unsubscribe(fui->sys, fui->sub_id);
        g_object_unref(fui->sys);
    }
    if (fui->fans) g_ptr_array_unref(fui->fans);
    g_ptr_array_unref(fui->user);
    g_free(fui);
    fui = NULL;
}

static GtkWidget *
dlg_button(const char *label, const char *css, GCallback cb)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_add_css_class(b, css);
    g_signal_connect(b, "clicked", cb, NULL);
    return b;
}

void
tempfm_open_fan_control(void)
{
    if (fui) {
        gtk_window_present(GTK_WINDOW(fui->win));
        request_state();
        return;
    }

    fui = g_new0(FanUI, 1);
    fui->drag = fui->hover = fui->selected = -1;
    fui->user = presets_load();
    fui->active_user = -1;
    fui->cancel = g_cancellable_new();

    GtkWidget *win = gtk_window_new();
    fui->win = win;
    GApplication *app = g_application_get_default();
    if (app && GTK_IS_APPLICATION(app))
        gtk_window_set_application(GTK_WINDOW(win), GTK_APPLICATION(app));
    gtk_widget_add_css_class(win, "props");
    gtk_window_set_title(GTK_WINDOW(win), "Fan control");
    gtk_window_set_default_size(GTK_WINDOW(win), 640, 560);
    tempfm_window_titlebar(win, "Fan control");
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

    /* ── on / off ── */
    gtk_box_append(GTK_BOX(page), tempfm_group_title("Fan curves"));
    GtkWidget *card = tempfm_card_new();
    fui->sw = gtk_switch_new();
    g_signal_connect(fui->sw, "notify::active", G_CALLBACK(on_switch), NULL);
    tempfm_card_add_widget(card, "Use TempeR's fan curves", fui->sw);
    gtk_box_append(GTK_BOX(page), card);
    fui->paused = gtk_label_new("Connecting to the fan control service\xE2\x80\xA6");
    gtk_widget_add_css_class(fui->paused, "prop-subtitle");
    gtk_label_set_wrap(GTK_LABEL(fui->paused), TRUE);
    gtk_label_set_xalign(GTK_LABEL(fui->paused), 0);
    gtk_widget_set_margin_top(fui->paused, 4);
    gtk_box_append(GTK_BOX(page), fui->paused);

    /* ── editor ── */
    fui->body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(fui->body, TRUE);
    gtk_box_append(GTK_BOX(page), fui->body);

    gtk_box_append(GTK_BOX(fui->body), tempfm_group_title("Curve"));
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    fui->tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_box_append(GTK_BOX(bar), fui->tabs);
    GtkWidget *sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(sp, TRUE);
    gtk_box_append(GTK_BOX(bar), sp);
    for (int k = -1; k < (int)G_N_ELEMENTS(PRESETS); k++) {
        GtkWidget *b = gtk_button_new_with_label(k < 0 ? "Default" : PRESETS[k].name);
        gtk_widget_add_css_class(b, "cmd-btn");
        gtk_widget_set_tooltip_text(b, k < 0 ? "The computer's own curve"
                                             : PRESETS[k].tip);
        g_object_set_data(G_OBJECT(b), "preset", GINT_TO_POINTER(k));
        g_signal_connect(b, "clicked", G_CALLBACK(on_preset), NULL);
        gtk_box_append(GTK_BOX(bar), b);
    }
    /* the user's own presets: copies of the ones above */
    GtkWidget *mine = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(mine), "My presets");
    gtk_widget_add_css_class(mine, "cmd-btn");
    gtk_widget_set_tooltip_text(mine, "Your own presets, made from one of TempeR's");
    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    fui->mine_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_popover_set_child(GTK_POPOVER(pop), fui->mine_box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(mine), pop);
    gtk_box_append(GTK_BOX(bar), mine);
    fill_mine_menu();
    gtk_box_append(GTK_BOX(fui->body), bar);

    GtkWidget *frame = tempfm_card_new();
    gtk_widget_set_margin_top(frame, 4);
    gtk_widget_set_vexpand(frame, TRUE);
    fui->graph = gtk_drawing_area_new();
    gtk_widget_set_size_request(fui->graph, 520, 260);
    gtk_widget_set_vexpand(fui->graph, TRUE);
    gtk_widget_set_hexpand(fui->graph, TRUE);
    gtk_widget_set_margin_top(fui->graph, 6);
    gtk_widget_set_margin_end(fui->graph, 4);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(fui->graph), draw_graph, NULL, NULL);
    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin",  G_CALLBACK(on_drag_begin),  NULL);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), NULL);
    g_signal_connect(drag, "drag-end",    G_CALLBACK(on_drag_end),    NULL);
    gtk_widget_add_controller(fui->graph, GTK_EVENT_CONTROLLER(drag));
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), NULL);
    g_signal_connect(motion, "leave",  G_CALLBACK(on_leave),  NULL);
    gtk_widget_add_controller(fui->graph, motion);
    gtk_box_append(GTK_BOX(frame), fui->graph);
    gtk_box_append(GTK_BOX(fui->body), frame);

    GtkWidget *info = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(info, 6);
    fui->readout = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(fui->readout), 0);
    gtk_label_set_ellipsize(GTK_LABEL(fui->readout), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(fui->readout, TRUE);
    gtk_box_append(GTK_BOX(info), fui->readout);
    fui->now = gtk_label_new("");
    gtk_widget_add_css_class(fui->now, "prop-value");
    gtk_box_append(GTK_BOX(info), fui->now);
    gtk_box_append(GTK_BOX(fui->body), info);

    GtkWidget *safety = gtk_label_new(
        "For safety, from 85 \xC2\xB0" "C up the fans always turn at least at 50 % "
        "(red area). The processor and graphics card still slow themselves down "
        "if they get too hot, whatever the curve.");
    gtk_widget_add_css_class(safety, "prop-subtitle");
    gtk_label_set_wrap(GTK_LABEL(safety), TRUE);
    gtk_label_set_xalign(GTK_LABEL(safety), 0);
    gtk_widget_set_margin_top(safety, 4);
    gtk_box_append(GTK_BOX(fui->body), safety);

    /* ── buttons ── */
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(btns, 10);
    fui->status = gtk_label_new("");
    gtk_widget_add_css_class(fui->status, "prop-subtitle");
    gtk_label_set_wrap(GTK_LABEL(fui->status), TRUE);
    gtk_label_set_xalign(GTK_LABEL(fui->status), 0);
    gtk_widget_set_hexpand(fui->status, TRUE);
    gtk_box_append(GTK_BOX(btns), fui->status);
    fui->btn_reset = dlg_button("Reset", "dlg-btn", G_CALLBACK(on_reset));
    gtk_widget_set_tooltip_text(fui->btn_reset, "Back to the computer's own curves");
    gtk_box_append(GTK_BOX(btns), fui->btn_reset);
    fui->btn_apply = dlg_button("Apply", "dlg-btn-accent", G_CALLBACK(on_apply));
    gtk_box_append(GTK_BOX(btns), fui->btn_apply);
    GtkWidget *close = gtk_button_new_with_label("Close");
    gtk_widget_add_css_class(close, "dlg-btn");
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_close), win);
    gtk_box_append(GTK_BOX(btns), close);
    gtk_box_append(GTK_BOX(page), btns);

    gtk_widget_set_sensitive(fui->body, FALSE);
    gtk_widget_set_sensitive(fui->sw, FALSE);
    gtk_widget_set_sensitive(fui->btn_apply, FALSE);

    GError *err = NULL;
    fui->sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
    if (fui->sys) {
        fui->sub_id = g_dbus_connection_signal_subscribe(fui->sys, FAN_BUS, FAN_IFACE,
            "Changed", FAN_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
            on_changed_signal, NULL, NULL);
        request_state();
    } else {
        set_status(err->message, TRUE);
        g_error_free(err);
    }
    fui->listen_id = tempfm_monitor_listen(on_monitor, NULL);

    gtk_window_present(GTK_WINDOW(win));
}

/* At TempeR's start (also at login, in the background): asks the service
 * for its state, so D-Bus starts it.  With the curves on it then takes the
 * fans back from asusd / ROG Control Center even if it was not started at
 * boot, and it applies the keyboard light chosen in TempeR.  Nothing is
 * shown: without the package the call simply fails. */
static void
on_wake_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    (void)ud;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (r) g_variant_unref(r);
}

void
tempfm_fan_service_wake(void)
{
    GDBusConnection *sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (!sys) return;
    g_dbus_connection_call(sys, FAN_BUS, FAN_PATH, FAN_IFACE, "GetState", NULL,
        NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, on_wake_done, NULL);
    g_object_unref(sys);
}
