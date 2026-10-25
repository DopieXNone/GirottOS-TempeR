/* temper-fand.c – TempeR fan control service.
 *
 * Runs as root (temper-fand.service) and is the only part of TempeR that
 * writes to the hardware.  The TempeR window talks to it over the system
 * D-Bus (com.girottos.TempeR.FanControl); every change is checked with
 * polkit (com.girottos.temper.fan-control: the user sitting at the
 * computer is allowed, anybody else needs the admin password).
 *
 * Two kinds of fans:
 *
 *   ASUS laptops   asus_custom_fan_curve (asus-wmi): 8 points per fan,
 *                  temperature -> speed, followed by the firmware itself,
 *                  exactly what Armoury Crate does on Windows.  The CPU fan
 *                  follows the CPU, the GPU fan the GPU.
 *   other chips    hwmon pwmN with pwmN_enable (nct67xx, it87, thinkpad,
 *                  dell_smm, older ASUS laptops…):
 *                  the same 8 point curve, followed here once a second
 *                  against the CPU temperature.  Graphics cards are left to
 *                  their driver.
 *
 * While the curves are on nobody else drives the fans:
 *
 *   - the firmware drops custom curves when the platform profile changes
 *     (power-profiles-daemon, asusd, Fn+F5) or after a resume: they are
 *     checked every second and written again;
 *   - asusd keeps running (keyboard light, charge limit, profiles…) but
 *     its own fan curves are switched off, and switched off again if
 *     something turns them back on;
 *   - fan control daemons (CoolerControl, fancontrol, fan2go, nbfc,
 *     thinkfan…) are stopped and masked until the next boot (--runtime),
 *     and stopped again at every boot.
 *
 * Turning the curves off gives everything back: firmware automatic mode,
 * asusd's curves as they were, the daemons started again.  The platform
 * profile, power management and the kernel's thermal protection are never
 * touched.
 *
 * TempeR comes first: while the curves are on the service starts at boot
 * (it enables itself again if somebody disabled it) and TempeR wakes it at
 * login too, so asusd and ROG Control Center, which apply their own curves
 * when they start, are switched off again within a second.
 *
 * It also sets the colour and brightness of a laptop's keyboard light,
 * straight through the kernel (/sys/class/leds/…::kbd_backlight), without
 * asusd, OpenRGB or other vendor tools:
 *
 *   ASUS           kbd_rgb_mode (asus-wmi): static colour
 *   System76       color (system76_acpi): RRGGBB
 *   others         multi_intensity (multicolour LED class: Clevo, Tuxedo…)
 *   no colour      brightness only
 */

#include <gio/gio.h>
#include <glib-unix.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONF_DIR      "/etc/temper"
#define CONF_FILE     CONF_DIR "/fan-control.ini"
#define STATE_DIR     "/var/lib/temper"
#define STATE_FILE    STATE_DIR "/fan-control.state"
#define HWMON_DIR     "/sys/class/hwmon"
#define ASUSD_CURVES  "/etc/asusd/fan_curves.ron"
#define SELF_UNIT     "temper-fand.service"
#define KBD_FILE      CONF_DIR "/keyboard.ini"
#define LEDS_DIR      "/sys/class/leds"

#define BUS_NAME      "com.girottos.TempeR.FanControl"
#define OBJ_PATH      "/com/girottos/TempeR/FanControl"
#define IFACE         "com.girottos.TempeR.FanControl"
#define ACTION_ID     "com.girottos.temper.fan-control"
#define KBD_ACTION_ID "com.girottos.temper.keyboard-light"

#define N_POINTS      8
#define MIN_TEMP      10
#define MAX_TEMP      110
/* from SAFE_TEMP up the fans always turn at least at SAFE_PWM (50 %) */
#define SAFE_TEMP     85
#define SAFE_PWM      128
/* without a session asking for anything and with the curves off, exit:
 * D-Bus starts the service again when TempeR needs it */
#define IDLE_EXIT_S   120

typedef enum { FAN_ASUS, FAN_PWM } FanType;

typedef struct {
    char    *id;            /* "asus-cpu", "nct6798-pwm2"          */
    char    *label;         /* "CPU fan"                            */
    char    *follows;       /* "CPU temperature"                    */
    FanType  type;
    char    *dir;           /* hwmon folder                         */
    int      index;         /* N of pwmN                            */
    guint8   temp[N_POINTS], pwm[N_POINTS];          /* the curve   */
    guint8   def_temp[N_POINTS], def_pwm[N_POINTS];  /* default     */

    /* FAN_PWM: followed here */
    int      orig_enable;   /* pwmN_enable before we took it, -1    */
    int      last_pwm;
    int      lower_ticks;
    gboolean refused;       /* the kernel does not let us drive it  */
} Fan;

/* fan control daemons stopped while the curves are on */
static const struct { const char *unit, *name; } CONFLICTS[] = {
    { "coolercontrold.service",         "CoolerControl"  },
    { "coolercontrol-liqctld.service",  "CoolerControl (liquidctl)" },
    { "fancontrol.service",             "fancontrol (lm_sensors)" },
    { "fan2go.service",                 "fan2go"         },
    { "nbfc_service.service",           "NoteBook FanControl" },
    { "nbfc.service",                   "NoteBook FanControl" },
    { "thinkfan.service",               "thinkfan"       },
    { "i8kmon.service",                 "i8kmon"         },
    { "mbpfan.service",                 "mbpfan"         },
    { "amdgpu-fan.service",             "amdgpu-fan"     },
};

static GMainLoop       *loop;
static GDBusConnection *bus;
static GDBusNodeInfo   *introspection;
static GPtrArray       *fans;           /* Fan* */
static gboolean         enabled;
static char            *cpu_temp_dir;   /* hwmon of the CPU, for FAN_PWM */
static gint64           last_activity;
static guint            tick_n;
static int              n_pending;      /* requests waiting for polkit */

/* what we took away, to give it back: survives restarts and reboots */
static GPtrArray       *paused_units;   /* units we stopped and masked  */
static GPtrArray       *masked_units;   /* units we only masked         */
static GPtrArray       *asusd_curves;   /* "balanced:cpu" we switched off */
static gint64           asusd_mtime;

/* keyboard light */
typedef enum { KBD_NONE, KBD_MONO, KBD_ASUS, KBD_S76, KBD_MULTI } KbdType;
static KbdType          kbd_type;
static char            *kbd_dir;        /* /sys/class/leds/asus::kbd_backlight */
static int              kbd_max;        /* max_brightness */
static int              kbd_bright = -1;/* saved, -1 = never set */
static guint8           kbd_rgb[3] = { 255, 255, 255 };


