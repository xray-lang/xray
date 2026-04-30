/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_runtime.h - Minimal runtime API for AOT-compiled code
 *
 * KEY CONCEPT:
 *   This header provides the thin API layer between AOT-generated C code
 *   and the xray runtime. Pure-compute functions (int64/float64 only)
 *   don't need this header. Functions using strings, arrays, objects,
 *   or mixed types include this header and link against libxray_rt.
 *
 * WHY THIS DESIGN:
 *   - AOT code should not depend on internal VM structures
 *   - xrt_* functions are thin wrappers around existing xr_* APIs
 *   - Simple operations (box/unbox) are static inline for zero overhead
 */

#ifndef XRAY_RUNTIME_H
#define XRAY_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "xray_export.h"

/* ========== Tagged Union Value (16 bytes) ========== */

typedef struct XrValue {
    union {
        int64_t i;
        double f;
        void *ptr;
    };
    uint32_t tag;
    uint32_t _pad;
} XrValue;

/* ========== Value Tags ========== */

#define XRT_TAG_NULL 0
#define XRT_TAG_TRUE 1
#define XRT_TAG_FALSE 2
#define XRT_TAG_I64 6
#define XRT_TAG_F64 12
#define XRT_TAG_PTR 13
#define XRT_TAG_STR 14  // NUL-terminated C string (const char*)

/* ========== Singleton Values ========== */

#define XRT_NULL ((XrValue){.ptr = NULL, .tag = XRT_TAG_NULL})
#define XRT_TRUE ((XrValue){.i = 1, .tag = XRT_TAG_TRUE})
#define XRT_FALSE ((XrValue){.i = 0, .tag = XRT_TAG_FALSE})

/* ========== Box / Unbox (inline, zero overhead) ========== */

static inline XrValue xrt_box_int(int64_t v) {
    return (XrValue){.i = v, .tag = XRT_TAG_I64};
}

static inline XrValue xrt_box_float(double v) {
    return (XrValue){.f = v, .tag = XRT_TAG_F64};
}

static inline XrValue xrt_box_bool(int64_t v) {
    return v ? XRT_TRUE : XRT_FALSE;
}

static inline XrValue xrt_box_ptr(void *p) {
    return (XrValue){.ptr = p, .tag = XRT_TAG_PTR};
}

static inline int64_t xrt_unbox_int(XrValue v) {
    return v.i;
}

static inline double xrt_unbox_float(XrValue v) {
    return v.f;
}

static inline void *xrt_unbox_ptr(XrValue v) {
    return v.ptr;
}

static inline bool xrt_is_null(XrValue v) {
    return v.tag == XRT_TAG_NULL;
}

static inline bool xrt_is_int(XrValue v) {
    return v.tag == XRT_TAG_I64;
}

static inline bool xrt_is_float(XrValue v) {
    return v.tag == XRT_TAG_F64;
}

static inline XrValue xrt_box_str(const char *s) {
    return (XrValue){.ptr = (void *) s, .tag = XRT_TAG_STR};
}

static inline bool xrt_is_str(XrValue v) {
    return v.tag == XRT_TAG_STR;
}

static inline const char *xrt_unbox_str(XrValue v) {
    return (const char *) v.ptr;
}

static inline bool xrt_is_bool(XrValue v) {
    return v.tag == XRT_TAG_TRUE || v.tag == XRT_TAG_FALSE;
}

static inline bool xrt_to_bool(XrValue v) {
    return v.tag == XRT_TAG_TRUE;
}

/* ========== Mixed-type Arithmetic (requires libxray_rt) ========== */

XRAY_API XrValue xrt_add(XrValue a, XrValue b);
XRAY_API XrValue xrt_sub(XrValue a, XrValue b);
XRAY_API XrValue xrt_mul(XrValue a, XrValue b);
XRAY_API XrValue xrt_div(XrValue a, XrValue b);
XRAY_API XrValue xrt_mod(XrValue a, XrValue b);
XRAY_API XrValue xrt_neg(XrValue a);

/* ========== Mixed-type Comparison ========== */

XRAY_API int64_t xrt_lt(XrValue a, XrValue b);
XRAY_API int64_t xrt_le(XrValue a, XrValue b);
XRAY_API int64_t xrt_eq(XrValue a, XrValue b);

/* ========== String Operations ========== */

XRAY_API XrValue xrt_string_concat(XrValue a, XrValue b);
XRAY_API XrValue xrt_string_len(XrValue s);
XRAY_API XrValue xrt_string_slice(XrValue s, int64_t start, int64_t end);
XRAY_API int64_t xrt_string_eq(XrValue a, XrValue b);

/* ========== Array Operations ========== */

XRAY_API XrValue xrt_array_new(int64_t capacity);
XRAY_API XrValue xrt_array_get(XrValue arr, int64_t idx);
XRAY_API void xrt_array_set(XrValue arr, int64_t idx, XrValue val);
XRAY_API int64_t xrt_array_len(XrValue arr);
XRAY_API void xrt_array_push(XrValue arr, XrValue val);

/* ========== Map Operations ========== */

XRAY_API XrValue xrt_map_new(void);
XRAY_API XrValue xrt_map_get(XrValue map, XrValue key);
XRAY_API void xrt_map_set(XrValue map, XrValue key, XrValue val);
XRAY_API int64_t xrt_map_len(XrValue map);

/* ========== Object Field Access ========== */

XRAY_API XrValue xrt_field_get(XrValue obj, const char *name);
XRAY_API void xrt_field_set(XrValue obj, const char *name, XrValue val);

/* ========== Print ========== */

XRAY_API void xrt_print(XrValue v);
XRAY_API void xrt_println(XrValue v);

/* ========== GC ========== */

XRAY_API void xrt_safepoint(void);
XRAY_API void *xrt_alloc(size_t size);

#endif  // XRAY_RUNTIME_H
