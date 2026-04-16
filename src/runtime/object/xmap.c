/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmap.c - Chained hash map implementation
 *
 * KEY CONCEPT:
 *   - Chained scatter table with Brent's variation
 *   - No tombstone mechanism needed
 *   - Can reach 100% load factor
 *   - Empty map uses static dummynode
 *   - Short strings use pointer comparison
 *   - Coroutine-heap maps use GC blob for node[] (no malloc/free)
 *   - System-heap maps use malloc for node[] (freed by destructor)
 */

#include "xmap.h"
#include "xstring.h"
#include "../xisolate_api.h"
#include "../gc/xalloc_unified.h"
#include "../../base/xchecks.h"
#include "../value/xvalue_hash.h"
#include "../../base/xmalloc.h"
#include "xarray.h"
#include "xiterator.h"
#include "../class/xclass_system.h"
#include "../class/xclass.h"
#include "../gc/xgc_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stddef.h>

#include "../xvm_call.h"

/* ========== Static Dummynode ========== */

// Shared dummynode for empty maps (key_tt=0 means empty)
XrMapNode xr_map_dummynode = {
    .value = {0},
    .key = {0},
    .next = 0,
    .key_tt = XR_MAP_NODE_NIL_KEY,
    ._pad = {0}
};

/* ========== Memory Profiling (optional) ========== */
#include "../../base/xmem_profiler.h"

#ifdef XR_PROFILE_MAP_MEMORY
XrProfileStats g_map_header_stats;
XrProfileStats g_map_node_stats;
size_t g_map_new_count;
size_t g_map_rehash_count;
#endif // ========== GC Allocation Adapter ==========
#include "../gc/xgc.h"

/* ========== Helper Functions ========== */

// Calculate ceil(log2(x))
static uint8_t ceillog2(uint32_t x) {
    if (x <= 1) return 0;
    uint8_t r = 0;
    x--;
    while (x > 0) { x >>= 1; r++; }
    return r;
}

// Get key type tag (for fast comparison, +1 reserves 0 for empty)
static inline uint8_t get_key_tt(XrValue key) {
    return (uint8_t)(xr_value_typeid(key) + 1);
}

// Read hash from string (lazy: compute on first use for non-interned strings)
static inline uint32_t string_hash_fast(XrString *str) {
    if (str->hash == 0) {
        uint32_t h = xr_string_hash(str->data, str->length);
        str->hash = (h == 0) ? 1 : h;
    }
    return str->hash;
}

/* ========== Main Position Calculation ========== */

/**
 * Calculate key's main position (ideal slot in hash table)
 * 
 * This is the key's "ideal" position in the hash table
 * Using bitwise AND for index (capacity is power of 2)
 */
static XrMapNode* mainposition(XrMap *map, XrValue key) {
    uint32_t hash;
    
    // Compute hash based on key type
    if (XR_LIKELY(XR_IS_STRING(key))) {
        hash = string_hash_fast((XrString*)key.ptr);
    } else if (XR_IS_INT(key)) {
        hash = xr_hash_int(XR_TO_INT(key));
    } else if (XR_IS_FLOAT(key)) {
        hash = xr_hash_float(XR_TO_FLOAT(key));
    } else if (XR_IS_BOOL(key)) {
        hash = XR_TO_BOOL(key) ? 1 : 0;
    } else {
        // Other types: use pointer hash
        hash = (uint32_t)(uintptr_t)XR_TO_PTR(key);
    }
    
    // Use bitwise AND for index (capacity is power of 2)
    uint32_t size = xr_map_sizenode(map);
    return &map->node[hash & (size - 1)];
}

// Calculate main position from node
static XrMapNode* mainpositionfromnode(XrMap *map, XrMapNode *n) {
    return mainposition(map, n->key);
}

/* ========== Key Comparison ========== */

// Compare if node's key matches (short strings use pointer comparison)
static inline bool keyequal(XrMapNode *n, XrValue key, uint8_t key_tt) {
    // Different type, return false
    if (n->key_tt != key_tt) return false;
    
    // Compare by type (subtract 1 to restore XrTypeId)
    XrTypeId tid = (XrTypeId)(key_tt - 1);
    if (tid == XR_TID_STRING) {
        // Direct data comparison — works for SSO, heap, or mixed
        if (xr_value_same(n->key, key)) return true;
        const char *d1 = xr_value_str_data(&n->key);
        uint32_t l1 = xr_value_str_len(&n->key);
        const char *d2 = xr_value_str_data(&key);
        uint32_t l2 = xr_value_str_len(&key);
        if (l1 != l2) return false;
        return memcmp(d1, d2, l1) == 0;
    }
    if (XR_TID_IS_INT(tid))
        return XR_TO_INT(n->key) == XR_TO_INT(key);
    if (XR_TID_IS_FLOAT(tid))
        return XR_TO_FLOAT(n->key) == XR_TO_FLOAT(key);
    if (tid == XR_TID_BOOL)
        return XR_TO_BOOL(n->key) == XR_TO_BOOL(key);
    if (tid == XR_TID_NULL)
        return true;
    // Object types: pointer comparison
    return XR_TO_PTR(n->key) == XR_TO_PTR(key);
}

