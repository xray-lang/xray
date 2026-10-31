/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_pointer.h - Typed raw byte pointers with independent origin lifetime
 *
 * KEY CONCEPT:
 *   A pointer retains its origin record, never the origin's payload. Readonly
 *   conversion preserves the actual origin and cannot grant write permission.
 */
#ifndef XXIR_POINTER_H
#define XXIR_POINTER_H
#include "xxir_type_arena.h"
#include "xxir_types.h"
static inline bool xr_xir_type_is_pointer(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && (node->kind==XR_XIR_TYPE_PTR || node->kind==XR_XIR_TYPE_MUT_PTR) &&
        node->element==XR_XIR_U8 && !node->parameter_span;
}
static inline bool xr_xir_pointer_writable(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return xr_xir_type_is_pointer(types,type) && node->kind==XR_XIR_TYPE_MUT_PTR;
}
XR_FUNC XrXirValueStatus xr_xir_pointer_null(XrXirDomain *domain,const XrXirTypeArena *arena,
    XrXirType type,XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_pointer_readonly(const XrXirValue *source,
    const XrXirTypeArena *arena,XrXirType type,XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_pointer_offset(const XrXirValue *source,int64_t offset,XrXirValue *output);
/* These raw accesses require a live origin and actual writable type for stores.
 * The unsafe caller is responsible for address, bounds, NULL and alignment. */
XR_FUNC XrXirValueStatus xr_xir_pointer_load(const XrXirValue *source,int64_t offset,uint8_t *output);
XR_FUNC XrXirValueStatus xr_xir_pointer_store(const XrXirValue *target,int64_t offset,uint8_t value);
XR_FUNC XrXirValueStatus xr_xir_pointer_copy(const XrXirValue *target,
    const XrXirValue *source,int64_t bytes,bool overlap);
XR_FUNC XrXirValueStatus xr_xir_pointer_equal(const XrXirValue *left,const XrXirValue *right,bool *output);
XR_FUNC XrXirValueStatus xr_xir_page_allocate(XrXirDomain *domain,const XrXirTypeArena *arena,
    XrXirType type,int64_t bytes,int64_t protection,XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_page_protect(const XrXirValue *pointer,int64_t bytes,int64_t protection,bool *output);
XR_FUNC XrXirValueStatus xr_xir_page_free(const XrXirValue *pointer,int64_t bytes,bool *output);
#endif // XXIR_POINTER_H
