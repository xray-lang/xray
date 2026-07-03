/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_core_freestanding.h - no-libc AOT prelude for --profile freestanding
 *
 * This header intentionally stays below hosted xrt.h.  It exposes the value
 * representation and scalar ABI helpers needed by freestanding @c_export
 * code without pulling in malloc, stdio, pthreads, setjmp, libm, or OS APIs.
 */

#ifndef XRT_CORE_FREESTANDING_H
#define XRT_CORE_FREESTANDING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <float.h>
#include <stdatomic.h>

#include "../shared/xr_obj_header.h"

#ifndef XR_FUNC
#define XR_FUNC extern
#endif

#if defined(__GNUC__) || defined(__clang__)
#define XR_LIKELY(x) __builtin_expect(!!(x), 1)
#define XR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define XRT_COLD __attribute__((cold))
#define XRT_NORETURN __attribute__((noreturn))
#define XR_ASSUME_ALIGNED(p, n) __builtin_assume_aligned((p), (n))
#define XRT_FN_CONST __attribute__((const))
#define XRT_FN_PURE __attribute__((pure))
#define XRT_RESTRICT __restrict__
#elif defined(_MSC_VER)
#define XR_LIKELY(x) (x)
#define XR_UNLIKELY(x) (x)
#define XRT_COLD
#define XRT_NORETURN __declspec(noreturn)
#define XR_ASSUME_ALIGNED(p, n) (p)
#define XRT_FN_CONST
#define XRT_FN_PURE
#define XRT_RESTRICT __restrict
#else
#define XR_LIKELY(x) (x)
#define XR_UNLIKELY(x) (x)
#define XRT_COLD
#define XRT_NORETURN
#define XR_ASSUME_ALIGNED(p, n) (p)
#define XRT_FN_CONST
#define XRT_FN_PURE
#define XRT_RESTRICT
#endif

#if defined(__APPLE__)
#define XR_FFI_ASMNAME(s) "_" s
#else
#define XR_FFI_ASMNAME(s) s
#endif

typedef struct XrValue {
    union {
        struct {
            uint8_t tag;
            uint8_t flags;
            uint16_t heap_type;
            uint32_t ext;
        };
        uint64_t descriptor;
    };
    union {
        int64_t i;
        double f;
        void *ptr;
    };
} XrValue;

#define XR_TAG_NULL 0
#define XR_TAG_BOOL 1
#define XR_TAG_CHAR 2
#define XR_TAG_I64 3
#define XR_TAG_F64 4
#define XR_TAG_PTR 5
#define XR_TAG_STRUCT_REF 6
#define XR_TAG_NOTFOUND 7
#define XR_TAG_STR 14
#define XR_TAG_ARRAY 15
#define XR_TAG_MAP 16
#define XR_TAG_STRBUF 17
#define XR_TAG_CLOSURE 18
#define XR_TAG_STR_ARC 19
#define XR_TAG_CELL 20
#define XR_TAG_TUPLE 21
#define XR_TAG_SET 22
#define XR_TAG_RANGE 23
#define XR_TAG_ENUM 24
#define XR_TAG_ITERATOR 25
#define XR_TAG_REGEX 26
#define XR_TAG_DATETIME 27
#define XR_TAG_SYS_MUTEX 28
#define XR_TAG_SYS_RWLOCK 29
#define XR_TAG_SYS_CONDVAR 30
#define XR_TAG_SYS_BARRIER 31
#define XR_TAG_SYS_ONCE 32

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

#define XR_FROM_INT(x) ((XrValue) {.tag = XR_TAG_I64, .i = (int64_t) (x)})
#define XR_FROM_FLOAT(x) ((XrValue) {.tag = XR_TAG_F64, .f = (double) (x)})
#define XR_FROM_BOOL(x) ((XrValue) {.tag = XR_TAG_BOOL, .i = (x) ? 1 : 0})
#define XR_FROM_CHAR(cp) ((XrValue) {.tag = XR_TAG_CHAR, .i = (int64_t) (uint32_t) (cp)})
#define XR_NULL_VAL ((XrValue) {.tag = XR_TAG_NULL})
#define XR_TRUE_VAL ((XrValue) {.tag = XR_TAG_BOOL, .i = 1})
#define XR_FALSE_VAL ((XrValue) {.tag = XR_TAG_BOOL, .i = 0})

