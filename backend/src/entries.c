/* Listing, creating and removing .desktop shortcuts.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "private.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ENTRIES_MAX_DEPTH 3

void ea_entry_clear(ExtAppEntry *e)
{
    free(e->file_path);
    free(e->id);
    free(e->name);
    free(e->exec);
    free(e->args);
    free(e->icon);
    free(e->comment);
    free(e->categories);
    free(e->target_path);
    memset(e, 0, sizeof *e);
}

void extapp_entry_list_free(ExtAppEntryList *list)
{
    if (!list)
        return;
    for (size_t i = 0; i < list->count; i++)
        ea_entry_clear(&list->items[i]);
    free(list->items);
    free(list);
}

/* ---- listing ----------------------------------------------------------- */

typedef struct {
    ExtAppEntryList *list;
    size_t cap;
} ListState;

static ExtAppStatus load_entry(ListState *s, const char *path, const char *id)
{
    EaDesktopEntry de;
    if (ea_de_parse_file(path, &de, NULL) != EXTAPP_OK)
        return EXTAPP_OK; /* unreadable or broken file: ignore */

    const char *type = ea_de_get(&de, "Type");
    if (type && strcmp(type, "Application") != 0) {
        ea_de_clear(&de);
        return EXTAPP_OK;
    }

    if (s->list->count == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 32;
        ExtAppEntry *items = realloc(s->list->items, nc * sizeof *items);
        if (!items) {
            ea_de_clear(&de);
            return EXTAPP_ERR_NOMEM;
        }
        s->list->items = items;
        s->cap = nc;
    }
    ExtAppEntry *e = &s->list->items[s->list->count];
    memset(e, 0, sizeof *e);
    e->file_path = strdup(path);
    e->id = strdup(id);
    const char *name = ea_de_get(&de, "Name");
    e->name = strdup(name ? name : id);
    e->exec = ea_strdup0(ea_de_get(&de, "Exec"));
    e->args = ea_strdup0(ea_de_get(&de, "X-ExtApp-Args"));
    e->icon = ea_strdup0(ea_de_get(&de, "Icon"));
    e->comment = ea_strdup0(ea_de_get(&de, "Comment"));
    e->categories = ea_strdup0(ea_de_get(&de, "Categories"));
    e->terminal = ea_de_get_bool(&de, "Terminal");
    e->no_display = ea_de_get_bool(&de, "NoDisplay") ||
                    ea_de_get_bool(&de, "Hidden");
    e->managed = ea_de_get_bool(&de, "X-ExtApp-Managed");
    e->target_path = ea_de_resolve_target(&de);
    e->target_exists = e->target_path && access(e->target_path, F_OK) == 0;
    ea_de_clear(&de);

    if (!e->file_path || !e->id || !e->name) {
        ea_entry_clear(e);
        return EXTAPP_ERR_NOMEM;
    }
    s->list->count++;
    return EXTAPP_OK;
}

static ExtAppStatus list_dir(ListState *s, const char *dir, const char *prefix,
                             int depth)
{
    DIR *d = opendir(dir);
    if (!d)
        return EXTAPP_OK;
    ExtAppStatus st = EXTAPP_OK;
    struct dirent *de;
    while (st == EXTAPP_OK && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        char *full = ea_path_join(dir, de->d_name);
        if (!full) {
            st = EXTAPP_ERR_NOMEM;
            break;
        }
        struct stat sb;
        if (stat(full, &sb) == 0) {
            /* desktop file id: sub/dir/foo.desktop -> sub-dir-foo.desktop */
            char *id = ea_strdup_printf("%s%s", prefix, de->d_name);
            if (!id) {
                st = EXTAPP_ERR_NOMEM;
            } else if (S_ISDIR(sb.st_mode) && depth < ENTRIES_MAX_DEPTH) {
                char *sub = ea_strdup_printf("%s-", id);
                st = sub ? list_dir(s, full, sub, depth + 1) : EXTAPP_ERR_NOMEM;
                free(sub);
            } else if (S_ISREG(sb.st_mode) &&
                       ea_has_suffix_ci(de->d_name, ".desktop")) {
                st = load_entry(s, full, id);
            }
            free(id);
        }
        free(full);
    }
    closedir(d);
    return st;
}

static int cmp_entries(const void *a, const void *b)
{
    const ExtAppEntry *x = a, *y = b;
    int r = strcasecmp(x->name, y->name);
    return r ? r : strcmp(x->file_path, y->file_path);
}

ExtAppStatus ea_list_entries(ExtAppContext *ctx, ExtAppEntryList *out,
                             char **err)
{
    memset(out, 0, sizeof *out);
    ListState s = {.list = out};
    ExtAppStatus st = list_dir(&s, ctx->applications_dir, "", 0);
    if (st != EXTAPP_OK) {
        for (size_t i = 0; i < out->count; i++)
            ea_entry_clear(&out->items[i]);
        free(out->items);
        memset(out, 0, sizeof *out);
        ea_set_err(err, "Out of memory while listing shortcuts");
        return st;
    }
    if (out->count > 1)
        qsort(out->items, out->count, sizeof *out->items, cmp_entries);
    return EXTAPP_OK;
}

