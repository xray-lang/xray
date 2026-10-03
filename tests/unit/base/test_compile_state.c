/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_compile_state.c - Sticky failure and producer-independent owner tests
 */
#include "base/xcompile_state.h"
#include "base/xarena.h"
#include "base/xsource_cache.h"
#include "frontend/xdiag_fmt.h"
#include "toolchain/xcompiler_arena_backing.h"
#include "frontend/parser/xstring_pool.h"
#include "frontend/parser/xtype_scope.h"
#include "frontend/parser/xtype_ref.h"
#include "shared/xr_decimal_float.h"
#include "runtime/value/xtype_pool.h"
#include "frontend/lexer/xquoted_literal.h"
#include "base/xmalloc.h"
#include "base/xchecks.h"
#include "base/xutf8.h"
#include <windows.h>
#include <stdio.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_COMPILE_RESOURCE_OK)

typedef struct Record { void *memory; size_t bytes; } Record;
static Record records[4096];
static size_t live, total, peak, blocks, calls, fail_at = SIZE_MAX;

static void *observed_alloc(size_t bytes) {
    if (calls++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory != NULL);
    size_t i = 0;
    while (i < 4096 && records[i].memory) ++i;
    CHECK(i < 4096);
    records[i] = (Record) {memory, bytes};
    ++blocks;
    live += bytes;
    total += bytes;
    if (live > peak) peak = live;
    return memory;
}

static void observed_free(void *memory) {
    if (!memory) return;
    size_t i = 0;
    while (i < 4096 && records[i].memory != memory) ++i;
    CHECK(i < 4096);
    live -= records[i].bytes;
    --blocks;
    records[i] = (Record) {0};
    xr_free(memory);
}

#undef xr_malloc
#undef xr_free
#define xr_malloc(size) observed_alloc(size)
#define xr_free(memory) observed_free(memory)
#include "base/xcompile_resources.c"

static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};

static void reset(void) {
    CHECK(!blocks && !live);
    total = peak = calls = 0;
    fail_at = SIZE_MAX;
}

static XrCompileResourceStats stats(XrCompileResources *resources) {
    XrCompileResourceStats value;
    OK(xr_compile_resources_stats(resources, &value));
    CHECK(value.live_bytes == live && value.allocated_bytes == total && value.peak_bytes == peak);
    return value;
}

static void lifetime_and_work(void) {
    reset();
    XrCompileResources *resources = NULL;
    XrCompileState *state = NULL;
    OK(xr_compile_resources_new(&unlimited, &resources));
    OK(xr_compile_state_new(resources, &state));
    CHECK(stats(resources).work == 2);
    char *text = NULL;
    OK(xr_compile_state_strdup(state, "abc", &text));
    CHECK(!strcmp(text, "abc") && stats(resources).work == 11);
    void *zero = NULL;
    OK(xr_compile_state_calloc(state, 2, 2, &zero));
    CHECK(((char *) zero)[0] == 0 && stats(resources).work == 16);
    void *resized = text;
    OK(xr_compile_state_resize(state, &resized, 8));
    CHECK(!strcmp(resized, "abc") && stats(resources).work == 21);
    OK(xr_compile_state_retain(state));
    xr_compile_resources_release(resources);
    xr_compile_state_release(state);
    CHECK(xr_compile_state_resources(state) == resources);
    OK(xr_compile_state_work(state, 2));
    xr_compile_state_release(state);
    CHECK(blocks == 3);
    xr_compile_state_free(zero);
    xr_compile_state_free(resized);
    CHECK(!blocks && !live);
}

