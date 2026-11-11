/*
 * libextapp — frontend-agnostic backend for ExtApp.
 *
 * This is the ONLY public header of the backend. It is plain C (C11), has no
 * dependency on GLib, Qt or libcurl types, and is safe to include from C++.
 *
 * Contract for frontends:
 *  - All functions are blocking. Run them on a worker thread (GTask,
 *    QtConcurrent, std::thread, ...) when called from a UI.
 *  - An ExtAppContext is immutable after creation and may be shared between
 *    threads; every call only touches its own arguments.
 *  - Every object returned through an out-parameter is owned by the caller and
 *    must be released with the matching *_free() function (or free() for plain
 *    char* / unsigned char* buffers).
 *  - Functions return an ExtAppStatus. When `err` is non-NULL and the call
 *    fails, *err receives a human readable message (free() it).
 *  - Long running calls accept an ExtAppCancelFn polled periodically; return
 *    true from it to abort with EXTAPP_ERR_CANCELLED.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef EXTAPP_EXTAPP_H
#define EXTAPP_EXTAPP_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define EXTAPP_API __attribute__((visibility("default")))
#else
#define EXTAPP_API
#endif

#define EXTAPP_VERSION_STRING "0.1.0"

/* ------------------------------------------------------------------------ */
/* Status / errors                                                          */
/* ------------------------------------------------------------------------ */

typedef enum {
    EXTAPP_OK = 0,
    EXTAPP_ERR_INVALID_ARG,
    EXTAPP_ERR_NOMEM,
    EXTAPP_ERR_IO,
    EXTAPP_ERR_NOT_FOUND,
    EXTAPP_ERR_EXISTS,       /* a shortcut for that target already exists   */
    EXTAPP_ERR_NOT_MANAGED,  /* refusing to touch a file ExtApp didn't make */
    EXTAPP_ERR_PARSE,
    EXTAPP_ERR_NETWORK,
    EXTAPP_ERR_CANCELLED,
    EXTAPP_ERR_UNSUPPORTED
} ExtAppStatus;

EXTAPP_API const char *extapp_status_str(ExtAppStatus status);

/* Return true to request cancellation. */
typedef bool (*ExtAppCancelFn)(void *user_data);
/* Called for every visited path during a scan (may be called often). */
typedef void (*ExtAppProgressFn)(const char *path, void *user_data);

/* ------------------------------------------------------------------------ */
/* Context                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct ExtAppContext ExtAppContext;

/* Uses $XDG_DATA_HOME (fallback: ~/.local/share). Returns NULL on failure. */
EXTAPP_API ExtAppContext *extapp_context_new(void);
/* Same, but with an explicit data home (useful for tests / portable setups). */
EXTAPP_API ExtAppContext *extapp_context_new_with_data_dir(const char *data_home);
EXTAPP_API void extapp_context_free(ExtAppContext *ctx);

/* e.g. ~/.local/share/applications */
EXTAPP_API const char *extapp_context_applications_dir(const ExtAppContext *ctx);
/* e.g. ~/.local/share/extapp/icons (where downloaded icons are stored) */
EXTAPP_API const char *extapp_context_icons_dir(const ExtAppContext *ctx);

/* ------------------------------------------------------------------------ */
/* Scanning a folder for launchable programs                                */
/* ------------------------------------------------------------------------ */

typedef enum {
    EXTAPP_KIND_APPIMAGE = 0,
    EXTAPP_KIND_ELF,
    EXTAPP_KIND_SCRIPT
} ExtAppKind;

EXTAPP_API const char *extapp_kind_str(ExtAppKind kind);

typedef struct {
    int  max_depth;        /* 0 = only the given folder; default 3          */
    bool include_scripts;  /* executable files starting with "#!"           */
    bool include_hidden;   /* descend into / report dot-files               */
    bool follow_symlinks;  /* descend into symlinked directories            */
} ExtAppScanOptions;

EXTAPP_API void extapp_scan_options_init(ExtAppScanOptions *opts);

typedef struct {
    char      *path;            /* path as found                             */
    char      *real_path;       /* canonical path (symlinks resolved)        */
    char      *suggested_name;  /* human friendly name, e.g. "Kdenlive"     */
    ExtAppKind kind;
    bool       is_executable;   /* false for e.g. freshly downloaded AppImages */
    char      *desktop_file;    /* existing shortcut pointing here, or NULL  */
} ExtAppCandidate;

typedef struct {
    ExtAppCandidate *items;
    size_t           count;
} ExtAppCandidateList;

/* Scan `dir` for executables/AppImages and link them with existing shortcuts.
 * `opts` may be NULL for defaults; `progress`/`cancel` may be NULL. */
EXTAPP_API ExtAppStatus extapp_scan(ExtAppContext *ctx, const char *dir,
                                    const ExtAppScanOptions *opts,
                                    ExtAppProgressFn progress,
                                    ExtAppCancelFn cancel, void *user_data,
                                    ExtAppCandidateList **out, char **err);
EXTAPP_API void extapp_candidate_list_free(ExtAppCandidateList *list);

/* Turn "Foo_Bar-1.2.3-x86_64.AppImage" into "Foo Bar". Caller frees. */
EXTAPP_API char *extapp_suggest_name(const char *path);

