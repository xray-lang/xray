/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_cookie.h - HTTP Cookie management
 *
 * KEY CONCEPT:
 *   Cookie parsing/serialization, Cookie Jar auto-management,
 *   domain/path matching, and expiration handling.
 */

#ifndef XR_STDLIB_HTTP_COOKIE_H
#define XR_STDLIB_HTTP_COOKIE_H

#include "../../src/base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

// Forward declaration
typedef struct XrayIsolate XrayIsolate;

/* ========== Cookie Structure ========== */

// SameSite attribute values (RFC 6265bis §4.1.2.7).
//   UNSPECIFIED = no SameSite attribute in Set-Cookie. Modern browsers
//                 treat this as Lax by default; the jar preserves it as
//                 its own bucket so callers can tell "explicit Lax" apart
//                 from "server didn't set one".
//   NONE        = cross-site traffic allowed. Servers MUST send this
//                 together with Secure; a Set-Cookie without Secure is
//                 rejected during parsing.
//   LAX         = same-site + top-level navigation GETs.
//   STRICT      = same-site only.
typedef enum {
    XR_SAMESITE_UNSPECIFIED = 0,
    XR_SAMESITE_NONE,
    XR_SAMESITE_LAX,
    XR_SAMESITE_STRICT,
} XrSameSite;

typedef struct XrHttpCookie {
    char *name;            // Cookie name
    char *value;           // Cookie value
    char *domain;          // Domain
    char *path;            // Path
    time_t expires;        // Expiration time (0 = session cookie)
    bool secure;           // HTTPS only
    bool http_only;        // No JS access
    XrSameSite same_site;  // SameSite attribute (RFC 6265bis)

    struct XrHttpCookie *next;  // Next in linked list
} XrHttpCookie;

/* ========== Cookie Jar ========== */

typedef struct XrCookieJar {
    XrHttpCookie *cookies;  // Cookie linked list
    int count;              // Cookie count
    int max_cookies;        // Max cookies (default 300)
    int max_per_domain;     // Max per domain (default 50)
} XrCookieJar;

/* ========== Cookie Parse API ========== */

/*
 * Parse Set-Cookie header
 * Returns: new cookie (caller must free)
 */
XR_FUNC XrHttpCookie *xr_cookie_parse(const char *set_cookie_header, const char *request_domain,
                                      const char *request_path);

/*
 * Free cookie
 */
XR_FUNC void xr_cookie_free(XrHttpCookie *cookie);

/*
 * Serialize cookie (generate Cookie header value)
 * Returns: newly allocated string (caller must free)
 */
XR_FUNC char *xr_cookie_serialize(XrHttpCookie *cookie);

/* ========== Cookie Jar API ========== */

/*
 * Create cookie jar
 */
XR_FUNC XrCookieJar *xr_cookie_jar_new(void);

/*
 * Free cookie jar
 */
XR_FUNC void xr_cookie_jar_free(XrCookieJar *jar);

/*
 * Add cookie to jar
 * Auto-handles:
 * - Same-name cookie replacement
 * - Expired cookie deletion
 * - Count limit
 */
XR_FUNC void xr_cookie_jar_add(XrCookieJar *jar, XrHttpCookie *cookie);

/*
 * Extract and add cookies from response headers
 * headers: "Set-Cookie: ..." header array
 * count: Header count
 */
XR_FUNC void xr_cookie_jar_add_from_response(XrCookieJar *jar, const char **set_cookie_headers,
                                             int count, const char *request_domain,
                                             const char *request_path);

/*
 * Get Cookie header value for specified URL
 * Returns: newly allocated string "name1=value1; name2=value2" (caller must free)
 */
XR_FUNC char *xr_cookie_jar_get_header(XrCookieJar *jar, const char *domain, const char *path,
                                       bool is_secure);

/*
 * Clear expired cookies
 */
XR_FUNC void xr_cookie_jar_cleanup(XrCookieJar *jar);

/*
 * Clear all cookies for specified domain
 */
XR_FUNC void xr_cookie_jar_clear_domain(XrCookieJar *jar, const char *domain);

/*
 * Clear all cookies
 */
XR_FUNC void xr_cookie_jar_clear(XrCookieJar *jar);

/* ========== Global Cookie Jar ========== */

/*
 * Get Isolate's cookie jar (auto-create)
 */
XR_FUNC XrCookieJar *xr_get_cookie_jar(XrayIsolate *X);

/*
 * Set whether cookie jar is enabled
 */
XR_FUNC void xr_set_cookie_jar_enabled(XrayIsolate *X, bool enabled);

/*
 * Check if cookie jar is enabled
 */
XR_FUNC bool xr_is_cookie_jar_enabled(XrayIsolate *X);

#endif
