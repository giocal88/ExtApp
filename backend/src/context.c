/* Context: resolves the XDG directories ExtApp works with.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "private.h"

#include <pthread.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;

static void init_curl(void)
{
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

static char *default_data_home(void)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] == '/')
        return strdup(xdg);

    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
    if (!home)
        return NULL;
    return ea_path_join(home, ".local/share");
}

ExtAppContext *extapp_context_new_with_data_dir(const char *data_home)
{
    if (!data_home || data_home[0] != '/')
        return NULL;

    pthread_once(&curl_once, init_curl);

    ExtAppContext *ctx = calloc(1, sizeof *ctx);
    if (!ctx)
        return NULL;
    ctx->data_home = strdup(data_home);
    ctx->applications_dir = ea_path_join(data_home, "applications");
    ctx->icons_dir = ea_path_join(data_home, "extapp/icons");
    ctx->user_agent = strdup(
        "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
    if (!ctx->data_home || !ctx->applications_dir || !ctx->icons_dir ||
        !ctx->user_agent) {
        extapp_context_free(ctx);
        return NULL;
    }
    return ctx;
}

ExtAppContext *extapp_context_new(void)
{
    char *dh = default_data_home();
    if (!dh)
        return NULL;
    ExtAppContext *ctx = extapp_context_new_with_data_dir(dh);
    free(dh);
    return ctx;
}

void extapp_context_free(ExtAppContext *ctx)
{
    if (!ctx)
        return;
    free(ctx->data_home);
    free(ctx->applications_dir);
    free(ctx->icons_dir);
    free(ctx->user_agent);
    free(ctx);
}

const char *extapp_context_applications_dir(const ExtAppContext *ctx)
{
    return ctx ? ctx->applications_dir : NULL;
}

const char *extapp_context_icons_dir(const ExtAppContext *ctx)
{
    return ctx ? ctx->icons_dir : NULL;
}