/* ========== Node Lookup ========== */

// Find node for key, returns NULL if not found
static XrMapNode* findnode(XrMap *map, XrValue key) {
    if (xr_map_isdummy(map)) return NULL;
    
    uint8_t key_tt = get_key_tt(key);
    XrMapNode *n = mainposition(map, key);
    
    // Traverse chain
    for (;;) {
        if (keyequal(n, key, key_tt)) {
            return n;  // Found
        }
        int next = n->next;
        if (next == 0) return NULL;  // End of chain, not found
        n += next;
    }
}

/* ========== Free Node Lookup ========== */

// Find free node from back to front
static XrMapNode* getfreepos(XrMap *map) {
    if (xr_map_isdummy(map)) return NULL;
    
    while (map->lastfree > map->node) {
        map->lastfree--;
        if (XR_MAP_NODE_EMPTY(map->lastfree)) {
            return map->lastfree;
        }
    }
    return NULL;  // No free node
}

/* ========== Node Array Allocation ========== */

// Allocate node array
static void setnodevector(XrMap *map, uint32_t size) {
    XR_DCHECK(map != NULL, "setnodevector: NULL map");
    if (size == 0) {
        // Use dummynode
        map->node = &xr_map_dummynode;
        map->lastfree = NULL;
        map->lsizenode = 0;
        map->flags |= XR_MAP_FLAG_DUMMY;
    } else {
        uint8_t lsize = ceillog2(size);
        if (lsize > XR_MAP_MAXHBITS) {
            lsize = XR_MAP_MAXHBITS;
        }
        size = 1u << lsize;
        size_t alloc_bytes = sizeof(XrMapNode) * size;
        
        XrMapNode *nodes = NULL;
        if (map->flags & XR_MAP_FLAG_NODES_ON_GC) {
            // GC blob path: no free needed, GC reclaims old blob
            XrCoroGC *gc = xr_current_coro_gc();
            if (gc) {
                nodes = (XrMapNode*)xr_coro_alloc_blob(gc, alloc_bytes);
            }
            if (!nodes) {
                // Fallback to malloc if no coro_gc
                nodes = (XrMapNode*)xr_malloc(alloc_bytes);
                map->flags &= ~XR_MAP_FLAG_NODES_ON_GC;
            }
        } else {
            nodes = (XrMapNode*)xr_malloc(alloc_bytes);
        }
        if (!nodes) return;
        XR_MAP_PROFILE_ALLOC_NODES(alloc_bytes);
        
        for (uint32_t i = 0; i < size; i++) {
            nodes[i].key_tt = XR_MAP_NODE_NIL_KEY;
            nodes[i].next = 0;
        }
        
        map->node = nodes;
        map->lastfree = &nodes[size];
        map->lsizenode = lsize;
        map->flags &= ~XR_MAP_FLAG_DUMMY;
    }
}

/* ========== Create and Destroy ========== */

XrMap* xr_map_new(struct XrCoroutine *coro) {
    XR_DCHECK(coro != NULL, "map_new: NULL coro");
    XrMap *map = (XrMap*)xr_alloc(coro, sizeof(XrMap), XR_TMAP);
    if (!map) return NULL;
    
    xr_gc_header_init_type(&map->gc, XR_TMAP);
    
    map->count = 0;
    map->lsizenode = 0;
    map->flags = XR_MAP_FLAG_DUMMY | XR_MAP_FLAG_NODES_ON_GC;
    map->key_tid = 0;
    map->value_tid = 0;
    map->node = &xr_map_dummynode;
    map->lastfree = NULL;
    
    XR_MAP_PROFILE_COUNT_NEW();
    XR_MAP_PROFILE_ALLOC_HEADER(sizeof(XrMap));
    
    return map;
}

