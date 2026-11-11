/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ea-objects.h"

/* ---- EaCandidate ------------------------------------------------------- */

struct _EaCandidate {
    GObject parent_instance;
    char *path, *real_path, *name, *desktop_file;
    ExtAppKind kind;
    gboolean is_executable;
};

enum { CAND_PROP_0, CAND_PROP_DESKTOP_FILE, CAND_PROP_IS_EXECUTABLE, CAND_N_PROPS };
static GParamSpec *cand_props[CAND_N_PROPS];

G_DEFINE_FINAL_TYPE(EaCandidate, ea_candidate, G_TYPE_OBJECT)

static void ea_candidate_finalize(GObject *obj)
{
    EaCandidate *self = EA_CANDIDATE(obj);
    g_free(self->path);
    g_free(self->real_path);
    g_free(self->name);
    g_free(self->desktop_file);
    G_OBJECT_CLASS(ea_candidate_parent_class)->finalize(obj);
}

static void ea_candidate_get_property(GObject *obj, guint id, GValue *v,
                                      GParamSpec *pspec)
{
    EaCandidate *self = EA_CANDIDATE(obj);
    switch (id) {
    case CAND_PROP_DESKTOP_FILE: g_value_set_string(v, self->desktop_file); break;
    case CAND_PROP_IS_EXECUTABLE: g_value_set_boolean(v, self->is_executable); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void ea_candidate_class_init(EaCandidateClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->finalize = ea_candidate_finalize;
    oc->get_property = ea_candidate_get_property;
    cand_props[CAND_PROP_DESKTOP_FILE] = g_param_spec_string(
        "desktop-file", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    cand_props[CAND_PROP_IS_EXECUTABLE] = g_param_spec_boolean(
        "is-executable", NULL, NULL, FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties(oc, CAND_N_PROPS, cand_props);
}

static void ea_candidate_init(EaCandidate *self) { (void)self; }

EaCandidate *ea_candidate_new(const ExtAppCandidate *c)
{
    EaCandidate *self = g_object_new(EA_TYPE_CANDIDATE, NULL);
    self->path = g_strdup(c->path);
    self->real_path = g_strdup(c->real_path);
    self->name = g_strdup(c->suggested_name);
    self->desktop_file = g_strdup(c->desktop_file);
    self->kind = c->kind;
    self->is_executable = c->is_executable;
    return self;
}

const char *ea_candidate_get_path(EaCandidate *self) { return self->path; }
const char *ea_candidate_get_real_path(EaCandidate *self) { return self->real_path; }
const char *ea_candidate_get_name(EaCandidate *self) { return self->name; }
ExtAppKind ea_candidate_get_kind(EaCandidate *self) { return self->kind; }
gboolean ea_candidate_get_is_executable(EaCandidate *self) { return self->is_executable; }
const char *ea_candidate_get_desktop_file(EaCandidate *self) { return self->desktop_file; }

void ea_candidate_set_is_executable(EaCandidate *self, gboolean v)
{
    if (self->is_executable == v)
        return;
    self->is_executable = v;
    g_object_notify_by_pspec(G_OBJECT(self), cand_props[CAND_PROP_IS_EXECUTABLE]);
}

void ea_candidate_set_desktop_file(EaCandidate *self, const char *path)
{
    if (g_strcmp0(self->desktop_file, path) == 0)
        return;
    g_free(self->desktop_file);
    self->desktop_file = g_strdup(path);
    g_object_notify_by_pspec(G_OBJECT(self), cand_props[CAND_PROP_DESKTOP_FILE]);
}

/* ---- EaEntry ----------------------------------------------------------- */

struct _EaEntry {
    GObject parent_instance;
    char *file_path, *id, *name, *exec, *args, *icon, *comment, *categories,
         *target_path;
    gboolean terminal, no_display, managed, target_exists;
};

G_DEFINE_FINAL_TYPE(EaEntry, ea_entry, G_TYPE_OBJECT)

static void ea_entry_finalize(GObject *obj)
{
    EaEntry *self = EA_ENTRY(obj);
    g_free(self->file_path);
    g_free(self->id);
    g_free(self->name);
    g_free(self->exec);
    g_free(self->args);
    g_free(self->icon);
    g_free(self->comment);
    g_free(self->categories);
    g_free(self->target_path);
    G_OBJECT_CLASS(ea_entry_parent_class)->finalize(obj);
}

static void ea_entry_class_init(EaEntryClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = ea_entry_finalize;
}

static void ea_entry_init(EaEntry *self) { (void)self; }

EaEntry *ea_entry_new(const ExtAppEntry *e)
{
    EaEntry *self = g_object_new(EA_TYPE_ENTRY, NULL);
    self->file_path = g_strdup(e->file_path);
    self->id = g_strdup(e->id);
    self->name = g_strdup(e->name);
    self->exec = g_strdup(e->exec);
    self->args = g_strdup(e->args);
    self->icon = g_strdup(e->icon);
    self->comment = g_strdup(e->comment);
    self->categories = g_strdup(e->categories);
    self->target_path = g_strdup(e->target_path);
    self->terminal = e->terminal;
    self->no_display = e->no_display;
    self->managed = e->managed;
    self->target_exists = e->target_exists;
    return self;
}

const char *ea_entry_get_file_path(EaEntry *self) { return self->file_path; }
const char *ea_entry_get_id(EaEntry *self) { return self->id; }
const char *ea_entry_get_name(EaEntry *self) { return self->name; }
const char *ea_entry_get_exec(EaEntry *self) { return self->exec; }
const char *ea_entry_get_args(EaEntry *self) { return self->args; }
const char *ea_entry_get_icon(EaEntry *self) { return self->icon; }
const char *ea_entry_get_comment(EaEntry *self) { return self->comment; }
const char *ea_entry_get_categories(EaEntry *self) { return self->categories; }
const char *ea_entry_get_target_path(EaEntry *self) { return self->target_path; }
gboolean ea_entry_get_terminal(EaEntry *self) { return self->terminal; }
gboolean ea_entry_get_no_display(EaEntry *self) { return self->no_display; }
gboolean ea_entry_get_managed(EaEntry *self) { return self->managed; }
gboolean ea_entry_get_target_exists(EaEntry *self) { return self->target_exists; }

/* ---- EaIconResult ------------------------------------------------------ */

struct _EaIconResult {
    GObject parent_instance;
    char *image_url, *thumbnail_url, *title;
    int width, height;
};

G_DEFINE_FINAL_TYPE(EaIconResult, ea_icon_result, G_TYPE_OBJECT)

static void ea_icon_result_finalize(GObject *obj)
{
    EaIconResult *self = EA_ICON_RESULT(obj);
    g_free(self->image_url);
    g_free(self->thumbnail_url);
    g_free(self->title);
    G_OBJECT_CLASS(ea_icon_result_parent_class)->finalize(obj);
}

static void ea_icon_result_class_init(EaIconResultClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = ea_icon_result_finalize;
}

static void ea_icon_result_init(EaIconResult *self) { (void)self; }

EaIconResult *ea_icon_result_new(const ExtAppIconResult *r)
{
    EaIconResult *self = g_object_new(EA_TYPE_ICON_RESULT, NULL);
    self->image_url = g_strdup(r->image_url);
    self->thumbnail_url = g_strdup(r->thumbnail_url);
    self->title = g_strdup(r->title);
    self->width = r->width;
    self->height = r->height;
    return self;
}

const char *ea_icon_result_get_image_url(EaIconResult *self) { return self->image_url; }
const char *ea_icon_result_get_thumbnail_url(EaIconResult *self) { return self->thumbnail_url; }
const char *ea_icon_result_get_title(EaIconResult *self) { return self->title; }
int ea_icon_result_get_width(EaIconResult *self) { return self->width; }
int ea_icon_result_get_height(EaIconResult *self) { return self->height; }
