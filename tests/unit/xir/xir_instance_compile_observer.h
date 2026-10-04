/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_instance_compile_observer.h - Actual compiler allocator faults and release
 *
 * KEY CONCEPT:
 *   Only the compiler resource allocator is replaced in these two test units.
 *   Runtime fault injection remains a separate allocation domain.
 */
#ifndef XIR_INSTANCE_COMPILE_OBSERVER_H
#define XIR_INSTANCE_COMPILE_OBSERVER_H
#include "base/xmalloc.h"
#include <string.h>
typedef struct InstanceCompileAllocation { void *pointer; size_t bytes; } InstanceCompileAllocation;
static InstanceCompileAllocation instance_compile_blocks[32768];
static size_t instance_compile_live, instance_compile_bytes, instance_compile_peak;
static size_t instance_compile_attempts, instance_compile_fail_at = SIZE_MAX;
static bool instance_compile_injected;
static void *instance_compile_malloc(size_t bytes) {
    if (instance_compile_attempts++ == instance_compile_fail_at) {
        instance_compile_injected = true;
        return NULL;
    }
    void *pointer = xr_malloc(bytes);
    if (pointer) {
        CHECK(instance_compile_live < 32768 && bytes <= SIZE_MAX - instance_compile_bytes);
        instance_compile_blocks[instance_compile_live++] = (InstanceCompileAllocation){pointer, bytes};
        instance_compile_bytes += bytes;
        if (instance_compile_bytes > instance_compile_peak) instance_compile_peak = instance_compile_bytes;
    }
    return pointer;
}
static void instance_compile_free(void *pointer) {
    if (pointer) {
        size_t i = 0;
        while (i < instance_compile_live && instance_compile_blocks[i].pointer != pointer) ++i;
        CHECK(i < instance_compile_live && instance_compile_blocks[i].bytes <= instance_compile_bytes);
        instance_compile_bytes -= instance_compile_blocks[i].bytes;
        instance_compile_blocks[i] = instance_compile_blocks[--instance_compile_live];
    }
    xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) instance_compile_malloc(bytes)
#define xr_free(pointer) instance_compile_free(pointer)
#include "base/xcompile_resources.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_malloc")
static inline void instance_compile_zero(void) {
    CHECK(!instance_compile_live && !instance_compile_bytes);
}
static inline void instance_compile_report(void) {
    instance_compile_zero();
    printf("compiler physical blocks/bytes=0/0; peak=%zu\n", instance_compile_peak);
}
#endif // XIR_INSTANCE_COMPILE_OBSERVER_H
