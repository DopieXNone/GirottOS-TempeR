/* tray.c – the system tray icon, so TempeR can keep running in the
 * background (overlay included) with its window closed.
 *
 * GTK 4 has no tray API, so this speaks the StatusNotifierItem protocol
 * (plus com.canonical.dbusmenu for the menu) directly over D-Bus with
 * GIO, which is part of GLib: no extra library.  Supported by KDE,
 * GNOME (with the AppIndicator extension, default on most distributions),
 * XFCE, Cinnamon, LXQt, waybar (Hyprland/Sway), and others.
 *
 * Without a tray host, closing the main window simply quits.
 */

#include "tempfm.h"

#define ITEM_PATH  "/StatusNotifierItem"
#define MENU_PATH  "/MenuBar"

enum { MENU_ROOT, MENU_OPEN, MENU_OVERLAY, MENU_AWAKE, MENU_FANS, MENU_SEP1, MENU_SETTINGS, MENU_SEP2, MENU_QUIT };

static GDBusConnection *bus = NULL;
static char            *bus_name = NULL;
static gboolean         registered = FALSE;
static guint            menu_revision = 1;
static guint            listen_id = 0;
static char            *tooltip = NULL;
static GVariant        *pixmaps = NULL;     /* a(iiay), floating-free ref */

static const char ITEM_XML[] =
    "<node>"
    " <interface name='org.kde.StatusNotifierItem'>"
    "  <property name='Category' type='s' access='read'/>"
    "  <property name='Id' type='s' access='read'/>"
    "  <property name='Title' type='s' access='read'/>"
    "  <property name='Status' type='s' access='read'/>"
    "  <property name='WindowId' type='i' access='read'/>"
    "  <property name='IconName' type='s' access='read'/>"
    "  <property name='IconThemePath' type='s' access='read'/>"
    "  <property name='IconPixmap' type='a(iiay)' access='read'/>"
    "  <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "  <property name='ItemIsMenu' type='b' access='read'/>"
    "  <property name='Menu' type='o' access='read'/>"
    "  <method name='ContextMenu'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "  <method name='Activate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "  <method name='SecondaryActivate'><arg name='x' type='i' direction='in'/><arg name='y' type='i' direction='in'/></method>"
    "  <method name='Scroll'><arg name='delta' type='i' direction='in'/><arg name='orientation' type='s' direction='in'/></method>"
    "  <signal name='NewTitle'/>"
    "  <signal name='NewIcon'/>"
    "  <signal name='NewToolTip'/>"
    "  <signal name='NewStatus'><arg name='status' type='s'/></signal>"
    " </interface>"
    "</node>";

