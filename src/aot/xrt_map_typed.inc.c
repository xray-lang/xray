/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_map_typed.inc.c - Swiss-table probing for tagged and typed maps,
 * plus the direct AOT helpers emitted by codegen for class-native fields.
 *
 * Every lookup family funnels into xrt_map_probe_* which walk the shared
 * control bytes declared in xrt_coll.h.  Typed maps store packed scalar
 * keys/values addressed by slot; tagged maps store XrValue pairs.
 */

#ifndef XRT_MAP_TYPED_INC
#define XRT_MAP_TYPED_INC

static inline int xrt_map_is_typed(const xrt_map_t *m) {
    return m && m->key_type != XR_ELEM_ANY && m->value_type != XR_ELEM_ANY;
}

static inline void xrt_map_typed_abort(const char *who, const char *msg) {
    fprintf(stderr, "%s: %s\n", who ? who : "xrt_map_typed", msg ? msg : "invalid map state");
    abort();
}

static inline XrValue xrt_map_new_typed(int64_t cap, uint8_t key_type, uint8_t value_type) {
    if (key_type == XR_ELEM_ANY && value_type == XR_ELEM_ANY)
        return xrt_map_new(cap);
    if (key_type == XR_ELEM_ANY || value_type == XR_ELEM_ANY || key_type >= XR_ELEM_COUNT ||
        value_type >= XR_ELEM_COUNT)
        xrt_map_typed_abort("xrt_map_new_typed", "unsupported typed map layout");
    xrt_map_t *m = (xrt_map_t *) XRT_MALLOC(sizeof(xrt_map_t));
    if (!m)
        xrt_map_typed_abort("xrt_map_new_typed", "out of memory");
    xrt_map_init_header(m);
    xrt_coll_make_deterministic(&m->hdr);
    m->key_type = key_type;
    m->value_type = value_type;
    m->key_size = XR_ELEM_SIZES[key_type];
    m->value_size = XR_ELEM_SIZES[value_type];
    xrt_map_alloc_slots(m, xrt_swiss_slots_for(cap));
    return xr_mkptr(m, XR_TAG_MAP);
}

/* Slot accessors — valid only for slots whose ctrl byte is FULL. */
static inline XrValue xrt_map_slot_key(xrt_map_t *m, int64_t slot) {
    return xrt_map_is_typed(m) ? xr_typed_get(m->keys, (int32_t) slot, m->key_type)
                               : m->entries[slot].key;
}

static inline XrValue xrt_map_slot_value(xrt_map_t *m, int64_t slot) {
    return xrt_map_is_typed(m) ? xr_typed_get(m->values, (int32_t) slot, m->value_type)
                               : m->entries[slot].value;
}

/* ---- typed scalar key normalization ------------------------------------ */

/* Pack a scalar key into the canonical 64-bit pattern used for hashing and
 * raw comparison.  Sub-width integers zero/sign-extend through their stored
 * representation; floats canonicalize -0.0 so hash agrees with IEEE ==. */
static inline uint64_t xrt_map_key_bits_i64(int64_t key, uint8_t key_type) {
    switch (key_type) {
        case XR_ELEM_I8:
            return (uint64_t) (int8_t) key;
        case XR_ELEM_U8:
            return (uint64_t) (uint8_t) key;
        case XR_ELEM_I16:
            return (uint64_t) (int16_t) key;
        case XR_ELEM_U16:
            return (uint64_t) (uint16_t) key;
        case XR_ELEM_I32:
            return (uint64_t) (int32_t) key;
        case XR_ELEM_U32:
            return (uint64_t) (uint32_t) key;
        case XR_ELEM_I64:
        case XR_ELEM_U64:
            return (uint64_t) key;
        case XR_ELEM_BOOL:
            return key != 0 ? 1u : 0u;
        default:
            xrt_map_typed_abort("xrt_map_key_bits_i64", "unsupported integer key type");
    }
    return 0;
}