/* ════════════════════════════════════════════════════════════
 * SMALL HELPERS
 * ════════════════════════════════════════════════════════════ */

static int
read_int(const char *dir, const char *file, int def)
{
    char *p = g_build_filename(dir, file, NULL);
    char *t = NULL;
    int v = def;
    if (g_file_get_contents(p, &t, NULL, NULL)) {
        char *end = NULL;
        long l = strtol(t, &end, 10);
        if (end != t) v = (int)l;
    }
    g_free(t);
    g_free(p);
    return v;
}

static gboolean
write_int(const char *dir, const char *file, int v)
{
    char *p = g_build_filename(dir, file, NULL);
    char buf[16];
    g_snprintf(buf, sizeof buf, "%d", v);
    /* sysfs: one write() of the whole value, no temporary file */
    FILE *f = fopen(p, "w");
    gboolean ok = f && fputs(buf, f) >= 0;
    if (f && fclose(f) != 0) ok = FALSE;
    if (!ok) g_printerr("temper-fand: cannot write %d to %s\n", v, p);
    g_free(p);
    return ok;
}

static char *
read_line(const char *dir, const char *file)
{
    char *p = g_build_filename(dir, file, NULL);
    char *t = NULL;
    g_file_get_contents(p, &t, NULL, NULL);
    g_free(p);
    return t ? g_strstrip(t) : NULL;
}

static gboolean
str_in(GPtrArray *a, const char *s)
{
    for (guint i = 0; i < a->len; i++)
        if (!strcmp(a->pdata[i], s)) return TRUE;
    return FALSE;
}

static void
add_unique(GPtrArray *a, const char *s)
{
    if (!str_in(a, s)) g_ptr_array_add(a, g_strdup(s));
}

/* runs a command, quietly; TRUE when it exits with 0 */
static gboolean
run(const char * const *argv)
{
    int status = 0;
    GError *err = NULL;
    if (!g_spawn_sync(NULL, (char **)argv, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                      G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, NULL, NULL, &status, &err)) {
        g_printerr("temper-fand: cannot run %s: %s\n", argv[0], err->message);
        g_error_free(err);
        return FALSE;
    }
    return g_spawn_check_wait_status(status, NULL);
}

static void
systemctl(const char *verb, const char *flag, const char *unit)
{
    const char *argv[] = { "systemctl", verb, flag ? flag : unit, flag ? unit : NULL, NULL };
    run(argv);
}

static gboolean
unit_active(const char *unit)
{
    const char *argv[] = { "systemctl", "is-active", "--quiet", unit, NULL };
    return run(argv);
}

static gboolean
unit_installed(const char *unit)
{
    static const char *dirs[] = { "/etc/systemd/system", "/usr/lib/systemd/system",
                                  "/lib/systemd/system", "/usr/local/lib/systemd/system", NULL };
    for (int i = 0; dirs[i]; i++) {
        char *p = g_build_filename(dirs[i], unit, NULL);
        gboolean ok = g_file_test(p, G_FILE_TEST_EXISTS);
        g_free(p);
        if (ok) return TRUE;
    }
    return FALSE;
}

static const char *
unit_name(const char *unit)
{
    for (guint i = 0; i < G_N_ELEMENTS(CONFLICTS); i++)
        if (!strcmp(CONFLICTS[i].unit, unit)) return CONFLICTS[i].name;
    return unit;
}


/* ════════════════════════════════════════════════════════════
 * CURVES
 * ════════════════════════════════════════════════════════════ */

/* Temperatures strictly rising, speeds never falling, and from SAFE_TEMP
 * up at least SAFE_PWM: whatever a client sends, the result is a curve
 * the firmware accepts and that cannot let the computer overheat. */
static void
sanitize(guint8 *temp, guint8 *pwm)
{
    for (int i = 0; i < N_POINTS; i++) {
        int t = CLAMP(temp[i], MIN_TEMP, MAX_TEMP - (N_POINTS - 1 - i));
        if (i > 0 && t <= temp[i - 1]) t = temp[i - 1] + 1;
        temp[i] = (guint8)t;
        if (i > 0 && pwm[i] < pwm[i - 1]) pwm[i] = pwm[i - 1];
    }
    for (int i = 0; i < N_POINTS; i++)
        if (temp[i] >= SAFE_TEMP && pwm[i] < SAFE_PWM) pwm[i] = SAFE_PWM;
    /* above the last point the fan keeps the last speed */
    if (pwm[N_POINTS - 1] < SAFE_PWM) pwm[N_POINTS - 1] = SAFE_PWM;
    for (int i = 1; i < N_POINTS; i++)
        if (pwm[i] < pwm[i - 1]) pwm[i] = pwm[i - 1];
}

static int
curve_at(const Fan *f, double t)
{
    if (t <= f->temp[0]) return f->pwm[0];
    for (int i = 1; i < N_POINTS; i++)
        if (t <= f->temp[i]) {
            double k = (t - f->temp[i - 1]) / (double)(f->temp[i] - f->temp[i - 1]);
            return (int)(f->pwm[i - 1] + k * (f->pwm[i] - f->pwm[i - 1]) + 0.5);
        }
    return f->pwm[N_POINTS - 1];
}

/* a default for chips without a firmware curve: quiet when cool, full
 * speed at 95 °C */
static const guint8 PWM_DEF_TEMP[N_POINTS] = { 30, 40, 50, 60, 70, 80, 90, 95 };
static const guint8 PWM_DEF_PWM[N_POINTS]  = { 51, 64, 89, 115, 153, 191, 230, 255 };


/* ════════════════════════════════════════════════════════════
 * FANS
 * ════════════════════════════════════════════════════════════ */

static void
fan_free(gpointer p)
{
    Fan *f = p;
    g_free(f->id);
    g_free(f->label);
    g_free(f->follows);
    g_free(f->dir);
    g_free(f);
}

static Fan *
find_fan(const char *id)
{
    for (guint i = 0; fans && i < fans->len; i++)
        if (!strcmp(((Fan *)fans->pdata[i])->id, id)) return fans->pdata[i];
    return NULL;
}

static void
asus_read_points(Fan *f, guint8 *temp, guint8 *pwm)
{
    for (int i = 0; i < N_POINTS; i++) {
        char n[48];
        g_snprintf(n, sizeof n, "pwm%d_auto_point%d_temp", f->index, i + 1);
        temp[i] = (guint8)CLAMP(read_int(f->dir, n, 0), 0, 255);
        g_snprintf(n, sizeof n, "pwm%d_auto_point%d_pwm", f->index, i + 1);
        pwm[i] = (guint8)CLAMP(read_int(f->dir, n, 0), 0, 255);
    }
}