static const char MENU_XML[] =
    "<node>"
    " <interface name='com.canonical.dbusmenu'>"
    "  <property name='Version' type='u' access='read'/>"
    "  <property name='TextDirection' type='s' access='read'/>"
    "  <property name='Status' type='s' access='read'/>"
    "  <property name='IconThemePath' type='as' access='read'/>"
    "  <method name='GetLayout'>"
    "   <arg type='i' name='parentId' direction='in'/>"
    "   <arg type='i' name='recursionDepth' direction='in'/>"
    "   <arg type='as' name='propertyNames' direction='in'/>"
    "   <arg type='u' name='revision' direction='out'/>"
    "   <arg type='(ia{sv}av)' name='layout' direction='out'/>"
    "  </method>"
    "  <method name='GetGroupProperties'>"
    "   <arg type='ai' name='ids' direction='in'/>"
    "   <arg type='as' name='propertyNames' direction='in'/>"
    "   <arg type='a(ia{sv})' name='properties' direction='out'/>"
    "  </method>"
    "  <method name='GetProperty'>"
    "   <arg type='i' name='id' direction='in'/>"
    "   <arg type='s' name='name' direction='in'/>"
    "   <arg type='v' name='value' direction='out'/>"
    "  </method>"
    "  <method name='Event'>"
    "   <arg type='i' name='id' direction='in'/>"
    "   <arg type='s' name='eventId' direction='in'/>"
    "   <arg type='v' name='data' direction='in'/>"
    "   <arg type='u' name='timestamp' direction='in'/>"
    "  </method>"
    "  <method name='EventGroup'>"
    "   <arg type='a(isvu)' name='events' direction='in'/>"
    "   <arg type='ai' name='idErrors' direction='out'/>"
    "  </method>"
    "  <method name='AboutToShow'>"
    "   <arg type='i' name='id' direction='in'/>"
    "   <arg type='b' name='needUpdate' direction='out'/>"
    "  </method>"
    "  <method name='AboutToShowGroup'>"
    "   <arg type='ai' name='ids' direction='in'/>"
    "   <arg type='ai' name='updatesNeeded' direction='out'/>"
    "   <arg type='ai' name='idErrors' direction='out'/>"
    "  </method>"
    "  <signal name='ItemsPropertiesUpdated'>"
    "   <arg type='a(ia{sv})' name='updatedProps'/>"
    "   <arg type='a(ias)' name='removedProps'/>"
    "  </signal>"
    "  <signal name='LayoutUpdated'>"
    "   <arg type='u' name='revision'/>"
    "   <arg type='i' name='parent'/>"
    "  </signal>"
    "  <signal name='ItemActivationRequested'>"
    "   <arg type='i' name='id'/>"
    "   <arg type='u' name='timestamp'/>"
    "  </signal>"
    " </interface>"
    "</node>";

gboolean
tempfm_tray_available(void)
{
    return registered;
}


/* ── icon ── */

/* ARGB32 in network byte order, as the protocol wants */
static GVariant *
pixmap_from_file(const char *path, int size)
{
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size(path, size, size, NULL);
    if (!pb) return NULL;
    if (!gdk_pixbuf_get_has_alpha(pb)) {
        GdkPixbuf *a = gdk_pixbuf_add_alpha(pb, FALSE, 0, 0, 0);
        g_object_unref(pb);
        pb = a;
    }
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    int stride = gdk_pixbuf_get_rowstride(pb);
    const guchar *px = gdk_pixbuf_read_pixels(pb);
    guchar *out = g_malloc((gsize)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const guchar *p = px + y * stride + x * 4;       /* R G B A */
            guchar *o = out + ((gsize)y * w + x) * 4;
            o[0] = p[3]; o[1] = p[0]; o[2] = p[1]; o[3] = p[2];
        }
    g_object_unref(pb);
    GVariant *bytes = g_variant_new_from_data(G_VARIANT_TYPE("ay"), out,
        (gsize)w * h * 4, TRUE, g_free, out);
    return g_variant_new("(ii@ay)", w, h, bytes);
}

static void
build_pixmaps(void)
{
    if (pixmaps) return;
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a(iiay)"));
    char *path = tempfm_tela_file("psensor", 64);
    if (path) {
        static const int sizes[] = { 22, 32, 48, 64 };
        for (guint i = 0; i < G_N_ELEMENTS(sizes); i++) {
            GVariant *v = pixmap_from_file(path, sizes[i]);
            if (v) g_variant_builder_add_value(&b, v);
        }
        g_free(path);
    }
    pixmaps = g_variant_ref_sink(g_variant_builder_end(&b));
}


/* ── menu ── */

