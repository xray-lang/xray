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
 * Value representation — unified with VM's XrValue layout (tag@byte0, payload@byte8).
 *
 * MEMORY LAYOUT (16 bytes, struct-of-unions):
 *   [0]    tag       uint8_t   - type tag (XRT_TAG_*)
 *   [1]    flags     uint8_t   - reserved (0 in AOT)
 *   [2-3]  heap_type uint16_t  - AOT object subtype (PTR only)
 *   [4-7]  ext       uint32_t  - reserved = 0
 *   [8-15] payload   union     - int64 / double / pointer
 *
 * This layout matches runtime/value/xvalue.h's XrValue exactly, enabling
 * shared binary representation between VM, JIT, and AOT paths.
 * The struct-of-unions form eliminates the compound literal bug that
 * existed in the old union-of-structs layout.
 * ========================================================================= */

typedef struct XrtValue {
    union {
        struct {
            uint8_t tag;         /* [0]   XRT_TAG_* */
            uint8_t flags;       /* [1]   reserved = 0 */
            uint16_t heap_type;  /* [2-3] AOT object subtype (PTR only) */
            uint32_t ext;        /* [4-7] reserved = 0 */
        };
        uint64_t descriptor;     /* [0-7] bulk load/compare */
    };
    union {
        int64_t i;   /* [8-15] integer payload (I64) */
        double f;    /* [8-15] float payload (F64) */
        void *ptr;   /* [8-15] heap pointer (PTR/STR_ARC/ARRAY/...) */
    };
} XrtValue;

/* Tag constants — base tags match VM's XrValueTag for binary compatibility.
 * Extended tags (>= 8) are AOT-specific and never appear in VM values. */
#define XRT_TAG_NULL    0   /* null singleton */
#define XRT_TAG_BOOL    1   /* bool: payload 0=false, 1=true */
#define XRT_TAG_I64     3   /* integer (stored in .i as int64) */
#define XRT_TAG_F64     4   /* float (stored in .f as double) */
#define XRT_TAG_PTR     5   /* generic heap object pointer */
#define XRT_TAG_STR    14   /* static / literal string (not heap-allocated) */
#define XRT_TAG_ARRAY  15
#define XRT_TAG_MAP    16
#define XRT_TAG_STRBUF 17
#define XRT_TAG_CLOSURE 18
#define XRT_TAG_STR_ARC 19  /* heap string (bump-allocated via xrt_arc_alloc) */

/* Treat both STR and STR_ARC as strings in generic operations */
#define XRT_IS_STR(v) ((v).tag == XRT_TAG_STR || (v).tag == XRT_TAG_STR_ARC)

/* =========================================================================
 * Boxing / unboxing
 *
 * The struct-of-unions layout allows direct compound literals for all
 * payload types — .tag and .i/.f/.ptr live in different struct members,
 * so there is no union-member conflict.
 * ========================================================================= */

/* Build XrtValue with ptr payload */
static inline XrtValue xrt_mkptr(void *p, uint8_t tag) {
    XrtValue r = {0};
    r.tag = tag;
    r.ptr = p;
    return r;
}

/* Build XrtValue with double payload */
static inline XrtValue xrt_mkf64(double v, uint8_t tag) {
    XrtValue r = {0};
    r.tag = tag;
    r.f = v;
    return r;
}

static inline XrtValue xrt_box_int(int64_t v) {
    XrtValue r = {0};
    r.tag = XRT_TAG_I64;
    r.i = v;
    return r;
}

static inline XrtValue xrt_box_float(double v) {
    XrtValue r = {0};
    r.tag = XRT_TAG_F64;
    r.f = v;
    return r;
}

static inline XrtValue xrt_box_bool(int64_t v) {
    XrtValue r = {0};
    r.tag = XRT_TAG_BOOL;
    r.i = v ? 1 : 0;
    return r;
}

static inline XrtValue xrt_box_str(const char *s) {
    XrtValue r = {0};
    r.tag = XRT_TAG_STR;
    r.ptr = (void *) s;
    return r;
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
 * XrtValue is now ABI-compatible with runtime/value/xvalue.h's XrValue:
 * identical layout (tag@0, payload@8) and base tag numbers (0-7).
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
