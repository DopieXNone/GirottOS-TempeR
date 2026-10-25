/* specs.c – the computer's specs window.
 *
 * Lists processor, graphics cards, memory and system.  The names of the
 * processor and of every graphics card, and the amount of RAM, can be
 * changed: they are saved in settings.ini ([Specs]) and shown everywhere
 * in TempeR instead of the real ones, until "Reset".
 */

#include "tempfm.h"

#include <stdlib.h>
#include <sys/utsname.h>

typedef struct {
    char *key;          /* "cpu", "gpu-0000-01-00-0" */
    char *original;
} Named;

typedef struct {
    GPtrArray *named;   /* Named*, same order as entries */
    GPtrArray *entries; /* GtkEntry*                     */
    GtkWidget *ram_spin;
    gboolean   building;
} SpecsUI;

static GtkWidget *specs_win = NULL;

gboolean
tempfm_specs_modified(void)
{
    TempfmSettings *s = tempfm_settings();
    return g_hash_table_size(s->spec_names) > 0 || s->spec_ram_gb > 0;
}

static void
named_free(gpointer p)
{
    Named *n = p;
    g_free(n->key);
    g_free(n->original);
    g_free(n);
}

static void
specs_ui_free(gpointer p)
{
    SpecsUI *ui = p;
    g_ptr_array_unref(ui->named);
    g_ptr_array_unref(ui->entries);
    g_free(ui);
}


/* ── facts ── */

static char *
os_name(void)
{
    char *txt = NULL, *res = NULL;
    if (g_file_get_contents("/etc/os-release", &txt, NULL, NULL) ||
        g_file_get_contents("/usr/lib/os-release", &txt, NULL, NULL)) {
        char **lines = g_strsplit(txt, "\n", -1);
        for (int i = 0; lines[i] && !res; i++)
            if (g_str_has_prefix(lines[i], "PRETTY_NAME=")) {
                char *v = lines[i] + 12;
                res = g_shell_unquote(v, NULL);
                if (!res) res = g_strdup(v);
            }
        g_strfreev(lines);
        g_free(txt);
    }
    return res ? res : g_strdup("Linux");
}

static void
cpu_counts(int *cores, int *threads)
{
    char *txt = NULL;
    *cores = *threads = 0;
    if (!g_file_get_contents("/proc/cpuinfo", &txt, NULL, NULL)) return;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char **lines = g_strsplit(txt, "\n", -1);
    int phys = 0;
    for (int i = 0; lines[i]; i++) {
        int v;
        if (g_str_has_prefix(lines[i], "processor")) (*threads)++;
        else if (sscanf(lines[i], "physical id : %d", &v) == 1) phys = v;
        else if (sscanf(lines[i], "core id : %d", &v) == 1)
            g_hash_table_add(seen, g_strdup_printf("%d/%d", phys, v));
    }
    *cores = g_hash_table_size(seen) ? (int)g_hash_table_size(seen) : *threads;
    g_hash_table_unref(seen);
    g_strfreev(lines);
    g_free(txt);
}

/* g_ptr_array_sort passes pointers to the elements: card0 < card1 < card10 */
static int
cmp_card(gconstpointer a, gconstpointer b)
{
    const char *sa = *(const char * const *)a, *sb = *(const char * const *)b;
    long na = strtol(sa + 4, NULL, 10), nb = strtol(sb + 4, NULL, 10);
    return na < nb ? -1 : na > nb;
}

/* Every graphics card, also those without a temperature sensor (an Intel
 * iGPU, for example) */
static void
list_gpus(GPtrArray *named)
{
    GPtrArray *devs = tempfm_monitor_devices();
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GDir *d = g_dir_open("/sys/class/drm", 0, NULL);
    GPtrArray *cards = g_ptr_array_new_with_free_func(g_free);
    if (d) {
        const char *e;
        while ((e = g_dir_read_name(d)) != NULL) {
            if (!g_str_has_prefix(e, "card") || !g_ascii_isdigit(e[4]) || strchr(e, '-'))
                continue;
            g_ptr_array_add(cards, g_strdup(e));
        }
        g_dir_close(d);
    }
    g_ptr_array_sort(cards, cmp_card);

    for (guint i = 0; i < cards->len; i++) {
        char *link = g_build_filename("/sys/class/drm", cards->pdata[i], "device", NULL);
        char *dev = realpath(link, NULL);
        g_free(link);
        if (!dev) continue;
        char *cls = NULL;
        char *cp = g_build_filename(dev, "class", NULL);
        g_file_get_contents(cp, &cls, NULL, NULL);
        g_free(cp);
        char *bus = g_path_get_basename(dev);
        if (cls && g_str_has_prefix(cls, "0x03") && !g_hash_table_contains(seen, bus)) {
            g_hash_table_add(seen, g_strdup(bus));
            Named *n = g_new0(Named, 1);
            n->key = tempfm_gpu_key(bus);
            for (guint k = 0; devs && k < devs->len && !n->original; k++) {
                Device *dv = devs->pdata[k];
                if (dv->key && !strcmp(dv->key, n->key)) n->original = g_strdup(dv->name);
            }
            if (!n->original) n->original = tempfm_pci_name(dev);
            g_ptr_array_add(named, n);
        }
        g_free(bus);
        g_free(cls);
        free(dev);
    }
    g_ptr_array_unref(cards);
    g_hash_table_unref(seen);
}


