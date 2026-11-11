/* Icon search: query helpers and the multi-provider front door.
 *
 * Every provider runs on its own thread; results are merged in a fixed
 * order (application catalogues first, then generic logos) so the most
 * likely correct icon comes first.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "private.h"

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_MAX_RESULTS 40

static const EaIconProvider *const providers[] = {
    &ea_provider_appimagehub,
    &ea_provider_flathub,
    &ea_provider_commons,
};
#define N_PROVIDERS (sizeof providers / sizeof providers[0])

/* ---- query helpers ----------------------------------------------------- */

static bool is_noise_word(const char *w)
{
    static const char *const noise[] = {
        "icon", "icons", "logo", "logos", "app", "apps", "application",
        "appimage", "png", "svg", "image", "desktop", "linux", NULL};
    for (const char *const *n = noise; *n; n++)
        if (strcmp(w, *n) == 0)
            return true;
    return false;
}

char **ea_icon_query_terms(const char *query)
{
    size_t n = 0, cap = 4;
    char **terms = calloc(cap + 1, sizeof *terms);
    if (!terms)
        return NULL;
    EaStrBuf tok = {0};
    for (const unsigned char *p = (const unsigned char *)(query ? query : "");; p++) {
        bool word = *p && (isalnum(*p) || *p >= 0x80);
        if (word) {
            ea_sb_append_c(&tok, (char)tolower(*p));
            continue;
        }
        if (tok.len > 0) {
            char *t = ea_sb_steal(&tok);
            bool keep = t && !is_noise_word(t);
            for (size_t i = 0; keep && i < n; i++)
                keep = strcmp(terms[i], t) != 0; /* drop duplicates */
            if (keep) {
                if (n == cap) {
                    char **nt = realloc(terms, (cap * 2 + 1) * sizeof *terms);
                    if (!nt) {
                        free(t);
                        ea_strv_free(terms);
                        return NULL;
                    }
                    terms = nt;
                    cap *= 2;
                }
                terms[n++] = t;
                terms[n] = NULL;
            } else {
                free(t);
            }
        }
        if (!*p)
            break;
    }
    ea_sb_clear(&tok);
    return terms;
}

char *ea_icon_terms_join(char **terms)
{
    EaStrBuf sb = {0};
    ea_sb_append(&sb, "");
    for (char **t = terms; t && *t; t++) {
        if (t != terms)
            ea_sb_append_c(&sb, ' ');
        ea_sb_append(&sb, *t);
    }
    return ea_sb_steal(&sb);
}

/* lower-case copy keeping only alnum/UTF-8 bytes, words separated by ' ' */
static char *normalize(const char *s)
{
    EaStrBuf sb = {0};
    ea_sb_append(&sb, "");
    bool space = false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || *p >= 0x80) {
            if (space && sb.len > 0)
                ea_sb_append_c(&sb, ' ');
            space = false;
            ea_sb_append_c(&sb, (char)tolower(*p));
        } else {
            space = true;
        }
    }
    return ea_sb_steal(&sb);
}

/* true if `term` starts one of the space separated words of `norm` */
static bool starts_a_word(const char *norm, const char *term)
{
    size_t lt = strlen(term);
    for (const char *w = norm; *w;) {
        if (strncmp(w, term, lt) == 0)
            return true;
        const char *sp = strchr(w, ' ');
        if (!sp)
            break;
        w = sp + 1;
    }
    return false;
}

int ea_icon_name_score(const char *name, char **terms)
{
    if (!name || !terms || !terms[0])
        return 0;
    char *n = normalize(name);
    char *q = ea_icon_terms_join(terms);
    int score = 0;
    if (n && q) {
        size_t lq = strlen(q);
        if (strcmp(n, q) == 0) {
            score = 100;                         /* "obsidian" == "obsidian" */
        } else if (strncmp(n, q, lq) == 0 && n[lq] == ' ') {
            score = 80;                          /* "gimp" -> "gimp portable" */
        } else {
            /* every term must start a word: "lm" matches "LM Studio" but
             * not "Elmer Studio" */
            bool all = true;
            for (char **t = terms; *t && all; t++)
                all = starts_a_word(n, *t);
            if (all)
                score = strncmp(n, terms[0], strlen(terms[0])) == 0 ? 60 : 40;
        }
    }
    free(n);
    free(q);
    return score;
}

/* ---- result lists ------------------------------------------------------ */

static void result_clear(ExtAppIconResult *r)
{
    free(r->provider);
    free(r->image_url);
    free(r->thumbnail_url);
    free(r->title);
    free(r->source);
    memset(r, 0, sizeof *r);
}

