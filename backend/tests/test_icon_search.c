/* Icon search: query helpers and provider parsers (no network). */
#include "testutil.h"
#include "private.h"

static char *slurp(const char *name)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", FIXTURES_DIR, name);
    char *data = NULL;
    if (ea_read_file(path, 1 << 22, &data, NULL, NULL) != EXTAPP_OK) {
        fprintf(stderr, "cannot read fixture %s\n", path);
        exit(2);
    }
    return data;
}

static void test_terms(void)
{
    char **t = ea_icon_query_terms("Kdenlive icon");
    CHECK(t && t[0] && !t[1]);
    if (t && t[0])
        CHECK_STR(t[0], "kdenlive");
    ea_strv_free(t);

    t = ea_icon_query_terms("  LM-Studio App LOGO  lm ");
    CHECK(t && t[0] && t[1] && !t[2]);
    if (t && t[0] && t[1]) {
        CHECK_STR(t[0], "lm");
        CHECK_STR(t[1], "studio");
    }
    char *j = ea_icon_terms_join(t);
    CHECK_STR(j, "lm studio");
    free(j);
    ea_strv_free(t);

    t = ea_icon_query_terms("icon logo");
    CHECK(t && !t[0]);
    j = ea_icon_terms_join(t);
    CHECK_STR(j, "");
    free(j);
    ea_strv_free(t);

    t = ea_icon_query_terms("Àudio Ünit");      /* UTF-8 kept as part of words */
    CHECK(t && t[0] && t[1] && !t[2]);
    ea_strv_free(t);
}

static void test_score(void)
{
    char *obs[] = {"obsidian", NULL};
    char *gimp[] = {"gimp", NULL};
    char *code[] = {"code", NULL};
    char *lm[] = {"lm", "studio", NULL};
    char *none[] = {NULL};
    CHECK(ea_icon_name_score("Obsidian", obs) == 100);
    CHECK(ea_icon_name_score("GIMP Portable", gimp) == 80);
    CHECK(ea_icon_name_score("Code - OSS", code) == 80);
    CHECK(ea_icon_name_score("Visual Studio Code", code) == 40);
    CHECK(ea_icon_name_score("LM Studio", lm) == 100);
    CHECK(ea_icon_name_score("Studio LM", lm) == 40);
    CHECK(ea_icon_name_score("Elmer_Studio", lm) == 0);  /* not a word start */
    CHECK(ea_icon_name_score("NotObsidianAtAll", obs) == 0);
    CHECK(ea_icon_name_score("org.gimp.GIMP", gimp) == 40);
    CHECK(ea_icon_name_score("Firefox", obs) == 0);
    CHECK(ea_icon_name_score("Obsidian", none) == 0);
    CHECK(ea_icon_name_score(NULL, obs) == 0);
}

