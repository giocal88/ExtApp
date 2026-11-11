#include "testutil.h"
#include "private.h"

int main(void)
{
    const unsigned char png[] = "\x89PNG\r\n\x1a\n....";
    const unsigned char jpg[] = "\xFF\xD8\xFF\xE0....";
    const unsigned char webp[] = "RIFF\x10\0\0\0WEBPVP8 ";
    const unsigned char ico[] = {0, 0, 1, 0, 1, 0, 16, 16};
    const char *svg = "\xEF\xBB\xBF  <?xml version=\"1.0\"?>\n<svg xmlns=\"x\"></svg>";
    const char *html = "<html><body>nope</body></html>";

    CHECK_STR(ea_sniff_image(png, sizeof png - 1), "png");
    CHECK_STR(ea_sniff_image(jpg, sizeof jpg - 1), "jpg");
    CHECK_STR(ea_sniff_image(webp, sizeof webp - 1), "webp");
    CHECK_STR(ea_sniff_image(ico, sizeof ico), "ico");
    CHECK_STR(ea_sniff_image((const unsigned char *)svg, strlen(svg)), "svg");
    CHECK(ea_sniff_image((const unsigned char *)html, strlen(html)) == NULL);

    char *data = t_mkdtemp();
    ExtAppContext *ctx = extapp_context_new_with_data_dir(data);
    char *a = NULL, *b = NULL, *err = NULL;
    CHECK(extapp_icon_save(ctx, "My App", png, sizeof png - 1, &a, &err) == EXTAPP_OK);
    CHECK(extapp_icon_save(ctx, "My App", png, sizeof png - 1, &b, &err) == EXTAPP_OK);
    CHECK(a && strstr(a, "/my-app.png"));
    CHECK(b && strstr(b, "/my-app-2.png"));
    free(a);
    a = NULL;
    CHECK(extapp_icon_save(ctx, "x", (const unsigned char *)html, strlen(html),
                           &a, &err) == EXTAPP_ERR_UNSUPPORTED);
    CHECK(a == NULL);
    free(b);
    free(err);
    extapp_context_free(ctx);
    t_rm_rf(data);
    free(data);
    TEST_DONE();
}
