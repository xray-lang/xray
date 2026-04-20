/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_cookie.c - HTTP Cookie management implementation
 *
 * KEY CONCEPT:
 *   RFC 6265 compliant cookie parsing and storage
 */

#include "http_cookie.h"
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ========== Internal Helper Functions ========== */

// Skip whitespace
static const char* skip_whitespace(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

// Copy string (with length)
static char* strdup_n(const char *s, size_t len) {
    char *copy = (char*)malloc(len + 1);
    if (copy) {
        memcpy(copy, s, len);
        copy[len] = '\0';
    }
    return copy;
}

// Domain matching (supports subdomains)
static bool domain_matches(const char *cookie_domain, const char *request_domain) {
    if (!cookie_domain || !request_domain) return false;

    size_t cookie_len = strlen(cookie_domain);
    size_t request_len = strlen(request_domain);

    // Exact match
    if (strcasecmp(cookie_domain, request_domain) == 0) return true;

    // Subdomain match: cookie_domain starts with .
    if (cookie_domain[0] == '.') {
        if (request_len >= cookie_len - 1) {
            const char *suffix = request_domain + request_len - (cookie_len - 1);
            if (strcasecmp(suffix, cookie_domain + 1) == 0) {
                // Ensure complete subdomain boundary
                if (suffix == request_domain || *(suffix - 1) == '.') {
                    return true;
                }
            }
        }
    }

    // Request domain is subdomain of cookie domain
    if (request_len > cookie_len + 1) {
        const char *suffix = request_domain + request_len - cookie_len;
        if (strcasecmp(suffix, cookie_domain) == 0 && *(suffix - 1) == '.') {
            return true;
        }
    }

    return false;
}

// Path matching
static bool path_matches(const char *cookie_path, const char *request_path) {
    if (!cookie_path || !request_path) return true;

    size_t cookie_len = strlen(cookie_path);
    size_t request_len = strlen(request_path);

    // Cookie path must be prefix of request path
    if (request_len < cookie_len) return false;
    if (strncmp(cookie_path, request_path, cookie_len) != 0) return false;

    // Boundary check
    if (request_len == cookie_len) return true;
    if (cookie_path[cookie_len - 1] == '/') return true;
    if (request_path[cookie_len] == '/') return true;

    return false;
}

// Parse date (simplified, supports common formats)
static time_t parse_cookie_date(const char *date_str) {
    struct tm tm = {0};

    // Try RFC 1123 format: Sun, 06 Nov 1994 08:49:37 GMT
    if (strptime(date_str, "%a, %d %b %Y %H:%M:%S", &tm)) {
        return timegm(&tm);
    }

    // Try RFC 850 format: Sunday, 06-Nov-94 08:49:37 GMT
    if (strptime(date_str, "%A, %d-%b-%y %H:%M:%S", &tm)) {
        return timegm(&tm);
    }

    // Try ANSI C format: Sun Nov  6 08:49:37 1994
    if (strptime(date_str, "%a %b %d %H:%M:%S %Y", &tm)) {
        return timegm(&tm);
    }

    return 0;
}

/* ========== Cookie Parsing ========== */

// Defensive upper bound on Set-Cookie input length. RFC 6265 §6.1
// recommends UAs accept at least 4096 bytes per cookie. We allow 8 KiB
// (per-cookie, already beyond any real-world server) to stop a hostile
// server from forcing us into O(N) scans over arbitrarily large headers.
#define XR_COOKIE_MAX_INPUT_LEN  8192

XrHttpCookie* xr_cookie_parse(const char *set_cookie_header,
                               const char *request_domain,
                               const char *request_path) {
    if (!set_cookie_header) return NULL;
    // strnlen caps the scan at XR_COOKIE_MAX_INPUT_LEN + 1 so a missing
    // NUL terminator also trips the check instead of overrunning memory.
    size_t header_len = strnlen(set_cookie_header, XR_COOKIE_MAX_INPUT_LEN + 1);
    if (header_len > XR_COOKIE_MAX_INPUT_LEN) return NULL;

    XrHttpCookie *cookie = (XrHttpCookie*)calloc(1, sizeof(XrHttpCookie));
    if (!cookie) return NULL;

    const char *p = set_cookie_header;

    // Parse name=value
    const char *eq = strchr(p, '=');
    if (!eq) {
        free(cookie);
        return NULL;
    }

    // Extract name
    const char *name_start = skip_whitespace(p);
    const char *name_end = eq;
    while (name_end > name_start && isspace((unsigned char)*(name_end - 1))) name_end--;
    cookie->name = strdup_n(name_start, name_end - name_start);

    // Extract value
    p = eq + 1;
    const char *value_start = skip_whitespace(p);
    const char *value_end = value_start;
    while (*value_end && *value_end != ';') value_end++;
    while (value_end > value_start && isspace((unsigned char)*(value_end - 1))) value_end--;
    cookie->value = strdup_n(value_start, value_end - value_start);

    // Parse attributes
    p = value_end;
    while (*p) {
        if (*p == ';') p++;
        p = skip_whitespace(p);
        if (!*p) break;

        const char *attr_start = p;
        while (*p && *p != '=' && *p != ';') p++;
        size_t attr_len = p - attr_start;

        char *attr_value = NULL;
        if (*p == '=') {
            p++;
            const char *val_start = p;
            while (*p && *p != ';') p++;
            attr_value = strdup_n(val_start, p - val_start);
        }

        // Match attributes
        if (strncasecmp(attr_start, "Domain", attr_len) == 0 && attr_value) {
            // Remove leading dot
            if (attr_value[0] == '.') {
                cookie->domain = strdup(attr_value + 1);
                free(attr_value);
            } else {
                cookie->domain = attr_value;
            }
        } else if (strncasecmp(attr_start, "Path", attr_len) == 0 && attr_value) {
            cookie->path = attr_value;
        } else if (strncasecmp(attr_start, "Expires", attr_len) == 0 && attr_value) {
            cookie->expires = parse_cookie_date(attr_value);
            free(attr_value);
        } else if (strncasecmp(attr_start, "Max-Age", attr_len) == 0 && attr_value) {
            int max_age = atoi(attr_value);
            cookie->expires = time(NULL) + max_age;
            free(attr_value);
        } else if (strncasecmp(attr_start, "Secure", attr_len) == 0) {
            cookie->secure = true;
            if (attr_value) free(attr_value);
        } else if (strncasecmp(attr_start, "HttpOnly", attr_len) == 0) {
            cookie->http_only = true;
            if (attr_value) free(attr_value);
        } else if (strncasecmp(attr_start, "SameSite", attr_len) == 0 && attr_value) {
            // RFC 6265bis §4.1.2.7: three valid tokens — Strict / Lax /
            // None. Unknown tokens leave the field at UNSPECIFIED, which
            // browsers map to the default (Lax in modern UAs).
            if (strcasecmp(attr_value, "Strict") == 0) {
                cookie->same_site = XR_SAMESITE_STRICT;
            } else if (strcasecmp(attr_value, "Lax") == 0) {
                cookie->same_site = XR_SAMESITE_LAX;
            } else if (strcasecmp(attr_value, "None") == 0) {
                cookie->same_site = XR_SAMESITE_NONE;
            }
            free(attr_value);
        } else {
            if (attr_value) free(attr_value);
        }
    }

    // Default domain and path
    if (!cookie->domain && request_domain) {
        cookie->domain = strdup(request_domain);
    }
    if (!cookie->path) {
        if (request_path) {
            // Use directory part of request path
            const char *last_slash = strrchr(request_path, '/');
            if (last_slash && last_slash != request_path) {
                cookie->path = strdup_n(request_path, last_slash - request_path);
            } else {
                cookie->path = strdup("/");
            }
        } else {
            cookie->path = strdup("/");
        }
    }

    // RFC 6265bis §4.1.2.7 hardening: SameSite=None is only valid when
    // combined with Secure. All major browsers reject the cookie in this
    // case (since Chrome 80). Mirror that behaviour so a misconfigured
    // server can't downgrade our cookie jar into sending sensitive cookies
    // over plain HTTP.
    if (cookie->same_site == XR_SAMESITE_NONE && !cookie->secure) {
        xr_cookie_free(cookie);
        return NULL;
    }

    return cookie;
}

void xr_cookie_free(XrHttpCookie *cookie) {
    if (!cookie) return;
    free(cookie->name);
    free(cookie->value);
    free(cookie->domain);
    free(cookie->path);
    free(cookie);
}

char* xr_cookie_serialize(XrHttpCookie *cookie) {
    if (!cookie || !cookie->name || !cookie->value) return NULL;

    size_t len = strlen(cookie->name) + strlen(cookie->value) + 2;
    char *buf = (char*)malloc(len);
    if (buf) {
        snprintf(buf, len, "%s=%s", cookie->name, cookie->value);
    }
    return buf;
}

/* ========== Cookie Jar ========== */

XrCookieJar* xr_cookie_jar_new(void) {
    XrCookieJar *jar = (XrCookieJar*)calloc(1, sizeof(XrCookieJar));
    if (jar) {
        jar->max_cookies = 300;
        jar->max_per_domain = 50;
    }
    return jar;
}

void xr_cookie_jar_free(XrCookieJar *jar) {
    if (!jar) return;
    xr_cookie_jar_clear(jar);
    free(jar);
}

void xr_cookie_jar_add(XrCookieJar *jar, XrHttpCookie *cookie) {
    if (!jar || !cookie) return;

    // Check if cookie with same name exists (same domain and path)
    XrHttpCookie **pp = &jar->cookies;
    while (*pp) {
        XrHttpCookie *c = *pp;
        if (strcmp(c->name, cookie->name) == 0 &&
            ((!c->domain && !cookie->domain) ||
             (c->domain && cookie->domain && strcasecmp(c->domain, cookie->domain) == 0)) &&
            ((!c->path && !cookie->path) ||
             (c->path && cookie->path && strcmp(c->path, cookie->path) == 0))) {
            // Replace
            cookie->next = c->next;
            *pp = cookie;
            xr_cookie_free(c);
            return;
        }
        pp = &c->next;
    }

    // Check count limit
    if (jar->count >= jar->max_cookies) {
        // Delete oldest cookie
        if (jar->cookies) {
            XrHttpCookie *old = jar->cookies;
            jar->cookies = old->next;
            xr_cookie_free(old);
            jar->count--;
        }
    }

    // Add to head of list
    cookie->next = jar->cookies;
    jar->cookies = cookie;
    jar->count++;
}

void xr_cookie_jar_add_from_response(XrCookieJar *jar,
                                      const char **set_cookie_headers,
                                      int count,
                                      const char *request_domain,
                                      const char *request_path) {
    if (!jar || !set_cookie_headers) return;

    for (int i = 0; i < count; i++) {
        XrHttpCookie *cookie = xr_cookie_parse(set_cookie_headers[i],
                                                request_domain, request_path);
        if (cookie) {
            xr_cookie_jar_add(jar, cookie);
        }
    }
}

char* xr_cookie_jar_get_header(XrCookieJar *jar,
                                const char *domain,
                                const char *path,
                                bool is_secure) {
    if (!jar || !domain) return NULL;

    // Calculate buffer size
    size_t buf_size = 1024;
    char *buf = (char*)malloc(buf_size);
    if (!buf) return NULL;
    buf[0] = '\0';

    size_t pos = 0;
    time_t now = time(NULL);

    XrHttpCookie *c = jar->cookies;
    while (c) {
        // Check expiration
        if (c->expires > 0 && c->expires < now) {
            c = c->next;
            continue;
        }

        // Check domain match
        if (!domain_matches(c->domain, domain)) {
            c = c->next;
            continue;
        }

        // Check path match
        if (!path_matches(c->path, path)) {
            c = c->next;
            continue;
        }

        // Check Secure flag
        if (c->secure && !is_secure) {
            c = c->next;
            continue;
        }

        // Serialize cookie
        char *serialized = xr_cookie_serialize(c);
        if (serialized) {
            size_t ser_len = strlen(serialized);

            // Expand buffer
            while (pos + ser_len + 3 > buf_size) {
                buf_size *= 2;
                char *new_buf = (char*)realloc(buf, buf_size);
                if (!new_buf) {
                    free(buf);
                    free(serialized);
                    return NULL;
                }
                buf = new_buf;
            }

            // Add delimiter
            if (pos > 0) {
                buf[pos++] = ';';
                buf[pos++] = ' ';
            }

            memcpy(buf + pos, serialized, ser_len);
            pos += ser_len;
            buf[pos] = '\0';

            free(serialized);
        }

        c = c->next;
    }

    if (pos == 0) {
        free(buf);
        return NULL;
    }

    return buf;
}

void xr_cookie_jar_cleanup(XrCookieJar *jar) {
    if (!jar) return;

    time_t now = time(NULL);
    XrHttpCookie **pp = &jar->cookies;

    while (*pp) {
        XrHttpCookie *c = *pp;
        if (c->expires > 0 && c->expires < now) {
            *pp = c->next;
            xr_cookie_free(c);
            jar->count--;
        } else {
            pp = &c->next;
        }
    }
}

void xr_cookie_jar_clear_domain(XrCookieJar *jar, const char *domain) {
    if (!jar || !domain) return;

    XrHttpCookie **pp = &jar->cookies;
    while (*pp) {
        XrHttpCookie *c = *pp;
        if (c->domain && strcasecmp(c->domain, domain) == 0) {
            *pp = c->next;
            xr_cookie_free(c);
            jar->count--;
        } else {
            pp = &c->next;
        }
    }
}

void xr_cookie_jar_clear(XrCookieJar *jar) {
    if (!jar) return;

    XrHttpCookie *c = jar->cookies;
    while (c) {
        XrHttpCookie *next = c->next;
        xr_cookie_free(c);
        c = next;
    }
    jar->cookies = NULL;
    jar->count = 0;
}

/* ========== Cookie Jar (managed via XrHttpContext) ========== */

XrCookieJar* xr_get_cookie_jar(XrayIsolate *X) {
    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx) return NULL;

    if (!ctx->cookie_jar && ctx->cookie_jar_enabled) {
        ctx->cookie_jar = xr_cookie_jar_new();
    }
    return ctx->cookie_jar;
}

void xr_set_cookie_jar_enabled(XrayIsolate *X, bool enabled) {
    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx) return;

    ctx->cookie_jar_enabled = enabled;
    if (!enabled && ctx->cookie_jar) {
        xr_cookie_jar_free(ctx->cookie_jar);
        ctx->cookie_jar = NULL;
    }
}

bool xr_is_cookie_jar_enabled(XrayIsolate *X) {
    XrHttpContext *ctx = xr_http_get_context(X);
    return ctx ? ctx->cookie_jar_enabled : false;
}
