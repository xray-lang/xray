/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_value.h - AOT value representation (self-contained for standalone AOT)
 *
 * Unified with runtime/value/xvalue.h:
 *   - Same struct layout (16B, tag@0, payload@8)
 *   - Same tag namespace (XR_TAG_*)
 *   - Same boxing API (XR_FROM_* / XR_TO_*)
 *   - Same truthiness semantics
 *
 * AOT-specific extensions (tags >= 8) encode object types without GC headers.
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
 * XrValue — 16 bytes, struct-of-unions, binary-compatible with VM XrValue.
 *
 * MEMORY LAYOUT:
 *   [0]    tag       uint8_t   XR_TAG_*
 *   [1]    flags     uint8_t   reserved = 0
 *   [2-3]  heap_type uint16_t  object subtype (PTR only)
 *   [4-7]  ext       uint32_t  reserved = 0
 *   [8-15] payload   union     int64 / double / pointer
 * ========================================================================= */

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
        int64_t i; /* [8-15] integer payload (I64) */
        double f;  /* [8-15] float payload (F64) */
        void *ptr; /* [8-15] heap pointer */
    };
} XrValue;

/* =========================================================================
 * Tag constants — base tags (0-7) identical to VM's XrValueTag.
 * Extended tags (>= 8) are AOT-specific: encode object type without GC header.
 * ========================================================================= */

#define XR_TAG_NULL 0       /* null singleton */
#define XR_TAG_BOOL 1       /* bool: payload 0=false, 1=true */
#define XR_TAG_I64 3        /* integer (stored in .i as int64) */
#define XR_TAG_F64 4        /* float (stored in .f as double) */
#define XR_TAG_PTR 5        /* generic heap object pointer */
#define XR_TAG_STRUCT_REF 6 /* stack-allocated struct ref */
#define XR_TAG_NOTFOUND 7   /* sentinel: map lookup miss */

/* AOT extensions — object type encoded in tag (no GC header available) */
#define XR_TAG_STR 14     /* static / literal string (const char*) */
#define XR_TAG_ARRAY 15   /* AOT array */
#define XR_TAG_MAP 16     /* AOT map */
#define XR_TAG_STRBUF 17  /* AOT string builder */
#define XR_TAG_CLOSURE 18 /* AOT closure */
#define XR_TAG_STR_ARC 19 /* bump-allocated string */

/* String type check (both literal and bump-allocated) */
#define XR_IS_STR(v) ((v).tag == XR_TAG_STR || (v).tag == XR_TAG_STR_ARC)

/* =========================================================================
 * Internal helpers — construct XrValue with explicit tag
 * ========================================================================= */

static inline XrValue xr_mkptr(void *p, uint8_t tag) {
    XrValue r = {0};
    r.tag = tag;
    r.ptr = p;
    return r;
}

static inline XrValue xr_mkf64(double v, uint8_t tag) {
    XrValue r = {0};
    r.tag = tag;
    r.f = v;
    return r;
}

/* =========================================================================
 * Boxing / unboxing — XR_FROM_* / XR_TO_* (same API as VM's xvalue.h)
 * ========================================================================= */

#define XR_FROM_INT(x) ((XrValue) {.tag = XR_TAG_I64, .i = (int64_t) (x)})
#define XR_FROM_FLOAT(x) ((XrValue) {.tag = XR_TAG_F64, .f = (double) (x)})
#define XR_FROM_BOOL(x) ((XrValue) {.tag = XR_TAG_BOOL, .i = (x) ? 1 : 0})
#define XR_NULL_VAL ((XrValue) {.tag = XR_TAG_NULL})
#define XR_TRUE_VAL ((XrValue) {.tag = XR_TAG_BOOL, .i = 1})
#define XR_FALSE_VAL ((XrValue) {.tag = XR_TAG_BOOL, .i = 0})

static inline XrValue xr_box_str(const char *s) {
    XrValue r = {0};
    r.tag = XR_TAG_STR;
    r.ptr = (void *) s;
    return r;
}

#define XR_TO_INT(v) ((v).i)
#define XR_TO_FLOAT(v) ((v).f)

static inline const char *xr_unbox_str(XrValue v) {
    return (const char *) v.ptr;
}

/* =========================================================================
 * Type checks
 * ========================================================================= */

#define XR_IS_NULL(v) ((v).tag == XR_TAG_NULL)
#define XR_IS_BOOL(v) ((v).tag == XR_TAG_BOOL)
#define XR_IS_INT(v) ((v).tag == XR_TAG_I64)
#define XR_IS_FLOAT(v) ((v).tag == XR_TAG_F64)
#define XR_IS_FALSE(v) ((v).tag == XR_TAG_BOOL && (v).i == 0)
#define XR_IS_NUM(v) (XR_IS_INT(v) || XR_IS_FLOAT(v))

/* Coerce any numeric/bool value to int64 (for typed array storage).
 * Non-numeric values return 0 in AOT context. */
static inline int64_t xr_value_to_int64_coerce(XrValue v) {
    if (XR_IS_INT(v))
        return v.i;
    if (XR_IS_FLOAT(v))
        return (int64_t) v.f;
    if (XR_IS_BOOL(v))
        return v.i;
    return 0;
}

/* Coerce any numeric/bool value to double (for typed array storage). */
static inline double xr_value_to_f64_coerce(XrValue v) {
    if (XR_IS_FLOAT(v))
        return v.f;
    if (XR_IS_INT(v))
        return (double) v.i;
    if (XR_IS_BOOL(v))
        return (double) v.i;
    return 0.0;
}

/* =========================================================================
 * String helpers
 * ========================================================================= */

static inline const char *xr_to_cstr(XrValue v, char *buf, size_t bufsz) {
    switch (v.tag) {
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            return (const char *) v.ptr;
        case XR_TAG_I64:
            snprintf(buf, bufsz, "%lld", (long long) v.i);
            return buf;
        case XR_TAG_F64:
            snprintf(buf, bufsz, "%g", v.f);
            return buf;
        case XR_TAG_BOOL:
            return v.i ? "true" : "false";
        case XR_TAG_NULL:
            return "null";
        default:
            snprintf(buf, bufsz, "<object@%p>", v.ptr);
            return buf;
    }
}

/* =========================================================================
 * Truthiness (matches VM semantics: null, false, 0, 0.0 are falsy)
 * ========================================================================= */

static inline int xr_truthy(XrValue v) {
    switch (v.tag) {
        case XR_TAG_NULL:
            return 0;
        case XR_TAG_BOOL:
            return v.i != 0;
        case XR_TAG_I64:
            return v.i != 0;
        case XR_TAG_F64:
            return v.f != 0.0;
        default:
            return 1;
    }
}

/* =========================================================================
 * Runtime context — opaque handle passed to all AOT functions.
 * Points to XrCoroutine* internally; AOT code never dereferences it.
 * ========================================================================= */

typedef void *XrtContext;

#endif  // XRT_VALUE_H
