/*
 * Shared private HTTP transport primitives for hosted HTTP backends.
 *
 * URL authority parsing, the zero-copy header view, and the redirect-status
 * predicate live here so the HTTP/2 transport and the migration oracle stay
 * self-contained now that the HTTP/1.x request path is owned by pure-Xray
 * stdlib/http/http.xr.
 */
#ifndef XR_SHARED_HTTP_URL_H
#define XR_SHARED_HTTP_URL_H

#include <stdbool.h>
#include <stddef.h>

#define XR_HTTP_DEFAULT_PORT 80
#define XR_HTTP_DEFAULT_HTTPS_PORT 443

typedef struct XrHttpUrl {
    char *scheme;
    char *host;
    int port;
    char *path;
    bool is_https;
} XrHttpUrl;

/* Zero-copy HTTP header view: name/value point into a caller-owned buffer. */
typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} XrHttpHeader;

int http_url_parse(const char *url, XrHttpUrl *out);
void http_url_free(XrHttpUrl *url);

/* True for the HTTP redirect status codes (301, 302, 303, 307, 308). */
bool is_redirect_status(int status);

#endif