static void test_appimagehub(void)
{
    char *feed = slurp("appimagehub_feed.json");
    ExtAppIconResultList l;
    char *err = NULL;

    char *obs[] = {"obsidian", NULL};
    CHECK(ea_appimagehub_parse(feed, obs, 0, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 2); /* "NotObsidianAtAll" is not a word match */
    if (l.count == 2) {
        CHECK_STR(l.items[0].title, "Obsidian");
        CHECK_STR(l.items[0].image_url,
                  "https://appimage.github.io/database/Obsidian/icons/128x128/obsidian.png");
        CHECK_STR(l.items[0].provider, "AppImageHub");
        CHECK_STR(l.items[0].source, "https://appimage.github.io/Obsidian/");
        CHECK(l.items[0].width == 128);
        CHECK(!l.items[0].scalable);
        /* prefix match second, and its largest raster icon is chosen */
        CHECK_STR(l.items[1].title, "Obsidian Clipper Helper");
        CHECK_STR(l.items[1].image_url,
                  "https://appimage.github.io/database/Obsidian%20Clipper%20Helper/icons/256x256/helper.png");
        CHECK(l.items[1].width == 256);
    }
    ea_icon_list_clear(&l);

    CHECK(ea_appimagehub_parse(feed, obs, 1, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 1);
    ea_icon_list_clear(&l);

    char *kd[] = {"kdenlive", NULL};
    CHECK(ea_appimagehub_parse(feed, kd, 0, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 1); /* "Kdenlive Tools" has no icons */
    if (l.count == 1) {
        CHECK_STR(l.items[0].image_url,
                  "https://appimage.github.io/database/Kdenlive/icons/scalable/kdenlive.svg");
        CHECK(l.items[0].width == 0);
        CHECK(l.items[0].scalable);
    }
    ea_icon_list_clear(&l);

    /* .svgz cannot be stored: skipped, falling back to a raster if any */
    char *kr[] = {"krita", NULL};
    CHECK(ea_appimagehub_parse(feed, kr, 0, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 0);
    ea_icon_list_clear(&l);
    char *dk[] = {"digikam", NULL};
    CHECK(ea_appimagehub_parse(feed, dk, 0, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 1);
    if (l.count == 1) {
        CHECK(strstr(l.items[0].image_url, "digikam.png") != NULL);
        CHECK(l.items[0].width == 48 && !l.items[0].scalable);
    }
    ea_icon_list_clear(&l);

    char *ff[] = {"firefox", NULL};
    CHECK(ea_appimagehub_parse(feed, ff, 0, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 1);
    if (l.count == 1)
        CHECK_STR(l.items[0].source, "https://appimage.github.io/Firefox%20Nightly/");
    ea_icon_list_clear(&l);

    char *evil[] = {"evil", NULL}; /* path traversal / absolute URLs rejected */
    CHECK(ea_appimagehub_parse(feed, evil, 0, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 0);
    ea_icon_list_clear(&l);

    char *none[] = {NULL};
    CHECK(ea_appimagehub_parse(feed, none, 0, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 0);

    CHECK(ea_appimagehub_parse("{\"items\": 3}", obs, 0, &l, &err) == EXTAPP_ERR_PARSE);
    CHECK(ea_appimagehub_parse("<html>", obs, 0, &l, &err) == EXTAPP_ERR_PARSE);
    free(err);
    free(feed);
}

static void test_flathub(void)
{
    char *json = slurp("flathub_search.json");
    ExtAppIconResultList l;
    char *err = NULL;
    char *obs[] = {"obsidian", NULL};
    CHECK(ea_flathub_parse(json, obs, 0, &l, &err) == EXTAPP_OK);
    /* top hit + "Obsidian Tools"; unrelated, icon-less and http hits dropped */
    CHECK(l.count == 2);
    if (l.count == 2) {
        CHECK_STR(l.items[0].title, "Obsidian");
        CHECK_STR(l.items[0].provider, "Flathub");
        CHECK_STR(l.items[0].source, "https://flathub.org/apps/md.obsidian.Obsidian");
        CHECK(strncmp(l.items[0].image_url, "https://dl.flathub.org/", 23) == 0);
        CHECK(l.items[0].width == 128);
        CHECK_STR(l.items[1].title, "Obsidian Tools");
        CHECK(l.items[1].width == 64);
    }
    ea_icon_list_clear(&l);

    /* keyword match: "vscode" finds Visual Studio Code */
    char *vs[] = {"vscode", NULL};
    CHECK(ea_flathub_parse(json, vs, 0, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 1);
    if (l.count == 1)
        CHECK_STR(l.items[0].title, "Visual Studio Code");
    ea_icon_list_clear(&l);

    /* app id match: "toolstack" -> com.toolstack.Folio */
    char *ts[] = {"toolstack", NULL};
    CHECK(ea_flathub_parse(json, ts, 0, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 1);
    if (l.count == 1)
        CHECK_STR(l.items[0].title, "Folio");
    ea_icon_list_clear(&l);

    /* nonsense: Flathub's fuzzy hits are all rejected */
    char *junk[] = {"qwzxkjv", NULL};
    CHECK(ea_flathub_parse(json, junk, 0, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 0);
    ea_icon_list_clear(&l);

    CHECK(ea_flathub_parse(json, obs, 1, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 1);
    ea_icon_list_clear(&l);

    CHECK(ea_flathub_parse("{\"hits\": []}", obs, 0, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 0);
    CHECK(ea_flathub_parse("{\"message\":\"bad\"}", obs, 0, &l, &err) == EXTAPP_ERR_PARSE);
    free(err);
    free(json);
}

static void test_commons(void)
{
    char *json = slurp("commons_search.json");
    ExtAppIconResultList l;
    char *err = NULL;
    char *kd[] = {"kdenlive", NULL};
    CHECK(ea_commons_parse(json, kd, 0, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 5); /* the PDF and the unrelated photo are skipped */
    if (l.count == 5) {
        /* ordered by search rank ("index"), not by JSON order */
        CHECK_STR(l.items[0].title, "Kdenlive-logo.svg");
        CHECK_STR(l.items[0].provider, "Wikimedia Commons");
        CHECK(strstr(l.items[0].image_url, "upload.wikimedia.org") != NULL);
        CHECK(strstr(l.items[0].image_url, ".svg") != NULL); /* SVG kept */
        CHECK(l.items[0].width == 0);
        CHECK(l.items[0].scalable);
        CHECK(strstr(l.items[0].thumbnail_url, "thumb") != NULL);
        CHECK_STR(l.items[1].title, "Kdenlive-logo-blank.png");
        CHECK(l.items[1].width == 500); /* small raster: original */
        CHECK(!l.items[1].scalable);
        CHECK_STR(l.items[4].title, "Kdenlive big screenshot.png");
        /* big raster: thumbnail stored instead of the 3000 px original */
        CHECK_STR(l.items[4].image_url, "https://upload.wikimedia.org/thumb/big.png/256px-big.png");
        /* Commons rounds thumbnail sizes: reported as unknown */
        CHECK(l.items[4].width == 0 && l.items[4].height == 0);
        CHECK(!l.items[4].scalable);
        CHECK_STR(l.items[4].source, "https://commons.wikimedia.org/wiki/File:Big.png");
    }
    ea_icon_list_clear(&l);

    CHECK(ea_commons_parse(json, kd, 2, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 2);
    ea_icon_list_clear(&l);

    char *other[] = {"gimp", NULL};
    CHECK(ea_commons_parse(json, other, 0, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 0);
    CHECK(ea_commons_parse("{\"batchcomplete\":true}", kd, 0, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 0); /* no results */
    CHECK(ea_commons_parse("{\"error\":{\"code\":\"x\",\"info\":\"Bad thing\"}}", kd, 0, &l, &err) ==
          EXTAPP_ERR_NETWORK);
    CHECK(err && strstr(err, "Bad thing"));
    CHECK(ea_commons_parse("[1,2]", kd, 0, &l, &err) == EXTAPP_ERR_PARSE);
    free(err);
    free(json);
}

static void test_list_push(void)
{
    ExtAppIconResultList l = {0};
    CHECK(!ea_icon_list_push(&l, "P", NULL, NULL, "t", NULL, 0, 0, false));
    CHECK(ea_icon_list_push(&l, "P", NULL, "https://t", NULL, NULL, 1, 1, false));
    CHECK(l.count == 1);
    if (l.count == 1)
        CHECK_STR(l.items[0].image_url, "https://t"); /* falls back to thumb */
    ea_icon_list_clear(&l);
    CHECK(l.count == 0 && l.items == NULL);
}

/* ---- merge / status contract with fake providers ------------------- */

static ExtAppStatus fake_a(ExtAppContext *c, const char *q, size_t m, ExtAppCancelFn cf,
                           void *ud, ExtAppIconResultList *out, char **err)
{
    (void)c; (void)q; (void)m; (void)cf; (void)ud; (void)err;
    ea_icon_list_push(out, "A", "https://a1", NULL, "a1", NULL, 1, 1, false);
    ea_icon_list_push(out, "A", "https://dup", NULL, "dup-a", NULL, 2, 2, true);
    return EXTAPP_OK;
}
static ExtAppStatus fake_b(ExtAppContext *c, const char *q, size_t m, ExtAppCancelFn cf,
                           void *ud, ExtAppIconResultList *out, char **err)
{
    (void)c; (void)q; (void)m; (void)cf; (void)ud; (void)err;
    ea_icon_list_push(out, "B", "https://dup", NULL, "dup-b", NULL, 3, 3, false);
    ea_icon_list_push(out, "B", "https://b1", NULL, "b1", NULL, 4, 4, false);
    return EXTAPP_OK;
}
static ExtAppStatus fake_fail(ExtAppContext *c, const char *q, size_t m, ExtAppCancelFn cf,
                              void *ud, ExtAppIconResultList *out, char **err)
{
    (void)c; (void)q; (void)m; (void)cf; (void)ud;
    ea_icon_list_push(out, "F", "https://partial", NULL, NULL, NULL, 0, 0, false);
    ea_set_err(err, "boom");
    return EXTAPP_ERR_NETWORK; /* partial results must be discarded */
}
static ExtAppStatus fake_empty(ExtAppContext *c, const char *q, size_t m, ExtAppCancelFn cf,
                               void *ud, ExtAppIconResultList *out, char **err)
{
    (void)c; (void)q; (void)m; (void)cf; (void)ud; (void)out; (void)err;
    return EXTAPP_OK;
}
static ExtAppStatus fake_cancel(ExtAppContext *c, const char *q, size_t m, ExtAppCancelFn cf,
                                void *ud, ExtAppIconResultList *out, char **err)
{
    (void)c; (void)q; (void)m; (void)out; (void)err;
    return cf && cf(ud) ? EXTAPP_ERR_CANCELLED : EXTAPP_OK;
}
static bool always_cancel(void *ud) { (void)ud; return true; }

static const EaIconProvider P_A = {"A", fake_a}, P_B = {"B", fake_b},
                            P_FAIL = {"Fail", fake_fail}, P_EMPTY = {"Empty", fake_empty},
                            P_CANCEL = {"Cancel", fake_cancel};

static void test_merge(void)
{
    char *dir = t_mkdtemp();
    ExtAppContext *ctx = extapp_context_new_with_data_dir(dir);
    ExtAppIconResultList *l = NULL;
    char *err = NULL;

    const EaIconProvider *ab[] = {&P_A, &P_B};
    CHECK(ea_icon_search_with(ab, 2, ctx, "x", 0, NULL, NULL, &l, &err) == EXTAPP_OK);
    CHECK(l && l->count == 3); /* duplicate URL kept once, first provider wins */
    if (l && l->count == 3) {
        CHECK_STR(l->items[0].image_url, "https://a1");
        CHECK_STR(l->items[1].title, "dup-a");
        CHECK(l->items[1].scalable && l->items[1].width == 2);
        CHECK_STR(l->items[2].image_url, "https://b1");
        CHECK_STR(l->items[2].provider, "B");
    }
    extapp_icon_result_list_free(l);

    CHECK(ea_icon_search_with(ab, 2, ctx, "x", 2, NULL, NULL, &l, &err) == EXTAPP_OK);
    CHECK(l && l->count == 2); /* max_results respected */
    extapp_icon_result_list_free(l);

    const EaIconProvider *a_fail[] = {&P_A, &P_FAIL};
    CHECK(ea_icon_search_with(a_fail, 2, ctx, "x", 0, NULL, NULL, &l, &err) == EXTAPP_OK);
    CHECK(l && l->count == 2); /* partial success; failed provider's items dropped */
    extapp_icon_result_list_free(l);

    const EaIconProvider *fail_empty[] = {&P_FAIL, &P_EMPTY};
    CHECK(ea_icon_search_with(fail_empty, 2, ctx, "x", 0, NULL, NULL, &l, &err) ==
          EXTAPP_ERR_NETWORK);
    CHECK(l == NULL);
    CHECK(err && strstr(err, "Fail: boom"));
    free(err);
    err = NULL;

    const EaIconProvider *empties[] = {&P_EMPTY, &P_EMPTY};
    CHECK(ea_icon_search_with(empties, 2, ctx, "x", 0, NULL, NULL, &l, &err) == EXTAPP_OK);
    CHECK(l && l->count == 0); /* nothing found, nothing broken */
    extapp_icon_result_list_free(l);

    const EaIconProvider *canc[] = {&P_CANCEL, &P_A};
    CHECK(ea_icon_search_with(canc, 2, ctx, "x", 0, always_cancel, NULL, &l, &err) ==
          EXTAPP_ERR_CANCELLED);
    CHECK(l == NULL);
    free(err);
    err = NULL;

    /* only noise words: rejected before any provider runs */
    CHECK(ea_icon_search_with(ab, 2, ctx, "icon logo", 0, NULL, NULL, &l, &err) ==
          EXTAPP_ERR_INVALID_ARG);
    CHECK(l == NULL);
    free(err);
    err = NULL;
    CHECK(extapp_icon_search(ctx, "", 0, NULL, NULL, &l, &err) == EXTAPP_ERR_INVALID_ARG);
    free(err);

    extapp_context_free(ctx);
    t_rm_rf(dir);
    free(dir);
}

int main(void)
{
    test_terms();
    test_score();
    test_appimagehub();
    test_flathub();
    test_commons();
    test_list_push();
    test_merge();
    TEST_DONE();
}
