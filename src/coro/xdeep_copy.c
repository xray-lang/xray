/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdeep_copy.c - Deep copy implementation
 */

#include "xdeep_copy.h"
#include "../base/xchecks.h"
#include "../runtime/xshared.h"
#include "../runtime/value/xvalue.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/mem/xheap.h"
#include "../runtime/mem/xfixed_heap.h"
#include "../runtime/mem/xcoro_heap.h"
#include "../runtime/mem/xalloc_unified.h"
#include "../runtime/mem/xsystem_heap.h"
#include "../runtime/object/xarray.h"
#include "../runtime/object/xmap.h"
#include "../runtime/object/xstring.h"
#include "../runtime/object/xset.h"
#include "../runtime/object/xjson.h"
#include "../runtime/class/xinstance.h"
#include "../runtime/closure/xclosure.h"
#include "xcoroutine.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/closure/xcell.h"
#include "../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>
#include "../base/xhash.h"

const XrObjDeepCopyFn xr_obj_deep_copy_ops[XR_OBJ_TYPE_MAX] = {
    [XR_TSTRING] = xr_deep_copy_string_with_ctx,     [XR_TARRAY] = xr_deep_copy_array_with_ctx,
    [XR_TMAP] = xr_deep_copy_map_with_ctx,           [XR_TSET] = xr_deep_copy_set_with_ctx,
    [XR_TINSTANCE] = xr_deep_copy_instance_with_ctx, [XR_TFUNCTION] = xr_deep_copy_closure_with_ctx,
    [XR_TCELL] = xr_deep_copy_cell_with_ctx,         [XR_TBOOLMAP] = xr_deep_copy_map_with_ctx,
};

const XrObjToSharedFn xr_obj_to_shared_ops[XR_OBJ_TYPE_MAX] = {
    [XR_TARRAY] = xr_to_shared_array,      [XR_TMAP] = xr_to_shared_map,
    [XR_TSET] = xr_to_shared_set,          [XR_TINSTANCE] = xr_to_shared_instance,
    [XR_TFUNCTION] = xr_to_shared_closure,
};

// Initial bucket count. Seen hash dynamically grows when the
// live entry count crosses 75% load factor — avoids O(N) chain traversals
// on deep graphs (10K+ shared objects).
#define SEEN_BUCKET_INIT 32
#define SEEN_LOAD_NUM 3
#define SEEN_LOAD_DEN 4  // grow when count >= bucket_count * 3/4

static inline int seen_hash_n(void *ptr, int bucket_count) {
    return (int) (xr_hash_int((int) (uintptr_t) ptr) % (unsigned int) bucket_count);
}

static void copy_context_init_common(XrCopyContext *ctx, XrRuntimeCore *core,
                                     XrFixedHeap *dst_fixed_heap) {
    XR_DCHECK(ctx != NULL, "copy_context_init: NULL ctx");
    XR_DCHECK(core != NULL, "copy_context_init: NULL runtime core");
    ctx->core = core;
    ctx->dst_fixed_heap = dst_fixed_heap ? dst_fixed_heap : &core->fixed_heap;
    ctx->dst_heap = NULL;
    ctx->to_transit = false;
    ctx->buckets = NULL;
    ctx->bucket_count = 0;
    ctx->objects_copied = 0;
    ctx->arena_head = NULL;
}

void xr_copy_context_init_core(XrCopyContext *ctx, XrRuntimeCore *core,
                               XrFixedHeap *dst_fixed_heap) {
    copy_context_init_common(ctx, core, dst_fixed_heap);
}

void xr_copy_context_init(XrCopyContext *ctx, struct XrayIsolate *X,
                          struct XrFixedHeap *dst_fixed_heap) {
    XR_DCHECK(X != NULL, "copy_context_init: NULL isolate");
    copy_context_init_common(ctx, xr_isolate_get_runtime_core(X),
                             dst_fixed_heap ? dst_fixed_heap : xr_isolate_get_fixed_heap(X));
}

// Channel-transit allocation: coroutine-independent shared object with
// one atomic reference owned by the channel buffer. Freed wholesale via
// xr_shared_destroy when the receive-side copy releases that reference.
static void *copy_ctx_alloc_transit(XrCopyContext *ctx, size_t size, uint8_t type) {
    XrSystemHeap *heap = ctx->core ? ctx->core->sys_heap : NULL;
    if (!heap)
        return NULL;
    XrObjHeader *obj = (XrObjHeader *) xr_sysheap_alloc_shared(heap, size, type);
    if (!obj)
        return NULL;
    obj->objsize = (uint32_t) size;
    xr_shared_init(obj);
    XR_OBJ_SET_FLAG(obj, XR_OBJ_TRANSIT);
    return obj;
}

// Unified allocation: transit sysheap, Region heap, or fixed heap fallback
static inline void *copy_ctx_alloc(XrCopyContext *ctx, size_t size, uint8_t type) {
    if (ctx->to_transit) {
        return copy_ctx_alloc_transit(ctx, size, type);
    }
    if (ctx->dst_heap) {
        return xr_coro_heap_new_obj(ctx->dst_heap, type, size);
    }
    return xr_fixed_heap_alloc(ctx->dst_fixed_heap, size, type);
}

XrValue xr_deep_copy_string_with_ctx(XrCopyContext *ctx, XrObjHeader *obj) {
    if (!ctx || !obj)
        return XR_NULL_VAL;
    XrString *copy = xr_string_clone_shared_core(ctx->core, (XrString *) obj);
    return copy ? xr_string_value(copy) : XR_NULL_VAL;
}

void xr_copy_context_cleanup(XrCopyContext *ctx) {
    // Free arena blocks (bulk deallocation, no per-entry free)
    XrSeenArena *arena = ctx->arena_head;
    while (arena) {
        XrSeenArena *next = arena->next;
        xr_free(arena);
        arena = next;
    }
    ctx->arena_head = NULL;
    if (ctx->buckets) {
        xr_free(ctx->buckets);
        ctx->buckets = NULL;
        ctx->bucket_count = 0;
    }
}

static XrValue xr_copy_context_lookup(XrCopyContext *ctx, void *src) {
    if (!ctx->buckets || ctx->bucket_count == 0)
        return XR_NULL_VAL;
    int idx = seen_hash_n(src, ctx->bucket_count);
    for (XrSeenEntry *e = ctx->buckets[idx]; e; e = e->next) {
        if (e->src == src) {
            /* Transit graphs are reclaimed by cascading RC drops, so a
             * second parent referencing the same copy must own its own
             * reference. */
            if (ctx->to_transit && XR_IS_PTR(e->dst)) {
                XrObjHeader *dst_obj = XR_VALUE_GCPTR(e->dst);
                if (dst_obj)
                    xr_shared_retain(dst_obj);
            }
            return e->dst;
        }
    }
    return XR_NULL_VAL;
}

// Allocate XrSeenEntry from arena (one malloc per 64 entries)
static inline XrSeenEntry *seen_arena_alloc(XrCopyContext *ctx) {
    XrSeenArena *a = ctx->arena_head;
    if (!a || a->used >= XR_SEEN_ARENA_BLOCK_SIZE) {
        a = (XrSeenArena *) xr_malloc(sizeof(XrSeenArena));
        if (!a)
            return NULL;
        a->used = 0;
        a->next = ctx->arena_head;
        ctx->arena_head = a;
    }
    return &a->entries[a->used++];
}