static inline uint64_t xrt_map_key_bits_f64(double key, uint8_t key_type) {
    if (key_type == XR_ELEM_F32) {
        float f = (float) key;
        uint32_t b32;
        if (f == 0.0f)
            f = 0.0f;
        memcpy(&b32, &f, sizeof(b32));
        return b32;
    }
    if (key_type == XR_ELEM_F64) {
        uint64_t b64;
        if (key == 0.0)
            key = 0.0;
        memcpy(&b64, &key, sizeof(b64));
        return b64;
    }
    xrt_map_typed_abort("xrt_map_key_bits_f64", "unsupported float key type");
    return 0;
}

static inline int64_t xrt_map_slot_key_raw(const xrt_map_t *m, int64_t slot) {
    switch (m->key_type) {
        case XR_ELEM_I8:
            return ((const int8_t *) m->keys)[slot];
        case XR_ELEM_U8:
        case XR_ELEM_BOOL:
            return ((const uint8_t *) m->keys)[slot];
        case XR_ELEM_I16:
            return ((const int16_t *) m->keys)[slot];
        case XR_ELEM_U16:
            return ((const uint16_t *) m->keys)[slot];
        case XR_ELEM_I32:
            return ((const int32_t *) m->keys)[slot];
        case XR_ELEM_U32:
            return ((const uint32_t *) m->keys)[slot];
        case XR_ELEM_I64:
        case XR_ELEM_U64:
            return ((const int64_t *) m->keys)[slot];
        default:
            xrt_map_typed_abort("xrt_map_slot_key_raw", "unsupported integer key type");
    }
    return 0;
}

static inline void xrt_map_slot_key_store_i64(xrt_map_t *m, int64_t slot, int64_t key) {
    switch (m->key_type) {
        case XR_ELEM_I8:
            ((int8_t *) m->keys)[slot] = (int8_t) key;
            return;
        case XR_ELEM_U8:
            ((uint8_t *) m->keys)[slot] = (uint8_t) key;
            return;
        case XR_ELEM_BOOL:
            ((uint8_t *) m->keys)[slot] = key != 0 ? 1 : 0;
            return;
        case XR_ELEM_I16:
            ((int16_t *) m->keys)[slot] = (int16_t) key;
            return;
        case XR_ELEM_U16:
            ((uint16_t *) m->keys)[slot] = (uint16_t) key;
            return;
        case XR_ELEM_I32:
            ((int32_t *) m->keys)[slot] = (int32_t) key;
            return;
        case XR_ELEM_U32:
            ((uint32_t *) m->keys)[slot] = (uint32_t) key;
            return;
        case XR_ELEM_I64:
        case XR_ELEM_U64:
            ((int64_t *) m->keys)[slot] = key;
            return;
        default:
            xrt_map_typed_abort("xrt_map_slot_key_store_i64", "unsupported integer key type");
    }
}

static inline void xrt_map_slot_key_store_f64(xrt_map_t *m, int64_t slot, double key) {
    if (m->key_type == XR_ELEM_F32) {
        ((float *) m->keys)[slot] = (float) key;
        return;
    }
    if (m->key_type == XR_ELEM_F64) {
        ((double *) m->keys)[slot] = key;
        return;
    }
    xrt_map_typed_abort("xrt_map_slot_key_store_f64", "unsupported float key type");
}

/* ---- probe helpers ------------------------------------------------------ */

typedef struct {
    xrt_map_t *map;
    uint64_t bits; /* canonical integer key pattern */
} xrt_map_probe_i64_ctx;

static inline int xrt_map_probe_i64_eq(void *ctxp, int64_t slot) {
    xrt_map_probe_i64_ctx *ctx = (xrt_map_probe_i64_ctx *) ctxp;
    return xrt_map_key_bits_i64(xrt_map_slot_key_raw(ctx->map, slot), ctx->map->key_type) ==
           ctx->bits;
}

typedef struct {
    xrt_map_t *map;
    double key;
} xrt_map_probe_f64_ctx;