static GVariant *
menu_props(int id)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    switch (id) {
        case MENU_ROOT:
            g_variant_builder_add(&b, "{sv}", "children-display", g_variant_new_string("submenu"));
            break;
        case MENU_OPEN:
            g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("Open TempeR"));
            break;
        case MENU_OVERLAY:
            g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("Show overlay"));
            g_variant_builder_add(&b, "{sv}", "toggle-type", g_variant_new_string("checkmark"));
            g_variant_builder_add(&b, "{sv}", "toggle-state",
                g_variant_new_int32(tempfm_settings()->ov_enabled ? 1 : 0));
            break;
        case MENU_AWAKE:
            g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("Keep awake"));
            g_variant_builder_add(&b, "{sv}", "toggle-type", g_variant_new_string("checkmark"));
            g_variant_builder_add(&b, "{sv}", "toggle-state",
                g_variant_new_int32(tempfm_settings()->keep_awake ? 1 : 0));
            break;
        case MENU_FANS:
            g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("Fan curves"));
            break;
        case MENU_SETTINGS:
            g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("Settings"));
            break;
        case MENU_QUIT:
            g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string("Quit"));
            break;
        case MENU_SEP1:
        case MENU_SEP2:
            g_variant_builder_add(&b, "{sv}", "type", g_variant_new_string("separator"));
            break;
    }
    return g_variant_builder_end(&b);
}

static GVariant *
menu_layout(void)
{
    GVariantBuilder kids;
    g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
    for (int id = MENU_OPEN; id <= MENU_QUIT; id++) {
        GVariant *child = g_variant_new("(i@a{sv}@av)", id, menu_props(id),
                                        g_variant_new_array(G_VARIANT_TYPE_VARIANT, NULL, 0));
        g_variant_builder_add(&kids, "v", child);
    }
    return g_variant_new("(i@a{sv}@av)", MENU_ROOT, menu_props(MENU_ROOT),
                         g_variant_builder_end(&kids));
}

static gboolean
menu_event_idle(gpointer data)
{
    switch (GPOINTER_TO_INT(data)) {
        case MENU_OPEN:     tempfm_gui_show();     break;
        case MENU_SETTINGS: tempfm_open_settings(); break;
        case MENU_FANS:     tempfm_open_fan_control(); break;
        case MENU_QUIT:     tempfm_quit();         break;
        case MENU_OVERLAY:
            tempfm_settings()->ov_enabled = !tempfm_settings()->ov_enabled;
            tempfm_settings_changed(FALSE);
            break;
        case MENU_AWAKE:
            tempfm_settings()->keep_awake = !tempfm_settings()->keep_awake;
            tempfm_settings_changed(FALSE);
            break;
    }
    return G_SOURCE_REMOVE;
}

static void
menu_call(GDBusConnection *c, const char *sender, const char *path,
          const char *iface, const char *method, GVariant *params,
          GDBusMethodInvocation *inv, gpointer ud)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)ud;

    if (!strcmp(method, "GetLayout")) {
        g_dbus_method_invocation_return_value(inv,
            g_variant_new("(u@(ia{sv}av))", menu_revision, menu_layout()));
    } else if (!strcmp(method, "GetGroupProperties")) {
        GVariantIter *ids;
        g_variant_get(params, "(aias)", &ids, NULL);
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(ia{sv})"));
        gint32 id;
        while (g_variant_iter_next(ids, "i", &id))
            g_variant_builder_add(&b, "(i@a{sv})", id, menu_props(id));
        g_variant_iter_free(ids);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(a(ia{sv}))", &b));
    } else if (!strcmp(method, "GetProperty")) {
        gint32 id;
        const char *name;
        g_variant_get(params, "(i&s)", &id, &name);
        GVariant *props = menu_props(id);
        GVariant *v = g_variant_lookup_value(props, name, NULL);
        g_variant_unref(g_variant_ref_sink(props));
        g_dbus_method_invocation_return_value(inv,
            g_variant_new("(v)", v ? v : g_variant_new_string("")));
        if (v) g_variant_unref(v);
    } else if (!strcmp(method, "Event")) {
        gint32 id;
        const char *ev;
        g_variant_get(params, "(i&svu)", &id, &ev, NULL, NULL);
        if (!strcmp(ev, "clicked"))
            g_idle_add(menu_event_idle, GINT_TO_POINTER(id));
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (!strcmp(method, "EventGroup")) {
        GVariantIter *it;
        g_variant_get(params, "(a(isvu))", &it);
        gint32 id;
        const char *ev;
        while (g_variant_iter_next(it, "(i&svu)", &id, &ev, NULL, NULL))
            if (!strcmp(ev, "clicked"))
                g_idle_add(menu_event_idle, GINT_TO_POINTER(id));
        g_variant_iter_free(it);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ai)", NULL));
    } else if (!strcmp(method, "AboutToShow")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE));
    } else if (!strcmp(method, "AboutToShowGroup")) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(aiai)", NULL, NULL));
    } else {
        g_dbus_method_invocation_return_dbus_error(inv,
            "org.freedesktop.DBus.Error.UnknownMethod", method);
    }
}

