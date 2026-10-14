/* gui.c – TempeR window: Windows 10 File Explorer look, same theme engine
 * as ExploreR.
 *
 *   ┌ title bar ─────────────────────────────────────────── _ □ X ┐
 *   │ Refresh │ Units ▾  Update every ▾  View ▾                   │  command bar
 *   │ ↑ [ 🌡 Temperatures › Processor                       ⟳ ]    │  address row
 *   │ Overview  │                                                 │
 *   │ Processor │   tiles (overview) or sensor list (category)    │
 *   │ …         │                                                 │
 *   │ 12 sensors                         Updated 16:44:03 · 2 s   │  status bar
 *   └─────────────────────────────────────────────────────────────┘
 *
 * Client-side decorations with the caption buttons in Windows order, and
 * every widget styled by our own CSS, so it looks the same on GNOME, KDE,
 * Hyprland, Sway, i3, Openbox… under Wayland or X11.
 */

#include "tempfm.h"

#define PAGE_OVERVIEW  CAT_COUNT

static const int INTERVALS[] = { 1, 2, 5, 10 };
#define N_INTERVALS ((int)G_N_ELEMENTS(INTERVALS))

typedef struct {
    Sensor    *s;
    GtkWidget *row, *value, *bar, *peak, *status;
} RowRef;

typedef struct {
    Device    *d;
    GtkWidget *title;
    GtkWidget *note;        /* "asleep" explanation, NVIDIA only */
} DevRef;

typedef struct {
    SensorCategory cat;
    GtkWidget     *sub, *bar, *value, *status;
} TileRef;

typedef struct {
    GtkWidget    *window;
    GtkWidget    *sidebar;
    GtkWidget    *stack;
    GtkWidget    *crumb_page;
    GtkWidget    *crumb_sep;
    GtkWidget    *btn_up;
    GtkWidget    *status_left;
    GtkWidget    *status_right;
    GtkWidget    *mod_dot;
    GtkWidget    *title_icon, *crumb_icon;     /* boxes holding the icon */
    GtkWidget    *overlay_btn;
    GtkWidget    *banner, *banner_icon, *banner_text;
    const char   *banner_icon_name;
    GtkWidget    *nav[CAT_COUNT + 1];

    GtkWidget    *unit_check[2];
    GtkWidget    *interval_check[N_INTERVALS];
    GtkWidget    *theme_check[4];

    GPtrArray    *devices;  /* owned by monitor.c */
    GArray       *rows;     /* RowRef  */
    GArray       *devrefs;  /* DevRef  */
    GArray       *tiles;    /* TileRef */

    int           page;
    gboolean      stale;        /* readings arrived while hidden */
    guint         listen_id;
    gboolean      space_down;   /* for Ctrl+Shift+Alt+Space+Enter */
} App;

static App *main_app = NULL;    /* the one main window */

static void update_values(gpointer data);
static void rebuild_ui(App *a);


/* ════════════════════════════════════════════════════════════
 * THEME ENGINE  (palettes and CSS from ExploreR)
 * ════════════════════════════════════════════════════════════ */

typedef struct {
    const char *key;
    const char *value;
} PaletteEntry;

/* Windows 10 File Explorer, light */
static const PaletteEntry PAL_LIGHT[] = {
    { "bg",        "#ffffff" },
    { "chrome",    "#ffffff" },
    { "toolbar",   "#f5f6f7" },
    { "nav",       "#ffffff" },
    { "fg",        "#1a1a1a" },
    { "dim",       "#6d6d6d" },
    { "faint",     "#a0a0a0" },
    { "border",    "#e5e5e5" },
    { "border2",   "#d9d9d9" },
    { "hover",     "#e5f3ff" },
    { "sel",       "#cce8ff" },
    { "selb",      "#99d1ff" },
    { "selhov",    "#d9eeff" },
    { "accent",    "#0078d7" },
    { "accentfg",  "#ffffff" },
    { "input",     "#ffffff" },
    { "menu",      "#f2f2f2" },
    { "menub",     "#cccccc" },
    { "menuhov",   "#91c9f7" },
    { "btnhov",    "#e5f3ff" },
    { "btnhovb",   "#cce4f7" },
    { "btnact",    "#cce4f7" },
    { "slider",    "#c2c2c2" },
    { "sliderhov", "#a6a6a6" },
    { "track",     "#e6e6e6" },
    { "card",      "#fbfbfb" },
    { "ok",        "#107c10" },
    { "warn",      "#9d5d00" },
    { "hot",       "#ca5010" },
    { "crit",      "#c42b1c" },
    { NULL, NULL }
};

/* Windows 10 File Explorer, dark */
static const PaletteEntry PAL_DARK[] = {
    { "bg",        "#191919" },
    { "chrome",    "#202020" },
    { "toolbar",   "#2b2b2b" },
    { "nav",       "#191919" },
    { "fg",        "#ffffff" },
    { "dim",       "#a6a6a6" },
    { "faint",     "#6e6e6e" },
    { "border",    "#2f2f2f" },
    { "border2",   "#454545" },
    { "hover",     "#2d2d2d" },
    { "sel",       "#4d4d4d" },
    { "selb",      "#6b6b6b" },
    { "selhov",    "#575757" },
    { "accent",    "#0078d7" },
    { "accentfg",  "#ffffff" },
    { "input",     "#2b2b2b" },
    { "menu",      "#2b2b2b" },
    { "menub",     "#4a4a4a" },
    { "menuhov",   "#414141" },
    { "btnhov",    "#3a3a3a" },
    { "btnhovb",   "#4a4a4a" },
    { "btnact",    "#4a4a4a" },
    { "slider",    "#5a5a5a" },
    { "sliderhov", "#7a7a7a" },
    { "track",     "#3a3a3a" },
    { "card",      "#262626" },
    { "ok",        "#6ccb5f" },
    { "warn",      "#fce100" },
    { "hot",       "#f7a35c" },
    { "crit",      "#ff99a4" },
    { NULL, NULL }
};

/* the original MyFM look */
static const PaletteEntry PAL_PURPLE[] = {
    { "bg",        "#1b1729" },
    { "chrome",    "#14111f" },
    { "toolbar",   "#1a1628" },
    { "nav",       "#171322" },
    { "fg",        "#e8e3f5" },
    { "dim",       "#8d85a8" },
    { "faint",     "#574f70" },
    { "border",    "#262038" },
    { "border2",   "#2d2747" },
    { "hover",     "#221d33" },
    { "sel",       "#352c58" },
    { "selb",      "#4b4374" },
    { "selhov",    "#3d3266" },
    { "accent",    "#6d4ed6" },
    { "accentfg",  "#ffffff" },
    { "input",     "#1d1930" },
    { "menu",      "#231d38" },
    { "menub",     "#352d55" },
    { "menuhov",   "#342c54" },
    { "btnhov",    "#272038" },
    { "btnhovb",   "#352d55" },
    { "btnact",    "#322a4c" },
    { "slider",    "#3a3358" },
    { "sliderhov", "#4b4374" },
    { "track",     "#2c2544" },
    { "card",      "#211c33" },
    { "ok",        "#7fd18b" },
    { "warn",      "#f0c674" },
    { "hot",       "#f5a97f" },
    { "crit",      "#ff8fab" },
    { NULL, NULL }
};