// Grow seen-hash buckets to double capacity and rehash in place.
// Arena entries are kept intact (their next pointers are just rewired).
// Returns true on success; on failure the context keeps the old table.
static bool seen_hash_grow(XrCopyContext *ctx) {
    int new_count = ctx->bucket_count ? ctx->bucket_count * 2 : SEEN_BUCKET_INIT;
    XrSeenEntry **new_buckets =
        (XrSeenEntry **) xr_calloc((size_t) new_count, sizeof(XrSeenEntry *));
    if (!new_buckets)
        return false;

    // Walk existing arena blocks and rehash every entry. We iterate arena
    // (not old buckets) so we don't depend on the old bucket ordering; any
    // entry recorded so far sits in the arena blocks.
    for (XrSeenArena *a = ctx->arena_head; a; a = a->next) {
        for (int i = 0; i < a->used; i++) {
            XrSeenEntry *e = &a->entries[i];
            int idx = seen_hash_n(e->src, new_count);
            e->next = new_buckets[idx];
            new_buckets[idx] = e;
        }
    }

    if (ctx->buckets)
        xr_free(ctx->buckets);
    ctx->buckets = new_buckets;
    ctx->bucket_count = new_count;
    return true;
}

static void xr_copy_context_record(XrCopyContext *ctx, void *src, XrValue dst) {
    // Lazy init.
    if (!ctx->buckets) {
        ctx->buckets = (XrSeenEntry **) xr_calloc(SEEN_BUCKET_INIT, sizeof(XrSeenEntry *));
        if (!ctx->buckets)
            return;
        ctx->bucket_count = SEEN_BUCKET_INIT;
    }

    // Grow before insert if load factor would exceed 75%. objects_copied
    // is bumped by callers after each record() so it matches live entries.
    if (ctx->bucket_count > 0 &&
        ctx->objects_copied * SEEN_LOAD_DEN >= ctx->bucket_count * SEEN_LOAD_NUM) {
        (void) seen_hash_grow(ctx);  // failure is non-fatal: fall through.
    }

    XrSeenEntry *entry = seen_arena_alloc(ctx);
    if (!entry)
        return;
    entry->src = src;
    entry->dst = dst;
    int idx = seen_hash_n(src, ctx->bucket_count);
    entry->next = ctx->buckets[idx];
    ctx->buckets[idx] = entry;
}

typedef struct XrAotNativeMapView {
    XrObjHeader hdr;
    XR_MAP_ABI_FIELDS;
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
} XrAotNativeMapView;

typedef struct XrAotNativeSetView {
    XrObjHeader hdr;
    XR_SET_ABI_FIELDS;
    int64_t len;
    int64_t cap;
    int64_t growth_left;
    void *items;
    int64_t *order;
    int64_t order_len;
    int64_t order_cap;
    uint8_t elem_type;
    uint8_t elem_size;
} XrAotNativeSetView;

typedef union XrAotBoolMapSlot {
    int64_t i;
    double f;
} XrAotBoolMapSlot;

typedef struct XrAotBoolMapView {
    XrObjHeader hdr;
    uint8_t value_type;
    uint8_t present;
    uint8_t order[2];
    uint8_t order_len;
    XrAotBoolMapSlot v[2];
} XrAotBoolMapView;

static inline bool xr_aot_native_obj(const XrObjHeader *obj) {
    return obj && XR_OBJ_GET_FLAG(obj, XR_OBJ_AOT_NATIVE);
}

static inline bool xr_aot_map_is_typed(const XrAotNativeMapView *map) {
    return map && map->key_type != XR_ELEM_ANY && map->value_type != XR_ELEM_ANY;
}

static inline bool xr_aot_set_is_typed(const XrAotNativeSetView *set) {
    return set && set->elem_type != XR_ELEM_ANY;
}

static inline bool xr_aot_typed_slot_live(const uint8_t *ctrl, int64_t cap, int64_t slot) {
    return ctrl && slot >= 0 && slot < cap && (ctrl[slot] & 0x80u) == 0;
}

static void xr_deep_copy_init_empty_map(XrCopyContext *ctx, XrMap *new_map, uint8_t flags,
                                        uint8_t key_tid, uint8_t value_tid) {
    new_map->count = 0;
    new_map->nentries = 0;
    new_map->entries_cap = 0;
    new_map->indices_size = 0;
    new_map->ctrl = NULL;
    new_map->indices = NULL;
    new_map->entries = NULL;
    new_map->owner_heap = ctx->dst_heap;
    new_map->flags = XR_MAP_FLAG_DUMMY;
    if (flags & XR_MAP_FLAG_WEAK)
        new_map->flags |= XR_MAP_FLAG_WEAK;
    new_map->key_tid = key_tid;
    new_map->value_tid = value_tid;
}

static XrValue xr_deep_copy_aot_boolmap_with_ctx(XrCopyContext *ctx, XrObjHeader *obj) {
    XrAotBoolMapView *map = (XrAotBoolMapView *) obj;
    if (!map || !ctx->dst_fixed_heap)
        return XR_NULL_VAL;
    XrValue cached = xr_copy_context_lookup(ctx, map);
    if (!XR_IS_NULL(cached))
        return cached;

    XrMap *new_map = (XrMap *) copy_ctx_alloc(ctx, sizeof(XrMap), XR_TMAP);
    if (!new_map)
        return XR_NULL_VAL;
    xr_deep_copy_init_empty_map(ctx, new_map, 0, 0, 0);

    XrValue result = XR_FROM_PTR(new_map);
    xr_copy_context_record(ctx, map, result);
    ctx->objects_copied++;

    uint32_t count = map->order_len <= 2 ? map->order_len : 2;
    if (count == 0)
        return result;
    if (!xr_map_reserve_external(new_map, count, ctx->dst_heap))
        return XR_NULL_VAL;
    for (uint32_t oi = 0; oi < count; oi++) {
        uint8_t key = map->order[oi] ? 1 : 0;
        if (((map->present >> key) & 1u) == 0)
            continue;
        XrValue value = map->value_type == XR_ELEM_F32 ? XR_FROM_FLOAT(map->v[key].f)
                                                       : XR_FROM_INT(map->v[key].i);
        xr_map_set(new_map, XR_FROM_BOOL(key != 0), value);
    }
    return result;
}

