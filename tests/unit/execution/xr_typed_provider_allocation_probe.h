/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_typed_provider_allocation_probe.h - Injected executor allocation accounting
 */
#include "../program/xr_program_allocation_probe.h"

static size_t typed_attempt, typed_fail_at, typed_live_count;
static bool typed_armed, typed_failed;
static void *typed_live[1024];

static bool typed_reject_allocation(void) {
    if (!typed_armed || ++typed_attempt != typed_fail_at) return false;
    typed_failed = true;
    return true;
}

static void *typed_record_allocation(void *pointer) {
    REQUIRE(pointer != NULL);
    for (size_t i = 0u; i < XR_COUNTOF(typed_live); ++i) {
        if (!typed_live[i]) {
            typed_live[i] = pointer;
            ++typed_live_count;
            return pointer;
        }
    }
    abort();
}

void *xr_program_test_malloc(size_t size) {
    return typed_reject_allocation() ? NULL : typed_record_allocation(xr_program_test_system_malloc(size));
}
void *xr_program_test_calloc(size_t count, size_t size) {
    return typed_reject_allocation() ? NULL : typed_record_allocation(xr_program_test_system_calloc(count, size));
}
void *xr_program_test_realloc(void *pointer, size_t size) {
    if (typed_reject_allocation()) return NULL;
    if (!pointer) return typed_record_allocation(xr_program_test_system_realloc(NULL, size));
    for (size_t i = 0u; i < XR_COUNTOF(typed_live); ++i) {
        if (typed_live[i] == pointer) {
            void *next = xr_program_test_system_realloc(pointer, size);
            REQUIRE(next != NULL);
            typed_live[i] = next;
            return next;
        }
    }
    abort();
}
void xr_program_test_free(void *pointer) {
    if (!pointer) return;
    for (size_t i = 0u; i < XR_COUNTOF(typed_live); ++i) {
        if (typed_live[i] == pointer) {
            typed_live[i] = NULL;
            --typed_live_count;
            xr_program_test_system_free(pointer);
            return;
        }
    }
    abort();
}

