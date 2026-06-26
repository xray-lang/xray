/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_boolmap.inc.c - AOT-only 2-slot direct-store specialization of
 * Map<bool, scalar>.
 *
 * A bool-keyed map has at most two entries (key false / key true), so the
 * generic Swiss-table with its separately heap-allocated keys/values/order
 * arrays is pure overhead: every has/get probes through several independent
 * cache lines and the pointer indirection blocks the C compiler from hoisting
 * loop-invariant lookups. This representation stores the two values inline in
 * the object as a fixed 2-slot array plus a presence bitset, so has/get reduce
 * to a bit test and an indexed load the compiler can prove invariant.
 *
 * Scope: created only for Map<bool,i64> and Map<bool,f32> (the two value
 * layouts codegen routes to the bool-direct helpers). Bool maps with reference
 * values stay generic tagged maps, so a boolmap never owns RC-managed values
 * and needs no per-slot retain/release.
 *
 * Boxing: a boolmap boxes as XR_TAG_MAP exactly like a generic map (the boxed
 * value's heap_type is XR_TMAP), so XR_IS_MAP / typeof / interop are unchanged.
 * The heap object's hdr.type is XR_TBOOLMAP, which is how the generic map entry
 * points and the RC collector discriminate it from a Swiss-table map.
 *
 * Iteration order: Map preserves insertion order, so the two keys are tracked
 * in an explicit 2-entry order list rather than a fixed false->true order.
 */

#ifndef XRT_BOOLMAP_INC
#define XRT_BOOLMAP_INC

typedef union {
    int64_t i;
    double f;
} xrt_boolmap_slot;

typedef struct xrt_boolmap_t {
    XrObjHeader hdr;       /* embedded-at-0 header; hdr.type == XR_TBOOLMAP */
    uint8_t value_type;    /* XR_ELEM_I64 or XR_ELEM_F32 */
    uint8_t present;       /* bit0: key false present, bit1: key true present */
    uint8_t order[2];      /* insertion order; order[n] is the key (0|1) inserted n-th */
    uint8_t order_len;     /* number of live entries (0..2) */
    xrt_boolmap_slot v[2]; /* v[0] = value of key false, v[1] = value of key true */
} xrt_boolmap_t;

/* A boxed map value points at either an xrt_map_t or an xrt_boolmap_t; the two
 * share the XrObjHeader prefix, so hdr.type is the safe discriminator before any
 * map-specific field is touched. */
static inline int xrt_map_is_boolmap(const xrt_map_t *m) {
    return m && ((const XrObjHeader *) m)->type == XR_TBOOLMAP;
}

static inline int xrt_boolmap_index(int64_t key) {
    return key != 0 ? 1 : 0;
}

static inline XrValue xrt_boolmap_new_typed(int64_t cap, uint8_t value_type) {
    (void) cap;
    xrt_boolmap_t *b = (xrt_boolmap_t *) XRT_MALLOC(sizeof(xrt_boolmap_t));
    if (XR_UNLIKELY(!b)) {
        fprintf(stderr, "xrt_boolmap_new: out of memory\n");
        abort();
    }
    xrt_bump_header_init(&b->hdr, XR_TBOOLMAP);
    xrt_coll_make_deterministic(&b->hdr);
    b->value_type = value_type;
    b->present = 0;
    b->order[0] = 0;
    b->order[1] = 0;
    b->order_len = 0;
    b->v[0].i = 0;
    b->v[1].i = 0;
    return xr_mkptr(b, XR_TAG_MAP);
}

/* Mark slot `i` present, appending it to the insertion order on first insert. */
static inline void xrt_boolmap_touch(xrt_boolmap_t *b, int i) {
    if (!((b->present >> i) & 1)) {
        b->order[b->order_len++] = (uint8_t) i;
        b->present |= (uint8_t) (1u << i);
    }
}

static inline void xrt_boolmap_set_i64(xrt_boolmap_t *b, int64_t key, int64_t value) {
    int i = xrt_boolmap_index(key);
    xrt_boolmap_touch(b, i);
    b->v[i].i = value;
}

/* f32 maps store the float-truncated value so AOT matches the generic typed
 * map (which narrows through `float`) and the VM. */
static inline void xrt_boolmap_set_f32(xrt_boolmap_t *b, int64_t key, double value) {
    int i = xrt_boolmap_index(key);
    xrt_boolmap_touch(b, i);
    b->v[i].f = (double) (float) value;
}

static inline int xrt_boolmap_delete(xrt_boolmap_t *b, int64_t key) {
    int i = xrt_boolmap_index(key);
    if (!((b->present >> i) & 1))
        return 0;
    b->present &= (uint8_t) ~(1u << i);
    b->v[i].i = 0;
    int w = 0;
    for (int o = 0; o < b->order_len; o++) {
        if (b->order[o] != (uint8_t) i)
            b->order[w++] = b->order[o];
    }
    b->order_len = (uint8_t) w;
    return 1;
}

static inline int64_t xrt_boolmap_len(const xrt_boolmap_t *b) {
    return b->order_len;
}

/* ---- codegen hot-path helpers (single representation, no dispatch branch) --
 * Signatures mirror xrt_map_find_bool_typed / xrt_map_get_*_value_typed so the
 * C backend can emit them in the same find->slot->value shape. The receiver is
 * always a real boolmap (codegen only routes Map<bool,i64|f32> here), so these
 * stay free of the is_boolmap branch that would block loop-invariant hoisting.
 * "slot" is the key index 0/1; -1 means absent. */
static inline int64_t xrt_boolmap_find(xrt_map_t *m, int64_t key, uint8_t key_type,
                                       uint8_t value_type) {
    (void) key_type;
    (void) value_type;
    const xrt_boolmap_t *b = (const xrt_boolmap_t *) m;
    int i = key != 0 ? 1 : 0;
    return ((b->present >> i) & 1) ? (int64_t) i : -1;
}

static inline int64_t xrt_boolmap_value_i64(xrt_map_t *m, int64_t slot, uint8_t value_type) {
    (void) value_type;
    return ((const xrt_boolmap_t *) m)->v[slot].i;
}

static inline double xrt_boolmap_value_f64(xrt_map_t *m, int64_t slot, uint8_t value_type) {
    (void) value_type;
    return ((const xrt_boolmap_t *) m)->v[slot].f;
}

/* Box slot `i`'s stored scalar back into a tagged value for interop. */
static inline XrValue xrt_boolmap_box_value(const xrt_boolmap_t *b, int i) {
    return b->value_type == XR_ELEM_F32 ? XR_FROM_FLOAT(b->v[i].f) : XR_FROM_INT(b->v[i].i);
}

/* ---- interop entry points (tagged XrValue keys/values) ----------------- */

static inline int xrt_boolmap_key_index_v(XrValue key, int *out_index) {
    if (!XR_IS_BOOL(key))
        return 0;
    if (out_index)
        *out_index = key.i != 0 ? 1 : 0;
    return 1;
}

static inline XrValue xrt_boolmap_get_v(xrt_boolmap_t *b, XrValue key) {
    int i = 0;
    if (!xrt_boolmap_key_index_v(key, &i))
        return XR_NULL_VAL;
    return ((b->present >> i) & 1) ? xrt_boolmap_box_value(b, i) : XR_NULL_VAL;
}

static inline int xrt_boolmap_has_v(xrt_boolmap_t *b, XrValue key) {
    int i = 0;
    return xrt_boolmap_key_index_v(key, &i) ? ((b->present >> i) & 1) : 0;
}

static inline int xrt_boolmap_delete_v(xrt_boolmap_t *b, XrValue key) {
    int i = 0;
    return xrt_boolmap_key_index_v(key, &i) ? xrt_boolmap_delete(b, i) : 0;
}

static inline void xrt_boolmap_set_v(xrt_boolmap_t *b, XrValue key, XrValue val) {
    int i = 0;
    if (!xrt_boolmap_key_index_v(key, &i))
        return;
    if (b->value_type == XR_ELEM_F32)
        xrt_boolmap_set_f32(b, i, xr_value_to_f64_coerce(val));
    else
        xrt_boolmap_set_i64(b, i, xr_value_to_int64_coerce(val));
}

static inline XrValue xrt_boolmap_keys(xrt_boolmap_t *b) {
    XrValue arr = xrt_array_new(b->order_len);
    for (int o = 0; o < b->order_len; o++)
        xrt_array_push(arr, XR_FROM_BOOL(b->order[o] != 0));
    return arr;
}

static inline XrValue xrt_boolmap_values(xrt_boolmap_t *b) {
    XrValue arr = xrt_array_new(b->order_len);
    for (int o = 0; o < b->order_len; o++)
        xrt_array_push(arr, xrt_boolmap_box_value(b, b->order[o]));
    return arr;
}

/* Iterator support: cursor indexes the live insertion-order list. */
static inline XrValue xrt_boolmap_iter_key(const xrt_boolmap_t *b, int64_t cursor) {
    return XR_FROM_BOOL(b->order[cursor] != 0);
}

static inline XrValue xrt_boolmap_iter_value(const xrt_boolmap_t *b, int64_t cursor) {
    return xrt_boolmap_box_value(b, b->order[cursor]);
}

static inline XrValue xrt_boolmap_clone(const xrt_boolmap_t *src) {
    XrValue dstv = xrt_boolmap_new_typed(2, src->value_type);
    xrt_boolmap_t *dst = (xrt_boolmap_t *) dstv.ptr;
    dst->present = src->present;
    dst->order_len = src->order_len;
    dst->order[0] = src->order[0];
    dst->order[1] = src->order[1];
    dst->v[0] = src->v[0];
    dst->v[1] = src->v[1];
    return dstv;
}

/* boolmap values are always scalars, so destruction only frees the block. */
static inline void xrt_boolmap_destroy(xrt_boolmap_t *b) {
    if (!b)
        return;
    XRT_FREE(b);
}

#endif /* XRT_BOOLMAP_INC */