static inline int xrt_map_probe_f64_eq(void *ctxp, int64_t slot) {
    xrt_map_probe_f64_ctx *ctx = (xrt_map_probe_f64_ctx *) ctxp;
    if (ctx->map->key_type == XR_ELEM_F32)
        return ((const float *) ctx->map->keys)[slot] == (float) ctx->key;
    return ((const double *) ctx->map->keys)[slot] == ctx->key;
}

static inline int64_t xrt_map_find_i64_hashed(xrt_map_t *m, uint64_t bits, uint64_t hash) {
    xrt_map_probe_i64_ctx ctx;
    ctx.map = m;
    ctx.bits = bits;
    return xrt_swiss_find(m->ctrl, m->cap, hash, xrt_map_probe_i64_eq, &ctx);
}

static inline int64_t xrt_map_find_f64_hashed(xrt_map_t *m, double key, uint64_t hash) {
    xrt_map_probe_f64_ctx ctx;
    ctx.map = m;
    ctx.key = key;
    return xrt_swiss_find(m->ctrl, m->cap, hash, xrt_map_probe_f64_eq, &ctx);
}

/* ---- rehash ------------------------------------------------------------- */

/* Hash the key stored at `slot` of a raw key array laid out for key_type. */
static inline uint64_t xrt_map_stored_key_hash(const void *keys, int64_t slot, uint8_t key_type) {
    switch (key_type) {
        case XR_ELEM_ANY:
            return xrt_hash_value(((const XrValue *) keys)[slot]);
        case XR_ELEM_F32:
            return xrt_hash_mix_u64(
                xrt_map_key_bits_f64((double) ((const float *) keys)[slot], XR_ELEM_F32));
        case XR_ELEM_F64:
            return xrt_hash_mix_u64(
                xrt_map_key_bits_f64(((const double *) keys)[slot], XR_ELEM_F64));
        case XR_ELEM_I8:
            return xrt_hash_mix_u64(
                xrt_map_key_bits_i64(((const int8_t *) keys)[slot], XR_ELEM_I8));
        case XR_ELEM_U8:
        case XR_ELEM_BOOL:
            return xrt_hash_mix_u64(xrt_map_key_bits_i64(((const uint8_t *) keys)[slot], key_type));
        case XR_ELEM_I16:
            return xrt_hash_mix_u64(
                xrt_map_key_bits_i64(((const int16_t *) keys)[slot], XR_ELEM_I16));
        case XR_ELEM_U16:
            return xrt_hash_mix_u64(
                xrt_map_key_bits_i64(((const uint16_t *) keys)[slot], XR_ELEM_U16));
        case XR_ELEM_I32:
            return xrt_hash_mix_u64(
                xrt_map_key_bits_i64(((const int32_t *) keys)[slot], XR_ELEM_I32));
        case XR_ELEM_U32:
            return xrt_hash_mix_u64(
                xrt_map_key_bits_i64(((const uint32_t *) keys)[slot], XR_ELEM_U32));
        case XR_ELEM_I64:
        case XR_ELEM_U64:
            return xrt_hash_mix_u64(xrt_map_key_bits_i64(((const int64_t *) keys)[slot], key_type));
        default:
            xrt_map_typed_abort("xrt_map_stored_key_hash", "unsupported key type");
    }
    return 0;
}

