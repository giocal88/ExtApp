/* Image + settings helpers for the GTK frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Bytes ready to be stored as an icon: SVG is kept as-is, everything else is
 * decoded and re-encoded as PNG, scaled down to fit `max_size`. */
GBytes     *ea_image_normalize(GBytes *in, int max_size, GError **error);
/* Decode any supported image (incl. SVG) into a texture of at most max_size. */
GdkTexture *ea_image_texture(GBytes *in, int max_size, GError **error);
gboolean    ea_image_is_svg(GBytes *in);

/* Show an Icon= value (icon name or absolute path) in a GtkImage. */
void ea_image_set_icon(GtkImage *image, const char *icon, const char *fallback);

/* Tiny persistent settings (~/.config/extapp/settings.ini). */
char    *ea_settings_get_string(const char *key);
void     ea_settings_set_string(const char *key, const char *value);
int      ea_settings_get_int(const char *key, int def);
void     ea_settings_set_int(const char *key, int value);

/* Small coloured label ("pill") used as a status badge. */
GtkWidget *ea_pill_new(const char *text, const char *style /* nullable */);

G_END_DECLS
