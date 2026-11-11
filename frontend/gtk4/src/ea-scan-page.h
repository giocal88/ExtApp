/* Page: choose a folder, scan it, create shortcuts for found programs.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <adwaita.h>
#include "ea-window.h"

G_BEGIN_DECLS

#define EA_TYPE_SCAN_PAGE (ea_scan_page_get_type())
G_DECLARE_FINAL_TYPE(EaScanPage, ea_scan_page, EA, SCAN_PAGE, AdwBin)

EaScanPage *ea_scan_page_new(EaWindow *window);

G_END_DECLS
