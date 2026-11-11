/* Async (GTask) wrappers around the blocking backend calls.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <gio/gio.h>
#include <extapp/extapp.h>

G_BEGIN_DECLS

#define EA_ERROR (ea_error_quark())
GQuark ea_error_quark(void);

/* Process wide backend context (immutable, safe to share between threads). */
void           ea_backend_init(void);
ExtAppContext *ea_backend(void);
void           ea_backend_shutdown(void);

/* Converts a backend failure into a GError. Cancellation becomes
 * G_IO_ERROR_CANCELLED so callers can match it uniformly. Takes `msg`. */
GError *ea_error_from_status(ExtAppStatus st, char *msg);

/* Scan: result is a GListStore of EaCandidate. `progress` (nullable) must be
 * a g_atomic_rc_box of gint; it is incremented for every visited path. */
void        ea_scan_async(const char *dir, const ExtAppScanOptions *opts,
                          gint *progress, GCancellable *cancellable,
                          GAsyncReadyCallback cb, gpointer user_data);
GListStore *ea_scan_finish(GAsyncResult *res, GError **error);

/* Shortcuts: result is a GListStore of EaEntry. */
void        ea_list_entries_async(GCancellable *cancellable,
                                  GAsyncReadyCallback cb, gpointer user_data);
GListStore *ea_list_entries_finish(GAsyncResult *res, GError **error);

/* Icon search: result is a GListStore of EaIconResult. */
void        ea_icon_search_async(const char *query, guint max_results,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback cb, gpointer user_data);
GListStore *ea_icon_search_finish(GAsyncResult *res, GError **error);

/* Download a URL into memory. */
void    ea_fetch_async(const char *url, gsize max_bytes,
                       GCancellable *cancellable, GAsyncReadyCallback cb,
                       gpointer user_data);
GBytes *ea_fetch_finish(GAsyncResult *res, GError **error);

G_END_DECLS
