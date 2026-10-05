/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cleanup_runtime_owner.h - Physical runtime ownership without changing call fault indices
 */
#ifndef XIR_CLEANUP_RUNTIME_OWNER_H
#define XIR_CLEANUP_RUNTIME_OWNER_H
#include "base/xmalloc.h"
#include <string.h>
typedef struct CleanupRuntimeBlock { void *pointer; size_t bytes; } CleanupRuntimeBlock;
static CleanupRuntimeBlock cleanup_runtime_blocks[4096];
static size_t cleanup_runtime_live, cleanup_runtime_bytes, cleanup_runtime_peak;
static void cleanup_runtime_record(void *pointer, size_t bytes) {
    if (!pointer) return;
    CHECK(cleanup_runtime_live < 4096 && bytes <= SIZE_MAX - cleanup_runtime_bytes);
    cleanup_runtime_blocks[cleanup_runtime_live++] = (CleanupRuntimeBlock){pointer,bytes};
    cleanup_runtime_bytes += bytes;
    if (cleanup_runtime_bytes > cleanup_runtime_peak) cleanup_runtime_peak = cleanup_runtime_bytes;
}
static void *cleanup_runtime_malloc(size_t bytes) {
    void *pointer = xr_malloc(bytes); cleanup_runtime_record(pointer,bytes); return pointer;
}
static void *cleanup_runtime_calloc(size_t count, size_t bytes) {
    CHECK(!bytes || count <= SIZE_MAX / bytes);
    void *pointer = xr_calloc(count,bytes); cleanup_runtime_record(pointer,count * bytes); return pointer;
}
static void cleanup_runtime_free(void *pointer) {
    if (pointer) {
        size_t at = 0;
        while (at < cleanup_runtime_live && cleanup_runtime_blocks[at].pointer != pointer) ++at;
        CHECK(at < cleanup_runtime_live && cleanup_runtime_blocks[at].bytes <= cleanup_runtime_bytes);
        cleanup_runtime_bytes -= cleanup_runtime_blocks[at].bytes;
        cleanup_runtime_blocks[at] = cleanup_runtime_blocks[--cleanup_runtime_live];
    }
    xr_free(pointer);
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_calloc")
#pragma push_macro("xr_free")
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc cleanup_runtime_malloc
#define xr_calloc cleanup_runtime_calloc
#define xr_free cleanup_runtime_free
#include "xir/xxir_type_arena.c"
#include "xir/xxir_value.c"
#include "xir/xxir_scalar.c"
#include "xir/xxir_float.c"
#include "xir/xxir_program.c"
#include "xir/xxir_instance.c"
#include "xir/xxir_output.c"
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_calloc")
#pragma pop_macro("xr_malloc")
#endif // XIR_CLEANUP_RUNTIME_OWNER_H
