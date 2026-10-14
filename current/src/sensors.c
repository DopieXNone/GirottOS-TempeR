/* sensors.c – finds and reads every temperature (and fan) sensor.
 *
 * Sources, all available on any Arch kernel without extra packages:
 *
 *   /sys/class/hwmon/hwmonN     CPU (coretemp, k10temp, zenpower…), GPU
 *                               (amdgpu, radeon, nouveau, i915, xe), NVMe,
 *                               SATA (drivetemp), RAM (spd5118, jc42),
 *                               motherboard / laptop EC chips, fans
 *   /sys/class/thermal/…        firmware thermal zones that have no hwmon
 *   nvidia-smi                  NVIDIA proprietary driver, which exports no
 *                               hwmon.  Optional: only used when present,
 *                               and never while the GPU is asleep, so a
 *                               laptop's dGPU is not woken up just to be
 *                               measured.
 *
 * Nothing here needs root.
 */

#include "tempfm.h"

#include <stdlib.h>

#define HWMON_DIR    "/sys/class/hwmon"
#define THERMAL_DIR  "/sys/class/thermal"
#define NVIDIA_DIR   "/sys/bus/pci/drivers/nvidia"
#define PCI_DIR      "/sys/bus/pci/devices"


/* ── small file helpers ── */

static char *
read_text(const char *path)
{
    char *txt = NULL;
    if (!g_file_get_contents(path, &txt, NULL, NULL)) return NULL;
    g_strstrip(txt);
    return txt;
}

static char *
read_text2(const char *dir, const char *file)
{
    char *p = g_build_filename(dir, file, NULL);
    char *t = read_text(p);
    g_free(p);
    return t;
}

static gboolean
read_number(const char *path, double *out)
{
    char *t = read_text(path);
    if (!t) return FALSE;
    char *end = NULL;
    double v = g_ascii_strtod(t, &end);
    gboolean ok = end && end != t;
    g_free(t);
    if (ok) *out = v;
    return ok;
}

/* "<dir>/temp3_max" → value in °C, or 0 */
static double
read_milli(const char *dir, const char *prefix, int idx, const char *suffix)
{
    char name[64];
    g_snprintf(name, sizeof name, "%s%d_%s", prefix, idx, suffix);
    char *p = g_build_filename(dir, name, NULL);
    double v = 0;
    if (!read_number(p, &v)) v = 0;
    g_free(p);
    return v / 1000.0;
}

/* hwmon10 must come after hwmon9 */
static int
cmp_numbered(gconstpointer a, gconstpointer b)
{
    const char *sa = *(const char * const *)a, *sb = *(const char * const *)b;
    const char *da = sa, *db = sb;
    while (*da && !g_ascii_isdigit(*da)) da++;
    while (*db && !g_ascii_isdigit(*db)) db++;
    long na = strtol(da, NULL, 10), nb = strtol(db, NULL, 10);
    if (na != nb) return na < nb ? -1 : 1;
    return strcmp(sa, sb);
}

static GPtrArray *
list_dir(const char *path, const char *prefix)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GDir *d = g_dir_open(path, 0, NULL);
    if (!d) return out;
    const char *n;
    while ((n = g_dir_read_name(d)) != NULL)
        if (!prefix || g_str_has_prefix(n, prefix))
            g_ptr_array_add(out, g_strdup(n));
    g_dir_close(d);
    g_ptr_array_sort(out, cmp_numbered);
    return out;
}

/* Indexes N for which "<prefix>N_input" exists, sorted */
static GArray *
input_indexes(const char *dir, const char *prefix)
{
    GArray *idx = g_array_new(FALSE, FALSE, sizeof(int));
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return idx;
    size_t pl = strlen(prefix);
    const char *n;
    while ((n = g_dir_read_name(d)) != NULL) {
        if (strncmp(n, prefix, pl) || !g_ascii_isdigit(n[pl])) continue;
        char *end = NULL;
        long v = strtol(n + pl, &end, 10);
        if (end && !strcmp(end, "_input")) {
            int i = (int)v;
            g_array_append_val(idx, i);
        }
    }
    g_dir_close(d);
    for (guint a = 1; a < idx->len; a++)                 /* tiny: insertion sort */
        for (guint b = a; b > 0 &&
             g_array_index(idx, int, b - 1) > g_array_index(idx, int, b); b--) {
            int t = g_array_index(idx, int, b);
            g_array_index(idx, int, b) = g_array_index(idx, int, b - 1);
            g_array_index(idx, int, b - 1) = t;
        }
    return idx;
}

static char *
collapse_spaces(char *s)
{
    if (!s) return s;
    char *w = s;
    gboolean sp = FALSE;
    for (char *r = s; *r; r++) {
        if (g_ascii_isspace(*r)) { sp = TRUE; continue; }
        if (sp && w != s) *w++ = ' ';
        sp = FALSE;
        *w++ = *r;
    }
    *w = '\0';
    return s;
}


