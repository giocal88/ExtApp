/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ea-util.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>

/* ---- images ------------------------------------------------------------ */

gboolean ea_image_is_svg(GBytes *in)
{
    gsize n = 0;
    const char *d = g_bytes_get_data(in, &n);
    gsize lim = MIN(n, 4096);
    gsize i = 0;
    if (n >= 3 && memcmp(d, "\xEF\xBB\xBF", 3) == 0)
        i = 3;
    while (i < lim && g_ascii_isspace(d[i]))
        i++;
    if (i >= lim || d[i] != '<')
        return FALSE;
    for (; i + 4 <= lim; i++)
        if (memcmp(d + i, "<svg", 4) == 0)
            return TRUE;
    return FALSE;
}

static void on_size_prepared(GdkPixbufLoader *loader, int w, int h, gpointer ud)
{
    int max = GPOINTER_TO_INT(ud);
    if (w <= 0 || h <= 0)
        return;
    if (w <= max && h <= max) {
        /* vector images may report a tiny default size: render them big */
        return;
    }
    double s = MIN((double)max / w, (double)max / h);
    gdk_pixbuf_loader_set_size(loader, MAX(1, (int)(w * s)), MAX(1, (int)(h * s)));
}

static GBytes *rasterize_png(GBytes *in, int max_size, GError **error)
{
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    g_signal_connect(loader, "size-prepared", G_CALLBACK(on_size_prepared),
                     GINT_TO_POINTER(max_size));
    gsize n = 0;
    const guchar *d = g_bytes_get_data(in, &n);
    if (!gdk_pixbuf_loader_write(loader, d, n, error)) {
        gdk_pixbuf_loader_close(loader, NULL);
        g_object_unref(loader);
        return NULL;
    }
    if (!gdk_pixbuf_loader_close(loader, error)) {
        g_object_unref(loader);
        return NULL;
    }
    GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf(loader);
    if (!pb) {
        g_set_error_literal(error, GDK_PIXBUF_ERROR,
                            GDK_PIXBUF_ERROR_UNKNOWN_TYPE,
                            "Unsupported image format");
        g_object_unref(loader);
        return NULL;
    }
    gchar *buf = NULL;
    gsize len = 0;
    gboolean ok = gdk_pixbuf_save_to_buffer(pb, &buf, &len, "png", error, NULL);
    g_object_unref(loader);
    return ok ? g_bytes_new_take(buf, len) : NULL;
}

GBytes *ea_image_normalize(GBytes *in, int max_size, GError **error)
{
    if (ea_image_is_svg(in))
        return g_bytes_ref(in);
    return rasterize_png(in, max_size, error);
}

GdkTexture *ea_image_texture(GBytes *in, int max_size, GError **error)
{
    GBytes *png = rasterize_png(in, max_size, error);
    if (!png)
        return NULL;
    GdkTexture *t = gdk_texture_new_from_bytes(png, error);
    g_bytes_unref(png);
    return t;
}

void ea_image_set_icon(GtkImage *image, const char *icon, const char *fallback)
{
    if (icon && icon[0] == '/' && g_file_test(icon, G_FILE_TEST_IS_REGULAR)) {
        GFile *f = g_file_new_for_path(icon);
        GBytes *b = g_file_load_bytes(f, NULL, NULL, NULL);
        g_object_unref(f);
        GdkTexture *t = b ? ea_image_texture(b, 128, NULL) : NULL;
        if (b)
            g_bytes_unref(b);
        if (t) {
            gtk_image_set_from_paintable(image, GDK_PAINTABLE(t));
            g_object_unref(t);
            return;
        }
    } else if (icon && *icon && icon[0] != '/') {
        GtkIconTheme *theme = gtk_icon_theme_get_for_display(
            gtk_widget_get_display(GTK_WIDGET(image)));
        if (gtk_icon_theme_has_icon(theme, icon)) {
            gtk_image_set_from_icon_name(image, icon);
            return;
        }
    }
    gtk_image_set_from_icon_name(image, fallback ? fallback
                                                 : "application-x-executable");
}

/* ---- settings ---------------------------------------------------------- */

static char *settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "extapp", "settings.ini",
                            NULL);
}

static GKeyFile *settings_load(void)
{
    GKeyFile *kf = g_key_file_new();
    char *p = settings_path();
    g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, NULL);
    g_free(p);
    return kf;
}

static void settings_save(GKeyFile *kf)
{
    char *p = settings_path();
    char *dir = g_path_get_dirname(p);
    g_mkdir_with_parents(dir, 0755);
    g_key_file_save_to_file(kf, p, NULL);
    g_free(dir);
    g_free(p);
}

char *ea_settings_get_string(const char *key)
{
    GKeyFile *kf = settings_load();
    char *v = g_key_file_get_string(kf, "General", key, NULL);
    g_key_file_unref(kf);
    return v;
}

void ea_settings_set_string(const char *key, const char *value)
{
    GKeyFile *kf = settings_load();
    g_key_file_set_string(kf, "General", key, value ? value : "");
    settings_save(kf);
    g_key_file_unref(kf);
}

int ea_settings_get_int(const char *key, int def)
{
    GKeyFile *kf = settings_load();
    GError *e = NULL;
    int v = g_key_file_get_integer(kf, "General", key, &e);
    if (e) {
        v = def;
        g_error_free(e);
    }
    g_key_file_unref(kf);
    return v;
}

void ea_settings_set_int(const char *key, int value)
{
    GKeyFile *kf = settings_load();
    g_key_file_set_integer(kf, "General", key, value);
    settings_save(kf);
    g_key_file_unref(kf);
}

/* ---- widgets ----------------------------------------------------------- */

GtkWidget *ea_pill_new(const char *text, const char *style)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_widget_add_css_class(l, "ea-pill");
    if (style)
        gtk_widget_add_css_class(l, style);
    gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
    return l;
}
