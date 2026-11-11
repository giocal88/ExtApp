/* Small string / filesystem helpers.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "private.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- string buffer ----------------------------------------------------- */

static bool sb_reserve(EaStrBuf *sb, size_t extra)
{
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap)
        return true;
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < need)
        cap *= 2;
    char *p = realloc(sb->data, cap);
    if (!p)
        return false;
    sb->data = p;
    sb->cap = cap;
    return true;
}

bool ea_sb_append_len(EaStrBuf *sb, const char *s, size_t n)
{
    if (!sb_reserve(sb, n))
        return false;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return true;
}

bool ea_sb_append(EaStrBuf *sb, const char *s)
{
    return ea_sb_append_len(sb, s, strlen(s));
}

bool ea_sb_append_c(EaStrBuf *sb, char c)
{
    return ea_sb_append_len(sb, &c, 1);
}

bool ea_sb_printf(EaStrBuf *sb, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || !sb_reserve(sb, (size_t)n)) {
        va_end(ap2);
        return false;
    }
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
    return true;
}

char *ea_sb_steal(EaStrBuf *sb)
{
    char *r = sb->data;
    if (!r)
        r = strdup("");
    sb->data = NULL;
    sb->len = sb->cap = 0;
    return r;
}

void ea_sb_clear(EaStrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

/* ---- strings ----------------------------------------------------------- */

char *ea_strdup0(const char *s)
{
    return s ? strdup(s) : NULL;
}

char *ea_strdup_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *r = NULL;
    if (vasprintf(&r, fmt, ap) < 0)
        r = NULL;
    va_end(ap);
    return r;
}

void ea_set_err(char **err, const char *fmt, ...)
{
    if (!err)
        return;
    free(*err);
    va_list ap;
    va_start(ap, fmt);
    if (vasprintf(err, fmt, ap) < 0)
        *err = NULL;
    va_end(ap);
}

const char *extapp_status_str(ExtAppStatus status)
{
    switch (status) {
    case EXTAPP_OK:              return "OK";
    case EXTAPP_ERR_INVALID_ARG: return "Invalid argument";
    case EXTAPP_ERR_NOMEM:       return "Out of memory";
    case EXTAPP_ERR_IO:          return "I/O error";
    case EXTAPP_ERR_NOT_FOUND:   return "Not found";
    case EXTAPP_ERR_EXISTS:      return "Already exists";
    case EXTAPP_ERR_NOT_MANAGED: return "Not managed by ExtApp";
    case EXTAPP_ERR_PARSE:       return "Parse error";
    case EXTAPP_ERR_NETWORK:     return "Network error";
    case EXTAPP_ERR_CANCELLED:   return "Cancelled";
    case EXTAPP_ERR_UNSUPPORTED: return "Unsupported";
    }
    return "Unknown error";
}

/* ---- paths ------------------------------------------------------------- */

char *ea_path_join(const char *a, const char *b)
{
    size_t la = strlen(a);
    if (la > 0 && a[la - 1] == '/')
        return ea_strdup_printf("%s%s", a, b);
    return ea_strdup_printf("%s/%s", a, b);
}

const char *ea_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

char *ea_dirname(const char *path)
{
    const char *s = strrchr(path, '/');
    if (!s)
        return strdup(".");
    if (s == path)
        return strdup("/");
    return strndup(path, (size_t)(s - path));
}

bool ea_has_suffix_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcasecmp(s + ls - lx, suffix) == 0;
}

bool ea_path_is_inside(const char *path, const char *dir)
{
    size_t ld = strlen(dir);
    while (ld > 1 && dir[ld - 1] == '/')
        ld--;
    return strncmp(path, dir, ld) == 0 && path[ld] == '/';
}

char *ea_slugify(const char *s)
{
    EaStrBuf sb = {0};
    bool dash = false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p)) {
            if (dash && sb.len > 0)
                ea_sb_append_c(&sb, '-');
            dash = false;
            ea_sb_append_c(&sb, (char)tolower(*p));
        } else if (*p >= 0x80) {
            /* keep UTF-8 bytes as-is: valid in file names */
            if (dash && sb.len > 0)
                ea_sb_append_c(&sb, '-');
            dash = false;
            ea_sb_append_c(&sb, (char)*p);
        } else {
            dash = true;
        }
    }
    if (sb.len == 0)
        ea_sb_append(&sb, "app");
    return ea_sb_steal(&sb);
}

