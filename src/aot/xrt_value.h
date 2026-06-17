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

#include "xrt_hash.h"
#include "../shared/xr_float_fmt.h"
#include "../shared/xr_obj_header.h" /* XrObjType ids shared with the VM */

/* =========================================================================
 * Branch expectation and code-layout hints.
 * Error / overflow / grow paths are annotated so the C compiler keeps the
 * hot path straight-line and moves cold blocks out of the way.
 * ========================================================================= */

#if defined(__GNUC__) || defined(__clang__)
#define XR_LIKELY(x) __builtin_expect(!!(x), 1)
#define XR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define XRT_COLD __attribute__((cold))
/* Alignment promise for the optimizer. Only assert alignments that the
 * allocation contract actually guarantees: array element buffers are
 * XRT_DATA_ALIGN (32B) aligned by xrt_coll.h's inline/stack rounding and by
 * XRT_ALLOC_ALIGNED after growth, and xrt_arc_alloc keeps object user data
 * 16-byte aligned (see xrt_arc.h). */
#define XR_ASSUME_ALIGNED(p, n) __builtin_assume_aligned((p), (n))
/* Function purity attributes, emitted only when the AOT prepare pass proved
 * the function effect-free (XaotFuncAttrPlan): CONST touches no memory,
 * PURE reads memory but never writes / throws / suspends. */
#define XRT_FN_CONST __attribute__((const))
#define XRT_FN_PURE __attribute__((pure))
/* Emitted only when the AOT prepare pass proved the pointer unique over its
 * storage (XaotAliasPlan) — the Rust-noalias analogue for generated C. */
#define XRT_RESTRICT __restrict__
#else
#define XR_LIKELY(x) (x)
#define XR_UNLIKELY(x) (x)
#define XRT_COLD
#define XR_ASSUME_ALIGNED(p, n) (p)
#define XRT_FN_CONST
#define XRT_FN_PURE
#if defined(_MSC_VER)
#define XRT_RESTRICT __restrict
#else
#define XRT_RESTRICT
#endif
#endif

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
#define XR_TAG_STRUCT_REF 6 /* AOT native struct reference */
#define XR_TAG_NOTFOUND 7   /* sentinel: map lookup miss */

/* AOT extensions — object type encoded in tag (no GC header available) */
#define XR_TAG_STR 14      /* static / literal string (const char*) */
#define XR_TAG_ARRAY 15    /* AOT array */
#define XR_TAG_MAP 16      /* AOT map */
#define XR_TAG_STRBUF 17   /* AOT string builder */
#define XR_TAG_CLOSURE 18  /* AOT closure */
#define XR_TAG_STR_ARC 19  /* bump-allocated string */
#define XR_TAG_CELL 20     /* AOT mutable closure cell */
#define XR_TAG_TUPLE 21    /* AOT tuple */
#define XR_TAG_SET 22      /* AOT set */
#define XR_TAG_RANGE 23    /* AOT range */
#define XR_TAG_ENUM 24     /* AOT bridged enum key */
#define XR_TAG_ITERATOR 25 /* AOT map/set iterator (for-in over the iterator protocol) */

typedef struct XrAotEnumValueView {
    uint64_t gc_words[2];
    void *klass;
    const char *enum_name;
    const char *member_name;
} XrAotEnumValueView;

static inline const char *xrt_enum_to_cstr(XrValue v, char *buf, size_t bufsz) {
    const XrAotEnumValueView *ev = (const XrAotEnumValueView *) v.ptr;
    if (ev && ev->enum_name && ev->member_name) {
        snprintf(buf, bufsz, "%s.%s", ev->enum_name, ev->member_name);
        return buf;
    }
    snprintf(buf, bufsz, "<enum@%p>", v.ptr);
    return buf;
}

/* Native field tags mirror XrNativeType for standalone generated C. */
#define XR_NATIVE_I64 0
#define XR_NATIVE_F64 1
#define XR_NATIVE_BOOL 2
#define XR_NATIVE_I8 3
#define XR_NATIVE_I16 4
#define XR_NATIVE_I32 5
#define XR_NATIVE_U8 6
#define XR_NATIVE_U16 7
#define XR_NATIVE_U32 8
#define XR_NATIVE_U64 9
#define XR_NATIVE_F32 10
#define XR_NATIVE_STRUCT 11
#define XR_NATIVE_ARRAY 12
#define XR_NATIVE_STRING 13
#define XR_NATIVE_ARRAY_REF 14
#define XR_NATIVE_MAP_REF 15
#define XR_NATIVE_SET_REF 16

/* String type check (both literal and bump-allocated) */
#define XR_IS_STR(v) ((v).tag == XR_TAG_STR || (v).tag == XR_TAG_STR_ARC)

/* Header-bearing container type checks. These containers box as a tagged
 * pointer carrying the XrObjType id in heap_type, identical to the VM, so the
 * same predicate works on both backends. */