void ea_icon_list_clear(ExtAppIconResultList *l)
{
    for (size_t i = 0; i < l->count; i++)
        result_clear(&l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

void extapp_icon_result_list_free(ExtAppIconResultList *list)
{
    if (!list)
        return;
    ea_icon_list_clear(list);
    free(list);
}

bool ea_icon_list_push(ExtAppIconResultList *l, const char *provider,
                       const char *image_url, const char *thumbnail_url,
                       const char *title, const char *source, int width,
                       int height, bool scalable)
{
    if (!image_url && !thumbnail_url)
        return false;
    /* grow one by one: lists are small (tens of items) */
    ExtAppIconResult *items = realloc(l->items, (l->count + 1) * sizeof *items);
    if (!items)
        return false;
    l->items = items;
    ExtAppIconResult *r = &l->items[l->count];
    memset(r, 0, sizeof *r);
    r->provider = ea_strdup0(provider);
    r->image_url = ea_strdup0(image_url ? image_url : thumbnail_url);
    r->thumbnail_url = ea_strdup0(thumbnail_url ? thumbnail_url : image_url);
    r->title = ea_strdup0(title);
    r->source = ea_strdup0(source);
    r->width = width;
    r->height = height;
    r->scalable = scalable;
    if (!r->image_url || !r->thumbnail_url || (provider && !r->provider) ||
        (title && !r->title) || (source && !r->source)) {
        result_clear(r);
        return false;
    }
    l->count++;
    return true;
}

/* ---- parallel search --------------------------------------------------- */

typedef struct {
    const EaIconProvider *provider;
    ExtAppContext *ctx;
    const char *query;
    size_t max;
    ExtAppCancelFn cancel;
    void *ud;
    ExtAppIconResultList list;
    char *err;
    ExtAppStatus st;
} Job;

static void *run_job(void *arg)
{
    Job *j = arg;
    j->st = j->provider->search(j->ctx, j->query, j->max, j->cancel, j->ud,
                                &j->list, &j->err);
    if (j->st != EXTAPP_OK)
        ea_icon_list_clear(&j->list);
    return NULL;
}

static bool already_listed(const ExtAppIconResultList *l, const char *url)
{
    for (size_t i = 0; i < l->count; i++)
        if (strcmp(l->items[i].image_url, url) == 0)
            return true;
    return false;
}

#define MAX_PROVIDERS 8

ExtAppStatus ea_icon_search_with(const EaIconProvider *const *provs,
                                 size_t n_provs, ExtAppContext *ctx,
                                 const char *query, size_t max_results,
                                 ExtAppCancelFn cancel, void *user_data,
                                 ExtAppIconResultList **out, char **err)
{
    if (out)
        *out = NULL;
    if (!ctx || !query || !out || !provs || n_provs > MAX_PROVIDERS) {
        ea_set_err(err, "Invalid arguments");
        return EXTAPP_ERR_INVALID_ARG;
    }
    char **terms = ea_icon_query_terms(query);
    if (!terms)
        return EXTAPP_ERR_NOMEM;
    bool empty = !terms[0];
    ea_strv_free(terms);
    if (empty) {
        ea_set_err(err, "Enter the name of an application to search for");
        return EXTAPP_ERR_INVALID_ARG;
    }
    if (!max_results)
        max_results = DEFAULT_MAX_RESULTS;

    Job jobs[MAX_PROVIDERS];
    pthread_t threads[MAX_PROVIDERS];
    bool started[MAX_PROVIDERS];
    for (size_t i = 0; i < n_provs; i++) {
        jobs[i] = (Job){.provider = provs[i], .ctx = ctx, .query = query,
                        .max = max_results, .cancel = cancel, .ud = user_data};
        started[i] = pthread_create(&threads[i], NULL, run_job, &jobs[i]) == 0;
        if (!started[i])
            run_job(&jobs[i]); /* no thread available: run inline */
    }
    for (size_t i = 0; i < n_provs; i++)
        if (started[i])
            pthread_join(threads[i], NULL);

    ExtAppIconResultList *merged = calloc(1, sizeof *merged);
    ExtAppStatus rc = merged ? EXTAPP_OK : EXTAPP_ERR_NOMEM;
    size_t ok = 0;
    bool cancelled = false;
    EaStrBuf errors = {0};
    for (size_t i = 0; i < n_provs; i++) {
        Job *j = &jobs[i];
        if (j->st == EXTAPP_OK) {
            ok++;
        } else {
            cancelled |= j->st == EXTAPP_ERR_CANCELLED;
            ea_sb_printf(&errors, "%s%s: %s", errors.len ? "; " : "",
                         j->provider->name,
                         j->err ? j->err : extapp_status_str(j->st));
        }
        for (size_t k = 0; merged && k < j->list.count; k++) {
            const ExtAppIconResult *r = &j->list.items[k];
            if (merged->count >= max_results || already_listed(merged, r->image_url))
                continue;
            if (!ea_icon_list_push(merged, r->provider, r->image_url,
                                   r->thumbnail_url, r->title, r->source,
                                   r->width, r->height, r->scalable))
                rc = EXTAPP_ERR_NOMEM;
        }
        ea_icon_list_clear(&j->list);
        free(j->err);
    }

    if (rc == EXTAPP_OK && cancelled && (!cancel || cancel(user_data)))
        rc = EXTAPP_ERR_CANCELLED;
    else if (rc == EXTAPP_OK && merged->count == 0 && ok < n_provs)
        rc = EXTAPP_ERR_NETWORK; /* nothing found and something broke */

    if (rc != EXTAPP_OK) {
        if (rc == EXTAPP_ERR_CANCELLED)
            ea_set_err(err, "Search cancelled");
        else if (rc == EXTAPP_ERR_NETWORK)
            ea_set_err(err, "No icon source could be searched (%s)",
                       errors.data ? errors.data : "unknown error");
        else
            ea_set_err(err, "Out of memory");
        extapp_icon_result_list_free(merged);
        ea_sb_clear(&errors);
        return rc;
    }
    ea_sb_clear(&errors);
    *out = merged;
    return EXTAPP_OK;
}

ExtAppStatus extapp_icon_search(ExtAppContext *ctx, const char *query,
                                size_t max_results, ExtAppCancelFn cancel,
                                void *user_data, ExtAppIconResultList **out,
                                char **err)
{
    if (out)
        *out = NULL;
    if (!query || !*query) {
        ea_set_err(err, "Enter the name of an application to search for");
        return EXTAPP_ERR_INVALID_ARG;
    }
    return ea_icon_search_with(providers, N_PROVIDERS, ctx, query, max_results,
                               cancel, user_data, out, err);
}
