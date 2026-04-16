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

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

// Forward declaration
typedef struct XrayIsolate XrayIsolate;

/* ========== Cookie Structure ========== */

typedef struct XrHttpCookie {
    char *name;             // Cookie name
    char *value;            // Cookie value
    char *domain;           // Domain
    char *path;             // Path
    time_t expires;         // Expiration time (0 = session cookie)
    bool secure;            // HTTPS only
    bool http_only;         // No JS access
    bool same_site_strict;  // SameSite=Strict
    bool same_site_lax;     // SameSite=Lax
    
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
XrHttpCookie* xr_cookie_parse(const char *set_cookie_header, 
                               const char *request_domain,
                               const char *request_path);

/*
 * Free cookie
 */
void xr_cookie_free(XrHttpCookie *cookie);

/*
 * Serialize cookie (generate Cookie header value)
 * Returns: newly allocated string (caller must free)
 */
char* xr_cookie_serialize(XrHttpCookie *cookie);

/* ========== Cookie Jar API ========== */

/*
 * Create cookie jar
 */
XrCookieJar* xr_cookie_jar_new(void);

/*
 * Free cookie jar
 */
void xr_cookie_jar_free(XrCookieJar *jar);

/*
 * Add cookie to jar
 * Auto-handles:
 * - Same-name cookie replacement
 * - Expired cookie deletion
 * - Count limit
 */
void xr_cookie_jar_add(XrCookieJar *jar, XrHttpCookie *cookie);

/*
 * Extract and add cookies from response headers
 * headers: "Set-Cookie: ..." header array
 * count: Header count
 */
void xr_cookie_jar_add_from_response(XrCookieJar *jar,
                                      const char **set_cookie_headers,
                                      int count,
                                      const char *request_domain,
                                      const char *request_path);

/*
 * Get Cookie header value for specified URL
 * Returns: newly allocated string "name1=value1; name2=value2" (caller must free)
 */
char* xr_cookie_jar_get_header(XrCookieJar *jar, 
                                const char *domain,
                                const char *path,
                                bool is_secure);

/*
 * Clear expired cookies
 */
void xr_cookie_jar_cleanup(XrCookieJar *jar);

/*
 * Clear all cookies for specified domain
 */
void xr_cookie_jar_clear_domain(XrCookieJar *jar, const char *domain);

/*
 * Clear all cookies
 */
void xr_cookie_jar_clear(XrCookieJar *jar);

/* ========== Global Cookie Jar ========== */

/*
 * Get Isolate's cookie jar (auto-create)
 */
XrCookieJar* xr_get_cookie_jar(XrayIsolate *X);

/*
 * Set whether cookie jar is enabled
 */
void xr_set_cookie_jar_enabled(XrayIsolate *X, bool enabled);

/*
 * Check if cookie jar is enabled
 */
bool xr_is_cookie_jar_enabled(XrayIsolate *X);

#endif
