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
#include <string.h>

#ifdef memcpy
#undef memcpy
#endif
#ifdef memmove
#undef memmove
#endif
#ifdef memset
#undef memset
#endif
#ifdef memcmp
#undef memcmp
#endif

#include "../shared/xr_obj_header.h"
#include "../shared/xr_elem_type.h"
#include "../shared/xr_arith_core.h"
#include "../shared/xr_bits_core.h"
#include "../shared/xr_int_arith.h" /* xr_i64_*_wrap for int wrapping methods (task 153) */
#include "../shared/xr_sync_core.h"
#include "xrt_method_symbols.h"

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
#define XRT_ATTR_SECTION(name) __attribute__((section(name)))
#define XRT_ATTR_WEAK __attribute__((weak))
#define XRT_ATTR_USED __attribute__((used))
#define XRT_RESTRICT __restrict__
#elif defined(_MSC_VER)
#define XR_LIKELY(x) (x)
#define XR_UNLIKELY(x) (x)
#define XRT_COLD
#define XRT_NORETURN __declspec(noreturn)
#define XR_ASSUME_ALIGNED(p, n) (p)
#define XRT_FN_CONST
#define XRT_FN_PURE
#define XRT_ATTR_SECTION(name)
#define XRT_ATTR_WEAK
#define XRT_ATTR_USED
#define XRT_RESTRICT __restrict
#else
#define XR_LIKELY(x) (x)
#define XR_UNLIKELY(x) (x)
#define XRT_COLD
#define XRT_NORETURN
#define XR_ASSUME_ALIGNED(p, n) (p)
#define XRT_FN_CONST
#define XRT_FN_PURE
#define XRT_ATTR_SECTION(name)
#define XRT_ATTR_WEAK
#define XRT_ATTR_USED
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
#define XR_TAG_AGG_REF 6
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
#define XR_TAG_THREAD 33
#define XR_TAG_BUFFER 34

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
#define XR_NATIVE_VALUE 17

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
#define XR_IS_ARRAY_REF(v) ((v).tag == XR_TAG_AGG_REF && (v).ext != 0)
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

static inline XrValue xr_aggregate_ref(void *p, uint16_t storage_size) {
    XrValue r = {0};
    r.tag = XR_TAG_AGG_REF;
    r.heap_type = storage_size;
    r.ptr = p;
    return r;
}

static inline XrValue xr_array_ref(void *ptr, uint8_t elem_native_type, uint16_t elem_count) {
    XrValue r = {0};
    r.tag = XR_TAG_AGG_REF;
    r.ext = ((uint32_t) elem_count << 8) | elem_native_type;
    r.ptr = ptr;
    return r;
}

typedef struct XrAotEnumBox {
    uint64_t gc_words[2];
    void *klass;
    const char *enum_name;
    const char *member_name;
    uint32_t member_index;
    uint32_t payload_count;
    uint32_t layout_id;
    XrValue payloads[];
} XrAotEnumBox;

static inline uint32_t xrt_enum_value_layout_id(XrValue v) {
    if (v.tag != XR_TAG_ENUM || !v.ptr)
        return 0;
    return ((const XrAotEnumBox *) v.ptr)->layout_id;
}

#define XR_AOT_ENUM_AGG_PAYLOAD_CAP 16u

typedef struct XrAotEnumAggregate {
    const char *enum_name;
    const char *member_name;
    int64_t tag;
    uint32_t payload_count;
    uint32_t layout_id;
    XrValue payloads[XR_AOT_ENUM_AGG_PAYLOAD_CAP];
} XrAotEnumAggregate;

static inline XrAotEnumAggregate xrt_enum_aggregate_zero(void) {
    XrAotEnumAggregate out;
    out.enum_name = NULL;
    out.member_name = NULL;
    out.tag = 0;
    out.payload_count = 0;
    out.layout_id = 0;
    out.payloads[0] = XR_NULL_VAL;
    out.payloads[1] = XR_NULL_VAL;
    out.payloads[2] = XR_NULL_VAL;
    out.payloads[3] = XR_NULL_VAL;
    out.payloads[4] = XR_NULL_VAL;
    out.payloads[5] = XR_NULL_VAL;
    out.payloads[6] = XR_NULL_VAL;
    out.payloads[7] = XR_NULL_VAL;
    out.payloads[8] = XR_NULL_VAL;
    out.payloads[9] = XR_NULL_VAL;
    out.payloads[10] = XR_NULL_VAL;
    out.payloads[11] = XR_NULL_VAL;
    out.payloads[12] = XR_NULL_VAL;
    out.payloads[13] = XR_NULL_VAL;
    out.payloads[14] = XR_NULL_VAL;
    out.payloads[15] = XR_NULL_VAL;
    return out;
}

