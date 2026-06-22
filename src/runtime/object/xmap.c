/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmap.c - Compact ordered hash map implementation (CPython-style compact dict)
 *
 * KEY CONCEPT:
 *   - entries[] is a dense, append-only array in insertion order; iteration
 *     scans it directly so observable order is insertion order.
 *   - ctrl[] is a Swiss-style h2 control-byte table; indices[] maps FULL ctrl
 *     slots to entries[] indices.
 *   - Deletion tombstones the entry (key_tt=0) and marks its ctrl byte DELETED;
 *     dead slots are reclaimed when the table is resized/compacted.
 *   - Resizing is triggered when nentries reaches entries_cap. Because
 *     entries_cap = indices_size*2/3 and every append consumes one index slot,
 *     the index table always keeps an EMPTY slot, so probing always terminates.
 *   - Empty map allocates nothing (DUMMY); the first insert allocates both
 *     arrays. Coroutine-heap maps allocate on the Region GC heap on first
 *     allocation; resizes use malloc (region recycling may overlap old blobs).
 */

#include "xmap.h"
#include "xstring.h"
#include "../gc/xalloc_unified.h"
#include "../gc/xweak_registry.h"
#include "../../base/xchecks.h"
#include "../value/xvalue_hash.h"
#include "../../base/xmalloc.h"
#include "xarray.h"
#include "xtuple.h"
#include "../class/xclass_system.h"
#include "../class/xclass.h"
#include "../gc/xgc_internal.h"
#include "../gc/xcoro_heap.h"
#include "../../coro/xcoroutine.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stddef.h>

/* ========== Memory Profiling (optional) ========== */
#include "../../base/xmem_profiler.h"
#include "../gc/xgc.h"

#ifdef XR_PROFILE_MAP_MEMORY
XrProfileStats g_map_header_stats;
XrProfileStats g_map_node_stats;
size_t g_map_new_count;
size_t g_map_rehash_count;
#endif

/* ========== Helper Functions ========== */

// Get key type tag (for fast comparison, +1 reserves 0 for empty)
static inline uint8_t get_key_tt(XrValue key) {
    return (uint8_t) (xr_value_typeid(key) + 1);
}

// Read hash from string (lazy: compute on first use for non-interned strings)
static inline uint32_t string_hash_fast(XrString *str) {
    if (str->hash == 0) {
        uint32_t h = xr_string_hash(str->data, str->length);
        str->hash = (h == 0) ? 1 : h;
    }
    return str->hash;
}

// Compute a key's hash (mirrors the original mainposition hashing).
static uint32_t hash_value(XrValue key) {
    if (XR_LIKELY(XR_IS_STRING(key))) {
        return string_hash_fast((XrString *) key.ptr);
    } else if (XR_IS_INT(key)) {
        return xr_hash_int(XR_TO_INT(key));
    } else if (XR_IS_FLOAT(key)) {
        return xr_hash_float(XR_TO_FLOAT(key));
    } else if (XR_IS_BOOL(key)) {
        return XR_TO_BOOL(key) ? 1u : 0u;
    }
    return (uint32_t) (uintptr_t) XR_TO_PTR(key);
}

// Compare an entry's key against (key, key_tt). Short strings compare by data.
// Returns int (not bool) to match the shared XrMapEqFn comparator signature.
static inline int entry_key_equal(const XrMapEntry *e, XrValue key, uint8_t key_tt) {
    if (e->key_tt != key_tt)
        return false;

    XrTypeId tid = (XrTypeId) (key_tt - 1);
    if (tid == XR_TID_STRING) {
        if (xr_value_same(e->key, key))
            return true;
        const char *d1 = xr_value_str_data(&e->key);
        uint32_t l1 = xr_value_str_len(&e->key);
        const char *d2 = xr_value_str_data(&key);
        uint32_t l2 = xr_value_str_len(&key);
        if (l1 != l2)
            return false;
        return memcmp(d1, d2, l1) == 0;
    }
    if (XR_TID_IS_INT(tid))
        return XR_TO_INT(e->key) == XR_TO_INT(key);
    if (XR_TID_IS_FLOAT(tid))
        return XR_TO_FLOAT(e->key) == XR_TO_FLOAT(key);
    if (tid == XR_TID_BOOL)
        return XR_TO_BOOL(e->key) == XR_TO_BOOL(key);
    if (tid == XR_TID_NULL)
        return true;
    return XR_TO_PTR(e->key) == XR_TO_PTR(key);
}