/* ── categories, levels ── */

const char *
tempfm_category_name(SensorCategory c)
{
    switch (c) {
        case CAT_CPU:     return "Processor";
        case CAT_GPU:     return "Graphics card";
        case CAT_STORAGE: return "Storage";
        case CAT_MEMORY:  return "Memory";
        case CAT_BATTERY: return "Battery";
        case CAT_SYSTEM:  return "System";
        case CAT_FANS:    return "Fans";
        default:          return "Other";
    }
}

const char *
tempfm_category_icon(SensorCategory c)
{
    switch (c) {
        case CAT_CPU:     return "\xF0\x9F\x96\xA5";          /* 🖥 */
        case CAT_GPU:     return "\xF0\x9F\x8E\xAE";          /* 🎮 */
        case CAT_STORAGE: return "\xF0\x9F\x92\xBE";          /* 💾 */
        case CAT_MEMORY:  return "\xF0\x9F\xA7\xA9";          /* 🧩 */
        case CAT_BATTERY: return "\xF0\x9F\x94\x8B";          /* 🔋 */
        case CAT_SYSTEM:  return "\xF0\x9F\x8C\xA1";          /* 🌡 */
        case CAT_FANS:    return "\xF0\x9F\x8C\x80";          /* 🌀 */
        default:          return "\xF0\x9F\x8C\xA1";
    }
}

/* Tela names (colourful scalable icons), see icons.c */
const char *
tempfm_category_icon_name(SensorCategory c)
{
    switch (c) {
        case CAT_CPU:     return "cpu";
        case CAT_GPU:     return "hardinfo";                    /* graphics card */
        case CAT_STORAGE: return "drive-harddisk-solidstate";
        case CAT_MEMORY:  return "media-memory";
        case CAT_BATTERY: return "battery";
        case CAT_SYSTEM:  return "preferences-devices-cpu";     /* motherboard   */
        case CAT_FANS:    return "resources";                   /* speed gauge   */
        default:          return "psensor";
    }
}

/* Used only when the sensor itself does not say where its limit is */
static double
default_crit(SensorCategory c)
{
    switch (c) {
        case CAT_CPU:     return 100;
        case CAT_GPU:     return 95;
        case CAT_STORAGE: return 80;
        case CAT_MEMORY:  return 85;
        case CAT_BATTERY: return 60;
        default:          return 90;
    }
}

static void
set_limits(Sensor *s, SensorCategory c, double max, double crit)
{
    /* RAM "max" is an alarm threshold (often 55 °C), not where it gets hot */
    if (c == CAT_MEMORY) max = 0;

    /* Many chips report 127 °C / 255 °C / 65261 °C as "no limit" */
    s->own_limits = crit > 30 && crit < 125;
    if (!s->own_limits) crit = default_crit(c);
    s->crit = crit;
    s->high = (max > 30 && max < 125 && max < crit - 2) ? max : crit - 15;
}

/* Secondary sensors without limits of their own (NVMe "Sensor 1/2")
 * follow the main sensor of the same chip. */
static void
inherit_limits(Device *d)
{
    Sensor *p = NULL;
    for (guint i = 0; i < d->sensors->len && !p; i++) {
        Sensor *s = d->sensors->pdata[i];
        if (s->primary && s->own_limits) p = s;
    }
    if (!p) return;
    for (guint i = 0; i < d->sensors->len; i++) {
        Sensor *s = d->sensors->pdata[i];
        if (s->kind != KIND_TEMP || s->own_limits) continue;
        s->high = p->high;
        s->crit = p->crit;
    }
}

TempLevel
tempfm_sensor_level(const Sensor *s)
{
    if (!s || s->kind != KIND_TEMP || !s->valid) return LEVEL_UNKNOWN;
    if (s->value >= s->crit)        return LEVEL_CRITICAL;
    if (s->value >= s->high)        return LEVEL_HOT;
    if (s->value >= s->high - 15)   return LEVEL_WARM;
    return LEVEL_NORMAL;
}

const char *
tempfm_level_name(TempLevel l)
{
    switch (l) {
        case LEVEL_NORMAL:   return "Normal";
        case LEVEL_WARM:     return "Warm";
        case LEVEL_HOT:      return "Hot";
        case LEVEL_CRITICAL: return "Critical";
        default:             return "Unavailable";
    }
}

const char *
tempfm_level_class(TempLevel l)
{
    switch (l) {
        case LEVEL_NORMAL:   return "lvl-normal";
        case LEVEL_WARM:     return "lvl-warm";
        case LEVEL_HOT:      return "lvl-hot";
        case LEVEL_CRITICAL: return "lvl-crit";
        default:             return "lvl-none";
    }
}


