/* Folder scanner: finds AppImages, ELF executables and scripts.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "private.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define SCAN_MAX_RESULTS 10000

const char *extapp_kind_str(ExtAppKind kind)
{
    switch (kind) {
    case EXTAPP_KIND_APPIMAGE: return "AppImage";
    case EXTAPP_KIND_ELF:      return "Executable";
    case EXTAPP_KIND_SCRIPT:   return "Script";
    }
    return "Unknown";
}

void extapp_scan_options_init(ExtAppScanOptions *opts)
{
    if (!opts)
        return;
    opts->max_depth = 3;
    opts->include_scripts = false;
    opts->include_hidden = false;
    opts->follow_symlinks = false;
}

/* ---- name suggestion --------------------------------------------------- */

static bool is_arch_token(const char *t)
{
    static const char *const arch[] = {
        "x86", "64", "x86_64", "amd64", "x64", "i386", "i686", "aarch64",
        "arm64", "armhf", "armv7l", "linux", "linux64", "appimage", "bin",
        "gnu", "glibc", "portable", "setup", "installer", NULL};
    for (const char *const *a = arch; *a; a++)
        if (strcasecmp(t, *a) == 0)
            return true;
    return false;
}

static bool is_version_token(const char *t)
{
    if (isdigit((unsigned char)t[0]))
        return true;
    if ((t[0] == 'v' || t[0] == 'V') && isdigit((unsigned char)t[1]))
        return true;
    return false;
}

char *extapp_suggest_name(const char *path)
{
    if (!path)
        return NULL;
    char *base = strdup(ea_basename(path));
    if (!base)
        return NULL;

    static const char *const exts[] = {".AppImage", ".sh", ".py", ".run",
                                       ".bin", ".x86_64", ".x86", ".elf",
                                       NULL};
    for (const char *const *e = exts; *e; e++) {
        if (ea_has_suffix_ci(base, *e) && strlen(base) > strlen(*e)) {
            base[strlen(base) - strlen(*e)] = '\0';
            break;
        }
    }

    EaStrBuf out = {0};
    char *save = NULL;
    char *work = strdup(base);
    if (!work) {
        free(base);
        return NULL;
    }
    for (char *tok = strtok_r(work, "-_. ", &save); tok;
         tok = strtok_r(NULL, "-_. ", &save)) {
        if (is_version_token(tok)) {
            if (out.len > 0)
                break; /* everything after the version is noise */
            continue;
        }
        if (is_arch_token(tok))
            continue;
        if (out.len > 0)
            ea_sb_append_c(&out, ' ');
        size_t start = out.len;
        ea_sb_append(&out, tok);
        out.data[start] = (char)toupper((unsigned char)out.data[start]);
    }
    free(work);

    if (out.len == 0) {
        ea_sb_clear(&out);
        return base;
    }
    free(base);
    return ea_sb_steal(&out);
}

/* ---- detection --------------------------------------------------------- */

static bool looks_like_library(const char *name)
{
    if (ea_has_suffix_ci(name, ".a") || ea_has_suffix_ci(name, ".o") ||
        ea_has_suffix_ci(name, ".ko"))
        return true;
    const char *so = strstr(name, ".so");
    return so && (so[3] == '\0' || so[3] == '.');
}

static uint16_t rd16(const unsigned char *p, bool le)
{
    return le ? (uint16_t)(p[0] | p[1] << 8) : (uint16_t)(p[0] << 8 | p[1]);
}