static void sticky_and_empty_output(void) {
    reset();
    XrCompileResources *resources = NULL;
    OK(xr_compile_resources_new(&unlimited, &resources));
    XrCompileState *sentinel = (XrCompileState *) (uintptr_t) 1;
    CHECK(xr_compile_state_new(resources, &sentinel) == XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    CHECK(sentinel == (XrCompileState *) (uintptr_t) 1 && calls == 1);
    XrCompileState *state = NULL;
    fail_at = calls;
    CHECK(xr_compile_state_new(resources, &state) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !state);
    CHECK(stats(resources).work == 2);
    fail_at = SIZE_MAX;
    OK(xr_compile_state_new(resources, &state));
    CHECK(xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET) == XR_COMPILE_RESOURCE_BUDGET);
    size_t before = calls, length = 17;
    char *text = NULL;
    CHECK(xr_compile_state_strdup(state, (const char *) (uintptr_t) 1, &text) == XR_COMPILE_RESOURCE_BUDGET);
    CHECK(xr_compile_state_string_length(state, (const char *) (uintptr_t) 1, &length) == XR_COMPILE_RESOURCE_BUDGET);
    CHECK(!text && length == 17 && calls == before);
    CHECK(xr_compile_state_fail(state, XR_COMPILE_RESOURCE_OUT_OF_MEMORY) == XR_COMPILE_RESOURCE_BUDGET);
    XrArena arena = {0};
    XrArenaBacking backing;
    CHECK(xr_compiler_arena_state_backing(state, &backing) == XR_ARENA_OK);
    CHECK(xr_arena_open(&arena, 64, &backing) == XR_ARENA_BUDGET);
    xr_compile_resources_release(resources);
    xr_compile_state_release(state);
    CHECK(blocks == 2);
    xr_arena_destroy(&arena);
    CHECK(!blocks);
}

static XrCompileResourceStatus pool_scope_pipeline(const XrCompileResourceLimits *limits, XrCompileResourceStats *result) {
    XrCompileResources *resources = NULL;
    XrCompileState *state = NULL;
    XrArena arena = {0};
    XrCompileStringPool *pool = NULL;
    XrTypeScope *scope = NULL;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = xr_compile_state_new(resources, &state);
    if (status != XR_COMPILE_RESOURCE_OK) goto done;
    XrArenaBacking backing;
    CHECK(xr_compiler_arena_state_backing(state, &backing) == XR_ARENA_OK);
    (void) xr_arena_open(&arena, 128, &backing);
    status = xr_compiler_arena_capture_status(&arena, state);
    if (status != XR_COMPILE_RESOURCE_OK) goto done;
    status = xr_compile_string_pool_open(state, &arena, &pool);
    if (status != XR_COMPILE_RESOURCE_OK) goto done;
    uint64_t start_work = stats(resources).work;
    const char *a = xr_string_pool_intern(pool, "key");
    const char *b = xr_string_pool_intern(pool, "key");
    if (xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK)
        /* Two length scans, six hash reads, two probes, four copied bytes,
         * one published entry, and six reads for the duplicate comparison. */
        CHECK(a == b && a && stats(resources).work == start_work + 26 +
              sizeof(struct { uint32_t hash; const char *text; size_t length; }) &&
              xr_string_pool_count(pool) == 1);
    for (size_t i = 0; i < 90 && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "entry-%zu", i);
        (void) xr_string_pool_intern(pool, name);
    }
    status = xr_compile_type_scope_open(state, NULL, &scope);
    if (status != XR_COMPILE_RESOURCE_OK) goto done;
    XrTypeAlias *alias = NULL;
    XrTypeScopeStatus defined = xr_compile_type_scope_define(scope, "Thing", NULL, &alias);
    if (defined == XR_TYPE_SCOPE_OK) {
        CHECK(alias && xr_type_scope_lookup(scope, "Thing") == alias);
        XrTypeAlias *duplicate = NULL;
        CHECK(xr_compile_type_scope_define(scope, "Thing", NULL, &duplicate) == XR_TYPE_SCOPE_DUPLICATE);
        CHECK(!duplicate && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK);
    }
    status = xr_compile_state_status(state);
done:
    *result = stats(resources);
    xr_compile_resources_release(resources);
    xr_compile_state_release(state);
    if (status == XR_COMPILE_RESOURCE_OK) {
        CHECK(xr_type_scope_lookup(scope, "Thing") != NULL);
        CHECK(xr_string_pool_intern(pool, "key") != NULL);
    }
    xr_type_scope_free(scope);
    xr_arena_destroy(&arena);
    CHECK(!blocks && !live);
    return status;
}

