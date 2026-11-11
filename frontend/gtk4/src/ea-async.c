/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ea-async.h"
#include "ea-objects.h"

#include <stdlib.h>

G_DEFINE_QUARK(ea-error-quark, ea_error)

static ExtAppContext *backend_ctx;

void ea_backend_init(void)
{
    if (!backend_ctx)
        backend_ctx = extapp_context_new();
    if (!backend_ctx)
        g_error("Cannot initialise the ExtApp backend (is $HOME set?)");
}

ExtAppContext *ea_backend(void)
{
    return backend_ctx;
}

void ea_backend_shutdown(void)
{
    g_clear_pointer(&backend_ctx, extapp_context_free);
}

GError *ea_error_from_status(ExtAppStatus st, char *msg)
{
    GError *e;
    if (st == EXTAPP_ERR_CANCELLED)
        e = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                msg ? msg : "Cancelled");
    else
        e = g_error_new_literal(EA_ERROR, (int)st,
                                msg ? msg : extapp_status_str(st));
    free(msg);
    return e;
}

static bool cancel_cb(void *ud)
{
    return ud && g_cancellable_is_cancelled(G_CANCELLABLE(ud));
}

/* ---- scan -------------------------------------------------------------- */

typedef struct {
    char *dir;
    ExtAppScanOptions opts;
    gint *progress; /* atomic rc box */
} ScanData;

static void scan_data_free(gpointer p)
{
    ScanData *d = p;
    g_free(d->dir);
    if (d->progress)
        g_atomic_rc_box_release(d->progress);
    g_free(d);
}

static void scan_progress(const char *path, void *ud)
{
    (void)path;
    ScanData *d = ud;
    if (d->progress)
        g_atomic_int_inc(d->progress);
}

/* the backend gets one user_data for both callbacks: bundle them */
typedef struct {
    ScanData *data;
    GCancellable *cancellable;
} ScanCb;

static void scan_progress_tramp(const char *path, void *ud)
{
    scan_progress(path, ((ScanCb *)ud)->data);
}

static bool scan_cancel_tramp(void *ud)
{
    return cancel_cb(((ScanCb *)ud)->cancellable);
}

static void scan_thread(GTask *task, gpointer src, gpointer task_data,
                        GCancellable *cancellable)
{
    (void)src;
    ScanData *d = task_data;
    ScanCb cb = {d, cancellable};
    ExtAppCandidateList *list = NULL;
    char *err = NULL;
    ExtAppStatus st = extapp_scan(ea_backend(), d->dir, &d->opts,
                                  scan_progress_tramp, scan_cancel_tramp, &cb,
                                  &list, &err);
    if (st != EXTAPP_OK) {
        g_task_return_error(task, ea_error_from_status(st, err));
        return;
    }
    GListStore *store = g_list_store_new(EA_TYPE_CANDIDATE);
    for (size_t i = 0; i < list->count; i++) {
        EaCandidate *c = ea_candidate_new(&list->items[i]);
        g_list_store_append(store, c);
        g_object_unref(c);
    }
    extapp_candidate_list_free(list);
    g_task_return_pointer(task, store, g_object_unref);
}

void ea_scan_async(const char *dir, const ExtAppScanOptions *opts,
                   gint *progress, GCancellable *cancellable,
                   GAsyncReadyCallback cb, gpointer user_data)
{
    ScanData *d = g_new0(ScanData, 1);
    d->dir = g_strdup(dir);
    if (opts)
        d->opts = *opts;
    else
        extapp_scan_options_init(&d->opts);
    d->progress = progress ? g_atomic_rc_box_acquire(progress) : NULL;

    GTask *task = g_task_new(NULL, cancellable, cb, user_data);
    g_task_set_source_tag(task, ea_scan_async);
    g_task_set_task_data(task, d, scan_data_free);
    g_task_run_in_thread(task, scan_thread);
    g_object_unref(task);
}

GListStore *ea_scan_finish(GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer(G_TASK(res), error);
}

/* ---- list entries ------------------------------------------------------ */

