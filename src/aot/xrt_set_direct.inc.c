/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_set_direct.inc.c - Swiss-table probing for Set plus the direct typed
 * helpers emitted by codegen for AOT class-native fields.
 */

#ifndef XRT_SET_DIRECT_INC
#define XRT_SET_DIRECT_INC

static inline void xrt_set_direct_type_mismatch(uint8_t expected, uint8_t actual, const char *who) {
    fprintf(stderr, "%s: set element type mismatch (%u != %u)\n", who ? who : "xrt_set_direct",
            (unsigned) expected, (unsigned) actual);
    abort();
}

static inline void xrt_set_direct_unsupported(uint8_t elem_type, const char *who) {
    fprintf(stderr, "%s: unsupported set element type %u\n", who ? who : "xrt_set_direct",
            (unsigned) elem_type);
    abort();
}

/* ---- canonical bit patterns --------------------------------------------- */

static inline uint64_t xrt_set_item_bits_i64(int64_t value, uint8_t elem_type) {
    uint64_t bits = 0;
    if (!xr_typed_scalar_bits_i64(value, elem_type, &bits))
        xrt_set_direct_unsupported(elem_type, "xrt_set_item_bits_i64");
    return bits;
}

static inline uint64_t xrt_set_item_bits_f64(double value, uint8_t elem_type) {
    uint64_t bits = 0;
    if (!xr_typed_scalar_bits_f64(value, elem_type, &bits))
        xrt_set_direct_unsupported(elem_type, "xrt_set_item_bits_f64");
    return bits;
}

static inline int64_t xrt_set_slot_raw_i64(const xrt_set_t *s, int64_t slot) {
    switch (s->elem_type) {
        case XR_ELEM_I8:
            return ((const int8_t *) s->items)[slot];
        case XR_ELEM_U8:
        case XR_ELEM_BOOL:
            return ((const uint8_t *) s->items)[slot];
        case XR_ELEM_I16:
            return ((const int16_t *) s->items)[slot];
        case XR_ELEM_U16:
            return ((const uint16_t *) s->items)[slot];
        case XR_ELEM_I32:
            return ((const int32_t *) s->items)[slot];
        case XR_ELEM_U32:
            return ((const uint32_t *) s->items)[slot];
        case XR_ELEM_I64:
        case XR_ELEM_U64:
            return ((const int64_t *) s->items)[slot];
        default:
            xrt_set_direct_unsupported(s->elem_type, "xrt_set_slot_raw_i64");
    }
    return 0;
}

static inline void xrt_set_slot_store_i64(xrt_set_t *s, int64_t slot, int64_t value) {
    switch (s->elem_type) {
        case XR_ELEM_I8:
            ((int8_t *) s->items)[slot] = (int8_t) value;
            return;
        case XR_ELEM_U8:
            ((uint8_t *) s->items)[slot] = (uint8_t) value;
            return;
        case XR_ELEM_BOOL:
            ((uint8_t *) s->items)[slot] = value != 0 ? 1 : 0;
            return;
        case XR_ELEM_I16:
            ((int16_t *) s->items)[slot] = (int16_t) value;
            return;
        case XR_ELEM_U16:
            ((uint16_t *) s->items)[slot] = (uint16_t) value;
            return;
        case XR_ELEM_I32:
            ((int32_t *) s->items)[slot] = (int32_t) value;
            return;
        case XR_ELEM_U32:
            ((uint32_t *) s->items)[slot] = (uint32_t) value;
            return;
        case XR_ELEM_I64:
        case XR_ELEM_U64:
            ((int64_t *) s->items)[slot] = value;
            return;
        default:
            xrt_set_direct_unsupported(s->elem_type, "xrt_set_slot_store_i64");
    }
}

static inline void xrt_set_slot_store_f64(xrt_set_t *s, int64_t slot, double value) {
    if (s->elem_type == XR_ELEM_F32) {
        ((float *) s->items)[slot] = (float) value;
        return;
    }
    if (s->elem_type == XR_ELEM_F64) {
        ((double *) s->items)[slot] = value;
        return;
    }
    xrt_set_direct_unsupported(s->elem_type, "xrt_set_slot_store_f64");
}

/* ---- probe context ------------------------------------------------------ */

typedef struct {
    xrt_set_t *set;
    uint64_t bits;
} xrt_set_probe_i64_ctx;

static inline int xrt_set_probe_i64_eq(void *ctxp, int64_t slot) {
    xrt_set_probe_i64_ctx *ctx = (xrt_set_probe_i64_ctx *) ctxp;
    return xrt_set_item_bits_i64(xrt_set_slot_raw_i64(ctx->set, slot), ctx->set->elem_type) ==
           ctx->bits;
}

typedef struct {
    xrt_set_t *set;
    double value;
} xrt_set_probe_f64_ctx;