static inline XrAotEnumAggregate
xrt_enum_aggregate_make(uint32_t layout_id, int64_t tag, uint32_t payload_count,
                        const char *enum_name, const char *member_name, const XrValue *payloads) {
    XrAotEnumAggregate out = xrt_enum_aggregate_zero();
    out.enum_name = enum_name;
    out.member_name = member_name;
    out.tag = tag;
    out.payload_count = payload_count;
    out.layout_id = layout_id;
    uint32_t limit =
        payload_count < XR_AOT_ENUM_AGG_PAYLOAD_CAP ? payload_count : XR_AOT_ENUM_AGG_PAYLOAD_CAP;
    for (uint32_t i = 0; i < limit; i++)
        out.payloads[i] = payloads ? payloads[i] : XR_NULL_VAL;
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

#ifdef XRT_IMPL
XrValue xrt_pending_error = {.tag = XR_TAG_NULL};
XrAotEnumAggregate xrt_pending_enum_error = {0};
int xrt_pending_enum_error_active = 0;
#else
extern XrValue xrt_pending_error;
extern XrAotEnumAggregate xrt_pending_enum_error;
extern int xrt_pending_enum_error_active;
#endif

static inline int xrt_has_pending_error(void) {
    return !XR_IS_NULL(xrt_pending_error) || xrt_pending_enum_error_active;
}

XRT_COLD XRT_NORETURN void xr_hook_panic(const char *message, size_t len);
void xr_hook_write(const char *bytes, size_t len);
void *xr_hook_alloc(size_t size, size_t align);
void xr_hook_free(void *ptr);

typedef struct xrt_buffer_object {
    void *data;
    int64_t length;
    size_t align;
} xrt_buffer_object_t;

#define XRT_FREESTANDING_OBJECT_ALIGN 16u
#define XRT_FREESTANDING_DEFAULT_ALLOC_ALIGN ((size_t) sizeof(void *))
#define XRT_ARC_KIND_BUFFER 10u
#define XRT_ARC_HDR(p) ((XrObjHeader *) ((char *) (p) - sizeof(XrObjHeader)))

static inline bool xrt_freestanding_is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

static inline size_t xrt_freestanding_align_up(size_t value, size_t align) {
    if (align == 0)
        return value;
    return (value + align - 1u) & ~(align - 1u);
}

static inline void *xrt_freestanding_alloc_bytes(size_t size, size_t align) {
    if (size == 0)
        return NULL;
    if (align == 0)
        align = XRT_FREESTANDING_DEFAULT_ALLOC_ALIGN;
    if (!xrt_freestanding_is_power_of_two(align))
        return NULL;
    return xr_hook_alloc(size, align);
}

static inline XrValue xrt_buffer_box(xrt_buffer_object_t *buf) {
    return buf ? xr_mkptr(buf, XR_TAG_BUFFER) : XR_NULL_VAL;
}

static inline int xrt_buffer_is(XrValue value) {
    return value.tag == XR_TAG_BUFFER && value.ptr != NULL;
}

static inline xrt_buffer_object_t *xrt_buffer_ptr(XrValue value) {
    return xrt_buffer_is(value) ? (xrt_buffer_object_t *) value.ptr : NULL;
}

static inline void xrt_buffer_free_data(void *data) {
    if (data)
        xr_hook_free(data);
}

static inline void xrt_buffer_destroy_builtin(void *obj) {
    xrt_buffer_object_t *buf = (xrt_buffer_object_t *) obj;
    if (!buf)
        return;
    xrt_buffer_free_data(buf->data);
    buf->data = NULL;
    buf->length = 0;
    buf->align = 0;
}

static inline void *xrt_freestanding_arc_alloc(size_t obj_size) {
    size_t rounded = xrt_freestanding_align_up(obj_size, XRT_FREESTANDING_OBJECT_ALIGN);
    size_t total = sizeof(XrObjHeader) + rounded;
    XrObjHeader *hdr =
        (XrObjHeader *) xrt_freestanding_alloc_bytes(total, XRT_FREESTANDING_OBJECT_ALIGN);
    if (!hdr)
        return NULL;
    memset(hdr, 0, total);
    hdr->extra = XR_OBJ_HAS_DTOR;
    atomic_store_explicit(&hdr->refcount, XR_RC_INIT, memory_order_relaxed);
    hdr->objsize = total > UINT32_MAX ? UINT32_MAX : (uint32_t) total;
    hdr->_rsv = XRT_ARC_KIND_BUFFER;
    return (char *) hdr + sizeof(XrObjHeader);
}

static inline void xrt_retain(XrValue v) {
    if (!xrt_buffer_is(v))
        return;
    XrObjHeader *hdr = XRT_ARC_HDR(v.ptr);
    int32_t rc = atomic_load_explicit(&hdr->refcount, memory_order_relaxed);
    if (rc == XR_RC_STICKY)
        return;
    atomic_fetch_add_explicit(&hdr->refcount, 1, memory_order_relaxed);
}

static inline bool xrt_rc_claim_release_last(XrObjHeader *hdr) {
    if (!hdr)
        return false;
    for (;;) {
        int32_t rc = atomic_load_explicit(&hdr->refcount, memory_order_acquire);
        if (rc == XR_RC_STICKY)
            return false;
        if (rc > 0) {
            int32_t next = rc - 1;
            if (atomic_compare_exchange_weak_explicit(&hdr->refcount, &rc, next,
                                                      memory_order_acq_rel, memory_order_acquire))
                return false;
            continue;
        }
        if (rc == 0) {
            int32_t next = XR_RC_STICKY;
            if (atomic_compare_exchange_weak_explicit(&hdr->refcount, &rc, next,
                                                      memory_order_acq_rel, memory_order_acquire))
                return true;
            continue;
        }
        return false;
    }
}

static inline void xrt_release(XrValue v) {
    if (!xrt_buffer_is(v))
        return;
    XrObjHeader *hdr = XRT_ARC_HDR(v.ptr);
    if (!xrt_rc_claim_release_last(hdr))
        return;
    xrt_buffer_destroy_builtin(v.ptr);
    xr_hook_free(hdr);
}

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

#ifndef XR_ERR_CMP_CONST_ASSIGN
#define XR_ERR_CMP_CONST_ASSIGN 303
#endif
#ifndef XR_ERR_TYPE_MISMATCH
#define XR_ERR_TYPE_MISMATCH 404
#endif
#ifndef XR_ERR_INDEX_OUT_OF_BOUNDS
#define XR_ERR_INDEX_OUT_OF_BOUNDS 430
#endif
#ifndef XR_ERR_OUT_OF_MEMORY
#define XR_ERR_OUT_OF_MEMORY 441
#endif

static inline XRT_COLD XRT_NORETURN void xrt_throw_error(int code, const char *message) {
    (void) code;
    xrt_freestanding_trap(message && message[0] ? message : "freestanding runtime error");
}

static inline XRT_COLD XRT_NORETURN void xrt_enum_aggregate_shape_fail(const char *what,
                                                                       const char *enum_name) {
    (void) enum_name;
    xrt_freestanding_trap(what && what[0] ? what : "AOT enum aggregate shape mismatch");
}

static inline void xrt_enum_aggregate_check_layout(uint32_t actual_layout_id,
                                                   uint32_t expected_layout_id,
                                                   const char *enum_name) {
    if (actual_layout_id != 0 && expected_layout_id != 0 && actual_layout_id != expected_layout_id)
        xrt_enum_aggregate_shape_fail("enum layout id mismatch", enum_name);
}

static inline void xrt_enum_aggregate_check_payload_count(uint32_t layout_id, uint32_t actual,
                                                          uint32_t expected,
                                                          const char *enum_name) {
    if (layout_id != 0 && actual != expected)
        xrt_enum_aggregate_shape_fail("enum payload count mismatch", enum_name);
}

static inline void xrt_enum_aggregate_check_payload_type(uint32_t layout_id, int ok,
                                                         const char *enum_name) {
    if (layout_id != 0 && !ok)
        xrt_enum_aggregate_shape_fail("enum payload type mismatch", enum_name);
}

static inline void xrt_enum_aggregate_check_known_tag(uint32_t layout_id, const char *enum_name) {
    if (layout_id != 0)
        xrt_enum_aggregate_shape_fail("enum tag mismatch", enum_name);
}

static inline XRT_COLD XRT_NORETURN void xrt_index_oob(int64_t idx, int64_t length) {
    (void) idx;
    (void) length;
    xrt_freestanding_trap("array index out of range");
}

static inline XRT_COLD XRT_NORETURN void xrt_fixed_index_oob(int64_t idx, int64_t length) {
    (void) idx;
    (void) length;
    xrt_freestanding_trap("fixed array index out of range");
}

static inline size_t xrt_value_native_type_size(uint8_t native_type) {
    switch (native_type) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_BOOL:
            return 1;
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
            return 2;
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_F32:
            return 4;
        case XR_NATIVE_VALUE:
            return sizeof(XrValue);
        default:
            return 8;
    }
}

