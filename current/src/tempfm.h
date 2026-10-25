/* tempfm.h – shared declarations for TempeR
 *
 *   main.c      entry point (Wayland and X11), log filter
 *   sensors.c   discovery and reading of the temperature / fan sensors
 *               (/sys/class/hwmon, /sys/class/thermal, nvidia-smi)
 *   monitor.c   the clock of the program: reads sensors and system usage
 *               (CPU, GPU, RAM, swap, disks, battery) and tells every
 *               listener (main window, overlay, tray)
 *   gui.c       main window, theme (CSS), overview and detail pages
 *   icons.c     Tela icons (emoji fallback), as in ExploreR
 *   settings.c  persistent preferences (INI file) and the Settings window
 *   overlay.c   on-screen overlay drawn above games and full-screen apps
 *   tray.c      system tray icon (StatusNotifierItem over D-Bus)
 *   specs.c     the computer's specs window
 *   fancontrol.c fan curves window (applied by the temper-fand service)
 *   keyboard.c  keyboard light window (laptops; also through temper-fand)
 *   awake.c     "Keep awake": no automatic sleep / screen blanking
 *
 * Same toolkit and same look as ExploreR: GTK 4 only, styled entirely by
 * our own CSS, nothing that needs a particular desktop environment.
 */
#ifndef TEMPFM_H
#define TEMPFM_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <gtk/gtk.h>

#include <unistd.h>
#include <string.h>


/* ── enums ── */

typedef enum {
    CAT_CPU,
    CAT_GPU,
    CAT_STORAGE,
    CAT_MEMORY,
    CAT_BATTERY,
    CAT_SYSTEM,
    CAT_FANS,
    CAT_COUNT
} SensorCategory;

typedef enum {
    KIND_TEMP,          /* value in °C   */
    KIND_FAN            /* value in RPM  */
} SensorKind;

typedef enum {
    LEVEL_UNKNOWN,      /* not readable right now      */
    LEVEL_NORMAL,
    LEVEL_WARM,
    LEVEL_HOT,
    LEVEL_CRITICAL
} TempLevel;

typedef enum {
    THEME_SYSTEM,       /* follow the desktop's light/dark preference */
    THEME_LIGHT,        /* Windows 10 light                           */
    THEME_DARK,         /* Windows 10 dark                            */
    THEME_PURPLE        /* the original MyFM dark/purple look         */
} TempfmTheme;

typedef enum {
    ICONS_TELA,         /* Tela icon theme, used by TempeR only (emoji
                         * when it isn't installed)                    */
    ICONS_EMOJI
} TempfmIconStyle;

typedef enum {
    UNIT_CELSIUS,
    UNIT_FAHRENHEIT
} TempUnit;

typedef enum {
    CORNER_TOP_LEFT,
    CORNER_TOP_RIGHT,
    CORNER_BOTTOM_LEFT,
    CORNER_BOTTOM_RIGHT
} OverlayCorner;

/* Lines of the overlay, in the order they are shown */
typedef enum {
    OV_CPU_TEMP,
    OV_CPU_USAGE,
    OV_CPU_CLOCK,
    OV_GPU_TEMP,
    OV_GPU_USAGE,
    OV_GPU_VRAM,
    OV_DISK_TEMP,
    OV_DISK_ACTIVITY,
    OV_RAM,
    OV_SWAP,
    OV_FANS,
    OV_BATTERY,
    OV_TIME,
    OV_COUNT
} OverlayItem;


/* ── sensors (sensors.c) ── */

typedef struct {
    char       *label;      /* "Core 3", "Composite", "CPU fan"      */
    char       *input;      /* sysfs file, NULL for nvidia-smi        */
    SensorKind  kind;
    double      high;       /* °C where "Hot" starts                  */
    double      crit;       /* °C where "Critical" starts             */
    gboolean    primary;    /* the one shown on the overview tile     */
    gboolean    own_limits; /* high/crit come from the chip itself    */

    double      value;      /* last reading                           */
    gboolean    valid;      /* last reading succeeded                 */
    double      peak;       /* highest reading since start            */
} Sensor;

typedef struct {
    SensorCategory category;
    char          *name;    /* "Intel(R) Core(TM) i7…", "WD SN5000S"  */
    char          *source;  /* driver / hwmon name, shown dimmed      */
    char          *key;     /* "cpu", "gpu-0000-01-00-0": name overrides */
    GPtrArray     *sensors; /* of Sensor*                             */

    /* NVIDIA proprietary driver (no hwmon): read through nvidia-smi */
    char          *nv_bus;  /* "0000:01:00.0" or NULL                 */
    gboolean       asleep;  /* GPU in runtime power saving            */

    /* graphics cards only */
    char          *pci_dir; /* /sys/bus/pci/devices/…                 */
    double         gpu_usage;              /* %, < 0 = unknown        */
    double         vram_used, vram_total;  /* MiB, total 0 = unknown  */
} Device;

typedef void (*TempfmReadDone)(gpointer data);

GPtrArray  *tempfm_sensors_discover(void);         /* of Device*      */
void        tempfm_sensors_read(GPtrArray *devices);
gboolean    tempfm_sensors_has_nvidia(GPtrArray *devices);
void        tempfm_sensors_read_nvidia_async(GPtrArray *devices,
                                             GCancellable *cancel,
                                             TempfmReadDone done,
                                             gpointer data);