#define XR_IS_ARRAY(v) ((v).tag == XR_TAG_PTR && (v).heap_type == XR_TARRAY)
#define XR_IS_MAP(v) ((v).tag == XR_TAG_PTR && (v).heap_type == XR_TMAP)
#define XR_IS_SET(v) ((v).tag == XR_TAG_PTR && (v).heap_type == XR_TSET)

/* =========================================================================
 * String object — every AOT string value points at an xrt_str_t header.
 *
 * XR_TAG_STR marks compiler-interned literals: the header is static const
 * data with the content hash precomputed at C generation time.
 * XR_TAG_STR_ARC marks runtime-allocated strings: header and bytes live in
 * one bump/heap block, `data` points at the trailing bytes.
 *
 * `len` makes length O(1); `hash` caches the content hash for map keys and
 * equality short-circuits (0 = not computed yet; real hashes are never 0).
 * Bytes stay NUL-terminated so C interop (`xr_str_data`) remains free.
 * ========================================================================= */

#define XRT_STR_LITERAL 0x1u

typedef struct {
    int64_t len;   /* byte length, excluding NUL */
    uint32_t hash; /* cached content hash, 0 = unset (literals: precomputed) */
    uint32_t flags;
    char *data; /* NUL-terminated bytes (trailing block for heap strings) */
} xrt_str_t;

static inline xrt_str_t *xr_str_hdr(XrValue v) {
    return (xrt_str_t *) v.ptr;
}

static inline const char *xr_str_data(XrValue v) {
    return ((const xrt_str_t *) v.ptr)->data;
}

/* Writable bytes of a freshly allocated (not yet shared) string. */
static inline char *xr_str_buf(XrValue v) {
    return ((xrt_str_t *) v.ptr)->data;
}

static inline int64_t xr_str_len(XrValue v) {
    return ((const xrt_str_t *) v.ptr)->len;
}

/* Wrap a static literal header into a value. */
static inline XrValue xr_str_lit(const xrt_str_t *hdr) {
    XrValue r = {0};
    r.tag = XR_TAG_STR;
    r.ptr = (void *) hdr;
    return r;
}

static inline XrValue xr_str_value_from_ptr(void *ptr) {
    if (!ptr)
        return (XrValue) {.tag = XR_TAG_NULL};
    const xrt_str_t *hdr = (const xrt_str_t *) ptr;
    XrValue r = {0};
    r.tag = (hdr->flags & XRT_STR_LITERAL) ? XR_TAG_STR : XR_TAG_STR_ARC;
    r.ptr = ptr;
    return r;
}

/* Define a static literal header for a C string literal.  hash stays 0
 * (hand-written headers cannot precompute it); xrt_str_hash recomputes on
 * demand without caching into const storage. */
#define XRT_STR_LIT_DEF(name, s)                                                                   \
    static const xrt_str_t name = {(int64_t) sizeof(s) - 1, 0, XRT_STR_LITERAL, (char *) (s)}

/* Content hash of a string value, cached in the header when writable.
 * The relaxed atomic store keeps concurrent lazy hashing well-defined:
 * every writer stores the same value. */
static inline uint32_t xrt_str_hash(XrValue v) {
    xrt_str_t *h = (xrt_str_t *) v.ptr;
    uint32_t cached = h->hash;
    if (cached)
        return cached;
    uint32_t computed = xrt_str_hash_bytes(h->data, (size_t) h->len);
    if (!(h->flags & XRT_STR_LITERAL))
        __atomic_store_n(&h->hash, computed, __ATOMIC_RELAXED);
    return computed;
}

/* =========================================================================
 * Internal helpers — construct XrValue with explicit tag
 * ========================================================================= */

/* Box a heap pointer as a tagged value with an explicit object subtype.
 * tag is XR_TAG_PTR; heap_type carries the XrObjType id, exactly like the VM.
 * This is the canonical form for the header-bearing container types so both
 * backends discriminate them identically (XR_IS_ARRAY/MAP/SET). */
static inline XrValue xr_mkheap(void *p, uint16_t heap_type) {
    XrValue r = {0};
    r.tag = XR_TAG_PTR;
    r.heap_type = heap_type;
    r.ptr = p;
    return r;
}

/* Box a heap pointer. The three header-bearing container selectors
 * (XR_TAG_ARRAY/MAP/SET) are normalized to the shared tagged-pointer form
 * (tag=PTR, heap_type=XR_T*) the VM also uses, so a boxed container is
 * discriminated identically on both backends; every other tag is stored
 * verbatim. The selector argument is a compile-time constant at all call
 * sites (runtime and generated C), so the switch folds away. */
static inline XrValue xr_mkptr(void *p, uint8_t tag) {
    switch (tag) {
        case XR_TAG_ARRAY:
            return xr_mkheap(p, XR_TARRAY);
        case XR_TAG_MAP:
            return xr_mkheap(p, XR_TMAP);
        case XR_TAG_SET:
            return xr_mkheap(p, XR_TSET);
        default:
            break;
    }
    XrValue r = {0};
    r.tag = tag;
    r.ptr = p;
    return r;
}