enum {
    XRT_SPAN_FLAG_READONLY = 1u << 0,
};

typedef struct {
    void *data;
    int64_t length;
    void *guard;
    uint8_t elem_type;
    uint8_t elem_size;
    uint8_t elem_tid;
    uint8_t contains_refs;
    uint32_t flags;
} xr_span_t;

static inline xr_span_t xrt_span_empty(void) {
    return (xr_span_t) {NULL, 0, NULL, XR_ELEM_ANY, (uint8_t) sizeof(XrValue), 0, 0, 0};
}

static inline void xrt_array_normalize_slice(int64_t len, int64_t *start, int64_t *end) {
    if (*start < 0)
        *start += len;
    if (*end < 0)
        *end += len;
    if (*start < 0)
        *start = 0;
    if (*end < 0)
        *end = 0;
    if (*start > len)
        *start = len;
    if (*end > len)
        *end = len;
    if (*start > *end)
        *start = *end;
}

static inline xr_span_t xrt_span_from_array_slice(XrValue arr, int64_t start, int64_t end) {
    if (!XR_IS_ARRAY_REF(arr))
        xrt_freestanding_trap("freestanding span slice supports only fixed arrays");
    uint8_t native_type = XR_ARRAY_REF_ELEM_TYPE(arr);
    int64_t len = XR_ARRAY_REF_ELEM_COUNT(arr);
    xrt_array_normalize_slice(len, &start, &end);
    int64_t count = end - start;
    if (count < 0)
        count = 0;
    uint8_t elem_size = (uint8_t) xrt_value_native_type_size(native_type);
    xr_span_t out = {0};
    out.data = (count > 0 && arr.ptr)
                   ? (void *) ((uint8_t *) arr.ptr + (size_t) start * (size_t) elem_size)
                   : arr.ptr;
    out.length = count;
    out.guard = NULL;
    out.elem_type = xr_native_type_to_elem_type(native_type);
    out.elem_size = elem_size ? elem_size : (uint8_t) sizeof(XrValue);
    out.elem_tid = 0;
    out.contains_refs = out.elem_type == XR_ELEM_ANY;
    out.flags = 0;
    return out;
}

static inline xr_span_t xrt_span_from_span_slice(xr_span_t src, int64_t start, int64_t end) {
    xrt_array_normalize_slice(src.length, &start, &end);
    int64_t count = end - start;
    if (count < 0)
        count = 0;
    xr_span_t out = src;
    out.data = (count > 0 && src.data)
                   ? (void *) ((uint8_t *) src.data + (size_t) start * (size_t) src.elem_size)
                   : src.data;
    out.length = count;
    return out;
}

static inline bool xrt_span_is_readonly(xr_span_t span) {
    return (span.flags & XRT_SPAN_FLAG_READONLY) != 0;
}

typedef struct XrArrayCoreRange {
    int64_t start;
    int64_t end;
    int64_t count;
} XrArrayCoreRange;

static inline XrArrayCoreRange xr_array_core_slice_range(int64_t length, int64_t start,
                                                         int64_t end) {
    if (length < 0)
        length = 0;
    if (start < 0)
        start += length;
    if (end < 0)
        end += length;
    if (start < 0)
        start = 0;
    if (start > length)
        start = length;
    if (end < 0)
        end = 0;
    if (end > length)
        end = length;
    if (start > end)
        start = end;
    return (XrArrayCoreRange) {start, end, end - start};
}

static inline int xr_array_core_common_prefix_diff_byte64(uint64_t diff) {
#if defined(__GNUC__) || defined(__clang__)
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&                                    \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return __builtin_clzll(diff) >> 3;
#else
    return __builtin_ctzll(diff) >> 3;
#endif
#else
    int n = 0;
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) &&                                    \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    while (((diff >> ((7 - n) * 8)) & UINT64_C(0xff)) == 0)
        n++;
#else
    while (((diff >> (n * 8)) & UINT64_C(0xff)) == 0)
        n++;
#endif
    return n;
#endif
}

static inline int64_t xr_array_core_bytes_common_prefix_raw(const void *left_data,
                                                            int64_t left_length,
                                                            const void *right_data,
                                                            int64_t right_length) {
    int64_t n = left_length < right_length ? left_length : right_length;
    if (n <= 0)
        return 0;
    const uint8_t *left = (const uint8_t *) left_data;
    const uint8_t *right = (const uint8_t *) right_data;
    int64_t i = 0;
    while (i + 8 <= n) {
        uint64_t lv = 0;
        uint64_t rv = 0;
        memcpy(&lv, left + i, sizeof(lv));
        memcpy(&rv, right + i, sizeof(rv));
        uint64_t diff = lv ^ rv;
        if (diff)
            return i + xr_array_core_common_prefix_diff_byte64(diff);
        i += 8;
    }
    while (i < n && left[i] == right[i])
        i++;
    return i;
}

static inline bool xr_array_core_memory_ranges_overlap(const void *a, int64_t a_len, const void *b,
                                                       int64_t b_len) {
    if (!a || !b || a_len <= 0 || b_len <= 0)
        return false;
    uintptr_t a_begin = (uintptr_t) a;
    uintptr_t b_begin = (uintptr_t) b;
    uintptr_t a_end = a_begin + (uintptr_t) a_len;
    uintptr_t b_end = b_begin + (uintptr_t) b_len;
    if (a_end < a_begin || b_end < b_begin)
        return true;
    return a_begin < b_end && b_begin < a_end;
}

