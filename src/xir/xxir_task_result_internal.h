/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_result_internal.h - Executable closed Task result families
 *
 * KEY CONCEPT:
 *   Capability admission does not replace the worker's Sendable or root proof.
 */
#ifndef XXIR_TASK_RESULT_INTERNAL_H
#define XXIR_TASK_RESULT_INTERNAL_H
#include "xxir_types.h"
static inline bool xr_xir_task_scalar_result_supported(XrXirType type) {
    return type == XR_XIR_UNIT || type == XR_XIR_BOOL || type == XR_XIR_RUNE ||
        xr_xir_type_is_number(type) || type == XR_XIR_STRING;
}
/* This is a backend capability test. The authentic worker certificate proves
 * the complete Sendable graph before spawn; a descriptor kind is no authority. */
static inline bool xr_xir_task_result_supported(const XrXirTypes *types, XrXirType type) {
    if (xr_xir_task_scalar_result_supported(type)) return true;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    if (!node || node->parameter_span) return false;
    if (xr_xir_type_is_class(types,type)) {
        const XrXirNominalTable *table=types->nominals;
        uint32_t flags=table->declarations?table->declarations[node->nominal.declaration].flags:
            table->identities[node->nominal.declaration].flags;
        return (flags & XR_XIR_NOMINAL_SENDABLE)!=0;
    }
    return node->kind == XR_XIR_TYPE_ARRAY || node->kind == XR_XIR_TYPE_NULLABLE ||
        node->kind == XR_XIR_TYPE_TUPLE || node->kind == XR_XIR_TYPE_ATOMIC ||
        node->kind == XR_XIR_TYPE_TASK || (node->kind == XR_XIR_TYPE_NOMINAL &&
        (xr_xir_type_is_enum(types,type) || xr_xir_type_is_struct(types,type)));
}
#endif // XXIR_TASK_RESULT_INTERNAL_H
