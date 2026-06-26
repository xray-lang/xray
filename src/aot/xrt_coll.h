/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_coll.h - Collection runtime: Array, Map, StringBuilder, Closure, index ops
 */

#ifndef XRT_COLL_H
#define XRT_COLL_H

#ifndef XR_FUNC
#define XR_FUNC extern
#endif

#include "xrt_value.h"
#include "xrt_arc.h"  // xrt_str_alloc used by xrt_strbuf_finish
#include "xrt_range.h"
#include "../runtime/xerror_codes.h"
#include "../shared/xr_array_abi.h"
#include "../shared/xr_array_core.h"
#include "../shared/xr_builtin_schema.h"
#include "../shared/xr_cell_abi.h"
#include "../shared/xr_elem_type.h"
#include "../shared/xr_error_core.h"
#include "../shared/xr_float_fmt.h"
#include "../shared/xr_map_set_abi.h"
#include "../shared/xr_typed_ops.h"
#include <string.h>

/* =========================================================================
 * Array runtime
 * ========================================================================= */

/* Shared array fields (data/length/capacity/source/data_storage/elem_*) come
 * from XR_ARRAY_ABI_FIELDS so the AOT and VM array layouts stay in lockstep.
 *
 * Alignment contract: non-slice arrays are XRT_DATA_ALIGN-aligned (32 bytes,
 * AVX-width). Initial storage lives in the header block or stack frame at a
 * rounded-up address; growth spills through xrt_array_data_grow into aligned
 * heap storage. Generated _adN caches assert it via XR_ASSUME_ALIGNED. Slice
 * views alias a sub-range of another array's storage at an arbitrary element
 * offset — no alignment promise.
 *
 * `source` and `contains_refs`/`elem_tid` are part of the shared ABI; the AOT
 * runtime keeps `source` NULL (slices borrow via arena lifetime) and does not
 * consult the GC-tracking fields. */
typedef struct {
    XrObjHeader hdr; /* embedded-at-0 header: same placement as the VM XrArray so
                      * the two layouts line up (C0 object-header unification) */
    XR_ARRAY_ABI_FIELDS;
} xrt_array_t;

static inline size_t xrt_array_data_bytes_or_abort(int64_t cap, uint8_t elem_size,
                                                   const char *where) {
    if (cap < 0)
        cap = 0;
    if (elem_size == 0)
        elem_size = 1;
    if ((uint64_t) cap > (uint64_t) SIZE_MAX / (uint64_t) elem_size) {
        fprintf(stderr, "%s: capacity overflow\n", where);
        abort();
    }
    return (size_t) cap * (size_t) elem_size;
}

/* Heap containers start with a bump-style header because the same init helpers
 * are shared by stack and heap constructors. Under the default deterministic
 * policy, heap containers become normal RC objects; under XRAY_AOT_ARENA=1 they
 * keep bump lifetime and release at process exit. */
static inline void xrt_coll_make_deterministic(XrObjHeader *h) {
    h->extra |= XR_OBJ_AOT_NATIVE;
    if (xrt_bump_enabled)
        return;
    h->extra &= (uint16_t) ~(uint16_t) XR_OBJ_STORAGE_BUMP;
    atomic_store_explicit(&h->refcount, 0, memory_order_relaxed);
}

static inline void xrt_array_init_header(xrt_array_t *a, int64_t cap, uint8_t etype,
                                         uint8_t elem_size) {
    xrt_bump_header_init(&a->hdr, XR_TARRAY);
    a->length = 0;
    a->capacity = cap;
    a->source = NULL;
    a->elem_type = etype;
    a->elem_size = elem_size;
    a->elem_tid = 0;
    a->contains_refs = 0;
    a->data_storage = XR_ARRAY_DATA_INLINE;
    a->adt_enum_name = NULL;
    a->adt_member_name = NULL;
}

static inline void xrt_array_check_store_or_abort(const xrt_array_t *a, XrValue val,
                                                  const char *where) {
    if (XR_UNLIKELY(a && a->elem_type == XR_ELEM_CHAR && val.tag != XR_TAG_CHAR)) {
        fprintf(stderr, "%s: Array<char> element must be char\n", where);
        abort();
    }
}

static inline xrt_array_t *xrt_array_alloc_inline(int64_t cap, uint8_t etype, int zeroed,
                                                  const char *where) {
    if (etype >= XR_ELEM_COUNT)
        etype = XR_ELEM_ANY;
    uint8_t elem_size = XR_ELEM_SIZES[etype];
    size_t data_bytes = xrt_array_data_bytes_or_abort(cap, elem_size, where);
    size_t pad = data_bytes ? (XRT_DATA_ALIGN - 1) : 0;
    if (data_bytes > SIZE_MAX - sizeof(xrt_array_t) - pad) {
        fprintf(stderr, "%s: allocation size overflow\n", where);
        abort();
    }
    size_t total = sizeof(xrt_array_t) + data_bytes + pad;
    xrt_array_t *a = (xrt_array_t *) XRT_MALLOC(total);
    if (XR_UNLIKELY(!a)) {
        fprintf(stderr, "%s: out of memory\n", where);
        abort();
    }
    xrt_array_init_header(a, cap, etype, elem_size);
    xrt_coll_make_deterministic(&a->hdr);
    if (data_bytes) {
        a->data =
            (void *) (((uintptr_t) ((char *) a + sizeof(xrt_array_t)) + (XRT_DATA_ALIGN - 1)) &
                      ~(uintptr_t) (XRT_DATA_ALIGN - 1));
        if (zeroed)
            memset(a->data, 0, data_bytes);
    } else {
        a->data = NULL;
    }
    return a;
}

/* Grow the element buffer to hold new_cap elements. Initial array storage lives
 * inline in the header allocation; the first growth spills to an aligned heap
 * buffer, and later growth frees the previous heap buffer. Stack arrays abort
 * instead of spilling so alloca-backed values cannot outlive their frame. */
static inline void xrt_array_data_grow(xrt_array_t *a, int64_t new_cap) {
    if (XR_UNLIKELY(a->data_storage == XR_ARRAY_DATA_STACK)) {
        fprintf(stderr, "xrt_array_data_grow: stack array capacity exceeded\n");
        abort();
    }
    size_t old_bytes =
        xrt_array_data_bytes_or_abort(a->capacity, a->elem_size, "xrt_array_data_grow");
    size_t new_bytes = xrt_array_data_bytes_or_abort(new_cap, a->elem_size, "xrt_array_data_grow");
    void *tmp = XRT_ALLOC_ALIGNED(new_bytes);
    if (XR_UNLIKELY(!tmp)) {
        fprintf(stderr, "xrt_array_data_grow: out of memory\n");
        abort();
    }
    if (a->data && old_bytes > 0) {
        memcpy(tmp, a->data, old_bytes);
        if (a->data_storage == XR_ARRAY_DATA_HEAP)
            XRT_FREE_ALIGNED(a->data);
    }
    if (new_bytes > old_bytes)
        memset((uint8_t *) tmp + old_bytes, 0, new_bytes - old_bytes);
    a->data = tmp;
    a->data_storage = XR_ARRAY_DATA_HEAP;
    a->capacity = new_cap;
}

static inline int xrt_array_data_is_inline(const xrt_array_t *a) {
    return a && a->data_storage == XR_ARRAY_DATA_INLINE;
}

static inline int xrt_array_data_is_heap(const xrt_array_t *a) {
    return a && a->data_storage == XR_ARRAY_DATA_HEAP;
}

static inline int xrt_array_data_is_stack(const xrt_array_t *a) {
    return a && a->data_storage == XR_ARRAY_DATA_STACK;
}

static inline int xrt_array_data_is_borrowed(const xrt_array_t *a) {
    return a && a->data_storage == XR_ARRAY_DATA_BORROWED;
}

static inline XrValue xrt_array_new(int64_t cap) {
    if (cap < 4)
        cap = 4;
    xrt_array_t *a = xrt_array_alloc_inline(cap, XR_ELEM_ANY, 1, "xrt_array_new");
    return xr_mkptr(a, XR_TAG_ARRAY);
}

static inline xrt_array_t *xrt_array_new_typed_ptr(int64_t cap, uint8_t etype) {
    if (cap < 4)
        cap = 4;
    return xrt_array_alloc_inline(cap, etype, 1, "xrt_array_new_typed");
}

static inline XrValue xrt_array_new_typed(int64_t cap, uint8_t etype) {
    return xr_mkptr(xrt_array_new_typed_ptr(cap, etype), XR_TAG_ARRAY);
}

static inline xrt_array_t *xrt_array_new_typed_uninit_ptr(int64_t cap, uint8_t etype) {
    if (cap < 4)
        cap = 4;
    return xrt_array_alloc_inline(cap, etype, 0, "xrt_array_new_typed_uninit");
}

static inline XrValue xrt_array_new_typed_uninit(int64_t cap, uint8_t etype) {
    return xr_mkptr(xrt_array_new_typed_uninit_ptr(cap, etype), XR_TAG_ARRAY);
}
static inline XrValue xrt_json_new_named(int64_t field_count, const char *const *field_names);
static inline void xrt_json_set_field(XrValue obj, int field_idx, XrValue val);
XRT_COLD _Noreturn void xrt_throw_exc(XrValue exc);

static inline XrValue xrt_error_message_value(const char *message) {
    if (!message)
        return XR_NULL_VAL;
    size_t len = strlen(message);
    XrValue s = xrt_str_alloc(len);
    if (len > 0)
        memcpy(xr_str_buf(s), message, len);
    return s;
}

static inline XrValue xrt_structured_error_value(int code, const char *message) {
    XrValue exc = xrt_json_new_named(EXCEPTION_FIELD_COUNT, xr_exception_field_names());
    xrt_json_set_field(exc, EXCEPTION_FIELD_MESSAGE, xrt_error_message_value(message));
    xrt_json_set_field(exc, EXCEPTION_FIELD_STACK, xrt_array_new(0));
    xrt_json_set_field(exc, EXCEPTION_FIELD_CAUSE, XR_NULL_VAL);
    xrt_json_set_field(exc, EXCEPTION_FIELD_CODE, XR_FROM_INT(code));
    xrt_json_set_field(exc, EXCEPTION_FIELD_DATA, XR_NULL_VAL);
    return exc;
}

static inline void xrt_throw_error(int code, const char *message) {
    xrt_throw_exc(xrt_structured_error_value(code, message));
}

static inline XrValue xrt_bytes_new_len(int64_t len) {
    if (len < 0)
        len = 0;
    XrValue arr = xrt_array_new_typed(len, XR_ELEM_U8);
    ((xrt_array_t *) arr.ptr)->length = len;
    return arr;
}

static inline XrValue xrt_bytes_new_fill(XrValue len_value, XrValue fill_value) {
    int64_t len = xr_value_to_int64_coerce(len_value);
    XrValue arr = xrt_bytes_new_len(len);
    xrt_array_t *a = (xrt_array_t *) arr.ptr;
    uint8_t fill = (uint8_t) (xr_value_to_int64_coerce(fill_value) & 0xFF);
    if (a->length > 0)
        memset(a->data, fill, (size_t) a->length);
    return arr;
}

static inline XrValue xrt_bytes_new_copy(XrValue src_value) {
    if (!XR_IS_ARRAY(src_value) || !src_value.ptr)
        return XR_NULL_VAL;
    xrt_array_t *src = (xrt_array_t *) src_value.ptr;
    XrValue arr = xrt_bytes_new_len(src->length);
    xrt_array_t *dst = (xrt_array_t *) arr.ptr;
    if (src->elem_type == XR_ELEM_U8) {
        memcpy(dst->data, src->data, (size_t) src->length);
        return arr;
    }
    for (int64_t i = 0; i < src->length; i++) {
        XrValue item = xr_typed_get(src->data, (int32_t) i, src->elem_type);
        xr_typed_set(dst->data, (int32_t) i, item, dst->elem_type);
    }
    return arr;
}

static inline XrValue xrt_bytes_new_1(XrValue arg) {
    if (XR_IS_ARRAY(arg))
        return xrt_bytes_new_copy(arg);
    return xrt_bytes_new_len(xr_value_to_int64_coerce(arg));
}

static inline void xrt_array_push(XrValue arr, XrValue val) {
    xrt_array_t *a = (xrt_array_t *) arr.ptr;
    xrt_array_check_store_or_abort(a, val, "xrt_array_push");
    if (XR_UNLIKELY(a->data_storage == XR_ARRAY_DATA_BORROWED)) {
        fprintf(stderr, "xrt_array_push: cannot push to array slice\n");
        abort();
    }
    if (XR_UNLIKELY(a->length >= a->capacity))
        xrt_array_data_grow(a, a->capacity == 0 ? 4 : a->capacity * 2);
    xr_typed_set(a->data, (int32_t) a->length, val, a->elem_type);
    a->length++;
}

/* Splice every element of `src_val` onto the end of `dst_val` (array spread
 * `[...a]`).  Borrowed source: each copied element is retained because the
 * source array keeps its own references.  Typed primitive arrays bulk-copy
 * (no per-element refcount), boxed (ANY) arrays retain each value. */
static inline void xrt_array_extend(XrValue dst_val, XrValue src_val) {
    xrt_array_t *dst = (xrt_array_t *) dst_val.ptr;
    xrt_array_t *src = (xrt_array_t *) src_val.ptr;
    int64_t n = src->length;
    if (n <= 0)
        return;
    if (XR_UNLIKELY(dst->data_storage == XR_ARRAY_DATA_BORROWED)) {
        fprintf(stderr, "xrt_array_extend: cannot extend array slice\n");
        abort();
    }
    if (dst->elem_type == src->elem_type && dst->elem_type != XR_ELEM_ANY) {
        /* Packed primitive storage: bulk memcpy, no refcount work. */
        int64_t need = dst->length + n;
        if (need > dst->capacity) {
            int64_t ncap = dst->capacity == 0 ? 4 : dst->capacity;
            while (ncap < need)
                ncap *= 2;
            xrt_array_data_grow(dst, ncap);
        }
        memcpy((uint8_t *) dst->data + (size_t) dst->length * dst->elem_size, src->data,
               (size_t) n * src->elem_size);
        dst->length = need;
        return;
    }
    for (int64_t j = 0; j < n; j++) {
        XrValue elem = xr_typed_get(src->data, (int32_t) j, src->elem_type);
        xrt_retain(elem);
        xrt_array_push(dst_val, elem);
    }
}

