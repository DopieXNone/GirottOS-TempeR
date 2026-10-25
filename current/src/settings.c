/* settings.c – persistent preferences and the Settings window.
 *
 * Same scheme as ExploreR: a plain INI file, settings.ini, next to the
 * TempeR executable, so the configuration travels with the program.  When
 * that folder is not writable (e.g. installed in /usr/bin) it falls back
 * to $XDG_CONFIG_HOME/temper/settings.ini.  GKeyFile only: no GSettings
 * schema and no dconf.
 */

#include "tempfm.h"

#include <glib/gstdio.h>

#define DEFAULT_WIDTH    900
#define DEFAULT_HEIGHT   600
#define MIN_WIDTH        480
#define MIN_HEIGHT       320
#define MAX_SIDE        8192

static TempfmSettings settings;
static gboolean       loaded = FALSE;

static const char *theme_ids[]  = { "system", "light", "dark", "purple", NULL };
static const char *icon_ids[]   = { "tela", "emoji", NULL };
static const char *unit_ids[]   = { "celsius", "fahrenheit", NULL };
static const char *corner_ids[] = { "top-left", "top-right", "bottom-left", "bottom-right", NULL };

/* [Overlay] keys, and what is shown by default (RAM is opt-in) */
static const struct {
    const char *id;
    const char *name;
    gboolean    def;
} OV_ITEMS[OV_COUNT] = {
    [OV_CPU_TEMP]      = { "cpu-temperature",  "CPU temperature",       TRUE  },
    [OV_CPU_USAGE]     = { "cpu-usage",        "CPU usage",             TRUE  },
    [OV_CPU_CLOCK]     = { "cpu-clock",        "CPU clock",             FALSE },
    [OV_GPU_TEMP]      = { "gpu-temperature",  "GPU temperature",       TRUE  },
    [OV_GPU_USAGE]     = { "gpu-usage",        "GPU usage",             TRUE  },
    [OV_GPU_VRAM]      = { "gpu-memory",       "GPU memory (VRAM)",     TRUE  },
    [OV_DISK_TEMP]     = { "disk-temperature", "Disk temperature",      TRUE  },
    [OV_DISK_ACTIVITY] = { "disk-activity",    "Disk activity",         TRUE  },
    [OV_RAM]           = { "ram",              "Memory (RAM) usage",    FALSE },
    [OV_SWAP]          = { "swap",             "Swap usage",            TRUE  },
    [OV_FANS]          = { "fans",             "Fan speed",             FALSE },
    [OV_BATTERY]       = { "battery",          "Battery",               FALSE },
    [OV_TIME]          = { "time",             "Clock",                 FALSE },
};

const char *
tempfm_overlay_item_name(OverlayItem i)
{
    return OV_ITEMS[i].name;
}

static const char *
config_path(void)
{
    static char *path = NULL;
    if (path) return path;

    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe) {
        char *dir = g_path_get_dirname(exe);
        char *candidate = g_build_filename(dir, "settings.ini", NULL);
        gboolean usable = g_file_test(candidate, G_FILE_TEST_EXISTS)
            ? access(candidate, W_OK) == 0
            : access(dir, W_OK) == 0;
        if (usable) path = candidate;
        else        g_free(candidate);
        g_free(dir);
        g_free(exe);
    }
    if (!path)
        path = g_build_filename(g_get_user_config_dir(), "temper", "settings.ini", NULL);
    return path;
}

const char *
tempfm_settings_path(void)
{
    return config_path();
}

static int
id_index(const char * const *ids, const char *value, int fallback)
{
    if (!value) return fallback;
    for (int i = 0; ids[i]; i++)
        if (!g_ascii_strcasecmp(ids[i], value)) return i;
    return fallback;
}

static int
kf_int(GKeyFile *kf, const char *group, const char *key, int def)
{
    GError *err = NULL;
    int v = g_key_file_get_integer(kf, group, key, &err);
    if (err) { g_error_free(err); return def; }
    return v;
}