static inline int xrt_set_probe_f64_eq(void *ctxp, int64_t slot) {
    xrt_set_probe_f64_ctx *ctx = (xrt_set_probe_f64_ctx *) ctxp;
    if (ctx->set->elem_type == XR_ELEM_F32)
        return ((const float *) ctx->set->items)[slot] == (float) ctx->value;
    return ((const double *) ctx->set->items)[slot] == ctx->value;
}

typedef struct {
    xrt_set_t *set;
    XrValue value;
} xrt_set_probe_tag_ctx;

static inline int xrt_set_probe_tag_eq(void *ctxp, int64_t slot) {
    xrt_set_probe_tag_ctx *ctx = (xrt_set_probe_tag_ctx *) ctxp;
    return xrt_eq(((const XrValue *) ctx->set->items)[slot], ctx->value) != 0;
}

/* ---- hash / find / insert / erase --------------------------------------- */

static inline uint64_t xrt_set_hash_value(xrt_set_t *s, XrValue value) {
    if (s->elem_type == XR_ELEM_ANY)
        return xrt_hash_value(value);
    if (s->elem_type == XR_ELEM_F32 || s->elem_type == XR_ELEM_F64) {
        double d = value.tag == XR_TAG_F64 ? value.f : (double) value.i;
        return xr_hash_core_mix_u64(xrt_set_item_bits_f64(d, s->elem_type));
    }
    int64_t i = value.tag == XR_TAG_F64 ? (int64_t) value.f : value.i;
    return xr_hash_core_mix_u64(xrt_set_item_bits_i64(i, s->elem_type));
}

static inline int64_t xrt_set_find_i64_typed_slot(xrt_set_t *s, int64_t value, uint8_t elem_type) {
    uint64_t bits = xrt_set_item_bits_i64(value, elem_type);
    xrt_set_probe_i64_ctx ctx;
    ctx.set = s;
    ctx.bits = bits;
    return xr_swiss_find_match_i64(s->ctrl, s->cap, xr_hash_core_mix_u64(bits),
                                   xrt_set_probe_i64_eq, &ctx);
}

static inline int64_t xrt_set_find_f64_typed_slot(xrt_set_t *s, double value, uint8_t elem_type) {
    xrt_set_probe_f64_ctx ctx;
    ctx.set = s;
    ctx.value = value;
    return xr_swiss_find_match_i64(s->ctrl, s->cap,
                                   xr_hash_core_mix_u64(xrt_set_item_bits_f64(value, elem_type)),
                                   xrt_set_probe_f64_eq, &ctx);
}

static inline int64_t xrt_set_find_value(xrt_set_t *s, XrValue value) {
    if (s->elem_type == XR_ELEM_ANY) {
        xrt_set_probe_tag_ctx ctx;
        ctx.set = s;
        ctx.value = value;
        return xr_swiss_find_match_i64(s->ctrl, s->cap, xrt_hash_value(value), xrt_set_probe_tag_eq,
                                       &ctx);
    }
    if (s->elem_type == XR_ELEM_F32 || s->elem_type == XR_ELEM_F64) {
        double d = value.tag == XR_TAG_F64 ? value.f : (double) value.i;
        return xrt_set_find_f64_typed_slot(s, d, s->elem_type);
    }
    int64_t i = value.tag == XR_TAG_F64 ? (int64_t) value.f : value.i;
    return xrt_set_find_i64_typed_slot(s, i, s->elem_type);
}

static inline void xrt_set_rehash(xrt_set_t *s, int64_t new_slots) {
    int64_t old_slots = s->cap;
    uint8_t *old_ctrl = s->ctrl;
    void *old_items = s->items;

    // Walk order[] (insertion order) and remap each live slot in place, so the
    // insertion order survives the rehash; alloc_slots leaves order[] untouched.
    int64_t old_order_len = s->order_len;
    xrt_set_alloc_slots(s, new_slots);
    /* Compact: write only live entries to the front of order[], dropping
     * tombstones (w <= oi, so no read-after-write clobber). */
    int64_t w = 0;
    for (int64_t oi = 0; oi < old_order_len; oi++) {
        int64_t slot = s->order[oi];
        if (slot < 0 || slot >= old_slots || (old_ctrl[slot] & 0x80u))
            continue;
        uint64_t hash = s->elem_type == XR_ELEM_ANY
                            ? xrt_hash_value(((const XrValue *) old_items)[slot])
                            : xrt_map_stored_key_hash(old_items, slot, s->elem_type);
        int64_t dst = xr_swiss_find_free_i64(s->ctrl, s->cap, hash);
        xr_swiss_ctrl_set_i64(s->ctrl, s->cap, dst, (uint8_t) (hash & 0x7F));
        memcpy((uint8_t *) s->items + (size_t) dst * s->elem_size,
               (const uint8_t *) old_items + (size_t) slot * s->elem_size, s->elem_size);
        s->order[w++] = dst;
        s->growth_left--;
    }
    s->order_len = w;
    XRT_FREE(old_ctrl);
    XRT_FREE(old_items);
}

