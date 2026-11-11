/* Minimal reader/writer helpers for freedesktop .desktop files.
 * Spec: https://specifications.freedesktop.org/desktop-entry-spec/latest/
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "private.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DESKTOP_MAX_SIZE (1024 * 1024)

/* ---- value escaping ---------------------------------------------------- */

char *ea_de_unescape_value(const char *s)
{
    EaStrBuf sb = {0};
    ea_sb_append(&sb, "");
    for (const char *p = s; *p; p++) {
        if (*p != '\\' || p[1] == '\0') {
            ea_sb_append_c(&sb, *p);
            continue;
        }
        p++;
        switch (*p) {
        case 's':  ea_sb_append_c(&sb, ' ');  break;
        case 'n':  ea_sb_append_c(&sb, '\n'); break;
        case 't':  ea_sb_append_c(&sb, '\t'); break;
        case 'r':  ea_sb_append_c(&sb, '\r'); break;
        case '\\': ea_sb_append_c(&sb, '\\'); break;
        default: /* keep unknown escapes (e.g. "\;" in lists) untouched */
            ea_sb_append_c(&sb, '\\');
            ea_sb_append_c(&sb, *p);
            break;
        }
    }
    return ea_sb_steal(&sb);
}

char *ea_de_escape_value(const char *s)
{
    EaStrBuf sb = {0};
    ea_sb_append(&sb, "");
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '\\': ea_sb_append(&sb, "\\\\"); break;
        case '\n': ea_sb_append(&sb, "\\n");  break;
        case '\t': ea_sb_append(&sb, "\\t");  break;
        case '\r': ea_sb_append(&sb, "\\r");  break;
        case ' ':
            ea_sb_append(&sb, p == s ? "\\s" : " ");
            break;
        default:   ea_sb_append_c(&sb, *p);   break;
        }
    }
    return ea_sb_steal(&sb);
}

/* ---- parsing ----------------------------------------------------------- */

static bool de_add(EaDesktopEntry *de, char *key, char *value)
{
    /* first occurrence wins, duplicates are ignored */
    for (size_t i = 0; i < de->count; i++) {
        if (strcmp(de->kv[i].key, key) == 0) {
            free(key);
            free(value);
            return true;
        }
    }
    if (de->count == de->cap) {
        size_t cap = de->cap ? de->cap * 2 : 16;
        EaKeyValue *kv = realloc(de->kv, cap * sizeof *kv);
        if (!kv) {
            free(key);
            free(value);
            return false;
        }
        de->kv = kv;
        de->cap = cap;
    }
    de->kv[de->count].key = key;
    de->kv[de->count].value = value;
    de->count++;
    return true;
}

ExtAppStatus ea_de_parse_string(const char *text, EaDesktopEntry *out)
{
    memset(out, 0, sizeof *out);
    bool in_group = false, seen_group = false;
    const char *line = text;

    /* skip UTF-8 BOM */
    if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
        (unsigned char)line[2] == 0xBF)
        line += 3;

    while (*line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        const char *next = end ? end + 1 : line + len;

        /* trim */
        while (len > 0 && (line[len - 1] == '\r' || isspace((unsigned char)line[len - 1])))
            len--;
        while (len > 0 && isspace((unsigned char)*line)) {
            line++;
            len--;
        }

        if (len == 0 || line[0] == '#') {
            /* blank or comment */
        } else if (line[0] == '[') {
            in_group = !seen_group && len == strlen("[Desktop Entry]") &&
                       strncmp(line, "[Desktop Entry]", len) == 0;
            if (in_group)
                seen_group = true;
        } else if (in_group) {
            const char *eq = memchr(line, '=', len);
            if (eq) {
                size_t klen = (size_t)(eq - line);
                while (klen > 0 && isspace((unsigned char)line[klen - 1]))
                    klen--;
                const char *v = eq + 1;
                size_t vlen = len - (size_t)(v - line);
                while (vlen > 0 && isspace((unsigned char)*v)) {
                    v++;
                    vlen--;
                }
                if (klen > 0) {
                    char *key = strndup(line, klen);
                    char *raw = strndup(v, vlen);
                    char *val = raw ? ea_de_unescape_value(raw) : NULL;
                    free(raw);
                    if (!key || !val || !de_add(out, key, val)) {
                        if (!key || !val) {
                            free(key);
                            free(val);
                        }
                        ea_de_clear(out);
                        return EXTAPP_ERR_NOMEM;
                    }
                }
            }
        }
        line = next;
    }
    return seen_group ? EXTAPP_OK : EXTAPP_ERR_PARSE;
}

ExtAppStatus ea_de_parse_file(const char *path, EaDesktopEntry *out, char **err)
{
    char *text = NULL;
    ExtAppStatus st = ea_read_file(path, DESKTOP_MAX_SIZE, &text, NULL, err);
    if (st != EXTAPP_OK)
        return st;
    st = ea_de_parse_string(text, out);
    free(text);
    if (st == EXTAPP_ERR_PARSE)
        ea_set_err(err, "%s has no [Desktop Entry] group", path);
    return st;
}

void ea_de_clear(EaDesktopEntry *de)
{
    for (size_t i = 0; i < de->count; i++) {
        free(de->kv[i].key);
        free(de->kv[i].value);
    }
    free(de->kv);
    memset(de, 0, sizeof *de);
}

const char *ea_de_get(const EaDesktopEntry *de, const char *key)
{
    for (size_t i = 0; i < de->count; i++)
        if (strcmp(de->kv[i].key, key) == 0)
            return de->kv[i].value;
    return NULL;
}

