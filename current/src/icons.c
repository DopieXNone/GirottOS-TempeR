/* icons.c – icons from the Tela icon theme, exactly as ExploreR does it.
 *
 * TempeR draws its icons from Tela (https://github.com/vinceliuice/Tela-icon-theme)
 * through a private GtkIconTheme: the desktop's own icon theme setting is
 * left untouched, and the rest of GTK (window buttons, dropdown arrows…)
 * keeps using it.  Only icons actually shipped by Tela are used; anything
 * else, or a system without Tela, falls back to an emoji.
 */

#include "tempfm.h"

static GtkWidget *
emoji_label(const char *emoji, int pixel_size)
{
    GtkWidget *l = gtk_label_new(emoji);
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs,
        pango_attr_size_new_absolute((int)(pixel_size * 0.8) * PANGO_SCALE));
    gtk_label_set_attributes(GTK_LABEL(l), attrs);
    pango_attr_list_unref(attrs);
    gtk_widget_set_size_request(l, pixel_size, pixel_size);
    return l;
}

static GtkIconTheme *
tela_theme(void)
{
    static GtkIconTheme *theme = NULL;
    static gboolean tried = FALSE;
    if (tried) return theme;

    GdkDisplay *display = gdk_display_get_default();
    if (!display) return NULL;      /* try again once there is one */
    tried = TRUE;

    GtkIconTheme *t = g_object_new(GTK_TYPE_ICON_THEME,
        "display", display, "theme-name", "Tela", NULL);
    if (gtk_icon_theme_has_icon(t, "folder")) {
        GtkIconPaintable *ip = gtk_icon_theme_lookup_icon(t, "folder", NULL,
            16, 1, GTK_TEXT_DIR_NONE, 0);
        GFile *f = ip ? gtk_icon_paintable_get_file(ip) : NULL;
        char *path = f ? g_file_get_path(f) : NULL;
        if (path && strstr(path, "/Tela/"))
            theme = t;
        g_free(path);
        g_clear_object(&f);
        g_clear_object(&ip);
    }
    if (!theme) g_object_unref(t);
    return theme;
}

gboolean
tempfm_tela_available(void)
{
    return tela_theme() != NULL;
}

/* Path of the Tela file for an icon, or NULL.  Tela's 16/22/24 px icons
 * are monochrome panel icons; the colourful ones live in scalable/ and
 * are selected by asking for >= 32 px. */
char *
tempfm_tela_file(const char *icon_name, int size)
{
    GtkIconTheme *theme = tela_theme();
    if (!theme || !icon_name) return NULL;
    GtkIconPaintable *ip = gtk_icon_theme_lookup_icon(theme, icon_name, NULL,
        MAX(size, 32), 1, GTK_TEXT_DIR_NONE, 0);
    if (!ip) return NULL;
    GFile *f = gtk_icon_paintable_get_file(ip);
    char *path = f ? g_file_get_path(f) : NULL;
    g_clear_object(&f);
    g_object_unref(ip);
    /* Tela inherits Adwaita/hicolor: refuse icons that came from those */
    if (path && !strstr(path, "/Tela")) {
        g_free(path);
        path = NULL;
    }
    return path;
}

/* Icons already resolved, keyed by "<name>@<size>": the Tela paintable,
 * or NULL when Tela doesn't have it (=> emoji).  Each icon is drawn once
 * and the paintable is shared by every widget that shows it. */
static GHashTable *icon_cache = NULL;

static void
unref_if_set(gpointer p)
{
    if (p) g_object_unref(p);
}

GtkWidget *
tempfm_icon(const char *icon_name, const char *emoji, int pixel_size)
{
    if (icon_name && tempfm_settings()->icon_style == ICONS_TELA && tela_theme()) {
        if (!icon_cache)
            icon_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, unref_if_set);
        char *key = g_strdup_printf("%s@%d", icon_name, pixel_size);
        gpointer p = NULL;
        if (!g_hash_table_lookup_extended(icon_cache, key, NULL, &p)) {
            char *path = tempfm_tela_file(icon_name, pixel_size);
            if (path) {
                GFile *f = g_file_new_for_path(path);
                /* drawn at 2x, so it stays sharp on HiDPI screens */
                p = gtk_icon_paintable_new_for_file(f, pixel_size, 2);
                g_object_unref(f);
                g_free(path);
            }
            g_hash_table_insert(icon_cache, g_strdup(key), p);   /* may be NULL */
        }
        g_free(key);
        if (p) {
            GtkWidget *img = gtk_image_new_from_paintable(GDK_PAINTABLE(p));
            gtk_image_set_pixel_size(GTK_IMAGE(img), pixel_size);
            return img;
        }
    }
    return emoji_label(emoji ? emoji : "\xF0\x9F\x8C\xA1", pixel_size);
}
