/* Icon provider: Flathub (https://flathub.org) search API.
 *
 * Returns the official 128x128 icon of matching Flatpak applications —
 * many apps distributed as AppImages are also on Flathub.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define SEARCH_URL     "https://flathub.org/api/v2/search"
#define APP_PAGE_URL   "https://flathub.org/apps/"
#define MAX_BYTES      (4u * 1024u * 1024u)
#define MAX_MATCHES    5

static int icon_size_from_url(const char *url)
{
    const char *p = strstr(url, "/icons/");
    int w = 0;
    if (p && sscanf(p, "/icons/%dx", &w) == 1)
        return w;
    return 0;
}

static bool hit_matches(const cJSON *hit, const char *name, char **terms)
{
    if (ea_icon_name_score(name, terms) > 0)
        return true;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(hit, "app_id");
    if (cJSON_IsString(id) && ea_icon_name_score(id->valuestring, terms) > 0)
        return true;
    const cJSON *kw;
    cJSON_ArrayForEach(kw, cJSON_GetObjectItemCaseSensitive(hit, "keywords"))
        if (cJSON_IsString(kw) && ea_icon_name_score(kw->valuestring, terms) > 0)
            return true;
    return false;
}

ExtAppStatus ea_flathub_parse(const char *json, char **terms,
                              size_t max_results, ExtAppIconResultList *out,
                              char **err)
{
    memset(out, 0, sizeof *out);
    cJSON *root = cJSON_Parse(json);
    const cJSON *hits = root ? cJSON_GetObjectItemCaseSensitive(root, "hits") : NULL;
    if (!cJSON_IsArray(hits)) {
        cJSON_Delete(root);
        ea_set_err(err, "Unexpected answer from Flathub");
        return EXTAPP_ERR_PARSE;
    }

    /* Flathub's search is fuzzy and always returns something, even for
     * nonsense. Keep a hit only when the query matches its name, its app id
     * ("gimp" -> org.gimp.GIMP) or one of its keywords ("vscode" ->
     * Visual Studio Code). Flathub's relevance order is preserved. */
    size_t limit = MAX_MATCHES;
    if (max_results && max_results < limit)
        limit = max_results;
    ExtAppStatus st = EXTAPP_OK;
    const cJSON *h;
    cJSON_ArrayForEach(h, hits) {
        if (out->count >= limit)
            break;
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(h, "name");
        const cJSON *icon = cJSON_GetObjectItemCaseSensitive(h, "icon");
        const cJSON *app_id = cJSON_GetObjectItemCaseSensitive(h, "app_id");
        if (!cJSON_IsString(icon) || !icon->valuestring ||
            strncmp(icon->valuestring, "https://", 8) != 0)
            continue;
        const char *n = cJSON_IsString(name) ? name->valuestring : NULL;
        if (!hit_matches(h, n, terms))
            continue;
        char *page = (cJSON_IsString(app_id) && app_id->valuestring)
                         ? ea_strdup_printf(APP_PAGE_URL "%s", app_id->valuestring)
                         : NULL;
        int size = icon_size_from_url(icon->valuestring);
        if (!ea_icon_list_push(out, ea_provider_flathub.name, icon->valuestring,
                               icon->valuestring, n, page, size, size, false))
            st = EXTAPP_ERR_NOMEM;
        free(page);
        if (st != EXTAPP_OK)
            break;
    }
    cJSON_Delete(root);
    if (st != EXTAPP_OK)
        ea_icon_list_clear(out);
    return st;
}

static ExtAppStatus flathub_search(ExtAppContext *ctx, const char *query,
                                   size_t max_results, ExtAppCancelFn cancel,
                                   void *user_data, ExtAppIconResultList *out,
                                   char **err)
{
    char **terms = ea_icon_query_terms(query);
    if (!terms)
        return EXTAPP_ERR_NOMEM;
    if (!terms[0]) {
        ea_strv_free(terms);
        return EXTAPP_OK;
    }

    char *q = ea_icon_terms_join(terms);
    cJSON *req = cJSON_CreateObject();
    char *body = NULL;
    if (q && req && cJSON_AddStringToObject(req, "query", q))
        body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    free(q);
    if (!body) {
        ea_strv_free(terms);
        return EXTAPP_ERR_NOMEM;
    }

    static const char *const headers[] = {"Content-Type: application/json",
                                          "Accept: application/json", NULL};
    EaHttpOptions o = {.headers = headers, .post_body = body, .https_only = true,
                       .max_bytes = MAX_BYTES, .timeout_sec = 15};
    char *json = NULL;
    ExtAppStatus rc = ea_http_get(ctx, SEARCH_URL, &o, cancel, user_data, &json,
                                  NULL, NULL, err);
    cJSON_free(body);
    if (rc == EXTAPP_OK)
        rc = ea_flathub_parse(json, terms, max_results, out, err);
    free(json);
    ea_strv_free(terms);
    return rc;
}

const EaIconProvider ea_provider_flathub = {
    .name = "Flathub",
    .search = flathub_search,
};