static gboolean
kf_bool(GKeyFile *kf, const char *group, const char *key, gboolean def)
{
    GError *err = NULL;
    gboolean v = g_key_file_get_boolean(kf, group, key, &err);
    if (err) { g_error_free(err); return def; }
    return v;
}

static int
kf_choice(GKeyFile *kf, const char *group, const char *key,
          const char * const *ids, int def)
{
    char *v = g_key_file_get_string(kf, group, key, NULL);
    int r = id_index(ids, v, def);
    g_free(v);
    return r;
}

TempfmSettings *
tempfm_settings(void)
{
    if (loaded) return &settings;
    loaded = TRUE;

    settings.spec_names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, config_path(), G_KEY_FILE_NONE, NULL);

    settings.theme      = (TempfmTheme)kf_choice(kf, "Appearance", "theme", theme_ids, THEME_SYSTEM);
    settings.icon_style = (TempfmIconStyle)kf_choice(kf, "Appearance", "icons", icon_ids, ICONS_TELA);
    settings.unit       = (TempUnit)kf_choice(kf, "Sensors", "unit", unit_ids, UNIT_CELSIUS);
    settings.interval   = CLAMP(kf_int(kf, "Sensors", "interval", 2), 1, 60);

    settings.win_width     = CLAMP(kf_int(kf, "Window", "width",  DEFAULT_WIDTH),  MIN_WIDTH,  MAX_SIDE);
    settings.win_height    = CLAMP(kf_int(kf, "Window", "height", DEFAULT_HEIGHT), MIN_HEIGHT, MAX_SIDE);
    settings.win_maximized = kf_bool(kf, "Window", "maximized", FALSE);
    settings.keep_in_tray  = kf_bool(kf, "Window", "keep-in-tray", TRUE);
    settings.autostart     = kf_bool(kf, "Startup", "autostart", TRUE);
    settings.keep_awake    = kf_bool(kf, "Power", "keep-awake", FALSE);

    settings.ov_enabled = kf_bool(kf, "Overlay", "enabled", FALSE);
    settings.ov_corner  = (OverlayCorner)kf_choice(kf, "Overlay", "position", corner_ids, CORNER_TOP_LEFT);
    settings.ov_monitor = CLAMP(kf_int(kf, "Overlay", "screen", 0), 0, 16);
    settings.ov_size    = CLAMP(kf_int(kf, "Overlay", "text-size", 1), 0, 2);
    settings.ov_opacity = CLAMP(kf_int(kf, "Overlay", "opacity", 55), 0, 100);
    for (int i = 0; i < OV_COUNT; i++)
        settings.ov_items[i] = kf_bool(kf, "Overlay", OV_ITEMS[i].id, OV_ITEMS[i].def);

    char **keys = g_key_file_get_keys(kf, "Specs", NULL, NULL);
    for (int i = 0; keys && keys[i]; i++) {
        if (!strcmp(keys[i], "ram-gb")) continue;
        char *v = g_key_file_get_string(kf, "Specs", keys[i], NULL);
        if (v && *v) g_hash_table_replace(settings.spec_names, g_strdup(keys[i]), v);
        else         g_free(v);
    }
    g_strfreev(keys);
    settings.spec_ram_gb = CLAMP(kf_int(kf, "Specs", "ram-gb", 0), 0, 1 << 20);

    g_key_file_free(kf);
    return &settings;
}