static void pool_scope_faults(void) {
    reset();
    XrCompileResourceStats measured = {0}, result = {0};
    OK(pool_scope_pipeline(&unlimited, &measured));
    size_t allocation_points = calls;
    for (size_t point = 0; point < allocation_points; ++point) {
        reset();
        fail_at = point;
        CHECK(pool_scope_pipeline(&unlimited, &result) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        CHECK(calls == point + 1);
    }
    /* The snapshot precedes two lifetime probes, so allow their fixed work
     * after the admission limit has been established by independent tracing. */
    reset();
    XrCompileResourceLimits limits = {measured.allocated_bytes, measured.peak_bytes, UINT64_MAX};
    OK(pool_scope_pipeline(&limits, &result));
    for (unsigned dimension = 0; dimension < 2; ++dimension) {
        reset();
        XrCompileResourceLimits smaller = limits;
        if (dimension == 0) --smaller.allocated_bytes;
        else --smaller.live_bytes;
        CHECK(pool_scope_pipeline(&smaller, &result) == XR_COMPILE_RESOURCE_BUDGET);
    }
}


static void quoted_work_and_faults(void) {
    /* Ledger + state = 2; two delimiter reads + allocation + 4 byte copy +
     * allocation + 4 byte escape reads + 3 output writes + terminator = 18. */
    const Token token = {.start = "\"a\\nb\"", .length = 6, .quote_count = 1,
                         .quoted_kind = XR_QUOTED_STRING, .source_form = XR_LITERAL_INLINE};
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        reset();
        XrCompileResources *resources = NULL;
        XrCompileState *state = NULL;
        XrCompileResourceLimits limits = unlimited;
        if (attempt == 1) limits.work = 18;
        if (attempt == 2) limits.work = 17;
        OK(xr_compile_resources_new(&limits, &resources));
        OK(xr_compile_state_new(resources, &state));
        if (attempt >= 3) fail_at = calls + attempt - 3;
        XrQuotedPayload payload = {0};
        const char *error = "unchanged";
        XrQuotedStatus status = xr_compile_quoted_payload_decode(state, &token, true, &payload, &error);
        if (attempt < 2) {
            CHECK(status == XR_QUOTED_OK && payload.length == 3 && !memcmp(payload.bytes, "a\nb", 3));
            CHECK(stats(resources).work == 18);
        } else {
            CHECK(status == (attempt == 2 ? XR_QUOTED_BUDGET : XR_QUOTED_OUT_OF_MEMORY));
            CHECK(!payload.bytes && !payload.length);
            CHECK(stats(resources).work == (attempt == 2 ? 17 : attempt == 3 ? 5 : 10));
        }
        CHECK(!strcmp(error, "unchanged"));
        xr_compile_state_release(state);
        xr_compile_resources_release(resources);
        if (attempt < 2) CHECK(!memcmp(payload.bytes, "a\nb", 3));
        xr_quoted_payload_free(&payload);
        CHECK(!blocks && !live);
    }
    reset();
    XrCompileResources *resources = NULL;
    XrCompileState *state = NULL;
    OK(xr_compile_resources_new(&unlimited, &resources));
    OK(xr_compile_state_new(resources, &state));
    const uint8_t malformed[] = "\\u{Z}";
    uint8_t output[16] = {0};
    size_t length = 99;
    const char *error = NULL;
    CHECK(xr_compile_escaped_bytes_decode(state, malformed, sizeof(malformed) - 1, output, &length, &error) == XR_QUOTED_SYNTAX);
    CHECK(length == 99 && error && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK);
    XrQuotedPayload sentinel = {(uint8_t *) (uintptr_t) 1, 7};
    CHECK(xr_compile_quoted_payload_decode(state, &token, true, &sentinel, &error) == XR_QUOTED_BAD_ARGUMENT);
    CHECK(sentinel.bytes == (uint8_t *) (uintptr_t) 1 && sentinel.length == 7);
    xr_compile_state_release(state);
    xr_compile_resources_release(resources);
    CHECK(!blocks && !live);
}