static const char *TEMPFM_CSS_TEMPLATE =
    /* ── base: compact, Segoe-like, square ── */
    "window.tempfm {"
    "  font-family: \"Segoe UI\", \"Noto Sans\", \"DejaVu Sans\", \"Liberation Sans\", sans-serif;"
    "  font-size: 12px; background-color: {bg}; color: {fg}; border-radius: 0;"
    "}"
    "window.tempfm.csd {"
    "  border-radius: 0; box-shadow: 0 0 0 1px {border2}, 0 2px 10px rgba(0,0,0,0.25);"
    "}"
    "label { color: inherit; }"

    /* ── title bar + Windows 10 caption buttons ── */
    "window.tempfm headerbar {"
    "  background: {chrome}; color: {fg}; border: none; box-shadow: none;"
    "  min-height: 30px; padding: 0; margin: 0; border-radius: 0;"
    "}"
    "window.tempfm headerbar:backdrop { color: {faint}; }"
    ".title-icon { margin-left: 8px; margin-right: 6px; font-size: 13px; }"
    ".title-text { font-size: 12px; color: {fg}; }"
    "headerbar:backdrop .title-text { color: {faint}; }"
    "windowcontrols { margin: 0; padding: 0; border-spacing: 0; }"
    "windowcontrols > button {"
    "  min-width: 44px; min-height: 30px; margin: 0; padding: 0;"
    "  border-radius: 0; border: none; box-shadow: none;"
    "  background: transparent; color: {fg};"
    "}"
    "windowcontrols > button > image {"
    "  background: none; box-shadow: none; border-radius: 0;"
    "  padding: 0; margin: 0; color: inherit; -gtk-icon-size: 12px;"
    "}"
    "windowcontrols > button:hover  { background: {btnhov}; }"
    "windowcontrols > button:active { background: {btnact}; }"
    "windowcontrols > button.close:hover  { background: #e81123; color: #ffffff; }"
    "windowcontrols > button.close:active { background: #f1707a; color: #ffffff; }"

    /* ── generic buttons ── */
    "button {"
    "  background: transparent; color: {fg}; border: 1px solid transparent;"
    "  border-radius: 0; box-shadow: none; padding: 2px 6px; min-height: 20px;"
    "  text-shadow: none; -gtk-icon-shadow: none; outline-color: {accent};"
    "}"
    "button:hover    { background: {btnhov}; border-color: {btnhovb}; }"
    "button:active, button:checked { background: {btnact}; border-color: {btnhovb}; }"
    "button:disabled { color: {faint}; background: transparent; border-color: transparent; }"
    "button:disabled label { color: {faint}; }"

    /* ── command bar (ribbon-lite) ── */
    ".commandbar {"
    "  background: {toolbar}; border-bottom: 1px solid {border2};"
    "  padding: 2px 4px; min-height: 26px;"
    "}"
    ".cmd-btn { padding: 1px 6px; min-height: 22px; }"
    ".cmd-btn-icon { margin-right: 4px; }"
    ".cmd-sep { background: {border2}; min-width: 1px; margin: 3px 4px; }"
    "menubutton > button { padding: 1px 6px; min-height: 22px; }"
    "menubutton.cmd-accent > button { color: {accent}; font-weight: bold; }"

    /* ── address row ── */
    ".addressbar { background: {bg}; padding: 4px 4px; border-bottom: 1px solid {border}; }"
    ".nav-btn { min-width: 24px; min-height: 22px; padding: 0 2px; font-size: 14px; }"
    ".crumb-bar {"
    "  background: {input}; border: 1px solid {border2}; border-radius: 0;"
    "  padding: 0 2px; min-height: 22px;"
    "}"
    ".crumb-bar:hover { border-color: {accent}; }"
    ".crumb-icon { margin: 0 4px 0 4px; }"
    ".crumb-btn { padding: 0 4px; min-height: 20px; font-size: 12px; }"
    ".crumb-sep { color: {dim}; font-size: 11px; margin: 0 1px; }"
    ".crumb-refresh { min-width: 22px; min-height: 20px; padding: 0; border-left: 1px solid {border}; }"

    /* ── navigation pane ── */
    ".sidebar { background: {nav}; padding: 4px 0; }"
    ".sidebar-title { color: {dim}; font-size: 11px; padding: 6px 0 2px 10px; }"
    ".sidebar-btn {"
    "  padding: 1px 8px 1px 14px; min-height: 22px; border-radius: 0;"
    "  border: 1px solid transparent; margin: 0 2px;"
    "}"
    ".sidebar-btn:hover { background: {hover}; border-color: transparent; }"
    ".sidebar-btn.current { background: {sel}; border-color: {selb}; }"
    ".sidebar-icon { margin-right: 6px; }"
    "paned > separator {"
    "  background: {border}; background-image: none; border: none; box-shadow: none;"
    "  min-width: 1px; min-height: 1px; margin: 0; padding: 0;"
    "}"

    /* ── content ── */
    ".content-area { background: {bg}; }"
    ".file-view { background: {bg}; padding: 6px 12px 12px 12px; }"
    /* header and rows share the same box model so the columns line up */
    ".col-header { background: {bg}; padding: 0 4px; border: 1px solid transparent; margin-bottom: 1px; }"
    ".cell { padding: 0 6px; border-right: 1px solid transparent; }"
    ".col-head { color: {dim}; min-height: 22px; border-right-color: {border}; }"
    ".file-row {"
    "  border: 1px solid transparent; border-radius: 0;"
    "  padding: 0 4px; min-height: 24px; color: {fg};"
    "}"
    ".file-row:hover { background: {hover}; }"
    ".file-label { color: {fg}; }"
    ".file-meta  { color: {dim}; }"
    ".info-label { color: {dim}; font-size: 12px; }"

    /* ── section titles and tiles (This PC style) ── */
    ".section-title {"
    "  color: {fg}; font-size: 12px; font-weight: normal;"
    "  padding-bottom: 2px; border-bottom: 1px solid {border};"
    "}"
    ".section-sub { color: {faint}; font-size: 11px; }"
    "flowboxchild { padding: 0; border-radius: 0; background: transparent; outline: none; }"
    ".drive-tile {"
    "  border: 1px solid transparent; border-radius: 0; padding: 6px 8px;"
    "}"
    ".drive-tile:hover { background: {hover}; border-color: transparent; }"
    ".drive-icon  { font-size: 30px; margin-right: 8px; }"
    ".drive-name  { color: {fg}; font-size: 12px; }"
    ".drive-path  { color: {faint}; font-size: 11px; }"
    ".tile-value  { color: {fg}; font-size: 16px; }"
    "progressbar trough {"
    "  background: {track}; border: 1px solid {border2}; border-radius: 0; min-height: 12px;"
    "}"
    "progressbar progress {"
    "  background: #26a0da; border-radius: 0; min-height: 12px; border: none;"
    "  margin: 0; min-width: 0;"
    "}"
    "progressbar.lvl-warm progress { background: #e8a200; }"
    "progressbar.lvl-hot  progress { background: #f7630c; }"
    "progressbar.lvl-crit progress { background: #da2626; }"
    "progressbar.lvl-none progress { background: transparent; }"
    ".row-bar trough, .row-bar progress { min-height: 8px; }"
    "progressbar.row-bar > trough { min-width: 40px; }"

    /* ── status words ── */
    "label.lvl-normal { color: {ok}; }"
    "label.lvl-warm   { color: {warn}; }"
    "label.lvl-hot    { color: {hot}; }"
    "label.lvl-crit   { color: {crit}; font-weight: bold; }"
    "label.lvl-none   { color: {faint}; }"

    /* ── summary banner ── */
    ".banner {"
    "  background: {card}; border: 1px solid {border}; border-left: 4px solid {ok};"
    "  padding: 8px 12px;"
    "}"
    ".banner.lvl-warm { border-left-color: #e8a200; }"
    ".banner.lvl-hot  { border-left-color: #f7630c; }"
    ".banner.lvl-crit { border-left-color: #da2626; }"
    ".banner.lvl-none { border-left-color: {faint}; }"
    ".banner-icon { font-size: 20px; margin-right: 10px; }"
    ".banner-text { color: {fg}; font-size: 13px; }"

    /* ── status bar ── */
    ".statusbar {"
    "  background: {bg}; border-top: 1px solid {border};"
    "  padding: 1px 8px; min-height: 20px;"
    "}"
    ".status-label { color: {fg}; font-size: 11px; }"

    /* ── scrollbars (thin, square) ── */
    "scrollbar { background: transparent; border: none; }"
    "scrollbar slider {"
    "  background: {slider}; border-radius: 0; border: none;"
    "  min-width: 6px; min-height: 6px;"
    "}"
    "scrollbar slider:hover  { background: {sliderhov}; }"

    /* ── popovers, menus, tooltips ── */
    "popover > contents {"
    "  background: {menu}; color: {fg}; border: 1px solid {menub};"
    "  border-radius: 0; padding: 2px; box-shadow: 2px 2px 4px rgba(0,0,0,0.25);"
    "}"
    "popover > arrow { background: {menu}; border: 1px solid {menub}; }"
    ".context-item {"
    "  border: none; border-radius: 0; padding: 2px 16px 2px 6px;"
    "  min-width: 160px; min-height: 20px;"
    "}"
    ".context-item:hover { background: {menuhov}; border: none; }"
    ".ctx-icon  { min-width: 20px; }"
    ".menu-title { color: {dim}; font-size: 11px; padding: 4px 6px 2px 6px; }"
    "separator { background: {border2}; min-height: 1px; min-width: 1px; }"
    "tooltip {"
    "  background: {menu}; color: {fg}; border: 1px solid {menub}; border-radius: 0;"
    "  box-shadow: none; padding: 2px 4px;"
    "}"
    "tooltip label { color: {fg}; }"
    "selection, label selection { background: {accent}; color: {accentfg}; }"

    /* ── icons ── */
    ".sidebar-icon { margin-right: 6px; }"
    ".tile-icon { margin-right: 10px; }"
    ".banner-icon { margin-right: 10px; }"
    ".cmd-active { background: {sel}; border-color: {selb}; }"
    "label.mod-dot { color: #e81123; font-size: 10px; margin-left: 6px; }"

    /* ── Settings / Specs windows (ExploreR "props") ── */
    "window.props {"
    "  font-family: \"Segoe UI\", \"Noto Sans\", \"DejaVu Sans\", \"Liberation Sans\", sans-serif;"
    "  font-size: 12px; background: {bg}; color: {fg}; border-radius: 0;"
    "}"
    "window.props.csd { border-radius: 0; box-shadow: 0 0 0 1px {border2}, 0 2px 10px rgba(0,0,0,0.25); }"
    "window.props headerbar {"
    "  background: {chrome}; color: {fg}; border: none; box-shadow: none;"
    "  min-height: 30px; padding: 0; margin: 0; border-radius: 0;"
    "}"
    "window.props headerbar:backdrop { color: {faint}; }"
    ".prop-title    { color: {fg}; font-size: 15px; }"
    ".prop-subtitle { color: {dim}; font-size: 11px; }"
    ".prop-group-title { color: {accent}; font-size: 12px; margin: 10px 2px 4px 2px; }"
    ".prop-card { background: {card}; border: 1px solid {border}; border-radius: 0; }"
    ".prop-row   { padding: 4px 10px; min-height: 26px; }"
    ".prop-label { color: {fg}; }"
    ".prop-value { color: {dim}; }"
    "entry, .text-entry, spinbutton {"
    "  background: {input}; color: {fg}; caret-color: {fg};"
    "  border: 1px solid {border2}; border-radius: 0; box-shadow: none;"
    "  min-height: 22px; padding: 0 6px; outline: none;"
    "}"
    "entry:focus-within, spinbutton:focus-within { border-color: {accent}; }"
    "entry text, spinbutton text { color: {fg}; }"
    "entry placeholder, entry > text > placeholder { color: {faint}; }"
    "spinbutton > button { border-radius: 0; border: none; min-width: 20px; }"
    "dropdown > button { border: 1px solid {border2}; background: {input}; }"
    "dropdown > button:hover { border-color: {accent}; }"
    "popover listview, popover list, popover scrolledwindow, popover viewport {"
    "  background: transparent; color: {fg};"
    "}"
    "popover listview > row, popover list > row {"
    "  background: transparent; color: {fg}; border-radius: 0; padding: 2px 8px; min-height: 20px;"
    "}"
    "popover listview > row:hover, popover listview > row:selected,"
    "popover list > row:hover, popover list > row:selected { background: {menuhov}; }"
    "switch {"
    "  background: {input}; border: 1px solid {dim}; border-radius: 10px;"
    "  box-shadow: none; min-height: 18px;"
    "}"
    "switch:checked { background: {accent}; border-color: {accent}; }"
    "switch > slider {"
    "  background: {dim}; border: none; box-shadow: none;"
    "  min-width: 12px; min-height: 12px; margin: 3px;"
    "}"
    "switch:checked > slider { background: {accentfg}; }"
    "scale trough { background: {track}; border-radius: 0; min-height: 4px; border: none; }"
    "scale highlight { background: {accent}; border-radius: 0; }"
    "scale slider { background: {accent}; border-radius: 0; min-width: 8px; min-height: 18px;"
    "  margin: -7px 0; padding: 0; border: none; box-shadow: none; }"
    "scale value { color: {dim}; }"
    ".dlg-btn, .dlg-btn-accent, .dlg-btn-danger {"
    "  min-width: 70px; min-height: 22px; padding: 1px 10px; border-radius: 0;"
    "  background: {toolbar}; border: 1px solid {border2}; color: {fg};"
    "}"
    ".dlg-btn:hover, .dlg-btn-accent:hover, .dlg-btn-danger:hover {"
    "  background: {btnhov}; border-color: {accent};"
    "}"
    ".dlg-btn-accent { border-color: {accent}; }"
    ".dlg-btn-danger { border-color: #e81123; }"
    ".dlg-btn-danger:hover { background: #e81123; color: #ffffff; border-color: #e81123; }";

