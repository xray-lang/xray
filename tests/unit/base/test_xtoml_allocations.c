/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xtoml_allocations.c - Exhaustive allocation rejection and release checks
 */
#include "base/xmalloc.h"
#include <stdlib.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "failed at %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static size_t attempts, fail_at, live_count, live_bytes;
static struct { void *pointer; size_t size; } slots[1024];

static void *track(void *pointer, size_t size) {
    REQUIRE(pointer);
    for (size_t i = 0; i < XR_COUNTOF(slots); ++i) {
        if (slots[i].pointer) continue;
        slots[i].pointer = pointer;
        slots[i].size = size;
        ++live_count;
        live_bytes += size;
        return pointer;
    }
    exit(1);
}
static void *probe_malloc(size_t size) {
    return ++attempts == fail_at ? NULL : track(xr_malloc(size), size);
}
static void *probe_calloc(size_t count, size_t size) {
    return ++attempts == fail_at ? NULL : track(xr_calloc(count, size), count * size);
}
static void probe_free(void *pointer) {
    if (!pointer) return;
    for (size_t i = 0; i < XR_COUNTOF(slots); ++i) {
        if (slots[i].pointer != pointer) continue;
        slots[i].pointer = NULL;
        --live_count;
        live_bytes -= slots[i].size;
        xr_free(pointer);
        return;
    }
    exit(1);
}
static void *probe_realloc(void *pointer, size_t size) {
    if (++attempts == fail_at) return NULL;
    if (!pointer) return track(xr_realloc(NULL, size), size);
    for (size_t i = 0; i < XR_COUNTOF(slots); ++i) {
        if (slots[i].pointer != pointer) continue;
        void *grown = xr_realloc(pointer, size);
        REQUIRE(grown);
        live_bytes = live_bytes - slots[i].size + size;
        slots[i].pointer = grown;
        slots[i].size = size;
        return grown;
    }
    exit(1);
}
static char *probe_strdup(const char *source) {
    size_t size = strlen(source) + 1;
    char *copy = probe_malloc(size);
    if (copy) memcpy(copy, source, size);
    return copy;
}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc probe_malloc
#define xr_calloc probe_calloc
#define xr_realloc probe_realloc
#define xr_free probe_free
#define xr_strdup probe_strdup
#include "../../../src/base/xtoml.c"

static size_t run(const char *source, size_t failure, bool valid) {
    attempts = 0;
    fail_at = failure;
    XrTomlParseStatus status = XR_TOML_PARSE_OK;
    XrTomlValue *value = xtoml_parse_limited(source, strlen(source),
        (XrTomlParseBudget){65536, 1024 * 1024, 1024 * 1024, 128}, &status, NULL);
    size_t total = attempts;
    if ((failure || !valid) && value) {
        fprintf(stderr, "accepted document at allocation %zu of %zu\n", failure, total);
        exit(1);
    }
    if (!failure && valid) REQUIRE(value);
    REQUIRE(status == (failure ? XR_TOML_PARSE_OUT_OF_MEMORY :
        valid ? XR_TOML_PARSE_OK : XR_TOML_PARSE_INVALID));
    xtoml_free(value);
    REQUIRE(live_count == 0 && live_bytes == 0);
    return total;
}
static void budget_boundaries(void) {
    const char *source = "a.b=[1,2,3]\nx=\"owned\\u0000suffix\"\n";
    size_t length = strlen(source);
    XrTomlParseStatus status;
    attempts = 0; fail_at = 0;
    REQUIRE(!xtoml_parse_limited(source, length, (XrTomlParseBudget){length - 1, 65536, 65536, 128}, &status, NULL));
    REQUIRE(status == XR_TOML_PARSE_LIMIT && !attempts && !live_count && !live_bytes);
    REQUIRE(!xtoml_parse("", 16u * 1024u * 1024u + 1u));
    REQUIRE(!attempts);
    size_t first_success = 0;
    for (size_t bytes = 0; bytes <= 8192; ++bytes) {
        XrTomlValue *root = xtoml_parse_limited(source, length,
            (XrTomlParseBudget){length, bytes, 65536, 128}, &status, NULL);
        if (root) {
            REQUIRE(status == XR_TOML_PARSE_OK);
            XrTomlValue *items = xtoml_get_array(xtoml_get_table(root, "a"), "b");
            REQUIRE(xtoml_array_len(items) == 3 && xtoml_array_get(items, 2)->as.integer == 3);
            first_success = bytes;
        } else REQUIRE(status == XR_TOML_PARSE_LIMIT);
        xtoml_free(root);
        REQUIRE(!live_count && !live_bytes);
        if (first_success) break;
    }
    REQUIRE(first_success);
    printf("TOML allocation budget: every limit through %zu bytes checked\n", first_success);
}