/* Returns true and sets *kind when the file is a candidate. */
static bool classify(int dirfd, const char *name, const struct stat *st,
                     const ExtAppScanOptions *opts, ExtAppKind *kind)
{
    if (!S_ISREG(st->st_mode) || st->st_size < 4)
        return false;
    if (looks_like_library(name))
        return false;

    bool exec_bit = (st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
    bool appimage_ext = ea_has_suffix_ci(name, ".AppImage");
    if (!exec_bit && !appimage_ext)
        return false;

    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0)
        return false;
    unsigned char h[20] = {0};
    ssize_t n = read(fd, h, sizeof h);
    close(fd);
    if (n < 4)
        return false;

    bool elf = memcmp(h, "\x7f" "ELF", 4) == 0;
    if (elf && n >= 11 && h[8] == 'A' && h[9] == 'I' && (h[10] == 1 || h[10] == 2)) {
        *kind = EXTAPP_KIND_APPIMAGE;
        return true;
    }
    if (elf && appimage_ext) {
        *kind = EXTAPP_KIND_APPIMAGE;
        return true;
    }
    if (elf && exec_bit && n >= 18) {
        uint16_t type = rd16(h + 16, h[5] != 2 /* EI_DATA: 1=LE, 2=BE */);
        if (type == 2 /* ET_EXEC */ || type == 3 /* ET_DYN (PIE) */) {
            *kind = EXTAPP_KIND_ELF;
            return true;
        }
        return false;
    }
    if (exec_bit && opts->include_scripts && h[0] == '#' && h[1] == '!') {
        *kind = EXTAPP_KIND_SCRIPT;
        return true;
    }
    return false;
}

/* ---- walking ----------------------------------------------------------- */

typedef struct {
    dev_t dev;
    ino_t ino;
} DirId;

typedef struct {
    const ExtAppScanOptions *opts;
    ExtAppProgressFn progress;
    ExtAppCancelFn cancel;
    void *ud;
    ExtAppCandidateList *list;
    size_t cap;
    DirId *visited;
    size_t nvisited, capvisited;
    bool cancelled;
} ScanState;

static bool mark_visited(ScanState *s, dev_t dev, ino_t ino)
{
    for (size_t i = 0; i < s->nvisited; i++)
        if (s->visited[i].dev == dev && s->visited[i].ino == ino)
            return false;
    if (s->nvisited == s->capvisited) {
        size_t nc = s->capvisited ? s->capvisited * 2 : 32;
        DirId *v = realloc(s->visited, nc * sizeof *v);
        if (!v)
            return false;
        s->visited = v;
        s->capvisited = nc;
    }
    s->visited[s->nvisited++] = (DirId){dev, ino};
    return true;
}

static ExtAppStatus add_candidate(ScanState *s, const char *path,
                                  ExtAppKind kind, const struct stat *st)
{
    if (s->list->count >= SCAN_MAX_RESULTS)
        return EXTAPP_OK;
    if (s->list->count == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 32;
        ExtAppCandidate *items = realloc(s->list->items, nc * sizeof *items);
        if (!items)
            return EXTAPP_ERR_NOMEM;
        s->list->items = items;
        s->cap = nc;
    }
    ExtAppCandidate *c = &s->list->items[s->list->count];
    memset(c, 0, sizeof *c);
    c->path = strdup(path);
    c->real_path = realpath(path, NULL);
    if (!c->real_path)
        c->real_path = strdup(path);
    c->suggested_name = extapp_suggest_name(path);
    c->kind = kind;
    c->is_executable = (st->st_mode & S_IXUSR) != 0;
    if (!c->path || !c->real_path || !c->suggested_name) {
        free(c->path);
        free(c->real_path);
        free(c->suggested_name);
        return EXTAPP_ERR_NOMEM;
    }
    s->list->count++;
    return EXTAPP_OK;
}