static int metered_utf8(void *context, const uint8_t *address, uint8_t *output) {
    if (xr_compile_state_work(context, 1) != XR_COMPILE_RESOURCE_OK) return 0;
    *output = *address;
    return 1;
}

static void utf8_and_scanner_lifetime(void) {
    reset();
    XrCompileResources *resources = NULL;
    XrCompileState *state = NULL;
    OK(xr_compile_resources_new(&unlimited, &resources));
    OK(xr_compile_state_new(resources, &state));
    const uint8_t malformed[] = {0xE0, 0x80, 0xBF, 'x'};
    XrUtf8ScanResult scan = {0};
    CHECK(xr_utf8_scan_strict_read(malformed, sizeof(malformed), metered_utf8, state, &scan));
    CHECK(scan.error == XR_UTF8_OVERLONG && scan.invalid_length == 3);
    /* Decoder reads lead + second, diagnostic reads lead + two continuations. */
    CHECK(stats(resources).work == 7);
    XrArena arena = {0};
    XrArenaBacking backing;
    CHECK(xr_compiler_arena_state_backing(state, &backing) == XR_ARENA_OK);
    CHECK(xr_arena_open(&arena, 64, &backing) == XR_ARENA_OK);
    Scanner scanner = {0};
    char borrowed_source[] = "// hello\nvar name = 1";
    OK(xr_compile_scanner_open_with_trivia(&scanner, state, &arena, borrowed_source, true));
    xr_compile_state_release(state);
    xr_compile_resources_release(resources);
    Token token = xr_scanner_scan(&scanner);
    CHECK(token.type == TK_VAR && token.leading_trivia && token.leading_trivia->length == 6);
    CHECK(!memcmp(token.leading_trivia->start, " hello", 6));
    Scanner rollback = scanner;
    CHECK(xr_scanner_scan(&scanner).type == TK_NAME);
    CHECK(xr_scanner_scan(&rollback).type == TK_NAME);
    CHECK(xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET) == XR_COMPILE_RESOURCE_BUDGET);
    CHECK(xr_scanner_scan(&rollback).type == TK_EOF);
    memset(borrowed_source, 'x', sizeof(borrowed_source));
    CHECK(!memcmp(token.leading_trivia->start, " hello", 6));
    xr_arena_destroy(&arena);
    CHECK(!blocks && !live);

    SYSTEM_INFO system;
    GetSystemInfo(&system);
    size_t page = system.dwPageSize;
    uint8_t *memory = VirtualAlloc(NULL, 2 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(memory);
    DWORD previous;
    CHECK(VirtualProtect(memory + page, page, PAGE_NOACCESS, &previous));
    memory[page - 1] = 0x80;
    reset();
    resources = NULL; state = NULL;
    XrCompileResourceLimits limits = unlimited;
    limits.work = 3;
    OK(xr_compile_resources_new(&limits, &resources));
    OK(xr_compile_state_new(resources, &state));
    CHECK(xr_utf8_scan_strict_read(memory + page - 1, 2, metered_utf8, state, &scan));
    CHECK(scan.error == XR_UTF8_STRAY_CONTINUATION && stats(resources).work == 3);
    XrUtf8ScanResult sentinel = {XR_UTF8_OUT_OF_RANGE, 42, 43, 44};
    scan = sentinel;
    CHECK(!xr_utf8_scan_strict_read(memory + page, 1, metered_utf8, state, &scan));
    CHECK(!memcmp(&scan, &sentinel, sizeof(scan)) && stats(resources).work == 3);
    xr_compile_state_release(state);
    xr_compile_resources_release(resources);
    CHECK(VirtualFree(memory, 0, MEM_RELEASE));
    CHECK(!blocks && !live);
}

static bool decimal_charge(void *context, uint64_t units) {
    return xr_compile_state_work(context, units) == XR_COMPILE_RESOURCE_OK;
}