static inline void xr_array_core_copy_nonoverlap_bytes(void *dst, const void *src, int64_t count) {
    if (count <= 0)
        return;
    if (count <= 16) {
        uint8_t *dp = (uint8_t *) dst;
        const uint8_t *sp = (const uint8_t *) src;
        if (count >= 8) {
            uint64_t first = 0;
            memcpy(&first, sp, sizeof(first));
            memcpy(dp, &first, sizeof(first));
            if (count > 8) {
                uint64_t last = 0;
                memcpy(&last, sp + count - 8, sizeof(last));
                memcpy(dp + count - 8, &last, sizeof(last));
            }
            return;
        }
        if (count >= 4) {
            uint32_t first = 0;
            memcpy(&first, sp, sizeof(first));
            memcpy(dp, &first, sizeof(first));
            if (count > 4) {
                uint32_t last = 0;
                memcpy(&last, sp + count - 4, sizeof(last));
                memcpy(dp + count - 4, &last, sizeof(last));
            }
            return;
        }
        if (count >= 2) {
            uint16_t first = 0;
            memcpy(&first, sp, sizeof(first));
            memcpy(dp, &first, sizeof(first));
            if (count > 2)
                dp[2] = sp[2];
            return;
        }
        dp[0] = sp[0];
        return;
    }
    memcpy(dst, src, (size_t) count);
}

static inline void xr_array_core_copy_or_move_bytes(void *dst, const void *src, int64_t count) {
    if (count <= 0)
        return;
    if (xr_array_core_memory_ranges_overlap(dst, count, src, count))
        memmove(dst, src, (size_t) count);
    else
        xr_array_core_copy_nonoverlap_bytes(dst, src, count);
}

static inline uint64_t xr_array_core_repeat_pattern64(const uint8_t *sp, int64_t distance) {
    uint8_t pattern[8];
    switch (distance) {
        case 2:
            pattern[0] = sp[0];
            pattern[1] = sp[1];
            pattern[2] = sp[0];
            pattern[3] = sp[1];
            pattern[4] = sp[0];
            pattern[5] = sp[1];
            pattern[6] = sp[0];
            pattern[7] = sp[1];
            break;
        case 4:
            pattern[0] = sp[0];
            pattern[1] = sp[1];
            pattern[2] = sp[2];
            pattern[3] = sp[3];
            pattern[4] = sp[0];
            pattern[5] = sp[1];
            pattern[6] = sp[2];
            pattern[7] = sp[3];
            break;
        default:
            memcpy(pattern, sp, sizeof(pattern));
            break;
    }
    uint64_t value = 0;
    memcpy(&value, pattern, sizeof(value));
    return value;
}

static inline void xr_array_core_bytes_repeat_copy(void *data, int64_t dst_offset, int64_t distance,
                                                   int64_t count) {
    if (count <= 0)
        return;
    uint8_t *dp = (uint8_t *) data + dst_offset;
    const uint8_t *sp = dp - distance;
    if (distance == 1) {
        memset(dp, sp[0], (size_t) count);
        return;
    }
    if (count <= distance) {
        xr_array_core_copy_nonoverlap_bytes(dp, sp, count);
        return;
    }
    if (distance == 2 || distance == 4 || distance == 8) {
        uint64_t pattern = xr_array_core_repeat_pattern64(sp, distance);
        int64_t copied = 0;
        for (; copied + 8 <= count; copied += 8)
            memcpy(dp + copied, &pattern, sizeof(pattern));
        if (copied < count)
            memcpy(dp + copied, &pattern, (size_t) (count - copied));
        return;
    }
    if (distance < 8) {
        xr_array_core_copy_nonoverlap_bytes(dp, sp, distance);
        int64_t copied = distance;
        while (copied < count) {
            int64_t chunk = copied;
            int64_t remaining = count - copied;
            if (chunk > remaining)
                chunk = remaining;
            xr_array_core_copy_nonoverlap_bytes(dp + copied, dp, chunk);
            copied += chunk;
        }
        return;
    }
    if (count <= 16) {
        xr_array_core_copy_nonoverlap_bytes(dp, sp, count);
        return;
    }
    xr_array_core_copy_nonoverlap_bytes(dp, sp, distance);
    int64_t copied = distance;
    while (copied < count) {
        int64_t chunk = copied;
        int64_t remaining = count - copied;
        if (chunk > remaining)
            chunk = remaining;
        xr_array_core_copy_nonoverlap_bytes(dp + copied, dp, chunk);
        copied += chunk;
    }
}

enum {
    XR_ENDIAN_NATIVE = 0,
    XR_ENDIAN_LE = 1,
    XR_ENDIAN_BE = 2,
};

static inline bool xrt_freestanding_host_is_little_endian(void) {
    const uint16_t one = 1;
    return *((const uint8_t *) &one) == 1;
}

static inline bool xrt_freestanding_endian_matches_host(int64_t endian) {
    if (endian == XR_ENDIAN_NATIVE)
        return true;
    return (endian == XR_ENDIAN_LE) == xrt_freestanding_host_is_little_endian();
}

static inline bool xrt_freestanding_bytes_range_ok(int64_t length, uint8_t elem_type,
                                                   int64_t offset, int64_t width) {
    return elem_type == XR_ELEM_U8 && length >= 0 && offset >= 0 && width >= 0 &&
           offset <= length && width <= length - offset;
}

static inline uint16_t xrt_freestanding_bswap16(uint16_t value) {
    return (uint16_t) ((value >> 8) | (value << 8));
}

static inline uint32_t xrt_freestanding_bswap32(uint32_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(value);
#else
    return ((value & UINT32_C(0x000000ff)) << 24) | ((value & UINT32_C(0x0000ff00)) << 8) |
           ((value & UINT32_C(0x00ff0000)) >> 8) | ((value & UINT32_C(0xff000000)) >> 24);
#endif
}

static inline uint64_t xrt_freestanding_bswap64(uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(value);
#else
    return ((value & UINT64_C(0x00000000000000ff)) << 56) |
           ((value & UINT64_C(0x000000000000ff00)) << 40) |
           ((value & UINT64_C(0x0000000000ff0000)) << 24) |
           ((value & UINT64_C(0x00000000ff000000)) << 8) |
           ((value & UINT64_C(0x000000ff00000000)) >> 8) |
           ((value & UINT64_C(0x0000ff0000000000)) >> 24) |
           ((value & UINT64_C(0x00ff000000000000)) >> 40) |
           ((value & UINT64_C(0xff00000000000000)) >> 56);
#endif
}

static inline uint16_t xr_array_core_bytes_load_u16(const void *data, int64_t length,
                                                    uint8_t elem_type, int64_t offset,
                                                    int64_t endian, bool *ok) {
    bool valid = data && xrt_freestanding_bytes_range_ok(length, elem_type, offset, 2);
    if (ok)
        *ok = valid;
    if (!valid)
        return 0;
    uint16_t value = 0;
    memcpy(&value, (const uint8_t *) data + offset, sizeof(value));
    return xrt_freestanding_endian_matches_host(endian) ? value : xrt_freestanding_bswap16(value);
}

