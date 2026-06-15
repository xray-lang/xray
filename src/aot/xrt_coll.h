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
#include "../shared/xr_elem_type.h"
#include "../shared/xr_typed_ops.h"

/* =========================================================================
 * Array runtime
 * ========================================================================= */

typedef struct {
    int64_t len;
    int64_t cap;
    /* Element storage. Alignment contract: for non-slice arrays this is
     * XRT_DATA_ALIGN-aligned (32 bytes, AVX-width) — heap buffers come from
     * XRT_ALLOC_ALIGNED, stack buffers round the alloca pointer up; growth
     * goes through xrt_array_data_grow which preserves the contract.
     * Generated _adN caches assert it via XR_ASSUME_ALIGNED. Slice views
     * alias a sub-range of another array's storage at an arbitrary element
     * offset — no alignment promise. */
    void *data;        /* uint8_t[] / int64_t[] / XrValue[] — depends on elem_type */
    uint8_t elem_type; /* XR_ELEM_ANY / XR_ELEM_U8 / ... */
    uint8_t elem_size; /* cached bytes per element */
    uint8_t is_slice;  /* view over another array; cannot grow */
    const char *adt_enum_name;
    const char *adt_member_name;
} xrt_array_t;

/* Grow the element buffer to hold new_cap elements. realloc cannot preserve
 * the XRT_DATA_ALIGN contract, so growth is aligned alloc + copy + aligned
 * free. The whole old buffer (cap elements, not just len) is copied so the
 * zero-filled spare capacity from the zeroing constructors stays zeroed.
 * Callers must reject slices first; stack-allocated arrays must never grow
 * (their data is not heap storage — same contract as before). */
static inline void xrt_array_data_grow(xrt_array_t *a, int64_t new_cap) {
    void *tmp = XRT_ALLOC_ALIGNED((size_t) new_cap * (size_t) a->elem_size);
    if (XR_UNLIKELY(!tmp)) {
        fprintf(stderr, "xrt_array_data_grow: out of memory\n");
        abort();
    }
    if (a->data) {
        memcpy(tmp, a->data, (size_t) a->cap * (size_t) a->elem_size);
        XRT_FREE_ALIGNED(a->data);
    }
    a->data = tmp;
    a->cap = new_cap;
}

/* Zero-initialized aligned element buffer (calloc replacement that keeps the
 * XRT_DATA_ALIGN contract). */
static inline void *xrt_array_data_alloc_zeroed(size_t bytes) {
    void *p = XRT_ALLOC_ALIGNED(bytes);
    if (p)
        memset(p, 0, bytes);
    return p;
}

static inline XrValue xrt_array_new(int64_t cap) {
    if (cap < 4)
        cap = 4;
    xrt_array_t *a = (xrt_array_t *) XRT_MALLOC(sizeof(xrt_array_t));
    if (XR_UNLIKELY(!a)) {
        fprintf(stderr, "xrt_array_new: out of memory\n");
        abort();
    }
    a->len = 0;
    a->cap = cap;
    a->elem_type = XR_ELEM_ANY;
    a->elem_size = (uint8_t) sizeof(XrValue);
    a->is_slice = 0;
    a->adt_enum_name = NULL;
    a->adt_member_name = NULL;
    a->data = xrt_array_data_alloc_zeroed((size_t) cap * sizeof(XrValue));
    if (XR_UNLIKELY(!a->data)) {
        fprintf(stderr, "xrt_array_new: out of memory\n");
        abort();
    }
    return xr_mkptr(a, XR_TAG_ARRAY);
}

static inline xrt_array_t *xrt_array_new_typed_ptr(int64_t cap, uint8_t etype) {
    if (cap < 4)
        cap = 4;
    xrt_array_t *a = (xrt_array_t *) XRT_MALLOC(sizeof(xrt_array_t));
    if (XR_UNLIKELY(!a)) {
        fprintf(stderr, "xrt_array_new_typed: out of memory\n");
        abort();
    }
    a->len = 0;
    a->cap = cap;
    a->elem_type = etype;
    a->elem_size = XR_ELEM_SIZES[etype];
    a->is_slice = 0;
    a->adt_enum_name = NULL;
    a->adt_member_name = NULL;
    a->data = xrt_array_data_alloc_zeroed((size_t) cap * (size_t) a->elem_size);
    if (XR_UNLIKELY(!a->data)) {
        fprintf(stderr, "xrt_array_new_typed: out of memory\n");
        abort();
    }
    return a;
}