static void type_format_and_decimal_work(void) {
    for (uint64_t limit = 2; limit <= 11; ++limit) {
        reset();
        XrCompileResources *resources = NULL;
        XrCompileState *state = NULL;
        XrCompileResourceLimits limits = unlimited;
        limits.work = limit;
        OK(xr_compile_resources_new(&limits, &resources));
        OK(xr_compile_state_new(resources, &state));
        XrTypeRef type = {0};
        type.kind = XR_TREF_NAMED;
        type.name = "Box";
        char buf[16] = "sentinel";
        int length = xr_compile_tref_to_string_buf(state, &type, buf, sizeof(buf));
        /* Owner/state allocations 2; node visit 1; reads 4; writes 3; NUL 1. */
        CHECK(stats(resources).work == limit);
        if (limit == 11) CHECK(length == 3 && !strcmp(buf, "Box"));
        else {
            CHECK(length == -1 && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_BUDGET);
            CHECK(xr_compile_tref_to_string_buf(state, (const XrTypeRef *) (uintptr_t) 1, buf, sizeof(buf)) == -1);
            CHECK(stats(resources).work == limit);
        }
        xr_compile_state_release(state);
        xr_compile_resources_release(resources);
        CHECK(!blocks && !live);
    }
    reset();
    XrCompileResources *resources = NULL;
    XrCompileState *state = NULL;
    OK(xr_compile_resources_new(&unlimited, &resources));
    OK(xr_compile_state_new(resources, &state));
    char single = 'x';
    CHECK(xr_compile_tref_to_string_buf(state, NULL, &single, 1) == 0 && single == 0);
    CHECK(stats(resources).work == 3); /* no visit/read when capacity is exhausted */
    XrDecimalWork decimal = {state, decimal_charge, false};
    uint64_t bits = UINT64_MAX;
    CHECK(xr_decimal_float_parse_work(&decimal, "0", 1, 64, &bits) == XR_DECIMAL_OK && bits == 0);
    CHECK(stats(resources).work == 801); /* independent decimal zero formula: 792 clear + 6 operations */
    xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BUDGET);
    bits = 42;
    CHECK(xr_decimal_float_parse_work(&decimal, (const char *) (uintptr_t) 1, 1, 64, &bits) == XR_DECIMAL_WORK_LIMIT);
    CHECK(bits == 42 && decimal.failed && stats(resources).work == 801);
    xr_compile_state_release(state);
    xr_compile_resources_release(resources);
    CHECK(!blocks && !live);
}