ExtAppStatus ea_mkdir_p(const char *path, char **err)
{
    char *tmp = strdup(path);
    if (!tmp)
        return EXTAPP_ERR_NOMEM;
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            ea_set_err(err, "Cannot create %s: %s", tmp, strerror(errno));
            free(tmp);
            return EXTAPP_ERR_IO;
        }
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        ea_set_err(err, "Cannot create %s: %s", tmp, strerror(errno));
        free(tmp);
        return EXTAPP_ERR_IO;
    }
    free(tmp);
    return EXTAPP_OK;
}

ExtAppStatus ea_write_file_atomic(const char *path, const void *data,
                                  size_t len, unsigned mode, char **err)
{
    char *dir = ea_dirname(path);
    char *tmpl = dir ? ea_strdup_printf("%s/.%s.XXXXXX", dir, ea_basename(path))
                     : NULL;
    free(dir);
    if (!tmpl)
        return EXTAPP_ERR_NOMEM;

    int fd = mkstemp(tmpl);
    if (fd < 0) {
        ea_set_err(err, "Cannot create temporary file for %s: %s", path,
                   strerror(errno));
        free(tmpl);
        return EXTAPP_ERR_IO;
    }
    const char *p = data;
    size_t left = len;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            ea_set_err(err, "Cannot write %s: %s", path, strerror(errno));
            close(fd);
            unlink(tmpl);
            free(tmpl);
            return EXTAPP_ERR_IO;
        }
        p += w;
        left -= (size_t)w;
    }
    if (fchmod(fd, (mode_t)mode) != 0 || fsync(fd) != 0 || close(fd) != 0) {
        ea_set_err(err, "Cannot finalize %s: %s", path, strerror(errno));
        unlink(tmpl);
        free(tmpl);
        return EXTAPP_ERR_IO;
    }
    if (rename(tmpl, path) != 0) {
        ea_set_err(err, "Cannot rename into %s: %s", path, strerror(errno));
        unlink(tmpl);
        free(tmpl);
        return EXTAPP_ERR_IO;
    }
    free(tmpl);
    return EXTAPP_OK;
}

ExtAppStatus ea_read_file(const char *path, size_t max_bytes, char **data,
                          size_t *len, char **err)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ea_set_err(err, "Cannot open %s: %s", path, strerror(errno));
        return errno == ENOENT ? EXTAPP_ERR_NOT_FOUND : EXTAPP_ERR_IO;
    }
    EaStrBuf sb = {0};
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (sb.len + n > max_bytes) {
            ea_set_err(err, "%s is too large", path);
            fclose(f);
            ea_sb_clear(&sb);
            return EXTAPP_ERR_IO;
        }
        if (!ea_sb_append_len(&sb, buf, n)) {
            fclose(f);
            ea_sb_clear(&sb);
            return EXTAPP_ERR_NOMEM;
        }
    }
    bool failed = ferror(f);
    fclose(f);
    if (failed) {
        ea_set_err(err, "Cannot read %s", path);
        ea_sb_clear(&sb);
        return EXTAPP_ERR_IO;
    }
    if (len)
        *len = sb.len;
    *data = ea_sb_steal(&sb);
    return *data ? EXTAPP_OK : EXTAPP_ERR_NOMEM;
}

char *ea_find_in_path(const char *name)
{
    if (!name || !*name || strchr(name, '/'))
        return NULL;
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";
    char *copy = strdup(path);
    if (!copy)
        return NULL;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir;
         dir = strtok_r(NULL, ":", &save)) {
        char *full = ea_path_join(*dir ? dir : ".", name);
        if (full && access(full, X_OK) == 0) {
            struct stat st;
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
                free(copy);
                return full;
            }
        }
        free(full);
    }
    free(copy);
    return NULL;
}