void
tempfm_settings_save(void)
{
    TempfmSettings *s = tempfm_settings();
    GKeyFile *kf = g_key_file_new();

    g_key_file_set_string (kf, "Appearance", "theme",  theme_ids[s->theme]);
    g_key_file_set_string (kf, "Appearance", "icons",  icon_ids[s->icon_style]);
    g_key_file_set_string (kf, "Sensors", "unit",      unit_ids[s->unit]);
    g_key_file_set_integer(kf, "Sensors", "interval",  s->interval);
    g_key_file_set_integer(kf, "Window", "width",      s->win_width);
    g_key_file_set_integer(kf, "Window", "height",     s->win_height);
    g_key_file_set_boolean(kf, "Window", "maximized",  s->win_maximized);
    g_key_file_set_boolean(kf, "Window", "keep-in-tray", s->keep_in_tray);
    g_key_file_set_boolean(kf, "Startup", "autostart", s->autostart);
    g_key_file_set_boolean(kf, "Power", "keep-awake", s->keep_awake);

    g_key_file_set_boolean(kf, "Overlay", "enabled",   s->ov_enabled);
    g_key_file_set_string (kf, "Overlay", "position",  corner_ids[s->ov_corner]);
    g_key_file_set_integer(kf, "Overlay", "screen",    s->ov_monitor);
    g_key_file_set_integer(kf, "Overlay", "text-size", s->ov_size);
    g_key_file_set_integer(kf, "Overlay", "opacity",   s->ov_opacity);
    for (int i = 0; i < OV_COUNT; i++)
        g_key_file_set_boolean(kf, "Overlay", OV_ITEMS[i].id, s->ov_items[i]);

    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, s->spec_names);
    while (g_hash_table_iter_next(&it, &k, &v))
        g_key_file_set_string(kf, "Specs", k, v);
    if (s->spec_ram_gb > 0)
        g_key_file_set_integer(kf, "Specs", "ram-gb", s->spec_ram_gb);

    const char *path = config_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    GError *err = NULL;
    if (!g_key_file_save_to_file(kf, path, &err)) {
        g_printerr("TempeR: cannot save %s: %s\n", path, err->message);
        g_error_free(err);
    }
    g_key_file_free(kf);
}


/* ════════════════════════════════════════════════════════════
 * START AT LOGIN
 * ════════════════════════════════════════════════════════════ */

/* An XDG autostart entry, ~/.config/autostart/com.girottos.TempeR.desktop,
 * understood by every desktop (GNOME, KDE, XFCE…) and by systemd's
 * xdg-autostart generator (Hyprland, Sway with uwsm…).  It starts TempeR
 * with --background: no window, only the tray icon and the overlay (when
 * it is on).  Only a file we wrote ourselves is ever removed. */

#define AUTOSTART_MARK "X-TempeR-Autostart=true"

static char *
autostart_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "autostart",
                            "com.girottos.TempeR.desktop", NULL);
}

/* "TempeR" when it is installed, else this very executable (build folder) */
static char *
autostart_exec(void)
{
    char *installed = g_find_program_in_path("TempeR");
    if (installed && g_str_has_prefix(installed, "/usr/")) {
        g_free(installed);
        return g_strdup("TempeR --background");
    }
    g_free(installed);
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *q = g_shell_quote(exe ? exe : "TempeR");
    char *r = g_strdup_printf("%s --background", q);
    g_free(q);
    g_free(exe);
    return r;
}

void
tempfm_autostart_sync(void)
{
    char *path = autostart_path();
    char *old = NULL;
    g_file_get_contents(path, &old, NULL, NULL);

    if (tempfm_settings()->autostart) {
        char *exec = autostart_exec();
        char *want = g_strdup_printf(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=TempeR\n"
            "Comment=Temperature monitor, overlay and fan curves in the background\n"
            "Exec=%s\n"
            "Icon=utilities-system-monitor\n"
            "Terminal=false\n"
            "NoDisplay=true\n"
            "X-GNOME-Autostart-enabled=true\n"
            "X-GNOME-Autostart-Delay=2\n"
            "X-KDE-autostart-after=panel\n"
            AUTOSTART_MARK "\n", exec);
        /* a file the user made by hand is theirs: leave it alone */
        if ((!old || strstr(old, AUTOSTART_MARK)) && g_strcmp0(old, want)) {
            char *dir = g_path_get_dirname(path);
            g_mkdir_with_parents(dir, 0755);
            g_free(dir);
            GError *err = NULL;
            if (!g_file_set_contents(path, want, -1, &err)) {
                g_printerr("TempeR: cannot write %s: %s\n", path, err->message);
                g_error_free(err);
            }
        }
        g_free(want);
        g_free(exec);
    } else if (old && strstr(old, AUTOSTART_MARK)) {
        g_unlink(path);
    }
    g_free(old);
    g_free(path);
}