static gboolean
is_gpu_hwmon(const char *dir)
{
    char *dev = g_build_filename(dir, "device", NULL);
    char *cls = read_line(dev, "class");
    gboolean gpu = cls && g_str_has_prefix(cls, "0x03");
    g_free(cls);
    g_free(dev);
    return gpu;
}

static void
discover(void)
{
    if (fans) g_ptr_array_unref(fans);
    fans = g_ptr_array_new_with_free_func(fan_free);
    g_clear_pointer(&cpu_temp_dir, g_free);

    GDir *d = g_dir_open(HWMON_DIR, 0, NULL);
    if (!d) return;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const char *e;
    while ((e = g_dir_read_name(d)) != NULL) g_ptr_array_add(names, g_strdup(e));
    g_dir_close(d);
    g_ptr_array_sort(names, (GCompareFunc)g_strcmp0);  /* stable ids order */

    /* ASUS laptops with firmware curves use those; older ones may still let
     * the "asus" chip's pwm1 be set by hand: a software curve then */
    gboolean asus_curves = FALSE;
    for (guint k = 0; k < names->len; k++) {
        char *dir = g_build_filename(HWMON_DIR, names->pdata[k], NULL);
        char *name = read_line(dir, "name");
        if (name && !strcmp(name, "asus_custom_fan_curve")) asus_curves = TRUE;
        g_free(name);
        g_free(dir);
    }

    for (guint k = 0; k < names->len; k++) {
        char *dir = g_build_filename(HWMON_DIR, names->pdata[k], NULL);
        char *name = read_line(dir, "name");
        if (!name) { g_free(dir); continue; }

        if (!strcmp(name, "coretemp") || !strcmp(name, "k10temp") ||
            !strcmp(name, "zenpower") || !strcmp(name, "k8temp") ||
            !strcmp(name, "via_cputemp") || !strcmp(name, "cpu_thermal") ||
            !strcmp(name, "soc_thermal")) {
            if (!cpu_temp_dir) cpu_temp_dir = g_strdup(dir);
        } else if (!strcmp(name, "asus_custom_fan_curve")) {
            static const char *ids[]     = { NULL, "asus-cpu", "asus-gpu", "asus-mid" };
            static const char *labels[]  = { NULL, "CPU fan", "GPU fan", "Middle fan" };
            static const char *follows[] = { NULL, "CPU temperature", "GPU temperature",
                                             "System temperature" };
            for (int n = 1; n <= 3; n++) {
                char en[32];
                g_snprintf(en, sizeof en, "pwm%d_enable", n);
                char *p = g_build_filename(dir, en, NULL);
                gboolean has = g_file_test(p, G_FILE_TEST_EXISTS);
                g_free(p);
                if (!has) continue;
                Fan *f = g_new0(Fan, 1);
                f->id = g_strdup(ids[n]);
                f->label = g_strdup(labels[n]);
                f->follows = g_strdup(follows[n]);
                f->type = FAN_ASUS;
                f->dir = g_strdup(dir);
                f->index = n;
                f->orig_enable = -1;
                f->last_pwm = -1;
                /* what the firmware (or asusd) uses now: the starting point */
                asus_read_points(f, f->def_temp, f->def_pwm);
                sanitize(f->def_temp, f->def_pwm);
                memcpy(f->temp, f->def_temp, N_POINTS);
                memcpy(f->pwm, f->def_pwm, N_POINTS);
                g_ptr_array_add(fans, f);
            }
        } else if ((strcmp(name, "asus") || !asus_curves) && !is_gpu_hwmon(dir)) {
            for (int n = 1; n <= 8; n++) {
                char a[32], b[32];
                g_snprintf(a, sizeof a, "pwm%d", n);
                g_snprintf(b, sizeof b, "pwm%d_enable", n);
                char *pa = g_build_filename(dir, a, NULL);
                char *pb = g_build_filename(dir, b, NULL);
                gboolean has = g_file_test(pa, G_FILE_TEST_EXISTS) &&
                               g_file_test(pb, G_FILE_TEST_EXISTS) &&
                               access(pa, W_OK) == 0;
                g_free(pa);
                g_free(pb);
                if (!has) continue;
                char lf[32];
                g_snprintf(lf, sizeof lf, "fan%d_label", n);
                char *lbl = read_line(dir, lf);
                Fan *f = g_new0(Fan, 1);
                f->id = g_strdup_printf("%s-pwm%d", name, n);
                f->label = lbl && *lbl ? g_strdup(lbl) : g_strdup_printf("%s fan %d", name, n);
                f->follows = g_strdup("CPU temperature");
                f->type = FAN_PWM;
                f->dir = g_strdup(dir);
                f->index = n;
                f->orig_enable = -1;
                f->last_pwm = -1;
                memcpy(f->def_temp, PWM_DEF_TEMP, N_POINTS);
                memcpy(f->def_pwm, PWM_DEF_PWM, N_POINTS);
                memcpy(f->temp, PWM_DEF_TEMP, N_POINTS);
                memcpy(f->pwm, PWM_DEF_PWM, N_POINTS);
                g_ptr_array_add(fans, f);
                g_free(lbl);
            }
        }
        g_free(name);
        g_free(dir);
    }
    g_ptr_array_unref(names);

    /* software curves need a CPU temperature */
    if (!cpu_temp_dir)
        for (guint i = fans->len; i > 0; i--)
            if (((Fan *)fans->pdata[i - 1])->type == FAN_PWM)
                g_ptr_array_remove_index(fans, i - 1);
}

static double
cpu_temp(void)
{
    double top = -1;
    GDir *d = cpu_temp_dir ? g_dir_open(cpu_temp_dir, 0, NULL) : NULL;
    if (!d) return top;
    const char *e;
    while ((e = g_dir_read_name(d)) != NULL) {
        if (!g_str_has_prefix(e, "temp") || !g_str_has_suffix(e, "_input")) continue;
        int v = read_int(cpu_temp_dir, e, -1);
        if (v > 0 && v / 1000.0 > top) top = v / 1000.0;
    }
    g_dir_close(d);
    return top;
}


/* ── applying ── */

static gboolean
asus_matches(Fan *f)
{
    char en[32];
    g_snprintf(en, sizeof en, "pwm%d_enable", f->index);
    if (read_int(f->dir, en, -1) != 1)
        return FALSE;
    guint8 t[N_POINTS], p[N_POINTS];
    asus_read_points(f, t, p);
    return !memcmp(t, f->temp, N_POINTS) && !memcmp(p, f->pwm, N_POINTS);
}