static GVariant *
menu_get(GDBusConnection *c, const char *sender, const char *path,
         const char *iface, const char *prop, GError **err, gpointer ud)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)err; (void)ud;
    if (!strcmp(prop, "Version"))       return g_variant_new_uint32(3);
    if (!strcmp(prop, "TextDirection")) return g_variant_new_string("ltr");
    if (!strcmp(prop, "Status"))        return g_variant_new_string("normal");
    if (!strcmp(prop, "IconThemePath")) return g_variant_new_strv(NULL, 0);
    return NULL;
}


/* ── item ── */

static void
item_call(GDBusConnection *c, const char *sender, const char *path,
          const char *iface, const char *method, GVariant *params,
          GDBusMethodInvocation *inv, gpointer ud)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)params; (void)ud;
    if (!strcmp(method, "Activate") || !strcmp(method, "SecondaryActivate"))
        tempfm_gui_toggle();
    g_dbus_method_invocation_return_value(inv, NULL);
}

static GVariant *
item_get(GDBusConnection *c, const char *sender, const char *path,
         const char *iface, const char *prop, GError **err, gpointer ud)
{
    (void)c; (void)sender; (void)path; (void)iface; (void)err; (void)ud;
    if (!strcmp(prop, "Category"))   return g_variant_new_string("Hardware");
    if (!strcmp(prop, "Id"))         return g_variant_new_string("temper");
    if (!strcmp(prop, "Title"))      return g_variant_new_string("TempeR");
    if (!strcmp(prop, "Status"))     return g_variant_new_string("Active");
    if (!strcmp(prop, "WindowId"))   return g_variant_new_int32(0);
    if (!strcmp(prop, "IconName")) {
        /* with our own pixmap, leave the name empty so every tray shows the
         * same Tela icon instead of whatever its theme calls "psensor" */
        build_pixmaps();
        return g_variant_new_string(g_variant_n_children(pixmaps) ? ""
                                    : "temperature-symbolic");
    }
    if (!strcmp(prop, "IconThemePath")) {
        char *p = tempfm_tela_file("psensor", 64);
        /* the theme's root: …/icons (the host looks for Tela/… inside) */
        char *root = NULL;
        if (p) {
            char *t = strstr(p, "/Tela");
            if (t) root = g_strndup(p, t - p);
        }
        GVariant *v = g_variant_new_string(root ? root : "");
        g_free(root);
        g_free(p);
        return v;
    }
    if (!strcmp(prop, "IconPixmap")) {
        build_pixmaps();
        return g_variant_ref(pixmaps);
    }
    if (!strcmp(prop, "ToolTip")) {
        build_pixmaps();
        return g_variant_new("(s@a(iiay)ss)", "", pixmaps,
                             "TempeR", tooltip ? tooltip : "");
    }
    if (!strcmp(prop, "ItemIsMenu")) return g_variant_new_boolean(FALSE);
    if (!strcmp(prop, "Menu"))       return g_variant_new_object_path(MENU_PATH);
    return NULL;
}

static const GDBusInterfaceVTable item_vtable = { item_call, item_get, NULL, { 0 } };
static const GDBusInterfaceVTable menu_vtable = { menu_call, menu_get, NULL, { 0 } };

static void
on_registered(GObject *src, GAsyncResult *res, gpointer ud)
{
    (void)ud;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    registered = r != NULL;
    if (r) g_variant_unref(r);
    tempfm_gui_sync(FALSE);       /* "keep in tray" now possible */
}

