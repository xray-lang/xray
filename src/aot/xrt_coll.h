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

#include "xrt_value.h"
#include "xrt_arc.h"  // xrt_str_alloc used by xrt_strbuf_finish
#include "xrt_range.h"
#include "../shared/xr_array_abi.h"
#include "../shared/xr_cell_abi.h"
#include "../shared/xr_elem_type.h"
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
 * `source` and `has_gc_ptrs`/`elem_tid` are part of the shared ABI; the AOT
 * runtime keeps `source` NULL (slices borrow via arena lifetime) and does not
 * consult the GC-tracking fields. */
typedef struct {
    XrGCHeader gc; /* embedded-at-0 header: same placement as the VM XrArray so
                    * the two layouts line up (C0 object-header unification) */
    XR_ARRAY_ABI_FIELDS;
    const char *adt_enum_name;
    const char *adt_member_name;
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

static inline void xrt_array_init_header(xrt_array_t *a, int64_t cap, uint8_t etype,
                                         uint8_t elem_size) {
    xrt_bump_header_init(&a->gc, XR_TARRAY);
    a->length = 0;
    a->capacity = cap;
    a->source = NULL;
    a->elem_type = etype;
    a->elem_size = elem_size;
    a->elem_tid = 0;
    a->has_gc_ptrs = 0;
    a->data_storage = XR_ARRAY_DATA_INLINE;
    a->adt_enum_name = NULL;
    a->adt_member_name = NULL;
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
    if (XR_UNLIKELY(a->data_storage == XR_ARRAY_DATA_BORROWED)) {
        fprintf(stderr, "xrt_array_push: cannot push to array slice\n");
        abort();
    }
    if (XR_UNLIKELY(a->length >= a->capacity))
        xrt_array_data_grow(a, a->capacity == 0 ? 4 : a->capacity * 2);
    xr_typed_set(a->data, (int32_t) a->length, val, a->elem_type);
    a->length++;
}

static inline int64_t xrt_array_len(XrValue arr) {
    return ((xrt_array_t *) arr.ptr)->length;
}

static inline XrValue xrt_array_slice_view(XrValue arr, int64_t start, int64_t end) {
    if (!XR_IS_ARRAY(arr) || !arr.ptr)
        return XR_NULL_VAL;
    xrt_array_t *src = (xrt_array_t *) arr.ptr;
    if (start < 0)
        start = 0;
    if (end < 0 || end > src->length)
        end = src->length;
    if (start > src->length)
        start = src->length;
    if (start > end)
        start = end;

    xrt_array_t *slice = (xrt_array_t *) XRT_MALLOC(sizeof(xrt_array_t));
    if (XR_UNLIKELY(!slice)) {
        fprintf(stderr, "xrt_array_slice_view: out of memory\n");
        abort();
    }
    xrt_bump_header_init(&slice->gc, XR_TARRAY);
    slice->length = end - start;
    slice->capacity = end - start;
    slice->source = NULL;
    slice->elem_type = src->elem_type;
    slice->elem_size = src->elem_size;
    slice->elem_tid = src->elem_tid;
    slice->has_gc_ptrs = src->has_gc_ptrs;
    slice->data_storage = XR_ARRAY_DATA_BORROWED;
    slice->adt_enum_name = NULL;
    slice->adt_member_name = NULL;
    slice->data = (uint8_t *) src->data + (size_t) start * (size_t) src->elem_size;
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
        xrt_bump_header_init(&_a->gc, XR_TARRAY);                                                  \
        _a->length = 0;                                                                            \
        _a->capacity = _cap;                                                                       \
        _a->source = NULL;                                                                         \
        _a->elem_type = XR_ELEM_ANY;                                                               \
        _a->elem_size = (uint8_t) sizeof(XrValue);                                                 \
        _a->elem_tid = 0;                                                                          \
        _a->has_gc_ptrs = 0;                                                                       \
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
    }
}

static inline XrValue xrt_strbuf_finish(XrValue sbv) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) sbv.ptr;
    XrValue v = xrt_str_alloc((size_t) sb->len);
    memcpy(xr_str_buf(v), sb->buf, (size_t) (sb->len + 1));
    return v;
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
    return xrt_hash_mix_u64(bits);
}