static GtkCssProvider *theme_provider = NULL;

/* What the desktop prefers (xdg-desktop-portal "color-scheme", or a
 * "-dark" GTK theme name on desktops without a portal). */
static gboolean
system_prefers_dark(void)
{
    GtkSettings *gs = gtk_settings_get_default();
    if (!gs) return FALSE;

    GObjectClass *klass = G_OBJECT_GET_CLASS(gs);
    if (g_object_class_find_property(klass, "gtk-interface-color-scheme")) {
        int scheme = 0;
        g_object_get(gs, "gtk-interface-color-scheme", &scheme, NULL);
        if (scheme == 2) return TRUE;    /* GTK_INTERFACE_COLOR_SCHEME_DARK  */
        if (scheme == 3) return FALSE;   /* GTK_INTERFACE_COLOR_SCHEME_LIGHT */
    }

    gboolean dark = FALSE;
    char *theme = NULL;
    if (g_object_class_find_property(klass, "gtk-application-prefer-dark-theme"))
        g_object_get(gs, "gtk-application-prefer-dark-theme", &dark, NULL);
    g_object_get(gs, "gtk-theme-name", &theme, NULL);
    if (theme) {
        char *lower = g_ascii_strdown(theme, -1);
        if (g_str_has_suffix(lower, "dark")) dark = TRUE;
        g_free(lower);
    }
    g_free(theme);
    return dark;
}

static void
on_system_scheme_changed(GObject *o, GParamSpec *ps, gpointer ud)
{
    (void)o; (void)ps; (void)ud;
    if (tempfm_settings()->theme == THEME_SYSTEM)
        tempfm_gui_apply_theme();
}

void
tempfm_gui_apply_theme(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return;

    if (!theme_provider) {
        theme_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(display,
            GTK_STYLE_PROVIDER(theme_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

        GtkSettings *gs = gtk_settings_get_default();
        if (gs) {
            GObjectClass *klass = G_OBJECT_GET_CLASS(gs);
            if (g_object_class_find_property(klass, "gtk-interface-color-scheme"))
                g_signal_connect(gs, "notify::gtk-interface-color-scheme",
                    G_CALLBACK(on_system_scheme_changed), NULL);
            g_signal_connect(gs, "notify::gtk-theme-name",
                G_CALLBACK(on_system_scheme_changed), NULL);
        }
    }

    const PaletteEntry *pal;
    switch (tempfm_settings()->theme) {
        case THEME_LIGHT:  pal = PAL_LIGHT;  break;
        case THEME_DARK:   pal = PAL_DARK;   break;
        case THEME_PURPLE: pal = PAL_PURPLE; break;
        case THEME_SYSTEM:
        default: pal = system_prefers_dark() ? PAL_DARK : PAL_LIGHT; break;
    }

    /* reloading the CSS restyles every window: only when it changes */
    static const PaletteEntry *loaded = NULL;
    if (pal == loaded) return;
    loaded = pal;

    GString *css = g_string_new(TEMPFM_CSS_TEMPLATE);
    for (int i = 0; pal[i].key; i++) {
        char *tok = g_strdup_printf("{%s}", pal[i].key);
        g_string_replace(css, tok, pal[i].value, 0);
        g_free(tok);
    }
    gtk_css_provider_load_from_string(theme_provider, css->str);
    g_string_free(css, TRUE);
}


/* ════════════════════════════════════════════════════════════
 * HELPERS
 * ════════════════════════════════════════════════════════════ */

char *
tempfm_format_temp(double c)
{
    if (tempfm_settings()->unit == UNIT_FAHRENHEIT)
        return g_strdup_printf("%.0f \xC2\xB0" "F", c * 9.0 / 5.0 + 32.0);
    return g_strdup_printf("%.0f \xC2\xB0" "C", c);
}

static GtkWidget *
label_new(const char *text, const char *css, float xalign)
{
    GtkWidget *l = gtk_label_new(text);
    if (css) gtk_widget_add_css_class(l, css);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    return l;
}

static void
clear_box(GtkWidget *box)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(box)) != NULL)
        gtk_box_remove(GTK_BOX(box), c);
}

/* Swaps the lvl-* class of a widget */
static void
set_level_class(GtkWidget *w, TempLevel l)
{
    static const char *all[] = { "lvl-none", "lvl-normal", "lvl-warm", "lvl-hot", "lvl-crit" };
    const char *want = tempfm_level_class(l);
    for (guint i = 0; i < G_N_ELEMENTS(all); i++)
        if (strcmp(all[i], want)) gtk_widget_remove_css_class(w, all[i]);
    gtk_widget_add_css_class(w, want);
}

static void
set_bar(GtkWidget *bar, const Sensor *s)
{
    TempLevel l = tempfm_sensor_level(s);
    double f = (s->valid && s->crit > 0) ? s->value / s->crit : 0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), CLAMP(f, 0.0, 1.0));
    set_level_class(bar, l);
}