static inline void xrt_map_rehash(xrt_map_t *m, int64_t new_slots) {
    int64_t old_slots = m->cap;
    uint8_t *old_ctrl = m->ctrl;
    void *old_keys = m->keys;
    void *old_values = m->values;

    int64_t old_order_len = m->order_len;
    xrt_map_alloc_slots(m, new_slots);
    if (old_order_len > 0) {
        /* Compact: write only live entries to the front of order[], dropping
         * tombstones (w <= oi, so no read-after-write clobber). */
        int64_t w = 0;
        for (int64_t oi = 0; oi < old_order_len; oi++) {
            int64_t slot = m->order[oi];
            if (slot < 0 || slot >= old_slots || (old_ctrl[slot] & 0x80u))
                continue;
            uint64_t hash = xrt_map_stored_key_hash(old_keys, slot, m->key_type);
            int64_t dst = xrt_swiss_find_free(m->ctrl, m->cap, hash);
            xrt_swiss_ctrl_set(m->ctrl, m->cap, dst, (uint8_t) (hash & 0x7F));
            memcpy((uint8_t *) m->keys + (size_t) dst * m->key_size,
                   (const uint8_t *) old_keys + (size_t) slot * m->key_size, m->key_size);
            memcpy((uint8_t *) m->values + (size_t) dst * m->value_size,
                   (const uint8_t *) old_values + (size_t) slot * m->value_size, m->value_size);
            m->order[w++] = dst;
            m->growth_left--;
        }
        m->order_len = w;
    } else {
        for (int64_t slot = 0; slot < old_slots; slot++) {
            if (old_ctrl[slot] & 0x80u)
                continue;
            uint64_t hash = xrt_map_stored_key_hash(old_keys, slot, m->key_type);
            int64_t dst = xrt_swiss_find_free(m->ctrl, m->cap, hash);
            xrt_swiss_ctrl_set(m->ctrl, m->cap, dst, (uint8_t) (hash & 0x7F));
            memcpy((uint8_t *) m->keys + (size_t) dst * m->key_size,
                   (const uint8_t *) old_keys + (size_t) slot * m->key_size, m->key_size);
            memcpy((uint8_t *) m->values + (size_t) dst * m->value_size,
                   (const uint8_t *) old_values + (size_t) slot * m->value_size, m->value_size);
            m->growth_left--;
        }
    }
    XRT_FREE(old_ctrl);
    XRT_FREE(old_keys);
    XRT_FREE(old_values);
}

static inline void xrt_map_rehash_tagged(xrt_map_t *m, int64_t new_slots) {
    xrt_map_rehash(m, new_slots);
}

/* Grow when the free budget is gone: double when genuinely loaded, otherwise
 * rebuild at the same size to flush tombstones. */
static inline void xrt_map_reserve_one(xrt_map_t *m) {
    if (m->growth_left > 0)
        return;
    xrt_map_rehash(m, m->len >= xrt_map_growth_budget(m->cap) / 2 ? m->cap * 2 : m->cap);
}

static inline int64_t xrt_map_insert_slot(xrt_map_t *m, uint64_t hash) {
    xrt_map_reserve_one(m);
    /* EMPTY-only: never reuse a tombstoned slot while a stale order[] entry may
     * still reference it; reserve_one guarantees an EMPTY slot exists. */
    int64_t slot = xrt_swiss_find_empty(m->ctrl, m->cap, hash);
    m->growth_left--;
    xrt_swiss_ctrl_set(m->ctrl, m->cap, slot, (uint8_t) (hash & 0x7F));
    m->len++;
    xrt_map_order_append(m, slot);
    return slot;
}

static inline void xrt_map_erase_slot(xrt_map_t *m, int64_t slot) {
    /* O(1) delete: tombstone the ctrl byte only. The slot's order[] entry stays
     * in place and is skipped by slot_is_full during iteration, then dropped by
     * rehash compaction, avoiding the old O(n) order[] scan + memmove that made
     * delete-heavy churn O(n^2). */
    xrt_swiss_ctrl_set(m->ctrl, m->cap, slot, (uint8_t) XRT_CTRL_DELETED);
    m->len--;
}

/* ---- tagged entry points via XrValue keys (typed maps unbox first) ------ */

static inline int64_t xrt_map_find_typed(xrt_map_t *m, XrValue key) {
    if (m->key_type == XR_ELEM_F32 || m->key_type == XR_ELEM_F64) {
        double k = key.tag == XR_TAG_F64 ? key.f : (double) key.i;
        return xrt_map_find_f64_hashed(m, k,
                                       xrt_hash_mix_u64(xrt_map_key_bits_f64(k, m->key_type)));
    }
    int64_t k = key.tag == XR_TAG_F64 ? (int64_t) key.f : key.i;
    uint64_t bits = xrt_map_key_bits_i64(k, m->key_type);
    return xrt_map_find_i64_hashed(m, bits, xrt_hash_mix_u64(bits));
}

