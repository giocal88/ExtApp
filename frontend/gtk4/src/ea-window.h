/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <adwaita.h>
#include "ea-objects.h"

G_BEGIN_DECLS

#define EA_TYPE_WINDOW (ea_window_get_type())
G_DECLARE_FINAL_TYPE(EaWindow, ea_window, EA, WINDOW, AdwApplicationWindow)

EaWindow   *ea_window_new(AdwApplication *app);
void        ea_window_toast(EaWindow *self, const char *msg);

/* Shared model of existing shortcuts (GListStore of EaEntry). It is updated
 * in place on reload, so connect to "items-changed" to follow it. */
GListModel *ea_window_get_entries(EaWindow *self);
void        ea_window_reload_entries(EaWindow *self);
/* Best shortcut launching `real_path` (managed ones first), or NULL. */
EaEntry    *ea_window_find_entry_for_target(EaWindow *self, const char *real_path);

/* Find the window a widget lives in (walks up through dialogs). */
EaWindow   *ea_window_from_widget(GtkWidget *w);

G_END_DECLS
