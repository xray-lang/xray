/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_fixed_array_type_internal.h - Exact plain scalar fixed storage
 *
 * KEY CONCEPT:
 *   The element type determines storage stride without changing the owner header.
 */
#ifndef XR_XIR_FIXED_ARRAY_TYPE_INTERNAL_H
#define XR_XIR_FIXED_ARRAY_TYPE_INTERNAL_H
#include "xxir_types.h"
static inline uint32_t xir_fixed_array_scalar_bytes(XrXirType element) {
    return element==XR_XIR_U8?1u:element==XR_XIR_U32?4u:element==XR_XIR_U64?8u:0u;
}
static inline XrXirType xir_fixed_array_element(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && node->kind==XR_XIR_TYPE_FIXED_ARRAY &&
        xir_fixed_array_scalar_bytes(node->element)?node->element:XR_XIR_UNIT;
}
#endif