ExtAppStatus extapp_list_entries(ExtAppContext *ctx, ExtAppEntryList **out,
                                 char **err)
{
    if (!ctx || !out) {
        ea_set_err(err, "Invalid arguments");
        return EXTAPP_ERR_INVALID_ARG;
    }
    *out = calloc(1, sizeof **out);
    if (!*out)
        return EXTAPP_ERR_NOMEM;
    ExtAppStatus st = ea_list_entries(ctx, *out, err);
    if (st != EXTAPP_OK) {
        free(*out);
        *out = NULL;
    }
    return st;
}

/* ---- creating ---------------------------------------------------------- */

static bool add_kv(EaStrBuf *sb, const char *key, const char *value)
{
    if (!value || !*value)
        return true;
    char *esc = ea_de_escape_value(value);
    if (!esc)
        return false;
    bool ok = ea_sb_printf(sb, "%s=%s\n", key, esc);
    free(esc);
    return ok;
}

static bool has_control_chars(const char *s)
{
    for (; s && *s; s++)
        if ((unsigned char)*s < 0x20 && *s != '\t')
            return true;
    return false;
}

static char *build_desktop_file(const ExtAppEntrySpec *spec,
                                const char *target)
{
    char *prog = ea_exec_quote_arg(target);
    char *exec = NULL;
    if (prog) {
        if (spec->args && *spec->args)
            exec = ea_strdup_printf("%s %s", prog, spec->args);
        else
            exec = strdup(prog);
    }
    free(prog);
    char *workdir = ea_dirname(target);

    char *cats = NULL;
    if (spec->categories && *spec->categories) {
        size_t l = strlen(spec->categories);
        cats = spec->categories[l - 1] == ';'
                   ? strdup(spec->categories)
                   : ea_strdup_printf("%s;", spec->categories);
    }

    EaStrBuf sb = {0};
    bool ok = exec && workdir && (cats || !spec->categories || !*spec->categories);
    ok = ok && ea_sb_append(&sb, "[Desktop Entry]\n"
                                 "Type=Application\n"
                                 "Version=1.5\n");
    ok = ok && add_kv(&sb, "Name", spec->name);
    ok = ok && add_kv(&sb, "Comment", spec->comment);
    ok = ok && add_kv(&sb, "Exec", exec);
    ok = ok && add_kv(&sb, "TryExec", target);
    ok = ok && add_kv(&sb, "Path", workdir);
    ok = ok && add_kv(&sb, "Icon", spec->icon);
    ok = ok && add_kv(&sb, "Categories", cats);
    ok = ok && ea_sb_printf(&sb, "Terminal=%s\n",
                            spec->terminal ? "true" : "false");
    ok = ok && ea_sb_append(&sb, "StartupNotify=true\n"
                                 "X-ExtApp-Managed=true\n");
    ok = ok && add_kv(&sb, "X-ExtApp-Target", target);
    ok = ok && add_kv(&sb, "X-ExtApp-Args", spec->args);

    free(exec);
    free(workdir);
    free(cats);
    if (!ok) {
        ea_sb_clear(&sb);
        return NULL;
    }
    return ea_sb_steal(&sb);
}

