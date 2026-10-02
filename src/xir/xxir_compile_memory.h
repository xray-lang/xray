/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_compile_memory.h - Typed allocation and work for compiler algorithms
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
    case XR_COMPILE_RESOURCE_BUDGET: return XR_XIR_BUDGET;
    case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_XIR_OUT_OF_MEMORY;
    default: return XR_XIR_BAD_STRUCTURE;
    }
}
static inline bool xir_compile_work(const XrXirCompileContext *context, uint64_t units) {
    return xir_compile_context_valid(context) &&
        xr_compile_resources_work(context->resources, units) == XR_COMPILE_RESOURCE_OK;
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
    if (!xir_compile_work(context, bytes)) {
        xr_compile_resources_free(memory); *status = XR_XIR_BUDGET; return NULL;
    }
    memcpy(memory, source, bytes);
    return memory;
}
#endif // XXIR_COMPILE_MEMORY_H
