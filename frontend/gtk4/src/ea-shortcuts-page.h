/* Page: list, edit and remove existing .desktop shortcuts.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <adwaita.h>
#include "ea-window.h"

G_BEGIN_DECLS

#define EA_TYPE_SHORTCUTS_PAGE (ea_shortcuts_page_get_type())
G_DECLARE_FINAL_TYPE(EaShortcutsPage, ea_shortcuts_page, EA, SHORTCUTS_PAGE, AdwBin)

EaShortcutsPage *ea_shortcuts_page_new(EaWindow *window);

G_END_DECLS
