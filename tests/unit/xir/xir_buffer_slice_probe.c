/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_buffer_slice_probe.c - Physical allocation faults in actual view creation
 *
 * KEY CONCEPT:
 *   Failures occur inside the production Value translation unit. The observer
 *   records physical ownership separately from the actual Domain live ledger.
 */
#include "base/xmalloc.h"
#include <stdlib.h>
static size_t slice_physical,slice_attempt,slice_fail_at=SIZE_MAX;
static void *slice_malloc(size_t bytes) {
    if (slice_attempt++==slice_fail_at) return NULL;
    void *memory=xr_malloc(bytes);if (memory) ++slice_physical;return memory;
}
static void *slice_calloc(size_t count,size_t bytes) {
    if (slice_attempt++==slice_fail_at) return NULL;
    void *memory=xr_calloc(count,bytes);if (memory) ++slice_physical;return memory;
}
static void *slice_realloc(void *memory,size_t bytes) {
    if (slice_attempt++==slice_fail_at) return NULL;
    bool empty=memory==NULL;void *replacement=xr_realloc(memory,bytes);
    if (replacement && empty) ++slice_physical;
    return replacement;
}
static void slice_free(void *memory) {
    if (memory) {if (!slice_physical) abort();--slice_physical;}xr_free(memory);
}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(bytes) slice_malloc(bytes)
#define xr_calloc(count,bytes) slice_calloc(count,bytes)
#define xr_realloc(memory,bytes) slice_realloc(memory,bytes)
#define xr_free(memory) slice_free(memory)
#include "xir/xxir_value.c"
XR_FUNC void buffer_slice_fail(size_t ordinal) {slice_attempt=0;slice_fail_at=ordinal;}
XR_FUNC void buffer_slice_fail_end(void) {slice_fail_at=SIZE_MAX;}
XR_FUNC size_t buffer_slice_physical(void) {return slice_physical;}