/* Hash for tagged values, consistent with xrt_eq: strings hash content
 * through the header cache (literals carry a precomputed hash). */
static inline uint64_t xrt_hash_value(XrValue v) {
    uint32_t tag = (v.tag == XR_TAG_STR_ARC) ? XR_TAG_STR : v.tag;
    switch (tag) {
        case XR_TAG_I64:
        case XR_TAG_BOOL:
            return xrt_hash_mix_u64((uint64_t) v.i);
        case XR_TAG_F64:
            return xrt_hash_f64(v.f);
        case XR_TAG_STR:
            return xrt_hash_mix_u64(xrt_str_hash(v));
        case XR_TAG_NULL:
            return xrt_hash_mix_u64(0x9e3779b97f4a7c15ull);
        default:
            return xrt_hash_mix_u64((uint64_t) (uintptr_t) v.ptr);
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

/* =========================================================================
 * Map runtime.
 *
 * Tagged maps use the shared hybrid-C ABI: dense insertion-order entries plus
 * a Swiss ctrl/index layer. Typed scalar maps temporarily keep their packed
 * SoA storage; 110 owns the typed-storage merge.
 * ========================================================================= */

typedef struct xrt_map_t {
    XrGCHeader gc; /* embedded-at-0 header: same placement as the VM XrMap (C0
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
    xrt_bump_header_init(&m->gc, XR_TMAP);
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

static inline uint32_t xrt_map_find_entry_slot(xrt_map_t *m, XrValue key, uint32_t hash,
                                               uint8_t key_tt, int32_t *out_eidx) {
    if (m->flags & XR_MAP_FLAG_DUMMY)
        return UINT32_MAX;

    uint32_t mask = m->indices_size - 1u;
    uint8_t h2 = xr_swiss_h2(hash);
    uint32_t pos = (hash >> 7u) & mask;
    uint32_t stride = 0;

    for (;;) {
        uint64_t group = xr_swiss_group_load(m->ctrl + pos);
        uint64_t matches = xr_swiss_group_match(group, h2);
        while (matches) {
            int off = xr_swiss_swar_first(matches);
            uint32_t slot = (pos + (uint32_t) off) & mask;
            int32_t eidx = m->indices[slot];
            if (eidx >= 0) {
                XrMapEntry *entry = &m->entries[eidx];
                if (entry->hash == hash && entry->key_tt == key_tt && xrt_eq(entry->key, key)) {
                    if (out_eidx)
                        *out_eidx = eidx;
                    return slot;
                }
            }
            matches &= ~(0xFFull << ((unsigned) off * 8u));
        }
        if (xr_swiss_group_match_empty(group))
            return UINT32_MAX;
        stride += XR_SWISS_GROUP;
        pos = (pos + stride) & mask;
    }
}

static inline int32_t xrt_map_find_entry(xrt_map_t *m, XrValue key, uint32_t hash, uint8_t key_tt) {
    int32_t eidx = -1;
    return xrt_map_find_entry_slot(m, key, hash, key_tt, &eidx) == UINT32_MAX ? -1 : eidx;
}

static inline void xrt_map_index_put(uint8_t *ctrl, int32_t *indices, uint32_t indices_size,
                                     uint32_t hash, int32_t eidx) {
    uint32_t slot = xr_swiss_find_free(ctrl, indices_size, hash);
    indices[slot] = eidx;
    xr_swiss_ctrl_set(ctrl, indices_size, slot, xr_swiss_h2(hash));
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

    uint32_t w = 0;
    for (uint32_t i = 0; i < old_nentries; i++) {
        XrMapEntry *old = &old_entries[i];
        if (old->key_tt == XR_MAP_ENTRY_NIL_KEY)
            continue;
        entries[w] = *old;
        xrt_map_index_put(ctrl, indices, indices_size, old->hash, (int32_t) w);
        w++;
    }

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

static inline void xrt_map_order_remove(xrt_map_t *m, int64_t slot) {
    for (int64_t i = 0; i < m->order_len; i++) {
        if (m->order[i] != slot)
            continue;
        if (i + 1 < m->order_len) {
            memmove(&m->order[i], &m->order[i + 1],
                    (size_t) (m->order_len - i - 1) * sizeof(int64_t));
        }
        m->order_len--;
        return;
    }
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

#include "xrt_map_typed.inc.c"

static inline int64_t xrt_map_len(const xrt_map_t *m) {
    return xrt_map_is_typed(m) ? m->len : (int64_t) m->count;
}

static inline int xrt_map_slot_is_full(const xrt_map_t *m, int64_t slot) {
    if (xrt_map_is_typed(m))
        return (m->ctrl[slot] & 0x80u) == 0;
    return slot >= 0 && (uint32_t) slot < m->nentries &&
           m->entries[slot].key_tt != XR_MAP_ENTRY_NIL_KEY;
}

static inline XrValue xrt_map_get(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_typed(m))
        return xrt_map_get_typed(m, key);
    uint8_t key_tt = xrt_value_type_tag(key);
    uint32_t hash = xrt_hash32_value(key);
    int32_t eidx = xrt_map_find_entry(m, key, hash, key_tt);
    return eidx >= 0 ? m->entries[eidx].value : XR_NULL_VAL;
}

static inline int xrt_map_has(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_typed(m))
        return xrt_map_has_typed(m, key);
    return xrt_map_find_entry(m, key, xrt_hash32_value(key), xrt_value_type_tag(key)) >= 0;
}

static inline int xrt_map_delete(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_typed(m))
        return xrt_map_delete_typed(m, key);
    uint8_t key_tt = xrt_value_type_tag(key);
    uint32_t hash = xrt_hash32_value(key);
    int32_t eidx = -1;
    uint32_t slot = xrt_map_find_entry_slot(m, key, hash, key_tt, &eidx);
    if (slot == UINT32_MAX)
        return 0;
    m->entries[eidx].key_tt = XR_MAP_ENTRY_NIL_KEY;
    m->entries[eidx].key = XR_NULL_VAL;
    m->entries[eidx].value = XR_NULL_VAL;
    m->indices[slot] = XR_MAP_IX_EMPTY;
    xr_swiss_ctrl_set(m->ctrl, m->indices_size, slot, XR_SWISS_CTRL_DELETED);
    m->count--;
    return 1;
}

static inline void xrt_map_set(xrt_map_t *m, XrValue key, XrValue val) {
    if (xrt_map_is_typed(m)) {
        xrt_map_set_typed(m, key, val);
        return;
    }
    uint8_t key_tt = xrt_value_type_tag(key);
    uint32_t hash = xrt_hash32_value(key);
    int32_t eidx = xrt_map_find_entry(m, key, hash, key_tt);
    if (eidx >= 0) {
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
    xrt_map_index_put(m->ctrl, m->indices, m->indices_size, hash, eidx);
}

/* =========================================================================
 * Set runtime.
 *
 * Tagged sets use the shared hybrid-C ABI. Typed scalar sets temporarily keep
 * their packed SoA storage; 110 owns the typed-storage merge.
 * ========================================================================= */

typedef struct xrt_set_t {
    XrGCHeader gc; /* embedded-at-0 header: same placement as the VM XrSet (C0
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
    xrt_bump_header_init(&s->gc, XR_TSET);
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
    if (s->flags & XR_SET_FLAG_DUMMY)
        return UINT32_MAX;

    uint32_t mask = s->indices_size - 1u;
    uint8_t h2 = xr_swiss_h2(hash);
    uint32_t pos = (hash >> 7u) & mask;
    uint32_t stride = 0;

    for (;;) {
        uint64_t group = xr_swiss_group_load(s->ctrl + pos);
        uint64_t matches = xr_swiss_group_match(group, h2);
        while (matches) {
            int off = xr_swiss_swar_first(matches);
            uint32_t slot = (pos + (uint32_t) off) & mask;
            int32_t eidx = s->indices[slot];
            if (eidx >= 0) {
                XrSetEntry *entry = &s->entries[eidx];
                if (entry->hash == hash && entry->val_tt == xrt_value_type_tag(value) &&
                    xrt_eq(entry->value, value)) {
                    if (out_eidx)
                        *out_eidx = eidx;
                    return slot;
                }
            }
            matches &= ~(0xFFull << ((unsigned) off * 8u));
        }
        if (xr_swiss_group_match_empty(group))
            return UINT32_MAX;
        stride += XR_SWISS_GROUP;
        pos = (pos + stride) & mask;
    }
}

static inline int32_t xrt_set_find_entry(xrt_set_t *s, XrValue value, uint32_t hash) {
    int32_t eidx = -1;
    return xrt_set_find_entry_slot(s, value, hash, &eidx) == UINT32_MAX ? -1 : eidx;
}

static inline void xrt_set_index_put(uint8_t *ctrl, int32_t *indices, uint32_t indices_size,
                                     uint32_t hash, int32_t eidx) {
    uint32_t slot = xr_swiss_find_free(ctrl, indices_size, hash);
    indices[slot] = eidx;
    xr_swiss_ctrl_set(ctrl, indices_size, slot, xr_swiss_h2(hash));
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

    uint32_t w = 0;
    for (uint32_t i = 0; i < old_nentries; i++) {
        XrSetEntry *old = &old_entries[i];
        if (old->val_tt == XR_SET_ENTRY_NIL)
            continue;
        entries[w] = *old;
        xrt_set_index_put(ctrl, indices, indices_size, old->hash, (int32_t) w);
        w++;
    }

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

static inline void xrt_set_order_remove(xrt_set_t *s, int64_t slot) {
    for (int64_t i = 0; i < s->order_len; i++) {
        if (s->order[i] != slot)
            continue;
        if (i + 1 < s->order_len) {
            memmove(&s->order[i], &s->order[i + 1],
                    (size_t) (s->order_len - i - 1) * sizeof(int64_t));
        }
        s->order_len--;
        return;
    }
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
        if (xrt_set_find_entry(s, value, hash) >= 0)
            return 0;
        if (s->nentries >= s->entries_cap)
            xrt_set_resize_tagged(s, s->count + 1);
        int32_t eidx = (int32_t) s->nentries++;
        XrSetEntry *entry = &s->entries[eidx];
        entry->value = value;
        entry->hash = hash;
        entry->val_tt = xrt_value_type_tag(value);
        s->count++;
        xrt_set_index_put(s->ctrl, s->indices, s->indices_size, hash, eidx);
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

static inline XrValue xrt_set_value_at(xrt_set_t *s, int64_t index) {
    if (index < 0)
        return XR_NULL_VAL;
    if (xrt_set_is_typed(s)) {
        if (index < s->order_len)
            return xrt_set_slot_item(s, s->order[index]);
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

/* =========================================================================
 * Iterator runtime — backs the for-in iterator protocol over Map / Set.
 * The iterator borrows its source by value (no extra RC: AOT collections are
 * not individually reclaimed) and walks dense entries or typed order[].
 * ========================================================================= */

#define XRT_ITER_KEYS 0   /* map: yield key */
#define XRT_ITER_VALUES 1 /* set: yield value; map: yield value */
#define XRT_ITER_PAIRS 2  /* map: yield (key, value) tuple */

typedef struct {
    XrValue coll;   /* XR_TAG_MAP or XR_TAG_SET being iterated */
    int64_t cursor; /* next dense entry index or typed order[] cursor */
    uint8_t kind;   /* XRT_ITER_* projection */
} xrt_iterator_t;

static inline XrValue xrt_iterator_new(XrValue coll, uint8_t kind) {
    xrt_iterator_t *it = (xrt_iterator_t *) XRT_MALLOC(sizeof(xrt_iterator_t));
    if (XR_UNLIKELY(!it)) {
        fprintf(stderr, "xrt_iterator_new: out of memory\n");
        abort();
    }
    it->coll = coll;
    it->cursor = 0;
    it->kind = kind;
    return xr_mkptr(it, XR_TAG_ITERATOR);
}

// Park cursor at the next live entry/order[] slot; return 1 if one exists.
static inline int xrt_iterator_has_next(xrt_iterator_t *it) {
    if (XR_IS_MAP(it->coll)) {
        xrt_map_t *m = (xrt_map_t *) it->coll.ptr;
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
    return 0;
}

static inline XrValue xrt_iterator_next(xrt_iterator_t *it) {
    if (!xrt_iterator_has_next(it))
        return XR_NULL_VAL;
    if (XR_IS_MAP(it->coll)) {
        xrt_map_t *m = (xrt_map_t *) it->coll.ptr;
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

typedef struct {
    int64_t field_count;
    const char **field_names;
    xrt_map_t *dynamic_fields;
    XrValue fields[]; /* flexible array of field values */
} xrt_json_t;

static inline XrValue xrt_value_clone_for_coro(XrValue val);

static inline XrValue xrt_json_new(int64_t field_count) {
    xrt_json_t *j =
        (xrt_json_t *) XRT_MALLOC(sizeof(xrt_json_t) + (size_t) field_count * sizeof(XrValue));
    if (XR_UNLIKELY(!j)) {
        fprintf(stderr, "xrt_json_new: out of memory\n");
        abort();
    }
    j->field_count = field_count;
    j->field_names = NULL;
    j->dynamic_fields = NULL;
    for (int64_t i = 0; i < field_count; i++)
        j->fields[i] = (XrValue) {.i = 0, .tag = XR_TAG_NULL};
    return xr_mkptr(j, XR_TAG_PTR);
}

static inline XrValue xrt_json_new_named(int64_t field_count, const char *const *field_names) {
    XrValue obj = xrt_json_new(field_count);
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

static inline XrValue xrt_getprop_name(XrValue obj, const char *name) {
    if (XR_IS_MAP(obj))
        return xrt_map_get((xrt_map_t *) obj.ptr, xr_box_str(name));
    return XR_NULL_VAL;
}

static inline XrValue xrt_setprop_name(XrValue obj, const char *name, XrValue val) {
    if (XR_IS_MAP(obj)) {
        xrt_map_set((xrt_map_t *) obj.ptr, xr_box_str(name), val);
        return val;
    }
    return val;
}

static inline XrValue xrt_json_clone_for_coro(XrValue val) {
    if (val.tag != XR_TAG_PTR || !val.ptr)
        return val;
    xrt_json_t *src = (xrt_json_t *) val.ptr;
    XrValue dstv = xrt_json_new_named(src->field_count, src->field_names);
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
        XrGCHeader *_hdr = (XrGCHeader *) __builtin_alloca(sizeof(XrGCHeader) + _obj_size);        \
        memset(_hdr, 0, sizeof(XrGCHeader) + _obj_size);                                           \
        _hdr->extra = XR_OBJ_HAS_DTOR | XR_OBJ_STORAGE_STACK;                                      \
        _hdr->_rsv = XRT_ARC_KIND_CLOSURE;                                                         \
        xrt_closure_t *_c = (xrt_closure_t *) ((char *) _hdr + sizeof(XrGCHeader));                \
        xrt_closure_init(_c, (fn_expr), _nupvals);                                                 \
        xr_mkptr(_c, XR_TAG_CLOSURE);                                                              \
    })
#endif

/* Post-header field set shared with the VM's XrCell (src/shared/xr_cell_abi.h)
 * so the AOT and VM cell layouts stay in lockstep. */
typedef struct xrt_cell {
    XR_CELL_ABI_FIELDS;
} xrt_cell_t;

static inline XrValue xrt_cell_new(XrValue value) {
    xrt_cell_t *cell = (xrt_cell_t *) xrt_arc_alloc(sizeof(xrt_cell_t));
    if (XR_UNLIKELY(!cell)) {
        fprintf(stderr, "xrt_cell_new: out of memory\n");
        abort();
    }
    xrt_arc_mark_builtin(cell, XRT_ARC_KIND_CELL);
    cell->value = value;
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
    ((xrt_cell_t *) cell_value.ptr)->value = value;
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
        default:
            break;
    }
}

static inline XrValue xrt_value_clone_for_coro(XrValue val) {
    switch (xrt_value_kind(val)) {
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
            uint32_t size = *(uint32_t *) val.ptr;
            if (size == 0 || size > (16u * 1024u * 1024u))
                return val;
            void *dst = xrt_arc_alloc(size);
            memcpy(dst, val.ptr, size);
            return xr_mkptr(dst, XR_TAG_STRUCT_REF);
        }
        default:
            return val;
    }
}

#endif  // XRT_COLL_H