// Smallest power-of-two index size whose usable capacity (2/3) covers `needed`.
static uint32_t calc_indices_size(uint32_t needed) {
    uint32_t size = 8;
    while ((uint64_t) size * 2 / 3 < needed) {
        if (size >= (1u << XR_MAP_MAXHBITS)) {
            size = 1u << XR_MAP_MAXHBITS;
            break;
        }
        size <<= 1;
    }
    return size;
}

static inline bool map_is_weak(const XrMap *map) {
    return (map->flags & XR_MAP_FLAG_WEAK) != 0;
}

static inline XrCoroHeap *map_current_or_owner_heap(XrMap *map) {
    XrCoroHeap *heap = xr_current_coro_heap();
    return heap ? heap : (map ? map->owner_heap : NULL);
}

static XrayIsolate *map_owning_isolate(XrCoroHeap *heap) {
    if (heap && heap->owner)
        return heap->owner->isolate;
    return NULL;
}

static void xr_map_release_entry_values(XrMap *map, XrMapEntry *e, XrCoroHeap *heap) {
    if (!map_is_weak(map))
        xr_rc_release_value(heap, e->key);
    xr_rc_release_value(heap, e->value);
    e->key = xr_null();
    e->value = xr_null();
}

static void xr_map_prepare_weak_key(XrMap *map, XrValue key, XrCoroHeap *heap) {
    if (!map_is_weak(map) || !XR_IS_PTR(key))
        return;
    XrObjHeader *target = XR_VALUE_GCPTR(key);
    XR_OBJ_SET_FLAG(target, XR_OBJ_WEAKABLE);
    xr_weak_registry_register_map(map_owning_isolate(heap), map);
}

/* ========== Swiss Index Lookup ========== */

// Returns the ctrl/indices slot for `key`, or UINT32_MAX if absent. The
// per-type key comparison is supplied via entry_key_equal (a constant function,
// so -O2 inlines the shared probe and devirtualizes the call).
static uint32_t map_lookup_slot(XrMap *map, XrValue key, uint32_t hash, uint8_t key_tt,
                                int32_t *out_eidx) {
    return xr_map_lookup_slot(map->ctrl, map->indices, map->entries, map->indices_size, key, hash,
                              key_tt, entry_key_equal, out_eidx);
}

// Returns entries[] index for `key`, or -1 if absent.
static int32_t map_lookup(XrMap *map, XrValue key, uint32_t hash, uint8_t key_tt) {
    int32_t eidx = -1;
    return map_lookup_slot(map, key, hash, key_tt, &eidx) == UINT32_MAX ? -1 : eidx;
}

/* ========== Grow / Compact ========== */

