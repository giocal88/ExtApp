#include "testutil.h"
#include "private.h"

static const ExtAppEntry *find_entry(const ExtAppEntryList *l, const char *id)
{
    for (size_t i = 0; i < l->count; i++)
        if (strcmp(l->items[i].id, id) == 0)
            return &l->items[i];
    return NULL;
}

int main(void)
{
    char *apps = t_mkdtemp();   /* folder with programs */
    char *data = t_mkdtemp();   /* fake XDG_DATA_HOME   */
    ExtAppContext *ctx = extapp_context_new_with_data_dir(data);
    CHECK(ctx != NULL);
    char p[4096], *err = NULL, *out = NULL;

    /* program with a space and a $ in its path, initially not executable */
    snprintf(p, sizeof p, "%s/My $Apps", apps);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/My $Apps/Foo-1.0.AppImage", apps);
    unsigned char elf[64] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 'A', 'I', 2};
    t_write(p, elf, sizeof elf, 0644);
    char *target = realpath(p, NULL);

    /* list on a non-existing applications dir is fine and empty */
    ExtAppEntryList *l = NULL;
    CHECK(extapp_list_entries(ctx, &l, &err) == EXTAPP_OK);
    CHECK(l && l->count == 0);
    extapp_entry_list_free(l);

    ExtAppEntrySpec spec = {
        .name = "Foo App",
        .target_path = p,
        .args = "--no-sandbox %U",
        .icon = "/tmp/icon.png",
        .comment = "Test app",
        .categories = "Utility",
        .terminal = false,
    };
    CHECK(extapp_entry_create(ctx, &spec, EXTAPP_CREATE_MAKE_EXECUTABLE, &out,
                              &err) == EXTAPP_OK);
    CHECK(out && strstr(out, "/applications/extapp-foo-app.desktop"));
    struct stat st;
    CHECK(stat(target, &st) == 0 && (st.st_mode & S_IXUSR));

    /* the written file parses back and resolves to the target */
    EaDesktopEntry de;
    CHECK(out && ea_de_parse_file(out, &de, NULL) == EXTAPP_OK);
    CHECK_STR(ea_de_get(&de, "Name"), "Foo App");
    CHECK_STR(ea_de_get(&de, "Categories"), "Utility;");
    CHECK(ea_de_get_bool(&de, "X-ExtApp-Managed"));
    size_t argc = 0;
    char **argv = ea_exec_split(ea_de_get(&de, "Exec"), &argc);
    CHECK(argv && argc == 2);
    if (argv && argc == 2) {
        CHECK_STR(argv[0], target);
        CHECK_STR(argv[1], "--no-sandbox");
    }
    ea_strv_free(argv);
    char *resolved = ea_de_resolve_target(&de);
    CHECK_STR(resolved, target);
    free(resolved);
    ea_de_clear(&de);

    /* second create for same target -> EXISTS, returns existing path */
    char *out2 = NULL;
    CHECK(extapp_entry_create(ctx, &spec, 0, &out2, &err) == EXTAPP_ERR_EXISTS);
    CHECK_STR(out2, out);
    free(out2);
    out2 = NULL;
    free(err);
    err = NULL;

    /* overwrite keeps the same file */
    spec.name = "Foo Renamed";
    CHECK(extapp_entry_create(ctx, &spec, EXTAPP_CREATE_OVERWRITE, &out2, &err) ==
          EXTAPP_OK);
    CHECK_STR(out2, out);
    free(out2);

    /* foreign (unmanaged) entry in a sub folder */
    snprintf(p, sizeof p, "%s/applications/vendor", data);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/applications/vendor/other.desktop", data);
    const char *foreign = "[Desktop Entry]\nType=Application\nName=Other\n"
                          "Exec=/bin/true\nNoDisplay=true\n";
    t_write(p, foreign, strlen(foreign), 0644);
    snprintf(p, sizeof p, "%s/applications/link.desktop", data);
    const char *link = "[Desktop Entry]\nType=Link\nName=Web\nURL=https://x\n";
    t_write(p, link, strlen(link), 0644);

    CHECK(extapp_list_entries(ctx, &l, &err) == EXTAPP_OK);
    CHECK(l && l->count == 2); /* Type=Link ignored */
    if (l) {
        const ExtAppEntry *e = find_entry(l, "extapp-foo-app.desktop");
        CHECK(e && e->managed && e->target_exists);
        if (e) {
            CHECK_STR(e->name, "Foo Renamed");
            CHECK_STR(e->args, "--no-sandbox %U");
            CHECK_STR(e->target_path, target);
        }
        e = find_entry(l, "vendor-other.desktop");
        CHECK(e && !e->managed && e->no_display);
        extapp_entry_list_free(l);
    }

    /* scan links the candidate to its shortcut */
    ExtAppCandidateList *cl = NULL;
    CHECK(extapp_scan(ctx, apps, NULL, NULL, NULL, NULL, &cl, &err) == EXTAPP_OK);
    CHECK(cl && cl->count == 1);
    if (cl && cl->count == 1)
        CHECK_STR(cl->items[0].desktop_file, out);
    extapp_candidate_list_free(cl);

    /* refuse removing unmanaged files unless allowed */
    snprintf(p, sizeof p, "%s/applications/vendor/other.desktop", data);
    CHECK(extapp_entry_remove(ctx, p, 0, &err) == EXTAPP_ERR_NOT_MANAGED);
    free(err);
    err = NULL;
    CHECK(access(p, F_OK) == 0);
    CHECK(extapp_entry_remove(ctx, p, EXTAPP_REMOVE_ALLOW_UNMANAGED, &err) ==
          EXTAPP_OK);
    CHECK(access(p, F_OK) != 0);

    /* refuse paths outside the applications dir */
    snprintf(p, sizeof p, "%s/outside.desktop", apps);
    t_write(p, foreign, strlen(foreign), 0644);
    CHECK(extapp_entry_remove(ctx, p, EXTAPP_REMOVE_ALLOW_UNMANAGED, &err) ==
          EXTAPP_ERR_INVALID_ARG);
    free(err);
    err = NULL;
    CHECK(access(p, F_OK) == 0);

    /* icon stored by us is deleted together with the shortcut */
    const unsigned char png[] = "\x89PNG\r\n\x1a\nrest-of-png";
    char *icon = NULL;
    CHECK(extapp_icon_save(ctx, "Bar", png, sizeof png - 1, &icon, &err) == EXTAPP_OK);
    CHECK(icon && strstr(icon, "/extapp/icons/bar.png"));
    spec.name = "Bar";
    spec.icon = icon;
    snprintf(p, sizeof p, "%s/bar", apps);
    t_write(p, elf, sizeof elf, 0755);
    spec.target_path = p;
    char *bar = NULL;
    CHECK(extapp_entry_create(ctx, &spec, 0, &bar, &err) == EXTAPP_OK);
    CHECK(extapp_entry_remove(ctx, bar, EXTAPP_REMOVE_DELETE_ICON, &err) == EXTAPP_OK);
    CHECK(access(bar, F_OK) != 0);
    CHECK(icon && access(icon, F_OK) != 0);
    free(bar);
    free(icon);

    /* removing the managed one works */
    CHECK(extapp_entry_remove(ctx, out, 0, &err) == EXTAPP_OK);
    CHECK(access(out, F_OK) != 0);

    if (err)
        fprintf(stderr, "last error: %s\n", err);
    free(err);
    free(out);
    free(target);
    extapp_context_free(ctx);
    t_rm_rf(apps);
    t_rm_rf(data);
    free(apps);
    free(data);
    TEST_DONE();
}