/* Logical object-kind tag used by kind-dispatch switches. The header-bearing
 * containers box as XR_TAG_PTR + heap_type, so map them back to their
 * XR_TAG_ARRAY/MAP/SET kind; every other value's physical tag is already its
 * kind. This keeps kind-dispatch switches expressed with their case labels
 * while the physical representation matches the VM's tagged-pointer form. */
static inline uint8_t xrt_value_kind(XrValue v) {
    if (v.tag == XR_TAG_PTR) {
        switch (v.heap_type) {
            case XR_TARRAY:
                return XR_TAG_ARRAY;
            case XR_TMAP:
                return XR_TAG_MAP;
            case XR_TSET:
                return XR_TAG_SET;
            default:
                return XR_TAG_PTR;
        }
    }
    return v.tag;
}

static inline XrValue xr_array_ref(void *ptr, uint8_t elem_native_type, uint16_t elem_count) {
    XrValue r = {0};
    r.tag = XR_TAG_STRUCT_REF;
    r.ext = ((uint32_t) elem_count << 8) | elem_native_type;
    r.ptr = ptr;
    return r;
}

#define XR_IS_ARRAY_REF(v) ((v).tag == XR_TAG_STRUCT_REF && (v).ext != 0)
#define XR_ARRAY_REF_ELEM_TYPE(v) ((uint8_t) ((v).ext & 0xFF))
#define XR_ARRAY_REF_ELEM_COUNT(v) ((uint16_t) ((v).ext >> 8))

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

#define XR_TO_INT(v) ((v).i)
#define XR_TO_FLOAT(v) ((v).f)

static inline const char *xr_unbox_str(XrValue v) {
    return xr_str_data(v);
}

/* =========================================================================
 * Value equality — single authoritative implementation for the AOT runtime.
 * Mirrors the VM's xr_value_eq semantics: strings compare by content
 * (XR_TAG_STR and XR_TAG_STR_ARC are interchangeable), numbers by value,
 * other heap objects by identity. Used by ==, map/set key lookup, and
 * array indexOf/includes — these must never diverge.
 * ========================================================================= */

static inline int64_t xrt_eq(XrValue a, XrValue b) {
    /* Normalize STR_ARC to STR so literal and allocated strings compare. */
    uint32_t ta = (a.tag == XR_TAG_STR_ARC) ? XR_TAG_STR : a.tag;
    uint32_t tb = (b.tag == XR_TAG_STR_ARC) ? XR_TAG_STR : b.tag;
    if (ta != tb)
        return 0;
    if (ta == XR_TAG_ENUM)
        return a.ptr == b.ptr && a.ext == b.ext;
    if (ta == XR_TAG_I64 || ta == XR_TAG_BOOL)
        return a.i == b.i;
    if (ta == XR_TAG_F64)
        return a.f == b.f;
    if (ta == XR_TAG_STR) {
        const xrt_str_t *sa = (const xrt_str_t *) a.ptr;
        const xrt_str_t *sb = (const xrt_str_t *) b.ptr;
        if (sa == sb)
            return 1;
        if (sa->len != sb->len)
            return 0;
        if (sa->hash && sb->hash && sa->hash != sb->hash)
            return 0;
        return memcmp(sa->data, sb->data, (size_t) sa->len) == 0;
    }
    return a.ptr == b.ptr;
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

static inline double xrt_math_number(XrValue v) {
    if (XR_IS_INT(v))
        return (double) v.i;
    if (XR_IS_FLOAT(v))
        return v.f;
    return NAN;
}

static inline XrValue xrt_math_abs(XrValue v) {
    if (XR_IS_INT(v)) {
        int64_t i = v.i;
        if (i == INT64_MIN)
            return XR_FROM_FLOAT((double) INT64_MAX + 1.0);
        return XR_FROM_INT(i < 0 ? -i : i);
    }
    return XR_FROM_FLOAT(fabs(xrt_math_number(v)));
}

/* =========================================================================
 * String helpers
 * ========================================================================= */

static inline int xrt_format_float(char *buf, size_t bufsz, double value) {
    return xr_format_float(buf, bufsz, value);
}

static inline const char *xr_to_cstr(XrValue v, char *buf, size_t bufsz) {
    switch (v.tag) {
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            return xr_str_data(v);
        case XR_TAG_I64:
            snprintf(buf, bufsz, "%lld", (long long) v.i);
            return buf;
        case XR_TAG_F64:
            xrt_format_float(buf, bufsz, v.f);
            return buf;
        case XR_TAG_BOOL:
            return v.i ? "true" : "false";
        case XR_TAG_NULL:
            return "null";
        case XR_TAG_ENUM:
            return xrt_enum_to_cstr(v, buf, bufsz);
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
