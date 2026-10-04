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
    uint32_t capacity; /* Allocated bitmap bytes, never retained query facts. */
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
    uint32_t length=count/8+(count%8!=0);
    if (scratch->capacity<length) {
        unsigned char *bytes=xir_compile_calloc(context,length,1,status);
        if (!bytes) return NULL;
        xr_compile_resources_free(scratch->bytes);scratch->bytes=bytes;scratch->capacity=length;
    } else {
        if (!xir_compile_work(context,length)) { *status=XR_XIR_BUDGET; return NULL; }
        memset(scratch->bytes,0,length);
    }
    return scratch->bytes;
}
/* A bitmap update reads one byte and writes that byte after charging both. */
static inline XrXirStatus xir_type_pending_mark(const XrXirCompileContext *context,
    unsigned char *pending, uint32_t index) {
    if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
    pending[index/8] |= (unsigned char)(1u<<(index%8));
    return XR_XIR_OK;
}
/* Strict descending IDs preserve the existing first reachable error. Lower
 * children can update the current byte, so each pop reads fresh bitmap state. */
static inline XrXirStatus xir_type_pending_next(const XrXirCompileContext *context,
    unsigned char *pending, uint32_t *ceiling, uint32_t *index, bool *found) {
    uint32_t at=*ceiling;
    while (at) {
        uint32_t byte=(at-1)/8;
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        unsigned value=pending[byte];
        if (!value) { at=byte*8; continue; }
        /* Three binary bit-selection steps, then one actual bitmap write. */
        if (!xir_compile_work(context,3)) return XR_XIR_BUDGET;
        unsigned top=0, bits=value;
        if (bits>=16) { bits>>=4; top+=4; }
        if (bits>=4) { bits>>=2; top+=2; }
        if (bits>=2) ++top;
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        pending[byte]=(unsigned char)(value & ~(1u<<top));
        *index=byte*8+top; *ceiling=*index; *found=true;
        return XR_XIR_OK;
    }
    *ceiling=0; *found=false; return XR_XIR_OK;
}
/* Signature-shape storage contains no retained type or declaration facts. */
XR_FUNC XrXirStatus xr_xir_compile_type_expression_shape_scratch(const XrXirCompileContext *context,
    const XrXirTypes *types, XrXirType type, uint32_t parameter_count, XirTypeScratch *scratch);
XR_FUNC XrXirStatus xr_xir_compile_type_access_scratch(const XrXirCompileContext *context,
    const XrXirModule *module, uint32_t function, XrXirType type, XirTypeScratch *scratch);
#endif // XXIR_TYPE_SCRATCH_INTERNAL_H
