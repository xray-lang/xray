/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmap.h - Chained hash map
 *
 * KEY CONCEPT:
 *   - Chained scatter table with Brent's variation
 *   - Short strings forced intern, pointer comparison
 *   - Empty map uses static dummynode (zero allocation)
 *   - Can reach 100% load factor
 *   - No tombstone mechanism needed
 */

#ifndef XMAP_H
#define XMAP_H

#include "../value/xvalue.h"
#include "xstring.h"
#include "../gc/xgc_header.h"
#include "../gc/xgc_internal.h"
#include "xarray.h"
#include <stdint.h>
#include <stdbool.h>

/* ========== Map Node (Chained Hash) ========== */

typedef struct XrMapNode {
    XrValue value;
    XrValue key;
    int32_t next;         // Chain offset (0 = end)
    uint8_t key_tt;       // Key type tag (0 = empty)
    uint8_t _pad[3];
} XrMapNode;

// Node state
#define XR_MAP_NODE_NIL_KEY  0
#define XR_MAP_NODE_EMPTY(n)  ((n)->key_tt == XR_MAP_NODE_NIL_KEY)

/* ========== Map Object ========== */

typedef struct XrMap {
    XrGCHeader gc;
    uint32_t count;
    uint8_t lsizenode;     // log2(node count), 0 = dummynode
    uint8_t flags;
    uint8_t key_tid;       // XrTypeId: key type for reified generics (0=any)
    uint8_t value_tid;     // XrTypeId: value type for reified generics (0=any)
    XrMapNode *node;
    XrMapNode *lastfree;
} XrMap;

// Macros
#define xr_map_sizenode(m)  (1u << (m)->lsizenode)
#define xr_map_node(m, i)   (&(m)->node[i])

// Flags
#define XR_MAP_FLAG_WEAK          0x01
#define XR_MAP_FLAG_DUMMY         0x02
#define XR_MAP_FLAG_NODES_ON_GC   0x04  // node[] allocated as GC blob on Immix heap

#define xr_map_isdummy(m)  ((m)->flags & XR_MAP_FLAG_DUMMY)

// Static dummynode
XR_DATA XrMapNode xr_map_dummynode;

// Initialize map in-place
XR_FUNC void xr_map_init_inplace(XrMap *map, uint32_t capacity_hint);

// Max hash size
#define XR_MAP_MAXHBITS  30

/* ========== Inline Fast Path (VM optimization) ========== */

// Inline string key lookup (zero function calls)
static inline XrMapNode* xr_map_find_string_fast(XrMap *map, XrString *key_str) {
    if (map->flags & XR_MAP_FLAG_DUMMY) return NULL;
    
    XrMapNode *n = &map->node[key_str->hash & ((1u << map->lsizenode) - 1)];
    
    for (;;) {
        if (n->key_tt == (XR_TID_STRING + 1) && XR_TO_STRING(n->key) == key_str) {
            return n;
        }
        int next = n->next;
        if (next == 0) return NULL;
        n += next;
    }
}

// Inline Map read (string constant key, zero function calls)
#define XR_MAP_GET_STRING_FAST(map, key_str, result, found) \
    do { \
        XrMapNode *_n = xr_map_find_string_fast(map, key_str); \
        if (_n) { \
            (result) = _n->value; \
            (found) = true; \
        } else { \
            (result) = xr_null(); \
            (found) = false; \
        } \
    } while(0)

// Inline Map set (string constant key)
#define XR_MAP_SET_STRING_FAST(map, key_str, key_val, _val) \
    do { \
        XrMapNode *_n = xr_map_find_string_fast(map, key_str); \
        if (_n) { \
            _n->value = (_val); \
        } else { \
            xr_map_set(map, key_val, _val); \
        } \
    } while(0)

/* ========== Basic Operations ========== */

XR_FUNC XrMap* xr_map_new(struct XrCoroutine *coro);
XR_FUNC XrMap* xr_map_with_capacity(struct XrCoroutine *coro, uint32_t capacity_hint);

XR_FUNC void xr_map_set(XrMap *map, XrValue key, XrValue value);
XR_FUNC XrValue xr_map_get(XrMap *map, XrValue key, bool *found);
XR_FUNC bool xr_map_has(XrMap *map, XrValue key);
XR_FUNC bool xr_map_delete(XrMap *map, XrValue key);
XR_FUNC void xr_map_clear(XrMap *map);

XR_FUNC uint32_t xr_map_size(XrMap *map);
XR_FUNC bool xr_map_is_empty(XrMap *map);
XR_FUNC bool xr_map_has_value(XrMap *map, XrValue value);

/* ========== Iteration Methods ========== */

XR_FUNC XrArray* xr_map_keys(struct XrCoroutine *coro, XrMap *map);
XR_FUNC XrArray* xr_map_values(struct XrCoroutine *coro, XrMap *map);
XR_FUNC XrArray* xr_map_entries(struct XrCoroutine *coro, XrMap *map);

struct XrIterator;
XR_FUNC struct XrIterator* xr_map_entries_iterator(XrayIsolate *iso, XrMap *map);

struct XrClosure;
XR_FUNC void xr_map_foreach(XrayIsolate *iso, XrMap *map, struct XrClosure *callback);

/* ========== Debug ========== */

XR_FUNC void xr_map_debug_print(XrMap *map);

#endif // XMAP_H