/* ── editing ── */

static void
on_name_changed(GtkEditable *e, gpointer ud)
{
    SpecsUI *ui = ud;
    if (ui->building) return;
    for (guint i = 0; i < ui->entries->len; i++) {
        if (ui->entries->pdata[i] != (gpointer)e) continue;
        Named *n = ui->named->pdata[i];
        char *t = g_strstrip(g_strdup(gtk_editable_get_text(e)));
        if (*t && strcmp(t, n->original))
            g_hash_table_replace(tempfm_settings()->spec_names, g_strdup(n->key), t);
        else {
            g_hash_table_remove(tempfm_settings()->spec_names, n->key);
            g_free(t);
        }
    }
    tempfm_settings_changed(FALSE);
    tempfm_monitor_notify();
}

static void
on_ram_changed(GtkSpinButton *sp, gpointer ud)
{
    SpecsUI *ui = ud;
    if (ui->building) return;
    int v = gtk_spin_button_get_value_as_int(sp);
    tempfm_settings()->spec_ram_gb = v == tempfm_installed_ram_gb() ? 0 : v;
    tempfm_settings_changed(FALSE);
    tempfm_monitor_notify();
}

static void
on_reset(GtkButton *b, gpointer ud)
{
    (void)b;
    SpecsUI *ui = ud;
    TempfmSettings *s = tempfm_settings();
    g_hash_table_remove_all(s->spec_names);
    s->spec_ram_gb = 0;

    ui->building = TRUE;
    for (guint i = 0; i < ui->entries->len; i++)
        gtk_editable_set_text(GTK_EDITABLE(ui->entries->pdata[i]),
                              ((Named *)ui->named->pdata[i])->original);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->ram_spin), tempfm_installed_ram_gb());
    ui->building = FALSE;

    tempfm_settings_changed(FALSE);
    tempfm_monitor_notify();
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

static GtkWidget *
value_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "prop-value");
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(l), 1.0f);
    return l;
}

static GtkWidget *
name_entry(SpecsUI *ui, Named *n)
{
    GtkWidget *e = gtk_entry_new();
    gtk_widget_add_css_class(e, "text-entry");
    const char *cur = g_hash_table_lookup(tempfm_settings()->spec_names, n->key);
    gtk_editable_set_text(GTK_EDITABLE(e), cur ? cur : n->original);
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), n->original);
    gtk_widget_set_size_request(e, 280, -1);
    g_signal_connect(e, "changed", G_CALLBACK(on_name_changed), ui);
    g_ptr_array_add(ui->named, n);
    g_ptr_array_add(ui->entries, e);
    return e;
}