static inline XrValue xrt_array_new_typed(int64_t cap, uint8_t etype) {
    return xr_mkptr(xrt_array_new_typed_ptr(cap, etype), XR_TAG_ARRAY);
}

static inline xrt_array_t *xrt_array_new_typed_uninit_ptr(int64_t cap, uint8_t etype) {
    if (cap < 4)
        cap = 4;
    xrt_array_t *a = (xrt_array_t *) XRT_MALLOC(sizeof(xrt_array_t));
    if (XR_UNLIKELY(!a)) {
        fprintf(stderr, "xrt_array_new_typed_uninit: out of memory\n");
        abort();
    }
    a->len = 0;
    a->cap = cap;
    a->elem_type = etype;
    a->elem_size = XR_ELEM_SIZES[etype];
    a->is_slice = 0;
    a->adt_enum_name = NULL;
    a->adt_member_name = NULL;
    a->data = XRT_ALLOC_ALIGNED((size_t) cap * (size_t) a->elem_size);
    if (XR_UNLIKELY(!a->data)) {
        fprintf(stderr, "xrt_array_new_typed_uninit: out of memory\n");
        abort();
    }
    return a;
}

static inline XrValue xrt_array_new_typed_uninit(int64_t cap, uint8_t etype) {
    return xr_mkptr(xrt_array_new_typed_uninit_ptr(cap, etype), XR_TAG_ARRAY);
}
static inline XrValue xrt_bytes_new_len(int64_t len) {
    if (len < 0)
        len = 0;
    XrValue arr = xrt_array_new_typed(len, XR_ELEM_U8);
    ((xrt_array_t *) arr.ptr)->len = len;
    return arr;
}

static inline XrValue xrt_bytes_new_fill(XrValue len_value, XrValue fill_value) {
    int64_t len = xr_value_to_int64_coerce(len_value);
    XrValue arr = xrt_bytes_new_len(len);
    xrt_array_t *a = (xrt_array_t *) arr.ptr;
    uint8_t fill = (uint8_t) (xr_value_to_int64_coerce(fill_value) & 0xFF);
    if (a->len > 0)
        memset(a->data, fill, (size_t) a->len);
    return arr;
}

static inline XrValue xrt_bytes_new_copy(XrValue src_value) {
    if (src_value.tag != XR_TAG_ARRAY || !src_value.ptr)
        return XR_NULL_VAL;
    xrt_array_t *src = (xrt_array_t *) src_value.ptr;
    XrValue arr = xrt_bytes_new_len(src->len);
    xrt_array_t *dst = (xrt_array_t *) arr.ptr;
    if (src->elem_type == XR_ELEM_U8) {
        memcpy(dst->data, src->data, (size_t) src->len);
        return arr;
    }
    for (int64_t i = 0; i < src->len; i++) {
        XrValue item = xr_typed_get(src->data, (int32_t) i, src->elem_type);
        xr_typed_set(dst->data, (int32_t) i, item, dst->elem_type);
    }
    return arr;
}

static inline XrValue xrt_bytes_new_1(XrValue arg) {
    if (arg.tag == XR_TAG_ARRAY)
        return xrt_bytes_new_copy(arg);
    return xrt_bytes_new_len(xr_value_to_int64_coerce(arg));
}

static inline void xrt_array_push(XrValue arr, XrValue val) {
    xrt_array_t *a = (xrt_array_t *) arr.ptr;
    if (XR_UNLIKELY(a->is_slice)) {
        fprintf(stderr, "xrt_array_push: cannot push to array slice\n");
        abort();
    }
    if (XR_UNLIKELY(a->len >= a->cap))
        xrt_array_data_grow(a, a->cap == 0 ? 4 : a->cap * 2);
    xr_typed_set(a->data, (int32_t) a->len, val, a->elem_type);
    a->len++;
}

static inline int64_t xrt_array_len(XrValue arr) {
    return ((xrt_array_t *) arr.ptr)->len;
}