// Grow (and compact away tombstones) to hold at least `min_needed` live entries.
// Handles the dummy -> first-allocation case too. Returns false on OOM.
static bool map_resize(XrMap *map, uint32_t min_needed) {
    XrCoroHeap *heap = map_current_or_owner_heap(map);
    bool was_dummy = xr_map_isdummy(map);
    XrMapEntry *old_entries = was_dummy ? NULL : map->entries;
    uint8_t *old_ctrl = was_dummy ? NULL : map->ctrl;
    int32_t *old_indices = was_dummy ? NULL : map->indices;
    uint32_t old_nentries = was_dummy ? 0 : map->nentries;
    uint32_t old_isize = was_dummy ? 0 : map->indices_size;
    uint32_t old_ecap = was_dummy ? 0 : map->entries_cap;
    bool old_on_heap = (map->flags & XR_MAP_FLAG_NODES_ON_GC) != 0;

    uint32_t needed = map->count > min_needed ? map->count : min_needed;
    if (needed < 1)
        needed = 1;
    uint32_t new_isize = calc_indices_size(needed);
    uint32_t new_ecap = (uint32_t) ((uint64_t) new_isize * 2 / 3);
    if (new_ecap < needed)
        new_ecap = needed;

    // First allocation from dummy may use the Region GC blob heap; subsequent
    // resizes force malloc (region recycling could overlap the old blob while
    // we copy live entries out of it).
    bool new_on_heap = was_dummy && old_on_heap;

    size_t cbytes = (size_t) new_isize + XR_SWISS_GROUP;
    size_t ibytes = sizeof(int32_t) * (size_t) new_isize;
    size_t ebytes = sizeof(XrMapEntry) * (size_t) new_ecap;
    uint8_t *new_ctrl = NULL;
    int32_t *new_indices = NULL;
    XrMapEntry *new_entries = NULL;

    if (new_on_heap && heap) {
        new_ctrl = (uint8_t *) xr_coro_alloc_blob(heap, cbytes);
        new_indices = (int32_t *) xr_coro_alloc_blob(heap, ibytes);
        new_entries = (XrMapEntry *) xr_coro_alloc_blob(heap, ebytes);
        if (!new_ctrl || !new_indices || !new_entries) {
            xr_coro_free_blob(heap, new_ctrl);
            xr_coro_free_blob(heap, new_indices);
            xr_coro_free_blob(heap, new_entries);
            new_on_heap = false;
            new_ctrl = (uint8_t *) xr_malloc(cbytes);
            new_indices = (int32_t *) xr_malloc(ibytes);
            new_entries = (XrMapEntry *) xr_malloc(ebytes);
        }
    } else {
        new_on_heap = false;
        new_ctrl = (uint8_t *) xr_malloc(cbytes);
        new_indices = (int32_t *) xr_malloc(ibytes);
        new_entries = (XrMapEntry *) xr_malloc(ebytes);
    }
    if (!new_ctrl || !new_indices || !new_entries) {
        if (!new_on_heap) {
            xr_free(new_ctrl);
            xr_free(new_indices);
            xr_free(new_entries);
        }
        return false;
    }
    if (!new_on_heap)
        xr_coro_heap_add_external(heap, (int64_t) (cbytes + ibytes + ebytes));

    memset(new_ctrl, (int) XR_SWISS_CTRL_EMPTY, cbytes);
    for (uint32_t i = 0; i < new_isize; i++)
        new_indices[i] = XR_MAP_IX_EMPTY;
    memset(new_entries, 0, ebytes);

    // Compactly copy live entries (preserving insertion order), rebuild indices.
    uint32_t w = xr_map_rehash_into(new_entries, new_ctrl, new_indices, new_isize, old_entries,
                                    old_nentries);

    map->ctrl = new_ctrl;
    map->indices = new_indices;
    map->entries = new_entries;
    map->indices_size = new_isize;
    map->entries_cap = new_ecap;
    map->nentries = w;
    map->flags &= ~XR_MAP_FLAG_DUMMY;
    if (new_on_heap)
        map->flags |= XR_MAP_FLAG_NODES_ON_GC;
    else
        map->flags &= ~XR_MAP_FLAG_NODES_ON_GC;

    if (old_entries) {
        if (old_on_heap) {
            xr_coro_free_blob(heap, old_ctrl);
            xr_coro_free_blob(heap, old_indices);
            xr_coro_free_blob(heap, old_entries);
        } else {
            xr_free(old_ctrl);
            xr_free(old_indices);
            xr_free(old_entries);
            xr_coro_heap_sub_external(heap, (int64_t) ((size_t) old_isize + XR_SWISS_GROUP +
                                                       sizeof(int32_t) * (size_t) old_isize +
                                                       sizeof(XrMapEntry) * (size_t) old_ecap));
        }
    }
    return true;
}

// Pre-allocate (malloc-backed) ctrl[]/indices[]/entries[] sized for `count` live
// entries, charging the external-byte accounting to `heap` rather than the
// current coroutine. Deep-copy runs off the destination coroutine and must
// charge the destination's heap (which later frees the map), so this avoids the
// byte-counter underflow that a current-coro accounting would cause. The map
// must be freshly dummy. After this, inserting up to `count` entries via
// xr_map_set will not trigger a resize.
bool xr_map_reserve_external(XrMap *map, uint32_t count, struct XrCoroHeap *heap) {
    if (count == 0)
        return true;  // Stays dummy; first insert will allocate.

    uint32_t isize = calc_indices_size(count);
    uint32_t ecap = (uint32_t) ((uint64_t) isize * 2 / 3);
    if (ecap < count)
        ecap = count;

    size_t cbytes = (size_t) isize + XR_SWISS_GROUP;
    size_t ibytes = sizeof(int32_t) * (size_t) isize;
    size_t ebytes = sizeof(XrMapEntry) * (size_t) ecap;
    uint8_t *ctrl = (uint8_t *) xr_malloc(cbytes);
    int32_t *idx = (int32_t *) xr_malloc(ibytes);
    XrMapEntry *ent = (XrMapEntry *) xr_malloc(ebytes);
    if (!ctrl || !idx || !ent) {
        xr_free(ctrl);
        xr_free(idx);
        xr_free(ent);
        return false;
    }
    if (heap)
        xr_coro_heap_add_external(heap, (int64_t) (cbytes + ibytes + ebytes));

    memset(ctrl, (int) XR_SWISS_CTRL_EMPTY, cbytes);
    for (uint32_t i = 0; i < isize; i++)
        idx[i] = XR_MAP_IX_EMPTY;
    memset(ent, 0, ebytes);

    map->ctrl = ctrl;
    map->indices = idx;
    map->entries = ent;
    map->indices_size = isize;
    map->entries_cap = ecap;
    map->nentries = 0;
    map->flags &= ~XR_MAP_FLAG_DUMMY;
    map->flags &= ~XR_MAP_FLAG_NODES_ON_GC;  // malloc-backed
    map->owner_heap = heap;
    return true;
}