bool ea_de_get_bool(const EaDesktopEntry *de, const char *key)
{
    const char *v = ea_de_get(de, key);
    return v && (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
}

/* ---- Exec quoting ------------------------------------------------------ */

static bool exec_reserved(char c)
{
    return strchr(" \t\n\"'\\><~|&;$*?#()`", c) != NULL;
}

char *ea_exec_quote_arg(const char *arg)
{
    bool quote = *arg == '\0';
    for (const char *p = arg; *p && !quote; p++)
        quote = exec_reserved(*p);

    EaStrBuf sb = {0};
    ea_sb_append(&sb, "");
    if (quote)
        ea_sb_append_c(&sb, '"');
    for (const char *p = arg; *p; p++) {
        if (*p == '%') {
            ea_sb_append(&sb, "%%");
        } else if (quote && strchr("\"`$\\", *p)) {
            ea_sb_append_c(&sb, '\\');
            ea_sb_append_c(&sb, *p);
        } else {
            ea_sb_append_c(&sb, *p);
        }
    }
    if (quote)
        ea_sb_append_c(&sb, '"');
    return ea_sb_steal(&sb);
}

void ea_strv_free(char **v)
{
    if (!v)
        return;
    for (char **p = v; *p; p++)
        free(*p);
    free(v);
}

static bool strv_push(char ***v, size_t *n, size_t *cap, char *s)
{
    if (*n + 1 >= *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        char **nv = realloc(*v, nc * sizeof **v);
        if (!nv)
            return false;
        *v = nv;
        *cap = nc;
    }
    (*v)[(*n)++] = s;
    (*v)[*n] = NULL;
    return true;
}

char **ea_exec_split(const char *exec, size_t *argc_out)
{
    char **argv = NULL;
    size_t argc = 0, cap = 0;
    EaStrBuf tok = {0};
    bool in_token = false, had_field = false;
    const char *p = exec;

    if (!strv_push(&argv, &argc, &cap, NULL))
        return NULL;
    argc = 0; /* keep the array NULL-terminated */

    for (;;) {
        char c = *p;
        if (c == '\0' || c == ' ' || c == '\t' || c == '\n') {
            if (in_token) {
                if (tok.len > 0 || !had_field) {
                    char *s = ea_sb_steal(&tok);
                    if (!s || !strv_push(&argv, &argc, &cap, s)) {
                        free(s);
                        goto fail;
                    }
                } else {
                    ea_sb_clear(&tok);
                }
            }
            in_token = had_field = false;
            if (c == '\0')
                break;
            p++;
            continue;
        }
        in_token = true;
        if (c == '"') {
            p++;
            ea_sb_append(&tok, ""); /* "" is an empty argument */
            while (*p && *p != '"') {
                if (*p == '\\' && p[1] && strchr("\"`$\\", p[1])) {
                    ea_sb_append_c(&tok, p[1]);
                    p += 2;
                } else if (*p == '%' && p[1] == '%') {
                    ea_sb_append_c(&tok, '%');
                    p += 2;
                } else {
                    ea_sb_append_c(&tok, *p++);
                }
            }
            if (*p != '"')
                goto fail; /* unterminated quote */
            p++;
        } else if (c == '\'') {
            /* not in the spec, but common in the wild: shell-like literal */
            p++;
            ea_sb_append(&tok, "");
            while (*p && *p != '\'')
                ea_sb_append_c(&tok, *p++);
            if (*p != '\'')
                goto fail;
            p++;
        } else if (c == '%' && p[1]) {
            if (p[1] == '%')
                ea_sb_append_c(&tok, '%');
            else
                had_field = true; /* drop field codes like %f %U %i */
            p += 2;
        } else if (c == '\\' && p[1]) {
            ea_sb_append_c(&tok, p[1]);
            p += 2;
        } else {
            ea_sb_append_c(&tok, c);
            p++;
        }
    }
    ea_sb_clear(&tok);
    if (argc_out)
        *argc_out = argc;
    return argv;

fail:
    ea_sb_clear(&tok);
    ea_strv_free(argv);
    return NULL;
}

/* ---- target resolution ------------------------------------------------- */

static char *resolve_program(const char *prog)
{
    if (!prog || !*prog)
        return NULL;
    char *abs = NULL;
    if (prog[0] == '/')
        abs = strdup(prog);
    else if (prog[0] == '~' && prog[1] == '/' && getenv("HOME"))
        abs = ea_path_join(getenv("HOME"), prog + 2);
    else
        abs = ea_find_in_path(prog);
    if (!abs)
        return NULL;
    char *real = realpath(abs, NULL);
    if (real) {
        free(abs);
        return real;
    }
    return abs; /* keep the dangling path so callers can show it */
}

char *ea_de_resolve_target(const EaDesktopEntry *de)
{
    const char *t = ea_de_get(de, "X-ExtApp-Target");
    if (t && *t)
        return resolve_program(t);

    const char *exec = ea_de_get(de, "Exec");
    if (exec) {
        size_t argc = 0;
        char **argv = ea_exec_split(exec, &argc);
        if (argv) {
            size_t i = 0;
            if (argc > 0 && strcmp(ea_basename(argv[0]), "env") == 0) {
                i = 1;
                while (i < argc && (argv[i][0] == '-' || strchr(argv[i], '=')))
                    i++;
            }
            char *r = i < argc ? resolve_program(argv[i]) : NULL;
            ea_strv_free(argv);
            if (r)
                return r;
        }
    }

    const char *tryexec = ea_de_get(de, "TryExec");
    if (tryexec && *tryexec)
        return resolve_program(tryexec);
    return NULL;
}