static inline int64_t xrt_array_len(XrValue arr) {
    return ((xrt_array_t *) arr.ptr)->length;
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

/* `.slice()` / `[a:b]` return an independent copy (value semantics), matching the VM.
 * A borrowed view would alias the source on write; copying keeps slices safe. */
static inline XrValue xrt_array_slice_view(XrValue arr, int64_t start, int64_t end) {
    if (!XR_IS_ARRAY(arr) || !arr.ptr)
        return XR_NULL_VAL;
    xrt_array_t *src = (xrt_array_t *) arr.ptr;
    xrt_array_normalize_slice(src->length, &start, &end);
    int64_t count = end - start;
    if (count < 0)
        count = 0;

    xrt_array_t *slice = xrt_array_new_typed_uninit_ptr(count, src->elem_type);
    slice->length = count;
    slice->elem_tid = src->elem_tid;
    slice->contains_refs = src->contains_refs;
    slice->adt_enum_name = src->adt_enum_name;
    slice->adt_member_name = src->adt_member_name;
    if (count > 0)
        memcpy(slice->data, (uint8_t *) src->data + (size_t) start * (size_t) src->elem_size,
               (size_t) count * (size_t) src->elem_size);
    if (src->contains_refs) {
        XrValue *data = (XrValue *) slice->data;
        for (int64_t i = 0; i < count; i++)
            xrt_retain(data[i]);
    }
    return xr_mkptr(slice, XR_TAG_ARRAY);
}

#include "xrt_array_bytes.inc.c"

typedef struct {
    int64_t len;
    XrValue items[];
} xrt_tuple_t;

static inline XrValue xrt_tuple_new(int64_t len) {
    if (len < 0)
        len = 0;
    xrt_tuple_t *t =
        (xrt_tuple_t *) XRT_MALLOC(sizeof(xrt_tuple_t) + (size_t) len * sizeof(XrValue));
    if (XR_UNLIKELY(!t)) {
        fprintf(stderr, "xrt_tuple_new: out of memory\n");
        abort();
    }
    t->len = len;
    for (int64_t i = 0; i < len; i++)
        t->items[i] = XR_NULL_VAL;
    return xr_mkptr(t, XR_TAG_TUPLE);
}

static inline XrValue xrt_tuple_make(int64_t len, const XrValue *items) {
    XrValue tuple = xrt_tuple_new(len);
    xrt_tuple_t *t = (xrt_tuple_t *) tuple.ptr;
    for (int64_t i = 0; i < t->len; i++)
        t->items[i] = items ? items[i] : XR_NULL_VAL;
    return tuple;
}

static inline XrValue xrt_tuple_get(XrValue tuple, int64_t index) {
    if (tuple.tag != XR_TAG_TUPLE || !tuple.ptr)
        return XR_NULL_VAL;
    xrt_tuple_t *t = (xrt_tuple_t *) tuple.ptr;
    if (index < 0 || index >= t->len)
        return XR_NULL_VAL;
    return t->items[index];
}

static inline XrValue xrt_slice(XrValue source, XrValue start_value, XrValue end_value) {
    int64_t start = xr_value_to_int64_coerce(start_value);
    int64_t end = xr_value_to_int64_coerce(end_value);
    if (XR_IS_ARRAY(source))
        return xrt_array_slice_view(source, start, end);
    if (XR_IS_STR(source)) {
        const char *s = xr_str_data(source);
        int64_t len = xr_str_len(source);
        if (start < 0) {
            start += len;
            if (start < 0)
                start = 0;
        }
        if (end < 0) {
            end += len;
            if (end < 0)
                end = 0;
        }
        if (start > len)
            start = len;
        if (end > len)
            end = len;
        if (start >= end)
            return xrt_str_alloc(0);
        int64_t rlen = end - start;
        XrValue sv = xrt_str_alloc((size_t) rlen);
        memcpy(xr_str_buf(sv), s + start, (size_t) rlen);
        return sv;
    }
    return XR_NULL_VAL;
}

/* Stack-allocated array: header on stack, data buffer on stack via alloca.
 * Used for NO_ESCAPE arrays (escape analysis optimization).
 * The returned XrValue is valid only within the current function scope.
 * The data pointer is rounded up so the XRT_DATA_ALIGN contract holds for
 * stack storage too (alloca only guarantees max_align_t). */
#ifndef xrt_array_stack_new
#define xrt_array_stack_new(cap_expr)                                                              \
    ({                                                                                             \
        int64_t _cap = (cap_expr);                                                                 \
        if (_cap < 4)                                                                              \
            _cap = 4;                                                                              \
        xrt_array_t *_a = (xrt_array_t *) __builtin_alloca(                                        \
            sizeof(xrt_array_t) + (size_t) _cap * sizeof(XrValue) + (XRT_DATA_ALIGN - 1));         \
        xrt_bump_header_init(&_a->hdr, XR_TARRAY);                                                 \
        _a->length = 0;                                                                            \
        _a->capacity = _cap;                                                                       \
        _a->source = NULL;                                                                         \
        _a->elem_type = XR_ELEM_ANY;                                                               \
        _a->elem_size = (uint8_t) sizeof(XrValue);                                                 \
        _a->elem_tid = 0;                                                                          \
        _a->contains_refs = 0;                                                                     \
        _a->data_storage = XR_ARRAY_DATA_STACK;                                                    \
        _a->adt_enum_name = NULL;                                                                  \
        _a->adt_member_name = NULL;                                                                \
        _a->data =                                                                                 \
            (void *) (((uintptr_t) ((char *) _a + sizeof(xrt_array_t)) + (XRT_DATA_ALIGN - 1)) &   \
                      ~(uintptr_t) (XRT_DATA_ALIGN - 1));                                          \
        memset(_a->data, 0, (size_t) _cap * sizeof(XrValue));                                      \
        xr_mkptr(_a, XR_TAG_ARRAY);                                                                \
    })
#endif

/* =========================================================================
 * StringBuilder runtime
 * ========================================================================= */

typedef struct {
    char *buf;
    int64_t len;
    int64_t cap;
} xrt_strbuf_t;

static inline XrValue xrt_strbuf_new(void) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) XRT_MALLOC(sizeof(xrt_strbuf_t));
    if (XR_UNLIKELY(!sb)) {
        fprintf(stderr, "xrt_strbuf_new: out of memory\n");
        abort();
    }
    sb->cap = 64;
    sb->len = 0;
    sb->buf = (char *) XRT_MALLOC(64);
    if (XR_UNLIKELY(!sb->buf)) {
        fprintf(stderr, "xrt_strbuf_new: out of memory\n");
        abort();
    }
    sb->buf[0] = 0;
    return xr_mkptr(sb, XR_TAG_STRBUF);
}

static inline void xrt_strbuf_grow(xrt_strbuf_t *sb, int64_t need) {
    if (XR_LIKELY(sb->len + need < sb->cap))
        return;
    while (sb->len + need >= sb->cap)
        sb->cap *= 2;
    char *tmp = (char *) XRT_REALLOC(sb->buf, (size_t) sb->cap);
    if (XR_UNLIKELY(!tmp)) {
        fprintf(stderr, "xrt_strbuf_grow: out of memory\n");
        abort();
    }
    sb->buf = tmp;
}

static inline void xrt_strbuf_append(XrValue sbv, XrValue val) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) sbv.ptr;
    if (val.tag == XR_TAG_STR || val.tag == XR_TAG_STR_ARC) {
        const char *s = xr_str_data(val);
        int64_t slen = xr_str_len(val);
        xrt_strbuf_grow(sb, slen);
        memcpy(sb->buf + sb->len, s, (size_t) slen);
        sb->len += slen;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XR_TAG_I64) {
        char tmp[24];
        int n = snprintf(tmp, sizeof(tmp), "%lld", (long long) val.i);
        xrt_strbuf_grow(sb, n);
        memcpy(sb->buf + sb->len, tmp, (size_t) n);
        sb->len += n;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XR_TAG_F64) {
        char tmp[64];
        int n = xr_format_float(tmp, sizeof(tmp), val.f);
        xrt_strbuf_grow(sb, n);
        memcpy(sb->buf + sb->len, tmp, (size_t) n);
        sb->len += n;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XR_TAG_BOOL) {
        const char *bs = val.i ? "true" : "false";
        int blen = val.i ? 4 : 5;
        xrt_strbuf_grow(sb, blen);
        memcpy(sb->buf + sb->len, bs, (size_t) blen);
        sb->len += blen;
        sb->buf[sb->len] = 0;
    } else if (val.tag == XR_TAG_NULL) {
        xrt_strbuf_grow(sb, 4);
        memcpy(sb->buf + sb->len, "null", 4);
        sb->len += 4;
        sb->buf[sb->len] = 0;
    } else {
        char tmp[64];
        const char *s = xr_to_cstr(val, tmp, sizeof(tmp));
        int64_t slen = (int64_t) strlen(s);
        xrt_strbuf_grow(sb, slen);
        memcpy(sb->buf + sb->len, s, (size_t) slen);
        sb->len += slen;
        sb->buf[sb->len] = 0;
    }
}

static inline XrValue xrt_strbuf_finish(XrValue sbv) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) sbv.ptr;
    XrValue v = xrt_str_alloc((size_t) sb->len);
    memcpy(xr_str_buf(v), sb->buf, (size_t) (sb->len + 1));
    return v;
}

typedef struct {
    const char *a;
    const char *b;
    size_t alen;
    size_t blen;
    char scratch[64];
    uint8_t kind;
} xrt_strpart_t;

#define XRT_STRPART_SINGLE 0u
#define XRT_STRPART_ENUM 1u

static inline size_t xrt_format_uint64(char *buf, size_t cap, uint64_t value) {
    int n = snprintf(buf, cap, "%llu", (unsigned long long) value);
    if (n < 0)
        return 0;
    if ((size_t) n >= cap)
        return cap ? cap - 1u : 0u;
    return (size_t) n;
}

static inline XrValue xrt_uint64_to_string(uint64_t value) {
    char scratch[32];
    size_t len = xrt_format_uint64(scratch, sizeof(scratch), value);
    XrValue out = xrt_str_alloc(len);
    memcpy(xr_str_buf(out), scratch, len);
    return out;
}

static inline void xrt_strpart_init_u64(xrt_strpart_t *part, uint64_t value) {
    part->a = "";
    part->b = NULL;
    part->alen = 0;
    part->blen = 0;
    part->kind = XRT_STRPART_SINGLE;
    part->alen = xrt_format_uint64(part->scratch, sizeof(part->scratch), value);
    part->a = part->scratch;
}

static inline void xrt_strpart_init(xrt_strpart_t *part, XrValue val) {
    part->a = "";
    part->b = NULL;
    part->alen = 0;
    part->blen = 0;
    part->kind = XRT_STRPART_SINGLE;

    if (val.tag == XR_TAG_STR || val.tag == XR_TAG_STR_ARC) {
        part->a = xr_str_data(val);
        part->alen = (size_t) xr_str_len(val);
    } else if (val.tag == XR_TAG_I64) {
        int n = snprintf(part->scratch, sizeof(part->scratch), "%lld", (long long) val.i);
        part->a = part->scratch;
        part->alen = (size_t) (n < 0 ? 0 : n);
    } else if (val.tag == XR_TAG_F64) {
        int n = xr_format_float(part->scratch, sizeof(part->scratch), val.f);
        part->a = part->scratch;
        part->alen = (size_t) (n < 0 ? 0 : n);
    } else if (val.tag == XR_TAG_BOOL) {
        part->a = val.i ? "true" : "false";
        part->alen = val.i ? 4u : 5u;
    } else if (val.tag == XR_TAG_CHAR) {
        uint32_t cp = XR_TO_CHAR(val);
        int n = 0;
        if (cp <= 0x7Fu) {
            part->scratch[n++] = (char) cp;
        } else if (cp <= 0x7FFu) {
            part->scratch[n++] = (char) (0xC0u | (cp >> 6));
            part->scratch[n++] = (char) (0x80u | (cp & 0x3Fu));
        } else if (cp <= 0xFFFFu && !(cp >= 0xD800u && cp <= 0xDFFFu)) {
            part->scratch[n++] = (char) (0xE0u | (cp >> 12));
            part->scratch[n++] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
            part->scratch[n++] = (char) (0x80u | (cp & 0x3Fu));
        } else if (cp <= 0x10FFFFu) {
            part->scratch[n++] = (char) (0xF0u | (cp >> 18));
            part->scratch[n++] = (char) (0x80u | ((cp >> 12) & 0x3Fu));
            part->scratch[n++] = (char) (0x80u | ((cp >> 6) & 0x3Fu));
            part->scratch[n++] = (char) (0x80u | (cp & 0x3Fu));
        }
        part->a = part->scratch;
        part->alen = (size_t) n;
    } else if (val.tag == XR_TAG_NULL) {
        part->a = "null";
        part->alen = 4u;
    } else if (val.tag == XR_TAG_ENUM) {
        const XrAotEnumValueView *ev = (const XrAotEnumValueView *) val.ptr;
        if (ev && ev->enum_name && ev->member_name) {
            part->a = ev->enum_name;
            part->b = ev->member_name;
            part->alen = strlen(ev->enum_name);
            part->blen = strlen(ev->member_name);
            part->kind = XRT_STRPART_ENUM;
        } else {
            int n = snprintf(part->scratch, sizeof(part->scratch), "<enum@%p>", val.ptr);
            part->a = part->scratch;
            part->alen = (size_t) (n < 0 ? 0 : n);
        }
    } else {
        const char *s = xr_to_cstr(val, part->scratch, sizeof(part->scratch));
        part->a = s;
        part->alen = strlen(s);
    }
}