#define XR_TO_INT(v) ((v).i)
#define XR_TO_FLOAT(v) ((v).f)
#define XR_TO_CHAR(v) ((uint32_t) (v).i)

#define XR_IS_NULL(v) ((v).tag == XR_TAG_NULL)
#define XR_IS_BOOL(v) ((v).tag == XR_TAG_BOOL)
#define XR_IS_CHAR(v) ((v).tag == XR_TAG_CHAR)
#define XR_IS_INT(v) ((v).tag == XR_TAG_I64)
#define XR_IS_FLOAT(v) ((v).tag == XR_TAG_F64)
#define XR_IS_FALSE(v) ((v).tag == XR_TAG_BOOL && (v).i == 0)
#define XR_IS_NUM(v) (XR_IS_INT(v) || XR_IS_FLOAT(v))
#define XR_IS_STR(v) ((v).tag == XR_TAG_STR || (v).tag == XR_TAG_STR_ARC)
#define XR_IS_ARRAY(v) ((v).tag == XR_TAG_PTR && (v).heap_type == XR_TARRAY)
#define XR_IS_MAP(v) ((v).tag == XR_TAG_PTR && (v).heap_type == XR_TMAP)
#define XR_IS_SET(v) ((v).tag == XR_TAG_PTR && (v).heap_type == XR_TSET)
#define XR_IS_ARRAY_REF(v) ((v).tag == XR_TAG_STRUCT_REF && (v).ext != 0)
#define XR_ARRAY_REF_ELEM_TYPE(v) ((uint8_t) ((v).ext & 0xFF))
#define XR_ARRAY_REF_ELEM_COUNT(v) ((uint16_t) ((v).ext >> 8))

#define XRT_STR_LITERAL 0x1u

typedef struct {
    int64_t len;
    uint32_t hash;
    uint32_t flags;
    char *data;
} xrt_str_t;

static inline xrt_str_t *xr_str_hdr(XrValue v) {
    return (xrt_str_t *) v.ptr;
}

static inline const char *xr_str_data(XrValue v) {
    return ((const xrt_str_t *) v.ptr)->data;
}

static inline char *xr_str_buf(XrValue v) {
    return ((xrt_str_t *) v.ptr)->data;
}

static inline int64_t xr_str_len(XrValue v) {
    return ((const xrt_str_t *) v.ptr)->len;
}

static inline XrValue xr_str_lit(const xrt_str_t *hdr) {
    XrValue r = {0};
    r.tag = XR_TAG_STR;
    r.ptr = (void *) hdr;
    return r;
}

static inline XrValue xr_str_value_from_ptr(void *ptr) {
    if (!ptr)
        return XR_NULL_VAL;
    const xrt_str_t *hdr = (const xrt_str_t *) ptr;
    XrValue r = {0};
    r.tag = (hdr->flags & XRT_STR_LITERAL) ? XR_TAG_STR : XR_TAG_STR_ARC;
    r.ptr = ptr;
    return r;
}

#define XRT_STR_LIT_DEF(name, s)                                                                   \
    static const xrt_str_t name = {(int64_t) sizeof(s) - 1, 0, XRT_STR_LITERAL, (char *) (s)}

static inline XrValue xr_mkheap(void *p, uint16_t heap_type) {
    XrValue r = {0};
    r.tag = XR_TAG_PTR;
    r.heap_type = heap_type;
    r.ptr = p;
    return r;
}

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

static inline XrValue xr_struct_ref(void *p, uint16_t storage_size) {
    XrValue r = {0};
    r.tag = XR_TAG_STRUCT_REF;
    r.heap_type = storage_size;
    r.ptr = p;
    return r;
}

static inline XrValue xr_array_ref(void *ptr, uint8_t elem_native_type, uint16_t elem_count) {
    XrValue r = {0};
    r.tag = XR_TAG_STRUCT_REF;
    r.ext = ((uint32_t) elem_count << 8) | elem_native_type;
    r.ptr = ptr;
    return r;
}