static XrValue xr_deep_copy_aot_native_map_with_ctx(XrCopyContext *ctx, XrObjHeader *obj) {
    if (XR_OBJ_GET_TYPE(obj) == XR_TBOOLMAP)
        return xr_deep_copy_aot_boolmap_with_ctx(ctx, obj);

    XrAotNativeMapView *map = (XrAotNativeMapView *) obj;
    if (!map || !ctx->dst_fixed_heap)
        return XR_NULL_VAL;
    XrValue cached = xr_copy_context_lookup(ctx, map);
    if (!XR_IS_NULL(cached))
        return cached;

    XrMap *new_map = (XrMap *) copy_ctx_alloc(ctx, sizeof(XrMap), XR_TMAP);
    if (!new_map)
        return XR_NULL_VAL;
    xr_deep_copy_init_empty_map(ctx, new_map, map->flags, map->key_tid, map->value_tid);

    XrValue result = XR_FROM_PTR(new_map);
    xr_copy_context_record(ctx, map, result);
    ctx->objects_copied++;

    if (map->flags & XR_MAP_FLAG_WEAK)
        return result;

    if (xr_aot_map_is_typed(map)) {
        if (map->len <= 0)
            return result;
        if (!xr_map_reserve_external(new_map, (uint32_t) map->len, ctx->dst_heap))
            return XR_NULL_VAL;
        if (map->order && map->order_len > 0) {
            for (int64_t oi = 0; oi < map->order_len; oi++) {
                int64_t slot = map->order[oi];
                if (!xr_aot_typed_slot_live(map->ctrl, map->cap, slot))
                    continue;
                XrValue key = xr_typed_get(map->keys, (int32_t) slot, map->key_type);
                XrValue value = xr_typed_get(map->values, (int32_t) slot, map->value_type);
                xr_map_set(new_map, key, value);
            }
        } else {
            for (int64_t slot = 0; slot < map->cap; slot++) {
                if (!xr_aot_typed_slot_live(map->ctrl, map->cap, slot))
                    continue;
                XrValue key = xr_typed_get(map->keys, (int32_t) slot, map->key_type);
                XrValue value = xr_typed_get(map->values, (int32_t) slot, map->value_type);
                xr_map_set(new_map, key, value);
            }
        }
        return result;
    }

    if ((map->flags & XR_MAP_FLAG_DUMMY) || map->count == 0)
        return result;
    if (!xr_map_reserve_external(new_map, map->count, ctx->dst_heap))
        return XR_NULL_VAL;
    for (uint32_t i = 0; i < map->nentries; i++) {
        XrMapEntry *node = &map->entries[i];
        if (!XR_MAP_ENTRY_EMPTY(node)) {
            xr_map_set(new_map, xr_deep_copy_with_ctx(ctx, node->key),
                       xr_deep_copy_with_ctx(ctx, node->value));
        }
    }
    return result;
}

static XrValue xr_deep_copy_aot_native_set_with_ctx(XrCopyContext *ctx, XrObjHeader *obj) {
    XrAotNativeSetView *set = (XrAotNativeSetView *) obj;
    if (!set || !ctx->dst_fixed_heap)
        return XR_NULL_VAL;
    XrValue cached = xr_copy_context_lookup(ctx, set);
    if (!XR_IS_NULL(cached))
        return cached;

    XrSet *new_set = (XrSet *) copy_ctx_alloc(ctx, sizeof(XrSet), XR_TSET);
    if (!new_set)
        return XR_NULL_VAL;
    xr_set_init_inplace(new_set);
    new_set->owner_heap = ctx->dst_heap;
    if (set->flags & XR_SET_FLAG_WEAK)
        new_set->flags |= XR_SET_FLAG_WEAK;
    new_set->elem_tid = set->elem_tid;

    XrValue result = XR_FROM_PTR(new_set);
    xr_copy_context_record(ctx, set, result);
    ctx->objects_copied++;
    if (set->flags & XR_SET_FLAG_WEAK)
        return result;

    if (xr_aot_set_is_typed(set)) {
        if (set->order && set->order_len > 0) {
            for (int64_t oi = 0; oi < set->order_len; oi++) {
                int64_t slot = set->order[oi];
                if (!xr_aot_typed_slot_live(set->ctrl, set->cap, slot))
                    continue;
                xr_set_add(new_set, xr_typed_get(set->items, (int32_t) slot, set->elem_type));
            }
        } else {
            for (int64_t slot = 0; slot < set->cap; slot++) {
                if (!xr_aot_typed_slot_live(set->ctrl, set->cap, slot))
                    continue;
                xr_set_add(new_set, xr_typed_get(set->items, (int32_t) slot, set->elem_type));
            }
        }
        return result;
    }

    for (uint32_t i = 0; i < set->nentries; i++) {
        XrSetEntry *entry = &set->entries[i];
        if (!XR_SET_ENTRY_EMPTY(entry))
            xr_set_add(new_set, xr_deep_copy_with_ctx(ctx, entry->value));
    }
    return result;
}

XrValue xr_deep_copy_array_with_ctx(XrCopyContext *ctx, XrObjHeader *obj) {
    XrArray *array = (XrArray *) obj;
    if (!array || !ctx->dst_fixed_heap)
        return XR_NULL_VAL;
    XrValue cached = xr_copy_context_lookup(ctx, array);
    if (!XR_IS_NULL(cached))
        return cached;

    int64_t length = array->length;
    XrArray *new_arr = (XrArray *) copy_ctx_alloc(ctx, sizeof(XrArray), XR_TARRAY);
    if (!new_arr)
        return XR_NULL_VAL;

    new_arr->length = length;
    new_arr->capacity = length > 0 ? length : XR_ARRAY_INIT_CAPACITY;
    XR_DCHECK(new_arr->length <= new_arr->capacity, "deep_copy_array: length > capacity");
    new_arr->source = NULL;
    new_arr->data_storage = XR_ARRAY_DATA_HEAP;
    new_arr->elem_type = array->elem_type;
    new_arr->elem_size = array->elem_size;
    new_arr->elem_tid = array->elem_tid;
    new_arr->contains_refs = array->contains_refs;
    new_arr->adt_enum_name = array->adt_enum_name;
    new_arr->adt_member_name = array->adt_member_name;
    new_arr->data_on_region_heap = 0;  // data allocated via xr_malloc (system heap)
    memset(new_arr->_pad, 0, sizeof(new_arr->_pad));

    size_t alloc_size = (size_t) new_arr->elem_size * new_arr->capacity;
    if (new_arr->capacity > 0) {
        new_arr->data = xr_malloc(alloc_size);
        if (!new_arr->data)
            return XR_NULL_VAL;
        memset(new_arr->data, 0, alloc_size);
        /* Notify destination coroutine heap about the external malloc'd data buffer
         * so sweep's sub_external (via xr_obj_destroy_array) balances. */
        if (ctx->dst_heap) {
            xr_coro_heap_add_external(ctx->dst_heap, (int64_t) alloc_size);
        }
    } else {
        new_arr->data = NULL;
    }

    XrValue result = XR_FROM_PTR(new_arr);
    xr_copy_context_record(ctx, array, result);
    ctx->objects_copied++;

    if (array->elem_type == XR_ELEM_ANY) {
        // Deep copy each element
        XrValue *src = (XrValue *) array->data;
        XrValue *dst = (XrValue *) new_arr->data;
        for (int32_t i = 0; i < length; i++)
            dst[i] = xr_deep_copy_with_ctx(ctx, src[i]);
    } else {
        // Typed array: memcpy raw data (no GC pointers)
        if (length > 0)
            memcpy(new_arr->data, array->data, (size_t) length * array->elem_size);
    }
    return result;
}

