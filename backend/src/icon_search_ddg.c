/* Icon search provider: DuckDuckGo Images.
 *
 * DuckDuckGo has no official image API. This uses the same two requests its
 * web UI makes: fetch a "vqd" token from the search page, then query i.js.
 * It may break if DuckDuckGo changes things — which is why it lives behind
 * the EaIconProvider table and can be swapped for another provider.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "private.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define DDG_MAX_PAGE (2u * 1024u * 1024u)

char *ea_ddg_extract_vqd(const char *html)
{
    if (!html)
        return NULL;
    for (const char *p = strstr(html, "vqd"); p; p = strstr(p + 3, "vqd")) {
        const char *q = p + 3;
        /* accept vqd="…", vqd='…', vqd=…&, "vqd":"…" */
        if (*q == '"')
            q++;
        while (*q == ' ')
            q++;
        if (*q != '=' && *q != ':')
            continue;
        q++;
        while (*q == ' ')
            q++;
        if (*q == '"' || *q == '\'')
            q++;
        const char *start = q;
        while (isalnum((unsigned char)*q) || *q == '-' || *q == '_')
            q++;
        if (q - start >= 5)
            return strndup(start, (size_t)(q - start));
    }
    return NULL;
}

static char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(it) && it->valuestring ? strdup(it->valuestring) : NULL;
}

static int json_int(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it))
        return it->valueint;
    if (cJSON_IsString(it) && it->valuestring)
        return atoi(it->valuestring);
    return 0;
}

static void result_clear(ExtAppIconResult *r)
{
    free(r->image_url);
    free(r->thumbnail_url);
    free(r->title);
    free(r->source);
    memset(r, 0, sizeof *r);
}

void extapp_icon_result_list_free(ExtAppIconResultList *list)
{
    if (!list)
        return;
    for (size_t i = 0; i < list->count; i++)
        result_clear(&list->items[i]);
    free(list->items);
    free(list);
}

ExtAppStatus ea_ddg_parse_results(const char *json, size_t max_results,
                                  ExtAppIconResultList *out, char **err)
{
    memset(out, 0, sizeof *out);
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ea_set_err(err, "DuckDuckGo returned an unexpected (non-JSON) answer");
        return EXTAPP_ERR_PARSE;
    }
    /* DuckDuckGo answers automated-looking traffic with a bot challenge.
     * We do not try to solve it: report it clearly instead. */
    const cJSON *rtype = cJSON_GetObjectItemCaseSensitive(root, "response_type");
    if (cJSON_GetObjectItemCaseSensitive(root, "challenge") ||
        (cJSON_IsString(rtype) && strstr(rtype->valuestring, "challenge"))) {
        cJSON_Delete(root);
        ea_set_err(err, "DuckDuckGo is temporarily blocking automated image "
                        "searches from this network. Try again later, or "
                        "choose an icon from a file.");
        return EXTAPP_ERR_NETWORK;
    }
    const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    if (!cJSON_IsArray(results)) {
        cJSON_Delete(root);
        ea_set_err(err, "DuckDuckGo answer has no results");
        return EXTAPP_ERR_PARSE;
    }

    size_t n = (size_t)cJSON_GetArraySize(results);
    if (max_results && n > max_results)
        n = max_results;
    out->items = n ? calloc(n, sizeof *out->items) : NULL;
    if (n && !out->items) {
        cJSON_Delete(root);
        return EXTAPP_ERR_NOMEM;
    }

    const cJSON *it;
    cJSON_ArrayForEach(it, results) {
        if (out->count >= n)
            break;
        ExtAppIconResult *r = &out->items[out->count];
        r->image_url = json_str(it, "image");
        r->thumbnail_url = json_str(it, "thumbnail");
        r->title = json_str(it, "title");
        r->source = json_str(it, "url");
        r->width = json_int(it, "width");
        r->height = json_int(it, "height");
        if (!r->image_url && !r->thumbnail_url) {
            result_clear(r);
            continue;
        }
        /* DuckDuckGo sometimes uses protocol-relative thumbnail URLs */
        if (r->thumbnail_url && strncmp(r->thumbnail_url, "//", 2) == 0) {
            char *fixed = ea_strdup_printf("https:%s", r->thumbnail_url);
            free(r->thumbnail_url);
            r->thumbnail_url = fixed;
        }
        out->count++;
    }
    cJSON_Delete(root);
    return EXTAPP_OK;
}