static gboolean
category_has_temps(GPtrArray *devs, SensorCategory c)
{
    for (guint i = 0; i < devs->len; i++) {
        Device *d = devs->pdata[i];
        if (d->category == c && d->sensors->len) return TRUE;
    }
    return FALSE;
}

/* Hottest representative sensor of a category, and its device */
static Sensor *
category_main(GPtrArray *devs, SensorCategory c, Device **dev_out, int *n_dev)
{
    Sensor *best = NULL;
    Device *bestd = NULL;
    int n = 0;
    for (guint i = 0; i < devs->len; i++) {
        Device *d = devs->pdata[i];
        if (d->category != c) continue;
        n++;
        Sensor *s = tempfm_device_main_sensor(d);
        if (!s) continue;
        if (!best || (s->valid && (!best->valid || s->value > best->value))) {
            best = s;
            bestd = d;
        }
    }
    if (!bestd) {
        for (guint i = 0; i < devs->len && !bestd; i++)
            if (((Device *)devs->pdata[i])->category == c) bestd = devs->pdata[i];
    }
    if (dev_out) *dev_out = bestd;
    if (n_dev) *n_dev = n;
    return best;
}

static const char *
category_page_id(int page)
{
    static const char *ids[] = { "cpu", "gpu", "storage", "memory", "battery",
                                 "system", "fans", "overview" };
    return ids[CLAMP(page, 0, PAGE_OVERVIEW)];
}

static const char *
page_title(int page)
{
    return page == PAGE_OVERVIEW ? "Overview" : tempfm_category_name((SensorCategory)page);
}


/* ════════════════════════════════════════════════════════════
 * VALUES  (called on every tick)
 * ════════════════════════════════════════════════════════════ */

static void
update_banner(App *a)
{
    if (!a->banner) return;

    /* Any Hot/Critical sensor is reported; otherwise the message talks
     * about the main reading of each part (the one on its tile), so it
     * never contradicts the tiles below it. */
    Sensor *worst = NULL, *worst_main = NULL;
    SensorCategory worst_cat = CAT_SYSTEM, main_cat = CAT_SYSTEM;
    int n_temps = 0;
    for (guint i = 0; i < a->devices->len; i++) {
        Device *d = a->devices->pdata[i];
        Sensor *m = tempfm_device_main_sensor(d);
        for (guint k = 0; k < d->sensors->len; k++) {
            Sensor *s = d->sensors->pdata[k];
            if (s->kind != KIND_TEMP) continue;
            n_temps++;
            if (!s->valid) continue;
            TempLevel l = tempfm_sensor_level(s);
            TempLevel wl = tempfm_sensor_level(worst);
            if (!worst || l > wl || (l == wl && s->value > worst->value)) {
                worst = s;
                worst_cat = d->category;
            }
            if (s != m) continue;
            wl = tempfm_sensor_level(worst_main);
            if (!worst_main || l > wl || (l == wl && s->value > worst_main->value)) {
                worst_main = s;
                main_cat = d->category;
            }
        }
    }
    if (worst && tempfm_sensor_level(worst) < LEVEL_HOT && worst_main) {
        worst = worst_main;
        worst_cat = main_cat;
    }

    TempLevel l = tempfm_sensor_level(worst);
    const char *icon, *icon_name;
    char *msg;
    char *t = worst ? tempfm_format_temp(worst->value) : NULL;
    char *what = g_ascii_strdown(tempfm_category_name(worst_cat), -1);

    if (n_temps == 0) {
        icon = "\xE2\x84\xB9";                                   /* ℹ */
        icon_name = "dialog-information";
        msg = g_strdup("No temperature sensors were found on this computer.");
    } else if (!worst) {
        icon = "\xE2\x84\xB9";
        icon_name = "dialog-information";
        msg = g_strdup("The sensors are not answering right now.");
    } else if (l == LEVEL_NORMAL) {
        icon = "\xE2\x9C\x94";                                   /* ✔ */
        icon_name = "security-high";
        msg = g_strdup_printf("Everything is running at normal temperatures. "
                              "The hottest part is the %s at %s.", what, t);
    } else if (l == LEVEL_WARM) {
        icon = "\xE2\x9C\x94";
        icon_name = "security-high";
        msg = g_strdup_printf("All good. The %s is warm (%s), which is "
                              "normal while it is working hard.", what, t);
    } else if (l == LEVEL_HOT) {
        icon = "\xE2\x9A\xA0";                                   /* ⚠ */
        icon_name = "dialog-warning";
        msg = g_strdup_printf("The %s is hot (%s). Make sure the air vents "
                              "are not blocked.", what, t);
    } else {
        icon = "\xE2\x9B\x94";                                   /* ⛔ */
        icon_name = "dialog-error";
        msg = g_strdup_printf("The %s has reached a critical temperature (%s)! "
                              "Save your work and let the computer cool down.", what, t);
    }

    if (a->banner_icon_name != icon_name) {
        a->banner_icon_name = icon_name;
        clear_box(a->banner_icon);
        gtk_box_append(GTK_BOX(a->banner_icon), tempfm_icon(icon_name, icon, 24));
    }
    gtk_label_set_text(GTK_LABEL(a->banner_text), msg);
    set_level_class(a->banner, n_temps && worst ? l : LEVEL_UNKNOWN);
    g_free(msg);
    g_free(what);
    g_free(t);
}

/* "Hot from 85 °C, critical at 100 °C": depends on the unit */
static void
update_tooltips(App *a)
{
    for (guint i = 0; i < a->rows->len; i++) {
        RowRef *r = &g_array_index(a->rows, RowRef, i);
        if (r->s->kind != KIND_TEMP) continue;
        char *hot  = tempfm_format_temp(r->s->high);
        char *crit = tempfm_format_temp(r->s->crit);
        char *tip = g_strdup_printf("Hot from %s, critical at %s", hot, crit);
        gtk_widget_set_tooltip_text(r->row, tip);
        g_free(tip);
        g_free(hot);
        g_free(crit);
    }
}

static void
update_values(gpointer data)
{
    App *a = data;

    for (guint i = 0; i < a->rows->len; i++) {
        RowRef *r = &g_array_index(a->rows, RowRef, i);
        Sensor *s = r->s;
        char *v;
        if (s->kind == KIND_FAN) {
            v = s->valid ? g_strdup_printf("%.0f RPM", s->value) : g_strdup("\xE2\x80\x94");
            gtk_label_set_text(GTK_LABEL(r->value), v);
            g_free(v);
            if (r->status) {
                gtk_label_set_text(GTK_LABEL(r->status),
                    !s->valid ? "Unavailable" : s->value > 0 ? "Spinning" : "Stopped");
                set_level_class(r->status, s->valid ? LEVEL_NORMAL : LEVEL_UNKNOWN);
            }
            continue;
        }
        v = s->valid ? tempfm_format_temp(s->value) : g_strdup("\xE2\x80\x94");
        gtk_label_set_text(GTK_LABEL(r->value), v);
        g_free(v);

        char *pk = s->peak > -999 ? tempfm_format_temp(s->peak) : g_strdup("\xE2\x80\x94");
        gtk_label_set_text(GTK_LABEL(r->peak), pk);
        g_free(pk);

        set_bar(r->bar, s);
        TempLevel l = tempfm_sensor_level(s);
        gtk_label_set_text(GTK_LABEL(r->status), tempfm_level_name(l));
        set_level_class(r->status, l);
    }

    for (guint i = 0; i < a->devrefs->len; i++) {
        DevRef *dr = &g_array_index(a->devrefs, DevRef, i);
        gtk_label_set_text(GTK_LABEL(dr->title), tempfm_device_name(dr->d));
        if (dr->note) {
            gtk_widget_set_visible(dr->note, dr->d->asleep);
            if (dr->d->asleep) {
                for (guint k = 0; k < a->rows->len; k++) {
                    RowRef *r = &g_array_index(a->rows, RowRef, k);
                    for (guint j = 0; j < dr->d->sensors->len; j++)
                        if (r->s == dr->d->sensors->pdata[j]) {
                            gtk_label_set_text(GTK_LABEL(r->status), "Sleeping");
                            set_level_class(r->status, LEVEL_UNKNOWN);
                        }
                }
            }
        }
    }

    for (guint i = 0; i < a->tiles->len; i++) {
        TileRef *t = &g_array_index(a->tiles, TileRef, i);
        Device *d = NULL;
        int n = 0;

        if (t->cat == CAT_FANS) {
            int count = 0, spinning = 0;
            double top = 0;
            for (guint k = 0; k < a->devices->len; k++) {
                Device *fd = a->devices->pdata[k];
                if (fd->category != CAT_FANS) continue;
                for (guint j = 0; j < fd->sensors->len; j++) {
                    Sensor *s = fd->sensors->pdata[j];
                    count++;
                    if (s->valid && s->value > 0) spinning++;
                    if (s->valid && s->value > top) top = s->value;
                }
            }
            char *sub = g_strdup_printf("%d fan%s, %d spinning", count,
                                        count == 1 ? "" : "s", spinning);
            gtk_label_set_text(GTK_LABEL(t->sub), sub);
            g_free(sub);
            char *v = g_strdup_printf("%.0f RPM", top);
            gtk_label_set_text(GTK_LABEL(t->value), v);
            g_free(v);
            gtk_label_set_text(GTK_LABEL(t->status), "fastest");
            set_level_class(t->status, LEVEL_UNKNOWN);
            continue;
        }

        Sensor *s = category_main(a->devices, t->cat, &d, &n);
        char *sub;
        if (t->cat == CAT_MEMORY)
            sub = g_strdup_printf("%d GB installed", tempfm_ram_gb());
        else if (d && n > 1)
            sub = g_strdup_printf("%s (+%d more)", tempfm_device_name(d), n - 1);
        else
            sub = g_strdup(d ? tempfm_device_name(d) : "");
        gtk_label_set_text(GTK_LABEL(t->sub), sub);
        g_free(sub);

        TempLevel l = tempfm_sensor_level(s);
        char *v = s && s->valid ? tempfm_format_temp(s->value) : g_strdup("\xE2\x80\x94");
        gtk_label_set_text(GTK_LABEL(t->value), v);
        g_free(v);
        gtk_label_set_text(GTK_LABEL(t->status),
            (!s || !s->valid) && d && d->asleep ? "Sleeping (power saving)"
                                                : tempfm_level_name(l));
        set_level_class(t->status, l);
        if (s) set_bar(t->bar, s);
    }

    update_banner(a);

    GDateTime *now = g_date_time_new_now_local();
    char *hm = g_date_time_format(now, "%H:%M:%S");
    char *txt = g_strdup_printf("Updated %s  \xC2\xB7  every %d s", hm,
                                tempfm_settings()->interval);
    gtk_label_set_text(GTK_LABEL(a->status_right), txt);
    gtk_widget_set_visible(a->mod_dot, tempfm_specs_modified());
    g_free(txt);
    g_free(hm);
    g_date_time_unref(now);
}