XrValue xr_deep_copy_map_with_ctx(XrCopyContext *ctx, XrObjHeader *obj) {
    if (xr_aot_native_obj(obj))
        return xr_deep_copy_aot_native_map_with_ctx(ctx, obj);

    XrMap *map = (XrMap *) obj;
    if (!map || !ctx->dst_fixed_heap)
        return XR_NULL_VAL;
    XrValue cached = xr_copy_context_lookup(ctx, map);
    if (!XR_IS_NULL(cached))
        return cached;

    XrMap *new_map = (XrMap *) copy_ctx_alloc(ctx, sizeof(XrMap), XR_TMAP);
    if (!new_map)
        return XR_NULL_VAL;

    new_map->count = 0;
    new_map->nentries = 0;
    new_map->entries_cap = 0;
    new_map->indices_size = 0;
    new_map->ctrl = NULL;
    new_map->indices = NULL;
    new_map->entries = NULL;
    new_map->owner_heap = ctx->dst_heap;
    new_map->flags = XR_MAP_FLAG_DUMMY;
    if (map->flags & XR_MAP_FLAG_WEAK)
        new_map->flags |= XR_MAP_FLAG_WEAK;
    new_map->key_tid = map->key_tid;
    new_map->value_tid = map->value_tid;

    XrValue result = XR_FROM_PTR(new_map);
    if (map->flags & XR_MAP_FLAG_WEAK) {
        xr_copy_context_record(ctx, map, result);
        ctx->objects_copied++;
        return result;
    }
    if (xr_map_isdummy(map) || map->count == 0) {
        xr_copy_context_record(ctx, map, result);
        ctx->objects_copied++;
        return result;
    }

    /* Pre-size the copy for the source's live count, charging the byte
     * accounting to the destination coroutine heap (which later frees the map);
     * without this the awaiting coroutine underflows when it frees a
     * deep-copied map (e.g. a Json result handed back from `await all`).
     * Pre-sizing also means the xr_map_set fill loop never resizes. */
    if (!xr_map_reserve_external(new_map, map->count, ctx->dst_heap))
        return XR_NULL_VAL;

    xr_copy_context_record(ctx, map, result);
    ctx->objects_copied++;

    for (uint32_t i = 0; i < map->nentries; i++) {
        XrMapEntry *node = &map->entries[i];
        if (!XR_MAP_ENTRY_EMPTY(node)) {
            xr_map_set(new_map, xr_deep_copy_with_ctx(ctx, node->key),
                       xr_deep_copy_with_ctx(ctx, node->value));
        }
    }
    return result;
}

XrValue xr_deep_copy_closure_with_ctx(XrCopyContext *ctx, XrObjHeader *obj) {
    XrClosure *closure = (XrClosure *) obj;
    if (!closure || !ctx->dst_fixed_heap)
        return XR_NULL_VAL;
    XrValue cached = xr_copy_context_lookup(ctx, closure);
    if (!XR_IS_NULL(cached))
        return cached;

    size_t alloc_size = sizeof(XrClosure) + closure->upval_count * sizeof(XrValue);
    XrClosure *new_closure = (XrClosure *) copy_ctx_alloc(ctx, alloc_size, XR_TFUNCTION);
    if (!new_closure)
        return XR_NULL_VAL;

    new_closure->proto = closure->proto;
    new_closure->upval_count = closure->upval_count;
    if (ctx->dst_heap)
        XR_OBJ_SET_FLAG(&new_closure->hdr, XR_OBJ_CYCLE_CANDIDATE);
    for (uint16_t i = 0; i < new_closure->upval_count; i++)
        new_closure->upvals[i] = XR_NULL_VAL;

    XrValue result = XR_FROM_PTR(new_closure);
    xr_copy_context_record(ctx, closure, result);
    ctx->objects_copied++;

    // Deep copy flat upvals (cells and values)
    for (int i = 0; i < closure->upval_count; i++) {
        new_closure->upvals[i] = xr_deep_copy_with_ctx(ctx, closure->upvals[i]);
    }
    return result;
}

XrValue xr_deep_copy_cell_with_ctx(XrCopyContext *ctx, XrObjHeader *obj) {
    XrCell *cell = (XrCell *) obj;
    if (!cell || !ctx->dst_fixed_heap)
        return XR_NULL_VAL;
    XrValue cached = xr_copy_context_lookup(ctx, cell);
    if (!XR_IS_NULL(cached))
        return cached;

    XrCell *new_cell = (XrCell *) copy_ctx_alloc(ctx, sizeof(XrCell), XR_TCELL);
    if (!new_cell)
        return XR_NULL_VAL;
    if (ctx->dst_heap)
        XR_OBJ_SET_FLAG(&new_cell->hdr, XR_OBJ_CYCLE_CANDIDATE);
    new_cell->value = XR_NULL_VAL;

    XrValue result = XR_FROM_PTR(new_cell);
    xr_copy_context_record(ctx, cell, result);
    ctx->objects_copied++;

    new_cell->value = xr_deep_copy_with_ctx(ctx, cell->value);
    return result;
}

XrValue xr_deep_copy_set_with_ctx(XrCopyContext *ctx, XrObjHeader *obj) {
    if (xr_aot_native_obj(obj))
        return xr_deep_copy_aot_native_set_with_ctx(ctx, obj);

    XrSet *set = (XrSet *) obj;
    if (!set || !ctx->dst_fixed_heap)
        return XR_NULL_VAL;
    XrValue cached = xr_copy_context_lookup(ctx, set);
    if (!XR_IS_NULL(cached))
        return cached;

    XrSet *new_set = (XrSet *) copy_ctx_alloc(ctx, sizeof(XrSet), XR_TSET);
    if (!new_set)
        return XR_NULL_VAL;
    xr_set_init_inplace(new_set);
    new_set->owner_heap = ctx->dst_heap;
    if (set->flags & XR_SET_FLAG_WEAK)
        new_set->flags |= XR_SET_FLAG_WEAK;
    new_set->elem_tid = set->elem_tid;

    XrValue result = XR_FROM_PTR(new_set);
    xr_copy_context_record(ctx, set, result);
    ctx->objects_copied++;
    if (set->flags & XR_SET_FLAG_WEAK)
        return result;
    for (uint32_t i = 0; i < set->nentries; i++) {
        XrSetEntry *entry = &set->entries[i];
        if (!XR_SET_ENTRY_EMPTY(entry))
            xr_set_add(new_set, xr_deep_copy_with_ctx(ctx, entry->value));
    }
    return result;
}

