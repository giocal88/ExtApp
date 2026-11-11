/* GObject wrappers around backend structs, for use in GListModels.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <glib-object.h>
#include <extapp/extapp.h>

G_BEGIN_DECLS

/* ---- EaCandidate: a program found by a folder scan --------------------- */

#define EA_TYPE_CANDIDATE (ea_candidate_get_type())
G_DECLARE_FINAL_TYPE(EaCandidate, ea_candidate, EA, CANDIDATE, GObject)

EaCandidate *ea_candidate_new(const ExtAppCandidate *c);
const char  *ea_candidate_get_path(EaCandidate *self);
const char  *ea_candidate_get_real_path(EaCandidate *self);
const char  *ea_candidate_get_name(EaCandidate *self);
ExtAppKind   ea_candidate_get_kind(EaCandidate *self);
gboolean     ea_candidate_get_is_executable(EaCandidate *self);
void         ea_candidate_set_is_executable(EaCandidate *self, gboolean v);
const char  *ea_candidate_get_desktop_file(EaCandidate *self); /* nullable */
void         ea_candidate_set_desktop_file(EaCandidate *self, const char *path);

/* ---- EaEntry: an existing .desktop shortcut --------------------------- */

#define EA_TYPE_ENTRY (ea_entry_get_type())
G_DECLARE_FINAL_TYPE(EaEntry, ea_entry, EA, ENTRY, GObject)

EaEntry    *ea_entry_new(const ExtAppEntry *e);
const char *ea_entry_get_file_path(EaEntry *self);
const char *ea_entry_get_id(EaEntry *self);
const char *ea_entry_get_name(EaEntry *self);
const char *ea_entry_get_exec(EaEntry *self);
const char *ea_entry_get_args(EaEntry *self);
const char *ea_entry_get_icon(EaEntry *self);
const char *ea_entry_get_comment(EaEntry *self);
const char *ea_entry_get_categories(EaEntry *self);
const char *ea_entry_get_target_path(EaEntry *self);
gboolean    ea_entry_get_terminal(EaEntry *self);
gboolean    ea_entry_get_no_display(EaEntry *self);
gboolean    ea_entry_get_managed(EaEntry *self);
gboolean    ea_entry_get_target_exists(EaEntry *self);

/* ---- EaIconResult: an online icon search hit -------------------------- */

#define EA_TYPE_ICON_RESULT (ea_icon_result_get_type())
G_DECLARE_FINAL_TYPE(EaIconResult, ea_icon_result, EA, ICON_RESULT, GObject)

EaIconResult *ea_icon_result_new(const ExtAppIconResult *r);
const char   *ea_icon_result_get_provider(EaIconResult *self);
const char   *ea_icon_result_get_image_url(EaIconResult *self);
const char   *ea_icon_result_get_thumbnail_url(EaIconResult *self);
const char   *ea_icon_result_get_title(EaIconResult *self);
int           ea_icon_result_get_width(EaIconResult *self);
int           ea_icon_result_get_height(EaIconResult *self);
gboolean      ea_icon_result_get_scalable(EaIconResult *self);

G_END_DECLS
