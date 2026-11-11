/* Dialog searching icons online and returning the chosen image bytes.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define EA_TYPE_ICON_SEARCH_DIALOG (ea_icon_search_dialog_get_type())
G_DECLARE_FINAL_TYPE(EaIconSearchDialog, ea_icon_search_dialog, EA,
                     ICON_SEARCH_DIALOG, AdwDialog)

/* Emits "icon-chosen" (GBytes *image, PNG or SVG) and closes itself. The
 * search starts immediately with `query`. */
EaIconSearchDialog *ea_icon_search_dialog_new(const char *query);

G_END_DECLS
