/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value.h - Owned values and independently lived string allocation domains
 *
 * KEY CONCEPT:
 *   Explicit copy and drop preserve value semantics across execution boundaries.
 */
#ifndef XXIR_VALUE_H
#define XXIR_VALUE_H
#include "../base/xdefs.h"

#define XR_XIR_VALUE_ABI_VERSION 10u
#define XR_XIR_CONSTRUCTED_TYPE_BASE 256u
#define XR_XIR_CONSTRUCTED_TYPE_LIMIT 65536u
#define XR_XIR_TYPE_PARAMETER_BASE 65536u
#define XR_XIR_TYPE_PARAMETER_LIMIT 131072u
#define XR_XIR_ARCH_X86_64 1u
typedef enum XrXirType { XR_XIR_UNIT, XR_XIR_BOOL, XR_XIR_I64, XR_XIR_STRING, XR_XIR_ATOMIC_I64,
    XR_XIR_I8, XR_XIR_I16, XR_XIR_I32, XR_XIR_U8, XR_XIR_U16, XR_XIR_U32, XR_XIR_U64, XR_XIR_F32, XR_XIR_F64 } XrXirType;
static inline uint32_t xr_xir_integer_bits(XrXirType type) {
    switch (type) {
    case XR_XIR_I8: case XR_XIR_U8: return 8;
    case XR_XIR_I16: case XR_XIR_U16: return 16;
    case XR_XIR_I32: case XR_XIR_U32: return 32;
    case XR_XIR_I64: case XR_XIR_U64: return 64;
    default: return 0;
    }
}
static inline bool xr_xir_type_is_integer(XrXirType type) {
    return xr_xir_integer_bits(type) != 0;
}
static inline bool xr_xir_integer_signed(XrXirType type) {
    return type == XR_XIR_I64 || (type >= XR_XIR_I8 && type <= XR_XIR_I32);
}
static inline bool xr_xir_integer_payload_valid(XrXirType type, int64_t payload) {
    uint32_t bits = xr_xir_integer_bits(type);
    if (!bits) return false;
    if (bits == 64) return true;
    int64_t bound = INT64_C(1) << (bits - (xr_xir_integer_signed(type) ? 1 : 0));
    return payload >= (xr_xir_integer_signed(type) ? -bound : 0) && payload < bound;
}

static inline uint32_t xr_xir_float_bits(XrXirType type) {
    return type == XR_XIR_F32 ? 32 : type == XR_XIR_F64 ? 64 : 0;
}
static inline bool xr_xir_type_is_number(XrXirType type) {
    return xr_xir_type_is_integer(type) || xr_xir_float_bits(type) != 0;
}
static inline bool xr_xir_float_payload_valid(XrXirType type, int64_t payload) {
    uint64_t bits = (uint64_t) payload;
    if (type == XR_XIR_F32) {
        if (bits > UINT32_MAX) return false;
        return (bits & UINT64_C(0x7fffffff)) <= UINT64_C(0x7f800000) || bits == UINT64_C(0x7fc00000);
    }
    return type == XR_XIR_F64 && ((bits & UINT64_C(0x7fffffffffffffff)) <= UINT64_C(0x7ff0000000000000) ||
        bits == UINT64_C(0x7ff8000000000000));
}

typedef struct XrXirValue {
    uint32_t type, reserved;
    int64_t payload;
} XrXirValue;
typedef enum XrXirValueStatus {
    XR_XIR_VALUE_OK, XR_XIR_VALUE_BAD_ARGUMENT, XR_XIR_VALUE_BAD_UTF8,
    XR_XIR_VALUE_OOM, XR_XIR_VALUE_LIMIT, XR_XIR_VALUE_REFCOUNT_LIMIT,
    XR_XIR_VALUE_BOUNDS
} XrXirValueStatus;
typedef struct XrXirDomain XrXirDomain;
typedef struct XrXirTypeArena XrXirTypeArena;
typedef struct XrXirDomainStats {
    uint64_t live_bytes, peak_bytes, allocations, frees, reallocations;
} XrXirDomainStats;