static inline XrValue xrt_array_slice_view(XrValue arr, int64_t start, int64_t end) {
    if (arr.tag != XR_TAG_ARRAY || !arr.ptr)
        return XR_NULL_VAL;
    xrt_array_t *src = (xrt_array_t *) arr.ptr;
    if (start < 0)
        start = 0;
    if (end < 0 || end > src->len)
        end = src->len;
    if (start > src->len)
        start = src->len;
    if (start > end)
        start = end;

    xrt_array_t *slice = (xrt_array_t *) XRT_MALLOC(sizeof(xrt_array_t));
    if (XR_UNLIKELY(!slice)) {
        fprintf(stderr, "xrt_array_slice_view: out of memory\n");
        abort();
    }
    slice->len = end - start;
    slice->cap = 0;
    slice->elem_type = src->elem_type;
    slice->elem_size = src->elem_size;
    slice->is_slice = 1;
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
    if (source.tag == XR_TAG_ARRAY)
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
        _a->len = 0;                                                                               \
        _a->cap = _cap;                                                                            \
        _a->elem_type = XR_ELEM_ANY;                                                               \
        _a->elem_size = (uint8_t) sizeof(XrValue);                                                 \
        _a->is_slice = 0;                                                                          \
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
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "%g", val.f);
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
 * Map runtime — swiss-table with SoA key/value slots.
 * Tagged maps store XrValue keys/values; typed maps store packed scalars.
 * ========================================================================= */

typedef struct {
    int64_t len;         /* live entries */
    int64_t cap;         /* slot count, power of two */
    int64_t growth_left; /* inserts into EMPTY slots before rehash */
    uint8_t *ctrl;       /* cap + XRT_GROUP control bytes */
    void *keys;          /* slot-indexed: XrValue[] or packed scalar[] */
    void *values;
    int64_t *order; /* live slot indices in insertion order */
    int64_t order_len;
    int64_t order_cap;
    uint8_t key_type; /* XR_ELEM_ANY for tagged */
    uint8_t value_type;
    uint8_t key_size;
    uint8_t value_size;
} xrt_map_t;

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

static inline XrValue xrt_map_new(int64_t cap) {
    xrt_map_t *m = (xrt_map_t *) XRT_MALLOC(sizeof(xrt_map_t));
    if (XR_UNLIKELY(!m)) {
        fprintf(stderr, "xrt_map_new: out of memory\n");
        abort();
    }
    m->len = 0;
    m->key_type = XR_ELEM_ANY;
    m->value_type = XR_ELEM_ANY;
    m->key_size = (uint8_t) sizeof(XrValue);
    m->value_size = (uint8_t) sizeof(XrValue);
    m->order = NULL;
    m->order_len = 0;
    m->order_cap = 0;
    xrt_map_alloc_slots(m, xrt_swiss_slots_for(cap));
    return xr_mkptr(m, XR_TAG_MAP);
}

static inline int xrt_map_slot_is_full(const xrt_map_t *m, int64_t slot) {
    return (m->ctrl[slot] & 0x80u) == 0;
}

#include "xrt_map_typed.inc.c"

static inline XrValue xrt_map_get(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_typed(m))
        return xrt_map_get_typed(m, key);
    int64_t slot = xrt_map_find_tagged(m, key);
    return slot >= 0 ? ((XrValue *) m->values)[slot] : XR_NULL_VAL;
}

static inline int xrt_map_has(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_typed(m))
        return xrt_map_has_typed(m, key);
    return xrt_map_find_tagged(m, key) >= 0;
}

static inline int xrt_map_delete(xrt_map_t *m, XrValue key) {
    if (xrt_map_is_typed(m))
        return xrt_map_delete_typed(m, key);
    int64_t slot = xrt_map_find_tagged(m, key);
    if (slot < 0)
        return 0;
    xrt_map_erase_slot(m, slot);
    return 1;
}

static inline void xrt_map_set(xrt_map_t *m, XrValue key, XrValue val) {
    if (xrt_map_is_typed(m)) {
        xrt_map_set_typed(m, key, val);
        return;
    }
    int64_t slot = xrt_map_find_tagged(m, key);
    if (slot < 0) {
        slot = xrt_map_insert_slot(m, xrt_hash_value(key));
        ((XrValue *) m->keys)[slot] = key;
    }
    ((XrValue *) m->values)[slot] = val;
}

/* =========================================================================
 * Set runtime — swiss-table over packed items (typed) or XrValue slots.
 * ========================================================================= */