/* ── objects ── */

static void
sensor_free(gpointer p)
{
    Sensor *s = p;
    g_free(s->label);
    g_free(s->input);
    g_free(s);
}

static void
device_free(gpointer p)
{
    Device *d = p;
    g_free(d->name);
    g_free(d->source);
    g_free(d->nv_bus);
    g_free(d->key);
    g_free(d->pci_dir);
    g_ptr_array_unref(d->sensors);
    g_free(d);
}

static Device *
device_new(SensorCategory c, const char *name, const char *source)
{
    Device *d = g_new0(Device, 1);
    d->category = c;
    d->name     = g_strdup(name);
    d->source   = g_strdup(source);
    d->sensors  = g_ptr_array_new_with_free_func(sensor_free);
    d->gpu_usage = -1;
    return d;
}

static Sensor *
sensor_add(Device *d, const char *label, const char *input, SensorKind kind)
{
    Sensor *s = g_new0(Sensor, 1);
    s->label = g_strdup(label);
    s->input = g_strdup(input);
    s->kind  = kind;
    s->peak  = -1000;
    g_ptr_array_add(d->sensors, s);
    return s;
}

Sensor *
tempfm_device_main_sensor(Device *d)
{
    Sensor *first = NULL, *hottest = NULL;
    for (guint i = 0; i < d->sensors->len; i++) {
        Sensor *s = d->sensors->pdata[i];
        if (s->kind != KIND_TEMP) continue;
        if (s->primary) return s;
        if (!first) first = s;
        if (s->valid && (!hottest || s->value > hottest->value)) hottest = s;
    }
    return hottest ? hottest : first;
}


/* ── friendly names ── */

char *
tempfm_cpu_model(void)
{
    char *txt = NULL;
    char *res = NULL;
    if (g_file_get_contents("/proc/cpuinfo", &txt, NULL, NULL)) {
        static const char *keys[] = { "model name", "Hardware", "Model", "cpu model", NULL };
        char **lines = g_strsplit(txt, "\n", -1);
        for (int k = 0; keys[k] && !res; k++)
            for (int i = 0; lines[i] && !res; i++) {
                if (!g_str_has_prefix(lines[i], keys[k])) continue;
                char *colon = strchr(lines[i], ':');
                if (!colon) continue;
                char *v = g_strstrip(g_strdup(colon + 1));
                if (*v) res = collapse_spaces(v);
                else    g_free(v);
            }
        g_strfreev(lines);
        g_free(txt);
    }
    return res ? res : g_strdup("Processor");
}

static gboolean
name_in(const char *name, const char * const *list)
{
    for (int i = 0; list[i]; i++)
        if (!strcmp(name, list[i])) return TRUE;
    return FALSE;
}

static gboolean
name_starts(const char *name, const char * const *list)
{
    for (int i = 0; list[i]; i++)
        if (g_str_has_prefix(name, list[i])) return TRUE;
    return FALSE;
}

static const char *CPU_DRIVERS[] = {
    "coretemp", "k10temp", "k8temp", "zenpower", "cpu_thermal",
    "via_cputemp", "x86_pkg_temp", "soc_thermal", NULL
};
static const char *STORAGE_DRIVERS[] = { "nvme", "drivetemp", NULL };
static const char *MEMORY_DRIVERS[]  = { "spd5118", "jc42", "ee1004", NULL };
static const char *WIFI_PREFIX[]     = { "iwlwifi", "mt76", "mt79", "ath", "rtw", "brcm", NULL };

/* "0000:01:00.0" → "gpu-0000-01-00-0", usable as an INI key */
char *
tempfm_gpu_key(const char *bus)
{
    char *k = g_strdup_printf("gpu-%s", bus);
    g_strdelimit(k, ":.", '-');
    return k;
}

static const char *
vendor_short(const char *vid)
{
    if (!g_ascii_strcasecmp(vid, "1002")) return "AMD";
    if (!g_ascii_strcasecmp(vid, "10de")) return "NVIDIA";
    if (!g_ascii_strcasecmp(vid, "8086")) return "Intel";
    return NULL;
}

/* Marketing name of a PCI device from the pci.ids database (hwdata):
 * "GB206M [GeForce RTX 5070 Max-Q / Mobile]" → "NVIDIA GeForce RTX 5070
 * Max-Q / Mobile".  Without pci.ids: "NVIDIA graphics card". */