static void type_pool_owner(void) {
    for (size_t failure = 0; failure < 5; ++failure) {
        reset();
        XrCompileResources *resources = NULL;
        XrCompileState *state = NULL;
        XrTypePool *pool = NULL;
        OK(xr_compile_resources_new(&unlimited, &resources));
        OK(xr_compile_state_new(resources, &state));
        if (failure < 2) fail_at = calls + failure;
        XrCompileResourceStatus opened = xr_compile_type_pool_open(state, &pool);
        if (failure < 2) {
            CHECK(opened == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !pool);
        } else {
            OK(opened);
            CHECK(stats(resources).work == 4 + sizeof(XrTypePool));
            XrType *type = xr_pool_alloc_type(pool, XR_KIND_INT);
            CHECK(type && type->id == 1 && type->kind == XR_KIND_INT);
            CHECK(stats(resources).work == 4 + sizeof(XrTypePool) + sizeof(XrType));
            xr_compile_state_release(state);
            xr_compile_resources_release(resources);
            state = pool->state;
            CHECK(xr_pool_strdup(pool, "owned"));
            if (failure == 2) {
                fail_at = calls;
                CHECK(!xr_pool_alloc(pool, 2 * XR_ARENA_SEGMENT_SIZE));
                CHECK(xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
            } else if (failure == 3) {
                CHECK(!xr_pool_alloc_array(pool, SIZE_MAX, 2));
                CHECK(xr_compile_state_status(state) == XR_COMPILE_RESOURCE_BUDGET);
            } else {
                pool->next_type_id = UINT32_MAX;
                CHECK(!xr_pool_alloc_type(pool, XR_KIND_INT));
                CHECK(xr_compile_state_status(state) == XR_COMPILE_RESOURCE_BUDGET);
            }
            size_t before = calls;
            XrCompileResourceStatus failed = xr_compile_state_status(state);
            xr_type_pool_reset(pool);
            CHECK(!xr_pool_strdup(pool, (const char *) (uintptr_t) 1));
            CHECK(calls == before && xr_compile_state_status(state) == failed);
            xr_type_pool_free(pool);
            CHECK(!blocks && !live);
            continue;
        }
        xr_compile_state_release(state);
        xr_compile_resources_release(resources);
        CHECK(!blocks && !live);
    }
}

static XrCompileResourceStatus source_cache_pipeline(const XrCompileResourceLimits *limits, XrCompileResourceStats *observed) {
    XrCompileResources *resources = NULL;
    XrCompileState *state = NULL;
    XrSourceCache *cache = NULL;
    XrCompileResourceStatus status = xr_compile_resources_new(limits, &resources);
    if (status != XR_COMPILE_RESOURCE_OK) return status;
    status = xr_compile_state_new(resources, &state);
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_source_cache_open(state, &cache);
    if (status == XR_COMPILE_RESOURCE_OK) status = xr_compile_source_cache_add(cache, "x", "a\nb");
    if (status == XR_COMPILE_RESOURCE_OK) {
        /* 2 owners; cache alloc+zero; path clone5; content clone9; line count4;
         * line alloc1 + scan4 + two pointer writes; table alloc1; publication. */
        CHECK(stats(resources).work == 27 + sizeof(XrSourceCache) +
                                           2 * sizeof(char *) + sizeof(XrSourceFile));
        CHECK(cache->file_count == 1 && cache->files[0].line_count == 2);
        CHECK(xr_runtime_source_cache_get_line_length(cache, "x", 1) == 1);
        CHECK(!strcmp(xr_runtime_source_cache_get_line(cache, "x", 2), "b"));
    } else if (cache) {
        CHECK(cache->file_count == 0);
    }
    *observed = stats(resources);
    xr_compile_state_release(state);
    xr_compile_resources_release(resources);
    if (cache && status == XR_COMPILE_RESOURCE_OK) {
        xr_compile_state_fail(cache->state, XR_COMPILE_RESOURCE_BUDGET);
        CHECK(!strcmp(xr_runtime_source_cache_get_line(cache, "x", 2), "b"));
        CHECK(xr_runtime_source_cache_get_line_length(cache, "x", 1) == 1);
        CHECK(stats(xr_compile_state_resources(cache->state)).work == observed->work);
    }
    xr_owned_source_cache_close(cache);
    CHECK(!blocks && !live);
    return status;
}

static void source_cache_owner(void) {
    reset();
    XrCompileResourceStats exact, observed;
    OK(source_cache_pipeline(&unlimited, &exact));
    size_t points = calls;
    for (size_t i = 0; i < points; ++i) {
        reset(); fail_at = i;
        CHECK(source_cache_pipeline(&unlimited, &observed) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
        CHECK(calls == i + 1);
    }
    XrCompileResourceLimits limits = {exact.allocated_bytes, exact.peak_bytes, exact.work};
    reset(); OK(source_cache_pipeline(&limits, &observed));
    for (unsigned i = 0; i < 3; ++i) {
        reset(); XrCompileResourceLimits smaller = limits;
        if (!i) --smaller.allocated_bytes;
        else if (i == 1) --smaller.live_bytes;
        else --smaller.work;
        CHECK(source_cache_pipeline(&smaller, &observed) == XR_COMPILE_RESOURCE_BUDGET);
    }
}

static void diagnostic_policy_work(void) {
    for (uint64_t limit = 2; limit <= 19; ++limit) {
        reset();
        XrCompileResources *resources = NULL;
        XrCompileState *state = NULL;
        XrCompileResourceLimits limits = unlimited;
        limits.work = limit;
        OK(xr_compile_resources_new(&limits, &resources));
        OK(xr_compile_state_new(resources, &state));
        char text[32] = "sentinel";
        XrDiagBuffer buffer = {text, sizeof(text), 0, state, decimal_charge};
        XrDiagPolicy policy = {&buffer, xr_diag_direct_read, xr_diag_buffer_write,
                               xr_diag_buffer_work, XR_DIAG_OK, false};
        (void) xr_diag_format(&policy, "%s:%u", "A", 12u);
        XrDiagStatus status = xr_diag_buffer_finish(&policy, &buffer);
        /* 2 owners + 6 format reads + 2 string reads + 4 writes +
         * 2 numeric scratch writes + 2 digit reads + terminator = 19. */
        CHECK(stats(resources).work == limit);
        if (limit == 19) CHECK(status == XR_DIAG_OK && !strcmp(text, "A:12"));
        else {
            CHECK(status == XR_DIAG_RESOURCE && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_BUDGET);
            CHECK(xr_diag_format(&policy, (const char *) (uintptr_t) 1) == XR_DIAG_RESOURCE);
            CHECK(stats(resources).work == limit);
        }
        xr_compile_state_release(state); xr_compile_resources_release(resources);
        CHECK(!blocks && !live);
    }
    reset();
    XrCompileResources *resources = NULL;
    XrCompileState *state = NULL;
    OK(xr_compile_resources_new(&unlimited, &resources));
    OK(xr_compile_state_new(resources, &state));
    char text[128];
    XrDiagBuffer buffer = {text, sizeof(text), 0, state, decimal_charge};
    XrDiagPolicy policy = {&buffer, xr_diag_direct_read, xr_diag_buffer_write,
                           xr_diag_buffer_work, XR_DIAG_OK, false};
    CHECK(xr_diag_format(&policy, "%04d|%*s|%.*s|%llu|%zu", -7, 3, "", 2, "abc",
                         (unsigned long long) UINT64_MAX, (size_t) 42) == XR_DIAG_OK);
    CHECK(xr_diag_buffer_finish(&policy, &buffer) == XR_DIAG_OK);
    CHECK(!strcmp(text, "-007|   |ab|18446744073709551615|42"));
    char display[256];
    buffer = (XrDiagBuffer) {display, sizeof(display), 0, state, decimal_charge};
    policy = (XrDiagPolicy) {&buffer, xr_diag_direct_read, xr_diag_buffer_write,
                            xr_diag_buffer_work, XR_DIAG_OK, false};
    const char source[] = "x = ?";
    CHECK(xr_diag_print_policy(&policy, XR_DIAG_ERROR, 7, "bad", "x.xr", 1, 5, 1, source, source + 4) == XR_DIAG_OK);
    CHECK(xr_diag_buffer_finish(&policy, &buffer) == XR_DIAG_OK);
    CHECK(!strcmp(display, "error[E0007]: bad\n  --> x.xr:1:5\n   |\n 1 | x = ?\n   |     ^\n\n"));
    xr_compile_state_release(state); xr_compile_resources_release(resources);
    CHECK(!blocks && !live);

    reset(); resources = NULL; state = NULL;
    OK(xr_compile_resources_new(&unlimited, &resources));
    OK(xr_compile_state_new(resources, &state));
    FILE *read_only = fopen("NUL", "r");
    CHECK(read_only);
    XrDiagOutput sink = {read_only, state, decimal_charge};
    policy = (XrDiagPolicy) {&sink, xr_diag_direct_read, xr_diag_output_write,
                            xr_diag_output_work, XR_DIAG_OK, false};
    CHECK(xr_diag_format(&policy, "%s%s", "a", (const char *) (uintptr_t) 1) == XR_DIAG_IO);
    CHECK(xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats(resources).work == 6); /* two format reads, one string read, one failed write attempt */
    CHECK(xr_diag_format(&policy, (const char *) (uintptr_t) 1) == XR_DIAG_IO);
    fclose(read_only);
    xr_compile_state_release(state); xr_compile_resources_release(resources);
    CHECK(!blocks && !live);
}

int main(void) {
    diagnostic_policy_work();
    source_cache_owner();
    type_pool_owner();
    type_format_and_decimal_work();
    utf8_and_scanner_lifetime();
    quoted_work_and_faults();
    lifetime_and_work();
    sticky_and_empty_output();
    pool_scope_faults();
    puts("compile state: sticky typed failures, independent work, producer lifetime and physical zero passed");
    return 0;
}