static inline size_t xrt_strpart_len(const xrt_strpart_t *part) {
    return part->kind == XRT_STRPART_ENUM ? part->alen + 1u + part->blen : part->alen;
}

static inline char *xrt_strpart_copy(char *dst, const xrt_strpart_t *part) {
    memcpy(dst, part->a, part->alen);
    dst += part->alen;
    if (part->kind == XRT_STRPART_ENUM) {
        *dst++ = '.';
        memcpy(dst, part->b, part->blen);
        dst += part->blen;
    }
    return dst;
}

static inline XrValue xrt_str_concat_parts(size_t count, xrt_strpart_t *parts) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        size_t len = xrt_strpart_len(&parts[i]);
        if (XR_UNLIKELY(len > SIZE_MAX - total)) {
            fprintf(stderr, "xrt_str_concat_parts: string length overflow\n");
            abort();
        }
        total += len;
    }
    XrValue out = xrt_str_alloc(total);
    char *dst = xr_str_buf(out);
    for (size_t i = 0; i < count; i++)
        dst = xrt_strpart_copy(dst, &parts[i]);
    *dst = 0;
    return out;
}

static inline XrValue xrt_enum_value_new(const char *enum_name, const char *member_name,
                                         XrValue raw_value, uint32_t member_index) {
    XrAotEnumValueView *ev = (XrAotEnumValueView *) XRT_CALLOC(1, sizeof(*ev));
    if (XR_UNLIKELY(!ev)) {
        fprintf(stderr, "xrt_enum_value_new: out of memory\n");
        abort();
    }
    ev->enum_name = enum_name;
    ev->member_name = member_name;
    ev->raw_value = raw_value;
    ev->member_index = member_index;

    XrValue v = {0};
    v.tag = XR_TAG_ENUM;
    v.ext = member_index;
    v.ptr = ev;
    return v;
}

static inline const XrAotEnumValueView *xrt_enum_value_view(XrValue obj) {
    return obj.tag == XR_TAG_ENUM ? (const XrAotEnumValueView *) obj.ptr : NULL;
}

static inline XrValue xrt_enum_value_name(XrValue obj) {
    const XrAotEnumValueView *ev = xrt_enum_value_view(obj);
    return (ev && ev->member_name) ? xr_box_str(ev->member_name) : XR_NULL_VAL;
}

static inline XrValue xrt_enum_value_raw(XrValue obj) {
    const XrAotEnumValueView *ev = xrt_enum_value_view(obj);
    return ev ? ev->raw_value : XR_NULL_VAL;
}

static inline XrValue xrt_enum_value_ordinal(XrValue obj) {
    const XrAotEnumValueView *ev = xrt_enum_value_view(obj);
    return ev ? XR_FROM_INT(ev->member_index) : XR_NULL_VAL;
}

/* =========================================================================
 * Swiss-table core — group-probed open addressing shared by Map and Set.
 *
 * Control bytes:  EMPTY=0xFF  DELETED=0x80  FULL=h2 (top bit clear, 7 hash
 * bits).  Slot count is a power of two; the ctrl array carries XRT_GROUP
 * mirrored tail bytes so an unaligned group load never reads out of bounds.
 * Probing is triangular by whole groups, which visits every group exactly
 * once for power-of-two capacities.  Group matching uses portable 8-byte
 * SWAR; a byte match may be a false positive, so callers always confirm
 * with a key comparison (empty detection is exact).
 * ========================================================================= */

#define XRT_GROUP 8
#define XRT_CTRL_EMPTY 0xFFu
#define XRT_CTRL_DELETED 0x80u
#define XRT_SWAR_LOW 0x0101010101010101ull
#define XRT_SWAR_HIGH 0x8080808080808080ull

static inline uint64_t xrt_group_load(const uint8_t *p) {
    uint64_t g;
    memcpy(&g, p, sizeof(g));
    return g;
}

/* High bit set in each byte that may equal b (false positives possible). */
static inline uint64_t xrt_group_match(uint64_t g, uint8_t b) {
    uint64_t x = g ^ (XRT_SWAR_LOW * (uint64_t) b);
    return (x - XRT_SWAR_LOW) & ~x & XRT_SWAR_HIGH;
}

/* Exact: EMPTY (0xFF) has bits 7 and 6 set; DELETED only bit 7; FULL neither. */
static inline uint64_t xrt_group_match_empty(uint64_t g) {
    return g & (g << 1) & XRT_SWAR_HIGH;
}

static inline uint64_t xrt_group_match_free(uint64_t g) {
    return g & XRT_SWAR_HIGH; /* EMPTY or DELETED */
}

static inline int xrt_swar_first(uint64_t bits) {
    int n = 0;
    while (!(bits & 0xFFu)) {
        bits >>= 8;
        n++;
    }
    return n;
}

/* xrt_hash_mix_u64 / xrt_hash_bytes live in xrt_value.h (shared with the
 * compile-time literal hashing in the C backend). */

static inline uint64_t xrt_hash_f64(double d) {
    uint64_t bits;
    if (d == 0.0)
        d = 0.0; /* canonicalize -0.0: IEEE == treats them equal */
    memcpy(&bits, &d, sizeof(bits));
    return xr_hash_core_mix_u64(bits);
}

/* Hash for tagged values, consistent with xrt_eq: strings hash content
 * through the header cache (literals carry a precomputed hash). */
static inline uint64_t xrt_hash_value(XrValue v) {
    uint32_t tag = (v.tag == XR_TAG_STR_ARC) ? XR_TAG_STR : v.tag;
    switch (tag) {
        case XR_TAG_I64:
        case XR_TAG_BOOL:
            return xr_hash_core_mix_u64((uint64_t) v.i);
        case XR_TAG_F64:
            return xrt_hash_f64(v.f);
        case XR_TAG_STR:
            return xr_hash_core_mix_u64(xrt_str_hash(v));
        case XR_TAG_NULL:
            return xr_hash_core_mix_u64(0x9e3779b97f4a7c15ull);
        default:
            return xr_hash_core_mix_u64((uint64_t) (uintptr_t) v.ptr);
    }
}

static inline int64_t xrt_swiss_slots_for(int64_t want) {
    /* Smallest power of two whose 7/8 usable share covers `want` entries. */
    int64_t slots = XRT_GROUP;
    if (want < 0)
        want = 0;
    while (slots - slots / 8 < want)
        slots <<= 1;
    return slots;
}

static inline uint8_t *xrt_swiss_ctrl_alloc(int64_t slots) {
    uint8_t *ctrl = (uint8_t *) XRT_MALLOC((size_t) slots + XRT_GROUP);
    if (!ctrl) {
        fprintf(stderr, "xrt swiss: out of memory\n");
        abort();
    }
    memset(ctrl, (int) XRT_CTRL_EMPTY, (size_t) slots + XRT_GROUP);
    return ctrl;
}

static inline void xrt_swiss_ctrl_set(uint8_t *ctrl, int64_t slots, int64_t slot, uint8_t b) {
    ctrl[slot] = b;
    if (slot < XRT_GROUP)
        ctrl[slots + slot] = b; /* mirrored tail keeps group loads in bounds */
}

/* Find the slot holding `h2`-tagged keys; `eq_slot(ctx, slot)` confirms.
 * Returns the slot index or -1. */
typedef int (*xrt_swiss_eq_fn)(void *ctx, int64_t slot);

static inline int64_t xrt_swiss_find(const uint8_t *ctrl, int64_t slots, uint64_t hash,
                                     xrt_swiss_eq_fn eq, void *ctx) {
    uint64_t mask = (uint64_t) slots - 1;
    uint8_t h2 = (uint8_t) (hash & 0x7F);
    uint64_t pos = (hash >> 7) & mask;
    uint64_t stride = 0;
    for (;;) {
        uint64_t g = xrt_group_load(ctrl + pos);
        uint64_t m = xrt_group_match(g, h2);
        while (m) {
            int off = xrt_swar_first(m);
            int64_t slot = (int64_t) ((pos + (uint64_t) off) & mask);
            if (eq(ctx, slot))
                return slot;
            m &= ~(0xFFull << ((unsigned) off * 8u)); /* clear matched byte */
        }
        if (xrt_group_match_empty(g))
            return -1;
        stride += XRT_GROUP;
        pos = (pos + stride) & mask;
    }
}

/* First free (EMPTY or DELETED) slot along the probe sequence. */
static inline int64_t xrt_swiss_find_free(const uint8_t *ctrl, int64_t slots, uint64_t hash) {
    uint64_t mask = (uint64_t) slots - 1;
    uint64_t pos = (hash >> 7) & mask;
    uint64_t stride = 0;
    for (;;) {
        uint64_t g = xrt_group_load(ctrl + pos);
        uint64_t m = xrt_group_match_free(g);
        if (m) {
            int off = xrt_swar_first(m);
            return (int64_t) ((pos + (uint64_t) off) & mask);
        }
        stride += XRT_GROUP;
        pos = (pos + stride) & mask;
    }
}

/* First EMPTY (never DELETED) slot along the probe sequence. Insertion uses this
 * so a tombstoned slot is never reused while a stale order[] entry still points
 * at it; tombstones are reclaimed only by rehash compaction. The caller must
 * ensure a free slot exists (growth_left > 0). */
static inline int64_t xrt_swiss_find_empty(const uint8_t *ctrl, int64_t slots, uint64_t hash) {
    uint64_t mask = (uint64_t) slots - 1;
    uint64_t pos = (hash >> 7) & mask;
    uint64_t stride = 0;
    for (;;) {
        uint64_t g = xrt_group_load(ctrl + pos);
        uint64_t m = xrt_group_match_empty(g);
        if (m) {
            int off = xrt_swar_first(m);
            return (int64_t) ((pos + (uint64_t) off) & mask);
        }
        stride += XRT_GROUP;
        pos = (pos + stride) & mask;
    }
}

/* =========================================================================
 * Map runtime.
 *
 * Tagged maps use the shared hybrid-C ABI: dense insertion-order entries plus
 * a Swiss ctrl/index layer. Typed scalar maps temporarily keep their packed
 * SoA storage; 110 owns the typed-storage merge.
 * ========================================================================= */

typedef struct xrt_map_t {
    XrObjHeader hdr; /* embedded-at-0 header: same placement as the VM XrMap (C0
                      * object-header unification) */
    XR_MAP_ABI_FIELDS;

    /* Typed scalar storage (key_type/value_type != XR_ELEM_ANY). */
    int64_t len;
    int64_t cap;
    int64_t growth_left;
    void *keys;
    void *values;
    int64_t *order;
    int64_t order_len;
    int64_t order_cap;
    uint8_t key_type;
    uint8_t value_type;
    uint8_t key_size;
    uint8_t value_size;
} xrt_map_t;

static inline uint8_t xrt_value_type_tag(XrValue v) {
    return (uint8_t) (((v.tag == XR_TAG_STR_ARC) ? XR_TAG_STR : v.tag) + 1u);
}

static inline uint32_t xrt_hash32_value(XrValue v) {
    return (uint32_t) xrt_hash_value(v);
}

static inline uint32_t xrt_ordered_indices_size_for(uint32_t needed, uint32_t max_hbits) {
    uint32_t size = XR_SWISS_GROUP;
    while ((uint64_t) size * 2 / 3 < needed) {
        if (size >= (1u << max_hbits))
            return 1u << max_hbits;
        size <<= 1;
    }
    return size;
}

static inline void xrt_map_init_header(xrt_map_t *m) {
    xrt_bump_header_init(&m->hdr, XR_TMAP);
    m->count = 0;
    m->nentries = 0;
    m->entries_cap = 0;
    m->indices_size = 0;
    m->ctrl = NULL;
    m->indices = NULL;
    m->entries = NULL;
    m->flags = XR_MAP_FLAG_DUMMY;
    m->key_tid = 0;
    m->value_tid = 0;
    m->len = 0;
    m->cap = 0;
    m->growth_left = 0;
    m->keys = NULL;
    m->values = NULL;
    m->order = NULL;
    m->order_len = 0;
    m->order_cap = 0;
    m->key_type = XR_ELEM_ANY;
    m->value_type = XR_ELEM_ANY;
    m->key_size = (uint8_t) sizeof(XrValue);
    m->value_size = (uint8_t) sizeof(XrValue);
}

/* Candidate comparators for the shared Swiss probe (xr_{map,set}_lookup_slot):
 * type tag then canonical equality. xrt_eq is type-aware, so the tag pre-check
 * only short-circuits type-mismatched hash collisions. Return int (not bool) to
 * match the runtime's bool-free generated-C convention. */
static inline int xrt_map_key_eq(const XrMapEntry *e, XrValue key, uint8_t key_tt) {
    return e->key_tt == key_tt && xrt_eq(e->key, key) != 0;
}
static inline int xrt_set_value_eq(const XrSetEntry *e, XrValue value, uint8_t val_tt) {
    return e->val_tt == val_tt && xrt_eq(e->value, value) != 0;
}

static inline uint32_t xrt_map_find_entry_slot(xrt_map_t *m, XrValue key, uint32_t hash,
                                               uint8_t key_tt, int32_t *out_eidx) {
    return xr_map_lookup_slot(m->ctrl, m->indices, m->entries, m->indices_size, key, hash, key_tt,
                              xrt_map_key_eq, out_eidx);
}