/* ========== Create and Destroy ========== */

XrMap *xr_map_new(struct XrCoroutine *coro) {
    XR_DCHECK(coro != NULL, "map_new: NULL coro");
    XrMap *map = (XrMap *) xr_alloc(coro, sizeof(XrMap), XR_TMAP);
    if (!map)
        return NULL;

    xr_obj_header_init_type(&map->hdr, XR_TMAP);
    map->owner_heap = xr_coro_get_heap(coro);

    map->count = 0;
    map->nentries = 0;
    map->entries_cap = 0;
    map->indices_size = 0;
    map->ctrl = NULL;
    map->indices = NULL;
    map->entries = NULL;
    map->flags = XR_MAP_FLAG_DUMMY | XR_MAP_FLAG_NODES_ON_GC;
    map->key_tid = 0;
    map->value_tid = 0;

    XR_MAP_PROFILE_COUNT_NEW();
    XR_MAP_PROFILE_ALLOC_HEADER(sizeof(XrMap));

    return map;
}

XrMap *xr_map_with_capacity(struct XrCoroutine *coro, uint32_t capacity_hint) {
    XrMap *map = xr_map_new(coro);
    if (!map)
        return NULL;
    if (capacity_hint > 0)
        map_resize(map, capacity_hint);
    return map;
}

// Initialize Map in-place on pre-allocated memory (for shared Map)
void xr_map_init_inplace(XrMap *map, uint32_t capacity_hint) {
    if (!map)
        return;
    XR_MAP_PROFILE_COUNT_NEW();

    map->count = 0;
    map->nentries = 0;
    map->entries_cap = 0;
    map->indices_size = 0;
    map->ctrl = NULL;
    map->indices = NULL;
    map->entries = NULL;
    map->owner_heap = NULL;
    map->flags = XR_MAP_FLAG_DUMMY;  // system-heap: arrays via malloc
    map->key_tid = 0;
    map->value_tid = 0;

    if (capacity_hint > 0)
        map_resize(map, capacity_hint);
}

/* ========== Basic Operations ========== */

void xr_map_set(XrMap *map, XrValue key, XrValue value) {
    XR_DCHECK(map != NULL, "map_set: NULL map");
    XR_DCHECK(XR_OBJ_GET_TYPE(&map->hdr) == XR_TMAP, "map_set: object is not a map");
    XR_DCHECK(!XR_IS_NULL(key), "map_set: NULL key");

    uint8_t key_tt = get_key_tt(key);
    uint32_t hash = hash_value(key);
    XrCoroHeap *heap = map_current_or_owner_heap(map);

    int32_t ix = map_lookup(map, key, hash, key_tt);
    if (ix >= 0) {
        // Update existing: keep the stored key, drop the incoming key + old value.
        XrMapEntry *e = &map->entries[ix];
        xr_rc_release_value(heap, key);
        xr_rc_release_value(heap, e->value);
        e->value = value;
        XR_GC_BARRIER_BACK_SAFE(heap, map);
        return;
    }

    // New key: ensure entries[] has room (also compacts away tombstones).
    if (map->nentries >= map->entries_cap) {
        if (!map_resize(map, map->count + 1))
            return;  // OOM: drop silently as before (no partial state)
    }

    uint32_t eidx = map->nentries;
    XrMapEntry *e = &map->entries[eidx];
    e->key = key;
    e->value = value;
    e->hash = hash;
    e->key_tt = key_tt;
    map->nentries++;
    map->count++;

    xr_swiss_indices_put(map->ctrl, map->indices, map->indices_size, hash, (int32_t) eidx);
    if (map_is_weak(map)) {
        xr_map_prepare_weak_key(map, key, heap);
        xr_rc_release_value(heap, key);
    }
    XR_GC_BARRIER_BACK_SAFE(heap, map);
}