/* monitor.c read everything again (rebuilt: after Refresh) */
static void
on_monitor(gboolean rebuilt, gpointer data)
{
    App *a = data;
    if (rebuilt) {
        rebuild_ui(a);
    } else if (!gtk_widget_get_visible(a->window)) {
        a->stale = TRUE;            /* in the tray: catch up when shown */
    } else {
        update_values(a);
    }
}


/* ════════════════════════════════════════════════════════════
 * NAVIGATION
 * ════════════════════════════════════════════════════════════ */

static void
show_page(App *a, int page)
{
    if (page != PAGE_OVERVIEW &&
        !gtk_stack_get_child_by_name(GTK_STACK(a->stack), category_page_id(page)))
        page = PAGE_OVERVIEW;
    a->page = page;
    gtk_stack_set_visible_child_name(GTK_STACK(a->stack), category_page_id(page));

    for (int i = 0; i <= CAT_COUNT; i++)
        if (a->nav[i]) {
            if (i == page) gtk_widget_add_css_class(a->nav[i], "current");
            else           gtk_widget_remove_css_class(a->nav[i], "current");
        }

    gboolean sub = page != PAGE_OVERVIEW;
    gtk_label_set_text(GTK_LABEL(a->crumb_page), sub ? page_title(page) : "");
    gtk_widget_set_visible(a->crumb_page, sub);
    gtk_widget_set_visible(a->crumb_sep, sub);
    gtk_widget_set_sensitive(a->btn_up, sub);
}

static void
on_nav_clicked(GtkButton *b, gpointer ud)
{
    App *a = ud;
    show_page(a, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "page")));
}

static void
on_go_overview(GtkButton *b, gpointer ud)
{
    (void)b;
    show_page(ud, PAGE_OVERVIEW);
}

static void
on_refresh(GtkButton *b, gpointer ud)
{
    (void)b; (void)ud;
    tempfm_monitor_rediscover();
}


/* ════════════════════════════════════════════════════════════
 * PAGES
 * ════════════════════════════════════════════════════════════ */

static GtkWidget *
scrolled_page(GtkWidget **inner)
{
    GtkWidget *sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(sc, "content-area");
    gtk_widget_set_vexpand(sc, TRUE);
    gtk_widget_set_hexpand(sc, TRUE);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "file-view");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), box);
    *inner = box;
    return sc;
}

static GtkWidget *
tile_new(App *a, SensorCategory c)
{
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "drive-tile");
    gtk_widget_set_focus_on_click(btn, FALSE);
    gtk_widget_set_size_request(btn, 270, -1);
    gtk_widget_set_tooltip_text(btn, "Show the details");
    g_object_set_data(G_OBJECT(btn), "page", GINT_TO_POINTER((int)c));
    g_signal_connect(btn, "clicked", G_CALLBACK(on_nav_clicked), a);

    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *ic = tempfm_icon(tempfm_category_icon_name(c), tempfm_category_icon(c), 40);
    gtk_widget_add_css_class(ic, "tile-icon");
    gtk_widget_set_valign(ic, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hb), ic);

    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(vb, TRUE);
    gtk_box_append(GTK_BOX(vb), label_new(tempfm_category_name(c), "drive-name", 0));

    TileRef t = { .cat = c };
    t.sub = label_new("", "drive-path", 0);
    gtk_label_set_ellipsize(GTK_LABEL(t.sub), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(t.sub), 30);
    gtk_box_append(GTK_BOX(vb), t.sub);

    t.bar = gtk_progress_bar_new();
    gtk_widget_set_margin_top(t.bar, 2);
    gtk_widget_set_visible(t.bar, c != CAT_FANS);
    gtk_box_append(GTK_BOX(vb), t.bar);

    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    t.value = label_new("", "tile-value", 0);
    t.status = label_new("", NULL, 0);
    gtk_widget_set_valign(t.status, GTK_ALIGN_BASELINE_CENTER);
    gtk_widget_set_valign(t.value, GTK_ALIGN_BASELINE_CENTER);
    gtk_box_append(GTK_BOX(line), t.value);
    gtk_box_append(GTK_BOX(line), t.status);
    gtk_box_append(GTK_BOX(vb), line);

    gtk_box_append(GTK_BOX(hb), vb);
    gtk_button_set_child(GTK_BUTTON(btn), hb);

    g_array_append_val(a->tiles, t);
    return btn;
}

static GtkWidget *
section_title(const char *text, GtkWidget **label_out, const char *sub)
{
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(hb, "section-title");
    gtk_widget_set_margin_top(hb, 10);
    gtk_widget_set_margin_bottom(hb, 4);
    GtkWidget *l = label_new(text, NULL, 0);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(hb), l);
    if (sub) {
        GtkWidget *s = label_new(sub, "section-sub", 0);
        gtk_widget_set_valign(s, GTK_ALIGN_BASELINE_CENTER);
        gtk_widget_set_valign(l, GTK_ALIGN_BASELINE_CENTER);
        gtk_box_append(GTK_BOX(hb), s);
    }
    if (label_out) *label_out = l;
    return hb;
}

static GtkWidget *
build_overview(App *a)
{
    GtkWidget *box;
    GtkWidget *page = scrolled_page(&box);

    a->banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(a->banner, "banner");
    gtk_widget_set_margin_top(a->banner, 6);
    a->banner_icon = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(a->banner_icon, "banner-icon");
    gtk_widget_set_valign(a->banner_icon, GTK_ALIGN_CENTER);
    a->banner_icon_name = NULL;
    a->banner_text = label_new("", "banner-text", 0);
    gtk_label_set_wrap(GTK_LABEL(a->banner_text), TRUE);
    gtk_widget_set_hexpand(a->banner_text, TRUE);
    gtk_box_append(GTK_BOX(a->banner), a->banner_icon);
    gtk_box_append(GTK_BOX(a->banner), a->banner_text);
    gtk_box_append(GTK_BOX(box), a->banner);

    int n = 0;
    for (int c = 0; c < CAT_COUNT; c++)
        if (category_has_temps(a->devices, c)) n++;

    char *title = g_strdup_printf("Components (%d)", n);
    gtk_box_append(GTK_BOX(box), section_title(title, NULL, NULL));
    g_free(title);

    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 4);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 6);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 6);
    gtk_widget_set_valign(flow, GTK_ALIGN_START);
    for (int c = 0; c < CAT_COUNT; c++)
        if (category_has_temps(a->devices, c))
            gtk_flow_box_append(GTK_FLOW_BOX(flow), tile_new(a, c));
    gtk_box_append(GTK_BOX(box), flow);

    GtkWidget *hint = label_new(
        "Normal and Warm are both fine. Hot means the part is close to its "
        "limit; Critical means it is at its limit and will slow itself down "
        "or switch off to protect itself.", "info-label", 0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_margin_top(hint, 14);
    gtk_box_append(GTK_BOX(box), hint);
    return page;
}