XrValue xr_deep_copy_instance_with_ctx(XrCopyContext *ctx, XrObjHeader *obj) {
    XrInstance *inst = (XrInstance *) obj;
    if (!inst || !ctx->dst_fixed_heap)
        return XR_NULL_VAL;
    XrValue cached = xr_copy_context_lookup(ctx, inst);
    if (!XR_IS_NULL(cached))
        return cached;

    XrClass *cls = inst->klass;
    uint32_t field_count = xr_class_instance_field_count(cls);

    XrInstance *new_inst = (XrInstance *) copy_ctx_alloc(ctx, xr_instance_size(cls), XR_TINSTANCE);
    if (!new_inst)
        return XR_NULL_VAL;
    xr_instance_init_inplace(new_inst, cls);

    XrValue result = XR_FROM_PTR(new_inst);
    xr_copy_context_record(ctx, inst, result);
    ctx->objects_copied++;

    // Copy fields (dynamic-layout uses in-object + overflow two-tier)
    if (cls->flags & XR_CLASS_DYNAMIC_LAYOUT) {
        uint16_t cap = cls->in_object_capacity;
        uint16_t inobj = (field_count < cap - 1) ? field_count : cap - 1;
        for (uint32_t i = 0; i < inobj; i++) {
            new_inst->fields[i] = xr_deep_copy_with_ctx(ctx, inst->fields[i]);
        }
        if (field_count > cap - 1) {
            XrValue *src_overflow = (XrValue *) inst->fields[cap - 1].ptr;
            if (src_overflow) {
                uint16_t overflow_count = field_count - (cap - 1);
                // Allocate overflow for new instance
                xr_instance_set_dynamic_field_direct(new_inst, cap - 1, xr_null());
                for (uint16_t i = 0; i < overflow_count; i++) {
                    XrValue copied = xr_deep_copy_with_ctx(ctx, src_overflow[i]);
                    xr_instance_set_dynamic_field_direct(new_inst, (cap - 1) + i, copied);
                }
            }
        }
    } else if ((cls->flags & XR_CLASS_FLAT_COPYABLE) && field_count > 0) {
        // Fast path: flat-copyable struct — memcpy all fields at once
        memcpy(new_inst->fields, inst->fields, sizeof(XrValue) * field_count);
    } else {
        for (uint32_t i = 0; i < field_count; i++) {
            new_inst->fields[i] = xr_deep_copy_with_ctx(ctx, inst->fields[i]);
        }
    }

    // Deep-copy native body if present
    XrNativeBodyDesc *desc = cls->native_body;
    if (desc) {
        if (desc->copy_policy == XR_NATIVE_BODY_COPY_FORBID) {
            // Cannot deep-copy types like Channel, Task — return null
            return XR_NULL_VAL;
        }
        if (desc->deep_copy) {
            if (!desc->deep_copy(ctx, inst, new_inst)) {
                return XR_NULL_VAL;
            }
        } else if (desc->copy_policy == XR_NATIVE_BODY_COPY_DEEP) {
            void *src_body = xr_instance_native_body(inst);
            void *dst_body = xr_instance_native_body(new_inst);
            XR_DCHECK(src_body != NULL, "deep_copy_instance: NULL source native body");
            XR_DCHECK(dst_body != NULL, "deep_copy_instance: NULL destination native body");
            memcpy(dst_body, src_body, desc->body_size);
        }
    }
    return result;
}

// xr_deep_copy_json_with_ctx removed: Json values flow through
// xr_deep_copy_instance_with_ctx via the unified transfer table dispatch.

// DateTime is now an XrInstance with native body — deep copy flows
// through xr_deep_copy_instance_with_ctx via the unified transfer table
// dispatch.

XrValue xr_deep_copy_with_ctx(XrCopyContext *ctx, XrValue value) {
    XR_DCHECK(ctx != NULL, "deep_copy_with_ctx: NULL context");
    if (!XR_IS_PTR(value))
        return value;
    XrObjHeader *obj = XR_VALUE_GCPTR(value);
    if (!obj)
        return value;
    /* Immortal fixed-heap singletons (enum value & type descriptors, classes —
     * allocated by xr_fixed_heap_alloc with MANAGED + sticky RC) are referenced by
     * every coroutine. Their identity and native C-struct members (e.g.
     * XrEnumValue.enum_name, which lives OUTSIDE the instance field array) make
     * a structural field-copy both wrong and corrupting. Share by pointer; the
     * sticky RC makes any cross-coroutine retain/drop a no-op. */
    if (XR_OBJ_GET_FLAG(obj, XR_OBJ_MANAGED) &&
        xr_rc_is_sticky(atomic_load_explicit(&obj->refcount, memory_order_relaxed)))
        return value;
    if (XR_OBJ_IS_SHARED(obj)) {
        /* TRANSIT graphs are never pointer-shared: the receive side must
         * materialize a private copy, so fall through to the per-type copy. */
        if (!XR_OBJ_GET_FLAG(obj, XR_OBJ_TRANSIT)) {
            xr_shared_retain(obj);
            return value;
        }
    }

    uint8_t type = XR_OBJ_GET_TYPE(obj);
    if (type >= XR_OBJ_TYPE_MAX)
        return value;

    // Compile-time types resolve through xr_obj_deep_copy_ops in O(1). Slot is
    // NULL for types that are either immediate by policy (TBLOB) or simply
    // not transferable (TCHANNEL, TCOROUTINE, TTASK, TBOUND_METHOD,
    // TEXCEPTION, TERROR). Strings have a hook because short runtime strings
    // are coroutine-local and must be promoted before crossing a boundary.
    XrObjDeepCopyFn fn = xr_obj_deep_copy_ops[type];
    return fn ? fn(ctx, obj) : value;
}

XrValue xr_deep_copy_core(XrRuntimeCore *core, XrValue value, XrFixedHeap *dst_fixed_heap) {
    XR_DCHECK(core != NULL, "deep_copy_core: NULL runtime core");
    if (xr_value_copy_kind(value) != XR_COPY_DEEP)
        return value;
    if (!dst_fixed_heap)
        dst_fixed_heap = &core->fixed_heap;
    XrCopyContext ctx;
    xr_copy_context_init_core(&ctx, core, dst_fixed_heap);
    XrValue result = xr_deep_copy_with_ctx(&ctx, value);
    xr_copy_context_cleanup(&ctx);
    return result;
}

XrValue xr_deep_copy(struct XrayIsolate *X, XrValue value, struct XrFixedHeap *dst_fixed_heap) {
    XR_DCHECK(X != NULL, "deep_copy: NULL isolate");
    return xr_deep_copy_core(xr_isolate_get_runtime_core(X), value,
                             dst_fixed_heap ? dst_fixed_heap : xr_isolate_get_fixed_heap(X));
}

XrValue xr_deep_copy_to_transit_core(XrRuntimeCore *core, XrValue value) {
    XR_DCHECK(core != NULL, "deep_copy_to_transit_core: NULL runtime core");
    if (xr_value_copy_kind(value) != XR_COPY_DEEP)
        return value;
    XrCopyContext ctx;
    xr_copy_context_init_core(&ctx, core, &core->fixed_heap);
    ctx.to_transit = true;
    XrValue result = xr_deep_copy_with_ctx(&ctx, value);
    xr_copy_context_cleanup(&ctx);
    return result;
}

XrValue xr_deep_copy_to_transit(struct XrayIsolate *X, XrValue value) {
    XR_DCHECK(X != NULL, "deep_copy_to_transit: NULL isolate");
    return xr_deep_copy_to_transit_core(xr_isolate_get_runtime_core(X), value);
}

/* ========== Zero-copy buffer move for self-contained scalar arrays ==========
 *
 * A typed (non-ANY) array's data buffer holds only scalars, so it has no
 * interior pointers into any coroutine heap and can be re-homed across heaps
 * (sender → transit → receiver) by moving the malloc'd buffer pointer instead
 * of allocating + memcpy'ing it twice. The small XrArray struct is still
 * allocated per side; only the (potentially large) data buffer moves. Every
 * unsafe shape falls back (returns false) to the normal deep-copy path:
 *   - ANY arrays / contains_refs : interior pointers, not self-contained.
 *   - slices (data_storage == BORROWED) : share a backing store.
 *   - data_on_region_heap : Region-blob data is bound to its owner heap and is
 *     freed on heap teardown — it must never escape to another heap.
 *   - aliased transit (refc != 1) : another holder needs the live buffer.
 */
static bool array_is_movable_scalar(const XrArray *a) {
    return a && a->elem_type != XR_ELEM_ANY && !a->contains_refs &&
           a->data_storage == XR_ARRAY_DATA_HEAP && a->source == NULL && a->capacity > 0 &&
           !a->data_on_region_heap && a->data != NULL && a->length > 0;
}