XrValue xr_map_get(XrMap *map, XrValue key, bool *found) {
    XR_DCHECK(map != NULL, "map_get: NULL map");
    XR_DCHECK(XR_OBJ_GET_TYPE(&map->hdr) == XR_TMAP, "map_get: object is not a map");

    uint8_t key_tt = get_key_tt(key);
    uint32_t hash = hash_value(key);
    int32_t ix = map_lookup(map, key, hash, key_tt);
    if (ix >= 0) {
        if (found)
            *found = true;
        return map->entries[ix].value;
    }
    if (found)
        *found = false;
    return xr_null();
}

bool xr_map_has(XrMap *map, XrValue key) {
    XR_DCHECK(map != NULL, "map_has: NULL map");
    XR_DCHECK(XR_OBJ_GET_TYPE(&map->hdr) == XR_TMAP, "map_has: object is not a map");
    uint8_t key_tt = get_key_tt(key);
    uint32_t hash = hash_value(key);
    return map_lookup(map, key, hash, key_tt) >= 0;
}

bool xr_map_delete(XrMap *map, XrValue key) {
    XR_DCHECK(map != NULL, "map_delete: NULL map");
    XR_DCHECK(XR_OBJ_GET_TYPE(&map->hdr) == XR_TMAP, "map_delete: object is not a map");
    if (xr_map_isdummy(map))
        return false;

    uint8_t key_tt = get_key_tt(key);
    uint32_t hash = hash_value(key);
    int32_t ix = -1;
    uint32_t slot = map_lookup_slot(map, key, hash, key_tt, &ix);
    if (slot == UINT32_MAX)
        return false;

    // Tombstone the entry (keeps its slot so order is preserved) and mark the
    // ctrl slot DELETED so probing skips past it.
    XrMapEntry *e = &map->entries[ix];
    xr_map_release_entry_values(map, e, map_current_or_owner_heap(map));
    e->key_tt = XR_MAP_ENTRY_NIL_KEY;
    map->indices[slot] = XR_MAP_IX_EMPTY;
    xr_swiss_ctrl_set(map->ctrl, map->indices_size, slot, XR_SWISS_CTRL_DELETED);
    map->count--;
    return true;
}

