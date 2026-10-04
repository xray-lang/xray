/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_scratch_internal.h - Owned bytes for operation-local type traversals
 *
 * KEY CONCEPT:
 *   Each query clears and rechecks its current context; storage grants no facts.
 */
#ifndef XXIR_TYPE_SCRATCH_INTERNAL_H
#define XXIR_TYPE_SCRATCH_INTERNAL_H
#include "xxir_types.h"
#include "xxir_compile_memory.h"
typedef struct XirTypeScratch {
    XrCompileResources *resources;
    unsigned char *bytes;
    uint32_t capacity;
} XirTypeScratch;
static inline void xir_type_scratch_free(XirTypeScratch *scratch) {
    if (!scratch) return;
    xr_compile_resources_free(scratch->bytes); *scratch=(XirTypeScratch){0};
}
static inline unsigned char *xir_type_scratch_pending(const XrXirCompileContext *context,
    XirTypeScratch *scratch, uint32_t count, XrXirStatus *status) {
    if (!xir_compile_context_valid(context) || !scratch || !count ||
        scratch->resources!=context->resources) { *status=XR_XIR_BAD_STRUCTURE; return NULL; }
    if (*status!=XR_XIR_OK) return NULL;
    if (scratch->capacity<count) {
        unsigned char *bytes=xir_compile_calloc(context,count,1,status);
        if (!bytes) return NULL;
        xr_compile_resources_free(scratch->bytes);scratch->bytes=bytes;scratch->capacity=count;
    } else {
        if (!xir_compile_work(context,count)) { *status=XR_XIR_BUDGET; return NULL; }
        memset(scratch->bytes,0,count);
    }
    return scratch->bytes;
}
/* Signature-shape storage contains no retained type or declaration facts. */
XR_FUNC XrXirStatus xr_xir_compile_type_expression_shape_scratch(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirType type, uint32_t parameter_count, XirTypeScratch *scratch);
XR_FUNC XrXirStatus xr_xir_compile_type_access_scratch(const XrXirCompileContext *context,
    const XrXirModule *module, uint32_t function, XrXirType type, XirTypeScratch *scratch);
#endif // XXIR_TYPE_SCRATCH_INTERNAL_H