char *
tempfm_pci_name(const char *pci_dir)
{
    char *v = pci_dir ? read_text2(pci_dir, "vendor") : NULL;
    char *d = pci_dir ? read_text2(pci_dir, "device") : NULL;
    const char *vid = v && g_str_has_prefix(v, "0x") ? v + 2 : v;
    const char *did = d && g_str_has_prefix(d, "0x") ? d + 2 : d;
    char *vendor = vid && vendor_short(vid) ? g_strdup(vendor_short(vid)) : NULL;
    char *model = NULL;

    static const char *dbs[] = { "/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", NULL };
    char *txt = NULL;
    for (int i = 0; dbs[i] && !txt && vid && did; i++)
        g_file_get_contents(dbs[i], &txt, NULL, NULL);

    if (txt) {
        gboolean in_vendor = FALSE;
        for (char *line = txt; line && *line; ) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (line[0] != '#' && line[0] != '\t' && line[0]) {
                if (in_vendor) break;                /* past our vendor */
                in_vendor = !g_ascii_strncasecmp(line, vid, 4) && line[4] == ' ';
                if (in_vendor && !vendor) {
                    char *vn = g_strstrip(g_strdup(line + 4));
                    char *sp = strchr(vn, ' ');      /* first word only */
                    if (sp) *sp = '\0';
                    vendor = vn;
                }
            } else if (in_vendor && line[0] == '\t' && line[1] != '\t' &&
                       !g_ascii_strncasecmp(line + 1, did, 4)) {
                char *nm = g_strstrip(g_strdup(line + 5));
                char *lb = strchr(nm, '['), *rb = lb ? strrchr(lb, ']') : NULL;
                if (lb && rb) {
                    *rb = '\0';
                    model = g_strdup(lb + 1);
                    g_free(nm);
                } else {
                    model = nm;
                }
                break;
            }
            line = nl ? nl + 1 : NULL;
        }
        g_free(txt);
    }

    char *res;
    if (model && vendor && !g_str_has_prefix(model, vendor))
        res = g_strdup_printf("%s %s", vendor, model);
    else if (model)
        res = g_strdup(model);
    else
        res = g_strdup_printf("%s graphics card", vendor ? vendor : "Unknown");
    g_free(model);
    g_free(vendor);
    g_free(v);
    g_free(d);
    return res;
}

/* The name to show: the one the user typed in the specs window, if any */
const char *
tempfm_device_name(Device *d)
{
    if (d->key) {
        const char *o = g_hash_table_lookup(tempfm_settings()->spec_names, d->key);
        if (o && *o) return o;
    }
    return d->name;
}