void xr_map_clear(XrMap *map) {
    XR_DCHECK(map != NULL, "map_clear: NULL map");
    if (xr_map_isdummy(map))
        return;

    XrCoroHeap *heap = map_current_or_owner_heap(map);
    for (uint32_t i = 0; i < map->nentries; i++) {
        XrMapEntry *e = &map->entries[i];
        if (e->key_tt != XR_MAP_ENTRY_NIL_KEY) {
            xr_map_release_entry_values(map, e, heap);
            e->key_tt = XR_MAP_ENTRY_NIL_KEY;
        }
    }
    memset(map->ctrl, (int) XR_SWISS_CTRL_EMPTY, (size_t) map->indices_size + XR_SWISS_GROUP);
    for (uint32_t i = 0; i < map->indices_size; i++)
        map->indices[i] = XR_MAP_IX_EMPTY;
    map->nentries = 0;
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

/* ========== Iteration (insertion order) ========== */

XrArray *xr_map_keys(struct XrCoroutine *coro, XrMap *map) {
    XR_DCHECK(coro != NULL, "map_keys: NULL coro");
    XR_DCHECK(map != NULL, "map_keys: NULL map");
    XrArray *arr = xr_array_with_capacity(coro, map->count);

    if (!xr_map_isdummy(map)) {
        for (uint32_t i = 0; i < map->nentries; i++) {
            XrMapEntry *e = &map->entries[i];
            if (e->key_tt != XR_MAP_ENTRY_NIL_KEY) {
                xr_rc_retain_value(e->key);
                xr_array_push(arr, e->key);
            }
        }
    }
    return arr;
}

XrArray *xr_map_values(struct XrCoroutine *coro, XrMap *map) {
    XR_DCHECK(coro != NULL, "map_values: NULL coro");
    XR_DCHECK(map != NULL, "map_values: NULL map");
    XrArray *arr = xr_array_with_capacity(coro, map->count);

    if (!xr_map_isdummy(map)) {
        for (uint32_t i = 0; i < map->nentries; i++) {
            XrMapEntry *e = &map->entries[i];
            if (e->key_tt != XR_MAP_ENTRY_NIL_KEY) {
                xr_rc_retain_value(e->value);
                xr_array_push(arr, e->value);
            }
        }
    }
    return arr;
}

XrArray *xr_map_entries(struct XrCoroutine *coro, XrMap *map) {
    XR_DCHECK(coro != NULL, "map_entries: NULL coro");
    XR_DCHECK(map != NULL, "map_entries: NULL map");
    XrArray *arr = xr_array_with_capacity(coro, map->count);

    if (!xr_map_isdummy(map)) {
        for (uint32_t i = 0; i < map->nentries; i++) {
            XrMapEntry *e = &map->entries[i];
            if (e->key_tt == XR_MAP_ENTRY_NIL_KEY)
                continue;
            /* Each entry is a (key, value) tuple — heterogeneous arity-2
             * product, exactly what destructuring `for ((k, v) in m.entries())`
             * expects. */
            XrTuple *pair = xr_tuple_new(coro, 2);
            if (pair) {
                xr_rc_retain_value(e->key);
                xr_rc_retain_value(e->value);
                xr_tuple_set(pair, 0, e->key);
                xr_tuple_set(pair, 1, e->value);
            }
            xr_array_push(arr, xr_value_from_tuple(pair));
        }
    }
    return arr;
}

/* ========== Other Operations ========== */

bool xr_map_has_value(XrMap *map, XrValue value) {
    if (xr_map_isdummy(map))
        return false;

    for (uint32_t i = 0; i < map->nentries; i++) {
        XrMapEntry *e = &map->entries[i];
        if (e->key_tt != XR_MAP_ENTRY_NIL_KEY) {
            if (xr_value_eq(e->value, value))
                return true;
        }
    }
    return false;
}

/* ========== Debug ========== */

void xr_map_debug_print(XrMap *map) {
    printf("Map[count=%u, nentries=%u, indices_size=%u, isdummy=%d]\n", map->count, map->nentries,
           map->indices_size, xr_map_isdummy(map));

    if (!xr_map_isdummy(map)) {
        for (uint32_t i = 0; i < map->nentries; i++) {
            XrMapEntry *e = &map->entries[i];
            if (e->key_tt != XR_MAP_ENTRY_NIL_KEY) {
                printf("  [%u] key_tt=%u hash=%u\n", i, e->key_tt, e->hash);
            }
        }
    }
}

/* ========== GC Integration ========== */

void xr_obj_destroy_map(XrObjHeader *obj, struct XrCoroHeap *owner_heap) {
    XrMap *map = (XrMap *) obj;
    if (map->flags & XR_MAP_FLAG_WEAK_REGISTERED)
        xr_weak_registry_unregister_map(map_owning_isolate(owner_heap), map);
    if (!xr_map_isdummy(map) && map->entries) {
        for (uint32_t i = 0; i < map->nentries; i++) {
            XrMapEntry *e = &map->entries[i];
            if (e->key_tt != XR_MAP_ENTRY_NIL_KEY)
                xr_map_release_entry_values(map, e, owner_heap);
        }
        size_t bytes = (size_t) map->indices_size + XR_SWISS_GROUP +
                       sizeof(int32_t) * (size_t) map->indices_size +
                       sizeof(XrMapEntry) * (size_t) map->entries_cap;
        if (map->flags & XR_MAP_FLAG_NODES_ON_GC) {
            xr_coro_free_blob(owner_heap, map->ctrl);
            xr_coro_free_blob(owner_heap, map->indices);
            xr_coro_free_blob(owner_heap, map->entries);
        } else {
            xr_free(map->ctrl);
            xr_free(map->indices);
            xr_free(map->entries);
            xr_coro_heap_sub_external(owner_heap, (int64_t) bytes);
        }
        map->ctrl = NULL;
        map->indices = NULL;
        map->entries = NULL;
    }
    XR_MAP_PROFILE_FREE_HEADER(sizeof(XrMap));
}