XR_FUNC XrXirValueStatus xr_xir_domain_new(uint64_t byte_limit, XrXirDomain **output);
XR_FUNC void xr_xir_domain_drop(XrXirDomain *domain);
XR_FUNC XrXirDomainStats xr_xir_domain_stats(XrXirDomain *domain);
/* Self-validation does not grant permission to execute an escaped function. */
XR_FUNC bool xr_xir_value_valid(const XrXirValue *value);
XR_FUNC bool xr_xir_value_argument(const XrXirValue *value, const XrXirTypeArena *arena, XrXirType type);
XR_FUNC const XrXirTypeArena *xr_xir_value_arena(const XrXirValue *value);
/* Copy/new outputs must be canonical unit handles. Native pointers are trusted. */
XR_FUNC XrXirValueStatus xr_xir_value_copy(const XrXirValue *source, XrXirValue *output);
XR_FUNC void xr_xir_value_drop(XrXirValue *value);
XR_FUNC XrXirValueStatus xr_xir_string_new(XrXirDomain *domain, const char *bytes,
                                         size_t length, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_string_append(XrXirValue *destination, const XrXirValue *suffix);
XR_FUNC bool xr_xir_string_view(const XrXirValue *value, const char **bytes, size_t *length);
XR_FUNC bool xr_xir_string_runes(const XrXirValue *value, size_t *count);
XR_FUNC void xr_xir_owned_slot_clear(void *frame, uint32_t offset);
XR_FUNC XrXirValueStatus xr_xir_owned_slot_copy(void *frame, uint32_t offset, const XrXirTypeArena *arena,
                                                XrXirType type, int64_t payload);
XR_FUNC XrXirValueStatus xr_xir_string_slot_concat(void *frame, uint32_t offset,
                                                  int64_t left, int64_t right);
XR_FUNC XrXirValueStatus xr_xir_atomic_i64_new(XrXirDomain *domain, int64_t initial, XrXirValue *output);
XR_FUNC bool xr_xir_atomic_i64_load(const XrXirValue *value, int64_t *output);
XR_FUNC bool xr_xir_atomic_i64_fetch_add(const XrXirValue *value, int64_t delta, int64_t *previous);
XR_FUNC void xr_xir_owned_slot_move(void *frame, uint32_t offset, XrXirValue *owned);
typedef struct XrXirFunctionBinding {
    void *owner;
    void (*release)(void *owner);
    uint32_t entry;
    const XrXirValue *captures;
    uint32_t capture_count;
} XrXirFunctionBinding;
/* The caller validates a live execution gate; pointer equality is not authority. */
typedef XrXirValueStatus (*XrXirFunctionAdmission)(void *context,
    const XrXirFunctionBinding *binding, XrXirType type, uint64_t *work);
typedef struct XrXirValueAdmission {
    const XrXirTypeArena *arena;
    XrXirDomain *domain;
    XrXirFunctionAdmission function;
    void *context;
    uint64_t work, scratch_bytes;
} XrXirValueAdmission;
/* Admission consumes work before inspecting a value and never publishes it. */
XR_FUNC XrXirValueStatus xr_xir_value_admit(const XrXirValue *value, XrXirType type,
                                           XrXirValueAdmission *admission);
/* Successful construction takes the binding lease; failures leave it with the caller. */
XR_FUNC XrXirValueStatus xr_xir_function_new(XrXirDomain *domain, XrXirTypeArena *arena,
    XrXirType type, const XrXirFunctionBinding *binding, XrXirValueAdmission *admission,
    XrXirValue *output);
XR_FUNC const XrXirFunctionBinding *xr_xir_function_binding(const XrXirValue *value);
XR_FUNC XrXirValueStatus xr_xir_cell_new(XrXirDomain *domain, XrXirTypeArena *arena,
    XrXirType type, const XrXirValue *initial, XrXirValueAdmission *admission,
    XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_cell_read(const XrXirValue *cell, XrXirValue *output);
XR_FUNC XrXirValueStatus xr_xir_cell_write(const XrXirValue *cell, const XrXirValue *value,
                                         XrXirValueAdmission *admission);
XR_FUNC bool xr_xir_cell_in_domain(const XrXirValue *cell, XrXirDomain *domain);
#endif // XXIR_VALUE_H
