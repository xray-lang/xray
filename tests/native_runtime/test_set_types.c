/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_set_types.c - Cold Set type owners and Root allocation failure atomicity
 *
 * KEY CONCEPT:
 *   The compiler allocator observes every arena and resource allocation made
 *   by the runtime factory, independently of the value-domain allocator.
 */
#include "runtime/native/xnative_set.h"
#include "runtime/native/xnative_execution.h"
#include "xir/xxir_types.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../unit/xir/xir_source_compile_owner.h"
static void physical_zero(void) {
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void set_type_facts(const XrXirTypeArena *arena) {
    const XrXirTypes *types = xr_xir_compile_type_arena_types(arena);
    CHECK(types && types->count == 1);
    const XrXirTypeNode *node = xr_xir_type_node(types, XR_XIR_CONSTRUCTED_TYPE_BASE);
    CHECK(node && node->kind == XR_XIR_TYPE_SET && node->element == XR_XIR_I64 && node->result == XR_XIR_UNIT);
}
static void cold_factory(void) {
    size_t sites = 0;
    for (size_t p = 0; p <= sites; ++p) {
        source_fixture_compile_attempts = 0;
        source_fixture_compile_fail_at = p ? p - 1 : SIZE_MAX;
        source_fixture_compile_injected = false;
        XrXirTypeArena *arena = NULL;
        uint64_t bytes = UINT64_C(123456789);
        XrXirValueStatus status = xrt_set_types_new(UINT64_MAX, &arena, &bytes);
        if (!p) {
            CHECK(status == XR_XIR_VALUE_OK && arena && bytes && bytes != UINT64_C(123456789));
            set_type_facts(arena);
            sites = source_fixture_compile_attempts;
            CHECK(sites);
            XrXirTypeArena *occupied = arena;
            uint64_t unchanged = UINT64_C(123456789);
            CHECK(xrt_set_types_new(UINT64_MAX, &occupied, &unchanged) == XR_XIR_VALUE_BAD_ARGUMENT);
            CHECK(occupied == arena && unchanged == UINT64_C(123456789) && source_fixture_compile_attempts == sites);
        } else {
            CHECK(status == XR_XIR_VALUE_OOM && source_fixture_compile_injected && !arena);
            CHECK(bytes == UINT64_C(123456789));
        }
        xr_xir_compile_type_arena_drop(arena);
        physical_zero();
    }
    source_fixture_compile_fail_at = SIZE_MAX;
    printf("Set cold type factory FI: %zu physical=0/0\n", sites);
}
static void set_root(void *opaque) {
    bool *completed = opaque;
    const int64_t elements[] = {1, 7, 1};
    XrtSet *set = xrt_set_i64_new(elements, 3, 1);
    CHECK(xrt_set_length(set, 1) == 2 && xrt_set_i64_contains(set, 7, 1));
    xrt_set_drop(&set);
    *completed = true;
}
static void cold_root(void) {
    XrtSourceLocation source = {"set.xr", 6, 1, 1, 1, 2};
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(&source, 1, &owner) == XRT_LOCATION_OK);
    size_t sites = 0;
    for (size_t p = 0; p <= sites; ++p) {
        source_fixture_compile_attempts = 0;
        source_fixture_compile_fail_at = p ? p - 1 : SIZE_MAX;
        source_fixture_compile_injected = false;
        XrtExecutionFailure failure = {0};
        bool completed = false;
        XrtExecutionStatus status = xrt_run_embedded(owner, &failure, set_root, &completed);
        if (!p) {
            CHECK(status == XRT_EXECUTION_COMPLETED && completed);
            sites = source_fixture_compile_attempts;
            CHECK(sites);
        } else {
            CHECK(status == XRT_EXECUTION_FAILURE && !completed && source_fixture_compile_injected);
            CHECK(failure.kind == XRT_FAILURE_VALUE && failure.detail.value.status == XR_XIR_VALUE_OOM);
        }
        xrt_execution_failure_drop(&failure);
        physical_zero();
    }
    source_fixture_compile_fail_at = SIZE_MAX;
    xrt_locations_release(owner);
    printf("Set cold Root type owner FI: %zu physical=0/0\n", sites);
}
static void finite_type_budget(void) {
    uint64_t low = 0, high = UINT64_C(1048576);
    while (low < high) {
        uint64_t limit = low + (high - low) / 2, bytes = UINT64_C(123456789);
        XrXirTypeArena *arena = NULL;
        XrXirValueStatus status = xrt_set_types_new(limit, &arena, &bytes);
        if (status == XR_XIR_VALUE_OK) {
            CHECK(arena && bytes <= limit);
            xr_xir_compile_type_arena_drop(arena);
            high = limit;
        } else {
            CHECK(status == XR_XIR_VALUE_MEMORY_LIMIT && !arena && bytes == UINT64_C(123456789));
            low = limit + 1;
        }
        physical_zero();
    }
    CHECK(low);
    printf("Set cold type exact finite budget: %llu physical=0/0\n", (unsigned long long)low);
}
int main(void) {
    cold_factory();
    cold_root();
    finite_type_budget();
    physical_zero();
    return 0;
}