static GtkWidget *
col_head(const char *text, int width, gboolean expand)
{
    GtkWidget *l = label_new(text, "col-head", 0);
    gtk_widget_add_css_class(l, "cell");
    if (width > 0) gtk_widget_set_size_request(l, width, -1);
    gtk_widget_set_hexpand(l, expand);
    return l;
}

static GtkWidget *
cell(const char *css, int width, gboolean expand)
{
    GtkWidget *l = label_new("", css, 0);
    gtk_widget_add_css_class(l, "cell");
    if (width > 0) gtk_widget_set_size_request(l, width, -1);
    gtk_widget_set_hexpand(l, expand);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    return l;
}

#define W_VALUE   100
#define W_BAR     150
#define W_PEAK    100
#define W_STATUS  110

static GtkWidget *
build_category(App *a, SensorCategory c)
{
    GtkWidget *box;
    GtkWidget *page = scrolled_page(&box);
    gboolean fans = c == CAT_FANS;

    for (guint i = 0; i < a->devices->len; i++) {
        Device *d = a->devices->pdata[i];
        if (d->category != c) continue;

        DevRef dr = { .d = d };
        gtk_box_append(GTK_BOX(box), section_title(tempfm_device_name(d), &dr.title, d->source));

        if (d->category == CAT_GPU && d->pci_dir) {
            dr.note = label_new("The graphics card is switched off to save power "
                                "(nothing is using it). Its temperature appears "
                                "as soon as it wakes up.", "info-label", 0);
            gtk_label_set_wrap(GTK_LABEL(dr.note), TRUE);
            gtk_widget_set_margin_bottom(dr.note, 4);
            gtk_widget_set_visible(dr.note, FALSE);
            gtk_box_append(GTK_BOX(box), dr.note);
        }
        g_array_append_val(a->devrefs, dr);

        GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(hdr, "col-header");
        gtk_box_append(GTK_BOX(hdr), col_head(fans ? "Fan" : "Sensor", 160, TRUE));
        gtk_box_append(GTK_BOX(hdr), col_head(fans ? "Speed" : "Temperature", W_VALUE, FALSE));
        if (!fans) {
            gtk_box_append(GTK_BOX(hdr), col_head("", W_BAR, FALSE));
            gtk_box_append(GTK_BOX(hdr), col_head("Highest", W_PEAK, FALSE));
        }
        gtk_box_append(GTK_BOX(hdr), col_head("Status", W_STATUS, FALSE));
        gtk_box_append(GTK_BOX(box), hdr);

        for (guint k = 0; k < d->sensors->len; k++) {
            Sensor *s = d->sensors->pdata[k];
            RowRef r = { .s = s };

            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            gtk_widget_add_css_class(row, "file-row");
            r.row = row;

            GtkWidget *name = cell("file-label", 160, TRUE);
            gtk_label_set_text(GTK_LABEL(name), s->label);
            gtk_box_append(GTK_BOX(row), name);

            r.value = cell("file-label", W_VALUE, FALSE);
            gtk_box_append(GTK_BOX(row), r.value);

            if (!fans) {
                GtkWidget *bc = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
                gtk_widget_add_css_class(bc, "cell");
                gtk_widget_set_size_request(bc, W_BAR, -1);
                gtk_widget_set_hexpand(bc, FALSE);      /* stop the bar's hexpand propagating */
                r.bar = gtk_progress_bar_new();
                gtk_widget_add_css_class(r.bar, "row-bar");
                gtk_widget_set_hexpand(r.bar, TRUE);
                gtk_widget_set_valign(r.bar, GTK_ALIGN_CENTER);
                gtk_box_append(GTK_BOX(bc), r.bar);
                gtk_box_append(GTK_BOX(row), bc);

                r.peak = cell("file-meta", W_PEAK, FALSE);
                gtk_box_append(GTK_BOX(row), r.peak);
            }

            r.status = cell("file-meta", W_STATUS, FALSE);
            gtk_box_append(GTK_BOX(row), r.status);

            gtk_box_append(GTK_BOX(box), row);
            g_array_append_val(a->rows, r);
        }
    }
    return page;
}

static GtkWidget *
nav_item(App *a, int page, const char *icon_name, const char *emoji, const char *text)
{
    GtkWidget *b = gtk_button_new();
    gtk_widget_add_css_class(b, "sidebar-btn");
    gtk_widget_set_focus_on_click(b, FALSE);
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *ic = tempfm_icon(icon_name, emoji, 16);
    gtk_widget_add_css_class(ic, "sidebar-icon");
    gtk_box_append(GTK_BOX(hb), ic);
    gtk_box_append(GTK_BOX(hb), label_new(text, NULL, 0));
    gtk_button_set_child(GTK_BUTTON(b), hb);
    g_object_set_data(G_OBJECT(b), "page", GINT_TO_POINTER(page));
    g_signal_connect(b, "clicked", G_CALLBACK(on_nav_clicked), a);
    a->nav[page] = b;
    return b;
}

/* Rebuilds sidebar and pages from monitor.c's device list */
static void
rebuild_ui(App *a)
{
    int keep = a->page;
    a->devices = tempfm_monitor_devices();

    g_array_set_size(a->rows, 0);
    g_array_set_size(a->devrefs, 0);
    g_array_set_size(a->tiles, 0);
    a->banner = NULL;
    memset(a->nav, 0, sizeof a->nav);

    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(a->stack)) != NULL)
        gtk_stack_remove(GTK_STACK(a->stack), c);
    clear_box(a->sidebar);

    GtkWidget *t = label_new("This PC", "sidebar-title", 0);
    gtk_box_append(GTK_BOX(a->sidebar), t);
    gtk_box_append(GTK_BOX(a->sidebar),
        nav_item(a, PAGE_OVERVIEW, "computer", "\xF0\x9F\x8F\xA0", "Overview"));  /* 🏠 */
    gtk_stack_add_named(GTK_STACK(a->stack), build_overview(a),
                        category_page_id(PAGE_OVERVIEW));

    gtk_box_append(GTK_BOX(a->sidebar), label_new("Components", "sidebar-title", 0));
    int n_sensors = 0;
    for (int cat = 0; cat < CAT_COUNT; cat++) {
        if (!category_has_temps(a->devices, cat)) continue;
        gtk_box_append(GTK_BOX(a->sidebar),
            nav_item(a, cat, tempfm_category_icon_name(cat), tempfm_category_icon(cat),
                     tempfm_category_name(cat)));
        gtk_stack_add_named(GTK_STACK(a->stack), build_category(a, cat),
                            category_page_id(cat));
    }
    for (guint i = 0; i < a->devices->len; i++)
        n_sensors += (int)((Device *)a->devices->pdata[i])->sensors->len;

    char *st = g_strdup_printf("%d sensor%s on %u device%s", n_sensors,
        n_sensors == 1 ? "" : "s", a->devices->len, a->devices->len == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(a->status_left), st);
    g_free(st);

    show_page(a, keep);
    update_tooltips(a);
    update_values(a);
}


/* ════════════════════════════════════════════════════════════
 * COMMAND BAR MENUS
 * ════════════════════════════════════════════════════════════ */

static void
set_checks(GtkWidget **checks, int n, int active)
{
    for (int i = 0; i < n; i++)
        gtk_label_set_text(GTK_LABEL(checks[i]), i == active ? "\xE2\x9C\x93" : "");
}

static void
sync_menus(App *a)
{
    TempfmSettings *s = tempfm_settings();
    set_checks(a->unit_check, 2, s->unit);
    set_checks(a->theme_check, 4, s->theme);
    int iv = -1;
    for (int i = 0; i < N_INTERVALS; i++)
        if (INTERVALS[i] == s->interval) iv = i;
    set_checks(a->interval_check, N_INTERVALS, iv);
}

typedef enum { MENU_UNIT, MENU_INTERVAL, MENU_THEME } MenuKind;

static void
on_menu_item(GtkButton *b, gpointer ud)
{
    App *a = ud;
    MenuKind kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "kind"));
    int value     = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "value"));
    TempfmSettings *s = tempfm_settings();

    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));

    (void)a;
    switch (kind) {
        case MENU_UNIT:     s->unit = (TempUnit)value;       break;
        case MENU_THEME:    s->theme = (TempfmTheme)value;   break;
        case MENU_INTERVAL:
            s->interval = value;
            tempfm_monitor_restart_timer();
            break;
    }
    tempfm_settings_changed(FALSE);     /* saves, and refreshes every window */
}