/* "cpu_fan" → "CPU fan", "Package id 0" → "Package", "Tccd1" → "Chiplet 1" */
static char *
friendly_label(const char *raw, int idx, int count, SensorKind kind)
{
    if (!raw || !*raw) {
        if (kind == KIND_FAN) return count == 1 ? g_strdup("Fan") : g_strdup_printf("Fan %d", idx);
        return count == 1 ? g_strdup("Temperature") : g_strdup_printf("Sensor %d", idx);
    }
    int n;
    if (sscanf(raw, "Package id %d", &n) == 1)
        return n == 0 ? g_strdup("Package (whole CPU)") : g_strdup_printf("Package %d", n);
    if (!strcmp(raw, "Tctl"))      return g_strdup("CPU (Tctl)");
    if (!strcmp(raw, "Tdie"))      return g_strdup("CPU die (Tdie)");
    if (sscanf(raw, "Tccd%d", &n) == 1) return g_strdup_printf("Chiplet %d (CCD)", n);
    if (!strcmp(raw, "edge"))      return g_strdup("GPU (edge)");
    if (!strcmp(raw, "junction"))  return g_strdup("Hotspot (junction)");
    if (!strcmp(raw, "mem"))       return g_strdup("Video memory");
    if (!strcmp(raw, "Composite")) return g_strdup("Drive (overall)");

    /* ACPI firmware thermal zone names */
    static const struct { const char *id, *nice; } zones[] = {
        { "TCPU",     "CPU area" },        { "TCPU_PCI", "CPU area (PCI)" },
        { "TGPU",     "GPU area" },        { "TVGA",     "GPU area" },
        { "TMEM",     "Memory area" },     { "TPCH",     "Chipset" },
        { "TSKN",     "Case surface" },    { "SKIN",     "Case surface" },
        { "TAMB",     "Ambient (inside)" },{ "TCHG",     "Charger" },
        { "CHRG",     "Charger" },         { "TBAT",     "Battery area" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(zones); i++)
        if (!strcmp(raw, zones[i].id)) return g_strdup(zones[i].nice);
    if (sscanf(raw, "SEN%d", &n) == 1) return g_strdup_printf("Board sensor %d", n);

    char *s = g_strdup(raw);
    g_strdelimit(s, "_", ' ');
    GString *g = g_string_new(NULL);
    char **w = g_strsplit(s, " ", -1);
    for (int i = 0; w[i]; i++) {
        if (!*w[i]) continue;
        if (g->len) g_string_append_c(g, ' ');
        if (!g_ascii_strcasecmp(w[i], "cpu") || !g_ascii_strcasecmp(w[i], "gpu") ||
            !g_ascii_strcasecmp(w[i], "pch") || !g_ascii_strcasecmp(w[i], "vrm") ||
            !g_ascii_strcasecmp(w[i], "soc")) {
            char *up = g_ascii_strup(w[i], -1);
            g_string_append(g, up);
            g_free(up);
        } else if (g->len == 0) {
            g_string_append_c(g, g_ascii_toupper(w[i][0]));
            g_string_append(g, w[i] + 1);
        } else {
            g_string_append(g, w[i]);
        }
    }
    g_strfreev(w);
    g_free(s);
    return g_string_free(g, FALSE);
}

static gboolean
is_primary_label(const char *raw)
{
    static const char *p[] = { "Package id 0", "Tctl", "Tdie", "edge", "Composite", "CPU", NULL };
    return raw && name_in(raw, p);
}


/* ── hwmon ── */

static Device *
find_device(GPtrArray *devs, SensorCategory c, const char *name)
{
    for (guint i = 0; i < devs->len; i++) {
        Device *d = devs->pdata[i];
        if (d->category == c && !g_strcmp0(d->name, name)) return d;
    }
    return NULL;
}

static void
scan_hwmon(GPtrArray *devs, const char *dir, int *mem_count, gboolean *has_cpu)
{
    char *drv = read_text2(dir, "name");
    if (!drv) return;

    char *devlink = g_build_filename(dir, "device", NULL);
    char *devpath = realpath(devlink, NULL);
    g_free(devlink);

    char *pci_class = devpath ? read_text2(devpath, "class") : NULL;
    char *ps_type   = devpath ? read_text2(devpath, "type")  : NULL;

    SensorCategory cat;
    char *name;

    if (name_in(drv, CPU_DRIVERS)) {
        cat = CAT_CPU;
        name = tempfm_cpu_model();
        *has_cpu = TRUE;
    } else if (pci_class && g_str_has_prefix(pci_class, "0x03")) {
        cat = CAT_GPU;
        name = tempfm_pci_name(devpath);
    } else if (name_in(drv, STORAGE_DRIVERS)) {
        cat = CAT_STORAGE;
        char *model  = devpath ? read_text2(devpath, "model")  : NULL;
        char *vendor = devpath ? read_text2(devpath, "vendor") : NULL;
        if (model && *model) {
            if (vendor && *vendor && g_ascii_strcasecmp(vendor, "ATA") &&
                !g_str_has_prefix(vendor, "0x"))
                name = g_strdup_printf("%s %s", vendor, model);
            else
                name = g_strdup(model);
            collapse_spaces(name);
        } else {
            name = g_strdup(!strcmp(drv, "nvme") ? "NVMe drive" : "Disk drive");
        }
        g_free(model);
        g_free(vendor);
    } else if (name_in(drv, MEMORY_DRIVERS)) {
        cat = CAT_MEMORY;
        name = g_strdup_printf("Memory module %d", ++*mem_count);
    } else if (g_str_has_prefix(drv, "BAT") ||
               (ps_type && !strcmp(ps_type, "Battery"))) {
        cat = CAT_BATTERY;
        name = g_strdup_printf("Battery (%s)", drv);
    } else {
        cat = CAT_SYSTEM;
        if (!strcmp(drv, "acpitz"))           name = g_strdup("ACPI thermal zone");
        else if (name_starts(drv, WIFI_PREFIX)) name = g_strdup("Wi-Fi adapter");
        else if (!strcmp(drv, "pch_cannonlake") || g_str_has_prefix(drv, "pch_"))
                                              name = g_strdup("Chipset (PCH)");
        else                                  name = g_strdup("Motherboard");
    }

    /* temperatures */
    GArray *ti = input_indexes(dir, "temp");
    Device *d = NULL;
    for (guint k = 0; k < ti->len; k++) {
        int i = g_array_index(ti, int, k);
        char f[64];

        g_snprintf(f, sizeof f, "temp%d_enable", i);
        char *en = read_text2(dir, f);
        gboolean off = en && !strcmp(en, "0");
        g_free(en);
        if (off) continue;

        g_snprintf(f, sizeof f, "temp%d_input", i);
        char *input = g_build_filename(dir, f, NULL);
        double v;
        /* A sensor that answers with nonsense is dropped; one that does
         * not answer at all (asleep GPU) is kept and shown as "—". */
        if (read_number(input, &v) && (v < -40000 || v > 200000)) {
            g_free(input);
            continue;
        }

        g_snprintf(f, sizeof f, "temp%d_label", i);
        char *raw = read_text2(dir, f);

        if (!d) {
            d = find_device(devs, cat, name);
            if (!d || cat == CAT_STORAGE || cat == CAT_GPU) {
                d = device_new(cat, name, drv);
                if (cat == CAT_CPU) d->key = g_strdup("cpu");
                if (cat == CAT_GPU && devpath) {
                    char *bus = g_path_get_basename(devpath);
                    d->key = tempfm_gpu_key(bus);
                    d->pci_dir = g_strdup(devpath);
                    g_free(bus);
                }
                g_ptr_array_add(devs, d);
            }
        }
        char *lbl = friendly_label(raw, i, (int)ti->len, KIND_TEMP);
        Sensor *s = sensor_add(d, lbl, input, KIND_TEMP);
        s->primary = is_primary_label(raw);
        set_limits(s, cat, read_milli(dir, "temp", i, "max"),
                           read_milli(dir, "temp", i, "crit"));
        g_free(lbl);
        g_free(raw);
        g_free(input);
    }
    g_array_unref(ti);
    if (d) inherit_limits(d);

    /* fans */
    GArray *fi = input_indexes(dir, "fan");
    Device *fd = NULL;
    for (guint k = 0; k < fi->len; k++) {
        int i = g_array_index(fi, int, k);
        char f[64];
        g_snprintf(f, sizeof f, "fan%d_input", i);
        char *input = g_build_filename(dir, f, NULL);
        double v;
        if (!read_number(input, &v) || v < 0 || v > 100000) {
            g_free(input);
            continue;
        }
        g_snprintf(f, sizeof f, "fan%d_label", i);
        char *raw = read_text2(dir, f);

        if (!fd) {
            const char *fname = cat == CAT_SYSTEM && !strcmp(drv, "acpi_fan")
                ? "ACPI fan" : (cat == CAT_SYSTEM ? "System fans" : name);
            fd = device_new(CAT_FANS, fname, drv);
            g_ptr_array_add(devs, fd);
        }
        char *lbl = friendly_label(raw, i, (int)fi->len, KIND_FAN);
        sensor_add(fd, lbl, input, KIND_FAN);
        g_free(lbl);
        g_free(raw);
        g_free(input);
    }
    g_array_unref(fi);

    g_free(name);
    g_free(pci_class);
    g_free(ps_type);
    free(devpath);
    g_free(drv);
}


/* ── thermal zones without a hwmon twin ── */

static void
scan_thermal(GPtrArray *devs, gboolean has_cpu)
{
    GPtrArray *zones = list_dir(THERMAL_DIR, "thermal_zone");
    Device *platform = NULL;

    for (guint z = 0; z < zones->len; z++) {
        char *dir = g_build_filename(THERMAL_DIR, zones->pdata[z], NULL);

        /* already listed through hwmon? */
        GPtrArray *hw = list_dir(dir, "hwmon");
        gboolean twin = hw->len > 0;
        g_ptr_array_unref(hw);

        char *type = read_text2(dir, "type");
        char *input = g_build_filename(dir, "temp", NULL);
        double v;
        gboolean skip = twin || !type ||
            g_str_has_prefix(type, "INT3400") ||           /* not a real sensor */
            (has_cpu && !strcmp(type, "x86_pkg_temp")) ||  /* = coretemp package */
            !read_number(input, &v) || v <= 0 || v > 200000;

        if (!skip) {
            /* trip points: "critical" and "hot"/"passive" */
            double crit = 0, high = 0;
            for (int t = 0; t < 16; t++) {
                char f[64];
                g_snprintf(f, sizeof f, "trip_point_%d_type", t);
                char *tt = read_text2(dir, f);
                if (!tt) break;
                g_snprintf(f, sizeof f, "trip_point_%d_temp", t);
                char *p = g_build_filename(dir, f, NULL);
                double tv = 0;
                if (read_number(p, &tv) && tv > 0) {
                    tv /= 1000.0;
                    if (!strcmp(tt, "critical") && (crit == 0 || tv < crit)) crit = tv;
                    else if ((!strcmp(tt, "hot") || !strcmp(tt, "passive")) &&
                             (high == 0 || tv < high)) high = tv;
                }
                g_free(p);
                g_free(tt);
            }

            Device *d;
            Sensor *s;
            if (!strcmp(type, "x86_pkg_temp") || name_in(type, CPU_DRIVERS)) {
                char *cpu = tempfm_cpu_model();
                d = find_device(devs, CAT_CPU, cpu);
                if (!d) {
                    d = device_new(CAT_CPU, cpu, type);
                    d->key = g_strdup("cpu");
                    g_ptr_array_add(devs, d);
                }
                g_free(cpu);
                s = sensor_add(d, "Package (whole CPU)", input, KIND_TEMP);
                s->primary = TRUE;
                set_limits(s, CAT_CPU, high, crit);
            } else {
                if (!platform) {
                    platform = device_new(CAT_SYSTEM, "Firmware thermal zones", "thermal");
                    g_ptr_array_add(devs, platform);
                }
                char *lbl = friendly_label(type, 0, 2, KIND_TEMP);
                s = sensor_add(platform, lbl, input, KIND_TEMP);
                set_limits(s, CAT_SYSTEM, high, crit);
                g_free(lbl);
            }
        }
        g_free(input);
        g_free(type);
        g_free(dir);
    }
    g_ptr_array_unref(zones);
}


/* ── NVIDIA proprietary driver ── */

static void
scan_nvidia(GPtrArray *devs)
{
    if (!g_file_test(NVIDIA_DIR, G_FILE_TEST_IS_DIR)) return;
    char *smi = g_find_program_in_path("nvidia-smi");
    if (!smi) return;
    g_free(smi);

    GPtrArray *ents = list_dir(NVIDIA_DIR, NULL);
    for (guint i = 0; i < ents->len; i++) {
        const char *bus = ents->pdata[i];
        if (!strchr(bus, ':') || !strchr(bus, '.')) continue;   /* only PCI ids */

        char *dev = g_build_filename(PCI_DIR, bus, NULL);
        char *cls = read_text2(dev, "class");
        gboolean gpu = cls && g_str_has_prefix(cls, "0x03");
        g_free(cls);
        if (!gpu) {
            g_free(dev);
            continue;
        }

        char *name = tempfm_pci_name(dev);
        Device *d = device_new(CAT_GPU, name, "nvidia");
        g_free(name);
        d->nv_bus  = g_strdup(bus);
        d->key     = tempfm_gpu_key(bus);
        d->pci_dir = dev;
        Sensor *s = sensor_add(d, "GPU core", NULL, KIND_TEMP);
        s->primary = TRUE;
        set_limits(s, CAT_GPU, 0, 0);
        g_ptr_array_add(devs, d);
    }
    g_ptr_array_unref(ents);
}

static int
cmp_device(gconstpointer a, gconstpointer b)
{
    const Device *da = *(Device * const *)a, *db = *(Device * const *)b;
    return (int)da->category - (int)db->category;
}

GPtrArray *
tempfm_sensors_discover(void)
{
    GPtrArray *devs = g_ptr_array_new_with_free_func(device_free);
    int mem_count = 0;
    gboolean has_cpu = FALSE;

    GPtrArray *hw = list_dir(HWMON_DIR, "hwmon");
    for (guint i = 0; i < hw->len; i++) {
        char *dir = g_build_filename(HWMON_DIR, hw->pdata[i], NULL);
        scan_hwmon(devs, dir, &mem_count, &has_cpu);
        g_free(dir);
    }
    g_ptr_array_unref(hw);

    scan_thermal(devs, has_cpu);
    scan_nvidia(devs);

    g_ptr_array_sort(devs, cmp_device);     /* stable: keeps discovery order */
    tempfm_sensors_read(devs);
    return devs;
}


/* ── reading ── */

static void
sensor_store(Sensor *s, gboolean ok, double v)
{
    s->valid = ok;
    if (!ok) return;
    s->value = v;
    if (v > s->peak) s->peak = v;
}

/* A graphics card in runtime power saving ("suspended") is not read at
 * all: on some drivers reading its sensors would wake it up and drain
 * the battery of a laptop just to show a number. */
static gboolean
gpu_asleep(const char *pci_dir)
{
    char *p = g_build_filename(pci_dir, "power", "runtime_status", NULL);
    char *st = read_text(p);
    gboolean r = st && !strcmp(st, "suspended");
    g_free(st);
    g_free(p);
    return r;
}

/* amdgpu (and radeon) publish usage and VRAM in sysfs; i915/xe/nouveau
 * do not, so those stay "unknown".  NVIDIA comes from nvidia-smi. */
static void
read_gpu_usage(Device *d)
{
    if (d->category != CAT_GPU || !d->pci_dir || d->nv_bus) return;
    char *p = g_build_filename(d->pci_dir, "gpu_busy_percent", NULL);
    double v;
    d->gpu_usage = read_number(p, &v) ? v : -1;
    g_free(p);
    double used = 0, total = 0;
    p = g_build_filename(d->pci_dir, "mem_info_vram_used", NULL);
    gboolean ok = read_number(p, &used);
    g_free(p);
    p = g_build_filename(d->pci_dir, "mem_info_vram_total", NULL);
    ok = ok && read_number(p, &total);
    g_free(p);
    d->vram_used  = ok ? used  / 1048576.0 : 0;
    d->vram_total = ok ? total / 1048576.0 : 0;
}

void
tempfm_sensors_read(GPtrArray *devices)
{
    for (guint i = 0; i < devices->len; i++) {
        Device *d = devices->pdata[i];
        if (d->category == CAT_GPU && d->pci_dir && !d->nv_bus) {
            d->asleep = gpu_asleep(d->pci_dir);
            if (d->asleep) {
                for (guint k = 0; k < d->sensors->len; k++)
                    ((Sensor *)d->sensors->pdata[k])->valid = FALSE;
                d->gpu_usage = 0;                   /* powered off = idle */
                d->vram_used = 0;
                continue;
            }
        }
        read_gpu_usage(d);
        for (guint k = 0; k < d->sensors->len; k++) {
            Sensor *s = d->sensors->pdata[k];
            if (!s->input) continue;                     /* nvidia-smi */
            double v = 0;
            gboolean ok = read_number(s->input, &v);
            if (ok && s->kind == KIND_TEMP) v /= 1000.0;
            sensor_store(s, ok, v);
        }
    }
}

gboolean
tempfm_sensors_has_nvidia(GPtrArray *devices)
{
    for (guint i = 0; i < devices->len; i++)
        if (((Device *)devices->pdata[i])->nv_bus) return TRUE;
    return FALSE;
}

typedef struct {
    GPtrArray     *devices;
    TempfmReadDone done;
    gpointer       data;
} NvJob;

static gboolean nv_busy = FALSE;

/* nvidia-smi prints "00000000:01:00.0", sysfs says "0000:01:00.0" */
static gboolean
same_bus(const char *smi, const char *sys)
{
    const char *a = strchr(smi, ':'), *b = strchr(sys, ':');
    return a && b && !g_ascii_strcasecmp(a, b);
}

static void
on_nvidia_done(GObject *src, GAsyncResult *res, gpointer ud)
{
    NvJob *job = ud;
    char *out = NULL;
    GError *err = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res,
                                                       &out, NULL, &err);
    nv_busy = FALSE;
    gboolean cancelled = err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);

    char **lines = ok && out ? g_strsplit(out, "\n", -1) : NULL;
    for (guint i = 0; i < job->devices->len; i++) {
        Device *d = job->devices->pdata[i];
        if (!d->nv_bus || d->asleep) continue;
        Sensor *s = d->sensors->pdata[0];
        gboolean found = FALSE;
        for (int l = 0; lines && lines[l]; l++) {
            /* bus, name, temperature, utilization, memory used, memory total */
            char **f = g_strsplit(lines[l], ",", 6);
            if (g_strv_length(f) == 6 && same_bus(g_strstrip(f[0]), d->nv_bus)) {
                char *end = NULL;
                double v = g_ascii_strtod(g_strstrip(f[2]), &end);
                found = end && end != f[2];
                sensor_store(s, found, v);
                char *nm = g_strstrip(f[1]);
                if (*nm && strcmp(nm, d->name)) {
                    g_free(d->name);
                    d->name = g_strdup(nm);
                }
                end = NULL;
                v = g_ascii_strtod(g_strstrip(f[3]), &end);
                d->gpu_usage = end && end != f[3] ? v : -1;
                d->vram_used  = g_ascii_strtod(g_strstrip(f[4]), NULL);
                d->vram_total = g_ascii_strtod(g_strstrip(f[5]), NULL);
            }
            g_strfreev(f);
        }
        if (!found) {
            s->valid = FALSE;
            d->gpu_usage = -1;
        }
    }
    g_strfreev(lines);
    g_free(out);

    if (!cancelled && job->done) job->done(job->data);
    if (err) g_error_free(err);
    g_ptr_array_unref(job->devices);
    g_free(job);
}

