/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_p09_compile_observer.h - Indexed physical compiler allocation observation
 *
 * KEY CONCEPT:
 *   Pointer lookup does not rescan every live block at each physical free.
 *   Fault ordinals and delegated allocations remain outside the static index.
 */
#ifndef XIR_P09_COMPILE_OBSERVER_H
#define XIR_P09_COMPILE_OBSERVER_H
#include "base/xmalloc.h"
#include <string.h>
typedef struct InstanceCompileAllocation { void *pointer; size_t bytes; } InstanceCompileAllocation;
static InstanceCompileAllocation instance_compile_blocks[65536];
static size_t instance_compile_live, instance_compile_bytes, instance_compile_peak;
static size_t instance_compile_attempts, instance_compile_fail_at = SIZE_MAX;
static bool instance_compile_injected;
static size_t instance_compile_slot(const void *pointer) {
    uint64_t key = (uint64_t)(uintptr_t)pointer;
    key ^= key >> 33; key *= UINT64_C(0xff51afd7ed558ccd); key ^= key >> 33;
    return (size_t)key & 65535;
}
static void *instance_compile_malloc(size_t bytes) {
#ifdef XR_INSTANCE_COMPILE_BEFORE_ALLOCATION
    XR_INSTANCE_COMPILE_BEFORE_ALLOCATION(bytes);
#endif
    if (instance_compile_attempts++ == instance_compile_fail_at) {
        instance_compile_injected = true; return NULL;
    }
    void *pointer = xr_malloc(bytes);
    if (pointer) {
        CHECK(instance_compile_live < 32768 && bytes <= SIZE_MAX - instance_compile_bytes);
        size_t slot = instance_compile_slot(pointer);
        while (instance_compile_blocks[slot].pointer) {
            CHECK(instance_compile_blocks[slot].pointer != pointer);
            slot = (slot + 1) & 65535;
        }
        instance_compile_blocks[slot] = (InstanceCompileAllocation){pointer, bytes};
        ++instance_compile_live; instance_compile_bytes += bytes;
        if (instance_compile_bytes > instance_compile_peak) instance_compile_peak = instance_compile_bytes;
    }
    return pointer;
}
static void instance_compile_free(void *pointer) {
    if (pointer) {
        size_t hole = instance_compile_slot(pointer);
        while (instance_compile_blocks[hole].pointer != pointer) {
            CHECK(instance_compile_blocks[hole].pointer);
            hole = (hole + 1) & 65535;
        }
        CHECK(instance_compile_live && instance_compile_blocks[hole].bytes <= instance_compile_bytes);
        instance_compile_bytes -= instance_compile_blocks[hole].bytes; --instance_compile_live;
        for (size_t next = (hole + 1) & 65535; instance_compile_blocks[next].pointer; next = (next + 1) & 65535) {
            size_t home = instance_compile_slot(instance_compile_blocks[next].pointer);
            bool stays = hole <= next ? hole < home && home <= next : hole < home || home <= next;
            if (!stays) { instance_compile_blocks[hole] = instance_compile_blocks[next]; hole = next; }
        }
        instance_compile_blocks[hole] = (InstanceCompileAllocation){0};
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
#endif // XIR_P09_COMPILE_OBSERVER_H