typedef struct {
    int64_t len;         /* live entries */
    int64_t cap;         /* slot count, power of two */
    int64_t growth_left; /* inserts into EMPTY slots before rehash */
    uint8_t *ctrl;       /* cap + XRT_GROUP control bytes */
    void *items;         /* slot-indexed element storage */
    int64_t *order;      /* live slot indices in insertion order */
    int64_t order_len;
    int64_t order_cap;
    uint8_t elem_type;
    uint8_t elem_size;
} xrt_set_t;

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
    s->len = 0;
    s->elem_type = elem_type;
    s->elem_size = elem_type == XR_ELEM_ANY ? (uint8_t) sizeof(XrValue) : XR_ELEM_SIZES[elem_type];
    s->order = NULL;
    s->order_len = 0;
    s->order_cap = 0;
    xrt_set_alloc_slots(s, xrt_swiss_slots_for(cap));
    return xr_mkptr(s, XR_TAG_SET);
}

static inline XrValue xrt_set_new(int64_t cap) {
    return xrt_set_new_typed(cap, XR_ELEM_ANY);
}

static inline int xrt_set_slot_is_full(const xrt_set_t *s, int64_t slot) {
    return (s->ctrl[slot] & 0x80u) == 0;
}

/* Slot accessor — valid only for FULL slots. */
static inline XrValue xrt_set_slot_item(xrt_set_t *s, int64_t slot) {
    return xr_typed_get(s->items, (int32_t) slot, s->elem_type);
}

#include "xrt_set_direct.inc.c"

static inline int xrt_set_has(xrt_set_t *s, XrValue value) {
    return xrt_set_find_value(s, value) >= 0;
}

static inline int xrt_set_add(xrt_set_t *s, XrValue value) {
    if (xrt_set_find_value(s, value) >= 0)
        return 0;
    int64_t slot = xrt_set_insert_slot(s, xrt_set_hash_value(s, value));
    (void) xr_typed_set(s->items, (int32_t) slot, value, s->elem_type);
    return 1;
}

static inline int xrt_set_delete(xrt_set_t *s, XrValue value) {
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
    memset(s->ctrl, (int) XRT_CTRL_EMPTY, (size_t) s->cap + XRT_GROUP);
    s->growth_left = s->cap - s->cap / 8;
    s->len = 0;
    s->order_len = 0;
}

static inline XrValue xrt_set_values(xrt_set_t *s) {
    XrValue arr = s->elem_type == XR_ELEM_ANY ? xrt_array_new(s->len)
                                              : xrt_array_new_typed(s->len, s->elem_type);
    for (int64_t oi = 0; oi < s->order_len; oi++) {
        int64_t slot = s->order[oi];
        if (xrt_set_slot_is_full(s, slot))
            xrt_array_push(arr, xrt_set_slot_item(s, slot));
    }
    return arr;
}

/* =========================================================================
 * Iterator runtime — backs the for-in iterator protocol over Map / Set.
 * The iterator borrows its source by value (no extra RC: AOT collections are
 * not individually reclaimed) and walks the source's insertion-order order[].
 * ========================================================================= */

#define XRT_ITER_KEYS 0   /* map: yield key */
#define XRT_ITER_VALUES 1 /* set: yield value; map: yield value */
#define XRT_ITER_PAIRS 2  /* map: yield (key, value) tuple */

