/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_internal.h - Complete immutable bytecode admission without execution
 */
#ifndef XRBC_INTERNAL_H
#define XRBC_INTERNAL_H

#include "xrbc.h"
#include "xir/xxir_types.h"

static inline bool rbc_captured_map_type(const XrRbcModule *module, XrXirType type) {
    if (!module->closure_types) return false;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(module->closure_types);
    const XrXirTypeNode *node = xr_xir_type_node(types, xr_xir_cell_element(types, type));
    return node && node->kind == XR_XIR_TYPE_MAP && node->element == XR_XIR_STRING && node->result == XR_XIR_I64;
}
static inline bool rbc_captured_cell_type(const XrRbcModule *module, XrXirType type) {
    return module->closure_types && (xr_xir_cell_element(xr_xir_compile_type_arena_types(module->closure_types),
        type) == XR_XIR_I64 || rbc_captured_map_type(module, type));
}

static inline bool xr_rbc_aggregate_return_word(const XrRbcAggregateAccess *accesses,
    size_t count, uint32_t word) {
    return (word & 255) == XR_RBC_AGGREGATE_ACCESS && accesses && (word >> 16) < count &&
        (accesses[word >> 16].operation == XR_RBC_AGGREGATE_RETURN ||
         accesses[word >> 16].operation == XR_RBC_FUNCTION_RETURN ||
         accesses[word >> 16].operation == XR_RBC_FUNCTION_SCOPE_RETURN);
}
#include "shared/xr_pod_state.h"

static inline XrRbcFunctionScopes rbc_function_scopes(const XrRbcModule *module, const XrRbcFunction *function) {
    return module->function_scopes ? module->function_scopes[function - module->functions] : (XrRbcFunctionScopes){0};
}

enum { XR_RBC_ARRAY_FAMILY_COUNT = 4 };
enum { XR_RBC_AGGREGATE_INLINE_SCALARS = 16 };

static inline bool xr_rbc_activation_size(const XrRbcFunction *function, size_t *environment,
    size_t *total) {
    size_t bytes = function->aggregate_size;
    if (function->environment_size) {
        if (bytes > SIZE_MAX - 7u) return false;
        bytes = (bytes + 7u) & ~(size_t)7u;
        *environment = bytes;
        if (function->environment_size > SIZE_MAX - bytes) return false;
        bytes += function->environment_size;
    } else *environment = bytes;
    *total = bytes;
    return true;
}

static inline bool xr_rbc_view_kind(XrRbcRegisterKind kind) {
    return kind >= XR_RBC_VIEW_FRAME && kind <= XR_RBC_SLICE_I64;
}

typedef struct XrRbcAggregateLayout {
    uint32_t size, alignment, scalar_slots;
} XrRbcAggregateLayout;

static inline bool xr_rbc_aggregate_layout(XrPodType type, XrRbcAggregateLayout *output) {
    if (!output || (type.kind != XR_POD_FIXED_U8 && type.kind != XR_POD_NULLABLE && type.kind != XR_POD_TUPLE_I64) ||
        !type.physical.size || !type.physical.alignment || type.physical.alignment > 8 ||
        (type.physical.alignment & (type.physical.alignment - 1))) return false;
    bool lanes = type.kind == XR_POD_FIXED_U8 && type.count <= XR_RBC_AGGREGATE_INLINE_SCALARS;
    *output = (XrRbcAggregateLayout){lanes ? type.count * 8u : type.physical.size,
        lanes ? 8u : type.physical.alignment, lanes ? type.count : 0u};
    return true;
}

static inline bool xr_rbc_aggregate_type_valid(const XrRbcModule *module, uint32_t ordinal) {
    XrPodType type = module->aggregate_types[ordinal];
    XrPodLayout expected;
    if (type.kind == XR_POD_FIXED_U8) {
        if (!type.count || type.count > 65535 || type.element) return false;
        expected = (XrPodLayout){type.count, 1, type.count};
    } else if (type.kind == XR_POD_TUPLE_I64) {
        if (type.element != XR_POD_I64 || !xr_pod_tuple_i64_layout(type.count, &expected)) return false;
    } else if (type.kind == XR_POD_NULLABLE && !type.count) {
        XrPodLayout payload;
        if (type.element < XR_POD_SCALAR_COUNT) {
            if (!xr_pod_scalar_layout((XrPodScalar)type.element, &payload)) return false;
        } else {
            uint32_t child = type.element - XR_POD_SCALAR_COUNT;
            if (child >= ordinal) return false;
            payload = module->aggregate_types[child].physical;
        }
        if (!xr_pod_nullable_layout(payload, &expected)) return false;
    } else return false;
    return type.physical.size == expected.size && type.physical.alignment == expected.alignment &&
        type.physical.scalar_count == expected.scalar_count;
}

static inline XrRbcRegisterKind xr_rbc_array_kind_at(uint32_t family) {
    static const XrRbcRegisterKind kinds[] = {XR_RBC_ARRAY_I64, XR_RBC_ARRAY_STRING, XR_RBC_ARRAY_U8, XR_RBC_ARRAY_U64};
    return family < XR_RBC_ARRAY_FAMILY_COUNT ? kinds[family] : XR_RBC_BITS;
}
static inline XrRbcRegisterKind xr_rbc_array_cell_at(uint32_t family) {
    static const XrRbcRegisterKind kinds[] = {
        XR_RBC_ARRAY_I64_CELL, XR_RBC_ARRAY_STRING_CELL, XR_RBC_ARRAY_U8_CELL, XR_RBC_ARRAY_U64_CELL};
    return family < XR_RBC_ARRAY_FAMILY_COUNT ? kinds[family] : XR_RBC_BITS;
}
static inline uint32_t xr_rbc_array_family(XrRbcRegisterKind kind) {
    for (uint32_t i = 0; i < XR_RBC_ARRAY_FAMILY_COUNT; ++i)
        if (kind == xr_rbc_array_kind_at(i)) return i;
    return XR_RBC_ARRAY_FAMILY_COUNT;
}
static inline bool xr_rbc_state_array(uint32_t kind) {
    return kind >= XRT_MODULE_ARRAY_I64 && kind <= XRT_MODULE_ARRAY_U64;
}
static inline XrRbcRegisterKind xr_rbc_array_ref_at(uint32_t family) {
    static const XrRbcRegisterKind kinds[] = {
        XR_RBC_ARRAY_I64_REF, XR_RBC_ARRAY_STRING_REF, XR_RBC_ARRAY_U8_REF, XR_RBC_ARRAY_U64_REF};
    return family < XR_RBC_ARRAY_FAMILY_COUNT ? kinds[family] : XR_RBC_BITS;
}
static inline uint32_t xr_rbc_array_ref_family(XrRbcRegisterKind kind) {
    for (uint32_t i = 0; i < XR_RBC_ARRAY_FAMILY_COUNT; ++i)
        if (kind == xr_rbc_array_ref_at(i)) return i;
    return XR_RBC_ARRAY_FAMILY_COUNT;
}

/* Both compiler publication and preparation use this complete receiver.
 * It does not allocate an execution or retain the borrowed module/locations. */
XR_FUNC XrRbcStatus xr_rbc_module_admit(const XrRbcModule *, const XrtLocationOwner *);

#endif // XRBC_INTERNAL_H