/* ════════════════════════════════════════════════════════════
 * BUILDING BLOCKS (same as ExploreR's Properties / Settings)
 * ════════════════════════════════════════════════════════════ */

GtkWidget *
tempfm_window_titlebar(GtkWidget *win, const char *title)
{
    GtkWidget *hb = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hb), TRUE);
    gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(hb), ":close");
    GtkWidget *tl = gtk_label_new(title);
    gtk_widget_add_css_class(tl, "title-text");
    gtk_widget_set_margin_start(tl, 10);
    gtk_label_set_ellipsize(GTK_LABEL(tl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(tl), 40);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), tl);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb),
        gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
    gtk_window_set_titlebar(GTK_WINDOW(win), hb);
    return hb;
}

GtkWidget *
tempfm_group_title(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "prop-group-title");
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    return l;
}

GtkWidget *
tempfm_card_new(void)
{
    GtkWidget *c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(c, "prop-card");
    return c;
}

/* returns the row */
GtkWidget *
tempfm_card_add_widget(GtkWidget *card, const char *label_text, GtkWidget *w)
{
    if (gtk_widget_get_first_child(card) != NULL)
        gtk_box_append(GTK_BOX(card), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row, "prop-row");
    GtkWidget *l = gtk_label_new(label_text);
    gtk_widget_add_css_class(l, "prop-label");
    gtk_widget_set_halign(l, GTK_ALIGN_START);
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(row), l);

    gtk_widget_set_halign(w, GTK_ALIGN_END);
    gtk_widget_set_valign(w, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_box_append(GTK_BOX(row), w);
    gtk_box_append(GTK_BOX(card), row);
    return row;
}


/* ════════════════════════════════════════════════════════════
 * SETTINGS WINDOW
 * ════════════════════════════════════════════════════════════ */

typedef struct {
    GtkWidget *dd_theme, *dd_icons, *dd_unit, *dd_interval;
    GtkWidget *sw_tray, *sw_autostart, *sw_overlay, *sw_awake;
    GtkWidget *dd_corner, *dd_screen, *dd_size, *sc_opacity;
    GtkWidget *sw_items[OV_COUNT];
    GtkWidget *ov_note;
    gboolean   building;
} SettingsUI;

static GtkWidget *settings_win = NULL;      /* only one at a time */

static const int INTERVALS[] = { 1, 2, 5, 10 };

static int
interval_index(int v)
{
    for (guint i = 0; i < G_N_ELEMENTS(INTERVALS); i++)
        if (INTERVALS[i] == v) return (int)i;
    return 1;
}

/* the command bar, the tray menu… change the same preferences: keep an
 * open Settings window showing the real values */
static void
settings_ui_sync(void)
{
    if (!settings_win) return;
    SettingsUI *ui = g_object_get_data(G_OBJECT(settings_win), "settings-ui");
    TempfmSettings *s = tempfm_settings();
    ui->building = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(ui->dd_theme),    s->theme);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(ui->dd_icons),    s->icon_style);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(ui->dd_unit),     s->unit);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(ui->dd_interval), interval_index(s->interval));
    gtk_switch_set_active(GTK_SWITCH(ui->sw_tray),    s->keep_in_tray);
    gtk_switch_set_active(GTK_SWITCH(ui->sw_autostart), s->autostart);
    gtk_switch_set_active(GTK_SWITCH(ui->sw_overlay), s->ov_enabled);
    gtk_switch_set_active(GTK_SWITCH(ui->sw_awake),   s->keep_awake);
    ui->building = FALSE;
}

void
tempfm_settings_changed(gboolean icons_changed)
{
    tempfm_settings_save();
    tempfm_autostart_sync();
    tempfm_gui_apply_theme();
    tempfm_gui_sync(icons_changed);
    tempfm_overlay_apply();
    tempfm_awake_apply();
    tempfm_tray_sync();
    settings_ui_sync();
}