typedef struct {
    XrValue coll;   /* XR_TAG_MAP or XR_TAG_SET being iterated */
    int64_t cursor; /* next index into the source's order[] */
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

// Park cursor at the next live order[] slot; return 1 if one exists.
static inline int xrt_iterator_has_next(xrt_iterator_t *it) {
    if (it->coll.tag == XR_TAG_MAP) {
        xrt_map_t *m = (xrt_map_t *) it->coll.ptr;
        while (it->cursor < m->order_len) {
            if (xrt_map_slot_is_full(m, m->order[it->cursor]))
                return 1;
            it->cursor++;
        }
        return 0;
    }
    if (it->coll.tag == XR_TAG_SET) {
        xrt_set_t *s = (xrt_set_t *) it->coll.ptr;
        while (it->cursor < s->order_len) {
            if (xrt_set_slot_is_full(s, s->order[it->cursor]))
                return 1;
            it->cursor++;
        }
        return 0;
    }
    return 0;
}

static inline XrValue xrt_iterator_next(xrt_iterator_t *it) {
    if (!xrt_iterator_has_next(it))
        return XR_NULL_VAL;
    if (it->coll.tag == XR_TAG_MAP) {
        xrt_map_t *m = (xrt_map_t *) it->coll.ptr;
        int64_t slot = m->order[it->cursor++];
        if (it->kind == XRT_ITER_PAIRS) {
            XrValue kv[2] = {xrt_map_slot_key(m, slot), xrt_map_slot_value(m, slot)};
            return xrt_tuple_make(2, kv);
        }
        if (it->kind == XRT_ITER_VALUES)
            return xrt_map_slot_value(m, slot);
        return xrt_map_slot_key(m, slot);
    }
    if (it->coll.tag == XR_TAG_SET) {
        xrt_set_t *s = (xrt_set_t *) it->coll.ptr;
        int64_t slot = s->order[it->cursor++];
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
    for (int64_t i = 0; i < field_count; i++)
        j->fields[i] = (XrValue) {.i = 0, .tag = XR_TAG_NULL};
    return xr_mkptr(j, XR_TAG_PTR);
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

static inline XrValue xrt_json_clone_for_coro(XrValue val) {
    if (val.tag != XR_TAG_PTR || !val.ptr)
        return val;
    xrt_json_t *src = (xrt_json_t *) val.ptr;
    XrValue dstv = xrt_json_new(src->field_count);
    xrt_json_t *dst = (xrt_json_t *) dstv.ptr;
    for (int64_t i = 0; i < src->field_count; i++)
        dst->fields[i] = xrt_value_clone_for_coro(src->fields[i]);
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

static inline XrValue xrt_closure_new(void *fn, int nupvals) {
    xrt_closure_t *c =
        (xrt_closure_t *) xrt_arc_alloc(sizeof(xrt_closure_t) + (size_t) nupvals * sizeof(XrValue));
    if (XR_UNLIKELY(!c)) {
        fprintf(stderr, "xrt_closure_new: out of memory\n");
        abort();
    }
    c->fn = fn;
    c->nupvals = nupvals;
    for (int i = 0; i < nupvals; i++)
        c->upvals[i] = (XrValue) {.i = 0, .tag = XR_TAG_NULL};
    return xr_mkptr(c, XR_TAG_CLOSURE);
}

typedef struct xrt_cell {
    XrValue value;
} xrt_cell_t;

static inline XrValue xrt_cell_new(XrValue value) {
    xrt_cell_t *cell = (xrt_cell_t *) xrt_arc_alloc(sizeof(xrt_cell_t));
    if (XR_UNLIKELY(!cell)) {
        fprintf(stderr, "xrt_cell_new: out of memory\n");
        abort();
    }
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

static inline XrValue xrt_value_clone_for_coro(XrValue val) {
    switch (val.tag) {
        case XR_TAG_ARRAY: {
            xrt_array_t *src = (xrt_array_t *) val.ptr;
            if (!src)
                return val;
            XrValue dstv = xrt_array_new_typed(src->cap, src->elem_type);
            xrt_array_t *dst = (xrt_array_t *) dstv.ptr;
            dst->len = src->len;
            dst->adt_enum_name = src->adt_enum_name;
            dst->adt_member_name = src->adt_member_name;
            if (src->elem_type == XR_ELEM_ANY) {
                for (int64_t i = 0; i < src->len; i++) {
                    XrValue item = xr_typed_get(src->data, (int32_t) i, src->elem_type);
                    xr_typed_set(dst->data, (int32_t) i, xrt_value_clone_for_coro(item),
                                 dst->elem_type);
                }
            } else {
                memcpy(dst->data, src->data, (size_t) src->len * (size_t) src->elem_size);
            }
            return dstv;
        }
        case XR_TAG_MAP: {
            xrt_map_t *src = (xrt_map_t *) val.ptr;
            if (!src)
                return val;
            XrValue dstv = xrt_map_is_typed(src)
                               ? xrt_map_new_typed(src->len, src->key_type, src->value_type)
                               : xrt_map_new(src->len);
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
            XrValue dstv = xrt_set_new_typed(src->len, src->elem_type);
            xrt_set_t *dst = (xrt_set_t *) dstv.ptr;
            if (src->elem_type != XR_ELEM_ANY && dst->cap == src->cap) {
                dst->len = src->len;
                dst->growth_left = src->growth_left;
                memcpy(dst->ctrl, src->ctrl, (size_t) src->cap + XRT_GROUP);
                memcpy(dst->items, src->items, (size_t) src->cap * (size_t) src->elem_size);
                xrt_set_order_reserve(dst, src->order_len);
                memcpy(dst->order, src->order, (size_t) src->order_len * sizeof(int64_t));
                dst->order_len = src->order_len;
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
