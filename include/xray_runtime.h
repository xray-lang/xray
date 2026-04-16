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

/* ========== Tagged Union Value (16 bytes) ========== */

typedef struct XrtValue {
    union {
        int64_t  i;
        double   f;
        void    *ptr;
    };
    uint32_t tag;
    uint32_t _pad;
} XrtValue;

/* ========== Value Tags ========== */

#define XRT_TAG_NULL   0
#define XRT_TAG_TRUE   1
#define XRT_TAG_FALSE  2
#define XRT_TAG_I64    6
#define XRT_TAG_F64    12
#define XRT_TAG_PTR    13
#define XRT_TAG_STR    14 // NUL-terminated C string (const char*)

/* ========== Singleton Values ========== */

#define XRT_NULL   ((XrtValue){ .ptr = NULL, .tag = XRT_TAG_NULL })
#define XRT_TRUE   ((XrtValue){ .i = 1,      .tag = XRT_TAG_TRUE })
#define XRT_FALSE  ((XrtValue){ .i = 0,      .tag = XRT_TAG_FALSE })

/* ========== Box / Unbox (inline, zero overhead) ========== */

static inline XrtValue xrt_box_int(int64_t v) {
    return (XrtValue){ .i = v, .tag = XRT_TAG_I64 };
}

static inline XrtValue xrt_box_float(double v) {
    return (XrtValue){ .f = v, .tag = XRT_TAG_F64 };
}

static inline XrtValue xrt_box_bool(int64_t v) {
    return v ? XRT_TRUE : XRT_FALSE;
}

static inline XrtValue xrt_box_ptr(void *p) {
    return (XrtValue){ .ptr = p, .tag = XRT_TAG_PTR };
}

static inline int64_t xrt_unbox_int(XrtValue v) {
    return v.i;
}

static inline double xrt_unbox_float(XrtValue v) {
    return v.f;
}

static inline void *xrt_unbox_ptr(XrtValue v) {
    return v.ptr;
}

static inline bool xrt_is_null(XrtValue v) {
    return v.tag == XRT_TAG_NULL;
}

static inline bool xrt_is_int(XrtValue v) {
    return v.tag == XRT_TAG_I64;
}

static inline bool xrt_is_float(XrtValue v) {
    return v.tag == XRT_TAG_F64;
}

static inline XrtValue xrt_box_str(const char *s) {
    return (XrtValue){ .ptr = (void *)s, .tag = XRT_TAG_STR };
}

static inline bool xrt_is_str(XrtValue v) {
    return v.tag == XRT_TAG_STR;
}

static inline const char *xrt_unbox_str(XrtValue v) {
    return (const char *)v.ptr;
}

static inline bool xrt_is_bool(XrtValue v) {
    return v.tag == XRT_TAG_TRUE || v.tag == XRT_TAG_FALSE;
}

static inline bool xrt_to_bool(XrtValue v) {
    return v.tag == XRT_TAG_TRUE;
}

/* ========== Mixed-type Arithmetic (requires libxray_rt) ========== */

XrtValue xrt_add(XrtValue a, XrtValue b);
XrtValue xrt_sub(XrtValue a, XrtValue b);
XrtValue xrt_mul(XrtValue a, XrtValue b);
XrtValue xrt_div(XrtValue a, XrtValue b);
XrtValue xrt_mod(XrtValue a, XrtValue b);
XrtValue xrt_neg(XrtValue a);

/* ========== Mixed-type Comparison ========== */

int64_t xrt_lt(XrtValue a, XrtValue b);
int64_t xrt_le(XrtValue a, XrtValue b);
int64_t xrt_eq(XrtValue a, XrtValue b);

/* ========== String Operations ========== */

XrtValue xrt_string_concat(XrtValue a, XrtValue b);
XrtValue xrt_string_len(XrtValue s);
XrtValue xrt_string_slice(XrtValue s, int64_t start, int64_t end);
int64_t  xrt_string_eq(XrtValue a, XrtValue b);

/* ========== Array Operations ========== */

XrtValue xrt_array_new(int64_t capacity);
XrtValue xrt_array_get(XrtValue arr, int64_t idx);
void     xrt_array_set(XrtValue arr, int64_t idx, XrtValue val);
int64_t  xrt_array_len(XrtValue arr);
void     xrt_array_push(XrtValue arr, XrtValue val);

/* ========== Map Operations ========== */

XrtValue xrt_map_new(void);
XrtValue xrt_map_get(XrtValue map, XrtValue key);
void     xrt_map_set(XrtValue map, XrtValue key, XrtValue val);
int64_t  xrt_map_len(XrtValue map);

/* ========== Object Field Access ========== */

XrtValue xrt_field_get(XrtValue obj, const char *name);
void     xrt_field_set(XrtValue obj, const char *name, XrtValue val);

/* ========== Print ========== */

void xrt_print(XrtValue v);
void xrt_println(XrtValue v);

/* ========== GC ========== */

void  xrt_safepoint(void);
void *xrt_alloc(size_t size);

#endif // XRAY_RUNTIME_H