static void list_thread(GTask *task, gpointer src, gpointer task_data,
                        GCancellable *cancellable)
{
    (void)src; (void)task_data; (void)cancellable;
    ExtAppEntryList *list = NULL;
    char *err = NULL;
    ExtAppStatus st = extapp_list_entries(ea_backend(), &list, &err);
    if (st != EXTAPP_OK) {
        g_task_return_error(task, ea_error_from_status(st, err));
        return;
    }
    GListStore *store = g_list_store_new(EA_TYPE_ENTRY);
    for (size_t i = 0; i < list->count; i++) {
        EaEntry *e = ea_entry_new(&list->items[i]);
        g_list_store_append(store, e);
        g_object_unref(e);
    }
    extapp_entry_list_free(list);
    g_task_return_pointer(task, store, g_object_unref);
}

void ea_list_entries_async(GCancellable *cancellable, GAsyncReadyCallback cb,
                           gpointer user_data)
{
    GTask *task = g_task_new(NULL, cancellable, cb, user_data);
    g_task_set_source_tag(task, ea_list_entries_async);
    g_task_run_in_thread(task, list_thread);
    g_object_unref(task);
}

GListStore *ea_list_entries_finish(GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer(G_TASK(res), error);
}

/* ---- icon search ------------------------------------------------------- */

typedef struct {
    char *query;
    guint max;
} SearchData;

static void search_data_free(gpointer p)
{
    SearchData *d = p;
    g_free(d->query);
    g_free(d);
}

static void search_thread(GTask *task, gpointer src, gpointer task_data,
                          GCancellable *cancellable)
{
    (void)src;
    SearchData *d = task_data;
    ExtAppIconResultList *list = NULL;
    char *err = NULL;
    ExtAppStatus st = extapp_icon_search(ea_backend(), d->query, d->max,
                                         cancel_cb, cancellable, &list, &err);
    if (st != EXTAPP_OK) {
        g_task_return_error(task, ea_error_from_status(st, err));
        return;
    }
    GListStore *store = g_list_store_new(EA_TYPE_ICON_RESULT);
    for (size_t i = 0; i < list->count; i++) {
        EaIconResult *r = ea_icon_result_new(&list->items[i]);
        g_list_store_append(store, r);
        g_object_unref(r);
    }
    extapp_icon_result_list_free(list);
    g_task_return_pointer(task, store, g_object_unref);
}

void ea_icon_search_async(const char *query, guint max_results,
                          GCancellable *cancellable, GAsyncReadyCallback cb,
                          gpointer user_data)
{
    SearchData *d = g_new0(SearchData, 1);
    d->query = g_strdup(query);
    d->max = max_results;
    GTask *task = g_task_new(NULL, cancellable, cb, user_data);
    g_task_set_source_tag(task, ea_icon_search_async);
    g_task_set_task_data(task, d, search_data_free);
    g_task_run_in_thread(task, search_thread);
    g_object_unref(task);
}

GListStore *ea_icon_search_finish(GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer(G_TASK(res), error);
}

/* ---- fetch ------------------------------------------------------------- */

typedef struct {
    char *url;
    gsize max;
} FetchData;

static void fetch_data_free(gpointer p)
{
    FetchData *d = p;
    g_free(d->url);
    g_free(d);
}

static void fetch_thread(GTask *task, gpointer src, gpointer task_data,
                         GCancellable *cancellable)
{
    (void)src;
    FetchData *d = task_data;
    unsigned char *data = NULL;
    size_t len = 0;
    char *err = NULL;
    ExtAppStatus st = extapp_http_fetch(ea_backend(), d->url, d->max,
                                        cancel_cb, cancellable, &data, &len,
                                        &err);
    if (st != EXTAPP_OK) {
        g_task_return_error(task, ea_error_from_status(st, err));
        return;
    }
    g_task_return_pointer(task, g_bytes_new_with_free_func(data, len, free, data),
                          (GDestroyNotify)g_bytes_unref);
}

void ea_fetch_async(const char *url, gsize max_bytes,
                    GCancellable *cancellable, GAsyncReadyCallback cb,
                    gpointer user_data)
{
    FetchData *d = g_new0(FetchData, 1);
    d->url = g_strdup(url);
    d->max = max_bytes;
    GTask *task = g_task_new(NULL, cancellable, cb, user_data);
    g_task_set_source_tag(task, ea_fetch_async);
    g_task_set_task_data(task, d, fetch_data_free);
    g_task_run_in_thread(task, fetch_thread);
    g_object_unref(task);
}

GBytes *ea_fetch_finish(GAsyncResult *res, GError **error)
{
    return g_task_propagate_pointer(G_TASK(res), error);
}