static void require_depth(const char *source, uint32_t depth, bool valid) {
    fail_at = 0;
    XrTomlParseStatus status;
    XrTomlValue *root = xtoml_parse_limited(source, strlen(source),
        (XrTomlParseBudget){65536, 1024 * 1024, 1024 * 1024, depth}, &status, NULL);
    REQUIRE(status == (valid ? XR_TOML_PARSE_OK : XR_TOML_PARSE_LIMIT));
    REQUIRE((root != NULL) == valid);
    xtoml_free(root);
    REQUIRE(!live_count && !live_bytes);
}

static void depth_boundaries(void) {
    require_depth("", 0, true);
    require_depth("x=1\n", 0, false);
    require_depth("x=1\n", 1, true);
    const char *const sources[] = {
        "a=[[[0]]]\n", "[a.b]\nc.d=1\n", "a.b={c=[{d=1}]}\n", "[[a.b]]\nc=[1]\n"
    };
    const uint32_t depths[] = {4, 4, 5, 5};
    for (size_t i = 0; i < XR_COUNTOF(sources); ++i) {
        require_depth(sources[i], depths[i] - 1, false);
        require_depth(sources[i], depths[i], true);
    }
    char deep[20008];
    for (unsigned nesting = 127; nesting <= 128; ++nesting) {
        size_t pos = 0;
        deep[pos++] = 'a'; deep[pos++] = '=';
        for (unsigned i = 0; i < nesting; ++i) deep[pos++] = '[';
        deep[pos++] = '0';
        for (unsigned i = 0; i < nesting; ++i) deep[pos++] = ']';
        deep[pos] = '\0';
        require_depth(deep, UINT32_MAX, nesting == 127);
    }
    memcpy(deep, "a=", 2);
    memset(deep + 2, '[', 5000); deep[5002] = '0';
    memset(deep + 5003, ']', 5000); deep[10003] = '\0';
    require_depth(deep, 128, false);
    for (unsigned i = 0; i < 5000; ++i) { deep[i * 2] = 'a'; deep[i * 2 + 1] = '.'; }
    memcpy(deep + 9999, "=1", 3);
    require_depth(deep, 128, false);
    memcpy(deep, "a=", 2);
    for (unsigned i = 0; i < 5000; ++i) memcpy(deep + 2 + i * 3, "{a=", 3);
    deep[15002] = '0'; memset(deep + 15003, '}', 5000); deep[20003] = '\0';
    require_depth(deep, 128, false);
}

static void work_boundaries(void) {
    const char *source = "same_prefix_aaaa=1\nsame_prefix_aaab=2\nsame_prefix_aaac=3\n";
    size_t length = strlen(source), first_success = 0;
    fail_at = 0;
    for (size_t work_limit = 0; work_limit < 1024; ++work_limit) {
        attempts = 0;
        XrTomlParseStatus status;
        XrTomlValue *root = xtoml_parse_limited(source, length,
            (XrTomlParseBudget){length, 65536, work_limit, 128}, &status, NULL);
        if (root) {
            REQUIRE(status == XR_TOML_PARSE_OK && xtoml_get_int(root, "same_prefix_aaac") == 3);
            first_success = work_limit;
        } else REQUIRE(status == XR_TOML_PARSE_LIMIT);
        if (work_limit < length) REQUIRE(!attempts);
        xtoml_free(root);
        REQUIRE(!live_count && !live_bytes);
        if (first_success) break;
    }
    REQUIRE(first_success > length);
    printf("TOML work budget: every limit through %zu units checked\n", first_success);
    TomlCtx context = {0};
    REQUIRE(!grown_capacity(&context, INT_MAX, sizeof(XrTomlMember)));
    REQUIRE(context.error && context.status == XR_TOML_PARSE_LIMIT);
    context.error = false;
    REQUIRE(!grown_capacity(&context, 4, SIZE_MAX));
    REQUIRE(context.error && context.status == XR_TOML_PARSE_LIMIT);
}

