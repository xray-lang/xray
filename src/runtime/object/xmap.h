/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmap.h - Compact ordered hash map (CPython-style compact dict)
 *
 * KEY CONCEPT:
 *   - Insertion-ordered: entries[] is a dense, append-only array kept in the
 *     order keys were first inserted; iteration scans it directly, so order
 *     matches the language spec (Map preserves insertion order).
 *   - ctrl[] is a Swiss-style h2 control-byte table; indices[] stores the
 *     corresponding entries[] index for FULL ctrl slots.
 *   - Deletion tombstones the entry (key_tt = 0) and marks its ctrl slot
 *     DELETED; dead entries are reclaimed when the table is resized/compacted.
 *   - Empty map allocates nothing (entries/indices NULL, DUMMY flag set); the
 *     first insertion allocates both arrays.
 *   - Short strings are force-interned, so string keys compare by pointer.
 */

#ifndef XMAP_H
#define XMAP_H

#include "../value/xvalue.h"
#include "xstring.h"
#include "../gc/xgc_header.h"
#include "../gc/xgc_internal.h"
#include "../gc/xalloc_unified.h"
#include "xarray.h"
#include "../../shared/xr_map_set_abi.h"
#include <stdint.h>
#include <stdbool.h>

/* ========== Map Object ========== */

typedef struct XrMap {
    XrObjHeader hdr;
    struct XrCoroHeap *owner_heap;
    XR_MAP_ABI_FIELDS;
} XrMap;

// Macros
#define xr_map_entry(m, i) (&(m)->entries[i])

// Initialize map in-place
XR_FUNC void xr_map_init_inplace(XrMap *map, uint32_t capacity_hint);

/* ========== Inline Fast Path (VM optimization) ========== */

// Inline string-key lookup over the compact-dict Swiss index table.
// Assumes the probed key string is interned (pointer comparison), matching
// how constant string keys are emitted by the front end.
static inline XrMapEntry *xr_map_find_string_fast(XrMap *map, XrString *key_str) {
    if (map->flags & XR_MAP_FLAG_DUMMY)
        return NULL;

    uint32_t mask = map->indices_size - 1;
    uint32_t hash = key_str->hash;
    uint8_t h2 = xr_swiss_h2(hash);
    uint32_t pos = (hash >> 7u) & mask;
    uint32_t stride = 0;

    for (;;) {
        uint64_t group = xr_swiss_group_load(map->ctrl + pos);
        uint64_t matches = xr_swiss_group_match(group, h2);
        while (matches) {
            int off = xr_swiss_swar_first(matches);
            uint32_t slot = (pos + (uint32_t) off) & mask;
            int32_t ix = map->indices[slot];
            if (ix >= 0) {
                XrMapEntry *e = &map->entries[ix];
                if (e->hash == hash && e->key_tt == (XR_TID_STRING + 1) &&
                    XR_TO_STRING(e->key) == key_str) {
                    return e;
                }
            }
            matches &= ~(0xFFull << ((unsigned) off * 8u));
        }
        if (xr_swiss_group_match_empty(group))
            return NULL;
        stride += XR_SWISS_GROUP;
        pos = (pos + stride) & mask;
    }
}

// Inline Map read (string constant key, zero function calls)
#define XR_MAP_GET_STRING_FAST(map, key_str, result, found)                                        \
    do {                                                                                           \
        XrMapEntry *_e = xr_map_find_string_fast(map, key_str);                                    \
        if (_e) {                                                                                  \
            (result) = _e->value;                                                                  \
            (found) = true;                                                                        \
        } else {                                                                                   \
            (result) = xr_null();                                                                  \
            (found) = false;                                                                       \
        }                                                                                          \
    } while (0)

// Inline Map set (string constant key)
#define XR_MAP_SET_STRING_FAST(map, key_str, key_val, _val)                                        \
    do {                                                                                           \
        XrMapEntry *_e = xr_map_find_string_fast(map, key_str);                                    \
        if (_e) {                                                                                  \
            xr_rc_release_value(xr_current_coro_heap(), _e->value);                                \
            _e->value = (_val);                                                                    \
        } else {                                                                                   \
            xr_map_set(map, key_val, _val);                                                        \
        }                                                                                          \
    } while (0)

/* ========== Basic Operations ========== */

XR_FUNC XrMap *xr_map_new(struct XrCoroutine *coro);
XR_FUNC XrMap *xr_map_with_capacity(struct XrCoroutine *coro, uint32_t capacity_hint);
XR_FUNC uint32_t xr_map_purge_weak_target(XrMap *map, XrObjHeader *target,
                                          struct XrCoroHeap *owner_heap);

struct XrCoroHeap;
// Pre-size entries[]/indices[] for `count` entries, charging external-byte
// accounting to `gc` (used by deep-copy, which runs off the destination coro).
XR_FUNC bool xr_map_reserve_external(XrMap *map, uint32_t count, struct XrCoroHeap *gc);

XR_FUNC void xr_map_set(XrMap *map, XrValue key, XrValue value);
XR_FUNC XrValue xr_map_get(XrMap *map, XrValue key, bool *found);
XR_FUNC bool xr_map_has(XrMap *map, XrValue key);
XR_FUNC bool xr_map_delete(XrMap *map, XrValue key);
XR_FUNC void xr_map_clear(XrMap *map);

XR_FUNC uint32_t xr_map_size(XrMap *map);
XR_FUNC bool xr_map_is_empty(XrMap *map);
XR_FUNC bool xr_map_has_value(XrMap *map, XrValue value);

/* ========== Iteration Methods ========== */

XR_FUNC XrArray *xr_map_keys(struct XrCoroutine *coro, XrMap *map);
XR_FUNC XrArray *xr_map_values(struct XrCoroutine *coro, XrMap *map);
XR_FUNC XrArray *xr_map_entries(struct XrCoroutine *coro, XrMap *map);

/* ========== Debug ========== */

XR_FUNC void xr_map_debug_print(XrMap *map);

#endif  // XMAP_H
