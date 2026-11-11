/* Icon provider: AppImageHub (https://appimage.github.io).
 *
 * The whole catalogue is one JSON feed (~2.5 MB, ~3700 AppImages). It is
 * cached in the cache dir and refreshed weekly; a stale copy is used when
 * the network is unavailable. Matching is done locally on the app name.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"

#define FEED_URL       "https://appimage.github.io/feed.json"
#define DATABASE_URL   "https://appimage.github.io/database/"
#define PAGE_URL       "https://appimage.github.io/"
#define FEED_MAX_BYTES (32u * 1024u * 1024u)
#define FEED_MAX_AGE   (7 * 24 * 60 * 60)
#define MAX_MATCHES    6

/* Pick the best icon of an app: plain SVG, else the largest NxN raster.
 * Formats ExtApp cannot store are skipped — notably .svgz, which GitHub
 * Pages serves as gzip data. */
static const char *best_icon(const cJSON *icons, int *size, bool *scalable)
{
    const char *best = NULL;
    int best_size = -1;
    const cJSON *it;
    cJSON_ArrayForEach(it, icons) {
        if (!cJSON_IsString(it) || !it->valuestring || !*it->valuestring)
            continue;
        const char *p = it->valuestring;
        /* only relative paths inside the database are expected */
        if (strstr(p, "..") || strstr(p, "://") || p[0] == '/')
            continue;
        int s = 0;
        if (ea_has_suffix_ci(p, ".svg")) {
            s = 100000; /* scalable wins */
        } else if (!ea_has_suffix_ci(p, ".png") && !ea_has_suffix_ci(p, ".jpg") &&
                   !ea_has_suffix_ci(p, ".jpeg") && !ea_has_suffix_ci(p, ".webp") &&
                   !ea_has_suffix_ci(p, ".ico")) {
            continue; /* .svgz, .xpm, ... */
        } else {
            const char *x = strstr(p, "/icons/");
            int w = 0, h = 0;
            if (x && sscanf(x, "/icons/%dx%d/", &w, &h) == 2)
                s = w;
        }
        if (s > best_size) {
            best = p;
            best_size = s;
        }
    }
    if (size)
        *size = best_size >= 100000 ? 0 : (best_size > 0 ? best_size : 0);
    if (scalable)
        *scalable = best_size >= 100000;
    return best;
}

/* Percent-encode for a URL path (spaces become %20, not '+'). */
static char *path_encode(const char *s, bool keep_slash)
{
    EaStrBuf sb = {0};
    ea_sb_append(&sb, "");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || strchr("-_.~", *p) ||
            (keep_slash && *p == '/'))
            ea_sb_append_c(&sb, (char)*p);
        else
            ea_sb_printf(&sb, "%%%02X", *p);
    }
    return ea_sb_steal(&sb);
}

typedef struct {
    const char *name;
    const char *icon;
    int size;
    bool scalable;
    int score;
    size_t order;
} Match;

static int cmp_match(const void *a, const void *b)
{
    const Match *x = a, *y = b;
    if (x->score != y->score)
        return y->score - x->score;
    return x->order < y->order ? -1 : x->order > y->order;
}

ExtAppStatus ea_appimagehub_parse(const char *feed_json, char **terms,
                                  size_t max_results,
                                  ExtAppIconResultList *out, char **err)
{
    memset(out, 0, sizeof *out);
    if (!terms || !terms[0])
        return EXTAPP_OK;

    cJSON *root = cJSON_Parse(feed_json);
    const cJSON *items = root ? cJSON_GetObjectItemCaseSensitive(root, "items") : NULL;
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        ea_set_err(err, "Unexpected AppImageHub catalogue format");
        return EXTAPP_ERR_PARSE;
    }

    Match top[MAX_MATCHES + 1];
    size_t ntop = 0, order = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, items) {
        order++;
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(it, "name");
        if (!cJSON_IsString(name) || !name->valuestring)
            continue;
        int score = ea_icon_name_score(name->valuestring, terms);
        if (score <= 0)
            continue;
        int size = 0;
        bool scalable = false;
        const char *icon = best_icon(cJSON_GetObjectItemCaseSensitive(it, "icons"),
                                     &size, &scalable);
        if (!icon)
            continue;
        /* keep the MAX_MATCHES best (insertion into a tiny sorted array) */
        top[ntop++] = (Match){name->valuestring, icon, size, scalable, score, order};
        qsort(top, ntop, sizeof *top, cmp_match);
        if (ntop > MAX_MATCHES)
            ntop = MAX_MATCHES;
    }

    ExtAppStatus st = EXTAPP_OK;
    for (size_t i = 0; i < ntop && (!max_results || out->count < max_results); i++) {
        char *icon = path_encode(top[i].icon, true);
        char *url = icon ? ea_strdup_printf(DATABASE_URL "%s", icon) : NULL;
        free(icon);
        char *enc = path_encode(top[i].name, false);
        char *page = enc ? ea_strdup_printf(PAGE_URL "%s/", enc) : NULL;
        if (!url || !page ||
            !ea_icon_list_push(out, ea_provider_appimagehub.name, url, url,
                               top[i].name, page, top[i].size, top[i].size,
                               top[i].scalable))
            st = EXTAPP_ERR_NOMEM;
        free(url);
        free(enc);
        free(page);
        if (st != EXTAPP_OK)
            break;
    }
    cJSON_Delete(root);
    if (st != EXTAPP_OK)
        ea_icon_list_clear(out);
    return st;
}