static void
on_dropdown(GObject *dd, GParamSpec *ps, gpointer ud)
{
    (void)ps;
    SettingsUI *ui = ud;
    if (ui->building) return;
    TempfmSettings *s = tempfm_settings();
    guint v = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    gboolean icons = FALSE;

    if (dd == G_OBJECT(ui->dd_theme))         s->theme = (TempfmTheme)v;
    else if (dd == G_OBJECT(ui->dd_icons))  { s->icon_style = (TempfmIconStyle)v; icons = TRUE; }
    else if (dd == G_OBJECT(ui->dd_unit))     s->unit = (TempUnit)v;
    else if (dd == G_OBJECT(ui->dd_interval)) {
        s->interval = INTERVALS[MIN(v, G_N_ELEMENTS(INTERVALS) - 1)];
        tempfm_monitor_restart_timer();
    }
    else if (dd == G_OBJECT(ui->dd_corner))   s->ov_corner = (OverlayCorner)v;
    else if (dd == G_OBJECT(ui->dd_screen))   s->ov_monitor = (int)v;
    else if (dd == G_OBJECT(ui->dd_size))     s->ov_size = (int)v;
    tempfm_settings_changed(icons);
}

static void
on_switch(GObject *sw, GParamSpec *ps, gpointer ud)
{
    (void)ps;
    SettingsUI *ui = ud;
    if (ui->building) return;
    gboolean *target = g_object_get_data(sw, "target");
    *target = gtk_switch_get_active(GTK_SWITCH(sw));
    tempfm_settings_changed(FALSE);
}

static void
on_opacity(GtkRange *r, gpointer ud)
{
    SettingsUI *ui = ud;
    if (ui->building) return;
    tempfm_settings()->ov_opacity = (int)gtk_range_get_value(r);
    tempfm_settings_changed(FALSE);
}

static void
on_fan_control(GtkButton *b, gpointer ud)
{
    (void)b; (void)ud;
    tempfm_open_fan_control();
}

static void
on_keyboard(GtkButton *b, gpointer ud)
{
    (void)b; (void)ud;
    tempfm_open_keyboard();
}

static GtkWidget *
make_dropdown(SettingsUI *ui, const char * const *labels, guint selected)
{
    GtkWidget *dd = gtk_drop_down_new_from_strings(labels);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), selected);
    gtk_widget_set_size_request(dd, 170, -1);
    g_signal_connect(dd, "notify::selected", G_CALLBACK(on_dropdown), ui);
    return dd;
}

static GtkWidget *
make_switch(SettingsUI *ui, gboolean *target)
{
    GtkWidget *sw = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw), *target);
    g_object_set_data(G_OBJECT(sw), "target", target);
    g_signal_connect(sw, "notify::active", G_CALLBACK(on_switch), ui);
    return sw;
}

static gboolean
on_settings_key(GtkEventControllerKey *c, guint keyval, guint code,
                GdkModifierType mod, gpointer ud)
{
    (void)c; (void)code; (void)mod;
    if (keyval == GDK_KEY_Escape) {
        gtk_window_close(GTK_WINDOW(ud));
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *
note_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "prop-subtitle");
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_set_margin_top(l, 4);
    return l;
}