bool xr_chan_try_move_array_to_transit_core(XrRuntimeCore *core, XrValue value, XrValue *out) {
    if (!core || !out || !XR_IS_PTR(value))
        return false;
    XrObjHeader *obj = XR_VALUE_GCPTR(value);
    if (!obj || XR_OBJ_GET_TYPE(obj) != XR_TARRAY || XR_OBJ_IS_SHARED(obj))
        return false; /* not a coroutine-local array source */
    XrArray *src = (XrArray *) obj;
    if (!array_is_movable_scalar(src))
        return false;

    XrSystemHeap *heap = core->sys_heap;
    if (!heap)
        return false;
    XrObjHeader *th = (XrObjHeader *) xr_sysheap_alloc_shared(heap, sizeof(XrArray), XR_TARRAY);
    if (!th)
        return false;
    th->objsize = (uint32_t) sizeof(XrArray);
    xr_shared_init(th); /* storage = SHARED, atomic refcount = 1 (channel owns) */
    XR_OBJ_SET_FLAG(th, XR_OBJ_TRANSIT);

    XrArray *t = (XrArray *) th;
    t->data = src->data; /* steal the buffer — no element copy */
    t->length = src->length;
    t->capacity = src->capacity;
    t->source = NULL;
    t->data_storage = XR_ARRAY_DATA_HEAP;
    t->elem_type = src->elem_type;
    t->elem_size = src->elem_size;
    t->elem_tid = src->elem_tid;
    t->contains_refs = 0;
    t->adt_enum_name = src->adt_enum_name;
    t->adt_member_name = src->adt_member_name;
    t->data_on_region_heap = 0;
    memset(t->_pad, 0, sizeof(t->_pad));

    /* Detach the buffer from the source so the caller's subsequent destruction
     * of the (now empty) source struct does not free the moved buffer, and
     * remove its bytes from the sender heap's external accounting (the transit
     * object carries no per-coro accounting). */
    size_t data_bytes = (size_t) src->elem_size * (size_t) src->capacity;
    src->data = NULL;
    src->capacity = 0;
    src->length = 0;
    xr_coro_heap_sub_external(xr_current_coro_heap(), (int64_t) data_bytes);

    *out = XR_FROM_PTR(t);
    return true;
}

bool xr_chan_try_move_array_to_transit(struct XrayIsolate *X, XrValue value, XrValue *out) {
    if (!X)
        return false;
    return xr_chan_try_move_array_to_transit_core(xr_isolate_get_runtime_core(X), value, out);
}

bool xr_chan_try_adopt_array_from_transit_core(XrValue value, struct XrCoroutine *recv_coro,
                                               XrValue *out) {
    if (!out || !recv_coro || !XR_IS_PTR(value))
        return false;
    XrObjHeader *obj = XR_VALUE_GCPTR(value);
    if (!obj || XR_OBJ_GET_TYPE(obj) != XR_TARRAY || !XR_OBJ_GET_FLAG(obj, XR_OBJ_TRANSIT))
        return false;
    /* Only the channel may reference this transit graph; an aliased graph must
     * be deep-copied so the other holders keep a live buffer. */
    if (xr_shared_get_refc(obj) != 1)
        return false;
    XrArray *t = (XrArray *) obj;
    if (!array_is_movable_scalar(t))
        return false;

    XrCoroHeap *dst_heap = xr_coro_ensure_heap(recv_coro);
    if (!dst_heap)
        return false;
    XrObjHeader *rh = xr_coro_heap_new_obj(dst_heap, XR_TARRAY, sizeof(XrArray));
    if (!rh)
        return false; /* OOM: caller falls back to the deep-copy path */

    XrArray *r = (XrArray *) rh;
    r->data = t->data; /* steal the buffer — no element copy */
    r->length = t->length;
    r->capacity = t->capacity;
    r->source = NULL;
    r->data_storage = XR_ARRAY_DATA_HEAP;
    r->elem_type = t->elem_type;
    r->elem_size = t->elem_size;
    r->elem_tid = t->elem_tid;
    r->contains_refs = 0;
    r->adt_enum_name = t->adt_enum_name;
    r->adt_member_name = t->adt_member_name;
    r->data_on_region_heap = 0; /* malloc-backed buffer, freed by the array dtor */
    memset(r->_pad, 0, sizeof(r->_pad));

    size_t data_bytes = (size_t) t->elem_size * (size_t) t->capacity;
    xr_coro_heap_add_external(dst_heap, (int64_t) data_bytes);

    /* Detach the buffer from the transit struct so releasing the last channel
     * reference frees only the now-empty struct, not the adopted buffer. */
    t->data = NULL;
    t->capacity = 0;
    t->length = 0;
    xr_chan_transit_release_core(recv_coro ? recv_coro->core : NULL, value);

    *out = XR_FROM_PTR(r);
    return true;
}

bool xr_chan_try_adopt_array_from_transit(struct XrayIsolate *X, XrValue value,
                                          struct XrCoroutine *recv_coro, XrValue *out) {
    (void) X;
    return xr_chan_try_adopt_array_from_transit_core(value, recv_coro, out);
}

XrValue xr_deep_copy_counted_core(XrRuntimeCore *core, XrValue value, XrFixedHeap *dst_fixed_heap,
                                  int *out_count) {
    XR_DCHECK(core != NULL, "deep_copy_counted_core: NULL runtime core");
    if (xr_value_copy_kind(value) != XR_COPY_DEEP) {
        if (out_count)
            *out_count = 0;
        return value;
    }
    if (!dst_fixed_heap)
        dst_fixed_heap = &core->fixed_heap;
    XrCopyContext ctx;
    xr_copy_context_init_core(&ctx, core, dst_fixed_heap);
    XrValue result = xr_deep_copy_with_ctx(&ctx, value);
    if (out_count)
        *out_count = ctx.objects_copied;
    xr_copy_context_cleanup(&ctx);
    return result;
}

XrValue xr_deep_copy_counted(struct XrayIsolate *X, XrValue value,
                             struct XrFixedHeap *dst_fixed_heap, int *out_count) {
    XR_DCHECK(X != NULL, "deep_copy_counted: NULL isolate");
    return xr_deep_copy_counted_core(xr_isolate_get_runtime_core(X), value,
                                     dst_fixed_heap ? dst_fixed_heap : xr_isolate_get_fixed_heap(X),
                                     out_count);
}

