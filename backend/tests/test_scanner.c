#include "testutil.h"
#include "private.h"

/* Minimal ELF64 little-endian header bytes. */
static void make_elf(unsigned char *h, size_t n, unsigned type, bool appimage)
{
    memset(h, 0, n);
    memcpy(h, "\x7f" "ELF", 4);
    h[4] = 2; /* 64 bit */
    h[5] = 1; /* little endian */
    h[6] = 1;
    if (appimage) {
        h[8] = 'A';
        h[9] = 'I';
        h[10] = 2;
    }
    h[16] = (unsigned char)type;
}

static void test_suggest_name(void)
{
    struct { const char *in, *out; } cases[] = {
        {"/x/Foo-1.2-x86_64.AppImage", "Foo"},
        {"/x/LM_Studio-0.2.31.AppImage", "LM Studio"},
        {"kdenlive-24.02.1-x86_64.appimage", "Kdenlive"},
        {"nvim.appimage", "Nvim"},
        {"balenaEtcher-1.18.11-x64.AppImage", "BalenaEtcher"},
        {"my_game.x86_64", "My Game"},
        {"2048", "2048"},
        {"tool-v3-linux", "Tool"},
        {NULL, NULL}};
    for (int i = 0; cases[i].in; i++) {
        char *s = extapp_suggest_name(cases[i].in);
        CHECK_STR(s, cases[i].out);
        free(s);
    }
}

static bool cancel_always(void *ud) { (void)ud; return true; }

static const ExtAppCandidate *find(const ExtAppCandidateList *l, const char *base)
{
    for (size_t i = 0; i < l->count; i++)
        if (strcmp(ea_basename(l->items[i].path), base) == 0)
            return &l->items[i];
    return NULL;
}

static void test_scan(void)
{
    char *root = t_mkdtemp();
    char *data = t_mkdtemp();
    CHECK(root && data);
    ExtAppContext *ctx = extapp_context_new_with_data_dir(data);
    CHECK(ctx != NULL);

    unsigned char h[64];
    char p[4096];

    make_elf(h, sizeof h, 2, true);
    snprintf(p, sizeof p, "%s/Foo-1.2-x86_64.AppImage", root);
    t_write(p, h, sizeof h, 0644); /* AppImage without +x */

    make_elf(h, sizeof h, 3, false);
    snprintf(p, sizeof p, "%s/mytool", root);
    t_write(p, h, sizeof h, 0755);

    snprintf(p, sizeof p, "%s/libfoo.so.1", root);
    t_write(p, h, sizeof h, 0755); /* library: skipped */

    make_elf(h, sizeof h, 1 /* ET_REL */, false);
    snprintf(p, sizeof p, "%s/object", root);
    t_write(p, h, sizeof h, 0755); /* not executable type: skipped */

    snprintf(p, sizeof p, "%s/run.sh", root);
    t_write(p, "#!/bin/sh\necho hi\n", 18, 0755);

    snprintf(p, sizeof p, "%s/readme.txt", root);
    t_write(p, "hello world", 11, 0644);

    snprintf(p, sizeof p, "%s/sub", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/sub/deep", root);
    mkdir(p, 0755);
    make_elf(h, sizeof h, 2, false);
    snprintf(p, sizeof p, "%s/sub/subtool", root);
    t_write(p, h, sizeof h, 0755);
    snprintf(p, sizeof p, "%s/sub/deep/deeptool", root);
    t_write(p, h, sizeof h, 0755);

    snprintf(p, sizeof p, "%s/.hidden", root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/.hidden/secret", root);
    t_write(p, h, sizeof h, 0755);

    /* symlink loop */
    snprintf(p, sizeof p, "%s/sub/loop", root);
    CHECK(symlink(root, p) == 0);

    ExtAppScanOptions o;
    extapp_scan_options_init(&o);
    o.max_depth = 1;
    o.follow_symlinks = true;
    ExtAppCandidateList *l = NULL;
    char *err = NULL;
    CHECK(extapp_scan(ctx, root, &o, NULL, NULL, NULL, &l, &err) == EXTAPP_OK);
    CHECK(l != NULL);
    if (l) {
        const ExtAppCandidate *c = find(l, "Foo-1.2-x86_64.AppImage");
        CHECK(c && c->kind == EXTAPP_KIND_APPIMAGE && !c->is_executable);
        if (c)
            CHECK_STR(c->suggested_name, "Foo");
        c = find(l, "mytool");
        CHECK(c && c->kind == EXTAPP_KIND_ELF && c->is_executable);
        CHECK(find(l, "subtool") != NULL);
        CHECK(find(l, "deeptool") == NULL); /* depth limit */
        CHECK(find(l, "libfoo.so.1") == NULL);
        CHECK(find(l, "object") == NULL);
        CHECK(find(l, "run.sh") == NULL); /* scripts off by default */
        CHECK(find(l, "readme.txt") == NULL);
        CHECK(find(l, "secret") == NULL);
        CHECK(l->count == 3);
        extapp_candidate_list_free(l);
    }

    o.max_depth = 5;
    o.include_scripts = true;
    o.include_hidden = true;
    CHECK(extapp_scan(ctx, root, &o, NULL, NULL, NULL, &l, &err) == EXTAPP_OK);
    if (l) {
        CHECK(find(l, "run.sh") && find(l, "run.sh")->kind == EXTAPP_KIND_SCRIPT);
        CHECK(find(l, "deeptool") != NULL);
        CHECK(find(l, "secret") != NULL);
        CHECK(l->count == 6); /* loop must not duplicate entries */
        extapp_candidate_list_free(l);
    }

    CHECK(extapp_scan(ctx, root, &o, NULL, cancel_always, NULL, &l, &err) ==
          EXTAPP_ERR_CANCELLED);
    CHECK(l == NULL);
    free(err);
    err = NULL;

    CHECK(extapp_scan(ctx, "/no/such/dir", NULL, NULL, NULL, NULL, &l, &err) ==
          EXTAPP_ERR_NOT_FOUND);
    free(err);

    extapp_context_free(ctx);
    t_rm_rf(root);
    t_rm_rf(data);
    free(root);
    free(data);
}

int main(void)
{
    test_suggest_name();
    test_scan();
    TEST_DONE();
}