static inline XrValue xrt_map_get_typed(xrt_map_t *m, XrValue key) {
    int64_t slot = xrt_map_find_typed(m, key);
    return slot >= 0 ? xrt_map_slot_value(m, slot) : XR_NULL_VAL;
}

static inline int xrt_map_has_typed(xrt_map_t *m, XrValue key) {
    return xrt_map_find_typed(m, key) >= 0;
}

static inline int xrt_map_delete_typed(xrt_map_t *m, XrValue key) {
    int64_t slot = xrt_map_find_typed(m, key);
    if (slot < 0)
        return 0;
    xrt_map_erase_slot(m, slot);
    return 1;
}

static inline void xrt_map_set_typed(xrt_map_t *m, XrValue key, XrValue val) {
    int64_t slot = xrt_map_find_typed(m, key);
    if (slot < 0) {
        uint64_t hash;
        if (m->key_type == XR_ELEM_F32 || m->key_type == XR_ELEM_F64) {
            double k = key.tag == XR_TAG_F64 ? key.f : (double) key.i;
            hash = xrt_hash_mix_u64(xrt_map_key_bits_f64(k, m->key_type));
        } else {
            int64_t k = key.tag == XR_TAG_F64 ? (int64_t) key.f : key.i;
            hash = xrt_hash_mix_u64(xrt_map_key_bits_i64(k, m->key_type));
        }
        slot = xrt_map_insert_slot(m, hash);
        (void) xr_typed_set(m->keys, (int32_t) slot, key, m->key_type);
    }
    (void) xr_typed_set(m->values, (int32_t) slot, val, m->value_type);
}

/* ---- direct AOT helpers (codegen-emitted, signatures are an ABI) -------- */

static inline void xrt_map_direct_type_mismatch(uint8_t expected_key, uint8_t actual_key,
                                                uint8_t expected_value, uint8_t actual_value,
                                                const char *who) {
    fprintf(stderr, "%s: map layout mismatch (key %u != %u, value %u != %u)\n",
            who ? who : "xrt_map_direct", (unsigned) expected_key, (unsigned) actual_key,
            (unsigned) expected_value, (unsigned) actual_value);
    abort();
}

static inline void xrt_map_direct_unsupported(uint8_t elem_type, const char *role,
                                              const char *who) {
    fprintf(stderr, "%s: unsupported %s map element type %u\n", who ? who : "xrt_map_direct",
            role ? role : "typed", (unsigned) elem_type);
    abort();
}

static inline void xrt_map_direct_require(xrt_map_t *m, uint8_t key_type, uint8_t value_type,
                                          const char *who) {
    if (!m || m->key_type != key_type || m->value_type != value_type) {
        uint8_t actual_key = m ? m->key_type : XR_ELEM_ANY;
        uint8_t actual_value = m ? m->value_type : XR_ELEM_ANY;
        xrt_map_direct_type_mismatch(key_type, actual_key, value_type, actual_value, who);
    }
}

static inline int64_t xrt_map_find_i64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                             uint8_t value_type) {
    (void) value_type;
    uint64_t bits = xrt_map_key_bits_i64(key, key_type);
    return xrt_map_find_i64_hashed(m, bits, xrt_hash_mix_u64(bits));
}

static inline int64_t xrt_map_find_bool_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                              uint8_t value_type) {
    (void) key_type;
    (void) value_type;
    uint8_t needle = key != 0 ? 1u : 0u;
    const uint8_t *keys = (const uint8_t *) m->keys;
    if (m->len == m->order_len) {
        if (m->len <= 0)
            return -1;
        int64_t slot0 = m->order[0];
        if (keys[slot0] == needle)
            return slot0;
        if (m->len > 1) {
            int64_t slot1 = m->order[1];
            if (keys[slot1] == needle)
                return slot1;
        }
        return -1;
    }
    // At most two live bool keys, but order[] may carry tombstoned slots after
    // lazy delete; skip dead slots (ctrl top bit set = EMPTY/DELETED) so a stale
    // key byte can't false-match.
    for (int64_t oi = 0; oi < m->order_len; oi++) {
        int64_t slot = m->order[oi];
        if (slot < 0 || (m->ctrl[slot] & 0x80u))
            continue;
        if (keys[slot] == needle)
            return slot;
    }
    return -1;
}