/* ------------------------------------------------------------------------ */
/* Desktop entries (shortcuts)                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
    char *file_path;    /* absolute path of the .desktop file               */
    char *id;           /* desktop file id, e.g. "extapp-foo.desktop"       */
    char *name;
    char *exec;
    char *args;         /* extra arguments (only known for managed entries) */
    char *icon;         /* icon name or absolute path (may be NULL)         */
    char *comment;
    char *categories;
    char *target_path;  /* resolved program the entry launches (may be NULL) */
    bool  terminal;
    bool  no_display;   /* NoDisplay=true or Hidden=true                    */
    bool  managed;      /* created by ExtApp (X-ExtApp-Managed=true)        */
    bool  target_exists;
} ExtAppEntry;

typedef struct {
    ExtAppEntry *items;
    size_t       count;
} ExtAppEntryList;

/* List every application .desktop file in the applications dir. */
EXTAPP_API ExtAppStatus extapp_list_entries(ExtAppContext *ctx,
                                            ExtAppEntryList **out, char **err);
EXTAPP_API void extapp_entry_list_free(ExtAppEntryList *list);

typedef struct {
    const char *name;         /* required                                   */
    const char *target_path;  /* required: program to launch                */
    const char *args;         /* optional, appended to Exec verbatim        */
    const char *icon;         /* optional: icon name or absolute path       */
    const char *comment;      /* optional                                   */
    const char *categories;   /* optional, e.g. "Utility;Development;"      */
    bool        terminal;
} ExtAppEntrySpec;

enum {
    EXTAPP_CREATE_MAKE_EXECUTABLE = 1u << 0, /* chmod +x the target          */
    EXTAPP_CREATE_OVERWRITE       = 1u << 1  /* replace an existing managed
                                                shortcut for the same target */
};

/* Create a shortcut. On EXTAPP_ERR_EXISTS, *out_path (if non-NULL) receives
 * the path of the managed shortcut that already points at the target. */
EXTAPP_API ExtAppStatus extapp_entry_create(ExtAppContext *ctx,
                                            const ExtAppEntrySpec *spec,
                                            unsigned flags, char **out_path,
                                            char **err);

enum {
    EXTAPP_REMOVE_ALLOW_UNMANAGED = 1u << 0, /* allow deleting foreign files */
    EXTAPP_REMOVE_DELETE_ICON     = 1u << 1  /* also delete our stored icon  */
};

EXTAPP_API ExtAppStatus extapp_entry_remove(ExtAppContext *ctx,
                                            const char *desktop_path,
                                            unsigned flags, char **err);

/* ------------------------------------------------------------------------ */
/* Icons                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
    char *provider;    /* "AppImageHub", "Flathub", "Wikimedia Commons"   */
    char *image_url;   /* full-size image to store as the icon             */
    char *thumbnail_url; /* small preview (may equal image_url)            */
    char *title;
    char *source;      /* web page describing the image / app             */
    int   width;       /* 0 when unknown                                   */
    int   height;
    bool  scalable;    /* vector image (SVG): any size                     */
} ExtAppIconResult;

typedef struct {
    ExtAppIconResult *items;
    size_t            count;
} ExtAppIconResultList;

/* Search icon candidates for an application name. Queries AppImageHub,
 * Flathub (real application icons) and Wikimedia Commons (logos) in
 * parallel and merges the results, best matches first. Words like "icon"
 * or "logo" in the query are ignored; a query made only of such words fails
 * with EXTAPP_ERR_INVALID_ARG. `max_results` 0 means 40.
 * Returns EXTAPP_OK (possibly with an empty list) when no source failed or
 * when there are results; EXTAPP_ERR_NETWORK when there are no results and a
 * source failed; EXTAPP_ERR_CANCELLED when cancelled.
 * `cancel` may be polled from several threads at once. */
EXTAPP_API ExtAppStatus extapp_icon_search(ExtAppContext *ctx,
                                           const char *query,
                                           size_t max_results,
                                           ExtAppCancelFn cancel,
                                           void *user_data,
                                           ExtAppIconResultList **out,
                                           char **err);
EXTAPP_API void extapp_icon_result_list_free(ExtAppIconResultList *list);

/* Download `url` (http/https) into memory, at most `max_bytes` (0 = 5 MiB). */
EXTAPP_API ExtAppStatus extapp_http_fetch(ExtAppContext *ctx, const char *url,
                                          size_t max_bytes,
                                          ExtAppCancelFn cancel,
                                          void *user_data,
                                          unsigned char **data, size_t *len,
                                          char **err);

/* Store image bytes (PNG, SVG, JPEG, WebP or ICO — detected from content) in
 * the icons dir as "<slug of name_hint>.<ext>". Frontends should preferably
 * convert to PNG first. *out_path receives the absolute path. */
EXTAPP_API ExtAppStatus extapp_icon_save(ExtAppContext *ctx,
                                         const char *name_hint,
                                         const unsigned char *data, size_t len,
                                         char **out_path, char **err);

#ifdef __cplusplus
}
#endif

#endif /* EXTAPP_EXTAPP_H */