static inline uint32_t xr_array_core_bytes_load_u32(const void *data, int64_t length,
                                                    uint8_t elem_type, int64_t offset,
                                                    int64_t endian, bool *ok) {
    bool valid = data && xrt_freestanding_bytes_range_ok(length, elem_type, offset, 4);
    if (ok)
        *ok = valid;
    if (!valid)
        return 0;
    uint32_t value = 0;
    memcpy(&value, (const uint8_t *) data + offset, sizeof(value));
    return xrt_freestanding_endian_matches_host(endian) ? value : xrt_freestanding_bswap32(value);
}

static inline uint64_t xr_array_core_bytes_load_u64(const void *data, int64_t length,
                                                    uint8_t elem_type, int64_t offset,
                                                    int64_t endian, bool *ok) {
    bool valid = data && xrt_freestanding_bytes_range_ok(length, elem_type, offset, 8);
    if (ok)
        *ok = valid;
    if (!valid)
        return 0;
    uint64_t value = 0;
    memcpy(&value, (const uint8_t *) data + offset, sizeof(value));
    return xrt_freestanding_endian_matches_host(endian) ? value : xrt_freestanding_bswap64(value);
}

static inline float xr_array_core_bytes_load_f32(const void *data, int64_t length,
                                                 uint8_t elem_type, int64_t offset, int64_t endian,
                                                 bool *ok) {
    uint32_t bits = xr_array_core_bytes_load_u32(data, length, elem_type, offset, endian, ok);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline double xr_array_core_bytes_load_f64(const void *data, int64_t length,
                                                  uint8_t elem_type, int64_t offset, int64_t endian,
                                                  bool *ok) {
    uint64_t bits = xr_array_core_bytes_load_u64(data, length, elem_type, offset, endian, ok);
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline uint16_t xr_array_core_bytes_load_u16_le(const void *data, int64_t length,
                                                       uint8_t elem_type, int64_t offset,
                                                       bool *ok) {
    return xr_array_core_bytes_load_u16(data, length, elem_type, offset, XR_ENDIAN_LE, ok);
}

static inline uint32_t xr_array_core_bytes_load_u32_le(const void *data, int64_t length,
                                                       uint8_t elem_type, int64_t offset,
                                                       bool *ok) {
    return xr_array_core_bytes_load_u32(data, length, elem_type, offset, XR_ENDIAN_LE, ok);
}

static inline uint64_t xr_array_core_bytes_load_u64_le(const void *data, int64_t length,
                                                       uint8_t elem_type, int64_t offset,
                                                       bool *ok) {
    return xr_array_core_bytes_load_u64(data, length, elem_type, offset, XR_ENDIAN_LE, ok);
}

static inline bool xr_array_core_bytes_store_u16(void *data, int64_t length, uint8_t elem_type,
                                                 int64_t offset, uint16_t value, int64_t endian) {
    if (!data || !xrt_freestanding_bytes_range_ok(length, elem_type, offset, 2))
        return false;
    if (!xrt_freestanding_endian_matches_host(endian))
        value = xrt_freestanding_bswap16(value);
    memcpy((uint8_t *) data + offset, &value, sizeof(value));
    return true;
}

static inline bool xr_array_core_bytes_store_u32(void *data, int64_t length, uint8_t elem_type,
                                                 int64_t offset, uint32_t value, int64_t endian) {
    if (!data || !xrt_freestanding_bytes_range_ok(length, elem_type, offset, 4))
        return false;
    if (!xrt_freestanding_endian_matches_host(endian))
        value = xrt_freestanding_bswap32(value);
    memcpy((uint8_t *) data + offset, &value, sizeof(value));
    return true;
}

static inline bool xr_array_core_bytes_store_u64(void *data, int64_t length, uint8_t elem_type,
                                                 int64_t offset, uint64_t value, int64_t endian) {
    if (!data || !xrt_freestanding_bytes_range_ok(length, elem_type, offset, 8))
        return false;
    if (!xrt_freestanding_endian_matches_host(endian))
        value = xrt_freestanding_bswap64(value);
    memcpy((uint8_t *) data + offset, &value, sizeof(value));
    return true;
}

static inline bool xr_array_core_bytes_store_f32(void *data, int64_t length, uint8_t elem_type,
                                                 int64_t offset, float value, int64_t endian) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return xr_array_core_bytes_store_u32(data, length, elem_type, offset, bits, endian);
}

static inline bool xr_array_core_bytes_store_f64(void *data, int64_t length, uint8_t elem_type,
                                                 int64_t offset, double value, int64_t endian) {
    uint64_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return xr_array_core_bytes_store_u64(data, length, elem_type, offset, bits, endian);
}

static inline int64_t xrt_span_bytes_load_u16_le_unchecked_raw(xr_span_t span, int64_t off) {
    bool ok = false;
    return (int64_t) xr_array_core_bytes_load_u16_le(span.data, span.length, span.elem_type, off,
                                                     &ok);
}

static inline int64_t xrt_span_bytes_load_u32_le_unchecked_raw(xr_span_t span, int64_t off) {
    bool ok = false;
    return (int64_t) xr_array_core_bytes_load_u32_le(span.data, span.length, span.elem_type, off,
                                                     &ok);
}

static inline int64_t xrt_span_bytes_load_u64_le_unchecked_raw(xr_span_t span, int64_t off) {
    bool ok = false;
    return (int64_t) xr_array_core_bytes_load_u64_le(span.data, span.length, span.elem_type, off,
                                                     &ok);
}

static inline int64_t xr_value_to_int64_coerce(XrValue v) {
    if (XR_IS_INT(v) || XR_IS_CHAR(v) || XR_IS_BOOL(v))
        return v.i;
    if (XR_IS_FLOAT(v))
        return (int64_t) v.f;
    return 0;
}

static inline double xr_value_to_f64_coerce(XrValue v) {
    if (XR_IS_FLOAT(v))
        return v.f;
    if (XR_IS_INT(v) || XR_IS_CHAR(v) || XR_IS_BOOL(v))
        return (double) v.i;
    return 0.0;
}

static inline XrValue xrt_fixed_array_get(void *base, uint8_t native_type, int64_t idx) {
    uint8_t *p = (uint8_t *) base + (size_t) idx * xrt_value_native_type_size(native_type);
    switch (native_type) {
        case XR_NATIVE_F32:
            return XR_FROM_FLOAT((double) *(float *) p);
        case XR_NATIVE_F64:
            return XR_FROM_FLOAT(*(double *) p);
        case XR_NATIVE_BOOL:
            return *(uint8_t *) p ? XR_TRUE_VAL : XR_FALSE_VAL;
        case XR_NATIVE_VALUE:
            return *(XrValue *) p;
        case XR_NATIVE_I8:
            return XR_FROM_INT((int64_t) *(int8_t *) p);
        case XR_NATIVE_I16:
            return XR_FROM_INT((int64_t) *(int16_t *) p);
        case XR_NATIVE_I32:
            return XR_FROM_INT((int64_t) *(int32_t *) p);
        case XR_NATIVE_U8:
            return XR_FROM_INT((int64_t) *(uint8_t *) p);
        case XR_NATIVE_U16:
            return XR_FROM_INT((int64_t) *(uint16_t *) p);
        case XR_NATIVE_U32:
            return XR_FROM_INT((int64_t) *(uint32_t *) p);
        case XR_NATIVE_U64:
            return XR_FROM_INT((int64_t) *(uint64_t *) p);
        default:
            return XR_FROM_INT(*(int64_t *) p);
    }
}

static inline void xrt_fixed_array_set(void *base, uint8_t native_type, int64_t idx,
                                       XrValue value) {
    uint8_t *p = (uint8_t *) base + (size_t) idx * xrt_value_native_type_size(native_type);
    switch (native_type) {
        case XR_NATIVE_F32:
            *(float *) p = (float) xr_value_to_f64_coerce(value);
            break;
        case XR_NATIVE_F64:
            *(double *) p = xr_value_to_f64_coerce(value);
            break;
        case XR_NATIVE_BOOL:
            *(uint8_t *) p = (uint8_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_VALUE:
            *(XrValue *) p = value;
            break;
        case XR_NATIVE_I8:
            *(int8_t *) p = (int8_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_I16:
            *(int16_t *) p = (int16_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_I32:
            *(int32_t *) p = (int32_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_U8:
            *(uint8_t *) p = (uint8_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_U16:
            *(uint16_t *) p = (uint16_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_U32:
            *(uint32_t *) p = (uint32_t) xr_value_to_int64_coerce(value);
            break;
        case XR_NATIVE_U64:
            *(uint64_t *) p = (uint64_t) xr_value_to_int64_coerce(value);
            break;
        default:
            *(int64_t *) p = xr_value_to_int64_coerce(value);
            break;
    }
}

static inline XrValue xrt_index_get(XrValue obj, XrValue key) {
    if (XR_IS_ARRAY_REF(obj) && XR_IS_INT(key)) {
        int64_t idx = key.i;
        uint16_t count = XR_ARRAY_REF_ELEM_COUNT(obj);
        if (XR_LIKELY(idx >= 0 && idx < count))
            return xrt_fixed_array_get(obj.ptr, XR_ARRAY_REF_ELEM_TYPE(obj), idx);
        xrt_fixed_index_oob(idx, count);
    }
    if (obj.tag == XR_TAG_ENUM && XR_IS_INT(key)) {
        const XrAotEnumBox *ev = (const XrAotEnumBox *) obj.ptr;
        if (!ev)
            return XR_NULL_VAL;
        if (key.i == 0)
            return XR_FROM_INT(ev->member_index);
        if (key.i > 0 && (uint32_t) key.i <= ev->payload_count)
            return ev->payloads[key.i - 1];
    }
    xrt_freestanding_trap("freestanding index get supports only fixed arrays");
    return XR_NULL_VAL;
}

static inline XrValue xrt_enum_field_get(XrValue boxed, int64_t index) {
    if (boxed.tag != XR_TAG_ENUM || !boxed.ptr)
        return XR_NULL_VAL;
    const XrAotEnumBox *ev = (const XrAotEnumBox *) boxed.ptr;
    if (index == 0)
        return XR_FROM_INT(ev->member_index);
    if (index > 0 && (uint32_t) index <= ev->payload_count)
        return ev->payloads[index - 1];
    return XR_NULL_VAL;
}

static inline XrValue xrt_enum_box_ordinal(XrValue obj) {
    if (XR_IS_INT(obj))
        return obj;
    if (obj.tag != XR_TAG_ENUM || !obj.ptr)
        return XR_FROM_INT(-1);
    const XrAotEnumBox *ev = (const XrAotEnumBox *) obj.ptr;
    return XR_FROM_INT(ev->member_index);
}

static inline void xrt_index_set(XrValue obj, XrValue key, XrValue val) {
    if (XR_IS_ARRAY_REF(obj) && XR_IS_INT(key)) {
        int64_t idx = key.i;
        uint16_t count = XR_ARRAY_REF_ELEM_COUNT(obj);
        if (XR_LIKELY(idx >= 0 && idx < count)) {
            xrt_fixed_array_set(obj.ptr, XR_ARRAY_REF_ELEM_TYPE(obj), idx, val);
            return;
        }
        xrt_fixed_index_oob(idx, count);
    }
    xrt_freestanding_trap("freestanding index set supports only fixed arrays");
}

static inline void xrt_write_bytes(const char *bytes, size_t len) {
    if (bytes && len > 0)
        xr_hook_write(bytes, len);
}

static inline void xrt_write_char(char c) {
    xr_hook_write(&c, 1);
}

static inline void xrt_write_cstr(const char *s) {
    xrt_write_bytes(s, xrt_freestanding_strlen(s));
}

static inline void xrt_print_u64(uint64_t value) {
    char buf[20];
    size_t pos = sizeof(buf);
    do {
        buf[--pos] = (char) ('0' + (value % 10u));
        value /= 10u;
    } while (value != 0);
    xrt_write_bytes(buf + pos, sizeof(buf) - pos);
}

static inline void xrt_print_i64(int64_t value) {
    if (value < 0) {
        xrt_write_char('-');
        xrt_print_u64((uint64_t) (-(value + 1)) + 1u);
    } else {
        xrt_print_u64((uint64_t) value);
    }
}

static inline void xrt_print_char(uint32_t cp) {
    char buf[4];
    size_t len = 0;
    if (cp <= 0x7Fu) {
        buf[len++] = (char) cp;
    } else if (cp <= 0x7FFu) {
        buf[len++] = (char) (0xC0u | (cp >> 6));
        buf[len++] = (char) (0x80u | (cp & 0x3Fu));
    } else if (cp <= 0xFFFFu) {
        buf[len++] = (char) (0xE0u | (cp >> 12));
        buf[len++] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        buf[len++] = (char) (0x80u | (cp & 0x3Fu));
    } else {
        buf[len++] = (char) (0xF0u | (cp >> 18));
        buf[len++] = (char) (0x80u | ((cp >> 12) & 0x3Fu));
        buf[len++] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
        buf[len++] = (char) (0x80u | (cp & 0x3Fu));
    }
    xrt_write_bytes(buf, len);
}

static inline void xrt_print(XrValue v) {
    if (XR_IS_INT(v)) {
        xrt_print_i64(v.i);
    } else if (XR_IS_BOOL(v)) {
        xrt_write_cstr(v.i ? "true" : "false");
    } else if (XR_IS_NULL(v)) {
        xrt_write_cstr("null");
    } else if (XR_IS_CHAR(v)) {
        xrt_print_char((uint32_t) v.i);
    } else if (XR_IS_STR(v)) {
        xrt_write_bytes(xr_str_data(v), (size_t) xr_str_len(v));
    } else {
        xrt_freestanding_trap("freestanding print supports only scalar/string values");
    }
}

static inline void xrt_println(XrValue v) {
    xrt_print(v);
    xrt_write_char('\n');
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
    if (ta == XR_TAG_ENUM) {
        const XrAotEnumBox *ea = (const XrAotEnumBox *) a.ptr;
        const XrAotEnumBox *eb = (const XrAotEnumBox *) b.ptr;
        if (!ea || !eb)
            return ea == eb;
        if (ea->layout_id != eb->layout_id || ea->member_index != eb->member_index ||
            ea->payload_count != eb->payload_count)
            return 0;
        for (uint32_t i = 0; i < ea->payload_count; i++) {
            if (!xrt_eq(ea->payloads[i], eb->payloads[i]))
                return 0;
        }
        return 1;
    }
    return a.ptr == b.ptr && a.ext == b.ext;
}

static inline int64_t xrt_lt(XrValue a, XrValue b) {
    if (XR_IS_INT(a) && XR_IS_INT(b))
        return a.i < b.i;
    return xrt_math_number(a) < xrt_math_number(b);
}

static inline int64_t xrt_le(XrValue a, XrValue b) {
    if (XR_IS_INT(a) && XR_IS_INT(b))
        return a.i <= b.i;
    return xrt_math_number(a) <= xrt_math_number(b);
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

static inline int64_t xrt_mem_int_arg(XrValue v) {
    return XR_IS_INT(v) ? XR_TO_INT(v) : 0;
}

/* Bit intrinsics + wrapping/overflow arithmetic are `int` methods now
 * (task 153). The freestanding profile still gets them zero-overhead: the
 * cgen lowers x.popcount()/x.rotateLeft(n)/x.addOverflows(y)/... straight
 * to xr_bits_core_* / xr_arith_core_* / xr_i64_*, and this header already
 * includes those shared cores above. The old boxed mem.* wrappers were
 * deleted along with the core.def mem entries. */

static inline XrValue xrt_mem_fence(XrValue ordering) {
    xr_sync_core_fence(xrt_mem_int_arg(ordering));
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_prefetch(XrValue ptr, XrValue rw) {
#if defined(__GNUC__) || defined(__clang__)
    if (xrt_mem_int_arg(rw) != 0)
        __builtin_prefetch(ptr.ptr, 1, 3);
    else
        __builtin_prefetch(ptr.ptr, 0, 3);
#else
    (void) ptr;
    (void) rw;
#endif
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_cache_line_size(void) {
    return XR_FROM_INT(64);
}

static inline XrValue xrt_buffer_new(int64_t length, int zeroed, size_t align) {
    if (length < 0)
        length = 0;
    if (align == 0)
        align = XRT_FREESTANDING_DEFAULT_ALLOC_ALIGN;
    if (!xrt_freestanding_is_power_of_two(align) || align < sizeof(void *))
        xrt_freestanding_trap("mem.allocAligned: align must be a power of two >= sizeof(void*)");

    xrt_buffer_object_t *buf =
        (xrt_buffer_object_t *) xrt_freestanding_arc_alloc(sizeof(xrt_buffer_object_t));
    if (!buf)
        xrt_freestanding_trap("mem.alloc: out of memory");
    buf->data = NULL;
    buf->length = length;
    buf->align = align;

    if (length > 0) {
        size_t size = (size_t) length;
        buf->data = xrt_freestanding_alloc_bytes(size, align);
        if (!buf->data) {
            xr_hook_free(XRT_ARC_HDR(buf));
            xrt_freestanding_trap("mem.alloc: out of memory");
        }
        if (zeroed)
            memset(buf->data, 0, size);
    }
    return xrt_buffer_box(buf);
}

static inline xr_span_t xrt_buffer_as_span(XrValue value) {
    xrt_buffer_object_t *buf = xrt_buffer_ptr(value);
    if (!buf)
        return xrt_span_empty();
    xr_span_t out = {0};
    out.data = buf->data;
    out.length = buf->length;
    out.guard = buf;
    out.elem_type = XR_ELEM_U8;
    out.elem_size = 1;
    out.elem_tid = 0;
    out.contains_refs = 0;
    out.flags = 0;
    return out;
}

static inline XrValue xrt_buffer_ptr_unchecked(XrValue value) {
    xrt_buffer_object_t *buf = xrt_buffer_ptr(value);
    return xr_mkptr(buf ? buf->data : NULL, XR_TAG_PTR);
}

static inline XrValue xrt_buffer_resize(XrValue value, XrValue size_value) {
    xrt_buffer_object_t *buf = xrt_buffer_ptr(value);
    int64_t new_len = xrt_mem_int_arg(size_value);
    if (!buf || new_len < 0)
        return XR_FALSE_VAL;
    if (new_len == 0) {
        xrt_buffer_free_data(buf->data);
        buf->data = NULL;
        buf->length = 0;
        return XR_TRUE_VAL;
    }

    void *new_data = xrt_freestanding_alloc_bytes((size_t) new_len, buf->align);
    if (!new_data)
        return XR_FALSE_VAL;
    size_t copy = (size_t) ((buf->length < new_len) ? buf->length : new_len);
    if (copy > 0 && buf->data)
        memcpy(new_data, buf->data, copy);
    xrt_buffer_free_data(buf->data);
    buf->data = new_data;
    buf->length = new_len;
    return XR_TRUE_VAL;
}

static inline XrValue xrt_buffer_method_0(XrValue recv, int sym) {
    xrt_buffer_object_t *buf = xrt_buffer_ptr(recv);
    if (!buf)
        return XR_NULL_VAL;
    if (sym == XRT_SYM_LENGTH || sym == XRT_SYM_SIZE)
        return XR_FROM_INT(buf->length);
    if (sym == XRT_SYM_PTR_UNCHECKED)
        return xrt_buffer_ptr_unchecked(recv);
    return XR_NULL_VAL;
}

static inline XrValue xrt_buffer_method_1(XrValue recv, int sym, XrValue arg0) {
    if (sym == XRT_SYM_RESIZE)
        return xrt_buffer_resize(recv, arg0);
    return XR_NULL_VAL;
}

static inline XrValue xrt_method_0(XrValue recv, int sym) {
    if (recv.tag == XR_TAG_BUFFER)
        return xrt_buffer_method_0(recv, sym);
    return XR_NULL_VAL;
}

static inline XrValue xrt_method_1(XrValue recv, int sym, XrValue arg0) {
    if (recv.tag == XR_TAG_BUFFER)
        return xrt_buffer_method_1(recv, sym, arg0);
    return XR_NULL_VAL;
}

static inline XrValue xrt_getprop(XrValue obj, int64_t symbol_id) {
    if (obj.tag == XR_TAG_BUFFER)
        return xrt_buffer_method_0(obj, (int) symbol_id);
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_alloc(XrValue n) {
    return xrt_buffer_new(xrt_mem_int_arg(n), 0, 0);
}

static inline XrValue xrt_mem_alloc_zeroed(XrValue n) {
    return xrt_buffer_new(xrt_mem_int_arg(n), 1, 0);
}

static inline XrValue xrt_mem_alloc_aligned(XrValue n, XrValue align) {
    return xrt_buffer_new(xrt_mem_int_arg(n), 0, (size_t) xrt_mem_int_arg(align));
}

static inline XrValue xrt_mem_from_address(XrValue addr) {
    return xr_mkptr((void *) (uintptr_t) (int64_t) xrt_mem_int_arg(addr), XR_TAG_PTR);
}

static inline XrValue xrt_mem_address_of(XrValue ptr) {
    return XR_FROM_INT((int64_t) (intptr_t) ptr.ptr);
}

static inline XrValue xrt_mem_copy(XrValue dst, XrValue src, XrValue n) {
    memcpy(dst.ptr, src.ptr, (size_t) xrt_mem_int_arg(n));
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_move(XrValue dst, XrValue src, XrValue n) {
    memmove(dst.ptr, src.ptr, (size_t) xrt_mem_int_arg(n));
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_set(XrValue dst, XrValue byte, XrValue n) {
    memset(dst.ptr, (int) xrt_mem_int_arg(byte), (size_t) xrt_mem_int_arg(n));
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_compare(XrValue a, XrValue b, XrValue n) {
    return XR_FROM_INT(memcmp(a.ptr, b.ptr, (size_t) xrt_mem_int_arg(n)));
}

static inline void xrt_mem_cache_maintain_range(void *ptr, size_t n) {
    if (!ptr || n == 0)
        return;
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    uintptr_t addr = (uintptr_t) ptr;
    uintptr_t start = addr & ~(uintptr_t) 63u;
    uintptr_t end = addr + n;
    if (end < addr)
        end = UINTPTR_MAX;
    for (uintptr_t p = start; p < end; p += 64u) {
        __asm__ __volatile__("clflush (%0)" : : "r"((const void *) p) : "memory");
        if (UINTPTR_MAX - p < 64u)
            break;
    }
#else
    (void) ptr;
    (void) n;
#endif
    xr_sync_core_fence(4);
}

static inline XrValue xrt_mem_cache_flush(XrValue ptr, XrValue n) {
    int64_t len = xrt_mem_int_arg(n);
    if (len > 0)
        xrt_mem_cache_maintain_range(ptr.ptr, (size_t) len);
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_cache_invalidate(XrValue ptr, XrValue n) {
    int64_t len = xrt_mem_int_arg(n);
    if (len > 0)
        xrt_mem_cache_maintain_range(ptr.ptr, (size_t) len);
    return XR_NULL_VAL;
}

static inline XrValue xrt_mem_volatile_load(XrValue ptr, XrValue size) {
    void *p = ptr.ptr;
    uint64_t v = 0;
    switch (xrt_mem_int_arg(size)) {
        case 1:
            v = *(volatile uint8_t *) p;
            break;
        case 2:
            v = *(volatile uint16_t *) p;
            break;
        case 4:
            v = *(volatile uint32_t *) p;
            break;
        case 8:
            v = *(volatile uint64_t *) p;
            break;
        default:
            break;
    }
    return XR_FROM_INT((int64_t) v);
}

static inline XrValue xrt_mem_volatile_store(XrValue ptr, XrValue value, XrValue size) {
    void *p = ptr.ptr;
    int64_t v = xrt_mem_int_arg(value);
    switch (xrt_mem_int_arg(size)) {
        case 1:
            *(volatile uint8_t *) p = (uint8_t) v;
            break;
        case 2:
            *(volatile uint16_t *) p = (uint16_t) v;
            break;
        case 4:
            *(volatile uint32_t *) p = (uint32_t) v;
            break;
        case 8:
            *(volatile uint64_t *) p = (uint64_t) v;
            break;
        default:
            break;
    }
    return XR_NULL_VAL;
}

static inline void xrt_mem_nontemporal_store_raw(void *p, uint64_t value, int64_t size) {
    int streamed = 0;
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    uintptr_t addr = (uintptr_t) p;
    if (size == 4 && (addr & 3u) == 0) {
        uint32_t v32 = (uint32_t) value;
        __asm__ __volatile__("movnti %1, %0" : "=m"(*(uint32_t *) p) : "r"(v32) : "memory");
        streamed = 1;
    }
#if defined(__x86_64__)
    else if (size == 8 && (addr & 7u) == 0) {
        uint64_t v64 = (uint64_t) value;
        __asm__ __volatile__("movnti %1, %0" : "=m"(*(uint64_t *) p) : "r"(v64) : "memory");
        streamed = 1;
    }
#endif
    else
#endif
    {
        switch (size) {
            case 1:
                *(volatile uint8_t *) p = (uint8_t) value;
                break;
            case 2:
                *(volatile uint16_t *) p = (uint16_t) value;
                break;
            case 4:
                *(volatile uint32_t *) p = (uint32_t) value;
                break;
            case 8:
                *(volatile uint64_t *) p = (uint64_t) value;
                break;
            default:
                break;
        }
    }
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    if (streamed)
        __asm__ __volatile__("sfence" ::: "memory");
    else
#endif
        xr_sync_core_fence(4);
}

static inline XrValue xrt_mem_nontemporal_store(XrValue ptr, XrValue value, XrValue size) {
    xrt_mem_nontemporal_store_raw(ptr.ptr, (uint64_t) xrt_mem_int_arg(value),
                                  xrt_mem_int_arg(size));
    return XR_NULL_VAL;
}

#endif  // XRT_CORE_FREESTANDING_H
