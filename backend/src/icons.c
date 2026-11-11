/* Storing icon images. The backend never decodes images: it only checks the
 * format from magic bytes so the file gets a correct extension.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "private.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ICON_MAX_BYTES (5u * 1024u * 1024u)

const char *ea_sniff_image(const unsigned char *d, size_t n)
{
    if (n >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0)
        return "png";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return "jpg";
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0)
        return "webp";
    if (n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 1 && d[3] == 0)
        return "ico";

    /* SVG: text starting (after BOM/whitespace) with "<", containing "<svg" */
    size_t i = 0;
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF)
        i = 3;
    while (i < n && isspace(d[i]))
        i++;
    if (i < n && d[i] == '<') {
        size_t lim = n < 4096 ? n : 4096;
        for (size_t j = i; j + 4 <= lim; j++)
            if (memcmp(d + j, "<svg", 4) == 0)
                return "svg";
    }
    return NULL;
}

ExtAppStatus extapp_icon_save(ExtAppContext *ctx, const char *name_hint,
                              const unsigned char *data, size_t len,
                              char **out_path, char **err)
{
    if (!ctx || !data || !len || !out_path) {
        ea_set_err(err, "Invalid arguments");
        return EXTAPP_ERR_INVALID_ARG;
    }
    *out_path = NULL;
    if (len > ICON_MAX_BYTES) {
        ea_set_err(err, "Icon is too large");
        return EXTAPP_ERR_INVALID_ARG;
    }
    const char *ext = ea_sniff_image(data, len);
    if (!ext) {
        ea_set_err(err, "Unsupported image format (use PNG, SVG, JPEG, WebP or ICO)");
        return EXTAPP_ERR_UNSUPPORTED;
    }

    ExtAppStatus st = ea_mkdir_p(ctx->icons_dir, err);
    if (st != EXTAPP_OK)
        return st;

    char *slug = ea_slugify(name_hint && *name_hint ? name_hint : "icon");
    char *path = NULL;
    for (int i = 1; slug && i < 1000 && !path; i++) {
        char *file = i == 1 ? ea_strdup_printf("%s.%s", slug, ext)
                            : ea_strdup_printf("%s-%d.%s", slug, i, ext);
        char *full = file ? ea_path_join(ctx->icons_dir, file) : NULL;
        free(file);
        if (full && access(full, F_OK) != 0 && errno == ENOENT)
            path = full;
        else
            free(full);
    }
    free(slug);
    if (!path) {
        ea_set_err(err, "Could not find a free file name for the icon");
        return EXTAPP_ERR_IO;
    }

    st = ea_write_file_atomic(path, data, len, 0644, err);
    if (st != EXTAPP_OK) {
        free(path);
        return st;
    }
    *out_path = path;
    return EXTAPP_OK;
}
