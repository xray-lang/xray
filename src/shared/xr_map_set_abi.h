/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_map_set_abi.h - Shared dense-entry Map/Set ABI.
 *
 * Dependency: the including header must define XrValue first. This keeps the
 * ABI usable from both the VM runtime and the standalone AOT runtime without
 * forcing either side to include the other's value header.
 */

#ifndef XR_MAP_SET_ABI_H
#define XR_MAP_SET_ABI_H

#include "xr_swiss_index.h"
#include <stdint.h>

/* ========================================================================
 * Sharing boundary (VM xmap.c/xset.c vs AOT xrt_coll.h)
 *
 * SHARED here and in xr_swiss_index.h is the allocator/GC-free *algorithmic*
 * core of the dense-entry Map/Set: the Swiss control-byte probe primitives,
 * index insertion (xr_swiss_indices_put), compact rehash (xr_*_rehash_into),
 * and candidate lookup (xr_*_lookup_slot, with a per-backend equality
 * comparator). After the tag/eq/hash unification these are byte-identical
 * across backends, so one source of truth removes drift.
 *
 * NOT shared, by design, are the mutating operator bodies (add/set/delete/
 * clear). Their remaining differences are entirely backend *policy*, not
 * algorithm: table allocation (VM Region blobs or malloc with external-byte
 * accounting; AOT bump/calloc), reference counting (VM xr_rc_retain/release),
 * the weak-key/value registry. AOT has none of those VM policies.
 * Routing those bodies through shared callbacks would put indirection on the
 * hot mutation/resize path for negligible dedup (the resize step also mutates
 * the table pointers, which fights the raw-fields contract the shared cores
 * use), so each backend keeps its own add/delete/clear around the shared cores.
 * ======================================================================== */

/* ========== Map Entry (insertion-order dense slot) ========== */

typedef struct XrMapEntry {
    XrValue value;
    XrValue key;
    uint32_t hash;  /* Cached key hash (avoids recompute on resize/lookup) */
    uint8_t key_tt; /* Key type tag (+1); 0 = empty/tombstone slot */
    uint8_t _pad[3];
} XrMapEntry;

#define XR_MAP_ENTRY_NIL_KEY 0
#define XR_MAP_ENTRY_EMPTY(e) ((e)->key_tt == XR_MAP_ENTRY_NIL_KEY)

/* Debug sentinel for indices[] slots whose ctrl byte is not FULL. FULL slots
 * always store a direct entries[] index. */
#define XR_MAP_IX_EMPTY (-1)

#define XR_MAP_ABI_FIELDS                                                                          \
    uint32_t count;        /* Live entries (excludes tombstones) */                                \
    uint32_t nentries;     /* Used entries incl. tombstones (= next append index) */               \
    uint32_t entries_cap;  /* Allocated entries[] capacity */                                      \
    uint32_t indices_size; /* indices[] slot count (power of two, 0 = dummy) */                    \
    uint8_t *ctrl;         /* Swiss control bytes, indices_size + XR_SWISS_GROUP */                \
    int32_t *indices;      /* FULL ctrl slots -> entries index */                                  \
    XrMapEntry *entries;   /* Dense insertion-order array */                                       \
    uint8_t flags;                                                                                 \
    uint8_t key_tid;   /* XrTypeId / storage tag for reified generic keys (0=any) */               \
    uint8_t value_tid; /* XrTypeId / storage tag for reified generic values (0=any) */             \
    uint8_t _pad

typedef struct XrMapCore {
    XR_MAP_ABI_FIELDS;
} XrMapCore;

#define XR_MAP_FLAG_WEAK 0x01
#define XR_MAP_FLAG_DUMMY 0x02           /* Empty map: no ctrl/indices/entries allocation */
#define XR_MAP_FLAG_NODES_ON_GC 0x04     /* ctrl/indices/entries live on Region GC heap */
#define XR_MAP_FLAG_WEAK_REGISTERED 0x08 /* Registered in the runtime weak registry */
#define XR_MAP_FLAG_NODES_ON_STACK 0x10  /* AOT stack Map nodes; never free or resize */

#define xr_map_isdummy(m) ((m)->flags & XR_MAP_FLAG_DUMMY)

#define XR_MAP_MAXHBITS 30