static ExtAppStatus walk(ScanState *s, const char *dir, int depth)
{
    if (s->cancel && s->cancel(s->ud)) {
        s->cancelled = true;
        return EXTAPP_ERR_CANCELLED;
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0)
        return EXTAPP_OK; /* unreadable sub-directory: skip silently */
    DIR *d = fdopendir(dfd);
    if (!d) {
        close(dfd);
        return EXTAPP_OK;
    }

    ExtAppStatus st = EXTAPP_OK;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (name[0] == '.' && !s->opts->include_hidden)
            continue;
        if (s->cancel && s->cancel(s->ud)) {
            s->cancelled = true;
            st = EXTAPP_ERR_CANCELLED;
            break;
        }

        struct stat lst, fst;
        if (fstatat(dfd, name, &lst, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        bool is_link = S_ISLNK(lst.st_mode);
        if (is_link) {
            if (fstatat(dfd, name, &fst, 0) != 0)
                continue; /* dangling */
        } else {
            fst = lst;
        }

        char *full = ea_path_join(dir, name);
        if (!full) {
            st = EXTAPP_ERR_NOMEM;
            break;
        }
        if (s->progress)
            s->progress(full, s->ud);

        if (S_ISDIR(fst.st_mode)) {
            if (depth < s->opts->max_depth &&
                (!is_link || s->opts->follow_symlinks) &&
                mark_visited(s, fst.st_dev, fst.st_ino)) {
                st = walk(s, full, depth + 1);
            }
        } else {
            ExtAppKind kind;
            if (classify(dfd, name, &fst, s->opts, &kind))
                st = add_candidate(s, full, kind, &fst);
        }
        free(full);
        if (st != EXTAPP_OK)
            break;
    }
    closedir(d);
    return st;
}

static int cmp_candidates(const void *a, const void *b)
{
    const ExtAppCandidate *x = a, *y = b;
    int r = strcasecmp(x->suggested_name, y->suggested_name);
    return r ? r : strcmp(x->path, y->path);
}

void extapp_candidate_list_free(ExtAppCandidateList *list)
{
    if (!list)
        return;
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].path);
        free(list->items[i].real_path);
        free(list->items[i].suggested_name);
        free(list->items[i].desktop_file);
    }
    free(list->items);
    free(list);
}

ExtAppStatus extapp_scan(ExtAppContext *ctx, const char *dir,
                         const ExtAppScanOptions *opts,
                         ExtAppProgressFn progress, ExtAppCancelFn cancel,
                         void *user_data, ExtAppCandidateList **out,
                         char **err)
{
    if (!ctx || !dir || !out) {
        ea_set_err(err, "Invalid arguments");
        return EXTAPP_ERR_INVALID_ARG;
    }
    *out = NULL;

    ExtAppScanOptions defaults;
    extapp_scan_options_init(&defaults);
    if (!opts)
        opts = &defaults;

    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ea_set_err(err, "%s is not a readable folder", dir);
        return EXTAPP_ERR_NOT_FOUND;
    }

    ScanState s = {.opts = opts, .progress = progress, .cancel = cancel,
                   .ud = user_data};
    s.list = calloc(1, sizeof *s.list);
    if (!s.list)
        return EXTAPP_ERR_NOMEM;
    mark_visited(&s, st.st_dev, st.st_ino);

    ExtAppStatus rc = walk(&s, dir, 0);
    free(s.visited);
    if (rc != EXTAPP_OK) {
        ea_set_err(err, "%s", rc == EXTAPP_ERR_CANCELLED ? "Scan cancelled"
                                                         : "Out of memory");
        extapp_candidate_list_free(s.list);
        return rc;
    }

    /* link with existing shortcuts */
    ExtAppEntryList entries = {0};
    if (ea_list_entries(ctx, &entries, NULL) == EXTAPP_OK) {
        for (size_t i = 0; i < s.list->count; i++) {
            ExtAppCandidate *c = &s.list->items[i];
            for (size_t j = 0; j < entries.count; j++) {
                const ExtAppEntry *e = &entries.items[j];
                if (e->target_path && strcmp(e->target_path, c->real_path) == 0) {
                    /* prefer the shortcut ExtApp manages */
                    if (!c->desktop_file || e->managed) {
                        free(c->desktop_file);
                        c->desktop_file = strdup(e->file_path);
                    }
                    if (e->managed)
                        break;
                }
            }
        }
        for (size_t j = 0; j < entries.count; j++)
            ea_entry_clear(&entries.items[j]);
        free(entries.items);
    }

    qsort(s.list->items, s.list->count, sizeof *s.list->items, cmp_candidates);
    *out = s.list;
    return EXTAPP_OK;
}
