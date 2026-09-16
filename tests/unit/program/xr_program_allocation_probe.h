/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_allocation_probe.h - Allocation failure injection for Program tests
 *
 * KEY CONCEPT:
 *   Only the test executable substitutes the compiler and verifier allocators.
 *   Production code retains the ordinary allocator and has no test-mode branch.
 */

#ifndef XR_PROGRAM_ALLOCATION_PROBE_H
#define XR_PROGRAM_ALLOCATION_PROBE_H

#include "base/xmalloc.h"

static inline void *xr_program_test_system_malloc(size_t size) {
    return xr_malloc(size);
}

static inline void *xr_program_test_system_calloc(size_t count, size_t size) {
    return xr_calloc(count, size);
}

static inline void xr_program_test_system_free(void *pointer) {
    xr_free(pointer);
}

XR_FUNC void *xr_program_test_malloc(size_t size);
XR_FUNC void *xr_program_test_calloc(size_t count, size_t size);
XR_FUNC void xr_program_test_free(void *pointer);

#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc(size) xr_program_test_malloc(size)
#define xr_calloc(count, size) xr_program_test_calloc(count, size)
#define xr_free(pointer) xr_program_test_free(pointer)

#endif // XR_PROGRAM_ALLOCATION_PROBE_H