XrValue xr_deep_copy_to_coro_core(XrRuntimeCore *core, XrValue value,
                                  struct XrCoroutine *dst_coro) {
    if (!core && dst_coro)
        core = dst_coro->core;
    XR_DCHECK(core != NULL, "deep_copy_to_coro_core: NULL runtime core");
    if (!XR_IS_PTR(value))
        return value;
    // Shared objects (channel, etc): just increment refcount, no copy needed.
    // TRANSIT graphs are the exception: they must be materialized privately.
    XrObjHeader *obj = XR_VALUE_GCPTR(value);
    if (obj && XR_OBJ_IS_SHARED(obj) && !XR_OBJ_GET_FLAG(obj, XR_OBJ_TRANSIT)) {
        xr_shared_retain(obj);
        return value;
    }
    if (xr_value_copy_kind(value) != XR_COPY_DEEP)
        return value;
    /* The value must land in the RECEIVER's private coro heap so the
     * receiver's own dup/drop (which resolve the heap via xr_current_coro_heap =
     * current_coro->heap) reclaim it. A spawned coroutine that only ever
     * receives never triggers lazy heap creation, leaving heap NULL; keying
     * on the field would fall back to the shared isolate heap while the
     * receiver's drop targets its own (now NULL) heap — leaking the object and
     * its data buffer. xr_coro_ensure_heap creates the heap on demand. */
    if (dst_coro) {
        XrCopyContext ctx;
        xr_copy_context_init_core(&ctx, core, &core->fixed_heap);
        ctx.dst_heap = xr_coro_ensure_heap(dst_coro);
        XrValue result = xr_deep_copy_with_ctx(&ctx, value);
        xr_copy_context_cleanup(&ctx);
        return result;
    }
    return xr_deep_copy_core(core, value, &core->fixed_heap);
}

XrValue xr_deep_copy_to_coro(struct XrayIsolate *X, XrValue value, struct XrCoroutine *dst_coro) {
    XR_DCHECK(X != NULL, "deep_copy_to_coro: NULL isolate");
    return xr_deep_copy_to_coro_core(xr_isolate_get_runtime_core(X), value, dst_coro);
}

XrValue xr_deep_copy_to_coro_counted_core(XrRuntimeCore *core, XrValue value,
                                          struct XrCoroutine *dst_coro, int *out_count) {
    if (!core && dst_coro)
        core = dst_coro->core;
    XR_DCHECK(core != NULL, "deep_copy_to_coro_counted_core: NULL runtime core");
    if (xr_value_copy_kind(value) != XR_COPY_DEEP) {
        if (out_count)
            *out_count = 0;
        return value;
    }
    if (dst_coro) {
        XrCopyContext ctx;
        xr_copy_context_init_core(&ctx, core, &core->fixed_heap);
        ctx.dst_heap = xr_coro_ensure_heap(dst_coro);
        XrValue result = xr_deep_copy_with_ctx(&ctx, value);
        if (out_count)
            *out_count = ctx.objects_copied;
        xr_copy_context_cleanup(&ctx);
        return result;
    }
    return xr_deep_copy_counted_core(core, value, &core->fixed_heap, out_count);
}

XrValue xr_deep_copy_to_coro_counted(struct XrayIsolate *X, XrValue value,
                                     struct XrCoroutine *dst_coro, int *out_count) {
    XR_DCHECK(X != NULL, "deep_copy_to_coro_counted: NULL isolate");
    return xr_deep_copy_to_coro_counted_core(xr_isolate_get_runtime_core(X), value, dst_coro,
                                             out_count);
}

XrValue xr_deep_copy_array(struct XrayIsolate *X, struct XrArray *array,
                           struct XrFixedHeap *dst_fixed_heap) {
    XR_DCHECK(X != NULL, "deep_copy_array: NULL isolate");
    if (!array)
        return XR_NULL_VAL;
    if (!dst_fixed_heap)
        dst_fixed_heap = xr_isolate_get_fixed_heap(X);
    XrCopyContext ctx;
    xr_copy_context_init(&ctx, X, dst_fixed_heap);
    XrValue result = xr_deep_copy_array_with_ctx(&ctx, (XrObjHeader *) array);
    xr_copy_context_cleanup(&ctx);
    return result;
}

XrValue xr_deep_copy_map(struct XrayIsolate *X, struct XrMap *map,
                         struct XrFixedHeap *dst_fixed_heap) {
    XR_DCHECK(X != NULL, "deep_copy_map: NULL isolate");
    if (!map)
        return XR_NULL_VAL;
    if (!dst_fixed_heap)
        dst_fixed_heap = xr_isolate_get_fixed_heap(X);
    XrCopyContext ctx;
    xr_copy_context_init(&ctx, X, dst_fixed_heap);
    XrValue result = xr_deep_copy_map_with_ctx(&ctx, (XrObjHeader *) map);
    xr_copy_context_cleanup(&ctx);
    return result;
}

XrValue xr_deep_copy_closure(struct XrayIsolate *X, struct XrClosure *closure,
                             struct XrFixedHeap *dst_fixed_heap) {
    XR_DCHECK(X != NULL, "deep_copy_closure: NULL isolate");
    if (!closure)
        return XR_NULL_VAL;
    if (!dst_fixed_heap)
        dst_fixed_heap = xr_isolate_get_fixed_heap(X);
    XrCopyContext ctx;
    xr_copy_context_init(&ctx, X, dst_fixed_heap);
    XrValue result = xr_deep_copy_closure_with_ctx(&ctx, (XrObjHeader *) closure);
    xr_copy_context_cleanup(&ctx);
    return result;
}

#include "../runtime/mem/xsystem_heap.h"
#include "../runtime/object/xstringbuilder.h"

bool xr_can_relocate(XrValue value) {
    if (!XR_IS_PTR(value))
        return false;
    XrObjHeader *obj = XR_VALUE_GCPTR(value);
    if (!obj)
        return false;
    if (XR_OBJ_IS_SHARED(obj))
        return false;
    if (XR_OBJ_GET_TYPE(obj) == XR_TSTRING)
        return false;
    return true;
}

XrValue xr_to_shared_array(struct XrayIsolate *X, XrObjHeader *obj) {
    XrArray *array = (XrArray *) obj;
    if (!array || !xr_isolate_get_sys_heap(X))
        return XR_NULL_VAL;
    int32_t length = array->length;
    XrArray *new_arr =
        (XrArray *) xr_sysheap_alloc_shared(xr_isolate_get_sys_heap(X), sizeof(XrArray), XR_TARRAY);
    if (!new_arr)
        return XR_NULL_VAL;
    xr_array_init_inplace(new_arr, length > 0 ? length : 4, array->elem_type);
    new_arr->adt_enum_name = array->adt_enum_name;
    new_arr->adt_member_name = array->adt_member_name;
    XR_OBJ_SET_STORAGE(&new_arr->hdr, XR_OBJ_STORAGE_SHARED);
    xr_shared_set_refc(&new_arr->hdr, 1);
    if (array->elem_type == XR_ELEM_ANY) {
        XrValue *src = (XrValue *) array->data;
        for (int32_t i = 0; i < length; i++)
            xr_array_push(new_arr, xr_to_shared(X, src[i]));
    } else {
        // Typed array: memcpy raw data
        if (length > 0) {
            xr_array_ensure_capacity(new_arr, length);
            memcpy(new_arr->data, array->data, (size_t) length * array->elem_size);
            new_arr->length = length;
        }
    }
    return XR_FROM_PTR(new_arr);
}

