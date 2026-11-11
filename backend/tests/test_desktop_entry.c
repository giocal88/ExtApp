#include "testutil.h"
#include "private.h"

static void test_parse(void)
{
    const char *text =
        "\xEF\xBB\xBF# comment\n"
        "[Desktop Entry]\r\n"
        "Type = Application\n"
        "Name=My App\n"
        "Name[it]=La mia app\n"
        "Comment=line1\\nline2 \\s x\n"
        "Exec=\"/opt/My Apps/run\\\\$x.sh\" --flag %U\n"
        "Terminal=true\n"
        "Name=Duplicate ignored\n"
        "\n"
        "[Desktop Action new]\n"
        "Name=Other group\n";
    EaDesktopEntry de;
    CHECK(ea_de_parse_string(text, &de) == EXTAPP_OK);
    CHECK_STR(ea_de_get(&de, "Type"), "Application");
    CHECK_STR(ea_de_get(&de, "Name"), "My App");
    CHECK_STR(ea_de_get(&de, "Name[it]"), "La mia app");
    CHECK_STR(ea_de_get(&de, "Comment"), "line1\nline2   x");
    CHECK(ea_de_get_bool(&de, "Terminal"));
    CHECK(!ea_de_get_bool(&de, "NoDisplay"));

    size_t argc = 0;
    char **argv = ea_exec_split(ea_de_get(&de, "Exec"), &argc);
    CHECK(argv != NULL);
    CHECK(argc == 2);
    if (argv && argc == 2) {
        CHECK_STR(argv[0], "/opt/My Apps/run$x.sh");
        CHECK_STR(argv[1], "--flag");
    }
    ea_strv_free(argv);
    ea_de_clear(&de);

    CHECK(ea_de_parse_string("Name=x\n", &de) == EXTAPP_ERR_PARSE);
}

static void test_quote_roundtrip(void)
{
    const char *cases[] = {
        "/usr/bin/simple",
        "/home/u/My Apps/Foo.AppImage",
        "/tmp/we\"ird$`\\name",
        "/tmp/100%real",
        "",
        NULL};
    for (const char **c = cases; *c; c++) {
        char *q = ea_exec_quote_arg(*c);
        /* simulate writing + reading the Exec value */
        char *esc = ea_de_escape_value(q);
        char *unesc = ea_de_unescape_value(esc);
        size_t argc = 0;
        char **argv = ea_exec_split(unesc, &argc);
        CHECK(argv && argc == 1);
        if (argv && argc == 1)
            CHECK_STR(argv[0], *c);
        ea_strv_free(argv);
        free(q);
        free(esc);
        free(unesc);
    }

    char *q = ea_exec_quote_arg("/usr/bin/plain");
    CHECK_STR(q, "/usr/bin/plain");
    free(q);
    q = ea_exec_quote_arg("/a b/c");
    CHECK_STR(q, "\"/a b/c\"");
    free(q);
}

static void test_exec_split(void)
{
    size_t argc = 0;
    char **v = ea_exec_split("env FOO=1 /bin/app %f --x=%u 100%%", &argc);
    CHECK(v && argc == 5);
    if (v && argc == 5) {
        CHECK_STR(v[0], "env");
        CHECK_STR(v[2], "/bin/app");
        CHECK_STR(v[3], "--x=");
        CHECK_STR(v[4], "100%");
    }
    ea_strv_free(v);
    CHECK(ea_exec_split("\"/unterminated", &argc) == NULL);
}

static void test_escape(void)
{
    char *e = ea_de_escape_value(" lead\\back\nnl");
    CHECK_STR(e, "\\slead\\\\back\\nnl");
    char *u = ea_de_unescape_value(e);
    CHECK_STR(u, " lead\\back\nnl");
    free(e);
    free(u);
}

static void test_resolve_target(void)
{
    EaDesktopEntry de;
    CHECK(ea_de_parse_string("[Desktop Entry]\nExec=env A=b sh -c x\n", &de) ==
          EXTAPP_OK);
    char *t = ea_de_resolve_target(&de);
    CHECK(t != NULL && strstr(t, "sh") != NULL); /* found via PATH */
    free(t);
    ea_de_clear(&de);

    CHECK(ea_de_parse_string("[Desktop Entry]\nExec=/no/such/prog\n"
                             "X-ExtApp-Target=/no/such/target\n",
                             &de) == EXTAPP_OK);
    t = ea_de_resolve_target(&de);
    CHECK_STR(t, "/no/such/target");
    free(t);
    ea_de_clear(&de);
}

static void test_slug(void)
{
    char *s = ea_slugify("  My Cool App!! 2 ");
    CHECK_STR(s, "my-cool-app-2");
    free(s);
    s = ea_slugify("???");
    CHECK_STR(s, "app");
    free(s);
}

int main(void)
{
    test_parse();
    test_quote_roundtrip();
    test_exec_split();
    test_escape();
    test_resolve_target();
    test_slug();
    TEST_DONE();
}
