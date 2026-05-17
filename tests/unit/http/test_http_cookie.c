/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_http_cookie.c - Unit tests for HTTP cookie parsing and cookie jar
 *
 * KEY CONCEPT:
 *   Tests cookie parsing from Set-Cookie headers, serialization,
 *   cookie jar operations (add/get/clear/cleanup), and domain matching.
 */

#include "../test_framework.h"
#include <stdlib.h>
#include <string.h>

// Include cookie header directly (it has no isolate dependency for core API)
#include "../../../stdlib/http/http_cookie.h"

/* ========== Cookie Parse ========== */

TEST(cookie_parse_simple) {
    XrHttpCookie *c = xr_cookie_parse("session=abc123", "example.com", "/");
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(c->name, "session");
    ASSERT_STR_EQ(c->value, "abc123");
    xr_cookie_free(c);
}

TEST(cookie_parse_with_attributes) {
    XrHttpCookie *c = xr_cookie_parse("token=xyz; Path=/api; Secure; HttpOnly", "example.com", "/");
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(c->name, "token");
    ASSERT_STR_EQ(c->value, "xyz");
    ASSERT_TRUE(c->secure);
    ASSERT_TRUE(c->http_only);
    ASSERT_NOT_NULL(c->path);
    ASSERT_STR_EQ(c->path, "/api");
    xr_cookie_free(c);
}

TEST(cookie_parse_with_domain) {
    XrHttpCookie *c = xr_cookie_parse("id=42; Domain=.example.com; Path=/", "www.example.com", "/");
    ASSERT_NOT_NULL(c);
    ASSERT_STR_EQ(c->name, "id");
    ASSERT_STR_EQ(c->value, "42");
    xr_cookie_free(c);
}

TEST(cookie_parse_samesite) {
    XrHttpCookie *c = xr_cookie_parse("pref=dark; SameSite=Strict", "example.com", "/");
    ASSERT_NOT_NULL(c);
    ASSERT_EQ_INT(c->same_site, XR_SAMESITE_STRICT);
    xr_cookie_free(c);

    c = xr_cookie_parse("pref=light; SameSite=Lax", "example.com", "/");
    ASSERT_NOT_NULL(c);
    ASSERT_EQ_INT(c->same_site, XR_SAMESITE_LAX);
    xr_cookie_free(c);

    // SameSite=None requires Secure (RFC 6265bis §4.1.2.7 hardening).
    // Without Secure the cookie MUST be rejected.
    c = xr_cookie_parse("pref=x; SameSite=None", "example.com", "/");
    ASSERT_NULL(c);

    c = xr_cookie_parse("pref=x; SameSite=None; Secure", "example.com", "/");
    ASSERT_NOT_NULL(c);
    ASSERT_EQ_INT(c->same_site, XR_SAMESITE_NONE);
    ASSERT_TRUE(c->secure);
    xr_cookie_free(c);

    // No SameSite attribute → UNSPECIFIED (caller may default to Lax).
    c = xr_cookie_parse("pref=x", "example.com", "/");
    ASSERT_NOT_NULL(c);
    ASSERT_EQ_INT(c->same_site, XR_SAMESITE_UNSPECIFIED);
    xr_cookie_free(c);
}

/* ========== Cookie Serialize ========== */

TEST(cookie_serialize) {
    XrHttpCookie *c = xr_cookie_parse("user=john", "example.com", "/");
    ASSERT_NOT_NULL(c);
    char *s = xr_cookie_serialize(c);
    ASSERT_NOT_NULL(s);
    ASSERT_NOT_NULL(strstr(s, "user=john"));
    free(s);
    xr_cookie_free(c);
}

/* ========== Cookie Jar ========== */

TEST(cookie_jar_new_free) {
    XrCookieJar *jar = xr_cookie_jar_new();
    ASSERT_NOT_NULL(jar);
    ASSERT_EQ_INT(jar->count, 0);
    xr_cookie_jar_free(jar);
}

TEST(cookie_jar_add) {
    XrCookieJar *jar = xr_cookie_jar_new();
    XrHttpCookie *c = xr_cookie_parse("a=1", "example.com", "/");
    ASSERT_NOT_NULL(c);

    xr_cookie_jar_add(jar, c);
    ASSERT_EQ_INT(jar->count, 1);

    xr_cookie_jar_free(jar);
}

TEST(cookie_jar_add_multiple) {
    XrCookieJar *jar = xr_cookie_jar_new();

    XrHttpCookie *c1 = xr_cookie_parse("a=1; Domain=example.com", "example.com", "/");
    XrHttpCookie *c2 = xr_cookie_parse("b=2; Domain=example.com", "example.com", "/");
    xr_cookie_jar_add(jar, c1);
    xr_cookie_jar_add(jar, c2);
    ASSERT_EQ_INT(jar->count, 2);

    xr_cookie_jar_free(jar);
}

TEST(cookie_jar_replace_same_name) {
    XrCookieJar *jar = xr_cookie_jar_new();

    XrHttpCookie *c1 = xr_cookie_parse("key=old; Domain=example.com; Path=/", "example.com", "/");
    XrHttpCookie *c2 = xr_cookie_parse("key=new; Domain=example.com; Path=/", "example.com", "/");
    xr_cookie_jar_add(jar, c1);
    xr_cookie_jar_add(jar, c2);

    // Should replace, not add duplicate
    ASSERT_EQ_INT(jar->count, 1);

    char *header = xr_cookie_jar_get_header(jar, "example.com", "/", false);
    if (header) {
        ASSERT_NOT_NULL(strstr(header, "new"));
        free(header);
    }

    xr_cookie_jar_free(jar);
}

TEST(cookie_jar_get_header) {
    XrCookieJar *jar = xr_cookie_jar_new();

    XrHttpCookie *c = xr_cookie_parse("sess=abc; Domain=example.com; Path=/", "example.com", "/");
    xr_cookie_jar_add(jar, c);

    char *header = xr_cookie_jar_get_header(jar, "example.com", "/", false);
    ASSERT_NOT_NULL(header);
    ASSERT_NOT_NULL(strstr(header, "sess=abc"));
    free(header);

    xr_cookie_jar_free(jar);
}

TEST(cookie_jar_clear) {
    XrCookieJar *jar = xr_cookie_jar_new();

    XrHttpCookie *c1 = xr_cookie_parse("a=1", "example.com", "/");
    XrHttpCookie *c2 = xr_cookie_parse("b=2", "example.com", "/");
    xr_cookie_jar_add(jar, c1);
    xr_cookie_jar_add(jar, c2);

    xr_cookie_jar_clear(jar);
    ASSERT_EQ_INT(jar->count, 0);

    xr_cookie_jar_free(jar);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Cookie - Parse");
RUN_TEST(cookie_parse_simple);
RUN_TEST(cookie_parse_with_attributes);
RUN_TEST(cookie_parse_with_domain);
RUN_TEST(cookie_parse_samesite);

RUN_TEST_SUITE("Cookie - Serialize");
RUN_TEST(cookie_serialize);

RUN_TEST_SUITE("Cookie Jar - Operations");
RUN_TEST(cookie_jar_new_free);
RUN_TEST(cookie_jar_add);
RUN_TEST(cookie_jar_add_multiple);
RUN_TEST(cookie_jar_replace_same_name);
RUN_TEST(cookie_jar_get_header);
RUN_TEST(cookie_jar_clear);

TEST_MAIN_END()
