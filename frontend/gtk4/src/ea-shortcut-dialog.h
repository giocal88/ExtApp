/* Dialog to create a new shortcut or edit an ExtApp-managed one.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <adwaita.h>
#include "ea-objects.h"

G_BEGIN_DECLS

#define EA_TYPE_SHORTCUT_DIALOG (ea_shortcut_dialog_get_type())
G_DECLARE_FINAL_TYPE(EaShortcutDialog, ea_shortcut_dialog, EA, SHORTCUT_DIALOG,
                     AdwDialog)

/* New shortcut for a scanned program. `existing` (nullable) is a shortcut that
 * already launches it: if managed, the dialog edits it; otherwise a notice is
 * shown. */
EaShortcutDialog *ea_shortcut_dialog_new_for_candidate(EaCandidate *c,
                                                       EaEntry *existing);
/* Edit an existing ExtApp-managed shortcut. */
EaShortcutDialog *ea_shortcut_dialog_new_for_entry(EaEntry *entry);

/* Signal "saved" (const char *desktop_file_path) is emitted on success. */

G_END_DECLS