static inline int32_t xrt_map_find_entry(xrt_map_t *m, XrValue key, uint32_t hash, uint8_t key_tt) {
    int32_t eidx = -1;
    return xrt_map_find_entry_slot(m, key, hash, key_tt, &eidx) == UINT32_MAX ? -1 : eidx;
}

static inline void xrt_map_resize_tagged(xrt_map_t *m, uint32_t min_needed) {
    if (XR_UNLIKELY(m->flags & XR_MAP_FLAG_NODES_ON_STACK)) {
        fprintf(stderr, "xrt_map_stack_new: capacity exceeded\n");
        abort();
    }

    XrMapEntry *old_entries = (m->flags & XR_MAP_FLAG_DUMMY) ? NULL : m->entries;
    uint8_t *old_ctrl = (m->flags & XR_MAP_FLAG_DUMMY) ? NULL : m->ctrl;
    int32_t *old_indices = (m->flags & XR_MAP_FLAG_DUMMY) ? NULL : m->indices;
    uint32_t old_nentries = (m->flags & XR_MAP_FLAG_DUMMY) ? 0 : m->nentries;

    uint32_t needed = m->count > min_needed ? m->count : min_needed;
    if (needed < 1)
        needed = 1;
    uint32_t indices_size = xrt_ordered_indices_size_for(needed, XR_MAP_MAXHBITS);
    uint32_t entries_cap = (uint32_t) ((uint64_t) indices_size * 2 / 3);
    if (entries_cap < needed)
        entries_cap = needed;

    size_t cbytes = (size_t) indices_size + XR_SWISS_GROUP;
    size_t ibytes = sizeof(int32_t) * (size_t) indices_size;
    uint8_t *ctrl = (uint8_t *) XRT_MALLOC(cbytes);
    int32_t *indices = (int32_t *) XRT_MALLOC(ibytes);
    XrMapEntry *entries = (XrMapEntry *) XRT_CALLOC((size_t) entries_cap, sizeof(XrMapEntry));
    if (XR_UNLIKELY(!ctrl || !indices || !entries)) {
        fprintf(stderr, "xrt_map_resize_tagged: out of memory\n");
        abort();
    }
    memset(ctrl, (int) XR_SWISS_CTRL_EMPTY, cbytes);
    for (uint32_t i = 0; i < indices_size; i++)
        indices[i] = XR_MAP_IX_EMPTY;

    uint32_t w =
        xr_map_rehash_into(entries, ctrl, indices, indices_size, old_entries, old_nentries);

    XRT_FREE(old_ctrl);
    XRT_FREE(old_indices);
    XRT_FREE(old_entries);

    m->ctrl = ctrl;
    m->indices = indices;
    m->entries = entries;
    m->indices_size = indices_size;
    m->entries_cap = entries_cap;
    m->nentries = w;
    m->flags &= (uint8_t) ~XR_MAP_FLAG_DUMMY;
}

static inline void xrt_map_order_reserve(xrt_map_t *m, int64_t need) {
    if (need <= m->order_cap)
        return;
    int64_t new_cap = m->order_cap > 0 ? m->order_cap : 4;
    while (new_cap < need)
        new_cap *= 2;
    int64_t *tmp = (int64_t *) XRT_REALLOC(m->order, (size_t) new_cap * sizeof(int64_t));
    if (XR_UNLIKELY(!tmp)) {
        fprintf(stderr, "xrt_map_order_reserve: out of memory\n");
        abort();
    }
    m->order = tmp;
    m->order_cap = new_cap;
}

static inline void xrt_map_order_append(xrt_map_t *m, int64_t slot) {
    xrt_map_order_reserve(m, m->order_len + 1);
    m->order[m->order_len++] = slot;
}

static inline int64_t xrt_map_growth_budget(int64_t slots) {
    return slots - slots / 8; /* 7/8 max load factor */
}

static inline void xrt_map_alloc_slots(xrt_map_t *m, int64_t slots) {
    m->cap = slots;
    m->growth_left = xrt_map_growth_budget(slots);
    m->ctrl = xrt_swiss_ctrl_alloc(slots);
    m->keys = XRT_CALLOC((size_t) slots, (size_t) m->key_size);
    m->values = XRT_CALLOC((size_t) slots, (size_t) m->value_size);
    if (XR_UNLIKELY(!m->keys || !m->values)) {
        fprintf(stderr, "xrt_map: out of memory\n");
        abort();
    }
}

static inline XrValue xrt_map_new_flags(int64_t cap, uint8_t flags) {
    xrt_map_t *m = (xrt_map_t *) XRT_MALLOC(sizeof(xrt_map_t));
    if (XR_UNLIKELY(!m)) {
        fprintf(stderr, "xrt_map_new: out of memory\n");
        abort();
    }
    xrt_map_init_header(m);
    xrt_coll_make_deterministic(&m->hdr);
    m->flags |= (uint8_t) (flags & XR_MAP_FLAG_WEAK);
    if (cap > 0)
        xrt_map_resize_tagged(m, (uint32_t) cap);
    return xr_mkptr(m, XR_TAG_MAP);
}

static inline XrValue xrt_map_new(int64_t cap) {
    return xrt_map_new_flags(cap, 0);
}

#ifndef xrt_map_stack_new
#define xrt_map_stack_new(cap_expr)                                                                \
    ({                                                                                             \
        int64_t _need64 = (cap_expr);                                                              \
        if (_need64 < 1)                                                                           \
            _need64 = 1;                                                                           \
        uint32_t _need = (uint32_t) _need64;                                                       \
        uint32_t _slots = xrt_ordered_indices_size_for(_need, XR_MAP_MAXHBITS);                    \
        uint32_t _ecap = (uint32_t) ((uint64_t) _slots * 2u / 3u);                                 \
        if (_ecap < _need)                                                                         \
            _ecap = _need;                                                                         \
        xrt_map_t *_m = (xrt_map_t *) __builtin_alloca(sizeof(xrt_map_t));                         \
        uint8_t *_ctrl = (uint8_t *) __builtin_alloca((size_t) _slots + XR_SWISS_GROUP);           \
        int32_t *_indices = (int32_t *) __builtin_alloca(sizeof(int32_t) * (size_t) _slots);       \
        XrMapEntry *_entries =                                                                     \
            (XrMapEntry *) __builtin_alloca(sizeof(XrMapEntry) * (size_t) _ecap);                  \
        xrt_map_init_header(_m);                                                                   \
        memset(_ctrl, (int) XR_SWISS_CTRL_EMPTY, (size_t) _slots + XR_SWISS_GROUP);                \
        for (uint32_t _i = 0; _i < _slots; _i++)                                                   \
            _indices[_i] = XR_MAP_IX_EMPTY;                                                        \
        memset(_entries, 0, sizeof(XrMapEntry) * (size_t) _ecap);                                  \
        _m->ctrl = _ctrl;                                                                          \
        _m->indices = _indices;                                                                    \
        _m->entries = _entries;                                                                    \
        _m->indices_size = _slots;                                                                 \
        _m->entries_cap = _ecap;                                                                   \
        _m->flags = XR_MAP_FLAG_NODES_ON_STACK;                                                    \
        xr_mkptr(_m, XR_TAG_MAP);                                                                  \
    })
#endif

#include "xrt_boolmap.inc.c"
#include "xrt_map_typed.inc.c"

static inline int64_t xrt_map_len(const xrt_map_t *m) {
    if (xrt_map_is_boolmap(m))
        return xrt_boolmap_len((const xrt_boolmap_t *) m);
    return xrt_map_is_typed(m) ? m->len : (int64_t) m->count;
}

static inline int xrt_map_slot_is_full(const xrt_map_t *m, int64_t slot) {
    if (xrt_map_is_typed(m))
        return (m->ctrl[slot] & 0x80u) == 0;
    return slot >= 0 && (uint32_t) slot < m->nentries &&
           m->entries[slot].key_tt != XR_MAP_ENTRY_NIL_KEY;
}

static inline XrValue xrt_map_get(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_boolmap(m))
        return xrt_boolmap_get_v((xrt_boolmap_t *) m, key);
    if (xrt_map_is_typed(m))
        return xrt_map_get_typed(m, key);
    uint8_t key_tt = xrt_value_type_tag(key);
    uint32_t hash = xrt_hash32_value(key);
    int32_t eidx = xrt_map_find_entry(m, key, hash, key_tt);
    return eidx >= 0 ? m->entries[eidx].value : XR_NULL_VAL;
}

static inline XrValue xrt_map_get_owned(xrt_map_t *m, XrValue key) {
    return xrt_value_to_owned(xrt_map_get(m, key));
}

static inline int xrt_map_has(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_boolmap(m))
        return xrt_boolmap_has_v((xrt_boolmap_t *) m, key);
    if (xrt_map_is_typed(m))
        return xrt_map_has_typed(m, key);
    return xrt_map_find_entry(m, key, xrt_hash32_value(key), xrt_value_type_tag(key)) >= 0;
}

static inline int xrt_map_delete(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_boolmap(m))
        return xrt_boolmap_delete_v((xrt_boolmap_t *) m, key);
    if (xrt_map_is_typed(m))
        return xrt_map_delete_typed(m, key);
    uint8_t key_tt = xrt_value_type_tag(key);
    uint32_t hash = xrt_hash32_value(key);
    int32_t eidx = -1;
    uint32_t slot = xrt_map_find_entry_slot(m, key, hash, key_tt, &eidx);
    if (slot == UINT32_MAX)
        return 0;
    m->entries[eidx].key_tt = XR_MAP_ENTRY_NIL_KEY;
    if (!(m->flags & XR_MAP_FLAG_WEAK)) {
        xrt_release(m->entries[eidx].key);
        xrt_release(m->entries[eidx].value);
    }
    m->entries[eidx].key = XR_NULL_VAL;
    m->entries[eidx].value = XR_NULL_VAL;
    m->indices[slot] = XR_MAP_IX_EMPTY;
    xr_swiss_ctrl_set(m->ctrl, m->indices_size, slot, XR_SWISS_CTRL_DELETED);
    m->count--;
    return 1;
}

static inline void xrt_map_set(xrt_map_t *m, XrValue key, XrValue val) {
    if (xrt_map_is_boolmap(m)) {
        xrt_boolmap_set_v((xrt_boolmap_t *) m, key, val);
        return;
    }
    if (xrt_map_is_typed(m)) {
        xrt_map_set_typed(m, key, val);
        return;
    }
    uint8_t key_tt = xrt_value_type_tag(key);
    uint32_t hash = xrt_hash32_value(key);
    int32_t eidx = xrt_map_find_entry(m, key, hash, key_tt);
    if (eidx >= 0) {
        if (!(m->flags & XR_MAP_FLAG_WEAK)) {
            xrt_release(key);
            xrt_release(m->entries[eidx].value);
        }
        m->entries[eidx].value = val;
        return;
    }
    if (m->nentries >= m->entries_cap)
        xrt_map_resize_tagged(m, m->count + 1);
    eidx = (int32_t) m->nentries++;
    XrMapEntry *entry = &m->entries[eidx];
    entry->key = key;
    entry->value = val;
    entry->hash = hash;
    entry->key_tt = key_tt;
    m->count++;
    xr_swiss_indices_put(m->ctrl, m->indices, m->indices_size, hash, eidx);
}

/* =========================================================================
 * Set runtime.
 *
 * Tagged sets use the shared hybrid-C ABI. Typed scalar sets temporarily keep
 * their packed SoA storage; 110 owns the typed-storage merge.
 * ========================================================================= */

typedef struct xrt_set_t {
    XrObjHeader hdr; /* embedded-at-0 header: same placement as the VM XrSet (C0
                      * object-header unification) */
    XR_SET_ABI_FIELDS;

    /* Typed scalar storage (elem_type != XR_ELEM_ANY). */
    int64_t len;
    int64_t cap;
    int64_t growth_left;
    void *items;
    int64_t *order;
    int64_t order_len;
    int64_t order_cap;
    uint8_t elem_type;
    uint8_t elem_size;
} xrt_set_t;

static inline void xrt_set_init_header(xrt_set_t *s, uint8_t elem_type) {
    xrt_bump_header_init(&s->hdr, XR_TSET);
    s->count = 0;
    s->nentries = 0;
    s->entries_cap = 0;
    s->indices_size = 0;
    s->ctrl = NULL;
    s->indices = NULL;
    s->entries = NULL;
    s->flags = XR_SET_FLAG_DUMMY;
    s->elem_tid = 0;
    s->len = 0;
    s->cap = 0;
    s->growth_left = 0;
    s->items = NULL;
    s->order = NULL;
    s->order_len = 0;
    s->order_cap = 0;
    s->elem_type = elem_type;
    s->elem_size = elem_type == XR_ELEM_ANY ? (uint8_t) sizeof(XrValue) : XR_ELEM_SIZES[elem_type];
}

static inline uint32_t xrt_set_find_entry_slot(xrt_set_t *s, XrValue value, uint32_t hash,
                                               int32_t *out_eidx) {
    return xr_set_lookup_slot(s->ctrl, s->indices, s->entries, s->indices_size, value, hash,
                              xrt_value_type_tag(value), xrt_set_value_eq, out_eidx);
}

static inline int32_t xrt_set_find_entry(xrt_set_t *s, XrValue value, uint32_t hash) {
    int32_t eidx = -1;
    return xrt_set_find_entry_slot(s, value, hash, &eidx) == UINT32_MAX ? -1 : eidx;
}

