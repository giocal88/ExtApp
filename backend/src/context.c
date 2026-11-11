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

static char *xdg_dir(const char *env, const char *fallback)
{
    const char *xdg = getenv(env);
    if (xdg && xdg[0] == '/')
        return strdup(xdg);

    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
    if (!home)
        return NULL;
    return ea_path_join(home, fallback);
}

static ExtAppContext *context_new(const char *data_home, const char *cache_dir)
{
    if (!data_home || data_home[0] != '/' || !cache_dir)
        return NULL;

    pthread_once(&curl_once, init_curl);

    ExtAppContext *ctx = calloc(1, sizeof *ctx);
    if (!ctx)
        return NULL;
    ctx->data_home = strdup(data_home);
    ctx->applications_dir = ea_path_join(data_home, "applications");
    ctx->icons_dir = ea_path_join(data_home, "extapp/icons");
    ctx->cache_dir = strdup(cache_dir);
    /* Identify honestly: Wikimedia and others ask clients for a descriptive
     * User-Agent with a way to reach the project. */
    ctx->user_agent = strdup("ExtApp/" EXTAPP_VERSION_STRING
                             " (https://codeberg.org/giocal88/ExtApp)");
    if (!ctx->data_home || !ctx->applications_dir || !ctx->icons_dir ||
        !ctx->cache_dir || !ctx->user_agent) {
        extapp_context_free(ctx);
        return NULL;
    }
    return ctx;
}

ExtAppContext *extapp_context_new_with_data_dir(const char *data_home)
{
    if (!data_home || data_home[0] != '/')
        return NULL;
    /* self-contained setup (tests, portable use): cache next to the data */
    char *cache = ea_path_join(data_home, "extapp/cache");
    ExtAppContext *ctx = context_new(data_home, cache);
    free(cache);
    return ctx;
}

ExtAppContext *extapp_context_new(void)
{
    char *dh = xdg_dir("XDG_DATA_HOME", ".local/share");
    char *ch = xdg_dir("XDG_CACHE_HOME", ".cache");
    char *cache = ch ? ea_path_join(ch, "extapp") : NULL;
    ExtAppContext *ctx = dh && cache ? context_new(dh, cache) : NULL;
    free(dh);
    free(ch);
    free(cache);
    return ctx;
}

void extapp_context_free(ExtAppContext *ctx)
{
    if (!ctx)
        return;
    free(ctx->data_home);
    free(ctx->applications_dir);
    free(ctx->icons_dir);
    free(ctx->cache_dir);
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