static gboolean
asus_apply(Fan *f)
{
    gboolean ok = TRUE;
    for (int i = 0; i < N_POINTS; i++) {
        char n[48];
        g_snprintf(n, sizeof n, "pwm%d_auto_point%d_temp", f->index, i + 1);
        ok &= write_int(f->dir, n, f->temp[i]);
        g_snprintf(n, sizeof n, "pwm%d_auto_point%d_pwm", f->index, i + 1);
        ok &= write_int(f->dir, n, f->pwm[i]);
    }
    char en[32];
    g_snprintf(en, sizeof en, "pwm%d_enable", f->index);
    /* 1 = custom curve: the kernel sends all 8 points to the firmware */
    ok &= write_int(f->dir, en, 1);
    return ok;
}

static void
pwm_step(Fan *f)
{
    char en[32], pw[16];
    g_snprintf(en, sizeof en, "pwm%d_enable", f->index);
    g_snprintf(pw, sizeof pw, "pwm%d", f->index);

    double t = cpu_temp();
    /* no reading: full speed is the only safe guess */
    int want = t < 0 ? 255 : curve_at(f, t);

    if (f->refused) return;
    int mode = read_int(f->dir, en, -1);
    if (mode != 1) {
        if (!write_int(f->dir, en, 1)) {
            /* e.g. thinkpad_acpi without fan_control=1: said once, then
             * the fan stays with the firmware */
            g_printerr("temper-fand: %s can't be controlled (the driver refuses it), "
                       "leaving it to the firmware\n", f->label);
            f->refused = TRUE;
            return;
        }
        if (f->orig_enable < 0) f->orig_enable = mode;
        else g_printerr("temper-fand: %s was taken back, taking control again\n", f->label);
        f->last_pwm = -1;
    }

    /* faster at once, slower only after 3 s: no up-and-down hunting */
    if (f->last_pwm >= 0 && want < f->last_pwm) {
        if (++f->lower_ticks < 3) return;
    }
    f->lower_ticks = 0;
    if (want != f->last_pwm || read_int(f->dir, pw, -1) != want) {
        write_int(f->dir, pw, want);
        f->last_pwm = want;
    }
}

static void
fan_release(Fan *f)
{
    char en[32];
    g_snprintf(en, sizeof en, "pwm%d_enable", f->index);
    if (f->type == FAN_ASUS) {
        /* 2 = automatic: the firmware's own curve for the profile */
        if (read_int(f->dir, en, 2) != 2) write_int(f->dir, en, 2);
    } else if (f->orig_enable >= 0) {
        write_int(f->dir, en, f->orig_enable);
        f->orig_enable = -1;
        f->last_pwm = -1;
    }
}


/* ════════════════════════════════════════════════════════════
 * CONFIGURATION AND STATE FILES
 * ════════════════════════════════════════════════════════════ */

static void
load_config(void)
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, CONF_FILE, G_KEY_FILE_NONE, NULL);
    enabled = g_key_file_get_boolean(kf, "General", "enabled", NULL);
    for (guint i = 0; i < fans->len; i++) {
        Fan *f = fans->pdata[i];
        gsize nt = 0, np = 0;
        int *t = g_key_file_get_integer_list(kf, f->id, "temperatures", &nt, NULL);
        int *p = g_key_file_get_integer_list(kf, f->id, "speeds", &np, NULL);
        if (t && p && nt == N_POINTS && np == N_POINTS) {
            for (int k = 0; k < N_POINTS; k++) {
                f->temp[k] = (guint8)CLAMP(t[k], 0, 255);
                f->pwm[k]  = (guint8)CLAMP(p[k], 0, 255);
            }
            sanitize(f->temp, f->pwm);
        }
        g_free(t);
        g_free(p);
    }
    g_key_file_free(kf);
}

static void
save_config(void)
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_comment(kf, NULL, NULL,
        " TempeR fan curves, written by temper-fand.\n"
        " temperatures in °C, speeds 0-255 (255 = 100 %), 8 points per fan.", NULL);
    g_key_file_set_boolean(kf, "General", "enabled", enabled);
    for (guint i = 0; i < fans->len; i++) {
        Fan *f = fans->pdata[i];
        int t[N_POINTS], p[N_POINTS];
        for (int k = 0; k < N_POINTS; k++) { t[k] = f->temp[k]; p[k] = f->pwm[k]; }
        g_key_file_set_integer_list(kf, f->id, "temperatures", t, N_POINTS);
        g_key_file_set_integer_list(kf, f->id, "speeds", p, N_POINTS);
    }
    g_mkdir_with_parents(CONF_DIR, 0755);
    GError *err = NULL;
    if (!g_key_file_save_to_file(kf, CONF_FILE, &err)) {
        g_printerr("temper-fand: cannot save %s: %s\n", CONF_FILE, err->message);
        g_error_free(err);
    }
    g_key_file_free(kf);
}

static void
load_list(GKeyFile *kf, const char *key, GPtrArray *into)
{
    char **v = g_key_file_get_string_list(kf, "Paused", key, NULL, NULL);
    for (int i = 0; v && v[i]; i++)
        if (*v[i]) add_unique(into, v[i]);
    g_strfreev(v);
}

static void
load_state(void)
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, STATE_FILE, G_KEY_FILE_NONE, NULL);
    load_list(kf, "stopped-units", paused_units);
    load_list(kf, "masked-units", masked_units);
    load_list(kf, "asusd-curves", asusd_curves);
    g_key_file_free(kf);
}

static void
save_state(void)
{
    if (!paused_units->len && !masked_units->len && !asusd_curves->len) {
        unlink(STATE_FILE);
        return;
    }
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_comment(kf, NULL, NULL,
        " What temper-fand switched off while its fan curves are on,\n"
        " given back when they are turned off (or with temper-fand --release).", NULL);
    g_key_file_set_string_list(kf, "Paused", "stopped-units",
        (const char * const *)paused_units->pdata, paused_units->len);
    g_key_file_set_string_list(kf, "Paused", "masked-units",
        (const char * const *)masked_units->pdata, masked_units->len);
    g_key_file_set_string_list(kf, "Paused", "asusd-curves",
        (const char * const *)asusd_curves->pdata, asusd_curves->len);
    g_mkdir_with_parents(STATE_DIR, 0755);
    g_key_file_save_to_file(kf, STATE_FILE, NULL);
    g_key_file_free(kf);
}


/* ════════════════════════════════════════════════════════════
 * OTHER FAN CONTROLLERS
 * ════════════════════════════════════════════════════════════ */

/* "balanced:cpu" for every curve asusd has switched on, from its own
 * config file (asusd rewrites it whenever something changes). */
