/* monitor.c – the clock of TempeR.
 *
 * One timer for the whole program: every "Update every" seconds it reads
 * the sensors (sensors.c) and the system usage below, then tells every
 * listener: the main window, the overlay and the tray icon.  The main
 * window can be hidden in the tray while the overlay keeps running.
 *
 *   CPU usage      /proc/stat
 *   CPU clock      /sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq
 *   RAM, swap      /proc/meminfo
 *   disk activity  /proc/diskstats (time spent doing I/O)
 *   battery        /sys/class/power_supply/BAT*
 */

#include "tempfm.h"

#include <stdlib.h>

typedef struct {
    guint          id;
    TempfmListener cb;
    gpointer       data;
} Listener;

static GPtrArray    *devices   = NULL;
static GArray       *listeners = NULL;   /* Listener */
static guint         next_id   = 1;
static guint         timer     = 0;
static GCancellable *cancel    = NULL;
static SystemStats   stats     = { .cpu_usage = -1, .disk_busy = -1, .battery = -1 };

/* previous samples */
static guint64 cpu_prev_total = 0, cpu_prev_idle = 0;
static gint64 cpu_prev_time = 0;
static GHashTable *disk_prev = NULL;     /* name → io_ticks (ms) */
static gint64 disk_prev_time = 0;

/* Usage is a difference between two samples: right after start or a
 * Refresh the previous one is only milliseconds old, which would give
 * nonsense (0 % or 100 %).  Keep the old value until enough time passed. */
#define MIN_SAMPLE_US  (250 * 1000)


/* ── system usage ── */

static void
read_cpu(void)
{
    char *txt = NULL;
    gint64 now = g_get_monotonic_time();
    if (cpu_prev_time && now - cpu_prev_time < MIN_SAMPLE_US) return;
    if (!g_file_get_contents("/proc/stat", &txt, NULL, NULL)) return;
    cpu_prev_time = now;
    guint64 v[10] = { 0 };
#define U64 "%" G_GUINT64_FORMAT
    if (g_str_has_prefix(txt, "cpu ") &&
        sscanf(txt + 4, U64 " " U64 " " U64 " " U64 " " U64 " " U64 " " U64 " " U64 " " U64 " " U64,
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) >= 4) {
        /* guest time is already counted in user/nice */
        guint64 idle  = v[3] + v[4];
        guint64 total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
        if (cpu_prev_total && total > cpu_prev_total) {
            double dt = (double)(total - cpu_prev_total);
            double di = (double)(idle - cpu_prev_idle);
            stats.cpu_usage = CLAMP(100.0 * (1.0 - di / dt), 0, 100);
        }
        cpu_prev_total = total;
        cpu_prev_idle  = idle;
    }
    g_free(txt);

    /* average clock of all threads */
    double sum = 0;
    int n = 0;
    GDir *d = g_dir_open("/sys/devices/system/cpu", 0, NULL);
    if (d) {
        const char *e;
        while ((e = g_dir_read_name(d)) != NULL) {
            if (strncmp(e, "cpu", 3) || !g_ascii_isdigit(e[3])) continue;
            char *p = g_build_filename("/sys/devices/system/cpu", e,
                                       "cpufreq", "scaling_cur_freq", NULL);
            char *t = NULL;
            if (g_file_get_contents(p, &t, NULL, NULL)) {
                sum += g_ascii_strtod(t, NULL) / 1000.0;   /* kHz → MHz */
                n++;
                g_free(t);
            }
            g_free(p);
        }
        g_dir_close(d);
    }
    stats.cpu_mhz = n ? sum / n : 0;
}

static void
read_memory(void)
{
    char *txt = NULL;
    if (!g_file_get_contents("/proc/meminfo", &txt, NULL, NULL)) return;
    double total = 0, avail = -1, free_ = 0, stotal = 0, sfree = 0;
    char **lines = g_strsplit(txt, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        double kb = 0;
        char key[64];
        if (sscanf(lines[i], "%63[^:]: %lf", key, &kb) != 2) continue;
        if      (!strcmp(key, "MemTotal"))     total  = kb;
        else if (!strcmp(key, "MemAvailable")) avail  = kb;
        else if (!strcmp(key, "MemFree"))      free_  = kb;
        else if (!strcmp(key, "SwapTotal"))    stotal = kb;
        else if (!strcmp(key, "SwapFree"))     sfree  = kb;
    }
    g_strfreev(lines);
    g_free(txt);
    stats.ram_total  = total * 1024;
    stats.ram_used   = (total - (avail >= 0 ? avail : free_)) * 1024;
    stats.swap_total = stotal * 1024;
    stats.swap_used  = (stotal - sfree) * 1024;
}