XrMap* xr_map_with_capacity(struct XrCoroutine *coro, uint32_t capacity_hint) {
    XrMap *map = xr_map_new(coro);
    if (!map) return NULL;
    
    // Pre-allocate node array
    if (capacity_hint > 0) {
        setnodevector(map, capacity_hint);
    }
    
    return map;
}

// Initialize Map in-place on pre-allocated memory (for shared Map)
void xr_map_init_inplace(XrMap *map, uint32_t capacity_hint) {
    if (!map) return;
    XR_MAP_PROFILE_COUNT_NEW();
    
    // Initialize as empty map (using dummynode)
    map->count = 0;
    map->lsizenode = 0;
    map->flags = XR_MAP_FLAG_DUMMY;
    map->key_tid = 0;
    map->value_tid = 0;
    map->node = &xr_map_dummynode;
    map->lastfree = NULL;
    
    // Pre-allocate node array
    if (capacity_hint > 0) {
        setnodevector(map, capacity_hint);
    }
}

/* ========== Key Insertion (Brent's Variation) ========== */

// Insert new key using Brent's variation:
// 1. If main position is free, insert directly
// 2. If occupied, check if occupier is at its main position
//    - If not, move occupier, new key takes main position
//    - If yes, new key goes to free position and chains
static bool insertkey(XrMap *map, XrValue key, XrValue value) {
    XrMapNode *mp = mainposition(map, key);
    
    if (!XR_MAP_NODE_EMPTY(mp) || xr_map_isdummy(map)) {
        // Main position occupied
        XrMapNode *f = getfreepos(map);
        if (f == NULL) return false;  // No free position
        
        XrMapNode *othern = mainpositionfromnode(map, mp);
        if (othern != mp) {
            // Occupier not at its main position, move it
            while (othern + othern->next != mp) {
                othern += othern->next;
            }
            othern->next = (int32_t)(f - othern);  // Relink to f
            *f = *mp;  // Copy occupier node to f
            if (mp->next != 0) {
                f->next += (int32_t)(mp - f);  // Fix next offset
                mp->next = 0;
            }
            mp->key_tt = XR_MAP_NODE_NIL_KEY;  // Clear mp
        } else {
            // Occupier at its main position, new key goes to free slot
            if (mp->next != 0) {
                f->next = (int32_t)((mp + mp->next) - f);
            }
            mp->next = (int32_t)(f - mp);
            mp = f;
        }
    }
    
    // Write key-value
    mp->key = key;
    mp->key_tt = get_key_tt(key);
    mp->value = value;
    map->count++;
    
    return true;
}

/* ========== Rehash ========== */

static void rehash(XrMap *map) {
    XR_MAP_PROFILE_COUNT_REHASH();
    bool was_dummy = xr_map_isdummy(map);
    uint32_t oldsize = was_dummy ? 0 : xr_map_sizenode(map);
    XrMapNode *oldnodes = map->node;
    
    // Save old allocation mode before setnodevector may change it
    // (setnodevector clears XR_MAP_FLAG_NODES_ON_GC on malloc fallback)
    bool old_nodes_on_gc = (map->flags & XR_MAP_FLAG_NODES_ON_GC) != 0;
    
    // New size: at least 2 nodes, or double current
    uint32_t newsize = (oldsize == 0) ? 2 : oldsize * 2;
    XR_DCHECK((newsize & (newsize - 1)) == 0, "rehash: new size not power-of-2");
    
    // Allocate new array
    uint32_t saved_count = map->count;

    /* Force malloc for new nodes during rehash.
     * Immix bump allocator may return memory overlapping the old GC blob
     * (line recycling within the same block), and setnodevector's init
     * loop would zero out old entries before we can re-insert them. */
    uint16_t saved_gc_flag = map->flags & XR_MAP_FLAG_NODES_ON_GC;
    map->flags &= ~XR_MAP_FLAG_NODES_ON_GC;

    setnodevector(map, newsize);
    map->count = 0;
    
    // Re-insert all keys
    for (uint32_t i = 0; i < oldsize; i++) {
        XrMapNode *old = &oldnodes[i];
        if (!XR_MAP_NODE_EMPTY(old)) {
            bool ok = insertkey(map, old->key, old->value);
            XR_DCHECK(ok, "rehash insert failed");
            (void)ok;
        }
    }
    
    XR_DCHECK(map->count == saved_count, "rehash: count mismatch after re-insert");
    
    // Free old array (dummynode cannot be freed)
    if (!was_dummy && oldnodes) {
        if (!old_nodes_on_gc) {
            xr_free(oldnodes);
        }
        // GC blob: old nodes reclaimed automatically by Immix sweep
        XR_MAP_PROFILE_FREE_NODES(sizeof(XrMapNode) * oldsize);
    }
}