typedef struct XrAotEnumValueView {
    uint64_t gc_words[2];
    void *klass;
    const char *enum_name;
    const char *member_name;
    XrValue raw_value;
    uint32_t member_index;
} XrAotEnumValueView;

typedef struct XrAotAdtValue {
    const char *enum_name;
    const char *member_name;
    int64_t tag;
    uint32_t payload_count;
    XrValue payload0;
} XrAotAdtValue;

static inline XrAotAdtValue xrt_adt_value_zero(void) {
    XrAotAdtValue out = {0};
    out.payload0 = XR_NULL_VAL;
    return out;
}

static inline XrAotAdtValue xrt_adt_value_make(int64_t tag, uint32_t payload_count,
                                               const char *enum_name, const char *member_name,
                                               XrValue payload0) {
    XrAotAdtValue out;
    out.enum_name = enum_name;
    out.member_name = member_name;
    out.tag = tag;
    out.payload_count = payload_count;
    out.payload0 = payload_count > 0 ? payload0 : XR_NULL_VAL;
    return out;
}

typedef struct xrt_closure {
    void *fn;
    int nupvals;
    XrValue upvals[];
} xrt_closure_t;

typedef void *XrtContext;
typedef struct XrAotRuntime XrAotRuntime;
struct XrCoroutine;

typedef enum {
    XR_AOT_RUN_DONE = 0,
    XR_AOT_RUN_BLOCKED,
    XR_AOT_RUN_YIELD,
    XR_AOT_RUN_SPAWN_CHILD,
    XR_AOT_RUN_ERROR,
    XR_AOT_RUN_CANCELLED,
    XR_AOT_RUN_GEN_YIELD
} XrAotRunKind;

typedef struct XrAotResult {
    XrAotRunKind kind;
    XrValue value;
    XrValue error;
    struct XrCoroutine *child;
    bool error_is_value;
} XrAotResult;

typedef struct XrAotVmHostOps {
    XrValue (*get_builtin)(void *host, int32_t index);
    void *reserved[15];
} XrAotVmHostOps;

typedef struct XrAotContext {
    XrAotRuntime *runtime;
    struct XrCoroutine *coro;
    const XrAotVmHostOps *vm_host_ops;
    void *vm_host;
    void *worker;
} XrAotContext;

XRT_COLD XRT_NORETURN void xr_hook_panic(const char *message, size_t len);

static inline size_t xrt_freestanding_strlen(const char *s) {
    size_t len = 0;
    if (!s)
        return 0;
    while (s[len] != '\0') {
        len++;
    }
    return len;
}

static inline XRT_COLD XRT_NORETURN void xrt_freestanding_trap(const char *message) {
    if (!message)
        message = "panic";
    xr_hook_panic(message, xrt_freestanding_strlen(message));
#if defined(__GNUC__) || defined(__clang__)
    __builtin_unreachable();
#else
    for (;;) {
    }
#endif
}

static inline int64_t xrt_i64_add(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a + (uint64_t) b);
}

static inline int64_t xrt_i64_sub(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a - (uint64_t) b);
}

static inline int64_t xrt_i64_mul(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a * (uint64_t) b);
}

static inline int64_t xrt_i64_neg(int64_t a) {
    return (int64_t) (-(uint64_t) a);
}

static inline int64_t xrt_int_div(int64_t a, int64_t b) {
    if (XR_UNLIKELY(b == 0))
        xrt_freestanding_trap("division by zero");
    if (b == -1)
        return xrt_i64_neg(a);
    return a / b;
}

static inline int64_t xrt_int_mod(int64_t a, int64_t b) {
    if (XR_UNLIKELY(b == 0))
        xrt_freestanding_trap("modulo by zero");
    if (b == -1)
        return 0;
    return a % b;
}

static inline int64_t xrt_i64_shl(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a << ((uint64_t) b & 63));
}

static inline int64_t xrt_i64_shr(int64_t a, int64_t b) {
    return a >> ((uint64_t) b & 63);
}

static inline int64_t xrt_i64_shr_u(int64_t a, int64_t b) {
    return (int64_t) ((uint64_t) a >> ((uint64_t) b & 63));
}