/* Compact the live entries of a Map into freshly allocated tables, preserving
 * insertion order, dropping tombstones, and rebuilding the index table. Returns
 * the new live-entry count. This is the GC-free algorithmic core of Map resize,
 * shared by the VM (xmap.c) and AOT (xrt_coll.h) resize paths; allocation, free
 * and GC accounting of the tables stay backend-specific around it. */
static inline uint32_t xr_map_rehash_into(XrMapEntry *new_entries, uint8_t *new_ctrl,
                                          int32_t *new_indices, uint32_t new_indices_size,
                                          const XrMapEntry *old_entries, uint32_t old_nentries) {
    uint32_t w = 0;
    for (uint32_t i = 0; i < old_nentries; i++) {
        const XrMapEntry *oe = &old_entries[i];
        if (oe->key_tt == XR_MAP_ENTRY_NIL_KEY)
            continue;
        new_entries[w] = *oe;
        xr_swiss_indices_put(new_ctrl, new_indices, new_indices_size, oe->hash, (int32_t) w);
        w++;
    }
    return w;
}

/* Per-backend key comparator for a candidate map entry. `query_tt` is the
 * query key's type tag, precomputed by the caller. Invoked only on h2-matched
 * candidates (never inside the probe loop), so the indirect call stays off the
 * hot path; with a constant function argument and -O2 the compiler inlines the
 * lookup and devirtualizes this call. Returns nonzero on equal (int, not bool,
 * to match the AOT runtime headers' bool-free generated-C convention). */
typedef int (*XrMapEqFn)(const XrMapEntry *e, XrValue query, uint8_t query_tt);

/* Locate the ctrl/indices slot whose live entry matches (query, query_tt) with
 * `hash`, writing its entries[] index to *out_eidx; returns UINT32_MAX if
 * absent. This is the shared Swiss-probe core of tagged-map lookup, used by the
 * VM (xmap.c) and AOT (xrt_coll.h); the key comparison is supplied via `eq`,
 * and table allocation/representation stay backend-specific. Pass raw fields
 * (not a struct) because the VM and AOT map structs carry an object header before
 * the shared ABI fields, so neither is castable to a common overlay. */
