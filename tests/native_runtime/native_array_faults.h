/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_array_faults.h - Real value and state allocation failure accounting
 */
#ifndef NATIVE_ARRAY_FAULTS_H
#define NATIVE_ARRAY_FAULTS_H
typedef struct NativeArrayBlock { void *pointer; size_t bytes; } NativeArrayBlock;
static NativeArrayBlock native_array_blocks[256];
static size_t native_array_calls, native_array_live, native_array_bytes;
static size_t native_array_fail_at = SIZE_MAX;
static bool native_array_injected, native_array_pending_failure;
static void (*native_array_before_allocate)(void);
static void (*native_array_before_free)(void);
static bool native_array_fail(void) {
    if (native_array_before_allocate) {
        void (*observe)(void) = native_array_before_allocate;
        native_array_before_allocate = NULL;
        observe();
    }
    if (native_array_calls++ != native_array_fail_at) return false;
    native_array_injected = true;
    XrtValueRoot *root = xrt_value_root_active();
    if (root && root->pending.type == XR_XIR_STRING) native_array_pending_failure = true;
    return true;
}
static void native_array_record(void *pointer, size_t bytes) {
    if (!pointer) return;
    CHECK(native_array_live < 256 && bytes <= SIZE_MAX - native_array_bytes);
    native_array_blocks[native_array_live++] = (NativeArrayBlock){pointer, bytes};
    native_array_bytes += bytes;
}
static size_t native_array_block(void *pointer) {
    size_t n = 0;
    while (n < native_array_live && native_array_blocks[n].pointer != pointer) ++n;
    CHECK(n < native_array_live);
    return n;
}
static void *native_array_malloc(size_t bytes) {
    if (native_array_fail()) return NULL;
    void *memory = xr_malloc(bytes); native_array_record(memory, bytes); return memory;
}
static void *native_array_calloc(size_t count, size_t bytes) {
    CHECK(!count || bytes <= SIZE_MAX / count);
    if (native_array_fail()) return NULL;
    void *memory = xr_calloc(count, bytes); native_array_record(memory, count * bytes); return memory;
}
static void native_array_free(void *pointer) {
    if (pointer && native_array_before_free) native_array_before_free();
    if (pointer) {
        size_t n = native_array_block(pointer);
        native_array_bytes -= native_array_blocks[n].bytes;
        native_array_blocks[n] = native_array_blocks[--native_array_live];
    }
    xr_free(pointer);
}
static void *native_array_aligned(size_t bytes, size_t alignment) {
    if (native_array_fail()) return NULL;
    void *memory = xr_malloc_aligned(bytes, alignment); native_array_record(memory, bytes); return memory;
}
static void native_array_aligned_free(void *pointer, size_t alignment) {
    (void)alignment;
    if (pointer && native_array_before_free) native_array_before_free();
    if (pointer) {
        size_t n = native_array_block(pointer);
        native_array_bytes -= native_array_blocks[n].bytes;
        native_array_blocks[n] = native_array_blocks[--native_array_live];
    }
    xr_free_aligned(pointer, alignment);
}
static void *native_array_realloc(void *pointer, size_t bytes) {
    CHECK(bytes);
    if (native_array_fail()) return NULL;
    size_t n = pointer ? native_array_block(pointer) : 0;
    void *memory = xr_realloc(pointer, bytes);
    if (!memory) return NULL;
    if (pointer) {
        native_array_bytes -= native_array_blocks[n].bytes;
        native_array_bytes += bytes;
        native_array_blocks[n] = (NativeArrayBlock){memory, bytes};
    } else native_array_record(memory, bytes);
    return memory;
}
#pragma push_macro("xr_malloc")
#pragma push_macro("xr_calloc")
#pragma push_macro("xr_free")
#pragma push_macro("xr_realloc")
#pragma push_macro("xr_malloc_aligned")
#pragma push_macro("xr_free_aligned")
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#undef xr_realloc
#undef xr_malloc_aligned
#undef xr_free_aligned
#define xr_malloc(bytes) native_array_malloc(bytes)
#define xr_calloc(count, bytes) native_array_calloc(count, bytes)
#define xr_free(pointer) native_array_free(pointer)
#define xr_realloc(pointer, bytes) native_array_realloc(pointer, bytes)
#define xr_malloc_aligned(bytes, alignment) native_array_aligned(bytes, alignment)
#define xr_free_aligned(pointer, alignment) native_array_aligned_free(pointer, alignment)
#include "xir/xxir_value.c"
#include "runtime/native/xnative_module_state.c"
#include "runtime/native/xnative_view.c"
#pragma pop_macro("xr_free_aligned")
#pragma pop_macro("xr_malloc_aligned")
#pragma pop_macro("xr_realloc")
#pragma pop_macro("xr_free")
#pragma pop_macro("xr_calloc")
#pragma pop_macro("xr_malloc")
#endif // NATIVE_ARRAY_FAULTS_H