static inline int64_t xrt_set_insert_slot(xrt_set_t *s, uint64_t hash) {
    if (s->growth_left <= 0)
        xrt_set_rehash(s, s->len >= xr_swiss_capacity_budget_i64(s->cap) / 2 ? s->cap * 2 : s->cap);
    /* EMPTY-only: never reuse a tombstoned slot while a stale order[] entry may
     * still reference it; the reserve above guarantees an EMPTY slot exists. */
    int64_t slot = xr_swiss_find_empty_i64(s->ctrl, s->cap, hash);
    s->growth_left--;
    xr_swiss_ctrl_set_i64(s->ctrl, s->cap, slot, (uint8_t) (hash & 0x7F));
    s->len++;
    xrt_set_order_append(s, slot);
    return slot;
}

static inline void xrt_set_erase_slot(xrt_set_t *s, int64_t slot) {
    /* O(1) delete: tombstone the ctrl byte only; the order[] entry is skipped by
     * slot_is_full on iteration and dropped by rehash compaction, avoiding the old
     * O(n) order[] scan + memmove that made delete-heavy churn O(n^2)). */
    xr_swiss_ctrl_set_i64(s->ctrl, s->cap, slot, (uint8_t) XR_SWISS_CTRL_DELETED);
    s->len--;
}

/* ---- direct typed helpers (codegen-emitted, signatures are an ABI) ------ */

static inline int xrt_set_has_i64_typed(xrt_set_t *s, int64_t value, uint8_t elem_type) {
    if (s->elem_type != elem_type)
        xrt_set_direct_type_mismatch(elem_type, s->elem_type, "xrt_set_has_i64_typed");
    return xrt_set_find_i64_typed_slot(s, value, elem_type) >= 0;
}

static inline int xrt_set_add_i64_typed(xrt_set_t *s, int64_t value, uint8_t elem_type) {
    if (s->elem_type != elem_type)
        xrt_set_direct_type_mismatch(elem_type, s->elem_type, "xrt_set_add_i64_typed");
    uint64_t bits = xrt_set_item_bits_i64(value, elem_type);
    xrt_set_probe_i64_ctx ctx;
    ctx.set = s;
    ctx.bits = bits;
    uint64_t hash = xr_hash_core_mix_u64(bits);
    if (xr_swiss_find_match_i64(s->ctrl, s->cap, hash, xrt_set_probe_i64_eq, &ctx) >= 0)
        return 0;
    int64_t slot = xrt_set_insert_slot(s, hash);
    xrt_set_slot_store_i64(s, slot, value);
    return 1;
}

static inline int xrt_set_delete_i64_typed(xrt_set_t *s, int64_t value, uint8_t elem_type) {
    if (s->elem_type != elem_type)
        xrt_set_direct_type_mismatch(elem_type, s->elem_type, "xrt_set_delete_i64_typed");
    int64_t slot = xrt_set_find_i64_typed_slot(s, value, elem_type);
    if (slot < 0)
        return 0;
    xrt_set_erase_slot(s, slot);
    return 1;
}

static inline int xrt_set_has_f64_typed(xrt_set_t *s, double value, uint8_t elem_type) {
    if (s->elem_type != elem_type)
        xrt_set_direct_type_mismatch(elem_type, s->elem_type, "xrt_set_has_f64_typed");
    return xrt_set_find_f64_typed_slot(s, value, elem_type) >= 0;
}

static inline int xrt_set_add_f64_typed(xrt_set_t *s, double value, uint8_t elem_type) {
    if (s->elem_type != elem_type)
        xrt_set_direct_type_mismatch(elem_type, s->elem_type, "xrt_set_add_f64_typed");
    uint64_t hash = xr_hash_core_mix_u64(xrt_set_item_bits_f64(value, elem_type));
    xrt_set_probe_f64_ctx ctx;
    ctx.set = s;
    ctx.value = value;
    if (xr_swiss_find_match_i64(s->ctrl, s->cap, hash, xrt_set_probe_f64_eq, &ctx) >= 0)
        return 0;
    int64_t slot = xrt_set_insert_slot(s, hash);
    xrt_set_slot_store_f64(s, slot, value);
    return 1;
}

static inline int xrt_set_delete_f64_typed(xrt_set_t *s, double value, uint8_t elem_type) {
    if (s->elem_type != elem_type)
        xrt_set_direct_type_mismatch(elem_type, s->elem_type, "xrt_set_delete_f64_typed");
    int64_t slot = xrt_set_find_f64_typed_slot(s, value, elem_type);
    if (slot < 0)
        return 0;
    xrt_set_erase_slot(s, slot);
    return 1;
}

#endif /* XRT_SET_DIRECT_INC */