static inline int64_t xrt_map_find_f64_typed(xrt_map_t *m, double key, uint8_t key_type,
                                             uint8_t value_type) {
    (void) value_type;
    return xrt_map_find_f64_hashed(m, key, xrt_hash_mix_u64(xrt_map_key_bits_f64(key, key_type)));
}

static inline int64_t xrt_map_get_i64_value_typed(xrt_map_t *m, int64_t slot, uint8_t value_type) {
    switch (value_type) {
        case XR_ELEM_I8:
            return ((const int8_t *) m->values)[slot];
        case XR_ELEM_U8:
        case XR_ELEM_BOOL:
            return ((const uint8_t *) m->values)[slot];
        case XR_ELEM_I16:
            return ((const int16_t *) m->values)[slot];
        case XR_ELEM_U16:
            return ((const uint16_t *) m->values)[slot];
        case XR_ELEM_I32:
            return ((const int32_t *) m->values)[slot];
        case XR_ELEM_U32:
            return ((const uint32_t *) m->values)[slot];
        case XR_ELEM_I64:
        case XR_ELEM_U64:
            return ((const int64_t *) m->values)[slot];
        default:
            xrt_map_direct_unsupported(value_type, "value", "xrt_map_get_i64_value_typed");
    }
    return 0;
}

static inline double xrt_map_get_f64_value_typed(xrt_map_t *m, int64_t slot, uint8_t value_type) {
    if (value_type == XR_ELEM_F32)
        return (double) ((const float *) m->values)[slot];
    if (value_type == XR_ELEM_F64)
        return ((const double *) m->values)[slot];
    xrt_map_direct_unsupported(value_type, "value", "xrt_map_get_f64_value_typed");
    return 0.0;
}

static inline void xrt_map_store_i64_value_typed(xrt_map_t *m, int64_t slot, int64_t value,
                                                 uint8_t value_type) {
    switch (value_type) {
        case XR_ELEM_I8:
            ((int8_t *) m->values)[slot] = (int8_t) value;
            return;
        case XR_ELEM_U8:
            ((uint8_t *) m->values)[slot] = (uint8_t) value;
            return;
        case XR_ELEM_BOOL:
            ((uint8_t *) m->values)[slot] = value != 0 ? 1 : 0;
            return;
        case XR_ELEM_I16:
            ((int16_t *) m->values)[slot] = (int16_t) value;
            return;
        case XR_ELEM_U16:
            ((uint16_t *) m->values)[slot] = (uint16_t) value;
            return;
        case XR_ELEM_I32:
            ((int32_t *) m->values)[slot] = (int32_t) value;
            return;
        case XR_ELEM_U32:
            ((uint32_t *) m->values)[slot] = (uint32_t) value;
            return;
        case XR_ELEM_I64:
        case XR_ELEM_U64:
            ((int64_t *) m->values)[slot] = value;
            return;
        default:
            xrt_map_direct_unsupported(value_type, "value", "xrt_map_store_i64_value_typed");
    }
}

static inline void xrt_map_store_f64_value_typed(xrt_map_t *m, int64_t slot, double value,
                                                 uint8_t value_type) {
    if (value_type == XR_ELEM_F32) {
        ((float *) m->values)[slot] = (float) value;
        return;
    }
    if (value_type == XR_ELEM_F64) {
        ((double *) m->values)[slot] = value;
        return;
    }
    xrt_map_direct_unsupported(value_type, "value", "xrt_map_store_f64_value_typed");
}

static inline int64_t xrt_map_get_i64_i64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                                uint8_t value_type) {
    int64_t slot = xrt_map_find_i64_typed(m, key, key_type, value_type);
    return slot >= 0 ? xrt_map_get_i64_value_typed(m, slot, value_type) : 0;
}