static inline double xrt_math_number(XrValue v) {
    if (XR_IS_FLOAT(v))
        return v.f;
    if (XR_IS_INT(v) || XR_IS_BOOL(v) || XR_IS_CHAR(v))
        return (double) v.i;
    return 0.0;
}

static inline XrValue xrt_to_int(XrValue v) {
    if (XR_IS_INT(v))
        return v;
    if (XR_IS_FLOAT(v))
        return XR_FROM_INT((int64_t) v.f);
    if (XR_IS_BOOL(v) || XR_IS_CHAR(v))
        return XR_FROM_INT(v.i);
    return XR_FROM_INT(0);
}

static inline XrValue xrt_to_float(XrValue v) {
    if (XR_IS_FLOAT(v))
        return v;
    return XR_FROM_FLOAT(xrt_math_number(v));
}

static inline XrValue xrt_to_bool(XrValue v) {
    if (XR_IS_BOOL(v))
        return v;
    if (XR_IS_NULL(v))
        return XR_FALSE_VAL;
    if (XR_IS_INT(v) || XR_IS_CHAR(v))
        return XR_FROM_BOOL(v.i != 0);
    if (XR_IS_FLOAT(v))
        return XR_FROM_BOOL(v.f != 0.0);
    return XR_TRUE_VAL;
}

static inline int xr_truthy(XrValue v) {
    return XR_TO_INT(xrt_to_bool(v)) != 0;
}

static inline int64_t xrt_eq(XrValue a, XrValue b) {
    uint32_t ta = (a.tag == XR_TAG_STR_ARC) ? XR_TAG_STR : a.tag;
    uint32_t tb = (b.tag == XR_TAG_STR_ARC) ? XR_TAG_STR : b.tag;
    if (ta != tb)
        return 0;
    if (ta == XR_TAG_I64 || ta == XR_TAG_BOOL || ta == XR_TAG_CHAR)
        return a.i == b.i;
    if (ta == XR_TAG_F64)
        return a.f == b.f;
    if (ta == XR_TAG_NULL)
        return 1;
    return a.ptr == b.ptr && a.ext == b.ext;
}

static inline XrValue xrt_add(XrValue a, XrValue b) {
    if (XR_IS_INT(a) && XR_IS_INT(b))
        return XR_FROM_INT(xrt_i64_add(a.i, b.i));
    return XR_FROM_FLOAT(xrt_math_number(a) + xrt_math_number(b));
}

static inline XrValue xrt_sub(XrValue a, XrValue b) {
    if (XR_IS_INT(a) && XR_IS_INT(b))
        return XR_FROM_INT(xrt_i64_sub(a.i, b.i));
    return XR_FROM_FLOAT(xrt_math_number(a) - xrt_math_number(b));
}

static inline XrValue xrt_mul(XrValue a, XrValue b) {
    if (XR_IS_INT(a) && XR_IS_INT(b))
        return XR_FROM_INT(xrt_i64_mul(a.i, b.i));
    return XR_FROM_FLOAT(xrt_math_number(a) * xrt_math_number(b));
}

static inline XrValue xrt_div(XrValue a, XrValue b) {
    if (XR_IS_INT(a) && XR_IS_INT(b))
        return XR_FROM_INT(xrt_int_div(a.i, b.i));
    return XR_FROM_FLOAT(xrt_math_number(a) / xrt_math_number(b));
}

static inline XrValue xrt_mod(XrValue a, XrValue b) {
    if (XR_IS_INT(a) && XR_IS_INT(b))
        return XR_FROM_INT(xrt_int_mod(a.i, b.i));
    xrt_freestanding_trap("modulo requires integer types");
    return XR_NULL_VAL;
}

static inline XrValue xrt_neg(XrValue a) {
    if (XR_IS_INT(a))
        return XR_FROM_INT(xrt_i64_neg(a.i));
    if (XR_IS_FLOAT(a))
        return XR_FROM_FLOAT(-a.f);
    return XR_FROM_INT(0);
}

static inline void xrt_arc_init(void) {
}

static inline void xrt_bump_destroy(void) {
}

#endif  // XRT_CORE_FREESTANDING_H