static GPtrArray *
asusd_enabled_curves(void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    char *txt = NULL;
    if (!g_file_get_contents(ASUSD_CURVES, &txt, NULL, NULL)) return out;
    char **lines = g_strsplit(txt, "\n", -1);
    char profile[32] = "", fan[16] = "";
    for (int i = 0; lines[i]; i++) {
        char *l = g_strstrip(lines[i]);
        char word[32];
        if (sscanf(l, "%31[a-zA-Z]: [", word) == 1 && strstr(l, "[")) {
            g_strlcpy(profile, word, sizeof profile);
        } else if (sscanf(l, "fan: %15[a-zA-Z]", word) == 1) {
            g_strlcpy(fan, word, sizeof fan);
        } else if (g_str_has_prefix(l, "enabled: true") && *profile && *fan) {
            char *p = g_ascii_strdown(profile, -1), *f = g_ascii_strdown(fan, -1);
            g_ptr_array_add(out, g_strdup_printf("%s:%s", p, f));
            g_free(p);
            g_free(f);
        }
    }
    g_strfreev(lines);
    g_free(txt);
    return out;
}

static gint64
file_mtime(const char *path)
{
    GStatBuf st;
    return g_stat(path, &st) == 0 ? (gint64)st.st_mtime : 0;
}

static gboolean
asusd_present(void)
{
    char *p = g_find_program_in_path("asusctl");
    gboolean ok = p && g_file_test(ASUSD_CURVES, G_FILE_TEST_EXISTS) && unit_active("asusd.service");
    g_free(p);
    return ok;
}

static void
asusd_set_curve(const char *pair, gboolean on)
{
    char *profile = g_strdup(pair);
    char *fan = strchr(profile, ':');
    if (!fan) { g_free(profile); return; }
    *fan++ = '\0';
    const char *argv[] = { "asusctl", "fan-curve", "--mod-profile", profile,
                           "--fan", fan, "--enable-fan-curve", on ? "true" : "false", NULL };
    if (!run(argv))
        g_printerr("temper-fand: asusctl could not %s the %s fan curve of %s\n",
                   on ? "enable" : "disable", fan, profile);
    g_free(profile);
}

/* Switches off asusd's curves, also when something switched them on again */
static void
asusd_take(void)
{
    gint64 mt = file_mtime(ASUSD_CURVES);
    if (mt && mt == asusd_mtime) return;
    if (!asusd_present()) {
        asusd_mtime = mt;               /* looked again every 5 s */
        return;
    }
    GPtrArray *on = asusd_enabled_curves();
    for (guint i = 0; i < on->len; i++) {
        g_print("temper-fand: switching off asusd's fan curve %s\n", (char *)on->pdata[i]);
        asusd_set_curve(on->pdata[i], FALSE);
        add_unique(asusd_curves, on->pdata[i]);
    }
    if (on->len) save_state();
    g_ptr_array_unref(on);
    asusd_mtime = file_mtime(ASUSD_CURVES);
}

static void
units_take(void)
{
    gboolean changed = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(CONFLICTS); i++) {
        const char *u = CONFLICTS[i].unit;
        if (!unit_installed(u)) continue;
        gboolean active = unit_active(u);
        if (!str_in(masked_units, u) && !str_in(paused_units, u)) {
            /* masked first, so nothing can start it again meanwhile */
            systemctl("mask", "--runtime", u);
            add_unique(active ? paused_units : masked_units, u);
            changed = TRUE;
        } else {
            /* after a reboot the runtime mask is gone */
            char *p = g_build_filename("/run/systemd/system", u, NULL);
            if (!g_file_test(p, G_FILE_TEST_IS_SYMLINK))
                systemctl("mask", "--runtime", u);
            g_free(p);
        }
        if (active) {
            g_print("temper-fand: stopping %s (%s)\n", unit_name(u), u);
            systemctl("stop", NULL, u);
            if (!str_in(paused_units, u)) {
                /* masked while off, started since: start it again later */
                for (guint k = 0; k < masked_units->len; k++)
                    if (!strcmp(masked_units->pdata[k], u)) {
                        g_ptr_array_remove_index(masked_units, k);
                        break;
                    }
                add_unique(paused_units, u);
            }
            changed = TRUE;
        }
    }
    if (changed) save_state();
}

static void
others_take(void)
{
    units_take();
    asusd_take();
}

static void
others_give_back(void)
{
    if (asusd_curves->len && asusd_present())
        for (guint i = 0; i < asusd_curves->len; i++) {
            g_print("temper-fand: switching asusd's fan curve %s back on\n",
                    (char *)asusd_curves->pdata[i]);
            asusd_set_curve(asusd_curves->pdata[i], TRUE);
        }
    g_ptr_array_set_size(asusd_curves, 0);
    asusd_mtime = 0;

    for (guint i = 0; i < masked_units->len; i++)
        systemctl("unmask", "--runtime", masked_units->pdata[i]);
    for (guint i = 0; i < paused_units->len; i++) {
        systemctl("unmask", "--runtime", paused_units->pdata[i]);
        g_print("temper-fand: starting %s again\n", unit_name(paused_units->pdata[i]));
        systemctl("start", NULL, paused_units->pdata[i]);
    }
    g_ptr_array_set_size(masked_units, 0);
    g_ptr_array_set_size(paused_units, 0);
    save_state();
}


/* ════════════════════════════════════════════════════════════
 * ON / OFF
 * ════════════════════════════════════════════════════════════ */

static void
take_control(void)
{
    others_take();
    for (guint i = 0; i < fans->len; i++) {
        Fan *f = fans->pdata[i];
        if (f->type == FAN_ASUS) asus_apply(f);
        else                     pwm_step(f);
    }
}

static void
release_fans(void)
{
    for (guint i = 0; fans && i < fans->len; i++)
        fan_release(fans->pdata[i]);
}

static void
emit_changed(void)
{
    if (bus)
        g_dbus_connection_emit_signal(bus, NULL, OBJ_PATH, IFACE, "Changed", NULL, NULL);
}

/* curves on: the service must start at every boot, before the session
 * (and ROG Control Center) does.  Enabled again if it was disabled by hand
 * or by an old package. */
static void
ensure_boot_start(void)
{
    const char *argv[] = { "systemctl", "is-enabled", "--quiet", SELF_UNIT, NULL };
    if (!run(argv)) {
        g_print("temper-fand: enabling %s, so the curves are on from boot\n", SELF_UNIT);
        systemctl("enable", NULL, SELF_UNIT);
    }
}

static void
set_enabled(gboolean on)
{
    if (on == enabled) return;
    enabled = on;
    save_config();
    if (on) {
        /* curves on: start at every boot too */
        ensure_boot_start();
        take_control();
    } else {
        release_fans();
        others_give_back();
        systemctl("disable", NULL, SELF_UNIT);
    }
    emit_changed();
}

