/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_compile_c_buffer.c - Independent formatting, work and physical bounds
 */
#include "base/xmalloc.h"
#include "xir/xxir_compile_memory.h"
#include <stdarg.h>
#include <stdio.h>
#include <limits.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)

typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[64];
static size_t attempts, fail_at = SIZE_MAX, live, physical, peak, total;
static void *observe_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory);
    size_t i = 0;
    while (i < 64 && allocations[i].pointer) ++i;
    CHECK(i < 64);
    allocations[i] = (Allocation){memory, bytes};
    ++live; physical += bytes; total += bytes;
    if (physical > peak) peak = physical;
    return memory;
}
static void observe_free(void *memory) {
    if (!memory) return;
    size_t i = 0;
    while (i < 64 && allocations[i].pointer != memory) ++i;
    CHECK(i < 64 && live);
    size_t bytes = allocations[i].bytes;
    xr_free(memory);
    allocations[i] = (Allocation){0}; --live; physical -= bytes;
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observe_alloc(bytes)
#define xr_free(memory) observe_free(memory)
#include "base/xcompile_resources.c"
#include "xir/xxir_emit_buffer.inc.c"

static void reset_observer(void) {
    CHECK(!live && !physical);
    attempts = peak = total = 0; fail_at = SIZE_MAX;
}
static XrXirCompileContext context_new(uint64_t work) {
    XrXirCompileContext context = {0};
    const XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, work};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    return context;
}
static XrCompileResourceStats stats(const XrXirCompileContext *context) {
    XrCompileResourceStats value;
    CHECK(xr_compile_resources_stats(context->resources, &value) == XR_COMPILE_RESOURCE_OK);
    CHECK(value.live_bytes == physical && value.peak_bytes == peak && value.allocated_bytes == total);
    return value;
}
static void formatting(void) {
    reset_observer();
    XrXirCompileContext context = context_new(UINT64_MAX);
    CBuffer first = {.limit = 512, .status = XR_XIR_OK, .context = &context};
    append(&first, "[%s] %d %u %llu %02x %%", "hello", INT_MIN, UINT_MAX, ULLONG_MAX, 9u);
    CHECK(emit_finalize(&first));
    CHECK(first.status == XR_XIR_OK);
    const char expected[] = "[hello] -2147483648 4294967295 18446744073709551615 09 %";
    CHECK(first.length == sizeof(expected)-1 && !memcmp(first.text, expected, sizeof(expected)));
    CBuffer second = {.limit = 32, .status = XR_XIR_OK, .context = &context};
    append(&second, "%d|%02x|%u|%s", 0, 255u, 0u, "");
    CHECK(emit_finalize(&second));
    CHECK(second.status == XR_XIR_OK && !strcmp(second.text, "0|ff|0|"));
    XrCompileResourceStats before = stats(&context);
    CHECK(before.allocation_count == 3 && live == 3);
    xr_compile_resources_release(context.resources);
    CHECK(live == 3 && !strcmp(first.text, expected));
    xr_compile_resources_free(second.text);
    CHECK(live == 2 && !strcmp(first.text, expected));
    xr_compile_resources_free(first.text);
    CHECK(!live && !physical);
}
static void exact_work(void) {
    /* Ledger creation 1, format bytes including NUL 5, one allocation 1,
     * four real output writes 4, two digit conversions 2, two stack reads 2
     * and the one final NUL write 1: total 16, independent of struct sizes. */
    for (uint64_t limit = 15; limit <= 16; ++limit) {
        reset_observer();
        XrXirCompileContext context = context_new(limit);
        CBuffer buffer = {.limit = 5, .status = XR_XIR_OK, .context = &context};
        append(&buffer, "ab%u", 12u);
        CHECK(buffer.status == XR_XIR_OK && buffer.length == 4 && !memcmp(buffer.text, "ab12", 4));
        CHECK(stats(&context).work == 15);
        buffer.text[buffer.length] = 'q';
        CHECK(emit_finalize(&buffer) == (limit == 16));
        CHECK(buffer.status == (limit == 16 ? XR_XIR_OK : XR_XIR_BUDGET));
        CHECK(stats(&context).work == limit);
        if (limit == 16) CHECK(!memcmp(buffer.text, "ab12", 5));
        else CHECK(buffer.text[buffer.length] == 'q' && !memcmp(buffer.text, "ab12", 4));
        xr_compile_resources_free(buffer.text);
        xr_compile_resources_release(context.resources);
        CHECK(!live && !physical);
    }
}
static void measured_work(void) {
    /* Sizing emits four counted bytes but no allocation or NUL: creation1
     * plus format5, four length advances, conversion2 and stack reads2=14.
     * Real formatting adds14; the only actual final NUL makes total29. */
    for (uint64_t limit = 28; limit <= 29; ++limit) {
        reset_observer();
        XrXirCompileContext context = context_new(limit);
        CBuffer measured = {.limit = 5, .status = XR_XIR_OK, .context = &context, .measuring = true};
        append(&measured, "ab%u", 12u);
        CHECK(measured.status == XR_XIR_OK && !measured.text && measured.length == 4 && !measured.capacity);
        CHECK(stats(&context).work == 14 && stats(&context).allocation_count == 1);
        CBuffer buffer = {.limit = 5, .status = XR_XIR_OK, .context = &context};
        append(&buffer, "ab%u", 12u);
        CHECK(buffer.status == XR_XIR_OK && buffer.length == measured.length && !memcmp(buffer.text, "ab12", 4));
        CHECK(stats(&context).work == 28);
        buffer.text[buffer.length] = 'q';
        CHECK(emit_finalize(&buffer) == (limit == 29));
        CHECK(stats(&context).work == limit);
        CHECK(buffer.text[buffer.length] == (limit == 29 ? '\0' : 'q'));
        xr_compile_resources_free(buffer.text); xr_compile_resources_release(context.resources);
        CHECK(!live && !physical);
    }
}
static void charged_before_reads(void) {
    for (unsigned i = 0; i < 2; ++i) {
        reset_observer();
        XrXirCompileContext context = context_new(i ? 3 : 1);
        CBuffer buffer = {.limit = 128, .status = XR_XIR_OK, .context = &context};
        const char *unreadable = (const char *)(uintptr_t)1;
        if (i) append(&buffer, "%s", unreadable); else append(&buffer, unreadable);
        CHECK(buffer.status == XR_XIR_BUDGET && !buffer.text && !buffer.length);
        uint64_t work = stats(&context).work;
        append(&buffer, unreadable);
        CHECK(stats(&context).work == work);
        xr_compile_resources_release(context.resources);
        CHECK(!live && !physical);
    }
}
static void growth_failures(void) {
    char input[350]; memset(input, 'q', sizeof(input)-1); input[sizeof(input)-1] = 0;
    size_t allocation_count = 0;
    for (size_t failure = SIZE_MAX;;) {
        reset_observer();
        XrXirCompileContext context = context_new(UINT64_MAX);
        fail_at = failure;
        CBuffer buffer = {.limit = 1024, .status = XR_XIR_OK, .context = &context};
        append(&buffer, "%s", input);
        (void) emit_finalize(&buffer);
        if (failure == SIZE_MAX) {
            CHECK(buffer.status == XR_XIR_OK && buffer.length == 349 && !strcmp(buffer.text, input));
            allocation_count = attempts;
            CHECK(allocation_count == 4);
        } else CHECK(buffer.status == XR_XIR_OUT_OF_MEMORY);
        XrCompileResourceStats before = stats(&context);
        if (failure != SIZE_MAX) {
            char *saved = buffer.text;
            append(&buffer, "%s", (const char *)(uintptr_t)1);
            CHECK(buffer.text == saved && stats(&context).work == before.work);
        }
        xr_compile_resources_free(buffer.text);
        xr_compile_resources_release(context.resources);
        CHECK(!live && !physical);
        if (failure == SIZE_MAX) failure = 1;
        else if (++failure == allocation_count) break;
    }
}
int main(void) {
    formatting(); exact_work(); measured_work(); charged_before_reads(); growth_failures();
    puts("C formatting: fixed outputs, exact final-write work 16/15 and sizing+write 29/28, pre-read limits, three allocation OOMs, physical zero");
    return 0;
}