static inline uint32_t xr_map_lookup_slot(const uint8_t *ctrl, const int32_t *indices,
                                          const XrMapEntry *entries, uint32_t indices_size,
                                          XrValue query, uint32_t hash, uint8_t query_tt,
                                          XrMapEqFn eq, int32_t *out_eidx) {
    if (indices_size == 0)
        return UINT32_MAX; /* dummy: no allocation */
    uint32_t mask = indices_size - 1u;
    uint8_t h2 = xr_swiss_h2(hash);
    uint32_t pos = (uint32_t) ((hash >> 7u) & mask);
    uint32_t stride = 0;
    for (;;) {
        uint64_t group = xr_swiss_group_load(ctrl + pos);
        uint64_t matches = xr_swiss_group_match(group, h2);
        while (matches) {
            int off = xr_swiss_swar_first(matches);
            uint32_t slot = (pos + (uint32_t) off) & mask;
            int32_t ix = indices[slot];
            if (ix >= 0) {
                const XrMapEntry *e = &entries[ix];
                if (e->hash == hash && eq(e, query, query_tt)) {
                    if (out_eidx)
                        *out_eidx = ix;
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

/* ========== Set Entry (insertion-order dense slot) ========== */

typedef struct XrSetEntry {
    XrValue value;
    uint32_t hash;  /* Cached value hash (avoids recompute on resize/lookup) */
    uint8_t val_tt; /* Value type tag (+1); 0 = empty/tombstone slot */
    uint8_t _pad[3];
} XrSetEntry;

#define XR_SET_ENTRY_NIL 0
#define XR_SET_ENTRY_EMPTY(e) ((e)->val_tt == XR_SET_ENTRY_NIL)

/* Debug sentinel for indices[] slots whose ctrl byte is not FULL. FULL slots
 * always store a direct entries[] index. */
#define XR_SET_IX_EMPTY (-1)

#define XR_SET_ABI_FIELDS                                                                          \
    uint32_t count;        /* Live entries (excludes tombstones) */                                \
    uint32_t nentries;     /* Used entries incl. tombstones (= next append index) */               \
    uint32_t entries_cap;  /* Allocated entries[] capacity */                                      \
    uint32_t indices_size; /* indices[] slot count (power of two, 0 = dummy) */                    \
    uint8_t *ctrl;         /* Swiss control bytes, indices_size + XR_SWISS_GROUP */                \
    int32_t *indices;      /* FULL ctrl slots -> entries index */                                  \
    XrSetEntry *entries;   /* Dense insertion-order array */                                       \
    uint8_t flags;                                                                                 \
    uint8_t elem_tid; /* XrTypeId / storage tag for reified generic elements (0=any) */            \
    uint8_t _pad[2]

typedef struct XrSetCore {
    XR_SET_ABI_FIELDS;
} XrSetCore;

#define XR_SET_FLAG_WEAK 0x01
#define XR_SET_FLAG_DUMMY 0x02           /* Empty set: no ctrl/indices/entries allocation */
#define XR_SET_FLAG_NODES_ON_GC 0x04     /* ctrl/indices/entries live on Region GC heap */
#define XR_SET_FLAG_WEAK_REGISTERED 0x08 /* Registered in the runtime weak registry */
#define XR_SET_FLAG_NODES_ON_STACK 0x10  /* AOT stack Set nodes; never free or resize */

#define xr_set_isdummy(s) ((s)->flags & XR_SET_FLAG_DUMMY)

#define XR_SET_MAXHBITS 30

/* Compact the live entries of a Set into freshly allocated tables, preserving
 * insertion order, dropping tombstones, and rebuilding the index table. Returns
 * the new live-entry count. This is the GC-free algorithmic core of Set resize,
 * shared by the VM (xset.c) and AOT (xrt_coll.h) resize paths; allocation, free
 * and GC accounting of the tables stay backend-specific around it. */
static inline uint32_t xr_set_rehash_into(XrSetEntry *new_entries, uint8_t *new_ctrl,
                                          int32_t *new_indices, uint32_t new_indices_size,
                                          const XrSetEntry *old_entries, uint32_t old_nentries) {
    uint32_t w = 0;
    for (uint32_t i = 0; i < old_nentries; i++) {
        const XrSetEntry *oe = &old_entries[i];
        if (oe->val_tt == XR_SET_ENTRY_NIL)
            continue;
        new_entries[w] = *oe;
        xr_swiss_indices_put(new_ctrl, new_indices, new_indices_size, oe->hash, (int32_t) w);
        w++;
    }
    return w;
}

/* Per-backend value comparator for a candidate set entry. `query_tt` is the
 * query value's type tag, precomputed by the caller. See XrMapEqFn for why the
 * indirect call is off the hot path and why it returns int rather than bool. */
typedef int (*XrSetEqFn)(const XrSetEntry *e, XrValue query, uint8_t query_tt);

/* Locate the ctrl/indices slot whose live entry matches (query, query_tt) with
 * `hash`; returns UINT32_MAX if absent. Shared Swiss-probe core of tagged-set
 * lookup for the VM (xset.c) and AOT (xrt_coll.h); see xr_map_lookup_slot for
 * the raw-fields rationale. */
static inline uint32_t xr_set_lookup_slot(const uint8_t *ctrl, const int32_t *indices,
                                          const XrSetEntry *entries, uint32_t indices_size,
                                          XrValue query, uint32_t hash, uint8_t query_tt,
                                          XrSetEqFn eq, int32_t *out_eidx) {
    if (indices_size == 0)
        return UINT32_MAX; /* dummy: no allocation */
    uint32_t mask = indices_size - 1u;
    uint8_t h2 = xr_swiss_h2(hash);
    uint32_t pos = (uint32_t) ((hash >> 7u) & mask);
    uint32_t stride = 0;
    for (;;) {
        uint64_t group = xr_swiss_group_load(ctrl + pos);
        uint64_t matches = xr_swiss_group_match(group, h2);
        while (matches) {
            int off = xr_swiss_swar_first(matches);
            uint32_t slot = (pos + (uint32_t) off) & mask;
            int32_t ix = indices[slot];
            if (ix >= 0) {
                const XrSetEntry *e = &entries[ix];
                if (e->hash == hash && eq(e, query, query_tt)) {
                    if (out_eidx)
                        *out_eidx = ix;
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

#endif  // XR_MAP_SET_ABI_H