static inline void xrt_set_resize_tagged(xrt_set_t *s, uint32_t min_needed) {
    if (XR_UNLIKELY(s->flags & XR_SET_FLAG_NODES_ON_STACK)) {
        fprintf(stderr, "xrt_set_stack_new: capacity exceeded\n");
        abort();
    }

    XrSetEntry *old_entries = (s->flags & XR_SET_FLAG_DUMMY) ? NULL : s->entries;
    uint8_t *old_ctrl = (s->flags & XR_SET_FLAG_DUMMY) ? NULL : s->ctrl;
    int32_t *old_indices = (s->flags & XR_SET_FLAG_DUMMY) ? NULL : s->indices;
    uint32_t old_nentries = (s->flags & XR_SET_FLAG_DUMMY) ? 0 : s->nentries;

    uint32_t needed = s->count > min_needed ? s->count : min_needed;
    if (needed < 1)
        needed = 1;
    uint32_t indices_size = xrt_ordered_indices_size_for(needed, XR_SET_MAXHBITS);
    uint32_t entries_cap = (uint32_t) ((uint64_t) indices_size * 2 / 3);
    if (entries_cap < needed)
        entries_cap = needed;

    size_t cbytes = (size_t) indices_size + XR_SWISS_GROUP;
    size_t ibytes = sizeof(int32_t) * (size_t) indices_size;
    uint8_t *ctrl = (uint8_t *) XRT_MALLOC(cbytes);
    int32_t *indices = (int32_t *) XRT_MALLOC(ibytes);
    XrSetEntry *entries = (XrSetEntry *) XRT_CALLOC((size_t) entries_cap, sizeof(XrSetEntry));
    if (XR_UNLIKELY(!ctrl || !indices || !entries)) {
        fprintf(stderr, "xrt_set_resize_tagged: out of memory\n");
        abort();
    }
    memset(ctrl, (int) XR_SWISS_CTRL_EMPTY, cbytes);
    for (uint32_t i = 0; i < indices_size; i++)
        indices[i] = XR_SET_IX_EMPTY;

    uint32_t w =
        xr_set_rehash_into(entries, ctrl, indices, indices_size, old_entries, old_nentries);

    XRT_FREE(old_ctrl);
    XRT_FREE(old_indices);
    XRT_FREE(old_entries);

    s->ctrl = ctrl;
    s->indices = indices;
    s->entries = entries;
    s->indices_size = indices_size;
    s->entries_cap = entries_cap;
    s->nentries = w;
    s->flags &= (uint8_t) ~XR_SET_FLAG_DUMMY;
}

static inline void xrt_set_order_reserve(xrt_set_t *s, int64_t need) {
    if (need <= s->order_cap)
        return;
    int64_t new_cap = s->order_cap > 0 ? s->order_cap : 4;
    while (new_cap < need)
        new_cap *= 2;
    int64_t *tmp = (int64_t *) XRT_REALLOC(s->order, (size_t) new_cap * sizeof(int64_t));
    if (XR_UNLIKELY(!tmp)) {
        fprintf(stderr, "xrt_set_order_reserve: out of memory\n");
        abort();
    }
    s->order = tmp;
    s->order_cap = new_cap;
}

static inline void xrt_set_order_append(xrt_set_t *s, int64_t slot) {
    xrt_set_order_reserve(s, s->order_len + 1);
    s->order[s->order_len++] = slot;
}

static inline void xrt_set_alloc_slots(xrt_set_t *s, int64_t slots) {
    s->cap = slots;
    s->growth_left = slots - slots / 8;
    s->ctrl = xrt_swiss_ctrl_alloc(slots);
    s->items = XRT_CALLOC((size_t) slots, (size_t) s->elem_size);
    if (XR_UNLIKELY(!s->items)) {
        fprintf(stderr, "xrt_set: out of memory\n");
        abort();
    }
}

static inline XrValue xrt_set_new_typed(int64_t cap, uint8_t elem_type) {
    if (elem_type >= XR_ELEM_COUNT)
        elem_type = XR_ELEM_ANY;
    xrt_set_t *s = (xrt_set_t *) XRT_MALLOC(sizeof(xrt_set_t));
    if (XR_UNLIKELY(!s)) {
        fprintf(stderr, "xrt_set_new: out of memory\n");
        abort();
    }
    xrt_set_init_header(s, elem_type);
    xrt_coll_make_deterministic(&s->hdr);
    if (elem_type == XR_ELEM_ANY) {
        if (cap > 0)
            xrt_set_resize_tagged(s, (uint32_t) cap);
    } else {
        xrt_set_alloc_slots(s, xrt_swiss_slots_for(cap));
    }
    return xr_mkptr(s, XR_TAG_SET);
}

static inline XrValue xrt_set_new_flags(int64_t cap, uint8_t flags) {
    XrValue v = xrt_set_new_typed(cap, XR_ELEM_ANY);
    ((xrt_set_t *) v.ptr)->flags |= (uint8_t) (flags & XR_SET_FLAG_WEAK);
    return v;
}

static inline int xrt_set_is_typed(const xrt_set_t *s);

static inline void xrt_array_destroy(xrt_array_t *a) {
    if (!a)
        return;
    if (a->elem_type == XR_ELEM_ANY && a->data_storage != XR_ARRAY_DATA_BORROWED && a->data &&
        a->length > 0) {
        XrValue *items = (XrValue *) a->data;
        for (int64_t i = 0; i < a->length; i++)
            xrt_release(items[i]);
    }
    if (a->data_storage == XR_ARRAY_DATA_HEAP && a->data)
        XRT_FREE_ALIGNED(a->data);
    XRT_FREE(a);
}

static inline void xrt_map_destroy(xrt_map_t *m) {
    if (!m)
        return;
    if (!xrt_map_is_typed(m) && !(m->flags & (XR_MAP_FLAG_DUMMY | XR_MAP_FLAG_WEAK)) &&
        m->entries) {
        for (uint32_t i = 0; i < m->nentries; i++) {
            if (m->entries[i].key_tt == XR_MAP_ENTRY_NIL_KEY)
                continue;
            xrt_release(m->entries[i].key);
            xrt_release(m->entries[i].value);
        }
    }
    XRT_FREE(m->ctrl);
    XRT_FREE(m->indices);
    XRT_FREE(m->entries);
    XRT_FREE(m->keys);
    XRT_FREE(m->values);
    XRT_FREE(m->order);
    XRT_FREE(m);
}

static inline void xrt_set_destroy(xrt_set_t *s) {
    if (!s)
        return;
    if (!xrt_set_is_typed(s) && !(s->flags & (XR_SET_FLAG_DUMMY | XR_SET_FLAG_WEAK)) &&
        s->entries) {
        for (uint32_t i = 0; i < s->nentries; i++) {
            if (s->entries[i].val_tt == XR_SET_ENTRY_NIL)
                continue;
            xrt_release(s->entries[i].value);
        }
    }
    XRT_FREE(s->ctrl);
    XRT_FREE(s->indices);
    XRT_FREE(s->entries);
    XRT_FREE(s->items);
    XRT_FREE(s->order);
    XRT_FREE(s);
}

static inline void xrt_coll_retain(XrValue v) {
    XrObjHeader *h = (XrObjHeader *) v.ptr;
    if (!h || (h->extra & XR_OBJ_STORAGE_BUMP))
        return;
    atomic_fetch_add_explicit(&h->refcount, 1, memory_order_relaxed);
}

static inline void xrt_coll_release(XrValue v) {
    XrObjHeader *h = (XrObjHeader *) v.ptr;
    if (!h || (h->extra & XR_OBJ_STORAGE_BUMP))
        return;
    int32_t rc = atomic_load_explicit(&h->refcount, memory_order_relaxed);
    if (rc != 0) {
        atomic_store_explicit(&h->refcount, rc - 1, memory_order_relaxed);
        return;
    }
    switch (h->type) {
        case XR_TARRAY:
            xrt_array_destroy((xrt_array_t *) v.ptr);
            break;
        case XR_TMAP:
            xrt_map_destroy((xrt_map_t *) v.ptr);
            break;
        case XR_TBOOLMAP:
            xrt_boolmap_destroy((xrt_boolmap_t *) v.ptr);
            break;
        case XR_TSET:
            xrt_set_destroy((xrt_set_t *) v.ptr);
            break;
        default:
            break;
    }
}

static inline XrValue xrt_set_new(int64_t cap) {
    return xrt_set_new_typed(cap, XR_ELEM_ANY);
}

#ifndef xrt_set_stack_new
#define xrt_set_stack_new(cap_expr)                                                                \
    ({                                                                                             \
        int64_t _need64 = (cap_expr);                                                              \
        if (_need64 < 1)                                                                           \
            _need64 = 1;                                                                           \
        uint32_t _need = (uint32_t) _need64;                                                       \
        uint32_t _slots = xrt_ordered_indices_size_for(_need, XR_SET_MAXHBITS);                    \
        uint32_t _ecap = (uint32_t) ((uint64_t) _slots * 2u / 3u);                                 \
        if (_ecap < _need)                                                                         \
            _ecap = _need;                                                                         \
        xrt_set_t *_s = (xrt_set_t *) __builtin_alloca(sizeof(xrt_set_t));                         \
        uint8_t *_ctrl = (uint8_t *) __builtin_alloca((size_t) _slots + XR_SWISS_GROUP);           \
        int32_t *_indices = (int32_t *) __builtin_alloca(sizeof(int32_t) * (size_t) _slots);       \
        XrSetEntry *_entries =                                                                     \
            (XrSetEntry *) __builtin_alloca(sizeof(XrSetEntry) * (size_t) _ecap);                  \
        xrt_set_init_header(_s, XR_ELEM_ANY);                                                      \
        memset(_ctrl, (int) XR_SWISS_CTRL_EMPTY, (size_t) _slots + XR_SWISS_GROUP);                \
        for (uint32_t _i = 0; _i < _slots; _i++)                                                   \
            _indices[_i] = XR_SET_IX_EMPTY;                                                        \
        memset(_entries, 0, sizeof(XrSetEntry) * (size_t) _ecap);                                  \
        _s->ctrl = _ctrl;                                                                          \
        _s->indices = _indices;                                                                    \
        _s->entries = _entries;                                                                    \
        _s->indices_size = _slots;                                                                 \
        _s->entries_cap = _ecap;                                                                   \
        _s->flags = XR_SET_FLAG_NODES_ON_STACK;                                                    \
        xr_mkptr(_s, XR_TAG_SET);                                                                  \
    })
#endif

static inline int xrt_set_is_typed(const xrt_set_t *s) {
    return s && s->elem_type != XR_ELEM_ANY;
}

static inline int64_t xrt_set_len(const xrt_set_t *s) {
    return xrt_set_is_typed(s) ? s->len : (int64_t) s->count;
}

static inline int xrt_set_slot_is_full(const xrt_set_t *s, int64_t slot) {
    if (xrt_set_is_typed(s))
        return (s->ctrl[slot] & 0x80u) == 0;
    return slot >= 0 && (uint32_t) slot < s->nentries &&
           s->entries[slot].val_tt != XR_SET_ENTRY_NIL;
}

/* Slot accessor — valid only for FULL slots. */
static inline XrValue xrt_set_slot_item(xrt_set_t *s, int64_t slot) {
    if (!xrt_set_is_typed(s))
        return s->entries[slot].value;
    return xr_typed_get(s->items, (int32_t) slot, s->elem_type);
}

#include "xrt_set_direct.inc.c"

static inline int xrt_set_has(xrt_set_t *s, XrValue value) {
    if (!xrt_set_is_typed(s))
        return xrt_set_find_entry(s, value, xrt_hash32_value(value)) >= 0;
    return xrt_set_find_value(s, value) >= 0;
}

static inline int xrt_set_add(xrt_set_t *s, XrValue value) {
    if (!xrt_set_is_typed(s)) {
        uint32_t hash = xrt_hash32_value(value);
        if (xrt_set_find_entry(s, value, hash) >= 0) {
            if (!(s->flags & XR_SET_FLAG_WEAK))
                xrt_release(value);
            return 0;
        }
        if (s->nentries >= s->entries_cap)
            xrt_set_resize_tagged(s, s->count + 1);
        int32_t eidx = (int32_t) s->nentries++;
        XrSetEntry *entry = &s->entries[eidx];
        entry->value = value;
        entry->hash = hash;
        entry->val_tt = xrt_value_type_tag(value);
        s->count++;
        xr_swiss_indices_put(s->ctrl, s->indices, s->indices_size, hash, eidx);
        return 1;
    }
    if (xrt_set_find_value(s, value) >= 0)
        return 0;
    int64_t slot = xrt_set_insert_slot(s, xrt_set_hash_value(s, value));
    (void) xr_typed_set(s->items, (int32_t) slot, value, s->elem_type);
    return 1;
}

static inline int xrt_set_delete(xrt_set_t *s, XrValue value) {
    if (!xrt_set_is_typed(s)) {
        uint32_t hash = xrt_hash32_value(value);
        int32_t eidx = -1;
        uint32_t slot = xrt_set_find_entry_slot(s, value, hash, &eidx);
        if (slot == UINT32_MAX)
            return 0;
        if (!(s->flags & XR_SET_FLAG_WEAK))
            xrt_release(s->entries[eidx].value);
        s->entries[eidx].value = XR_NULL_VAL;
        s->entries[eidx].val_tt = XR_SET_ENTRY_NIL;
        s->indices[slot] = XR_SET_IX_EMPTY;
        xr_swiss_ctrl_set(s->ctrl, s->indices_size, slot, XR_SWISS_CTRL_DELETED);
        s->count--;
        return 1;
    }
    int64_t slot = xrt_set_find_value(s, value);
    if (slot < 0)
        return 0;
    xrt_set_erase_slot(s, slot);
    return 1;
}

static inline int xrt_set_has_i64(xrt_set_t *s, int64_t value) {
    if (s->elem_type != XR_ELEM_I64)
        return xrt_set_has(s, XR_FROM_INT(value));
    return xrt_set_find_i64_typed_slot(s, value, XR_ELEM_I64) >= 0;
}

