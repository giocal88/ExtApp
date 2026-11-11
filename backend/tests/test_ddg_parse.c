#include "testutil.h"
#include "private.h"

static char *slurp(const char *name)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", FIXTURES_DIR, name);
    char *data = NULL;
    if (ea_read_file(path, 1 << 20, &data, NULL, NULL) != EXTAPP_OK) {
        fprintf(stderr, "cannot read fixture %s\n", path);
        exit(2);
    }
    return data;
}

int main(void)
{
    char *html = slurp("ddg_search.html");
    char *vqd = ea_ddg_extract_vqd(html);
    CHECK_STR(vqd, "4-123456789012345678901234567890");
    free(vqd);
    free(html);

    vqd = ea_ddg_extract_vqd("...&vqd=4-99887766&p=1...");
    CHECK_STR(vqd, "4-99887766");
    free(vqd);
    vqd = ea_ddg_extract_vqd("{\"vqd\":\"4-1122334455\"}");
    CHECK_STR(vqd, "4-1122334455");
    free(vqd);
    CHECK(ea_ddg_extract_vqd("<html>nothing here</html>") == NULL);

    char *json = slurp("ddg_images.json");
    ExtAppIconResultList l;
    char *err = NULL;
    CHECK(ea_ddg_parse_results(json, 0, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 3); /* the entry without URLs is dropped */
    if (l.count == 3) {
        CHECK_STR(l.items[0].image_url, "https://example.com/firefox.png");
        CHECK_STR(l.items[0].thumbnail_url, "https://tse1.mm.bing.net/th?id=OIP.1");
        CHECK_STR(l.items[0].title, "Firefox logo");
        CHECK(l.items[0].width == 512 && l.items[0].height == 512);
        CHECK_STR(l.items[1].thumbnail_url, "https://tse2.mm.bing.net/th?id=OIP.2");
        CHECK(l.items[2].width == 256); /* numeric string accepted */
    }
    for (size_t i = 0; i < l.count; i++) {
        free(l.items[i].image_url);
        free(l.items[i].thumbnail_url);
        free(l.items[i].title);
        free(l.items[i].source);
    }
    free(l.items);

    CHECK(ea_ddg_parse_results(json, 1, &l, &err) == EXTAPP_OK);
    CHECK(l.count == 1);
    for (size_t i = 0; i < l.count; i++) {
        free(l.items[i].image_url);
        free(l.items[i].thumbnail_url);
        free(l.items[i].title);
        free(l.items[i].source);
    }
    free(l.items);
    free(json);

    CHECK(ea_ddg_parse_results(
              "{\"challenge\":{\"cc\":\"botnet\"},\"response_type\":\"botnet_challenge\"}",
              0, &l, &err) == EXTAPP_ERR_NETWORK);
    CHECK(err && strstr(err, "blocking"));
    CHECK(ea_ddg_parse_results("not json", 0, &l, &err) == EXTAPP_ERR_PARSE);
    CHECK(ea_ddg_parse_results("{\"x\":1}", 0, &l, &err) == EXTAPP_ERR_PARSE);
    free(err);
    TEST_DONE();
}