/* ---- feed cache -------------------------------------------------------- */

static ExtAppStatus parse_cached(const char *path, char **terms, size_t max,
                                 ExtAppIconResultList *out)
{
    char *json = NULL;
    ExtAppStatus rc = ea_read_file(path, FEED_MAX_BYTES, &json, NULL, NULL);
    if (rc == EXTAPP_OK)
        rc = ea_appimagehub_parse(json, terms, max, out, NULL);
    free(json);
    return rc;
}

/* 1. use a fresh cached catalogue if it parses;
 * 2. otherwise download it, and cache it only if it parses (a captive
 *    portal page or truncated file must never replace a good copy);
 * 3. if that fails, fall back to an older cached copy. */
static ExtAppStatus appimagehub_search(ExtAppContext *ctx, const char *query,
                                       size_t max_results, ExtAppCancelFn cancel,
                                       void *user_data, ExtAppIconResultList *out,
                                       char **err)
{
    char **terms = ea_icon_query_terms(query);
    if (!terms)
        return EXTAPP_ERR_NOMEM;
    char *path = terms[0] ? ea_path_join(ctx->cache_dir, "appimagehub-feed.json") : NULL;
    if (!terms[0] || !path) {
        ea_strv_free(terms);
        return terms[0] ? EXTAPP_ERR_NOMEM : EXTAPP_OK;
    }

    struct stat st;
    bool cached = stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
    bool fresh = cached && time(NULL) - st.st_mtime < FEED_MAX_AGE;
    ExtAppStatus rc = EXTAPP_ERR_NOT_FOUND;
    if (fresh) {
        rc = parse_cached(path, terms, max_results, out);
        if (rc != EXTAPP_OK)
            ea_icon_list_clear(out); /* broken cache: download again */
    }

    char *dl_err = NULL;
    if (rc != EXTAPP_OK) {
        EaHttpOptions o = {.https_only = true, .max_bytes = FEED_MAX_BYTES,
                           .timeout_sec = 30};
        size_t len = 0;
        char *body = NULL;
        rc = ea_http_get(ctx, FEED_URL, &o, cancel, user_data, &body, &len,
                         NULL, &dl_err);
        if (rc == EXTAPP_OK) {
            rc = ea_appimagehub_parse(body, terms, max_results, out, &dl_err);
            /* best effort: a failing cache write must not fail the search */
            if (rc == EXTAPP_OK && ea_mkdir_p(ctx->cache_dir, NULL) == EXTAPP_OK)
                ea_write_file_atomic(path, body, len, 0644, NULL);
        }
        free(body);
    }

    if (rc != EXTAPP_OK && rc != EXTAPP_ERR_CANCELLED && cached && !fresh) {
        ea_icon_list_clear(out);
        if (parse_cached(path, terms, max_results, out) == EXTAPP_OK)
            rc = EXTAPP_OK; /* offline or bad download: use the older copy */
        else
            ea_icon_list_clear(out);
    }

    if (rc != EXTAPP_OK) {
        ea_icon_list_clear(out);
        ea_set_err(err, "%s", dl_err ? dl_err : "AppImageHub catalogue unavailable");
    }
    free(dl_err);
    free(path);
    ea_strv_free(terms);
    return rc;
}

const EaIconProvider ea_provider_appimagehub = {
    .name = "AppImageHub",
    .search = appimagehub_search,
};