int main(void) {
    depth_boundaries();
    work_boundaries();
    budget_boundaries();
    static const char source[] =
        "title=\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\"\n"
        "a.b.c.d.e.f=1\na.b.c.d.e.g=[1,2,3,4,5,6,7,8,9]\n"
        "empty=\"\"\nempty_literal=''\n\"quoted\"='literal'\n"
        "continued=\"\"\"first\\ \t\r\n \t\n second\"\"\"\n"
        "one=\"\"\"x\"\"\"\"\n"
        "two='''x'''''\n"
        "numbers=[-9223372036854775808,9223372036854775807,0x7fff_ffff_ffff_ffff,0o755,0b1010,1_000.2_5e+0_2,-0.0]\n"
        "dates=[2024-02-29,07:32:00.123456789,1979-05-27 07:32:00.123456789+01:30]\n"
        "escaped=\"\\b\\t\\n\\f\\r\\\"\\\\\\u0041\\U0001F600\"\n"
        "\"a\\u0000b\"=\"x\\u0000y\"\n\"a\\u0000c\"='distinct'\n"
        "\"nested\\u0000key\".child=\"owned\\u0000suffix\"\n"
        "map={a=1,b=2,c=3,d=4,e=5,f=6,g=7,h=8,i=9}\n"
        "[settings.parent]\nx=1\n"
        "[[items]]\nx=1\n[[items]]\nx=2\n[[items]]\nx=3\n"
        "[[items]]\nx=4\n[[items]]\nx=5\n[[items]]\nx=6\n"
        "[[items]]\nx=7\n[[items]]\nx=8\n[[items]]\nx=9\n";
    size_t total = run(source, 0, true);
    for (size_t failure = 1; failure <= total; ++failure)
        run(source, failure, true);
    static const char *const invalid[] = {
        "x={a=1,a=2}\n", "x=1\nx.a=2\n",
        "\"a\\u0000b\"=1\n\"a\\u0000b\"=2\n",
        "\"a\\u0000b\"=1\n\"a\\u0000b\".child=2\n",
        "[a]\nx=1\n[a]\ny=2\n",
        "a.b=1\n[a]\nc=2\n",
        "a={b=1}\na.c=2\n",
        "a={}\n[a.b]\nx=1\n",
        "a=[]\n[[a]]\nx=1\n",
        "a=[{b=1}]\n[a.c]\nx=1\n",
        "[[a]]\nx=1\n[a]\ny=2\n",
        "[a.b]\nx=1\n[a]\nb.y=2\n",
        "a={b={c=1},b.d=2}\n",
        "a={b=1}\n[[a.c]]\nx=1\n",
        "version=1 junk\n",
        "flag=true false\n",
        "[declarations] junk\nversion=1\n",
        "[[items]] junk\nx=1\n",
        "name=\"bad\\q\"\n",
        "name=\"bad\\\ncontinued\"\n",
        "name=\"\"\"bad\\  text\"\"\"\n",
        "\"\"\"key\"\"\"=1\n",
        "'''key'''=1\n",
        "name=\"\\u1",
        "name=\"\\U00110000\"\n",
        "name=\"\\uD800\"\n",
        "version=1+2\n", "version=01\n", "version=1_\n",
        "a=0x_FF\n", "a=+0x1\n", "a=1.e2\n", "a=1e_2\n",
        "a=9223372036854775808\n", "a=-9223372036854775809\n", "a=0x8000000000000000\n",
        "a=2023-02-29\n", "a=2024-01-01T24:00:00Z\n", "a=2024-01-01T00:00:00+00:60\n",
        "a=2024-01-01T00:00:00.\n", "a=07:32:00Z\n"
    };
    for (size_t i = 0; i < XR_COUNTOF(invalid); ++i) {
        size_t count = run(invalid[i], 0, false);
        for (size_t failure = 1; failure <= count; ++failure)
            run(invalid[i], failure, false);
        total += count;
    }
    printf("TOML allocation checks: %zu failures injected, zero live allocations/bytes\n", total);
    return 0;
}
