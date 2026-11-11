/* Tiny test harness. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EXTAPP_TESTUTIL_H
#define EXTAPP_TESTUTIL_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int t_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            t_failures++;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_STR(a, b)                                                      \
    do {                                                                     \
        const char *_a = (a), *_b = (b);                                     \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                             \
            fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n", __FILE__,\
                    __LINE__, _b ? _b : "(null)", _a ? _a : "(null)");       \
            t_failures++;                                                    \
        }                                                                    \
    } while (0)

#define TEST_DONE()                                                          \
    do {                                                                     \
        if (t_failures)                                                      \
            fprintf(stderr, "%d check(s) failed\n", t_failures);             \
        return t_failures ? 1 : 0;                                           \
    } while (0)

static inline char *t_mkdtemp(void)
{
    const char *base = getenv("TMPDIR");
    char *tmpl = NULL;
    if (asprintf(&tmpl, "%s/extapp-test-XXXXXX", base ? base : "/tmp") < 0)
        return NULL;
    return mkdtemp(tmpl);
}

static inline void t_write(const char *path, const void *data, size_t len,
                           mode_t mode)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fwrite(data, 1, len, f);
    fclose(f);
    chmod(path, mode);
}

static inline void t_rm_rf(const char *path)
{
    char *cmd = NULL;
    if (asprintf(&cmd, "rm -rf '%s'", path) >= 0) {
        if (system(cmd) != 0)
            fprintf(stderr, "cleanup of %s failed\n", path);
        free(cmd);
    }
}

#endif