void
tempfm_sensors_read_nvidia_async(GPtrArray *devices, GCancellable *cancel,
                                 TempfmReadDone done, gpointer data)
{
    gboolean any_awake = FALSE;

    for (guint i = 0; i < devices->len; i++) {
        Device *d = devices->pdata[i];
        if (!d->nv_bus) continue;
        /* "suspended" = the dGPU is powered off; nvidia-smi would wake it */
        d->asleep = gpu_asleep(d->pci_dir);
        if (d->asleep) {
            ((Sensor *)d->sensors->pdata[0])->valid = FALSE;
            d->gpu_usage = 0;                       /* powered off = idle */
            d->vram_used = 0;
        } else {
            any_awake = TRUE;
        }
    }

    if (!any_awake || nv_busy) {
        if (done) done(data);
        return;
    }

    GSubprocess *proc = g_subprocess_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
        "nvidia-smi",
        "--query-gpu=pci.bus_id,name,temperature.gpu,utilization.gpu,memory.used,memory.total",
        "--format=csv,noheader,nounits", NULL);
    if (!proc) {
        if (done) done(data);
        return;
    }

    NvJob *job = g_new0(NvJob, 1);
    job->devices = g_ptr_array_ref(devices);
    job->done    = done;
    job->data    = data;
    nv_busy = TRUE;
    g_subprocess_communicate_utf8_async(proc, NULL, cancel, on_nvidia_done, job);
    g_object_unref(proc);
}
