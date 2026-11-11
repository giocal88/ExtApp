/* Internal declarations shared by the backend sources. Not installed.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EXTAPP_PRIVATE_H
#define EXTAPP_PRIVATE_H

#include "extapp/extapp.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

struct ExtAppContext {
    char *data_home;        /* ~/.local/share                 */
    char *applications_dir; /* ~/.local/share/applications    */
    char *icons_dir;        /* ~/.local/share/extapp/icons    */
    char *cache_dir;        /* ~/.cache/extapp                */
    char *user_agent;
};

/* ---- util.c ------------------------------------------------------------ */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} EaStrBuf;

bool  ea_sb_append_len(EaStrBuf *sb, const char *s, size_t n);
bool  ea_sb_append(EaStrBuf *sb, const char *s);
bool  ea_sb_append_c(EaStrBuf *sb, char c);
bool  ea_sb_printf(EaStrBuf *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
char *ea_sb_steal(EaStrBuf *sb); /* returns "" if empty, never NULL unless OOM */
void  ea_sb_clear(EaStrBuf *sb);

char *ea_strdup0(const char *s); /* NULL-safe strdup */
char *ea_strdup_printf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void  ea_set_err(char **err, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

char *ea_path_join(const char *a, const char *b);
const char *ea_basename(const char *path);    /* pointer into path */
char *ea_dirname(const char *path);           /* allocated */
bool  ea_has_suffix_ci(const char *s, const char *suffix);
bool  ea_path_is_inside(const char *path, const char *dir); /* both canonical */
char *ea_slugify(const char *s);              /* "Foo Bar!" -> "foo-bar" */

/* mkdir -p with mode 0755 */
ExtAppStatus ea_mkdir_p(const char *path, char **err);
/* Write atomically (tmp file + rename) with the given permissions. */
ExtAppStatus ea_write_file_atomic(const char *path, const void *data,
                                  size_t len, unsigned mode, char **err);
ExtAppStatus ea_read_file(const char *path, size_t max_bytes, char **data,
                          size_t *len, char **err);

/* Find `name` in $PATH (only if it has no '/'). Returns NULL if not found. */
char *ea_find_in_path(const char *name);

/* ---- desktop_entry.c --------------------------------------------------- */

typedef struct {
    char *key;   /* includes locale suffix, e.g. "Name[it]" */
    char *value; /* already unescaped (\s \n \t \r \\)      */
} EaKeyValue;

typedef struct {
    EaKeyValue *kv;
    size_t      count;
    size_t      cap;
} EaDesktopEntry;

/* Parses only the [Desktop Entry] group. */
ExtAppStatus ea_de_parse_string(const char *text, EaDesktopEntry *out);
ExtAppStatus ea_de_parse_file(const char *path, EaDesktopEntry *out,
                              char **err);
void         ea_de_clear(EaDesktopEntry *de);
const char  *ea_de_get(const EaDesktopEntry *de, const char *key);
bool         ea_de_get_bool(const EaDesktopEntry *de, const char *key);

char *ea_de_escape_value(const char *s);   /* for writing a value  */
char *ea_de_unescape_value(const char *s); /* for reading a value  */

/* Quote a single argument for the Exec key (before value escaping). */
char *ea_exec_quote_arg(const char *arg);
/* Split an (already unescaped) Exec value into argv. Field codes such as %f
 * are dropped, %% becomes %. Returns NULL-terminated array; free with
 * ea_strv_free. Returns NULL on syntax error. */
char **ea_exec_split(const char *exec, size_t *argc_out);
void   ea_strv_free(char **v);

/* Resolve the program a desktop entry launches to a canonical path, or NULL.
 * Uses X-ExtApp-Target, TryExec, then first Exec token (skipping "env"). */
char *ea_de_resolve_target(const EaDesktopEntry *de);

/* ---- http.c ------------------------------------------------------------ */

typedef struct {
    const char *const *headers;  /* NULL-terminated "Name: value" list or NULL */
    const char *post_body;       /* non-NULL: send a POST with this body       */
    bool        https_only;
    size_t      max_bytes;
    long        timeout_sec;
} EaHttpOptions;

ExtAppStatus ea_http_get(ExtAppContext *ctx, const char *url,
                         const EaHttpOptions *opts, ExtAppCancelFn cancel,
                         void *user_data, char **body, size_t *len,
                         long *http_code, char **err);
char *ea_url_encode(const char *s);

/* ---- icon_search.c --------------------------------------------------- */

/* An icon source. `search` appends to `out` (which starts empty) and may be
 * called concurrently with other providers; `cancel` may be polled from
 * several threads at once. */
typedef struct {
    const char *name;
    ExtAppStatus (*search)(ExtAppContext *ctx, const char *query,
                           size_t max_results, ExtAppCancelFn cancel,
                           void *user_data, ExtAppIconResultList *out,
                           char **err);
} EaIconProvider;

extern const EaIconProvider ea_provider_appimagehub;
extern const EaIconProvider ea_provider_flathub;
extern const EaIconProvider ea_provider_commons;

/* Lower-cased search words with noise words ("icon", "logo", "app", ...)
 * removed. NULL-terminated; may be empty. Free with ea_strv_free. */
char **ea_icon_query_terms(const char *query);
/* Join terms with spaces (empty string if none). */
char  *ea_icon_terms_join(char **terms);
/* How well an app name matches the terms: 0 = no match, higher = better. */
int    ea_icon_name_score(const char *name, char **terms);
/* Append a result (strings are copied; NULLs allowed except image/thumb
 * both NULL, which is rejected). */
bool   ea_icon_list_push(ExtAppIconResultList *l, const char *provider,
                         const char *image_url, const char *thumbnail_url,
                         const char *title, const char *source, int width,
                         int height, bool scalable);
/* extapp_icon_search with an explicit provider list (used by tests). */
ExtAppStatus ea_icon_search_with(const EaIconProvider *const *providers,
                                 size_t n_providers, ExtAppContext *ctx,
                                 const char *query, size_t max_results,
                                 ExtAppCancelFn cancel, void *user_data,
                                 ExtAppIconResultList **out, char **err);
void   ea_icon_list_clear(ExtAppIconResultList *l);

/* Response parsers, exposed for unit tests. */
ExtAppStatus ea_appimagehub_parse(const char *feed_json, char **terms,
                                  size_t max_results,
                                  ExtAppIconResultList *out, char **err);
ExtAppStatus ea_flathub_parse(const char *json, char **terms,
                              size_t max_results, ExtAppIconResultList *out,
                              char **err);
ExtAppStatus ea_commons_parse(const char *json, char **terms,
                              size_t max_results, ExtAppIconResultList *out,
                              char **err);

/* ---- icons.c ----------------------------------------------------------- */

/* Returns file extension ("png", "svg", ...) or NULL when unsupported. */
const char *ea_sniff_image(const unsigned char *data, size_t len);

/* ---- entries.c --------------------------------------------------------- */

/* Internal: list entries (used by scan to link candidates). */
ExtAppStatus ea_list_entries(ExtAppContext *ctx, ExtAppEntryList *out,
                             char **err);
void ea_entry_clear(ExtAppEntry *e);

#endif /* EXTAPP_PRIVATE_H */
