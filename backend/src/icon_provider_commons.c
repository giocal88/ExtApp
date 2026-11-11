/* Icon provider: Wikimedia Commons (https://commons.wikimedia.org).
 *
 * Searches the File: namespace for "<name> logo" and returns image URLs
 * plus pre-rendered thumbnails. Wikimedia asks API clients for a
 * descriptive User-Agent, which the context provides.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "private.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define API_URL   "https://commons.wikimedia.org/w/api.php"
#define MAX_BYTES (4u * 1024u * 1024u)
#define MAX_FETCH 20
/* larger rasters are replaced by their 256 px thumbnail */
#define MAX_RASTER_SIDE 512

typedef struct {
    const cJSON *page;
    int index;
} Page;

static int cmp_page(const void *a, const void *b)
{
    const Page *x = a, *y = b;
    return (x->index > y->index) - (x->index < y->index);
}

static const char *str_item(const cJSON *o, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static int int_item(const cJSON *o, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(it) ? it->valueint : 0;
}

static bool usable_mime(const char *mime)
{
    return mime && (strcmp(mime, "image/png") == 0 ||
                    strcmp(mime, "image/svg+xml") == 0 ||
                    strcmp(mime, "image/jpeg") == 0 ||
                    strcmp(mime, "image/webp") == 0);
}

ExtAppStatus ea_commons_parse(const char *json, char **terms,
                              size_t max_results, ExtAppIconResultList *out,
                              char **err)
{
    memset(out, 0, sizeof *out);
    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ea_set_err(err, "Unexpected answer from Wikimedia Commons");
        return EXTAPP_ERR_PARSE;
    }
    const cJSON *apierr = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (apierr) {
        const char *info = str_item(apierr, "info");
        ea_set_err(err, "Wikimedia Commons: %s", info ? info : "API error");
        cJSON_Delete(root);
        return EXTAPP_ERR_NETWORK;
    }
    /* no "query" member simply means no results */
    const cJSON *query = cJSON_GetObjectItemCaseSensitive(root, "query");
    const cJSON *pages = query ? cJSON_GetObjectItemCaseSensitive(query, "pages") : NULL;
    int n = pages ? cJSON_GetArraySize(pages) : 0; /* array (formatversion=2) or object */
    if (n <= 0) {
        cJSON_Delete(root);
        return EXTAPP_OK;
    }

    Page *sorted = calloc((size_t)n, sizeof *sorted);
    if (!sorted) {
        cJSON_Delete(root);
        return EXTAPP_ERR_NOMEM;
    }
    size_t count = 0;
    const cJSON *pg;
    cJSON_ArrayForEach(pg, pages) {
        sorted[count].page = pg;
        sorted[count].index = int_item(pg, "index");
        count++;
    }
    qsort(sorted, count, sizeof *sorted, cmp_page);

    ExtAppStatus st = EXTAPP_OK;
    for (size_t i = 0; i < count && (!max_results || out->count < max_results); i++) {
        const cJSON *infos = cJSON_GetObjectItemCaseSensitive(sorted[i].page, "imageinfo");
        const cJSON *info = cJSON_IsArray(infos) ? cJSON_GetArrayItem(infos, 0) : NULL;
        if (!info)
            continue;
        const char *mime = str_item(info, "mime");
        const char *url = str_item(info, "url");
        const char *thumb = str_item(info, "thumburl");
        if (!usable_mime(mime) || !url || strncmp(url, "https://", 8) != 0)
            continue;
        if (thumb && strncmp(thumb, "https://", 8) != 0)
            thumb = NULL;
        int w = int_item(info, "width"), h = int_item(info, "height");
        bool svg = strcmp(mime, "image/svg+xml") == 0;
        const char *image = url;
        if (!svg && thumb && (w > MAX_RASTER_SIDE || h > MAX_RASTER_SIDE)) {
            /* big photo-sized rasters: store the thumbnail instead. Commons
             * rounds thumbnails up to standard widths (asking for 256 may
             * give 330 px), so its reported size is not trustworthy. */
            image = thumb;
            w = h = 0;
        }
        const char *title = str_item(sorted[i].page, "title");
        if (title && strncmp(title, "File:", 5) == 0)
            title += 5;
        /* Commons search is full text: drop files whose name does not
         * mention the application (photos, unrelated documents...) */
        if (ea_icon_name_score(title, terms) <= 0)
            continue;
        if (!ea_icon_list_push(out, ea_provider_commons.name, image,
                               thumb ? thumb : image, title,
                               str_item(info, "descriptionurl"),
                               svg ? 0 : w, svg ? 0 : h, svg)) {
            st = EXTAPP_ERR_NOMEM;
            break;
        }
    }
    free(sorted);
    cJSON_Delete(root);
    if (st != EXTAPP_OK)
        ea_icon_list_clear(out);
    return st;
}

static ExtAppStatus commons_search(ExtAppContext *ctx, const char *query,
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
    char *joined = ea_icon_terms_join(terms);
    char *search = joined ? ea_strdup_printf("%s logo", joined) : NULL;
    char *enc = search ? ea_url_encode(search) : NULL;
    size_t limit = max_results && max_results < MAX_FETCH ? max_results : MAX_FETCH;
    char *url = enc ? ea_strdup_printf(
        API_URL "?action=query&format=json&formatversion=2"
        "&generator=search&gsrnamespace=6&gsrsearch=%s&gsrlimit=%zu"
        "&prop=imageinfo&iiprop=url%%7Csize%%7Cmime&iiurlwidth=256",
        enc, limit) : NULL;
    free(joined);
    free(search);
    free(enc);
    if (!url) {
        ea_strv_free(terms);
        return EXTAPP_ERR_NOMEM;
    }

    static const char *const headers[] = {"Accept: application/json", NULL};
    EaHttpOptions o = {.headers = headers, .https_only = true,
                       .max_bytes = MAX_BYTES, .timeout_sec = 15};
    char *json = NULL;
    ExtAppStatus rc = ea_http_get(ctx, url, &o, cancel, user_data, &json, NULL,
                                  NULL, err);
    free(url);
    if (rc == EXTAPP_OK)
        rc = ea_commons_parse(json, terms, max_results, out, err);
    free(json);
    ea_strv_free(terms);
    return rc;
}

const EaIconProvider ea_provider_commons = {
    .name = "Wikimedia Commons",
    .search = commons_search,
};
