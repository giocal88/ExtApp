/* extapp-cli — command line frontend for libextapp.
 * Mostly a demonstration that the backend is frontend-agnostic, and a handy
 * tool for scripting / testing.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <extapp/extapp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *f)
{
    fprintf(f,
        "Usage: extapp-cli <command> [options]\n"
        "\n"
        "Commands:\n"
        "  scan DIR [--depth N] [--scripts] [--hidden]\n"
        "                          list launchable programs in DIR\n"
        "  list                    list shortcuts in the applications folder\n"
        "  create PATH --name NAME [--args ARGS] [--icon ICON] [--comment TEXT]\n"
        "         [--categories CATS] [--terminal] [--chmod] [--overwrite]\n"
        "                          create a shortcut for PATH\n"
        "  remove FILE.desktop [--force] [--delete-icon]\n"
        "                          remove a shortcut (--force: even if not\n"
        "                          created by ExtApp)\n"
        "  icons QUERY [--max N]   search icons online (DuckDuckGo)\n"
        "  fetch-icon URL NAME     download an image and store it as icon\n"
        "\n"
        "Applications folder: $XDG_DATA_HOME/applications "
        "(default ~/.local/share/applications)\n");
}

static int fail(ExtAppStatus st, char *err)
{
    fprintf(stderr, "error: %s\n", err ? err : extapp_status_str(st));
    free(err);
    return 1;
}

static const char *opt_value(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "error: %s needs a value\n", argv[*i]);
        exit(2);
    }
    return argv[++*i];
}

static int cmd_scan(ExtAppContext *ctx, int argc, char **argv)
{
    if (argc < 1) {
        usage(stderr);
        return 2;
    }
    ExtAppScanOptions o;
    extapp_scan_options_init(&o);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--depth") == 0)
            o.max_depth = atoi(opt_value(argc, argv, &i));
        else if (strcmp(argv[i], "--scripts") == 0)
            o.include_scripts = true;
        else if (strcmp(argv[i], "--hidden") == 0)
            o.include_hidden = true;
        else {
            fprintf(stderr, "error: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    ExtAppCandidateList *l = NULL;
    char *err = NULL;
    ExtAppStatus st = extapp_scan(ctx, argv[0], &o, NULL, NULL, NULL, &l, &err);
    if (st != EXTAPP_OK)
        return fail(st, err);
    for (size_t i = 0; i < l->count; i++) {
        const ExtAppCandidate *c = &l->items[i];
        printf("%-24s %-10s %s%s\n    shortcut: %s\n", c->suggested_name,
               extapp_kind_str(c->kind), c->path,
               c->is_executable ? "" : "  (not executable)",
               c->desktop_file ? c->desktop_file : "-");
    }
    printf("%zu program(s) found\n", l->count);
    extapp_candidate_list_free(l);
    return 0;
}

static int cmd_list(ExtAppContext *ctx)
{
    ExtAppEntryList *l = NULL;
    char *err = NULL;
    ExtAppStatus st = extapp_list_entries(ctx, &l, &err);
    if (st != EXTAPP_OK)
        return fail(st, err);
    for (size_t i = 0; i < l->count; i++) {
        const ExtAppEntry *e = &l->items[i];
        printf("%s%-24s %s\n    exec: %s\n    target: %s%s\n",
               e->managed ? "* " : "  ", e->name, e->file_path,
               e->exec ? e->exec : "-",
               e->target_path ? e->target_path : "-",
               e->target_path && !e->target_exists ? "  (missing!)" : "");
    }
    printf("%zu shortcut(s) in %s  (* = created by ExtApp)\n", l->count,
           extapp_context_applications_dir(ctx));
    extapp_entry_list_free(l);
    return 0;
}

static int cmd_create(ExtAppContext *ctx, int argc, char **argv)
{
    if (argc < 1) {
        usage(stderr);
        return 2;
    }
    ExtAppEntrySpec spec = {.target_path = argv[0]};
    unsigned flags = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0)
            spec.name = opt_value(argc, argv, &i);
        else if (strcmp(argv[i], "--args") == 0)
            spec.args = opt_value(argc, argv, &i);
        else if (strcmp(argv[i], "--icon") == 0)
            spec.icon = opt_value(argc, argv, &i);
        else if (strcmp(argv[i], "--comment") == 0)
            spec.comment = opt_value(argc, argv, &i);
        else if (strcmp(argv[i], "--categories") == 0)
            spec.categories = opt_value(argc, argv, &i);
        else if (strcmp(argv[i], "--terminal") == 0)
            spec.terminal = true;
        else if (strcmp(argv[i], "--chmod") == 0)
            flags |= EXTAPP_CREATE_MAKE_EXECUTABLE;
        else if (strcmp(argv[i], "--overwrite") == 0)
            flags |= EXTAPP_CREATE_OVERWRITE;
        else {
            fprintf(stderr, "error: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    char *name = NULL;
    if (!spec.name)
        spec.name = name = extapp_suggest_name(argv[0]);

    char *path = NULL, *err = NULL;
    ExtAppStatus st = extapp_entry_create(ctx, &spec, flags, &path, &err);
    free(name);
    if (st == EXTAPP_ERR_EXISTS) {
        fprintf(stderr, "error: %s (%s) — use --overwrite to replace it\n",
                err ? err : "already exists", path ? path : "");
        free(err);
        free(path);
        return 1;
    }
    if (st != EXTAPP_OK)
        return fail(st, err);
    printf("%s\n", path);
    free(path);
    return 0;
}

static int cmd_remove(ExtAppContext *ctx, int argc, char **argv)
{
    if (argc < 1) {
        usage(stderr);
        return 2;
    }
    unsigned flags = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0)
            flags |= EXTAPP_REMOVE_ALLOW_UNMANAGED;
        else if (strcmp(argv[i], "--delete-icon") == 0)
            flags |= EXTAPP_REMOVE_DELETE_ICON;
        else {
            fprintf(stderr, "error: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    char *err = NULL;
    ExtAppStatus st = extapp_entry_remove(ctx, argv[0], flags, &err);
    if (st != EXTAPP_OK)
        return fail(st, err);
    printf("removed %s\n", argv[0]);
    return 0;
}

static int cmd_icons(ExtAppContext *ctx, int argc, char **argv)
{
    if (argc < 1) {
        usage(stderr);
        return 2;
    }
    size_t max = 20;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--max") == 0)
            max = (size_t)atoi(opt_value(argc, argv, &i));
        else {
            fprintf(stderr, "error: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    ExtAppIconResultList *l = NULL;
    char *err = NULL;
    ExtAppStatus st = extapp_icon_search(ctx, argv[0], max, NULL, NULL, &l, &err);
    if (st != EXTAPP_OK)
        return fail(st, err);
    for (size_t i = 0; i < l->count; i++) {
        const ExtAppIconResult *r = &l->items[i];
        printf("%4dx%-4d %s\n          thumb: %s\n", r->width, r->height,
               r->image_url ? r->image_url : "-",
               r->thumbnail_url ? r->thumbnail_url : "-");
    }
    printf("%zu result(s)\n", l->count);
    extapp_icon_result_list_free(l);
    return 0;
}

static int cmd_fetch_icon(ExtAppContext *ctx, int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    unsigned char *data = NULL;
    size_t len = 0;
    char *err = NULL, *path = NULL;
    ExtAppStatus st = extapp_http_fetch(ctx, argv[0], 0, NULL, NULL, &data,
                                        &len, &err);
    if (st != EXTAPP_OK)
        return fail(st, err);
    st = extapp_icon_save(ctx, argv[1], data, len, &path, &err);
    free(data);
    if (st != EXTAPP_OK)
        return fail(st, err);
    printf("%s\n", path);
    free(path);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }
    ExtAppContext *ctx = extapp_context_new();
    if (!ctx) {
        fprintf(stderr, "error: cannot determine the data directory\n");
        return 1;
    }
    const char *cmd = argv[1];
    int sub_argc = argc - 2;
    char **sub_argv = argv + 2;
    int rc;
    if (strcmp(cmd, "scan") == 0)
        rc = cmd_scan(ctx, sub_argc, sub_argv);
    else if (strcmp(cmd, "list") == 0)
        rc = cmd_list(ctx);
    else if (strcmp(cmd, "create") == 0)
        rc = cmd_create(ctx, sub_argc, sub_argv);
    else if (strcmp(cmd, "remove") == 0)
        rc = cmd_remove(ctx, sub_argc, sub_argv);
    else if (strcmp(cmd, "icons") == 0)
        rc = cmd_icons(ctx, sub_argc, sub_argv);
    else if (strcmp(cmd, "fetch-icon") == 0)
        rc = cmd_fetch_icon(ctx, sub_argc, sub_argv);
    else {
        usage(stderr);
        rc = 2;
    }
    extapp_context_free(ctx);
    return rc;
}