static void
on_watcher_appeared(GDBusConnection *c, const char *name, const char *owner, gpointer ud)
{
    (void)name; (void)owner; (void)ud;
    g_dbus_connection_call(c, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
        g_variant_new("(s)", bus_name), NULL, G_DBUS_CALL_FLAGS_NONE, 3000,
        NULL, on_registered, NULL);
}

static void
on_watcher_vanished(GDBusConnection *c, const char *name, gpointer ud)
{
    (void)c; (void)name; (void)ud;
    registered = FALSE;
}

/* register only once the name is ours, or the watcher may not find it */
static void
on_name_acquired(GDBusConnection *c, const char *name, gpointer ud)
{
    (void)name; (void)ud;
    static gboolean watching = FALSE;
    if (watching) return;
    watching = TRUE;
    g_bus_watch_name_on_connection(c, "org.kde.StatusNotifierWatcher",
        G_BUS_NAME_WATCHER_FLAGS_NONE, on_watcher_appeared, on_watcher_vanished, NULL, NULL);
}

/* tooltip: "CPU 55 °C · GPU 43 °C" */
static void
on_monitor(gboolean rebuilt, gpointer ud)
{
    (void)rebuilt; (void)ud;
    if (!bus) return;
    GPtrArray *devs = tempfm_monitor_devices();
    GString *s = g_string_new(NULL);
    static const SensorCategory cats[] = { CAT_CPU, CAT_GPU, CAT_STORAGE };
    static const char *short_names[] = { "CPU", "GPU", "Disk" };
    for (guint c = 0; c < G_N_ELEMENTS(cats); c++) {
        Sensor *best = NULL;
        for (guint i = 0; i < devs->len; i++) {
            Device *d = devs->pdata[i];
            if (d->category != cats[c]) continue;
            Sensor *m = tempfm_device_main_sensor(d);
            if (m && m->valid && (!best || m->value > best->value)) best = m;
        }
        if (!best) continue;
        char *t = tempfm_format_temp(best->value);
        g_string_append_printf(s, "%s%s %s", s->len ? "  \xC2\xB7  " : "", short_names[c], t);
        g_free(t);
    }
    if (g_strcmp0(s->str, tooltip)) {
        g_free(tooltip);
        tooltip = g_string_free(s, FALSE);
        g_dbus_connection_emit_signal(bus, NULL, ITEM_PATH,
            "org.kde.StatusNotifierItem", "NewToolTip", NULL, NULL);
    } else {
        g_string_free(s, TRUE);
    }
}

void
tempfm_tray_sync(void)
{
    if (!bus) return;
    menu_revision++;
    g_dbus_connection_emit_signal(bus, NULL, MENU_PATH, "com.canonical.dbusmenu",
        "LayoutUpdated", g_variant_new("(ui)", menu_revision, 0), NULL);
}

void
tempfm_tray_start(GApplication *app)
{
    if (bus) return;
    bus = g_application_get_dbus_connection(app);
    if (!bus) return;
    g_object_ref(bus);

    GDBusNodeInfo *item = g_dbus_node_info_new_for_xml(ITEM_XML, NULL);
    GDBusNodeInfo *menu = g_dbus_node_info_new_for_xml(MENU_XML, NULL);
    g_dbus_connection_register_object(bus, ITEM_PATH, item->interfaces[0],
                                      &item_vtable, NULL, NULL, NULL);
    g_dbus_connection_register_object(bus, MENU_PATH, menu->interfaces[0],
                                      &menu_vtable, NULL, NULL, NULL);
    g_dbus_node_info_unref(item);
    g_dbus_node_info_unref(menu);

    bus_name = g_strdup_printf("org.kde.StatusNotifierItem-%d-1", getpid());
    g_bus_own_name_on_connection(bus, bus_name, G_BUS_NAME_OWNER_FLAGS_NONE,
                                 on_name_acquired, NULL, NULL, NULL);

    listen_id = tempfm_monitor_listen(on_monitor, NULL);
}