static gboolean
on_tick(gpointer ud)
{
    (void)ud;
    tick_n++;
    if (enabled) {
        for (guint i = 0; i < fans->len; i++) {
            Fan *f = fans->pdata[i];
            if (f->type == FAN_PWM) {
                pwm_step(f);
            } else if (!asus_matches(f)) {
                /* platform profile changed, resume, or someone else wrote */
                g_print("temper-fand: %s curve was replaced, applying it again\n", f->label);
                asus_apply(f);
            }
        }
        /* asusd's file is checked every second (only its date, unless it
         * changed): ROG Control Center turning its curves on at login is
         * undone at once; the other programs every 5 s */
        if (tick_n % 5 == 0) {
            units_take();
            asusd_mtime = 0;            /* asusd started meanwhile? */
        }
        asusd_take();
    } else if (!n_pending &&
               g_get_monotonic_time() - last_activity > (gint64)IDLE_EXIT_S * G_USEC_PER_SEC) {
        g_main_loop_quit(loop);
    }
    return G_SOURCE_CONTINUE;
}


/* ════════════════════════════════════════════════════════════
 * KEYBOARD LIGHT
 * ════════════════════════════════════════════════════════════ */

static gboolean
has_file(const char *dir, const char *file)
{
    char *p = g_build_filename(dir, file, NULL);
    gboolean ok = g_file_test(p, G_FILE_TEST_EXISTS);
    g_free(p);
    return ok;
}

static void
kbd_discover(void)
{
    kbd_type = KBD_NONE;
    g_clear_pointer(&kbd_dir, g_free);
    GDir *d = g_dir_open(LEDS_DIR, 0, NULL);
    if (!d) return;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    const char *e;
    while ((e = g_dir_read_name(d)) != NULL)
        if (strstr(e, "kbd_backlight")) g_ptr_array_add(names, g_strdup(e));
    g_dir_close(d);
    g_ptr_array_sort(names, (GCompareFunc)g_strcmp0);
    /* "asus::kbd_backlight" before "asus::kbd_backlight_1" */
    if (names->len) {
        kbd_dir = g_build_filename(LEDS_DIR, names->pdata[0], NULL);
        kbd_max = read_int(kbd_dir, "max_brightness", 0);
        if (has_file(kbd_dir, "kbd_rgb_mode"))
            kbd_type = KBD_ASUS;
        else if (has_file(kbd_dir, "color"))
            kbd_type = KBD_S76;
        else if (has_file(kbd_dir, "multi_intensity") && has_file(kbd_dir, "multi_index"))
            kbd_type = KBD_MULTI;
        else if (kbd_max > 0)
            kbd_type = KBD_MONO;
        else
            g_clear_pointer(&kbd_dir, g_free);
    }
    g_ptr_array_unref(names);
}

static void
kbd_load(void)
{
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, KBD_FILE, G_KEY_FILE_NONE, NULL)) {
        GError *err = NULL;
        int b = g_key_file_get_integer(kf, "Keyboard", "brightness", &err);
        if (!err) kbd_bright = b;
        g_clear_error(&err);
        gsize n = 0;
        int *c = g_key_file_get_integer_list(kf, "Keyboard", "color", &n, NULL);
        if (c && n == 3)
            for (int i = 0; i < 3; i++) kbd_rgb[i] = (guint8)CLAMP(c[i], 0, 255);
        g_free(c);
    }
    g_key_file_free(kf);
}

static void
kbd_save(void)
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_comment(kf, NULL, NULL,
        " TempeR keyboard light, written by temper-fand.\n"
        " brightness 0-max_brightness of the LED, color red;green;blue 0-255.", NULL);
    g_key_file_set_integer(kf, "Keyboard", "brightness", kbd_bright);
    int c[3] = { kbd_rgb[0], kbd_rgb[1], kbd_rgb[2] };
    g_key_file_set_integer_list(kf, "Keyboard", "color", c, 3);
    g_mkdir_with_parents(CONF_DIR, 0755);
    g_key_file_save_to_file(kf, KBD_FILE, NULL);
    g_key_file_free(kf);
}

static gboolean
write_str(const char *dir, const char *file, const char *v)
{
    char *p = g_build_filename(dir, file, NULL);
    FILE *f = fopen(p, "w");
    gboolean ok = f && fputs(v, f) >= 0;
    if (f && fclose(f) != 0) ok = FALSE;
    if (!ok) g_printerr("temper-fand: cannot write \"%s\" to %s\n", v, p);
    g_free(p);
    return ok;
}

/* keep: TRUE also stores the colour in the keyboard itself (ASUS), so it
 * is there at boot before anything runs */
static gboolean
kbd_apply(gboolean keep)
{
    if (kbd_type == KBD_NONE || kbd_bright < 0) return TRUE;
    gboolean ok = TRUE;
    char buf[64];
    switch (kbd_type) {
        case KBD_ASUS:
            /* cmd (0 now, 1 also saved) mode (0 static) red green blue speed */
            g_snprintf(buf, sizeof buf, "%d 0 %d %d %d 0", keep ? 1 : 0,
                       kbd_rgb[0], kbd_rgb[1], kbd_rgb[2]);
            ok &= write_str(kbd_dir, "kbd_rgb_mode", buf);
            break;
        case KBD_S76:
            g_snprintf(buf, sizeof buf, "%02X%02X%02X", kbd_rgb[0], kbd_rgb[1], kbd_rgb[2]);
            ok &= write_str(kbd_dir, "color", buf);
            break;
        case KBD_MULTI: {
            /* one value per channel, in the order of multi_index */
            char *idx = read_line(kbd_dir, "multi_index");
            char **ch = idx ? g_strsplit_set(idx, " \t", -1) : NULL;
            GString *v = g_string_new(NULL);
            for (int i = 0; ch && ch[i]; i++) {
                if (!*ch[i]) continue;
                int c = !strcmp(ch[i], "red")   ? kbd_rgb[0] :
                        !strcmp(ch[i], "green") ? kbd_rgb[1] :
                        !strcmp(ch[i], "blue")  ? kbd_rgb[2] :
                        (kbd_rgb[0] + kbd_rgb[1] + kbd_rgb[2]) / 3;
                g_string_append_printf(v, "%s%d", v->len ? " " : "",
                                       (c * kbd_max + 127) / 255);
            }
            if (v->len) ok &= write_str(kbd_dir, "multi_intensity", v->str);
            g_string_free(v, TRUE);
            g_strfreev(ch);
            g_free(idx);
            break;
        }
        default:
            break;
    }
    ok &= write_int(kbd_dir, "brightness", CLAMP(kbd_bright, 0, kbd_max));
    return ok;
}


