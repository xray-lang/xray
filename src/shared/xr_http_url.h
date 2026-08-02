/* Shared private URL authority contract for hosted HTTP transports. */
#ifndef XR_SHARED_HTTP_URL_H
#define XR_SHARED_HTTP_URL_H

#include <stdbool.h>

#define XR_HTTP_DEFAULT_PORT 80
#define XR_HTTP_DEFAULT_HTTPS_PORT 443

typedef struct XrHttpUrl {
    char *scheme;
    char *host;
    int port;
    char *path;
    bool is_https;
} XrHttpUrl;

int http_url_parse(const char *url, XrHttpUrl *out);
void http_url_free(XrHttpUrl *url);

#endif