XrValue xr_to_shared_map(struct XrayIsolate *X, XrObjHeader *obj) {
    XrMap *map = (XrMap *) obj;
    if (!map || !xr_isolate_get_sys_heap(X))
        return XR_NULL_VAL;
    XrMap *new_map =
        (XrMap *) xr_sysheap_alloc_shared(xr_isolate_get_sys_heap(X), sizeof(XrMap), XR_TMAP);
    if (!new_map)
        return XR_NULL_VAL;
    xr_map_init_inplace(new_map, 8);
    if (map->flags & XR_MAP_FLAG_WEAK)
        new_map->flags |= XR_MAP_FLAG_WEAK;
    new_map->key_tid = map->key_tid;
    new_map->value_tid = map->value_tid;
    XR_OBJ_SET_STORAGE(&new_map->hdr, XR_OBJ_STORAGE_SHARED);
    xr_shared_set_refc(&new_map->hdr, 1);
    if (!(map->flags & XR_MAP_FLAG_WEAK) && !xr_map_isdummy(map)) {
        for (uint32_t i = 0; i < map->nentries; i++) {
            XrMapEntry *node = &map->entries[i];
            if (!XR_MAP_ENTRY_EMPTY(node))
                xr_map_set(new_map, xr_to_shared(X, node->key), xr_to_shared(X, node->value));
        }
    }
    return XR_FROM_PTR(new_map);
}

XrValue xr_to_shared_set(struct XrayIsolate *X, XrObjHeader *obj) {
    XrSet *set = (XrSet *) obj;
    if (!set || !xr_isolate_get_sys_heap(X))
        return XR_NULL_VAL;
    XrSet *new_set =
        (XrSet *) xr_sysheap_alloc_shared(xr_isolate_get_sys_heap(X), sizeof(XrSet), XR_TSET);
    if (!new_set)
        return XR_NULL_VAL;
    xr_set_init_inplace(new_set);
    if (set->flags & XR_SET_FLAG_WEAK)
        new_set->flags |= XR_SET_FLAG_WEAK;
    new_set->elem_tid = set->elem_tid;
    XR_OBJ_SET_STORAGE(&new_set->hdr, XR_OBJ_STORAGE_SHARED);
    xr_shared_set_refc(&new_set->hdr, 1);
    if (set->flags & XR_SET_FLAG_WEAK)
        return XR_FROM_PTR(new_set);
    for (uint32_t i = 0; i < set->nentries; i++) {
        XrSetEntry *entry = &set->entries[i];
        if (!XR_SET_ENTRY_EMPTY(entry))
            xr_set_add(new_set, xr_to_shared(X, entry->value));
    }
    return XR_FROM_PTR(new_set);
}

XrValue xr_to_shared_instance(struct XrayIsolate *X, XrObjHeader *obj) {
    XrInstance *inst = (XrInstance *) obj;
    if (!inst || !xr_isolate_get_sys_heap(X))
        return XR_NULL_VAL;
    XrClass *cls = inst->klass;
    XrInstance *new_inst = (XrInstance *) xr_sysheap_alloc_shared(
        xr_isolate_get_sys_heap(X), xr_instance_size(cls), XR_TINSTANCE);
    if (!new_inst)
        return XR_NULL_VAL;
    xr_instance_init_inplace(new_inst, cls);
    XR_OBJ_SET_STORAGE(&new_inst->hdr, XR_OBJ_STORAGE_SHARED);
    xr_shared_set_refc(&new_inst->hdr, 1);
    uint32_t field_count = xr_class_instance_field_count(cls);
    if (cls->flags & XR_CLASS_DYNAMIC_LAYOUT) {
        uint16_t cap = cls->in_object_capacity;
        uint16_t inobj = (field_count < cap - 1) ? field_count : cap - 1;
        for (uint32_t i = 0; i < inobj; i++)
            new_inst->fields[i] = xr_to_shared(X, inst->fields[i]);
        if (field_count > cap - 1) {
            XrValue *src_overflow = (XrValue *) inst->fields[cap - 1].ptr;
            if (src_overflow) {
                uint16_t overflow_count = field_count - (cap - 1);
                xr_instance_set_dynamic_field(X, new_inst, cap - 1, xr_null());
                for (uint16_t i = 0; i < overflow_count; i++) {
                    XrValue shared_val = xr_to_shared(X, src_overflow[i]);
                    xr_instance_set_dynamic_field(X, new_inst, (cap - 1) + i, shared_val);
                }
            }
        }
    } else {
        for (uint32_t i = 0; i < field_count; i++)
            new_inst->fields[i] = xr_to_shared(X, inst->fields[i]);
    }

    // Handle native body to_shared
    XrNativeBodyDesc *desc = cls->native_body;
    if (desc) {
        if (desc->copy_policy == XR_NATIVE_BODY_COPY_FORBID) {
            return XR_NULL_VAL;
        }
        if (desc->to_shared) {
            if (!desc->to_shared(X, inst, new_inst)) {
                return XR_NULL_VAL;
            }
        } else if (desc->copy_policy == XR_NATIVE_BODY_COPY_DEEP) {
            void *src_body = xr_instance_native_body(inst);
            void *dst_body = xr_instance_native_body(new_inst);
            XR_DCHECK(src_body != NULL, "to_shared_instance: NULL source native body");
            XR_DCHECK(dst_body != NULL, "to_shared_instance: NULL destination native body");
            memcpy(dst_body, src_body, desc->body_size);
        }
    }
    return XR_FROM_PTR(new_inst);
}

XrValue xr_to_shared_closure(struct XrayIsolate *X, XrObjHeader *obj) {
    XrClosure *closure = (XrClosure *) obj;
    if (!closure || !xr_isolate_get_sys_heap(X))
        return XR_NULL_VAL;
    XrClosure *new_cl = (XrClosure *) xr_sysheap_alloc_shared(xr_isolate_get_sys_heap(X),
                                                              sizeof(XrClosure), XR_TFUNCTION);
    if (!new_cl)
        return XR_NULL_VAL;
    new_cl->proto = closure->proto;
    new_cl->upval_count = 0;  // shared closures don't carry upvals (captured via shared_array)
    XR_OBJ_SET_STORAGE(&new_cl->hdr, XR_OBJ_STORAGE_SHARED);
    xr_shared_set_refc(&new_cl->hdr, 1);
    return XR_FROM_PTR(new_cl);
}

XrValue xr_to_shared(struct XrayIsolate *X, XrValue value) {
    if (!XR_IS_PTR(value))
        return value;
    XrObjHeader *obj = XR_VALUE_GCPTR(value);
    if (!obj)
        return value;
    /* Immortal fixed-heap singletons (enum descriptors, classes) are global
     * and already coroutine-independent; a structural copy would corrupt their
     * native C-struct members. Share by pointer (see xr_deep_copy_with_ctx). */
    if (XR_OBJ_GET_FLAG(obj, XR_OBJ_MANAGED) &&
        xr_rc_is_sticky(atomic_load_explicit(&obj->refcount, memory_order_relaxed)))
        return value;
    // Already shared: no-op (do NOT incref — caller already owns the reference)
    if (XR_OBJ_IS_SHARED(obj))
        return value;
    if (XR_OBJ_GET_TYPE(obj) == XR_TSTRING) {
        XrString *shared =
            xr_string_clone_shared_core(xr_isolate_get_runtime_core(X), (XrString *) obj);
        return shared ? xr_string_value(shared) : XR_NULL_VAL;
    }

    uint8_t type = XR_OBJ_GET_TYPE(obj);
    if (type >= XR_OBJ_TYPE_MAX)
        return value;

    // Compile-time types resolve through xr_obj_to_shared_ops in O(1). Slot is
    // NULL for types that have no shared form (channels are already
    // shared at construction; coroutines / tasks / cells / bound-methods
    // / exceptions / errors are deliberately not transferable).
    XrObjToSharedFn fn = xr_obj_to_shared_ops[type];
    return fn ? fn(X, obj) : value;
}
