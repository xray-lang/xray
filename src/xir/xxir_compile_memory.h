/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_compile_memory.h - Typed allocation for compiler algorithms
 *
 * KEY CONCEPT:
 *   Helpers preserve the first failure and never select an allocation domain.
 */
#ifndef XXIR_COMPILE_MEMORY_H
#define XXIR_COMPILE_MEMORY_H
#include "xxir.h"
#include <string.h>

static inline bool xir_compile_context_valid(const XrXirCompileContext *context) {
    return context && context->resources;
}
static inline XrXirStatus xir_compile_resource_status(XrCompileResourceStatus status) {
    switch (status) {
    case XR_COMPILE_RESOURCE_OK: return XR_XIR_OK;
    case XR_COMPILE_RESOURCE_MEMORY_LIMIT: return XR_XIR_MEMORY_LIMIT;
    case XR_COMPILE_RESOURCE_OVERFLOW: return XR_XIR_OVERFLOW;
    case XR_COMPILE_RESOURCE_CANCELLED: return XR_XIR_CANCELLED;
    case XR_COMPILE_RESOURCE_DEADLINE: return XR_XIR_DEADLINE;
    case XR_COMPILE_RESOURCE_CLOCK_FAILURE: return XR_XIR_CLOCK_FAILURE;
    case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_XIR_OUT_OF_MEMORY;
    default: return XR_XIR_BAD_STRUCTURE;
    }
}
static inline void *xir_compile_alloc(const XrXirCompileContext *context, size_t bytes,
    XrXirStatus *status) {
    if (*status != XR_XIR_OK || !bytes) return NULL;
    void *memory = NULL;
    *status = xir_compile_context_valid(context) ?
        xir_compile_resource_status(xr_compile_resources_alloc(context->resources, bytes, &memory)) :
        XR_XIR_BAD_STRUCTURE;
    return memory;
}
static inline void *xir_compile_calloc(const XrXirCompileContext *context, size_t count,
    size_t size, XrXirStatus *status) {
    if (*status != XR_XIR_OK || !count || !size) return NULL;
    void *memory = NULL;
    *status = xir_compile_context_valid(context) ?
        xir_compile_resource_status(xr_compile_resources_calloc(context->resources, count, size, &memory)) :
        XR_XIR_BAD_STRUCTURE;
    return memory;
}
static inline void *xir_compile_copy(const XrXirCompileContext *context, const void *source,
    size_t bytes, XrXirStatus *status) {
    if (*status != XR_XIR_OK || !bytes) return NULL;
    if (!source) { *status = XR_XIR_BAD_STRUCTURE; return NULL; }
    void *memory = xir_compile_alloc(context, bytes, status);
    if (!memory) return NULL;

    memcpy(memory, source, bytes);
    return memory;
}
#endif // XXIR_COMPILE_MEMORY_H
