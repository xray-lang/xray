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
#include "xray_native_array.h"
#include "xray_native_function.h"
#include "xray_native_state.h"
#include "base/xchecks.h"
#include <string.h>

typedef struct XrRbcNativeShape {
    /* The result is at A. Arguments follow it, ending with the source ID. */
    uint8_t window_inputs;
    bool writes_result;
    bool requires_typed_window;
} XrRbcNativeShape;

static inline XrRbcNativeShape xr_rbc_native_shape(XrRbcNativeKind kind) {
    static const XrRbcNativeShape shapes[XR_RBC_NATIVE_COUNT] = {
        [XR_RBC_NATIVE_PRINT_I64] = {.window_inputs = 2},
        [XR_RBC_NATIVE_CLOCK_REALTIME] = {.window_inputs = 1, .writes_result = true},
        [XR_RBC_NATIVE_CLOCK_CPU] = {.window_inputs = 1, .writes_result = true},
        [XR_RBC_NATIVE_CLOCK_MONOTONIC] = {.window_inputs = 1, .writes_result = true},
        [XR_RBC_NATIVE_UTC_OFFSET_AT] = {.window_inputs = 2, .writes_result = true},
#define XR_MATH_RBC_SHAPE(tag, name, arity) \
        [XR_RBC_NATIVE_MATH_##tag] = {.window_inputs = (arity) + 1, .writes_result = true, .requires_typed_window = true},
        XR_MATH_HOST_OPERATIONS(XR_MATH_RBC_SHAPE)
#undef XR_MATH_RBC_SHAPE
        [XR_RBC_NATIVE_MATH_RANDOM] = {.window_inputs = 1, .writes_result = true,
            .requires_typed_window = true},
        [XR_RBC_NATIVE_MATH_RANDOM_INT] = {.window_inputs = 3, .writes_result = true,
            .requires_typed_window = true},
#if defined(XR_RBC_LINKED_MATH_LERP)
        [XR_RBC_NATIVE_MATH_LERP] = {.window_inputs = 4, .writes_result = true,
            .requires_typed_window = true},
#endif
    };
    return (unsigned)kind < XR_RBC_NATIVE_COUNT ? shapes[kind] : (XrRbcNativeShape){0};
}

enum { XR_RBC_FUNCTION_LANES = sizeof(XrtFunction) / sizeof(uint64_t) };
_Static_assert(sizeof(XrtFunction) % sizeof(uint64_t) == 0 &&
    _Alignof(XrtFunction) <= _Alignof(uint64_t), "indexed callable storage fits complete register lanes");
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

enum { XR_RBC_ARRAY_FAMILY_COUNT = XRT_BACKING_ARRAY_COUNT };
enum { XR_RBC_AGGREGATE_INLINE_SCALARS = 16 };

/* One fixed element keeps its own natural width inside the aggregate bytes,
 * exactly as the compiled aggregate and the runtime plan place it. */
static inline uint32_t xr_rbc_pod_scalar_size(uint32_t element) {
    XrPodLayout scalar;
    return element < XR_POD_SCALAR_COUNT && xr_pod_scalar_layout((XrPodScalar)element, &scalar) ?
        scalar.size : 0;
}
static inline void xr_rbc_fixed_store(uint8_t *at, uint32_t width, uint64_t bits) {
    XR_CHECK(at && (width == 1 || width == 4 || width == 8),
        "admitted fixed storage stores one admitted element width");
    if (width == 1) { at[0] = (uint8_t)bits; return; }
    if (width == 4) { uint32_t element = (uint32_t)bits; memcpy(at, &element, sizeof element); return; }
    uint64_t element = bits; memcpy(at, &element, sizeof element);
}
static inline uint64_t xr_rbc_fixed_load(const uint8_t *at, uint32_t width) {
    XR_CHECK(at && (width == 1 || width == 4 || width == 8),
        "admitted fixed storage loads one admitted element width");
    if (width == 1) return at[0];
    if (width == 4) { uint32_t element = 0; memcpy(&element, at, sizeof element); return element; }
    uint64_t element = 0; memcpy(&element, at, sizeof element); return element;
}

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
    if (!output || (type.kind != XR_POD_FIXED_INT && type.kind != XR_POD_NULLABLE) ||
        !type.physical.size || !type.physical.alignment || type.physical.alignment > 8 ||
        (type.physical.alignment & (type.physical.alignment - 1))) return false;
    bool lanes = type.kind == XR_POD_FIXED_INT && type.count <= XR_RBC_AGGREGATE_INLINE_SCALARS;
    *output = (XrRbcAggregateLayout){lanes ? type.count * 8u : type.physical.size,
        lanes ? 8u : type.physical.alignment, lanes ? type.count : 0u};
    return true;
}