static ExtAppStatus ddg_search(ExtAppContext *ctx, const char *query,
                               size_t max_results, ExtAppCancelFn cancel,
                               void *user_data, ExtAppIconResultList *out,
                               char **err)
{
    char *q = ea_url_encode(query);
    if (!q)
        return EXTAPP_ERR_NOMEM;

    /* 1) token */
    char *url = ea_strdup_printf(
        "https://duckduckgo.com/?q=%s&iax=images&ia=images", q);
    static const char *const page_headers[] = {
        "Accept: text/html,application/xhtml+xml",
        "Accept-Language: en-US,en;q=0.8", NULL};
    EaHttpOptions o1 = {.headers = page_headers, .https_only = true,
                        .max_bytes = DDG_MAX_PAGE, .timeout_sec = 15};
    char *html = NULL;
    ExtAppStatus st = url ? ea_http_get(ctx, url, &o1, cancel, user_data,
                                        &html, NULL, NULL, err)
                          : EXTAPP_ERR_NOMEM;
    free(url);
    if (st != EXTAPP_OK) {
        free(q);
        return st;
    }
    char *vqd = ea_ddg_extract_vqd(html);
    free(html);
    if (!vqd) {
        free(q);
        ea_set_err(err, "Could not obtain a search token from DuckDuckGo "
                        "(the service may have changed or rate-limited us)");
        return EXTAPP_ERR_NETWORK;
    }

    /* 2) results */
    url = ea_strdup_printf("https://duckduckgo.com/i.js?l=us-en&o=json&q=%s"
                           "&vqd=%s&f=,,,,,&p=1", q, vqd);
    free(vqd);
    free(q);
    static const char *const json_headers[] = {
        "Accept: application/json, text/javascript, */*; q=0.01",
        "Accept-Language: en-US,en;q=0.8",
        "Referer: https://duckduckgo.com/",
        "X-Requested-With: XMLHttpRequest", NULL};
    EaHttpOptions o2 = {.headers = json_headers, .https_only = true,
                        .max_bytes = DDG_MAX_PAGE, .timeout_sec = 15};
    char *json = NULL;
    long code = 0;
    st = url ? ea_http_get(ctx, url, &o2, cancel, user_data, &json, NULL,
                           &code, err)
             : EXTAPP_ERR_NOMEM;
    free(url);
    if (st != EXTAPP_OK) {
        if (code == 403)
            ea_set_err(err, "DuckDuckGo refused the image search (HTTP 403). "
                            "Try again in a little while.");
        return st;
    }
    st = ea_ddg_parse_results(json, max_results, out, err);
    free(json);
    return st;
}

const EaIconProvider ea_ddg_provider = {
    .name = "DuckDuckGo",
    .search = ddg_search,
};

ExtAppStatus extapp_icon_search(ExtAppContext *ctx, const char *query,
                                size_t max_results, ExtAppCancelFn cancel,
                                void *user_data, ExtAppIconResultList **out,
                                char **err)
{
    if (!ctx || !query || !*query || !out) {
        ea_set_err(err, "A search query is required");
        return EXTAPP_ERR_INVALID_ARG;
    }
    *out = calloc(1, sizeof **out);
    if (!*out)
        return EXTAPP_ERR_NOMEM;
    ExtAppStatus st = ea_ddg_provider.search(ctx, query, max_results, cancel,
                                             user_data, *out, err);
    if (st != EXTAPP_OK) {
        extapp_icon_result_list_free(*out);
        *out = NULL;
    }
    return st;
}
