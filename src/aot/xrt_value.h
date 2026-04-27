/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_value.h - AOT value representation: tags, boxing/unboxing, string helpers
 */

#ifndef XRT_VALUE_H
#define XRT_VALUE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdarg.h>
#include <math.h>

/* =========================================================================
 * Value representation
 * ========================================================================= */

typedef union XrtValue {
    struct {
        int64_t i;
        uint8_t _pi[7];
        uint8_t tag;
    };
    struct {
        double f;
        uint8_t _pf[7];
        uint8_t _tf;
    };
    struct {
        void *ptr;
        uint32_t heap_type;
        uint8_t _pp[3];
        uint8_t _tp;
    };
    uint8_t raw[16];
} XrtValue;

#define XRT_TAG_NULL 0
#define XRT_TAG_BOOL 1
// 2 reserved (was XRT_TAG_FALSE)
#define XRT_TAG_I64 6
#define XRT_TAG_F64 12
#define XRT_TAG_PTR 13
#define XRT_TAG_STR 14  // static / literal string (not heap-allocated)
#define XRT_TAG_ARRAY 15
#define XRT_TAG_MAP 16
#define XRT_TAG_STRBUF 17
#define XRT_TAG_CLOSURE 18
#define XRT_TAG_STR_ARC 19  // heap string (bump-allocated via xrt_arc_alloc)

// Treat both STR and STR_ARC as strings in generic operations
#define XRT_IS_STR(v) ((v).tag == XRT_TAG_STR || (v).tag == XRT_TAG_STR_ARC)

/* =========================================================================
 * Boxing / unboxing
 *
 * IMPORTANT: XrtValue is a union with 3 anonymous structs. Compound
 * literals like (XrtValue){.f = v, .tag = T} cross struct boundaries —
 * Clang zeros .f because .tag selects a different union member.
 * Always use two-step init or xrt_mkptr/xrt_mkf64 helpers.
 * ========================================================================= */

// Helper: build XrtValue with ptr payload (avoids compound literal bug)
static inline XrtValue xrt_mkptr(void *p, uint8_t tag) {
    XrtValue r;
    r.i = 0;
    r.tag = tag;
    r.ptr = p;
    return r;
}

// Helper: build XrtValue with double payload
static inline XrtValue xrt_mkf64(double v, uint8_t tag) {
    XrtValue r;
    r.i = 0;
    r.tag = tag;
    r.f = v;
    return r;
}

static inline XrtValue xrt_box_int(int64_t v) {
    return (XrtValue){.i = v, .tag = XRT_TAG_I64};
}

static inline XrtValue xrt_box_float(double v) {
    return xrt_mkf64(v, XRT_TAG_F64);
}

static inline XrtValue xrt_box_bool(int64_t v) {
    return (XrtValue){.i = v ? 1 : 0, .tag = XRT_TAG_BOOL};
}

static inline XrtValue xrt_box_str(const char *s) {
    return xrt_mkptr((void *) s, XRT_TAG_STR);
}

static inline int64_t xrt_unbox_int(XrtValue v) {
    return v.i;
}
static inline double xrt_unbox_float(XrtValue v) {
    return v.f;
}
static inline const char *xrt_unbox_str(XrtValue v) {
    return (const char *) v.ptr;
}

/* =========================================================================
 * String helpers
 * ========================================================================= */

static inline const char *xrt_to_cstr(XrtValue v, char *buf, size_t bufsz) {
    switch (v.tag) {
        case XRT_TAG_STR:
        case XRT_TAG_STR_ARC:
            return (const char *) v.ptr;
        case XRT_TAG_I64:
            snprintf(buf, bufsz, "%lld", (long long) v.i);
            return buf;
        case XRT_TAG_F64:
            snprintf(buf, bufsz, "%g", v.f);
            return buf;
        case XRT_TAG_BOOL:
            return v.i ? "true" : "false";
        case XRT_TAG_NULL:
            return "null";
        default:
            snprintf(buf, bufsz, "<object@%p>", v.ptr);
            return buf;
    }
}

/* =========================================================================
 * Truthiness (matches VM semantics: null, false, 0, 0.0 are falsy)
 * ========================================================================= */

static inline int xrt_truthy(XrtValue v) {
    switch (v.tag) {
        case XRT_TAG_NULL:
            return 0;
        case XRT_TAG_BOOL:
            return v.i != 0;
        case XRT_TAG_I64:
            return v.i != 0;
        case XRT_TAG_F64:
            return v.f != 0.0;
        default:
            return 1;  // strings, ptrs, arrays, etc.
    }
}

/* =========================================================================
 * Source-level aliases for standalone AOT output
 *
 * These aliases let AOT-generated code reuse familiar XrValue / XR_TAG_*
 * spellings inside standalone output. They do not imply ABI compatibility
 * with runtime/value/xvalue.h's XrValue: the runtime layout and tag numbers
 * are different.
 * ========================================================================= */

typedef XrtValue XrValue;

// Source-level tag aliases
#define XR_TAG_NULL XRT_TAG_NULL
#define XR_TAG_BOOL XRT_TAG_BOOL
#define XR_TAG_I64 XRT_TAG_I64
#define XR_TAG_F64 XRT_TAG_F64
#define XR_TAG_PTR XRT_TAG_PTR

// Source-level type checks
#define XR_IS_NULL(v) ((v).tag == XR_TAG_NULL)
#define XR_IS_INT(v) ((v).tag == XR_TAG_I64)
#define XR_IS_FLOAT(v) ((v).tag == XR_TAG_F64)
#define XR_IS_NUM(v) (XR_IS_INT(v) || XR_IS_FLOAT(v))

// Source-level value creation
#define XR_FROM_INT(x) ((XrValue){.i = (int64_t) (x), .tag = XR_TAG_I64})
#define XR_FROM_FLOAT(x) ((XrValue){.f = (double) (x), .tag = XR_TAG_F64})
#define XR_FROM_BOOL(x) ((XrValue){.i = (x) ? 1 : 0, .tag = XR_TAG_BOOL})
#define XR_NULL_VAL ((XrValue){.ptr = 0, .tag = XR_TAG_NULL})
#define XR_TRUE_VAL ((XrValue){.i = 1, .tag = XR_TAG_BOOL})
#define XR_FALSE_VAL ((XrValue){.i = 0, .tag = XR_TAG_BOOL})

// Source-level value extraction
#define XR_TO_INT(v) ((v).i)
#define XR_TO_FLOAT(v) ((v).f)

/* =========================================================================
 * Runtime context — opaque handle passed to all AOT functions.
 * Points to XrCoroutine* internally; AOT code never dereferences it.
 * ========================================================================= */

typedef void *XrtContext;

#endif  // XRT_VALUE_H
