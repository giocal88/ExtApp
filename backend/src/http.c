/* Thin libcurl wrapper: GET into memory with size cap and cancellation.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "private.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#define DEFAULT_MAX_BYTES (5u * 1024u * 1024u)

typedef struct {
    EaStrBuf buf;
    size_t max;
    bool too_big;
    ExtAppCancelFn cancel;
    void *ud;
} Transfer;

static size_t on_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    Transfer *t = userdata;
    size_t n = size * nmemb;
    if (t->buf.len + n > t->max) {
        t->too_big = true;
        return 0; /* abort */
    }
    return ea_sb_append_len(&t->buf, ptr, n) ? n : 0;
}

static int on_progress(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    Transfer *t = userdata;
    return (t->cancel && t->cancel(t->ud)) ? 1 : 0;
}

char *ea_url_encode(const char *s)
{
    EaStrBuf sb = {0};
    ea_sb_append(&sb, "");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isalnum(*p) || strchr("-_.~", *p))
            ea_sb_append_c(&sb, (char)*p);
        else if (*p == ' ')
            ea_sb_append_c(&sb, '+');
        else
            ea_sb_printf(&sb, "%%%02X", *p);
    }
    return ea_sb_steal(&sb);
}

ExtAppStatus ea_http_get(ExtAppContext *ctx, const char *url,
                         const EaHttpOptions *opts, ExtAppCancelFn cancel,
                         void *user_data, char **body, size_t *len,
                         long *http_code, char **err)
{
    *body = NULL;
    if (len)
        *len = 0;
    if (http_code)
        *http_code = 0;

    bool https = strncmp(url, "https://", 8) == 0;
    bool http = strncmp(url, "http://", 7) == 0;
    if (!https && !(http && !opts->https_only)) {
        ea_set_err(err, "Refusing to fetch unsupported URL: %s", url);
        return EXTAPP_ERR_INVALID_ARG;
    }

    CURL *curl = curl_easy_init();
    if (!curl)
        return EXTAPP_ERR_NOMEM;

    Transfer t = {.max = opts->max_bytes ? opts->max_bytes : DEFAULT_MAX_BYTES,
                  .cancel = cancel, .ud = user_data};
    struct curl_slist *hdrs = NULL;
    for (const char *const *h = opts->headers; h && *h; h++)
        hdrs = curl_slist_append(hdrs, *h);

    char errbuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ctx->user_agent);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    if (opts->post_body)
        curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, opts->post_body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); /* gzip, br, ... */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, opts->timeout_sec ? opts->timeout_sec : 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); /* thread safety */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500 /* 7.85.0 */
    const char *protos = opts->https_only ? "https" : "http,https";
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, protos);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, protos);
#else
    long protos = opts->https_only ? CURLPROTO_HTTPS : (CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, protos);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, protos);
#endif

    CURLcode cc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    curl_slist_free_all(hdrs);

    if (http_code)
        *http_code = code;

    if (cc == CURLE_ABORTED_BY_CALLBACK) {
        ea_sb_clear(&t.buf);
        ea_set_err(err, "Download cancelled");
        return EXTAPP_ERR_CANCELLED;
    }
    if (t.too_big) {
        ea_sb_clear(&t.buf);
        ea_set_err(err, "Response from %s is larger than %zu bytes", url, t.max);
        return EXTAPP_ERR_NETWORK;
    }
    if (cc != CURLE_OK) {
        ea_sb_clear(&t.buf);
        ea_set_err(err, "Request failed: %s", errbuf[0] ? errbuf : curl_easy_strerror(cc));
        return EXTAPP_ERR_NETWORK;
    }
    if (code < 200 || code >= 300) {
        ea_sb_clear(&t.buf);
        ea_set_err(err, "Server answered HTTP %ld for %s", code, url);
        return EXTAPP_ERR_NETWORK;
    }

    if (len)
        *len = t.buf.len;
    *body = ea_sb_steal(&t.buf);
    return *body ? EXTAPP_OK : EXTAPP_ERR_NOMEM;
}

ExtAppStatus extapp_http_fetch(ExtAppContext *ctx, const char *url,
                               size_t max_bytes, ExtAppCancelFn cancel,
                               void *user_data, unsigned char **data,
                               size_t *len, char **err)
{
    if (!ctx || !url || !data || !len) {
        ea_set_err(err, "Invalid arguments");
        return EXTAPP_ERR_INVALID_ARG;
    }
    EaHttpOptions opts = {.headers = NULL, .https_only = false,
                          .max_bytes = max_bytes, .timeout_sec = 20};
    char *body = NULL;
    ExtAppStatus st = ea_http_get(ctx, url, &opts, cancel, user_data, &body,
                                  len, NULL, err);
    *data = (unsigned char *)body;
    return st;
}