static inline int xrt_set_add_i64(xrt_set_t *s, int64_t value) {
    if (s->elem_type != XR_ELEM_I64)
        return xrt_set_add(s, XR_FROM_INT(value));
    return xrt_set_add_i64_typed(s, value, XR_ELEM_I64);
}

static inline int xrt_set_delete_i64(xrt_set_t *s, int64_t value) {
    if (s->elem_type != XR_ELEM_I64)
        return xrt_set_delete(s, XR_FROM_INT(value));
    return xrt_set_delete_i64_typed(s, value, XR_ELEM_I64);
}

static inline void xrt_set_clear(xrt_set_t *s) {
    if (!xrt_set_is_typed(s)) {
        if (s->flags & XR_SET_FLAG_DUMMY)
            return;
        memset(s->ctrl, (int) XR_SWISS_CTRL_EMPTY, (size_t) s->indices_size + XR_SWISS_GROUP);
        for (uint32_t i = 0; i < s->indices_size; i++)
            s->indices[i] = XR_SET_IX_EMPTY;
        s->nentries = 0;
        s->count = 0;
        return;
    }
    memset(s->ctrl, (int) XRT_CTRL_EMPTY, (size_t) s->cap + XRT_GROUP);
    s->growth_left = s->cap - s->cap / 8;
    s->len = 0;
    s->order_len = 0;
}

static inline XrValue xrt_set_values(xrt_set_t *s) {
    XrValue arr = s->elem_type == XR_ELEM_ANY ? xrt_array_new(xrt_set_len(s))
                                              : xrt_array_new_typed(xrt_set_len(s), s->elem_type);
    if (!xrt_set_is_typed(s)) {
        for (uint32_t i = 0; i < s->nentries; i++) {
            if (s->entries[i].val_tt != XR_SET_ENTRY_NIL)
                xrt_array_push(arr, s->entries[i].value);
        }
        return arr;
    }
    for (int64_t oi = 0; oi < s->order_len; oi++) {
        int64_t slot = s->order[oi];
        if (xrt_set_slot_is_full(s, slot))
            xrt_array_push(arr, xrt_set_slot_item(s, slot));
    }
    return arr;
}

static inline XrValue xrt_map_keys(xrt_map_t *m) {
    if (xrt_map_is_boolmap(m))
        return xrt_boolmap_keys((xrt_boolmap_t *) m);
    XrValue arr = xrt_array_new(xrt_map_len(m));
    if (!xrt_map_is_typed(m)) {
        for (uint32_t i = 0; i < m->nentries; i++) {
            if (m->entries[i].key_tt != XR_MAP_ENTRY_NIL_KEY)
                xrt_array_push(arr, m->entries[i].key);
        }
        return arr;
    }
    for (int64_t oi = 0; oi < m->order_len; oi++) {
        int64_t slot = m->order[oi];
        if (xrt_map_slot_is_full(m, slot))
            xrt_array_push(arr, xrt_map_slot_key(m, slot));
    }
    return arr;
}

static inline XrValue xrt_map_values(xrt_map_t *m) {
    if (xrt_map_is_boolmap(m))
        return xrt_boolmap_values((xrt_boolmap_t *) m);
    XrValue arr = xrt_array_new(xrt_map_len(m));
    if (!xrt_map_is_typed(m)) {
        for (uint32_t i = 0; i < m->nentries; i++) {
            if (m->entries[i].key_tt != XR_MAP_ENTRY_NIL_KEY)
                xrt_array_push(arr, m->entries[i].value);
        }
        return arr;
    }
    for (int64_t oi = 0; oi < m->order_len; oi++) {
        int64_t slot = m->order[oi];
        if (xrt_map_slot_is_full(m, slot))
            xrt_array_push(arr, xrt_map_slot_value(m, slot));
    }
    return arr;
}

/* Value at the given insertion-order index. Used by enum `for-in` lowering
 * (getMember) since a user enum is materialized as an insertion-ordered map. */
