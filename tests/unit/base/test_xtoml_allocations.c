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
    XrTomlValue *value = xtoml_parse(source, strlen(source));
    size_t total = attempts;
    if ((failure || !valid) && value) {
        fprintf(stderr, "accepted document at allocation %zu of %zu\n", failure, total);
        exit(1);
    }
    if (!failure && valid) REQUIRE(value);
    xtoml_free(value);
    REQUIRE(live_count == 0 && live_bytes == 0);
    return total;
}
int main(void) {
    static const char source[] =
        "title=\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\"\n"
        "a.b.c.d.e.f=1\na.b.c.d.e.g=[1,2,3,4,5,6,7,8,9]\n"
        "empty=\"\"\nempty_literal=''\n\"quoted\"='literal'\n"
        "map={a=1,b=2,c=3,d=4,e=5,f=6,g=7,h=8,i=9}\n"
        "[settings.parent]\nx=1\n"
        "[[items]]\nx=1\n[[items]]\nx=2\n[[items]]\nx=3\n"
        "[[items]]\nx=4\n[[items]]\nx=5\n[[items]]\nx=6\n"
        "[[items]]\nx=7\n[[items]]\nx=8\n[[items]]\nx=9\n";
    size_t total = run(source, 0, true);
    for (size_t failure = 1; failure <= total; ++failure)
        run(source, failure, true);
    static const char *const invalid[] = {"x={a=1,a=2}\n", "x=1\nx.a=2\n"};
    for (size_t i = 0; i < XR_COUNTOF(invalid); ++i) {
        size_t count = run(invalid[i], 0, false);
        for (size_t failure = 1; failure <= count; ++failure)
            run(invalid[i], failure, false);
        total += count;
    }
    printf("TOML allocation checks: %zu failures injected, zero live allocations/bytes\n", total);
    return 0;
}