/* whole disks only: nvme0n1, sda, vda, mmcblk0 (not their partitions) */
static gboolean
is_whole_disk(const char *n)
{
    if (g_str_has_prefix(n, "nvme"))
        return strchr(n, 'n') && !strchr(n + 4, 'p');
    if (g_str_has_prefix(n, "mmcblk"))
        return !strchr(n + 6, 'p');
    if ((n[0] == 's' || n[0] == 'v' || n[0] == 'x') && n[1] == 'd') {
        for (const char *c = n + 2; *c; c++)
            if (!g_ascii_isalpha(*c)) return FALSE;
        return TRUE;
    }
    return FALSE;
}

static void
read_disks(void)
{
    char *txt = NULL;
    gint64 now = g_get_monotonic_time();
    if (disk_prev_time && now - disk_prev_time < MIN_SAMPLE_US) return;
    if (!g_file_get_contents("/proc/diskstats", &txt, NULL, NULL)) return;
    if (!disk_prev) disk_prev = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    double dt_ms = disk_prev_time ? (now - disk_prev_time) / 1000.0 : 0;
    double busiest = -1;

    char **lines = g_strsplit(txt, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        unsigned maj, min;
        char name[64];
        guint64 f[10];
        /* rd_ios rd_merges rd_sectors rd_ticks wr_ios wr_merges wr_sectors
         * wr_ticks in_flight io_ticks */
        if (sscanf(lines[i], " %u %u %63s " U64 " " U64 " " U64 " " U64 " " U64 " "
                   U64 " " U64 " " U64 " " U64 " " U64,
                   &maj, &min, name, &f[0], &f[1], &f[2], &f[3], &f[4], &f[5],
                   &f[6], &f[7], &f[8], &f[9]) != 13) continue;
        if (!is_whole_disk(name)) continue;
        guint64 *prev = g_hash_table_lookup(disk_prev, name);
        if (prev && dt_ms > 0) {
            double busy = CLAMP((double)(f[9] - *prev) * 100.0 / dt_ms, 0, 100);
            if (busy > busiest) busiest = busy;
        }
        guint64 *nv = g_new(guint64, 1);
        *nv = f[9];
        g_hash_table_replace(disk_prev, g_strdup(name), nv);
    }
    g_strfreev(lines);
    g_free(txt);
    disk_prev_time = now;
    stats.disk_busy = busiest;
}

static void
read_battery(void)
{
    stats.battery = -1;
    GDir *d = g_dir_open("/sys/class/power_supply", 0, NULL);
    if (!d) return;
    const char *e;
    while ((e = g_dir_read_name(d)) != NULL) {
        char *dir = g_build_filename("/sys/class/power_supply", e, NULL);
        char *type = NULL, *cap = NULL, *st = NULL;
        char *p = g_build_filename(dir, "type", NULL);
        g_file_get_contents(p, &type, NULL, NULL);
        g_free(p);
        if (type && g_str_has_prefix(type, "Battery")) {
            p = g_build_filename(dir, "capacity", NULL);
            g_file_get_contents(p, &cap, NULL, NULL);
            g_free(p);
            p = g_build_filename(dir, "status", NULL);
            g_file_get_contents(p, &st, NULL, NULL);
            g_free(p);
            if (cap) {
                stats.battery  = atoi(cap);
                stats.charging = st && (g_str_has_prefix(st, "Charging") ||
                                        g_str_has_prefix(st, "Full"));
            }
        }
        g_free(type);
        g_free(cap);
        g_free(st);
        g_free(dir);
        if (stats.battery >= 0) break;
    }
    g_dir_close(d);
}

/* RAM the kernel brought online, counted in memory blocks: unlike
 * MemTotal it includes what the firmware and an integrated GPU keep for
 * themselves.  0 when /sys/devices/system/memory is not there. */