static GtkWidget *
menu_item(App *a, GtkWidget *box, const char *text, MenuKind kind, int value,
          GtkWidget **check_out)
{
    GtkWidget *b = gtk_button_new();
    gtk_widget_add_css_class(b, "context-item");
    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *ck = label_new("", "ctx-icon", 0.5f);
    gtk_box_append(GTK_BOX(hb), ck);
    gtk_box_append(GTK_BOX(hb), label_new(text, NULL, 0));
    gtk_button_set_child(GTK_BUTTON(b), hb);
    g_object_set_data(G_OBJECT(b), "kind",  GINT_TO_POINTER((int)kind));
    g_object_set_data(G_OBJECT(b), "value", GINT_TO_POINTER(value));
    g_signal_connect(b, "clicked", G_CALLBACK(on_menu_item), a);
    gtk_box_append(GTK_BOX(box), b);
    *check_out = ck;
    return b;
}

static GtkWidget *
menu_button(const char *label, GtkWidget **box_out)
{
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(mb), label);
    gtk_widget_set_focus_on_click(mb, FALSE);
    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_popover_set_child(GTK_POPOVER(pop), vb);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), pop);
    *box_out = vb;
    return mb;
}

static GtkWidget *
cmd_btn(const char *glyph, const char *label, const char *tooltip,
        GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new();
    gtk_widget_add_css_class(b, "cmd-btn");
    gtk_widget_set_focus_on_click(b, FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(box), label_new(glyph, "cmd-btn-icon", 0.5f));
    if (label) gtk_box_append(GTK_BOX(box), gtk_label_new(label));
    gtk_button_set_child(GTK_BUTTON(b), box);
    if (tooltip) gtk_widget_set_tooltip_text(b, tooltip);
    if (cb) g_signal_connect(b, "clicked", cb, data);
    return b;
}

static GtkWidget *
vsep(void)
{
    GtkWidget *s = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_add_css_class(s, "cmd-sep");
    return s;
}

static GtkWidget *
nav_btn(const char *glyph, const char *tooltip, GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new_with_label(glyph);
    gtk_widget_add_css_class(b, "nav-btn");
    gtk_widget_set_focus_on_click(b, FALSE);
    if (tooltip) gtk_widget_set_tooltip_text(b, tooltip);
    if (cb) g_signal_connect(b, "clicked", cb, data);
    return b;
}


/* ════════════════════════════════════════════════════════════
 * WINDOW
 * ════════════════════════════════════════════════════════════ */

static void
remember_size(GtkWindow *w)
{
    TempfmSettings *s = tempfm_settings();
    if (!gtk_widget_get_visible(GTK_WIDGET(w))) return;
    s->win_maximized = gtk_window_is_maximized(w);
    if (!s->win_maximized) {
        int width = 0, height = 0;
        gtk_window_get_default_size(w, &width, &height);
        if (width > 0)  s->win_width  = width;
        if (height > 0) s->win_height = height;
    }
}

/* Really quits: tray menu, Ctrl+Q, or closing without a tray */
void
tempfm_quit(void)
{
    if (main_app) remember_size(GTK_WINDOW(main_app->window));
    tempfm_settings_save();
    tempfm_settings()->ov_enabled = FALSE;     /* not saved: just close it */
    tempfm_overlay_apply();
    GApplication *app = g_application_get_default();
    if (app) g_application_quit(app);
}

/* With a tray, closing only hides the window: TempeR (and the overlay)
 * keep running in the background. */
static gboolean
on_close_request(GtkWindow *w, gpointer ud)
{
    (void)ud;
    remember_size(w);
    tempfm_settings_save();
    if (tempfm_settings()->keep_in_tray && tempfm_tray_available()) {
        gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
        return TRUE;
    }
    tempfm_quit();
    return TRUE;
}

void
tempfm_gui_show(void)
{
    App *a = main_app;
    if (!a) return;
    if (a->stale) {
        a->stale = FALSE;
        update_values(a);
    }
    gtk_window_present(GTK_WINDOW(a->window));
}

/* Tray icon click.  Only hides (never quits, whatever "keep running in
 * the tray" says: the icon is there to bring the window back). */
void
tempfm_gui_toggle(void)
{
    if (!main_app) return;
    GtkWindow *w = GTK_WINDOW(main_app->window);
    if (gtk_widget_get_visible(GTK_WIDGET(w)) && gtk_window_is_active(w)) {
        remember_size(w);
        tempfm_settings_save();
        gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
    } else {
        tempfm_gui_show();
    }
}

static void
sync_overlay_btn(App *a)
{
    if (tempfm_settings()->ov_enabled) gtk_widget_add_css_class(a->overlay_btn, "cmd-active");
    else                               gtk_widget_remove_css_class(a->overlay_btn, "cmd-active");
}

static void
fill_icon(GtkWidget *box, const char *name, const char *emoji, int size)
{
    clear_box(box);
    gtk_box_append(GTK_BOX(box), tempfm_icon(name, emoji, size));
}

/* Settings changed (Settings window, menus, tray, specs window) */
void
tempfm_gui_sync(gboolean icons_changed)
{
    App *a = main_app;
    if (!a) return;
    sync_menus(a);
    sync_overlay_btn(a);
    if (icons_changed) {
        fill_icon(a->title_icon, "psensor", "\xF0\x9F\x8C\xA1", 16);
        fill_icon(a->crumb_icon, "psensor", "\xF0\x9F\x8C\xA1", 16);
        rebuild_ui(a);
        return;
    }
    update_tooltips(a);
    update_values(a);
}

static void
on_toggle_overlay(GtkButton *b, gpointer ud)
{
    (void)b; (void)ud;
    tempfm_settings()->ov_enabled = !tempfm_settings()->ov_enabled;
    tempfm_settings_changed(FALSE);
}

static void
on_open_settings(GtkButton *b, gpointer ud)
{
    (void)b; (void)ud;
    tempfm_open_settings();
}

/* Ctrl+Shift+Alt+Space+Enter: the specs window.  Space is not a
 * modifier, so whether it is held down is tracked by hand. */
static gboolean
on_key_pressed(GtkEventControllerKey *c, guint keyval, guint code,
               GdkModifierType mod, gpointer ud)
{
    (void)c; (void)code;
    App *a = ud;
    const GdkModifierType need = GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK;
    if (keyval == GDK_KEY_space) {
        a->space_down = TRUE;
        /* don't let it also press a focused button */
        return (mod & need) == need;
    }
    if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter || keyval == GDK_KEY_ISO_Enter) &&
        a->space_down && (mod & need) == need) {
        a->space_down = FALSE;
        tempfm_open_specs();
        return TRUE;
    }
    return FALSE;
}

static void
on_key_released(GtkEventControllerKey *c, guint keyval, guint code,
                GdkModifierType mod, gpointer ud)
{
    (void)c; (void)code; (void)mod;
    if (keyval == GDK_KEY_space) ((App *)ud)->space_down = FALSE;
}

static void
on_focus_left(GtkEventControllerFocus *c, gpointer ud)
{
    (void)c;
    ((App *)ud)->space_down = FALSE;
}

static void
app_free(gpointer p)
{
    App *a = p;
    tempfm_monitor_unlisten(a->listen_id);
    if (main_app == a) main_app = NULL;
    g_array_unref(a->rows);
    g_array_unref(a->devrefs);
    g_array_unref(a->tiles);
    g_free(a);
}

static gboolean
on_shortcut_refresh(GtkWidget *w, GVariant *args, gpointer ud)
{
    (void)w; (void)args; (void)ud;
    tempfm_monitor_rediscover();
    return TRUE;
}

static gboolean
on_shortcut_close(GtkWidget *w, GVariant *args, gpointer ud)
{
    (void)args; (void)ud;
    gtk_window_close(GTK_WINDOW(w));
    return TRUE;
}

static gboolean
on_shortcut_quit(GtkWidget *w, GVariant *args, gpointer ud)
{
    (void)w; (void)args; (void)ud;
    tempfm_quit();
    return TRUE;
}

static gboolean
on_shortcut_settings(GtkWidget *w, GVariant *args, gpointer ud)
{
    (void)w; (void)args; (void)ud;
    tempfm_open_settings();
    return TRUE;
}

