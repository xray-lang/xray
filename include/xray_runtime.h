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
 * VALUE LAYOUT:
 *   Binary-compatible with the VM's XrValue (16 bytes, tag@0, payload@8).
 *   Tag namespace: XR_TAG_* constants shared with runtime/value/xvalue.h.
 *   Bool: single XR_TAG_BOOL with payload 0=false, 1=true.
 */

#ifndef XRAY_RUNTIME_H
#define XRAY_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "xray_export.h"

/* ========== Tagged Union Value (16 bytes, tag@0, payload@8) ========== */

typedef struct XrValue {
    union {
        struct {
            uint8_t tag;        /* [0]   XR_TAG_* */
            uint8_t flags;      /* [1]   reserved = 0 */
            uint16_t heap_type; /* [2-3] object subtype (PTR only) */
            uint32_t ext;       /* [4-7] reserved = 0 */
        };
        uint64_t descriptor; /* [0-7] bulk load/compare */
    };
    union {
        int64_t i; /* [8-15] integer payload */
        double f;  /* [8-15] float payload */
        void *ptr; /* [8-15] heap pointer */
    };
} XrValue;

/* ========== Value Tags ========== */

#define XR_TAG_NULL 0 /* null singleton */
#define XR_TAG_BOOL 1 /* bool: payload 0=false, 1=true */
#define XR_TAG_I64 3  /* integer (stored in .i as int64) */
#define XR_TAG_F64 4  /* float (stored in .f as double) */
#define XR_TAG_PTR 5  /* generic heap object pointer */
#define XR_TAG_STR 14 /* NUL-terminated C string (const char*) */

/* ========== Singleton Values ========== */

#define XR_NULL_VAL ((XrValue) {.tag = XR_TAG_NULL})
#define XR_TRUE_VAL ((XrValue) {.tag = XR_TAG_BOOL, .i = 1})
#define XR_FALSE_VAL ((XrValue) {.tag = XR_TAG_BOOL, .i = 0})

/* ========== Box / Unbox (inline, zero overhead) ========== */

static inline XrValue xrt_box_int(int64_t v) {
    XrValue r = {0};
    r.tag = XR_TAG_I64;
    r.i = v;
    return r;
}

static inline XrValue xrt_box_float(double v) {
    XrValue r = {0};
    r.tag = XR_TAG_F64;
    r.f = v;
    return r;
}

static inline XrValue xrt_box_bool(int64_t v) {
    XrValue r = {0};
    r.tag = XR_TAG_BOOL;
    r.i = v ? 1 : 0;
    return r;
}

static inline XrValue xrt_box_ptr(void *p) {
    XrValue r = {0};
    r.tag = XR_TAG_PTR;
    r.ptr = p;
    return r;
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
    return v.tag == XR_TAG_NULL;
}

static inline bool xrt_is_int(XrValue v) {
    return v.tag == XR_TAG_I64;
}

static inline bool xrt_is_float(XrValue v) {
    return v.tag == XR_TAG_F64;
}

static inline XrValue xrt_box_str(const char *s) {
    XrValue r = {0};
    r.tag = XR_TAG_STR;
    r.ptr = (void *) s;
    return r;
}

static inline bool xrt_is_str(XrValue v) {
    return v.tag == XR_TAG_STR;
}

static inline const char *xrt_unbox_str(XrValue v) {
    return (const char *) v.ptr;
}

static inline bool xrt_is_bool(XrValue v) {
    return v.tag == XR_TAG_BOOL;
}

static inline bool xrt_to_bool(XrValue v) {
    return v.tag == XR_TAG_BOOL && v.i != 0;
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