ExtAppStatus extapp_entry_create(ExtAppContext *ctx,
                                 const ExtAppEntrySpec *spec, unsigned flags,
                                 char **out_path, char **err)
{
    if (out_path)
        *out_path = NULL;
    if (!ctx || !spec || !spec->name || !*spec->name || !spec->target_path ||
        !*spec->target_path) {
        ea_set_err(err, "A name and a target program are required");
        return EXTAPP_ERR_INVALID_ARG;
    }
    if (has_control_chars(spec->name) || has_control_chars(spec->args) ||
        has_control_chars(spec->icon) || has_control_chars(spec->categories)) {
        ea_set_err(err, "Fields must not contain line breaks");
        return EXTAPP_ERR_INVALID_ARG;
    }

    char *target = realpath(spec->target_path, NULL);
    struct stat st;
    if (!target || stat(target, &st) != 0 || !S_ISREG(st.st_mode)) {
        ea_set_err(err, "%s is not an existing file", spec->target_path);
        free(target);
        return EXTAPP_ERR_NOT_FOUND;
    }

    ExtAppStatus rc = ea_mkdir_p(ctx->applications_dir, err);
    if (rc != EXTAPP_OK) {
        free(target);
        return rc;
    }

    /* Is there already a managed shortcut for this program? */
    char *path = NULL;
    ExtAppEntryList entries;
    rc = ea_list_entries(ctx, &entries, err);
    if (rc != EXTAPP_OK) {
        free(target);
        return rc;
    }
    for (size_t i = 0; i < entries.count && !path; i++) {
        const ExtAppEntry *e = &entries.items[i];
        if (e->managed && e->target_path && strcmp(e->target_path, target) == 0)
            path = strdup(e->file_path);
    }
    for (size_t i = 0; i < entries.count; i++)
        ea_entry_clear(&entries.items[i]);
    free(entries.items);

    if (path && !(flags & EXTAPP_CREATE_OVERWRITE)) {
        ea_set_err(err, "A shortcut for %s already exists", target);
        if (out_path)
            *out_path = path;
        else
            free(path);
        free(target);
        return EXTAPP_ERR_EXISTS;
    }

    if (!path) {
        char *slug = ea_slugify(spec->name);
        for (int i = 1; slug && i < 1000 && !path; i++) {
            char *file = i == 1 ? ea_strdup_printf("extapp-%s.desktop", slug)
                                : ea_strdup_printf("extapp-%s-%d.desktop", slug, i);
            char *full = file ? ea_path_join(ctx->applications_dir, file) : NULL;
            free(file);
            if (full && access(full, F_OK) != 0 && errno == ENOENT)
                path = full;
            else
                free(full);
        }
        free(slug);
        if (!path) {
            ea_set_err(err, "Could not find a free file name for the shortcut");
            free(target);
            return EXTAPP_ERR_IO;
        }
    }

    if ((flags & EXTAPP_CREATE_MAKE_EXECUTABLE) && !(st.st_mode & S_IXUSR)) {
        mode_t m = st.st_mode & 07777;
        m |= S_IXUSR;
        if (m & S_IRGRP)
            m |= S_IXGRP;
        if (m & S_IROTH)
            m |= S_IXOTH;
        if (chmod(target, m) != 0) {
            ea_set_err(err, "Cannot make %s executable: %s", target,
                       strerror(errno));
            free(target);
            free(path);
            return EXTAPP_ERR_IO;
        }
    }

    char *content = build_desktop_file(spec, target);
    free(target);
    if (!content) {
        free(path);
        return EXTAPP_ERR_NOMEM;
    }
    rc = ea_write_file_atomic(path, content, strlen(content), 0644, err);
    free(content);
    if (rc == EXTAPP_OK && out_path)
        *out_path = path;
    else
        free(path);
    return rc;
}

/* ---- removing ---------------------------------------------------------- */

ExtAppStatus extapp_entry_remove(ExtAppContext *ctx, const char *desktop_path,
                                 unsigned flags, char **err)
{
    if (!ctx || !desktop_path || !ea_has_suffix_ci(desktop_path, ".desktop")) {
        ea_set_err(err, "Not a .desktop file");
        return EXTAPP_ERR_INVALID_ARG;
    }

    char *real = realpath(desktop_path, NULL);
    char *appdir = realpath(ctx->applications_dir, NULL);
    if (!real || !appdir) {
        ea_set_err(err, "%s does not exist", desktop_path);
        free(real);
        free(appdir);
        return EXTAPP_ERR_NOT_FOUND;
    }
    /* Only delete files living in the user's applications dir. We check the
     * link itself (not its resolved target) because users may symlink. */
    char *linkdir = ea_dirname(desktop_path);
    char *reallinkdir = linkdir ? realpath(linkdir, NULL) : NULL;
    free(linkdir);
    bool inside = reallinkdir && (strcmp(reallinkdir, appdir) == 0 ||
                                  ea_path_is_inside(reallinkdir, appdir));
    free(reallinkdir);
    free(appdir);
    if (!inside) {
        ea_set_err(err, "%s is outside %s", desktop_path, ctx->applications_dir);
        free(real);
        return EXTAPP_ERR_INVALID_ARG;
    }

    EaDesktopEntry de;
    ExtAppStatus rc = ea_de_parse_file(real, &de, err);
    free(real);
    bool managed = false;
    char *icon = NULL;
    if (rc == EXTAPP_OK) {
        managed = ea_de_get_bool(&de, "X-ExtApp-Managed");
        icon = ea_strdup0(ea_de_get(&de, "Icon"));
        ea_de_clear(&de);
    } else if (!(flags & EXTAPP_REMOVE_ALLOW_UNMANAGED)) {
        return rc;
    }

    if (!managed && !(flags & EXTAPP_REMOVE_ALLOW_UNMANAGED)) {
        ea_set_err(err, "%s was not created by ExtApp", desktop_path);
        free(icon);
        return EXTAPP_ERR_NOT_MANAGED;
    }

    if (unlink(desktop_path) != 0) {
        ea_set_err(err, "Cannot delete %s: %s", desktop_path, strerror(errno));
        free(icon);
        return EXTAPP_ERR_IO;
    }

    if ((flags & EXTAPP_REMOVE_DELETE_ICON) && icon && icon[0] == '/') {
        char *ricon = realpath(icon, NULL);
        char *ricons_dir = realpath(ctx->icons_dir, NULL);
        if (ricon && ricons_dir && ea_path_is_inside(ricon, ricons_dir))
            unlink(ricon); /* best effort */
        free(ricon);
        free(ricons_dir);
    }
    free(icon);
    return EXTAPP_OK;
}