/* ════════════════════════════════════════════════════════════
 * D-BUS
 * ════════════════════════════════════════════════════════════ */

static const char INTROSPECTION_XML[] =
    "<node>"
    " <interface name='" IFACE "'>"
    "  <method name='GetState'>"
    "   <arg type='b' name='enabled' direction='out'/>"
    /* id, label, follows, temperatures, speeds, default temps, default speeds */
    "   <arg type='a(sssayayayay)' name='fans' direction='out'/>"
    "   <arg type='as' name='paused' direction='out'/>"
    "  </method>"
    "  <method name='SetCurves'>"
    "   <arg type='a(sayay)' name='curves' direction='in'/>"
    "  </method>"
    "  <method name='SetEnabled'>"
    "   <arg type='b' name='enabled' direction='in'/>"
    "  </method>"
    "  <method name='ResetCurves'/>"
    "  <signal name='Changed'/>"
    /* found, has colour, brightness (-1 never set), max brightness, colour */
    "  <method name='GetKeyboard'>"
    "   <arg type='b' name='found' direction='out'/>"
    "   <arg type='b' name='color' direction='out'/>"
    "   <arg type='i' name='brightness' direction='out'/>"
    "   <arg type='i' name='max_brightness' direction='out'/>"
    "   <arg type='ay' name='rgb' direction='out'/>"
    "  </method>"
    "  <method name='SetKeyboard'>"
    "   <arg type='i' name='brightness' direction='in'/>"
    "   <arg type='ay' name='rgb' direction='in'/>"
    "  </method>"
    " </interface>"
    "</node>";

static GVariant *
bytes(const guint8 *b)
{
    return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, b, N_POINTS, 1);
}

static GVariant *
state_variant(void)
{
    GVariantBuilder fb, pb;
    g_variant_builder_init(&fb, G_VARIANT_TYPE("a(sssayayayay)"));
    for (guint i = 0; i < fans->len; i++) {
        Fan *f = fans->pdata[i];
        g_variant_builder_add(&fb, "(sss@ay@ay@ay@ay)", f->id, f->label, f->follows,
                              bytes(f->temp), bytes(f->pwm),
                              bytes(f->def_temp), bytes(f->def_pwm));
    }
    g_variant_builder_init(&pb, G_VARIANT_TYPE("as"));
    for (guint i = 0; i < paused_units->len; i++)
        g_variant_builder_add(&pb, "s", unit_name(paused_units->pdata[i]));
    if (asusd_curves->len)
        g_variant_builder_add(&pb, "s", "asusd fan curves");
    return g_variant_new("(ba(sssayayayay)as)", enabled, &fb, &pb);
}

static gboolean
read_curve(GVariant *ay_t, GVariant *ay_p, guint8 *temp, guint8 *pwm)
{
    gsize nt = 0, np = 0;
    const guint8 *t = g_variant_get_fixed_array(ay_t, &nt, 1);
    const guint8 *p = g_variant_get_fixed_array(ay_p, &np, 1);
    if (nt != N_POINTS || np != N_POINTS) return FALSE;
    memcpy(temp, t, N_POINTS);
    memcpy(pwm, p, N_POINTS);
    sanitize(temp, pwm);
    return TRUE;
}

static void
do_reset(void)
{
    for (guint i = 0; i < fans->len; i++) {
        Fan *f = fans->pdata[i];
        if (f->type == FAN_ASUS && enabled) {
            char en[32];
            g_snprintf(en, sizeof en, "pwm%d_enable", f->index);
            /* 3 = load the factory curve of the current profile (and
             * switch to automatic): then read it back.  Only while the
             * fans are ours, it would undo asusd's curve otherwise. */
            if (write_int(f->dir, en, 3)) {
                asus_read_points(f, f->def_temp, f->def_pwm);
                sanitize(f->def_temp, f->def_pwm);
            }
        }
        memcpy(f->temp, f->def_temp, N_POINTS);
        memcpy(f->pwm, f->def_pwm, N_POINTS);
        sanitize(f->temp, f->pwm);
        if (enabled && f->type == FAN_ASUS) asus_apply(f);
    }
    save_config();
}

/* ── polkit ── */

typedef struct {
    GDBusMethodInvocation *inv;
    char                  *method;
    GVariant              *params;
} Pending;

static void
pending_free(Pending *p)
{
    g_object_unref(p->inv);
    g_free(p->method);
    g_variant_unref(p->params);
    g_free(p);
    n_pending--;
    last_activity = g_get_monotonic_time();
}

static void
perform(Pending *p)
{
    if (!strcmp(p->method, "SetEnabled")) {
        gboolean on;
        g_variant_get(p->params, "(b)", &on);
        set_enabled(on);
    } else if (!strcmp(p->method, "SetCurves")) {
        GVariantIter *it;
        const char *id;
        GVariant *t, *pw;
        g_variant_get(p->params, "(a(sayay))", &it);
        while (g_variant_iter_next(it, "(&s@ay@ay)", &id, &t, &pw)) {
            Fan *f = find_fan(id);
            guint8 nt[N_POINTS], np[N_POINTS];
            if (f && read_curve(t, pw, nt, np)) {
                memcpy(f->temp, nt, N_POINTS);
                memcpy(f->pwm, np, N_POINTS);
                if (enabled && f->type == FAN_ASUS) asus_apply(f);
                f->lower_ticks = 3;     /* software fans: follow at once */
            }
            g_variant_unref(t);
            g_variant_unref(pw);
        }
        g_variant_iter_free(it);
        save_config();
        emit_changed();
    } else if (!strcmp(p->method, "ResetCurves")) {
        do_reset();
        emit_changed();
    } else if (!strcmp(p->method, "SetKeyboard")) {
        int b;
        GVariant *rgb;
        g_variant_get(p->params, "(i@ay)", &b, &rgb);
        gsize n = 0;
        const guint8 *c = g_variant_get_fixed_array(rgb, &n, 1);
        if (n == 3) memcpy(kbd_rgb, c, 3);
        g_variant_unref(rgb);
        kbd_bright = CLAMP(b, 0, kbd_max);
        kbd_save();
        if (!kbd_apply(TRUE)) {
            g_dbus_method_invocation_return_dbus_error(p->inv,
                "org.freedesktop.DBus.Error.Failed",
                "The keyboard did not accept the new light");
            return;
        }
    }
    g_dbus_method_invocation_return_value(p->inv, NULL);
}