void
tempfm_open_specs(void)
{
    if (specs_win) {
        gtk_window_present(GTK_WINDOW(specs_win));
        return;
    }

    SpecsUI *ui = g_new0(SpecsUI, 1);
    ui->named   = g_ptr_array_new_with_free_func(named_free);
    ui->entries = g_ptr_array_new();
    ui->building = TRUE;

    GtkWidget *win = gtk_window_new();
    specs_win = win;
    g_object_add_weak_pointer(G_OBJECT(win), (gpointer *)&specs_win);
    g_object_set_data_full(G_OBJECT(win), "specs-ui", ui, specs_ui_free);
    GApplication *app = g_application_get_default();
    if (app && GTK_IS_APPLICATION(app))
        gtk_window_set_application(GTK_WINDOW(win), GTK_APPLICATION(app));

    gtk_widget_add_css_class(win, "props");
    gtk_window_set_title(GTK_WINDOW(win), "Specs");
    gtk_window_set_default_size(GTK_WINDOW(win), 520, -1);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    tempfm_window_titlebar(win, "Specs");

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), win);
    gtk_widget_add_controller(win, kc);

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(page, 12);
    gtk_widget_set_margin_end(page, 12);
    gtk_widget_set_margin_top(page, 10);
    gtk_widget_set_margin_bottom(page, 12);
    gtk_window_set_child(GTK_WINDOW(win), page);

    /* ── header: computer icon, host name, OS ── */
    char *os = os_name();
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *ic = tempfm_icon(g_file_test("/sys/class/power_supply/BAT0", G_FILE_TEST_EXISTS)
                                ? "computer-laptop" : "computer", "\xF0\x9F\x92\xBB", 48);
    gtk_box_append(GTK_BOX(head), ic);
    GtkWidget *hv = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(hv, GTK_ALIGN_CENTER);
    GtkWidget *t = gtk_label_new(g_get_host_name());
    gtk_widget_add_css_class(t, "prop-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_box_append(GTK_BOX(hv), t);
    GtkWidget *st = gtk_label_new(os);
    gtk_widget_add_css_class(st, "prop-subtitle");
    gtk_label_set_xalign(GTK_LABEL(st), 0);
    gtk_box_append(GTK_BOX(hv), st);
    gtk_box_append(GTK_BOX(head), hv);
    gtk_box_append(GTK_BOX(page), head);

    /* ── processor ── */
    gtk_box_append(GTK_BOX(page), tempfm_group_title("Processor"));
    GtkWidget *card = tempfm_card_new();
    Named *cpu = g_new0(Named, 1);
    cpu->key = g_strdup("cpu");
    cpu->original = tempfm_cpu_model();
    tempfm_card_add_widget(card, "Name", name_entry(ui, cpu));
    int cores, threads;
    cpu_counts(&cores, &threads);
    char *ct = g_strdup_printf("%d cores, %d threads", cores, threads);
    tempfm_card_add_widget(card, "Cores", value_label(ct));
    g_free(ct);
    gtk_box_append(GTK_BOX(page), card);

    /* ── graphics ── */
    GPtrArray *tmp = g_ptr_array_new();
    list_gpus(tmp);
    gtk_box_append(GTK_BOX(page), tempfm_group_title(
        tmp->len > 1 ? "Graphics cards" : "Graphics card"));
    card = tempfm_card_new();
    for (guint i = 0; i < tmp->len; i++) {
        char *lbl = tmp->len > 1 ? g_strdup_printf("GPU %u", i + 1) : g_strdup("Name");
        tempfm_card_add_widget(card, lbl, name_entry(ui, tmp->pdata[i]));
        g_free(lbl);
    }
    if (!tmp->len) tempfm_card_add_widget(card, "Name", value_label("None found"));
    g_ptr_array_unref(tmp);
    gtk_box_append(GTK_BOX(page), card);

    /* ── memory ── */
    gtk_box_append(GTK_BOX(page), tempfm_group_title("Memory"));
    card = tempfm_card_new();
    GtkWidget *rb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    ui->ram_spin = gtk_spin_button_new_with_range(1, 1 << 20, 1);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(ui->ram_spin), TRUE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->ram_spin), tempfm_ram_gb());
    gtk_widget_set_size_request(ui->ram_spin, 120, -1);
    g_signal_connect(ui->ram_spin, "value-changed", G_CALLBACK(on_ram_changed), ui);
    gtk_box_append(GTK_BOX(rb), ui->ram_spin);
    GtkWidget *gb = gtk_label_new("GB");
    gtk_widget_add_css_class(gb, "prop-value");
    gtk_box_append(GTK_BOX(rb), gb);
    tempfm_card_add_widget(card, "Installed RAM", rb);
    gtk_box_append(GTK_BOX(page), card);

    /* ── system ── */
    gtk_box_append(GTK_BOX(page), tempfm_group_title("System"));
    card = tempfm_card_new();
    tempfm_card_add_widget(card, "Operating system", value_label(os));
    struct utsname u;
    if (uname(&u) == 0) {
        char *k = g_strdup_printf("%s %s (%s)", u.sysname, u.release, u.machine);
        tempfm_card_add_widget(card, "Kernel", value_label(k));
        g_free(k);
    }
    const char *de = g_getenv("XDG_CURRENT_DESKTOP");
    tempfm_card_add_widget(card, "Desktop", value_label(de && *de ? de : "Unknown"));
    const char *dt = G_OBJECT_TYPE_NAME(gdk_display_get_default());
    tempfm_card_add_widget(card, "Display server", value_label(
        strstr(dt, "Wayland") ? "Wayland" : strstr(dt, "X11") ? "X11" : dt));
    gtk_box_append(GTK_BOX(page), card);
    g_free(os);

    /* ── buttons ── */
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btns, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btns, 12);
    GtkWidget *reset = gtk_button_new_with_label("Reset");
    gtk_widget_add_css_class(reset, "dlg-btn-danger");
    gtk_widget_set_tooltip_text(reset, "Go back to the real names and amount of RAM");
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset), ui);
    gtk_box_append(GTK_BOX(btns), reset);
    GtkWidget *close = gtk_button_new_with_label("Close");
    gtk_widget_add_css_class(close, "dlg-btn-accent");
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_close), win);
    gtk_box_append(GTK_BOX(btns), close);
    gtk_box_append(GTK_BOX(page), btns);

    ui->building = FALSE;
    gtk_window_present(GTK_WINDOW(win));
}