static inline double xrt_map_get_i64_f64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                               uint8_t value_type) {
    int64_t slot = xrt_map_find_i64_typed(m, key, key_type, value_type);
    return slot >= 0 ? xrt_map_get_f64_value_typed(m, slot, value_type) : 0.0;
}

static inline int64_t xrt_map_get_f64_i64_typed(xrt_map_t *m, double key, uint8_t key_type,
                                                uint8_t value_type) {
    int64_t slot = xrt_map_find_f64_typed(m, key, key_type, value_type);
    return slot >= 0 ? xrt_map_get_i64_value_typed(m, slot, value_type) : 0;
}

static inline double xrt_map_get_f64_f64_typed(xrt_map_t *m, double key, uint8_t key_type,
                                               uint8_t value_type) {
    int64_t slot = xrt_map_find_f64_typed(m, key, key_type, value_type);
    return slot >= 0 ? xrt_map_get_f64_value_typed(m, slot, value_type) : 0.0;
}

static inline int xrt_map_has_i64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                        uint8_t value_type) {
    return xrt_map_find_i64_typed(m, key, key_type, value_type) >= 0;
}

static inline int xrt_map_has_f64_typed(xrt_map_t *m, double key, uint8_t key_type,
                                        uint8_t value_type) {
    return xrt_map_find_f64_typed(m, key, key_type, value_type) >= 0;
}

static inline int xrt_map_delete_i64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                           uint8_t value_type) {
    int64_t slot = xrt_map_find_i64_typed(m, key, key_type, value_type);
    if (slot < 0)
        return 0;
    xrt_map_erase_slot(m, slot);
    return 1;
}

static inline int xrt_map_delete_f64_typed(xrt_map_t *m, double key, uint8_t key_type,
                                           uint8_t value_type) {
    int64_t slot = xrt_map_find_f64_typed(m, key, key_type, value_type);
    if (slot < 0)
        return 0;
    xrt_map_erase_slot(m, slot);
    return 1;
}

static inline void xrt_map_set_i64_i64_typed(xrt_map_t *m, int64_t key, int64_t value,
                                             uint8_t key_type, uint8_t value_type) {
    uint64_t bits = xrt_map_key_bits_i64(key, key_type);
    uint64_t hash = xrt_hash_mix_u64(bits);
    int64_t slot = xrt_map_find_i64_hashed(m, bits, hash);
    if (slot < 0) {
        slot = xrt_map_insert_slot(m, hash);
        xrt_map_slot_key_store_i64(m, slot, key);
    }
    xrt_map_store_i64_value_typed(m, slot, value, value_type);
}

static inline void xrt_map_set_i64_f64_typed(xrt_map_t *m, int64_t key, double value,
                                             uint8_t key_type, uint8_t value_type) {
    uint64_t bits = xrt_map_key_bits_i64(key, key_type);
    uint64_t hash = xrt_hash_mix_u64(bits);
    int64_t slot = xrt_map_find_i64_hashed(m, bits, hash);
    if (slot < 0) {
        slot = xrt_map_insert_slot(m, hash);
        xrt_map_slot_key_store_i64(m, slot, key);
    }
    xrt_map_store_f64_value_typed(m, slot, value, value_type);
}

static inline void xrt_map_set_f64_i64_typed(xrt_map_t *m, double key, int64_t value,
                                             uint8_t key_type, uint8_t value_type) {
    uint64_t hash = xrt_hash_mix_u64(xrt_map_key_bits_f64(key, key_type));
    int64_t slot = xrt_map_find_f64_hashed(m, key, hash);
    if (slot < 0) {
        slot = xrt_map_insert_slot(m, hash);
        xrt_map_slot_key_store_f64(m, slot, key);
    }
    xrt_map_store_i64_value_typed(m, slot, value, value_type);
}