/* ========== Basic Operations ========== */

void xr_map_set(XrMap *map, XrValue key, XrValue value) {
    XR_DCHECK(map != NULL, "map_set: NULL map");
    XR_DCHECK(XR_GC_GET_TYPE(&map->gc) == XR_TMAP, "map_set: object is not a map");
    XR_DCHECK(!XR_IS_NULL(key), "map_set: NULL key");
    // Check if key exists
    XrMapNode *n = findnode(map, key);
    
    if (n != NULL) {
        // Update existing value
        n->value = value;
        XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), map);
        return;
    }
    
    // New key: try insert
    if (!insertkey(map, key, value)) {
        // Insert failed, need rehash
        rehash(map);
        bool ok = insertkey(map, key, value);
        XR_DCHECK(ok, "insert after rehash failed");
        (void)ok;
    }
    XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), map);
}

XrValue xr_map_get(XrMap *map, XrValue key, bool *found) {
    XR_DCHECK(map != NULL, "map_get: NULL map");
    XR_DCHECK(XR_GC_GET_TYPE(&map->gc) == XR_TMAP, "map_get: object is not a map");
    XrMapNode *n = findnode(map, key);
    
    if (n != NULL) {
        if (found) *found = true;
        return n->value;
    } else {
        if (found) *found = false;
        return xr_null();
    }
}

bool xr_map_has(XrMap *map, XrValue key) {
    XR_DCHECK(map != NULL, "map_has: NULL map");
    XR_DCHECK(XR_GC_GET_TYPE(&map->gc) == XR_TMAP, "map_has: object is not a map");
    return findnode(map, key) != NULL;
}

bool xr_map_delete(XrMap *map, XrValue key) {
    XR_DCHECK(map != NULL, "map_delete: NULL map");
    XR_DCHECK(XR_GC_GET_TYPE(&map->gc) == XR_TMAP, "map_delete: object is not a map");
    if (xr_map_isdummy(map)) return false;
    
    uint8_t key_tt = get_key_tt(key);
    XrMapNode *mp = mainposition(map, key);
    XrMapNode *prev = NULL;
    XrMapNode *n = mp;
    
    // Traverse chain
    for (;;) {
        if (keyequal(n, key, key_tt)) {
            // Found, delete
            if (prev == NULL) {
                // Is chain head
                if (n->next != 0) {
                    // Has successor, copy to current position
                    XrMapNode *next_node = n + n->next;
                    int32_t next_offset = n->next;
                    *n = *next_node;
                    if (n->next != 0) {
                        n->next += next_offset;  // Fix offset
                    }
                    next_node->key_tt = XR_MAP_NODE_NIL_KEY;
                } else {
                    // No successor, clear directly
                    n->key_tt = XR_MAP_NODE_NIL_KEY;
                }
            } else {
                // Not chain head, modify prev's next
                if (n->next != 0) {
                    prev->next += n->next;
                } else {
                    prev->next = 0;
                }
                n->key_tt = XR_MAP_NODE_NIL_KEY;
            }
            map->count--;
            XR_DCHECK(map->count <= xr_map_sizenode(map), "map_delete: count > capacity");
            return true;
        }
        
        if (n->next == 0) return false;  // Not found
        prev = n;
        n += n->next;
    }
}

void xr_map_clear(XrMap *map) {
    XR_DCHECK(map != NULL, "map_clear: NULL map");
    if (xr_map_isdummy(map)) return;
    
    // Clear all nodes
    uint32_t size = xr_map_sizenode(map);
    for (uint32_t i = 0; i < size; i++) {
        map->node[i].key_tt = XR_MAP_NODE_NIL_KEY;
        map->node[i].next = 0;
    }
    map->lastfree = &map->node[size];
    map->count = 0;
}

uint32_t xr_map_size(XrMap *map) {
    XR_DCHECK(map != NULL, "map_size: NULL map");
    return map->count;
}

bool xr_map_is_empty(XrMap *map) {
    XR_DCHECK(map != NULL, "map_is_empty: NULL map");
    return map->count == 0;
}

/* ========== Iteration ========== */