void
tempfm_open_settings(void)
{
    if (settings_win) {
        gtk_window_present(GTK_WINDOW(settings_win));
        return;
    }

    TempfmSettings *s = tempfm_settings();
    SettingsUI *ui = g_new0(SettingsUI, 1);
    ui->building = TRUE;

    GtkWidget *win = gtk_window_new();
    settings_win = win;
    g_object_add_weak_pointer(G_OBJECT(win), (gpointer *)&settings_win);
    g_object_set_data_full(G_OBJECT(win), "settings-ui", ui, g_free);

    GApplication *app = g_application_get_default();
    if (app && GTK_IS_APPLICATION(app))
        gtk_window_set_application(GTK_WINDOW(win), GTK_APPLICATION(app));

    gtk_widget_add_css_class(win, "props");
    gtk_window_set_title(GTK_WINDOW(win), "Settings");
    gtk_window_set_default_size(GTK_WINDOW(win), 440, 640);
    tempfm_window_titlebar(win, "Settings");

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_settings_key), win);
    gtk_widget_add_controller(win, kc);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_window_set_child(GTK_WINDOW(win), scroll);

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(page, 12);
    gtk_widget_set_margin_end(page, 12);
    gtk_widget_set_margin_top(page, 4);
    gtk_widget_set_margin_bottom(page, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), page);

    /* ── Appearance ── */
    gtk_box_append(GTK_BOX(page), tempfm_group_title("Appearance"));
    GtkWidget *card = tempfm_card_new();
    static const char *theme_labels[] = { "Follow system", "Light", "Dark", "Purple (classic)", NULL };
    ui->dd_theme = make_dropdown(ui, theme_labels, s->theme);
    tempfm_card_add_widget(card, "Theme", ui->dd_theme);
    static const char *icon_labels[] = { "Tela", "Emoji", NULL };
    ui->dd_icons = make_dropdown(ui, icon_labels, s->icon_style);
    if (!tempfm_tela_available())
        gtk_widget_set_tooltip_text(ui->dd_icons,
            "The Tela icon theme is not installed: emoji are shown instead.");
    tempfm_card_add_widget(card, "Icons", ui->dd_icons);
    gtk_box_append(GTK_BOX(page), card);

    /* ── Readings ── */
    gtk_box_append(GTK_BOX(page), tempfm_group_title("Readings"));
    card = tempfm_card_new();
    static const char *unit_labels[] = { "Celsius (\xC2\xB0" "C)", "Fahrenheit (\xC2\xB0" "F)", NULL };
    ui->dd_unit = make_dropdown(ui, unit_labels, s->unit);
    tempfm_card_add_widget(card, "Temperature unit", ui->dd_unit);
    static const char *iv_labels[] = { "1 second", "2 seconds", "5 seconds", "10 seconds", NULL };
    ui->dd_interval = make_dropdown(ui, iv_labels, interval_index(s->interval));
    tempfm_card_add_widget(card, "Update every", ui->dd_interval);
    gtk_box_append(GTK_BOX(page), card);

    /* ── Background ── */
    gtk_box_append(GTK_BOX(page), tempfm_group_title("Background"));
    card = tempfm_card_new();
    ui->sw_autostart = make_switch(ui, &s->autostart);
    tempfm_card_add_widget(card, "Start TempeR when I log in", ui->sw_autostart);
    ui->sw_tray = make_switch(ui, &s->keep_in_tray);
    tempfm_card_add_widget(card, "Keep running in the tray when closed", ui->sw_tray);
    gtk_box_append(GTK_BOX(page), card);
    gtk_box_append(GTK_BOX(page), note_label(
        "At login TempeR starts in the background: no window, only the tray "
        "icon, the overlay when it is on, and your fan curves."));
    if (!tempfm_tray_available())
        gtk_box_append(GTK_BOX(page), note_label(
            "No system tray was found on this desktop, so closing the window "
            "quits TempeR. On GNOME, install the \"AppIndicator and KStatusNotifierItem "
            "Support\" extension to get one."));

    /* ── Power ── */
    gtk_box_append(GTK_BOX(page), tempfm_group_title("Power"));
    card = tempfm_card_new();
    ui->sw_awake = make_switch(ui, &s->keep_awake);
    tempfm_card_add_widget(card, "Keep the computer awake", ui->sw_awake);
    gtk_box_append(GTK_BOX(page), card);
    gtk_box_append(GTK_BOX(page), note_label(
        "While it is on, the computer does not go to sleep by itself and the "
        "screen does not turn off or lock when you don't use it. You can still "
        "suspend it yourself."));

    /* ── Fans ── */
    gtk_box_append(GTK_BOX(page), tempfm_group_title("Fans"));
    card = tempfm_card_new();
    GtkWidget *fan_btn = gtk_button_new_with_label("Open\xE2\x80\xA6");
    gtk_widget_add_css_class(fan_btn, "dlg-btn");
    g_signal_connect(fan_btn, "clicked", G_CALLBACK(on_fan_control), NULL);
    tempfm_card_add_widget(card, "Fan curves", fan_btn);
    gtk_box_append(GTK_BOX(page), card);

    /* ── Keyboard (laptops with a lit keyboard) ── */
    if (tempfm_keyboard_available()) {
        gtk_box_append(GTK_BOX(page), tempfm_group_title("Keyboard"));
        card = tempfm_card_new();
        GtkWidget *kbd_btn = gtk_button_new_with_label("Open\xE2\x80\xA6");
        gtk_widget_add_css_class(kbd_btn, "dlg-btn");
        g_signal_connect(kbd_btn, "clicked", G_CALLBACK(on_keyboard), NULL);
        tempfm_card_add_widget(card, "Keyboard light", kbd_btn);
        gtk_box_append(GTK_BOX(page), card);
    }

    /* ── Overlay ── */
    gtk_box_append(GTK_BOX(page), tempfm_group_title("Overlay"));
    card = tempfm_card_new();
    ui->sw_overlay = make_switch(ui, &s->ov_enabled);
    tempfm_card_add_widget(card, "Show the overlay", ui->sw_overlay);

    static const char *corner_labels[] = { "Top left", "Top right", "Bottom left", "Bottom right", NULL };
    ui->dd_corner = make_dropdown(ui, corner_labels, s->ov_corner);
    tempfm_card_add_widget(card, "Position", ui->dd_corner);

    GListModel *mons = tempfm_overlay_monitors();
    guint n_mons = mons ? g_list_model_get_n_items(mons) : 0;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < MAX(n_mons, 1); i++) {
        GdkMonitor *m = mons && i < n_mons ? g_list_model_get_item(mons, i) : NULL;
        const char *model = m ? gdk_monitor_get_model(m) : NULL;
        g_ptr_array_add(names, model && *model
            ? g_strdup_printf("%u \xE2\x80\x93 %s", i + 1, model)
            : g_strdup_printf("Screen %u", i + 1));
        if (m) g_object_unref(m);
    }
    g_ptr_array_add(names, NULL);
    ui->dd_screen = make_dropdown(ui, (const char * const *)names->pdata,
                                  MIN((guint)s->ov_monitor, MAX(n_mons, 1) - 1));
    g_ptr_array_unref(names);
    tempfm_card_add_widget(card, "Screen", ui->dd_screen);

    static const char *size_labels[] = { "Small", "Medium", "Large", NULL };
    ui->dd_size = make_dropdown(ui, size_labels, s->ov_size);
    tempfm_card_add_widget(card, "Text size", ui->dd_size);

    ui->sc_opacity = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
    gtk_range_set_value(GTK_RANGE(ui->sc_opacity), s->ov_opacity);
    gtk_scale_set_draw_value(GTK_SCALE(ui->sc_opacity), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(ui->sc_opacity), GTK_POS_LEFT);
    gtk_widget_set_size_request(ui->sc_opacity, 170, -1);
    g_signal_connect(ui->sc_opacity, "value-changed", G_CALLBACK(on_opacity), ui);
    tempfm_card_add_widget(card, "Background opacity (%)", ui->sc_opacity);
    gtk_box_append(GTK_BOX(page), card);

    gtk_box_append(GTK_BOX(page), tempfm_group_title("Overlay contents"));
    card = tempfm_card_new();
    for (int i = 0; i < OV_COUNT; i++) {
        ui->sw_items[i] = make_switch(ui, &s->ov_items[i]);
        tempfm_card_add_widget(card, OV_ITEMS[i].name, ui->sw_items[i]);
    }
    gtk_box_append(GTK_BOX(page), card);

    gtk_box_append(GTK_BOX(page), note_label(tempfm_overlay_on_top()
        ? "The overlay is a separate window drawn above games and full-screen "
          "apps: it never touches them, and clicks go through it."
        : "The overlay needs XWayland to stay above full-screen windows, and "
          "it is not available in this session: it is shown as a normal window."));

    char *where = g_strdup_printf("Saved in %s", config_path());
    gtk_box_append(GTK_BOX(page), note_label(where));
    g_free(where);

    ui->building = FALSE;
    gtk_window_present(GTK_WINDOW(win));
}