static inline XrValue xrt_map_value_at(xrt_map_t *m, int64_t index) {
    if (!m || index < 0)
        return XR_NULL_VAL;
    if (!xrt_map_is_typed(m)) {
        int64_t out = 0;
        for (uint32_t i = 0; i < m->nentries; i++) {
            if (m->entries[i].key_tt != XR_MAP_ENTRY_NIL_KEY) {
                if (out == index)
                    return m->entries[i].value;
                out++;
            }
        }
        return XR_NULL_VAL;
    }
    int64_t out = 0;
    for (int64_t oi = 0; oi < m->order_len; oi++) {
        int64_t slot = m->order[oi];
        if (xrt_map_slot_is_full(m, slot)) {
            if (out == index)
                return xrt_map_slot_value(m, slot);
            out++;
        }
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_set_value_at(xrt_set_t *s, int64_t index) {
    if (index < 0)
        return XR_NULL_VAL;
    if (xrt_set_is_typed(s)) {
        // order[] may hold tombstoned slots (lazy delete); walk live slots in
        // insertion order to the index-th one.
        int64_t out = 0;
        for (int64_t oi = 0; oi < s->order_len; oi++) {
            int64_t slot = s->order[oi];
            if (!xrt_set_slot_is_full(s, slot))
                continue;
            if (out == index)
                return xrt_set_slot_item(s, slot);
            out++;
        }
        return XR_NULL_VAL;
    }
    int64_t out = 0;
    for (uint32_t i = 0; i < s->nentries; i++) {
        if (s->entries[i].val_tt == XR_SET_ENTRY_NIL)
            continue;
        if (out == index)
            return s->entries[i].value;
        out++;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_set_value_at_owned(xrt_set_t *s, int64_t index) {
    return xrt_value_to_owned(xrt_set_value_at(s, index));
}

/* =========================================================================
 * Iterator runtime — backs the for-in iterator protocol over Map / Set / string.
 * The iterator borrows its source by value (no extra RC: AOT collections are
 * not individually reclaimed) and walks dense entries, typed order[], or UTF-8
 * scalar boundaries.
 * ========================================================================= */

#define XRT_ITER_KEYS 0      /* map: yield key */
#define XRT_ITER_VALUES 1    /* set: yield value; map: yield value */
#define XRT_ITER_PAIRS 2     /* map/string: yield (key, value) tuple */
#define XRT_ITER_GENERATOR 3 /* coroutine-backed generator: pull-driven via gen_drive */

typedef struct XrCoroutine XrCoroutine;

typedef struct {
    XrValue coll;     /* XR_TAG_MAP, XR_TAG_SET, or string being iterated */
    int64_t cursor;   /* collection cursor, or generator phase: 0=idle 1=buffered 2=done */
    int64_t index;    /* logical iteration index; used by string pair iteration */
    uint8_t kind;     /* XRT_ITER_* projection */
    XrCoroutine *gen; /* XRT_ITER_GENERATOR only; owns the producer coroutine */
} xrt_iterator_t;

/* Generator iterator pull helpers live in xray_rt_coro. Freestanding AOT
 * collection users must not reference them or they would pull in the coroutine
 * runtime for plain Map/Set/String iteration. */
#ifdef XRT_ENABLE_GENERATORS
XR_FUNC int xrt_gen_iter_has_next(xrt_iterator_t *it);
XR_FUNC XrValue xrt_gen_iter_next(xrt_iterator_t *it);
#endif

static inline XrValue xrt_iterator_new(XrValue coll, uint8_t kind) {
    xrt_iterator_t *it = (xrt_iterator_t *) XRT_MALLOC(sizeof(xrt_iterator_t));
    if (XR_UNLIKELY(!it)) {
        fprintf(stderr, "xrt_iterator_new: out of memory\n");
        abort();
    }
    it->coll = coll;
    it->cursor = 0;
    it->index = 0;
    it->kind = kind;
    it->gen = NULL;
    return xr_mkptr(it, XR_TAG_ITERATOR);
}

static inline int xrt_iter_utf8_decode_scalar(const char *s, int64_t slen, int64_t *cursor,
                                              uint32_t *out_cp) {
    if (!s || !cursor || !out_cp || *cursor < 0 || *cursor >= slen)
        return 0;
    const unsigned char *p = (const unsigned char *) s + *cursor;
    const unsigned char *end = (const unsigned char *) s + slen;
    unsigned char b0 = p[0];
    uint32_t cp = 0;
    int size = 0;
    if (b0 < 0x80u) {
        cp = b0;
        size = 1;
    } else if ((b0 & 0xE0u) == 0xC0u) {
        if (p + 2 > end || (p[1] & 0xC0u) != 0x80u)
            goto invalid;
        cp = ((uint32_t) (b0 & 0x1Fu) << 6) | (uint32_t) (p[1] & 0x3Fu);
        if (cp < 0x80u)
            goto invalid;
        size = 2;
    } else if ((b0 & 0xF0u) == 0xE0u) {
        if (p + 3 > end || (p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u)
            goto invalid;
        cp = ((uint32_t) (b0 & 0x0Fu) << 12) | ((uint32_t) (p[1] & 0x3Fu) << 6) |
             (uint32_t) (p[2] & 0x3Fu);
        if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))
            goto invalid;
        size = 3;
    } else if ((b0 & 0xF8u) == 0xF0u) {
        if (p + 4 > end || (p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u ||
            (p[3] & 0xC0u) != 0x80u)
            goto invalid;
        cp = ((uint32_t) (b0 & 0x07u) << 18) | ((uint32_t) (p[1] & 0x3Fu) << 12) |
             ((uint32_t) (p[2] & 0x3Fu) << 6) | (uint32_t) (p[3] & 0x3Fu);
        if (cp < 0x10000u || cp > 0x10FFFFu)
            goto invalid;
        size = 4;
    } else {
        goto invalid;
    }
    *cursor += size;
    *out_cp = cp;
    return 1;

invalid:
    (*cursor)++;
    *out_cp = 0;
    return 0;
}

// Park cursor at the next live entry/order[] slot; return 1 if one exists.
static inline int xrt_iterator_has_next(xrt_iterator_t *it) {
    if (it->kind == XRT_ITER_GENERATOR) {
#ifdef XRT_ENABLE_GENERATORS
        return xrt_gen_iter_has_next(it);
#else
        return 0;
#endif
    }
    if (XR_IS_MAP(it->coll)) {
        xrt_map_t *m = (xrt_map_t *) it->coll.ptr;
        if (xrt_map_is_boolmap(m))
            return it->cursor < xrt_boolmap_len((const xrt_boolmap_t *) m);
        if (xrt_map_is_typed(m)) {
            while (it->cursor < m->order_len) {
                if (xrt_map_slot_is_full(m, m->order[it->cursor]))
                    return 1;
                it->cursor++;
            }
        } else {
            while ((uint32_t) it->cursor < m->nentries) {
                if (xrt_map_slot_is_full(m, it->cursor))
                    return 1;
                it->cursor++;
            }
        }
        return 0;
    }
    if (XR_IS_SET(it->coll)) {
        xrt_set_t *s = (xrt_set_t *) it->coll.ptr;
        if (xrt_set_is_typed(s)) {
            while (it->cursor < s->order_len) {
                if (xrt_set_slot_is_full(s, s->order[it->cursor]))
                    return 1;
                it->cursor++;
            }
        } else {
            while ((uint32_t) it->cursor < s->nentries) {
                if (xrt_set_slot_is_full(s, it->cursor))
                    return 1;
                it->cursor++;
            }
        }
        return 0;
    }
    if (XR_IS_STR(it->coll))
        return it->cursor < xr_str_len(it->coll);
    return 0;
}

static inline XrValue xrt_iterator_next(xrt_iterator_t *it) {
    if (it->kind == XRT_ITER_GENERATOR) {
#ifdef XRT_ENABLE_GENERATORS
        return xrt_gen_iter_next(it);
#else
        return XR_NULL_VAL;
#endif
    }
    if (!xrt_iterator_has_next(it))
        return XR_NULL_VAL;
    if (XR_IS_MAP(it->coll)) {
        xrt_map_t *m = (xrt_map_t *) it->coll.ptr;
        if (xrt_map_is_boolmap(m)) {
            xrt_boolmap_t *b = (xrt_boolmap_t *) m;
            int64_t cursor = it->cursor++;
            if (it->kind == XRT_ITER_PAIRS) {
                XrValue kv[2] = {xrt_boolmap_iter_key(b, cursor),
                                 xrt_boolmap_iter_value(b, cursor)};
                return xrt_tuple_make(2, kv);
            }
            if (it->kind == XRT_ITER_VALUES)
                return xrt_boolmap_iter_value(b, cursor);
            return xrt_boolmap_iter_key(b, cursor);
        }
        int64_t slot = xrt_map_is_typed(m) ? m->order[it->cursor++] : it->cursor++;
        if (it->kind == XRT_ITER_PAIRS) {
            XrValue kv[2] = {xrt_map_slot_key(m, slot), xrt_map_slot_value(m, slot)};
            return xrt_tuple_make(2, kv);
        }
        if (it->kind == XRT_ITER_VALUES)
            return xrt_map_slot_value(m, slot);
        return xrt_map_slot_key(m, slot);
    }
    if (XR_IS_SET(it->coll)) {
        xrt_set_t *s = (xrt_set_t *) it->coll.ptr;
        int64_t slot = xrt_set_is_typed(s) ? s->order[it->cursor++] : it->cursor++;
        return xrt_set_slot_item(s, slot);
    }
    if (XR_IS_STR(it->coll)) {
        int64_t char_index = it->index++;
        uint32_t cp = 0;
        int ok = xrt_iter_utf8_decode_scalar(xr_str_data(it->coll), xr_str_len(it->coll),
                                             &it->cursor, &cp);
        XrValue ch = ok ? XR_FROM_CHAR(cp) : XR_NULL_VAL;
        if (it->kind == XRT_ITER_KEYS)
            return XR_FROM_INT(char_index);
        if (it->kind == XRT_ITER_PAIRS) {
            XrValue kv[2] = {XR_FROM_INT(char_index), ch};
            return xrt_tuple_make(2, kv);
        }
        return ch;
    }
    return XR_NULL_VAL;
}

static inline XrValue xrt_process_new(const char *file, int argc, char **argv, const char *dir) {
    if (argc < 0)
        argc = 0;
    XrValue args = xrt_array_new(argc);
    for (int i = 0; argv && i < argc; i++)
        xrt_array_push(args, xr_box_str(argv[i] ? argv[i] : ""));

    XrValue process = xrt_map_new(4);
    xrt_map_t *m = (xrt_map_t *) process.ptr;
    xrt_map_set(m, xr_box_str("file"), file ? xr_box_str(file) : XR_NULL_VAL);
    xrt_map_set(m, xr_box_str("args"), args);
    xrt_map_set(m, xr_box_str("dir"), dir ? xr_box_str(dir) : XR_NULL_VAL);
    return process;
}

/* =========================================================================
 * Json object runtime (flat field array, O(1) indexed access)
 * ========================================================================= */

enum {
    XRT_OBJECT_JSON = 0,
    XRT_OBJECT_RECORD = 1,
};

typedef struct {
    int64_t field_count;
    uint8_t object_kind;
    const char **field_names;
    xrt_map_t *dynamic_fields;
    XrValue fields[]; /* flexible array of field values */
} xrt_json_t;

static inline XrValue xrt_value_clone_for_coro(XrValue val);

static inline XrValue xrt_object_new_kind(int64_t field_count, uint8_t object_kind) {
    xrt_json_t *j =
        (xrt_json_t *) XRT_MALLOC(sizeof(xrt_json_t) + (size_t) field_count * sizeof(XrValue));
    if (XR_UNLIKELY(!j)) {
        fprintf(stderr, "xrt_object_new: out of memory\n");
        abort();
    }
    j->field_count = field_count;
    j->object_kind = object_kind;
    j->field_names = NULL;
    j->dynamic_fields = NULL;
    for (int64_t i = 0; i < field_count; i++)
        j->fields[i] = (XrValue) {.i = 0, .tag = XR_TAG_NULL};
    return xr_mkptr(j, XR_TAG_PTR);
}

static inline XrValue xrt_object_new_named_kind(int64_t field_count, const char *const *field_names,
                                                uint8_t object_kind) {
    XrValue obj = xrt_object_new_kind(field_count, object_kind);
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    if (field_count <= 0 || !field_names)
        return obj;
    j->field_names = (const char **) XRT_MALLOC((size_t) field_count * sizeof(const char *));
    if (XR_UNLIKELY(!j->field_names)) {
        fprintf(stderr, "xrt_json_new_named: out of memory\n");
        abort();
    }
    for (int64_t i = 0; i < field_count; i++)
        j->field_names[i] = field_names[i] ? field_names[i] : "?";
    return obj;
}

static inline XrValue xrt_json_new(int64_t field_count) {
    return xrt_object_new_kind(field_count, XRT_OBJECT_JSON);
}

static inline XrValue xrt_json_new_named(int64_t field_count, const char *const *field_names) {
    return xrt_object_new_named_kind(field_count, field_names, XRT_OBJECT_JSON);
}

static inline XrValue xrt_record_new(int64_t field_count) {
    return xrt_object_new_kind(field_count, XRT_OBJECT_RECORD);
}

static inline XrValue xrt_record_new_named(int64_t field_count, const char *const *field_names) {
    return xrt_object_new_named_kind(field_count, field_names, XRT_OBJECT_RECORD);
}

static inline int64_t xrt_json_find_field(xrt_json_t *j, const char *name) {
    if (!j || !name || !j->field_names)
        return -1;
    for (int64_t i = 0; i < j->field_count; i++) {
        if (j->field_names[i] && strcmp(j->field_names[i], name) == 0)
            return i;
    }
    return -1;
}

static inline XrValue xrt_json_get_field(XrValue obj, int field_idx) {
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    if (field_idx >= 0 && field_idx < j->field_count)
        return j->fields[field_idx];
    return (XrValue) {.i = 0, .tag = XR_TAG_NULL};
}

static inline void xrt_json_set_field(XrValue obj, int field_idx, XrValue val) {
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    if (field_idx >= 0 && field_idx < j->field_count)
        j->fields[field_idx] = val;
}

static inline XrValue xrt_json_get_name(XrValue obj, const char *name) {
    if (obj.tag != XR_TAG_PTR || !obj.ptr || !name)
        return XR_NULL_VAL;
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    int64_t idx = xrt_json_find_field(j, name);
    if (idx >= 0)
        return j->fields[idx];
    return j->dynamic_fields ? xrt_map_get(j->dynamic_fields, xr_box_str(name)) : XR_NULL_VAL;
}

static inline XrValue xrt_json_get_name_owned(XrValue obj, const char *name) {
    return xrt_value_to_owned(xrt_json_get_name(obj, name));
}

static inline XrValue xrt_json_set_name(XrValue obj, const char *name, XrValue val) {
    if (obj.tag != XR_TAG_PTR || !obj.ptr || !name)
        return val;
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    int64_t idx = xrt_json_find_field(j, name);
    if (idx >= 0) {
        j->fields[idx] = val;
        return val;
    }
    if (!j->dynamic_fields) {
        XrValue dyn = xrt_map_new(8);
        j->dynamic_fields = (xrt_map_t *) dyn.ptr;
    }
    xrt_map_set(j->dynamic_fields, xr_box_str(name), val);
    return val;
}

static inline XrValue xrt_json_encode_value(XrValue val, int depth);

#define XRT_JSON_ENCODE_MAX_DEPTH 512

static inline void xrt_json_encode_abort(const char *msg, XrValue val) {
    fprintf(stderr, "Json.encode: %s (tag=%u, heap_type=%u)\n", msg ? msg : "unsupported value",
            (unsigned) val.tag, (unsigned) val.heap_type);
    abort();
}

static inline void xrt_json_put_string_key(XrValue obj, XrValue key, XrValue val) {
    if (!XR_IS_STR(key))
        xrt_json_encode_abort("object key must be string", key);
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    const char *name = xr_str_data(key);
    int64_t idx = xrt_json_find_field(j, name);
    if (idx >= 0) {
        j->fields[idx] = val;
        return;
    }
    if (!j->dynamic_fields) {
        XrValue dyn = xrt_map_new(8);
        j->dynamic_fields = (xrt_map_t *) dyn.ptr;
    }
    xrt_map_set(j->dynamic_fields, key, val);
}

static inline XrValue xrt_json_encode_map_key(XrValue key) {
    if (XR_IS_STR(key))
        return key;
    if (XR_IS_INT(key)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long) XR_TO_INT(key));
        return xrt_str_from_cstr(buf);
    }
    xrt_json_encode_abort("Map keys must be string or int", key);
    return XR_NULL_VAL;
}

static inline XrValue xrt_json_encode_array_value(xrt_array_t *src, int depth) {
    XrValue dst = xrt_array_new(src ? src->length : 0);
    if (!src)
        return dst;
    for (int64_t i = 0; i < src->length; i++) {
        XrValue item = xr_typed_get(src->data, (int32_t) i, src->elem_type);
        xrt_array_push(dst, xrt_json_encode_value(item, depth + 1));
    }
    return dst;
}

static inline XrValue xrt_json_encode_object_value(xrt_json_t *src, int depth) {
    if (!src)
        return xrt_json_new(0);
    XrValue dstv = xrt_json_new_named(src->field_count, src->field_names);
    xrt_json_t *dst = (xrt_json_t *) dstv.ptr;
    for (int64_t i = 0; i < src->field_count; i++)
        dst->fields[i] = xrt_json_encode_value(src->fields[i], depth + 1);
    if (src->dynamic_fields) {
        xrt_map_t *m = src->dynamic_fields;
        for (uint32_t i = 0; i < m->nentries; i++) {
            XrMapEntry *entry = &m->entries[i];
            if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
                continue;
            xrt_json_put_string_key(dstv, entry->key,
                                    xrt_json_encode_value(entry->value, depth + 1));
        }
    }
    return dstv;
}

static inline XrValue xrt_json_encode_map_value(xrt_map_t *src, int depth) {
    XrValue dst = xrt_json_new(0);
    if (!src)
        return dst;
    if (xrt_map_is_boolmap(src))
        xrt_json_encode_abort("Map keys must be string or int", XR_FROM_BOOL(1));
    if (!xrt_map_is_typed(src)) {
        for (uint32_t i = 0; i < src->nentries; i++) {
            XrMapEntry *entry = &src->entries[i];
            if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
                continue;
            XrValue key = xrt_json_encode_map_key(entry->key);
            xrt_json_put_string_key(dst, key, xrt_json_encode_value(entry->value, depth + 1));
        }
        return dst;
    }
    for (int64_t oi = 0; oi < src->order_len; oi++) {
        int64_t slot = src->order[oi];
        if (!xrt_map_slot_is_full(src, slot))
            continue;
        XrValue key = xrt_json_encode_map_key(xrt_map_slot_key(src, slot));
        xrt_json_put_string_key(dst, key,
                                xrt_json_encode_value(xrt_map_slot_value(src, slot), depth + 1));
    }
    return dst;
}

static inline XrValue xrt_json_encode_set_value(xrt_set_t *src, int depth) {
    XrValue dst = xrt_array_new(src ? xrt_set_len(src) : 0);
    if (!src)
        return dst;
    if (!xrt_set_is_typed(src)) {
        for (uint32_t i = 0; i < src->nentries; i++) {
            if (src->entries[i].val_tt == XR_SET_ENTRY_NIL)
                continue;
            xrt_array_push(dst, xrt_json_encode_value(src->entries[i].value, depth + 1));
        }
        return dst;
    }
    for (int64_t oi = 0; oi < src->order_len; oi++) {
        int64_t slot = src->order[oi];
        if (xrt_set_slot_is_full(src, slot))
            xrt_array_push(dst, xrt_json_encode_value(xrt_set_slot_item(src, slot), depth + 1));
    }
    return dst;
}

static inline XrValue xrt_json_encode_value(XrValue val, int depth) {
    if (depth > XRT_JSON_ENCODE_MAX_DEPTH)
        xrt_json_encode_abort("value is too deeply nested", val);
    switch (xrt_value_kind(val)) {
        case XR_TAG_NULL:
        case XR_TAG_BOOL:
        case XR_TAG_I64:
        case XR_TAG_STR:
        case XR_TAG_STR_ARC:
            return val;
        case XR_TAG_F64:
            return isfinite(val.f) ? val : XR_NULL_VAL;
        case XR_TAG_CHAR: {
            char buf[4];
            int n = xrt_char_utf8_encode(XR_TO_CHAR(val), buf);
            if (n <= 0)
                return XR_NULL_VAL;
            XrValue out = xrt_str_alloc((size_t) n);
            memcpy(xr_str_buf(out), buf, (size_t) n);
            return out;
        }
        case XR_TAG_ENUM:
            return xrt_enum_value_name(val);
        case XR_TAG_ARRAY:
            return xrt_json_encode_array_value((xrt_array_t *) val.ptr, depth);
        case XR_TAG_MAP:
            return xrt_json_encode_map_value((xrt_map_t *) val.ptr, depth);
        case XR_TAG_SET:
            return xrt_json_encode_set_value((xrt_set_t *) val.ptr, depth);
        case XR_TAG_PTR:
            if (val.ptr && val.heap_type == 0)
                return xrt_json_encode_object_value((xrt_json_t *) val.ptr, depth);
            break;
        default:
            break;
    }
    xrt_json_encode_abort("cannot encode value to JSON", val);
    return XR_NULL_VAL;
}

static inline XrValue xrt_json_encode(XrValue val) {
    return xrt_json_encode_value(val, 0);
}

static inline XrValue xrt_getprop_name(XrValue obj, const char *name) {
    if (obj.tag == XR_TAG_ENUM) {
        if (!name)
            return XR_NULL_VAL;
        if (strcmp(name, "name") == 0)
            return xrt_enum_value_name(obj);
        if (strcmp(name, "value") == 0)
            return xrt_enum_value_raw(obj);
        if (strcmp(name, "ordinal") == 0)
            return xrt_enum_value_ordinal(obj);
    }
    if (XR_IS_MAP(obj))
        return xrt_map_get_owned((xrt_map_t *) obj.ptr, xr_box_str(name));
    if (obj.tag == XR_TAG_PTR && obj.ptr && obj.heap_type == 0)
        return xrt_json_get_name_owned(obj, name);
    return XR_NULL_VAL;
}

static inline XrValue xrt_setprop_name(XrValue obj, const char *name, XrValue val) {
    if (XR_IS_MAP(obj)) {
        xrt_map_set((xrt_map_t *) obj.ptr, xr_box_str(name), val);
        return val;
    }
    if (obj.tag == XR_TAG_PTR && obj.ptr && obj.heap_type == 0)
        return xrt_json_set_name(obj, name, val);
    return val;
}

static inline XrValue xrt_json_clone_for_coro(XrValue val) {
    if (val.tag != XR_TAG_PTR || !val.ptr)
        return val;
    xrt_json_t *src = (xrt_json_t *) val.ptr;
    XrValue dstv = xrt_object_new_named_kind(src->field_count, src->field_names, src->object_kind);
    xrt_json_t *dst = (xrt_json_t *) dstv.ptr;
    for (int64_t i = 0; i < src->field_count; i++)
        dst->fields[i] = xrt_value_clone_for_coro(src->fields[i]);
    if (src->dynamic_fields) {
        XrValue dyn = xrt_map_new(xrt_map_len(src->dynamic_fields));
        xrt_map_t *dst_map = (xrt_map_t *) dyn.ptr;
        for (uint32_t i = 0; i < src->dynamic_fields->nentries; i++) {
            XrMapEntry *entry = &src->dynamic_fields->entries[i];
            if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
                continue;
            xrt_map_set(dst_map, xrt_value_clone_for_coro(entry->key),
                        xrt_value_clone_for_coro(entry->value));
        }
        dst->dynamic_fields = dst_map;
    }
    return dstv;
}

/* Object spread `{...src}`: copy every field of `src` into `dst`, overwriting
 * existing keys so later spread parts / literal fields win.  AOT json fields
 * are kept alive by escape analysis (the spread source is HEAP_ESCAPE), so no
 * per-value retain is needed — this mirrors how object-literal fields are
 * stored without an explicit dup. */
static inline void xrt_json_merge(XrValue dst_val, XrValue src_val) {
    if (dst_val.tag != XR_TAG_PTR || !dst_val.ptr)
        return;
    if (src_val.tag != XR_TAG_PTR || !src_val.ptr)
        return;
    xrt_json_t *src = (xrt_json_t *) src_val.ptr;
    for (int64_t i = 0; i < src->field_count; i++) {
        const char *name = src->field_names ? src->field_names[i] : NULL;
        if (name)
            xrt_json_set_name(dst_val, name, src->fields[i]);
    }
    if (src->dynamic_fields) {
        xrt_map_t *m = src->dynamic_fields;
        for (uint32_t i = 0; i < m->nentries; i++) {
            XrMapEntry *entry = &m->entries[i];
            if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
                continue;
            if (XR_IS_STR(entry->key))
                xrt_json_set_name(dst_val, xr_str_data(entry->key), entry->value);
        }
    }
}

#include "xrt_index_helpers.inc.c"

/* =========================================================================
 * Closure runtime
 * ========================================================================= */

typedef struct xrt_closure {
    void *fn;          // C function pointer
    int nupvals;       // number of captured upvalues
    XrValue upvals[];  // captured values (flexible array)
} xrt_closure_t;

static inline size_t xrt_closure_object_size(int nupvals) {
    if (nupvals < 0)
        nupvals = 0;
    if ((size_t) nupvals > (SIZE_MAX - sizeof(xrt_closure_t)) / sizeof(XrValue)) {
        fprintf(stderr, "xrt_closure_new: allocation size overflow\n");
        abort();
    }
    return sizeof(xrt_closure_t) + (size_t) nupvals * sizeof(XrValue);
}

static inline void xrt_closure_init(xrt_closure_t *c, void *fn, int nupvals) {
    if (nupvals < 0)
        nupvals = 0;
    c->fn = fn;
    c->nupvals = nupvals;
    for (int i = 0; i < nupvals; i++)
        c->upvals[i] = XR_NULL_VAL;
}

static inline XrValue xrt_closure_new(void *fn, int nupvals) {
    xrt_closure_t *c = (xrt_closure_t *) xrt_arc_alloc(xrt_closure_object_size(nupvals));
    if (XR_UNLIKELY(!c)) {
        fprintf(stderr, "xrt_closure_new: out of memory\n");
        abort();
    }
    xrt_arc_mark_builtin(c, XRT_ARC_KIND_CLOSURE);
    xrt_closure_init(c, fn, nupvals);
    return xr_mkptr(c, XR_TAG_CLOSURE);
}

#ifndef xrt_closure_stack_new
#define xrt_closure_stack_new(fn_expr, nupvals_expr)                                               \
    ({                                                                                             \
        int _nupvals = (nupvals_expr);                                                             \
        if (_nupvals < 0)                                                                          \
            _nupvals = 0;                                                                          \
        size_t _obj_size = xrt_closure_object_size(_nupvals);                                      \
        XrObjHeader *_hdr = (XrObjHeader *) __builtin_alloca(sizeof(XrObjHeader) + _obj_size);     \
        memset(_hdr, 0, sizeof(XrObjHeader) + _obj_size);                                          \
        _hdr->extra = XR_OBJ_STORAGE_STACK;                                                        \
        xrt_closure_t *_c = (xrt_closure_t *) ((char *) _hdr + sizeof(XrObjHeader));               \
        xrt_closure_init(_c, (fn_expr), _nupvals);                                                 \
        xr_mkptr(_c, XR_TAG_CLOSURE);                                                              \
    })
#endif

/* Post-header field set shared with the VM's XrCell (src/shared/xr_cell_abi.h)
 * so the AOT and VM cell layouts stay in lockstep. */
typedef struct xrt_cell {
    XR_CELL_ABI_FIELDS;
} xrt_cell_t;

#ifdef XRT_ENABLE_REGEX
static inline void xrt_regex_destroy_builtin(void *obj);
#endif

static inline XrValue xrt_cell_new(XrValue value) {
    xrt_cell_t *cell = (xrt_cell_t *) xrt_arc_alloc(sizeof(xrt_cell_t));
    if (XR_UNLIKELY(!cell)) {
        fprintf(stderr, "xrt_cell_new: out of memory\n");
        abort();
    }
    xrt_arc_mark_builtin(cell, XRT_ARC_KIND_CELL);
    cell->value = value;
    xrt_retain(value);
    return xr_mkptr(cell, XR_TAG_CELL);
}

static inline XrValue xrt_cell_get(XrValue cell_value) {
    if (cell_value.tag != XR_TAG_CELL || !cell_value.ptr)
        return cell_value;
    return ((xrt_cell_t *) cell_value.ptr)->value;
}

static inline void xrt_cell_set(XrValue cell_value, XrValue value) {
    if (cell_value.tag != XR_TAG_CELL || !cell_value.ptr)
        return;
    xrt_cell_t *cell = (xrt_cell_t *) cell_value.ptr;
    XrValue old = cell->value;
    xrt_retain(value);
    cell->value = value;
    xrt_release(old);
}

static inline void xrt_dispatch_builtin_destructor(uint32_t kind, void *obj) {
    if (!obj)
        return;
    switch (kind) {
        case XRT_ARC_KIND_CLOSURE: {
            xrt_closure_t *c = (xrt_closure_t *) obj;
            for (int i = 0; i < c->nupvals; i++)
                xrt_release(c->upvals[i]);
            break;
        }
        case XRT_ARC_KIND_CELL:
            xrt_release(((xrt_cell_t *) obj)->value);
            break;
#ifdef XRT_ENABLE_REGEX
        case XRT_ARC_KIND_REGEX:
            xrt_regex_destroy_builtin(obj);
            break;
#endif
        default:
            break;
    }
}

static inline XrValue xrt_value_clone_for_coro(XrValue val) {
    switch (xrt_value_kind(val)) {
        case XR_TAG_STR:
            return val;
        case XR_TAG_STR_ARC: {
            xrt_str_t *src = (xrt_str_t *) val.ptr;
            if (!src)
                return val;
            XrValue dstv = xrt_str_alloc((size_t) src->len);
            xrt_str_t *dst = (xrt_str_t *) dstv.ptr;
            dst->hash = src->hash;
            memcpy(xr_str_buf(dstv), src->data, (size_t) src->len);
            return dstv;
        }
        case XR_TAG_ARRAY: {
            xrt_array_t *src = (xrt_array_t *) val.ptr;
            if (!src)
                return val;
            XrValue dstv = xrt_array_new_typed(src->capacity, src->elem_type);
            xrt_array_t *dst = (xrt_array_t *) dstv.ptr;
            dst->length = src->length;
            dst->adt_enum_name = src->adt_enum_name;
            dst->adt_member_name = src->adt_member_name;
            if (src->elem_type == XR_ELEM_ANY) {
                for (int64_t i = 0; i < src->length; i++) {
                    XrValue item = xr_typed_get(src->data, (int32_t) i, src->elem_type);
                    xr_typed_set(dst->data, (int32_t) i, xrt_value_clone_for_coro(item),
                                 dst->elem_type);
                }
            } else {
                memcpy(dst->data, src->data, (size_t) src->length * (size_t) src->elem_size);
            }
            return dstv;
        }
        case XR_TAG_MAP: {
            xrt_map_t *src = (xrt_map_t *) val.ptr;
            if (!src)
                return val;
            if (xrt_map_is_boolmap(src))
                return xrt_boolmap_clone((const xrt_boolmap_t *) src);
            if (!xrt_map_is_typed(src) && (src->flags & XR_MAP_FLAG_WEAK))
                return xrt_map_new_flags(0, XR_MAP_FLAG_WEAK);
            XrValue dstv = xrt_map_is_typed(src)
                               ? xrt_map_new_typed(src->len, src->key_type, src->value_type)
                               : xrt_map_new(xrt_map_len(src));
            xrt_map_t *dst = (xrt_map_t *) dstv.ptr;
            if (xrt_map_is_typed(src) && dst->cap == src->cap) {
                /* Same geometry: clone the slot arrays and control bytes. */
                dst->len = src->len;
                dst->growth_left = src->growth_left;
                memcpy(dst->ctrl, src->ctrl, (size_t) src->cap + XRT_GROUP);
                memcpy(dst->keys, src->keys, (size_t) src->cap * (size_t) src->key_size);
                memcpy(dst->values, src->values, (size_t) src->cap * (size_t) src->value_size);
                xrt_map_order_reserve(dst, src->order_len);
                memcpy(dst->order, src->order, (size_t) src->order_len * sizeof(int64_t));
                dst->order_len = src->order_len;
            } else if (!xrt_map_is_typed(src)) {
                for (uint32_t i = 0; i < src->nentries; i++) {
                    XrMapEntry *entry = &src->entries[i];
                    if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
                        continue;
                    XrValue cloned_key = xrt_value_clone_for_coro(entry->key);
                    XrValue cloned_val = xrt_value_clone_for_coro(entry->value);
                    xrt_map_set(dst, cloned_key, cloned_val);
                }
            } else {
                for (int64_t oi = 0; oi < src->order_len; oi++) {
                    int64_t slot = src->order[oi];
                    if (!xrt_map_slot_is_full(src, slot))
                        continue;
                    XrValue cloned_key = xrt_value_clone_for_coro(xrt_map_slot_key(src, slot));
                    XrValue cloned_val = xrt_value_clone_for_coro(xrt_map_slot_value(src, slot));
                    xrt_map_set(dst, cloned_key, cloned_val);
                }
            }
            return dstv;
        }
        case XR_TAG_SET: {
            xrt_set_t *src = (xrt_set_t *) val.ptr;
            if (!src)
                return val;
            if (!xrt_set_is_typed(src) && (src->flags & XR_SET_FLAG_WEAK))
                return xrt_set_new_flags(0, XR_SET_FLAG_WEAK);
            XrValue dstv = xrt_set_new_typed(xrt_set_len(src), src->elem_type);
            xrt_set_t *dst = (xrt_set_t *) dstv.ptr;
            if (src->elem_type != XR_ELEM_ANY && dst->cap == src->cap) {
                dst->len = src->len;
                dst->growth_left = src->growth_left;
                memcpy(dst->ctrl, src->ctrl, (size_t) src->cap + XRT_GROUP);
                memcpy(dst->items, src->items, (size_t) src->cap * (size_t) src->elem_size);
                xrt_set_order_reserve(dst, src->order_len);
                memcpy(dst->order, src->order, (size_t) src->order_len * sizeof(int64_t));
                dst->order_len = src->order_len;
            } else if (!xrt_set_is_typed(src)) {
                for (uint32_t i = 0; i < src->nentries; i++) {
                    if (src->entries[i].val_tt != XR_SET_ENTRY_NIL)
                        xrt_set_add(dst, xrt_value_clone_for_coro(src->entries[i].value));
                }
            } else {
                for (int64_t oi = 0; oi < src->order_len; oi++) {
                    int64_t slot = src->order[oi];
                    if (!xrt_set_slot_is_full(src, slot))
                        continue;
                    xrt_set_add(dst, xrt_value_clone_for_coro(xrt_set_slot_item(src, slot)));
                }
            }
            return dstv;
        }
        case XR_TAG_STRBUF: {
            xrt_strbuf_t *src = (xrt_strbuf_t *) val.ptr;
            if (!src)
                return val;
            XrValue dstv = xrt_strbuf_new();
            xrt_strbuf_t *dst = (xrt_strbuf_t *) dstv.ptr;
            xrt_strbuf_grow(dst, src->len);
            memcpy(dst->buf, src->buf, (size_t) src->len + 1u);
            dst->len = src->len;
            return dstv;
        }
        case XR_TAG_STRUCT_REF: {
            if (!val.ptr)
                return val;
            if (XR_IS_ARRAY_REF(val)) {
                uint8_t elem_type = XR_ARRAY_REF_ELEM_TYPE(val);
                uint16_t elem_count = XR_ARRAY_REF_ELEM_COUNT(val);
                size_t size = xrt_native_type_size(elem_type) * (size_t) elem_count;
                void *dst = xrt_arc_alloc(size);
                if (XR_UNLIKELY(!dst)) {
                    fprintf(stderr, "xrt_value_clone_for_coro: out of memory\n");
                    abort();
                }
                memcpy(dst, val.ptr, size);
                return xr_array_ref(dst, elem_type, elem_count);
            }
            uint16_t storage_size = val.heap_type;
            uint32_t size = storage_size ? storage_size : *(uint32_t *) val.ptr;
            if (size == 0 || size > (16u * 1024u * 1024u))
                return val;
            void *dst = xrt_arc_alloc(size);
            memcpy(dst, val.ptr, size);
            return storage_size ? xr_struct_ref(dst, storage_size)
                                : xr_mkptr(dst, XR_TAG_STRUCT_REF);
        }
        case XR_TAG_REGEX:
            xrt_retain(val);
            return val;
        default:
            return val;
    }
}

#endif  // XRT_COLL_H