static inline bool xr_rbc_aggregate_type_valid(const XrRbcModule *module, uint32_t ordinal) {
    XrPodType type = module->aggregate_types[ordinal];
    XrPodLayout expected;
    if (type.kind == XR_POD_FIXED_INT) {
        XrtValueKind kind;
        uint32_t width = xr_rbc_pod_scalar_size(type.element);
        if (!type.count || type.count > 65535 || !width ||
            !xr_pod_value_kind((XrPodScalar)type.element, &kind) ||
            !xrt_value_kind_is_fixed_element(kind)) return false;
        expected = (XrPodLayout){(uint32_t)(type.count * width), width, type.count};
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
    static const XrRbcRegisterKind kinds[XR_RBC_ARRAY_FAMILY_COUNT] = {
        XR_RBC_ARRAY_I64, XR_RBC_ARRAY_STRING, XR_RBC_ARRAY_U8, XR_RBC_ARRAY_U64, XR_RBC_ARRAY_F64};
    return family < XR_RBC_ARRAY_FAMILY_COUNT ? kinds[family] : XR_RBC_BITS;
}
static inline XrRbcRegisterKind xr_rbc_array_cell_at(uint32_t family) {
    static const XrRbcRegisterKind kinds[] = {
        XR_RBC_ARRAY_I64_CELL, XR_RBC_ARRAY_STRING_CELL, XR_RBC_ARRAY_U8_CELL, XR_RBC_ARRAY_U64_CELL,
        XR_RBC_ARRAY_F64_CELL};
    return family < XR_RBC_ARRAY_FAMILY_COUNT ? kinds[family] : XR_RBC_BITS;
}
static inline uint32_t xr_rbc_array_family(XrRbcRegisterKind kind) {
    for (uint32_t i = 0; i < XR_RBC_ARRAY_FAMILY_COUNT; ++i)
        if (kind == xr_rbc_array_kind_at(i)) return i;
    return XR_RBC_ARRAY_FAMILY_COUNT;
}
static inline bool xr_rbc_state_array(uint32_t kind) {
    return xrt_value_kind_is_scalar_array(kind);
}
static inline XrRbcRegisterKind xr_rbc_array_ref_at(uint32_t family) {
    static const XrRbcRegisterKind kinds[] = {
        XR_RBC_ARRAY_I64_REF, XR_RBC_ARRAY_STRING_REF, XR_RBC_ARRAY_U8_REF, XR_RBC_ARRAY_U64_REF,
        XR_RBC_ARRAY_F64_REF};
    return family < XR_RBC_ARRAY_FAMILY_COUNT ? kinds[family] : XR_RBC_BITS;
}
static inline uint32_t xr_rbc_array_ref_family(XrRbcRegisterKind kind) {
    for (uint32_t i = 0; i < XR_RBC_ARRAY_FAMILY_COUNT; ++i)
        if (kind == xr_rbc_array_ref_at(i)) return i;
    return XR_RBC_ARRAY_FAMILY_COUNT;
}

/* Compiler publication and public raw preparation use this complete receiver.
 * It does not allocate an execution or retain the borrowed module/locations. */
XR_FUNC XrRbcStatus xr_rbc_module_admit(const XrRbcModule *, const XrtLocationOwner *);

/* Private receiving path for an immutable module that has already passed
 * complete admission. Runtime entry arguments and allocation failures remain
 * checked. Public raw callers use xr_rbc_module_prepare instead. */
XR_FUNC XrRbcStatus xr_rbc_module_prepare_admitted(const XrRbcModule *, uint32_t entry,
    const uint64_t *arguments, size_t argument_count, XrtLocationOwner *, XrRbcExecution **);
/* Only the synchronous test-session owner supplies this matching State pair. */
XR_FUNC XrRbcStatus xr_rbc_module_prepare_test_action(const XrRbcModule *, uint32_t entry,
    XrtLocationOwner *, XrtModuleLayout *, XrtModuleState *, XrRbcExecution **);
XR_FUNC XrRbcOutcome *xr_rbc_execution_run_test_action(XrRbcExecution *, const XrtTestRetirementHook *,
    XrtMetadataCloser);
XR_FUNC bool xr_rbc_execution_root_retired(const XrRbcExecution *);
XR_FUNC XrOsIoStatus xr_rbc_test_action_free(XrRbcExecution **, XrtMetadataCloser);
XR_FUNC XrRbcStatus xr_rbc_test_close_failure(XrOsIoStatus, XrtLocationOwner *, XrRbcOutcome **);

#endif // XRBC_INTERNAL_H