GtkWidget *
tempfm_window_new(GtkApplication *app)
{
    TempfmSettings *s = tempfm_settings();
    tempfm_gui_apply_theme();

    GtkWidget *window = gtk_application_window_new(app);
    gtk_widget_add_css_class(window, "tempfm");
    gtk_window_set_title(GTK_WINDOW(window), "TempeR");
    gtk_window_set_default_size(GTK_WINDOW(window), s->win_width, s->win_height);
    gtk_widget_set_size_request(window, 480, 320);
    if (s->win_maximized) gtk_window_maximize(GTK_WINDOW(window));

    App *a = g_new0(App, 1);
    a->window  = window;
    a->page    = PAGE_OVERVIEW;
    a->rows    = g_array_new(FALSE, TRUE, sizeof(RowRef));
    a->devrefs = g_array_new(FALSE, TRUE, sizeof(DevRef));
    a->tiles   = g_array_new(FALSE, TRUE, sizeof(TileRef));
    main_app   = a;
    g_object_set_data_full(G_OBJECT(window), "tempfm-app", a, app_free);
    g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), NULL);

    /* ── title bar: icon + title left, caption buttons right (Windows order) ── */
    GtkWidget *hb = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hb), TRUE);
    gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(hb), ":minimize,maximize,close");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb),
        gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
    gtk_window_set_titlebar(GTK_WINDOW(window), hb);
    a->title_icon = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(a->title_icon, "title-icon");
    gtk_widget_set_valign(a->title_icon, GTK_ALIGN_CENTER);
    fill_icon(a->title_icon, "psensor", "\xF0\x9F\x8C\xA1", 16);           /* 🌡 */
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), a->title_icon);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(hb), label_new("TempeR", "title-text", 0));

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(window), root);

    /* ── command bar ── */
    GtkWidget *cmdbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 1);
    gtk_widget_add_css_class(cmdbar, "commandbar");

    gtk_box_append(GTK_BOX(cmdbar), cmd_btn("\xE2\x9F\xB3", "Refresh",
        "Look for sensors again (F5)", G_CALLBACK(on_refresh), a));
    gtk_box_append(GTK_BOX(cmdbar), vsep());

    GtkWidget *box;
    GtkWidget *mb = menu_button("Units", &box);
    menu_item(a, box, "Celsius (\xC2\xB0" "C)",    MENU_UNIT, UNIT_CELSIUS,    &a->unit_check[0]);
    menu_item(a, box, "Fahrenheit (\xC2\xB0" "F)", MENU_UNIT, UNIT_FAHRENHEIT, &a->unit_check[1]);
    gtk_box_append(GTK_BOX(cmdbar), mb);

    mb = menu_button("Update every", &box);
    for (int i = 0; i < N_INTERVALS; i++) {
        char *t = g_strdup_printf("%d second%s", INTERVALS[i], INTERVALS[i] == 1 ? "" : "s");
        menu_item(a, box, t, MENU_INTERVAL, INTERVALS[i], &a->interval_check[i]);
        g_free(t);
    }
    gtk_box_append(GTK_BOX(cmdbar), mb);

    mb = menu_button("View", &box);
    gtk_box_append(GTK_BOX(box), label_new("Theme", "menu-title", 0));
    menu_item(a, box, "Use system setting", MENU_THEME, THEME_SYSTEM, &a->theme_check[0]);
    menu_item(a, box, "Light",              MENU_THEME, THEME_LIGHT,  &a->theme_check[1]);
    menu_item(a, box, "Dark",               MENU_THEME, THEME_DARK,   &a->theme_check[2]);
    menu_item(a, box, "Purple",             MENU_THEME, THEME_PURPLE, &a->theme_check[3]);
    gtk_box_append(GTK_BOX(cmdbar), mb);
    sync_menus(a);

    gtk_box_append(GTK_BOX(cmdbar), vsep());
    a->overlay_btn = cmd_btn("\xE2\x96\xA3", "Overlay",
        "Show the temperatures above games and full-screen apps",
        G_CALLBACK(on_toggle_overlay), a);
    sync_overlay_btn(a);
    gtk_box_append(GTK_BOX(cmdbar), a->overlay_btn);

    GtkWidget *cspacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(cspacer, TRUE);
    gtk_box_append(GTK_BOX(cmdbar), cspacer);
    gtk_box_append(GTK_BOX(cmdbar), cmd_btn("\xE2\x9A\x99", NULL, "Settings (Ctrl+,)",
        G_CALLBACK(on_open_settings), a));

    gtk_box_append(GTK_BOX(root), cmdbar);

    /* ── address row: ↑ [ 🌡 Temperatures › Page   ⟳ ] ── */
    GtkWidget *addr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(addr, "addressbar");
    a->btn_up = nav_btn("\xE2\x86\x91", "Up to the overview",
                        G_CALLBACK(on_go_overview), a);
    gtk_box_append(GTK_BOX(addr), a->btn_up);

    GtkWidget *crumbs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(crumbs, "crumb-bar");
    gtk_widget_set_hexpand(crumbs, TRUE);
    gtk_widget_set_margin_start(crumbs, 4);
    a->crumb_icon = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(a->crumb_icon, "crumb-icon");
    gtk_widget_set_valign(a->crumb_icon, GTK_ALIGN_CENTER);
    fill_icon(a->crumb_icon, "psensor", "\xF0\x9F\x8C\xA1", 16);
    gtk_box_append(GTK_BOX(crumbs), a->crumb_icon);
    GtkWidget *home = gtk_button_new_with_label("Temperatures");
    gtk_widget_add_css_class(home, "crumb-btn");
    gtk_widget_set_focus_on_click(home, FALSE);
    g_signal_connect(home, "clicked", G_CALLBACK(on_go_overview), a);
    gtk_box_append(GTK_BOX(crumbs), home);
    a->crumb_sep = label_new("\xE2\x80\xBA", "crumb-sep", 0.5f);     /* › */
    gtk_box_append(GTK_BOX(crumbs), a->crumb_sep);
    a->crumb_page = label_new("", "crumb-btn", 0);
    gtk_box_append(GTK_BOX(crumbs), a->crumb_page);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(crumbs), spacer);
    GtkWidget *rf = gtk_button_new_with_label("\xE2\x9F\xB3");
    gtk_widget_add_css_class(rf, "crumb-refresh");
    gtk_widget_set_focus_on_click(rf, FALSE);
    gtk_widget_set_tooltip_text(rf, "Refresh (F5)");
    g_signal_connect(rf, "clicked", G_CALLBACK(on_refresh), a);
    gtk_box_append(GTK_BOX(crumbs), rf);
    gtk_box_append(GTK_BOX(addr), crumbs);
    gtk_box_append(GTK_BOX(root), addr);

    /* ── navigation pane | content ── */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(paned), 190);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_vexpand(paned, TRUE);

    GtkWidget *nav_sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(nav_sc),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    a->sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(a->sidebar, "sidebar");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(nav_sc), a->sidebar);
    gtk_widget_add_css_class(nav_sc, "sidebar");
    gtk_paned_set_start_child(GTK_PANED(paned), nav_sc);

    a->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(a->stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_paned_set_end_child(GTK_PANED(paned), a->stack);
    gtk_box_append(GTK_BOX(root), paned);

    /* ── status bar ── */
    GtkWidget *sb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(sb, "statusbar");
    a->status_left = label_new("", "status-label", 0);
    gtk_widget_set_hexpand(a->status_left, TRUE);
    a->status_right = label_new("", "status-label", 1);
    a->mod_dot = label_new("\xE2\x97\x8F", "mod-dot", 0.5f);              /* ● */
    gtk_widget_set_valign(a->mod_dot, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(a->mod_dot, FALSE);
    gtk_box_append(GTK_BOX(sb), a->status_left);
    gtk_box_append(GTK_BOX(sb), a->status_right);
    gtk_box_append(GTK_BOX(sb), a->mod_dot);
    gtk_box_append(GTK_BOX(root), sb);

    /* ── shortcuts: F5 refresh, Ctrl+W close, Ctrl+Q quit, Ctrl+, settings ── */
    GtkEventController *sc = gtk_shortcut_controller_new();
    gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(sc), GTK_SHORTCUT_SCOPE_GLOBAL);
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_F5, 0),
                         gtk_callback_action_new(on_shortcut_refresh, a, NULL)));
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
        gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>w"),
                         gtk_callback_action_new(on_shortcut_close, NULL, NULL)));
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
        gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>q"),
                         gtk_callback_action_new(on_shortcut_quit, NULL, NULL)));
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
        gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Control>comma"),
                         gtk_callback_action_new(on_shortcut_settings, NULL, NULL)));
    gtk_widget_add_controller(window, sc);

    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    g_signal_connect(kc, "key-pressed",  G_CALLBACK(on_key_pressed),  a);
    g_signal_connect(kc, "key-released", G_CALLBACK(on_key_released), a);
    gtk_widget_add_controller(window, kc);
    GtkEventController *fc = gtk_event_controller_focus_new();
    g_signal_connect(fc, "leave", G_CALLBACK(on_focus_left), a);
    gtk_widget_add_controller(window, fc);

    rebuild_ui(a);
    a->listen_id = tempfm_monitor_listen(on_monitor, a);
    gtk_window_present(GTK_WINDOW(window));
    return window;
}