Sensor     *tempfm_device_main_sensor(Device *d);  /* NULL if none    */
const char *tempfm_device_name(Device *d);         /* with override   */
TempLevel   tempfm_sensor_level(const Sensor *s);
const char *tempfm_level_name(TempLevel l);
const char *tempfm_level_class(TempLevel l);
const char *tempfm_category_name(SensorCategory c);
const char *tempfm_category_icon(SensorCategory c);       /* emoji     */
const char *tempfm_category_icon_name(SensorCategory c);  /* Tela name */
char       *tempfm_cpu_model(void);
char       *tempfm_pci_name(const char *pci_dir);  /* from pci.ids    */
char       *tempfm_gpu_key(const char *bus);       /* "gpu-0000-01-00-0" */


/* ── monitor (monitor.c) ── */

typedef struct {
    double cpu_usage;           /* %, < 0 = unknown                     */
    double cpu_mhz;             /* average clock, 0 = unknown           */
    double ram_total, ram_used; /* bytes                                */
    double swap_total, swap_used;
    double disk_busy;           /* % of the busiest disk, < 0 = unknown */
    int    battery;             /* %, < 0 = no battery                  */
    gboolean charging;
} SystemStats;

/* rebuilt: the device list was replaced (Refresh) */
typedef void (*TempfmListener)(gboolean rebuilt, gpointer data);

void               tempfm_monitor_start(void);
void               tempfm_monitor_stop(void);
GPtrArray         *tempfm_monitor_devices(void);
const SystemStats *tempfm_monitor_stats(void);
void               tempfm_monitor_rediscover(void);
void               tempfm_monitor_restart_timer(void);
void               tempfm_monitor_notify(void);     /* names, units… */
guint              tempfm_monitor_listen(TempfmListener cb, gpointer data);
void               tempfm_monitor_unlisten(guint id);
int                tempfm_installed_ram_gb(void);   /* detected       */
int                tempfm_ram_gb(void);             /* with override  */


/* ── persistent preferences (settings.c) ── */

typedef struct {
    TempfmTheme     theme;
    TempfmIconStyle icon_style;
    TempUnit        unit;
    int             interval;      /* seconds between readings */
    int             win_width;
    int             win_height;
    gboolean        win_maximized;
    gboolean        keep_in_tray;  /* closing the window hides it */
    gboolean        autostart;     /* start (in the background) at login */
    gboolean        keep_awake;    /* no automatic sleep / screen off */

    gboolean        ov_enabled;
    OverlayCorner   ov_corner;
    int             ov_monitor;    /* index of the screen         */
    int             ov_size;       /* 0 small, 1 medium, 2 large  */
    int             ov_opacity;    /* background, 0–100 %         */
    gboolean        ov_items[OV_COUNT];

    GHashTable     *spec_names;    /* key → name, see Device.key  */
    int             spec_ram_gb;   /* 0 = not changed             */
} TempfmSettings;

TempfmSettings *tempfm_settings(void);              /* loaded on first use */
void            tempfm_settings_save(void);
const char     *tempfm_settings_path(void);
const char     *tempfm_overlay_item_name(OverlayItem i);
void            tempfm_open_settings(void);
void            tempfm_settings_changed(gboolean icons_changed); /* refresh all */
void            tempfm_autostart_sync(void);        /* ~/.config/autostart */

/* settings window building blocks (ExploreR "props" style) */
GtkWidget *tempfm_window_titlebar(GtkWidget *win, const char *title);
GtkWidget *tempfm_group_title(const char *text);
GtkWidget *tempfm_card_new(void);
GtkWidget *tempfm_card_add_widget(GtkWidget *card, const char *label, GtkWidget *w);


/* ── icons (icons.c) ── */

GtkWidget *tempfm_icon(const char *icon_name, const char *emoji, int pixel_size);
gboolean   tempfm_tela_available(void);
char      *tempfm_tela_file(const char *icon_name, int size);  /* path or NULL */


/* ── gui (gui.c) ── */

void       tempfm_gui_apply_theme(void);
GtkWidget *tempfm_window_new(GtkApplication *app, gboolean show);
void       tempfm_gui_show(void);                   /* from the tray  */
void       tempfm_gui_toggle(void);
void       tempfm_gui_sync(gboolean icons_changed); /* settings changed */
char      *tempfm_format_temp(double celsius);      /* "53 °C" / "127 °F" */
void       tempfm_quit(void);


/* ── overlay (overlay.c) ── */

void        tempfm_overlay_apply(void);             /* show/hide/restyle */
gboolean    tempfm_overlay_on_top(void);            /* can stay above full screen */
GListModel *tempfm_overlay_monitors(void);


/* ── tray (tray.c) ── */

void       tempfm_tray_start(GApplication *app);
gboolean   tempfm_tray_available(void);
void       tempfm_tray_sync(void);


/* ── specs window (specs.c) ── */

void       tempfm_open_specs(void);
gboolean   tempfm_specs_modified(void);


/* ── fan curves window (fancontrol.c) ── */

void       tempfm_open_fan_control(void);
void       tempfm_fan_service_wake(void);       /* at start: curves back on */


/* ── keyboard light window (keyboard.c) ── */

gboolean   tempfm_keyboard_available(void);     /* laptop with a lit keyboard */
void       tempfm_open_keyboard(void);


/* ── keep awake (awake.c) ── */

void       tempfm_awake_apply(void);            /* on/off from the settings */

#endif