static double
online_memory_bytes(void)
{
    char *t = NULL;
    if (!g_file_get_contents("/sys/devices/system/memory/block_size_bytes", &t, NULL, NULL))
        return 0;
    double block = (double)g_ascii_strtoull(t, NULL, 16);
    g_free(t);
    int n = 0;
    GDir *d = g_dir_open("/sys/devices/system/memory", 0, NULL);
    if (d) {
        const char *e;
        while ((e = g_dir_read_name(d)) != NULL) {
            if (!g_str_has_prefix(e, "memory") || !g_ascii_isdigit(e[6])) continue;
            char *p = g_build_filename("/sys/devices/system/memory", e, "state", NULL);
            char *st = NULL;
            if (g_file_get_contents(p, &st, NULL, NULL) && g_str_has_prefix(st, "online")) n++;
            g_free(st);
            g_free(p);
        }
        g_dir_close(d);
    }
    return block * n;
}

/* Installed RAM in GB, rounded up to an even number (what is fitted is
 * always a bit more than what the kernel can use). */
int
tempfm_installed_ram_gb(void)
{
    static int cached = 0;
    if (cached) return cached;
    if (stats.ram_total <= 0) read_memory();
    double bytes = MAX(online_memory_bytes(), stats.ram_total);
    double gib = bytes / (1024.0 * 1024.0 * 1024.0);
    int gb = (int)gib;
    if (gib > gb) gb++;
    if (gb > 2 && gb % 2) gb++;
    cached = MAX(gb, 1);
    return cached;
}

int
tempfm_ram_gb(void)
{
    int o = tempfm_settings()->spec_ram_gb;
    return o > 0 ? o : tempfm_installed_ram_gb();
}


/* ── listeners ── */

static void
notify_all(gboolean rebuilt)
{
    if (!listeners) return;
    /* copy: a listener may (un)register while being called */
    GArray *copy = g_array_copy(listeners);
    for (guint i = 0; i < copy->len; i++) {
        Listener *l = &g_array_index(copy, Listener, i);
        l->cb(rebuilt, l->data);
    }
    g_array_unref(copy);
}

guint
tempfm_monitor_listen(TempfmListener cb, gpointer data)
{
    if (!listeners) listeners = g_array_new(FALSE, TRUE, sizeof(Listener));
    Listener l = { next_id++, cb, data };
    g_array_append_val(listeners, l);
    return l.id;
}

void
tempfm_monitor_unlisten(guint id)
{
    if (!listeners) return;
    for (guint i = 0; i < listeners->len; i++)
        if (g_array_index(listeners, Listener, i).id == id) {
            g_array_remove_index(listeners, i);
            return;
        }
}

void
tempfm_monitor_notify(void)
{
    notify_all(FALSE);
}

static void
on_nvidia_read(gpointer data)
{
    (void)data;
    notify_all(FALSE);
}

static gboolean
on_tick(gpointer data)
{
    (void)data;
    tempfm_sensors_read(devices);
    read_cpu();
    read_memory();
    read_disks();
    read_battery();
    notify_all(FALSE);
    if (tempfm_sensors_has_nvidia(devices))
        tempfm_sensors_read_nvidia_async(devices, cancel, on_nvidia_read, NULL);
    return G_SOURCE_CONTINUE;
}

void
tempfm_monitor_restart_timer(void)
{
    if (timer) g_source_remove(timer);
    timer = g_timeout_add_seconds((guint)tempfm_settings()->interval, on_tick, NULL);
}

/* (Re)discovers the sensors: Refresh */
void
tempfm_monitor_rediscover(void)
{
    if (cancel) {
        g_cancellable_cancel(cancel);
        g_object_unref(cancel);
    }
    cancel = g_cancellable_new();
    if (devices) g_ptr_array_unref(devices);
    devices = tempfm_sensors_discover();
    notify_all(TRUE);
    on_tick(NULL);
    tempfm_monitor_restart_timer();
}

void
tempfm_monitor_start(void)
{
    if (devices) return;
    read_cpu();             /* first samples, so the first tick has a delta */
    read_disks();
    tempfm_monitor_rediscover();
}

void
tempfm_monitor_stop(void)
{
    if (timer) g_source_remove(timer);
    timer = 0;
    if (cancel) g_cancellable_cancel(cancel);
}

GPtrArray *
tempfm_monitor_devices(void)
{
    if (!devices) tempfm_monitor_start();
    return devices;
}

const SystemStats *
tempfm_monitor_stats(void)
{
    return &stats;
}