XrArray* xr_map_keys(struct XrCoroutine *coro, XrMap *map) {
    XR_DCHECK(coro != NULL, "map_keys: NULL coro");
    XR_DCHECK(map != NULL, "map_keys: NULL map");
    XrArray *arr = xr_array_with_capacity(coro, map->count);
    
    if (!xr_map_isdummy(map)) {
        uint32_t size = xr_map_sizenode(map);
        for (uint32_t i = 0; i < size; i++) {
            XrMapNode *n = &map->node[i];
            if (!XR_MAP_NODE_EMPTY(n)) {
                xr_array_push(arr, n->key);
            }
        }
    }
    return arr;
}

XrArray* xr_map_values(struct XrCoroutine *coro, XrMap *map) {
    XR_DCHECK(coro != NULL, "map_values: NULL coro");
    XR_DCHECK(map != NULL, "map_values: NULL map");
    XrArray *arr = xr_array_with_capacity(coro, map->count);
    
    if (!xr_map_isdummy(map)) {
        uint32_t size = xr_map_sizenode(map);
        for (uint32_t i = 0; i < size; i++) {
            XrMapNode *n = &map->node[i];
            if (!XR_MAP_NODE_EMPTY(n)) {
                xr_array_push(arr, n->value);
            }
        }
    }
    return arr;
}

XrArray* xr_map_entries(struct XrCoroutine *coro, XrMap *map) {
    XR_DCHECK(coro != NULL, "map_entries: NULL coro");
    XR_DCHECK(map != NULL, "map_entries: NULL map");
    XrArray *arr = xr_array_with_capacity(coro, map->count);
    
    if (!xr_map_isdummy(map)) {
        uint32_t size = xr_map_sizenode(map);
        for (uint32_t i = 0; i < size; i++) {
            XrMapNode *n = &map->node[i];
            if (!XR_MAP_NODE_EMPTY(n)) {
                XrArray *pair = xr_array_with_capacity(coro, 2);
                xr_array_push(pair, n->key);
                xr_array_push(pair, n->value);
                xr_array_push(arr, xr_value_from_array(pair));
            }
        }
    }
    return arr;
}

/* ========== Other Operations ========== */

bool xr_map_has_value(XrMap *map, XrValue value) {
    if (xr_map_isdummy(map)) return false;
    
    uint32_t size = xr_map_sizenode(map);
    for (uint32_t i = 0; i < size; i++) {
        XrMapNode *n = &map->node[i];
        if (!XR_MAP_NODE_EMPTY(n)) {
            if (xr_value_eq(n->value, value)) return true;
        }
    }
    return false;
}

void xr_map_foreach(XrayIsolate *isolate, XrMap *map, struct XrClosure *callback) {
    if (xr_map_isdummy(map)) return;
    
    uint32_t size = xr_map_sizenode(map);
    XrValue args[3];
    
    for (uint32_t i = 0; i < size; i++) {
        XrMapNode *n = &map->node[i];
        if (!XR_MAP_NODE_EMPTY(n)) {
            args[0] = n->value;
            args[1] = n->key;
            args[2] = xr_value_from_map(map);
            xr_vm_call_closure(isolate, callback, args, 3);
        }
    }
}

XrIterator* xr_map_entries_iterator(XrayIsolate *iso, XrMap *map) {
    // Use lazy iterator, traverse Map directly
    return xr_iterator_new_from_map(xr_current_coro(iso), map);
}

/* ========== Debug ========== */

void xr_map_debug_print(XrMap *map) {
    printf("Map[count=%u, lsizenode=%u, isdummy=%d]\n", 
           map->count, map->lsizenode, xr_map_isdummy(map));
    
    if (!xr_map_isdummy(map)) {
        uint32_t size = xr_map_sizenode(map);
        for (uint32_t i = 0; i < size; i++) {
            XrMapNode *n = &map->node[i];
            if (!XR_MAP_NODE_EMPTY(n)) {
                printf("  [%u] key_tt=%u next=%d\n", i, n->key_tt, n->next);
            }
        }
    }
}

/* ========== GC Integration ========== */

void xr_gc_destroy_map(XrGCHeader *obj, struct XrCoroGC *owning_gc) {
    (void)owning_gc;
    XrMap *map = (XrMap*)obj;
    if (!xr_map_isdummy(map) && map->node) {
        XR_MAP_PROFILE_FREE_NODES(sizeof(XrMapNode) * xr_map_sizenode(map));
        if (map->flags & XR_MAP_FLAG_NODES_ON_GC) {
            // GC blob: no free needed, Immix sweep reclaims
            map->node = NULL;
        } else {
            xr_free(map->node);
            map->node = NULL;
        }
    }
    XR_MAP_PROFILE_FREE_HEADER(sizeof(XrMap));
}