static void
on_polkit(GObject *src, GAsyncResult *res, gpointer ud)
{
    Pending *p = ud;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    gboolean ok = FALSE;
    if (r) {
        g_variant_get(r, "((bba{ss}))", &ok, NULL, NULL);
        g_variant_unref(r);
    }
    if (ok) {
        perform(p);
    } else {
        g_dbus_method_invocation_return_dbus_error(p->inv,
            "org.freedesktop.DBus.Error.AccessDenied",
            err ? err->message : "Not authorized to change the fan curves");
    }
    if (err) g_error_free(err);
    pending_free(p);
}

static void
authorize_and_perform(GDBusMethodInvocation *inv, const char *method, GVariant *params)
{
    Pending *p = g_new0(Pending, 1);
    n_pending++;
    p->inv = g_object_ref(inv);
    p->method = g_strdup(method);
    p->params = g_variant_ref(params);

    /* root (temper-fand itself, scripts) needs no question */
    const char *sender = g_dbus_method_invocation_get_sender(inv);
    GVariant *u = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetConnectionUnixUser",
        g_variant_new("(s)", sender), G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    guint32 uid = G_MAXUINT32;
    if (u) {
        g_variant_get(u, "(u)", &uid);
        g_variant_unref(u);
    }
    if (uid == 0) {
        perform(p);
        pending_free(p);
        return;
    }

    GVariantBuilder subj;
    g_variant_builder_init(&subj, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&subj, "{sv}", "name", g_variant_new_string(sender));
    GVariantBuilder details;
    g_variant_builder_init(&details, G_VARIANT_TYPE("a{ss}"));
    const char *action = !strcmp(method, "SetKeyboard") ? KBD_ACTION_ID : ACTION_ID;
    guint32 flags = (g_dbus_message_get_flags(g_dbus_method_invocation_get_message(inv)) &
                     G_DBUS_MESSAGE_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION) ? 1 : 0;
    g_dbus_connection_call(bus, "org.freedesktop.PolicyKit1",
        "/org/freedesktop/PolicyKit1/Authority", "org.freedesktop.PolicyKit1.Authority",
        "CheckAuthorization",
        g_variant_new("((sa{sv})sa{ss}us)", "system-bus-name", &subj, action,
                      &details, flags, ""),
        G_VARIANT_TYPE("((bba{ss}))"), G_DBUS_CALL_FLAGS_NONE,
        G_MAXINT, NULL, on_polkit, p);
}

static void
method_call(GDBusConnection *c, const char *sender, const char *path,
            const char *iface, const char *method, GVariant *params,
            GDBusMethodInvocation *inv, gpointer ud)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)ud;
    last_activity = g_get_monotonic_time();
    if (!strcmp(method, "GetState"))
        g_dbus_method_invocation_return_value(inv, state_variant());
    else if (!strcmp(method, "GetKeyboard"))
        g_dbus_method_invocation_return_value(inv, g_variant_new("(bbii@ay)",
            kbd_type != KBD_NONE, kbd_type >= KBD_ASUS, kbd_bright, kbd_max,
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, kbd_rgb, 3, 1)));
    else
        authorize_and_perform(inv, method, params);
}

static const GDBusInterfaceVTable vtable = { method_call, NULL, NULL, { 0 } };

static void
on_bus_acquired(GDBusConnection *c, const char *name, gpointer ud)
{
    (void)name; (void)ud;
    bus = c;
    GError *err = NULL;
    if (!g_dbus_connection_register_object(c, OBJ_PATH, introspection->interfaces[0],
                                           &vtable, NULL, NULL, &err)) {
        g_printerr("temper-fand: %s\n", err->message);
        g_error_free(err);
        g_main_loop_quit(loop);
    }
}

static void
on_name_lost(GDBusConnection *c, const char *name, gpointer ud)
{
    (void)c; (void)ud;
    g_printerr("temper-fand: cannot own %s on the system bus (already running?)\n", name);
    g_main_loop_quit(loop);
}

static gboolean
on_signal(gpointer ud)
{
    (void)ud;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}


/* ════════════════════════════════════════════════════════════
 * MAIN
 * ════════════════════════════════════════════════════════════ */

int
main(int argc, char **argv)
{
    if (geteuid() != 0) {
        g_printerr("temper-fand must run as root (it is started by systemd).\n");
        return 1;
    }

    paused_units = g_ptr_array_new_with_free_func(g_free);
    masked_units = g_ptr_array_new_with_free_func(g_free);
    asusd_curves = g_ptr_array_new_with_free_func(g_free);
    discover();
    load_state();
    load_config();
    kbd_discover();
    kbd_load();

    /* --release: give everything back and switch the curves off (package
     * removal); --list: show what was found */
    if (argc > 1 && !strcmp(argv[1], "--release")) {
        if (enabled) release_fans();
        others_give_back();
        if (enabled) {
            enabled = FALSE;
            save_config();
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--list")) {
        g_print("curves %s\n", enabled ? "on" : "off");
        for (guint i = 0; i < fans->len; i++) {
            Fan *f = fans->pdata[i];
            g_print("%-12s %-12s %s, %s:", f->id, f->label,
                    f->type == FAN_ASUS ? "firmware curve" : "software curve", f->follows);
            for (int k = 0; k < N_POINTS; k++)
                g_print(" %d°C=%d%%", f->temp[k], (f->pwm[k] * 100 + 127) / 255);
            g_print("\n");
        }
        static const char *kinds[] = { "none", "brightness only", "ASUS colour",
                                       "System76 colour", "multicolour LED" };
        g_print("keyboard light: %s%s%s\n", kinds[kbd_type],
                kbd_dir ? ", " : "", kbd_dir ? kbd_dir : "");
        return 0;
    }

    introspection = g_dbus_node_info_new_for_xml(INTROSPECTION_XML, NULL);
    loop = g_main_loop_new(NULL, FALSE);
    last_activity = g_get_monotonic_time();

    if (enabled) {
        ensure_boot_start();
        take_control();
    }
    /* the light chosen in TempeR, also on keyboards that forget it at boot */
    kbd_apply(FALSE);

    guint owner = g_bus_own_name(G_BUS_TYPE_SYSTEM, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE,
                                 on_bus_acquired, NULL, on_name_lost, NULL, NULL);
    g_timeout_add_seconds(1, on_tick, NULL);
    g_unix_signal_add(SIGTERM, on_signal, NULL);
    g_unix_signal_add(SIGINT, on_signal, NULL);

    g_main_loop_run(loop);

    /* stopped (shutdown, systemctl stop): the firmware takes over again;
     * what was paused stays paused, it is the user's choice to keep the
     * curves, and they come back at the next start.  With the curves off
     * the fans were never ours: leave them alone. */
    if (enabled) release_fans();
    g_bus_unown_name(owner);
    g_dbus_node_info_unref(introspection);
    g_main_loop_unref(loop);
    return 0;
}