static inline void xrt_map_set_f64_f64_typed(xrt_map_t *m, double key, double value,
                                             uint8_t key_type, uint8_t value_type) {
    uint64_t hash = xrt_hash_mix_u64(xrt_map_key_bits_f64(key, key_type));
    int64_t slot = xrt_map_find_f64_hashed(m, key, hash);
    if (slot < 0) {
        slot = xrt_map_insert_slot(m, hash);
        xrt_map_slot_key_store_f64(m, slot, key);
    }
    xrt_map_store_f64_value_typed(m, slot, value, value_type);
}

/* bool-key direct families. codegen only emits these names for Map<bool,i64> and
 * Map<bool,f32>, whose receiver is always the 2-slot xrt_boolmap_t, so they cast
 * and operate directly with no representation dispatch. Other bool-keyed value
 * types (i32, f64, ...) stay generic typed maps and route through the i64/f64
 * helpers, never here. */

static inline int64_t xrt_map_get_bool_i64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                                 uint8_t value_type) {
    (void) key_type;
    int64_t slot = xrt_boolmap_find(m, key, XR_ELEM_BOOL, value_type);
    return slot >= 0 ? xrt_boolmap_value_i64(m, slot, value_type) : 0;
}

static inline int xrt_map_has_bool_i64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                             uint8_t value_type) {
    (void) key_type;
    return xrt_boolmap_find(m, key, XR_ELEM_BOOL, value_type) >= 0;
}

static inline int xrt_map_delete_bool_i64_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                                uint8_t value_type) {
    (void) key_type;
    (void) value_type;
    return xrt_boolmap_delete((xrt_boolmap_t *) m, key);
}

static inline void xrt_map_set_bool_i64_typed(xrt_map_t *m, int64_t key, int64_t value,
                                              uint8_t key_type, uint8_t value_type) {
    (void) key_type;
    (void) value_type;
    xrt_boolmap_set_i64((xrt_boolmap_t *) m, key, value);
}

static inline double xrt_map_get_bool_f32_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                                uint8_t value_type) {
    (void) key_type;
    int64_t slot = xrt_boolmap_find(m, key, XR_ELEM_BOOL, value_type);
    return slot >= 0 ? xrt_boolmap_value_f64(m, slot, value_type) : 0.0;
}

static inline int xrt_map_has_bool_f32_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                             uint8_t value_type) {
    (void) key_type;
    return xrt_boolmap_find(m, key, XR_ELEM_BOOL, value_type) >= 0;
}

static inline int xrt_map_delete_bool_f32_typed(xrt_map_t *m, int64_t key, uint8_t key_type,
                                                uint8_t value_type) {
    (void) key_type;
    (void) value_type;
    return xrt_boolmap_delete((xrt_boolmap_t *) m, key);
}

static inline void xrt_map_set_bool_f32_typed(xrt_map_t *m, int64_t key, double value,
                                              uint8_t key_type, uint8_t value_type) {
    (void) key_type;
    (void) value_type;
    xrt_boolmap_set_f32((xrt_boolmap_t *) m, key, value);
}

static inline double xrt_map_get_f32_f32_typed(xrt_map_t *m, double key, uint8_t key_type,
                                               uint8_t value_type) {
    (void) key_type;
    return xrt_map_get_f64_f64_typed(m, key, XR_ELEM_F32, value_type);
}

static inline int xrt_map_has_f32_f32_typed(xrt_map_t *m, double key, uint8_t key_type,
                                            uint8_t value_type) {
    (void) key_type;
    return xrt_map_has_f64_typed(m, key, XR_ELEM_F32, value_type);
}

static inline int xrt_map_delete_f32_f32_typed(xrt_map_t *m, double key, uint8_t key_type,
                                               uint8_t value_type) {
    (void) key_type;
    return xrt_map_delete_f64_typed(m, key, XR_ELEM_F32, value_type);
}

static inline void xrt_map_set_f32_f32_typed(xrt_map_t *m, double key, double value,
                                             uint8_t key_type, uint8_t value_type) {
    (void) key_type;
    xrt_map_set_f64_f64_typed(m, key, value, XR_ELEM_F32, value_type);
}

#endif /* XRT_MAP_TYPED_INC */
