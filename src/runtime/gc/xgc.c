/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xgc.c - Simplified global GC implementation
 *
 * KEY CONCEPT:
 *   Manages fixed GC objects and type function registration:
 *   1. Fixed objects: lifetime equals program runtime (e.g. main coroutine)
 *   2. Type functions: traverse/destroy/getgclist
 *
 * Note: Runtime objects allocated on coroutine heap (Per-Coroutine GC arch)
 *
 * RELATED MODULES:
 *   - xcoro_gc.c: Per-coroutine GC implementation
 */

#include "xgc_internal.h"
#include "xcoro_gc.h"
#include "xalloc_unified.h"
#include "xweak_registry.h"
#include "../object/xmap.h"
#include "../object/xset.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "../../base/xmutex.h"
#include "../value/xvalue.h"
#include "../core/xr_runtime_core.h"
#include "../xisolate_api.h"
#include "../xisolate_internal.h"
#include "../../coro/xcoroutine.h"
#include "../../coro/xresult_group.h"
#include "../../coro/xwork_queue.h"
#include "../../coro/xworker.h"
#include "../../coro/xdeep_copy.h"  // Per-type deep_copy / to_shared hooks
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../base/xmalloc.h"

/* ========== Compile-Time Per-Type Operations Table (.rodata) ==========
 *
 * One entry per GC type. Each callback is optional:
 *
 *   destroy   — release malloc-backed side resources (sweep / fixedgc cleanup)
 *   traverse  — mark GC-traced children at the mark phase
 *   deep_copy — produce a per-coroutine deep clone (cross-coro send)
 *   to_shared — produce a sysheap shared/refcounted copy (shared store)
 *
 * NULL means "this capability does not apply to this type": it skips
 * the corresponding GC fast path (destroy/traverse) or makes the
 * cross-coroutine dispatcher pass the value through unchanged
 * (deep_copy/to_shared).
 *
 * Adding a new compile-time GC type is a one-liner here. */

const XrTypeOps g_type_ops[XGC_MAX_TYPES] = {
    // Containers — destroy + deep_copy + to_shared (cross-coroutine).
    [XR_TARRAY] = {.destroy = xr_gc_destroy_array,
                   .deep_copy = xr_deep_copy_array_with_ctx,
                   .to_shared = xr_to_shared_array},
    [XR_TMAP] = {.destroy = xr_gc_destroy_map,
                 .deep_copy = xr_deep_copy_map_with_ctx,
                 .to_shared = xr_to_shared_map},
    [XR_TSET] = {.destroy = xr_gc_destroy_set,
                 .deep_copy = xr_deep_copy_set_with_ctx,
                 .to_shared = xr_to_shared_set},
    [XR_TINSTANCE] = {.destroy = xr_gc_destroy_instance,
                      .deep_copy = xr_deep_copy_instance_with_ctx,
                      .to_shared = xr_to_shared_instance},
    [XR_TFUNCTION] = {.destroy = xr_gc_destroy_closure,
                      .deep_copy = xr_deep_copy_closure_with_ctx,
                      .to_shared = xr_to_shared_closure},

    // Channels — already shared at construction; pass-through across coro.
    [XR_TCHANNEL] = {.destroy = xr_gc_destroy_channel},

    // Atomic — system-heap shared object (refcounted). No side resources.
    [XR_TATOMIC] = {0},

    // WorkQueue — system-heap shared object with per-shard buffers.
    [XR_TWORKQUEUE] = {.destroy = xr_gc_destroy_work_queue},

    // ResultGroup — system-heap shared object with queued reduction batches.
    [XR_TRESULTGROUP] = {.destroy = xr_gc_destroy_result_group},

    // Other GC types: have destroy responsibilities, but are deliberately
    // not transferable across coroutines (the dispatchers return the raw
    // value, matching the pre-table default).
    [XR_TCOROUTINE] = {.destroy = xr_gc_destroy_coroutine},
    [XR_TTASK] = {.destroy = xr_gc_destroy_task},
    [XR_TCELL] = {.destroy = xr_gc_destroy_cell, .deep_copy = xr_deep_copy_cell_with_ctx},
    [XR_TBOUND_METHOD] = {0},
    [XR_TMODULE] = {0},
    [XR_TERROR] = {0},

    // NetConn / NetListener are now XR_TINSTANCE with native body
    // descriptors — destroy is handled by xr_gc_destroy_instance.

    // XR_TBLOB / XR_TSTRING are pure leaves with no
    // capabilities; their slots are zero-initialised by default.
};

/* ========== GC State ========== */

#define xr_gc_gettype(o) XR_GC_GET_TYPE(o)

/* ========== Weak Container Registry ========== */

typedef struct XrWeakContainerRegistry {
    XrAdaptiveMutex lock;
    XrMap **maps;
    uint32_t map_count;
    uint32_t map_cap;
    XrSet **sets;
    uint32_t set_count;
    uint32_t set_cap;
} XrWeakContainerRegistry;

static XrWeakContainerRegistry *weak_registry_get(XrayIsolate *isolate, bool create) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    if (!core)
        return NULL;
    XrWeakContainerRegistry *registry = (XrWeakContainerRegistry *) core->weak_registry;
    if (registry || !create)
        return registry;
    registry = (XrWeakContainerRegistry *) xr_calloc(1, sizeof(XrWeakContainerRegistry));
    if (!registry)
        return NULL;
    xr_amutex_init(&registry->lock);
    core->weak_registry = registry;
    return registry;
}

static bool weak_maps_reserve(XrWeakContainerRegistry *registry, uint32_t needed) {
    if (registry->map_cap >= needed)
        return true;
    uint32_t new_cap = registry->map_cap ? registry->map_cap * 2u : 8u;
    while (new_cap < needed)
        new_cap *= 2u;
    XrMap **maps = (XrMap **) xr_realloc(registry->maps, sizeof(XrMap *) * new_cap);
    if (!maps)
        return false;
    registry->maps = maps;
    registry->map_cap = new_cap;
    return true;
}

static bool weak_sets_reserve(XrWeakContainerRegistry *registry, uint32_t needed) {
    if (registry->set_cap >= needed)
        return true;
    uint32_t new_cap = registry->set_cap ? registry->set_cap * 2u : 8u;
    while (new_cap < needed)
        new_cap *= 2u;
    XrSet **sets = (XrSet **) xr_realloc(registry->sets, sizeof(XrSet *) * new_cap);
    if (!sets)
        return false;
    registry->sets = sets;
    registry->set_cap = new_cap;
    return true;
}

void xr_weak_registry_register_map(XrayIsolate *isolate, XrMap *map) {
    if (!isolate || !map || !(map->flags & XR_MAP_FLAG_WEAK) ||
        (map->flags & XR_MAP_FLAG_WEAK_REGISTERED))
        return;
    XrWeakContainerRegistry *registry = weak_registry_get(isolate, true);
    if (!registry)
        return;
    xr_amutex_lock(&registry->lock);
    if (!(map->flags & XR_MAP_FLAG_WEAK_REGISTERED)) {
        if (weak_maps_reserve(registry, registry->map_count + 1)) {
            registry->maps[registry->map_count++] = map;
            map->flags |= XR_MAP_FLAG_WEAK_REGISTERED;
        }
    }
    xr_amutex_unlock(&registry->lock);
}

void xr_weak_registry_unregister_map(XrayIsolate *isolate, XrMap *map) {
    if (!isolate || !map || !(map->flags & XR_MAP_FLAG_WEAK_REGISTERED))
        return;
    XrWeakContainerRegistry *registry = weak_registry_get(isolate, false);
    if (!registry)
        return;
    xr_amutex_lock(&registry->lock);
    for (uint32_t i = 0; i < registry->map_count; i++) {
        if (registry->maps[i] == map) {
            registry->maps[i] = registry->maps[--registry->map_count];
            break;
        }
    }
    map->flags &= (uint8_t) ~XR_MAP_FLAG_WEAK_REGISTERED;
    xr_amutex_unlock(&registry->lock);
}

void xr_weak_registry_register_set(XrayIsolate *isolate, XrSet *set) {
    if (!isolate || !set || !(set->flags & XR_SET_FLAG_WEAK) ||
        (set->flags & XR_SET_FLAG_WEAK_REGISTERED))
        return;
    XrWeakContainerRegistry *registry = weak_registry_get(isolate, true);
    if (!registry)
        return;
    xr_amutex_lock(&registry->lock);
    if (!(set->flags & XR_SET_FLAG_WEAK_REGISTERED)) {
        if (weak_sets_reserve(registry, registry->set_count + 1)) {
            registry->sets[registry->set_count++] = set;
            set->flags |= XR_SET_FLAG_WEAK_REGISTERED;
        }
    }
    xr_amutex_unlock(&registry->lock);
}

void xr_weak_registry_unregister_set(XrayIsolate *isolate, XrSet *set) {
    if (!isolate || !set || !(set->flags & XR_SET_FLAG_WEAK_REGISTERED))
        return;
    XrWeakContainerRegistry *registry = weak_registry_get(isolate, false);
    if (!registry)
        return;
    xr_amutex_lock(&registry->lock);
    for (uint32_t i = 0; i < registry->set_count; i++) {
        if (registry->sets[i] == set) {
            registry->sets[i] = registry->sets[--registry->set_count];
            break;
        }
    }
    set->flags &= (uint8_t) ~XR_SET_FLAG_WEAK_REGISTERED;
    xr_amutex_unlock(&registry->lock);
}

void xr_weak_registry_target_dying(XrayIsolate *isolate, XrGCHeader *target, XrCoroGC *owning_gc) {
    if (!isolate || !target || !(target->extra & XR_OBJ_WEAKABLE))
        return;
    XrWeakContainerRegistry *registry = weak_registry_get(isolate, false);
    if (!registry)
        return;

    XrMap *stack_maps[64];
    XrSet *stack_sets[64];
    XrMap **maps = stack_maps;
    XrSet **sets = stack_sets;
    uint32_t map_count = 0;
    uint32_t set_count = 0;

    xr_amutex_lock(&registry->lock);
    map_count = registry->map_count;
    set_count = registry->set_count;
    if (map_count > (uint32_t) (sizeof(stack_maps) / sizeof(stack_maps[0])))
        maps = (XrMap **) xr_malloc(sizeof(XrMap *) * map_count);
    if (set_count > (uint32_t) (sizeof(stack_sets) / sizeof(stack_sets[0])))
        sets = (XrSet **) xr_malloc(sizeof(XrSet *) * set_count);
    if (maps && map_count > 0)
        memcpy(maps, registry->maps, sizeof(XrMap *) * map_count);
    else
        map_count = 0;
    if (sets && set_count > 0)
        memcpy(sets, registry->sets, sizeof(XrSet *) * set_count);
    else
        set_count = 0;
    xr_amutex_unlock(&registry->lock);

    for (uint32_t i = 0; i < map_count; i++)
        xr_map_purge_weak_target(maps[i], target, owning_gc);
    for (uint32_t i = 0; i < set_count; i++)
        xr_set_purge_weak_target(sets[i], target);

    if (maps != stack_maps)
        xr_free(maps);
    if (sets != stack_sets)
        xr_free(sets);
}

void xr_weak_registry_destroy(XrayIsolate *isolate) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    if (!core || !core->weak_registry)
        return;
    XrWeakContainerRegistry *registry = (XrWeakContainerRegistry *) core->weak_registry;
    xr_free(registry->maps);
    xr_free(registry->sets);
    xr_free(registry);
    core->weak_registry = NULL;
}

static XrGCDestroyFn get_destroy_func(uint8_t type) {
    return (type < XGC_MAX_TYPES) ? g_type_ops[type].destroy : NULL;
}

XR_FUNC bool xr_gc_type_may_need_finalize(uint8_t type) {
    return type < XGC_MAX_TYPES && (g_type_ops[type].destroy != NULL || type > XR_TATOMIC);
}

/* ========== Init/Cleanup ========== */

void xr_gc_init(XrGC *gc, struct XrayIsolate *isolate) {
    XR_DCHECK(gc != NULL, "gc_init: NULL gc");
    memset(gc, 0, sizeof(XrGC));
    gc->isolate = isolate;
    gc->gcstate = XGC_IDLE;
}

void xr_gc_cleanup(XrGC *gc) {
    XR_DCHECK(gc != NULL, "gc_cleanup: NULL gc");
    // Free fixed GC objects
    XrGCObjectNode *node = gc->fixedgc;
    while (node != NULL) {
        XrGCObjectNode *next = node->next;
        XrGCHeader *obj = node->obj;
        uint8_t type = xr_gc_gettype(obj);
        XrGCDestroyFn destroy = get_destroy_func(type);
        if (!destroy && gc->isolate) {
            destroy = (XrGCDestroyFn) xr_isolate_get_ext_destroy(gc->isolate, type);
        }
        if (destroy != NULL) {
            destroy(obj, NULL);
        }
        xr_free(obj);
        xr_free(node);
        node = next;
    }
    gc->fixedgc = NULL;

    gc->object_count = 0;
    gc->totalbytes = 0;
}

/* ========== Allocation (Only for fixed objects during initialization) ========== */

void *xr_gc_alloc(XrGC *gc, size_t size, uint8_t type) {
    XR_DCHECK(gc != NULL, "gc_alloc: NULL gc");
    XR_DCHECK(size >= sizeof(XrGCHeader), "gc_alloc: size too small");
    XR_DCHECK(type < XGC_MAX_TYPES, "gc_alloc: invalid GC type");
    // Global GC: Allocate fixed objects using malloc
    // Note: Runtime objects should use xr_alloc() or xr_coro_gc_alloc()
    XrGCObjectNode *node = (XrGCObjectNode *) xr_malloc(sizeof(XrGCObjectNode));
    if (!node)
        return NULL;
    XrGCHeader *obj = (XrGCHeader *) xr_malloc(size);
    if (obj) {
        obj->type = type;
        obj->extra = XR_OBJ_MANAGED;
        /* Fixed objects are immortal: the sign-tagged RC must be sticky so
         * the hot-path drop (which frees on rc == 0) routes them to the
         * cold path's immortal no-op instead of freeing them. */
        obj->refcount = XR_RC_STICKY;
        obj->objsize = (uint32_t) size;
        obj->_rsv = XR_CYCLE_NOT_IN_ROOTS;
        node->obj = obj;
        node->next = gc->fixedgc;
        gc->fixedgc = node;
        gc->totalbytes += (int64_t) size;
        gc->object_count++;
    } else {
        xr_free(node);
    }
    return obj;
}

XrGCHeader *xr_gc_newobj(XrGC *gc, uint8_t type, size_t size) {
    XR_DCHECK(gc != NULL, "gc_newobj: NULL gc");
    return (XrGCHeader *) xr_gc_alloc(gc, size, type);
}

/* The compile-time RC dup/drop primitives (xr_rc_retain_value /
 * xr_rc_release_value) are inline in xcoro_gc.h: a single implementation
 * shared by the VM and container runtime so the DEAD guard and
 * cycle-root bookkeeping cannot drift between paths. */

/* ========== Debug ========== */

void xr_gc_printstats(XrGC *gc) {
    XR_DCHECK(gc != NULL, "gc_printstats: NULL gc");
    printf("=== XrGC Stats (Global/Fixed) ===\n");
    printf("Objects: %zu\n", gc->object_count);
    printf("Total bytes: %lld\n", (long long) gc->totalbytes);
    printf("=================================\n");
}

// GC Header Debug Print
void xr_gc_header_print(XrGCHeader *obj) {
    if (!obj) {
        printf("GC Header: NULL\n");
        return;
    }
    printf("GC Header:\n");
    printf("  type: %d\n", obj->type);
    printf("  refcount: %d\n", obj->refcount);
    printf("  objsize: %u\n", obj->objsize);
}

/* ========== Unified Allocation Interface ========== */

void *xr_alloc(struct XrCoroutine *coro, size_t size, uint8_t type) {
    XR_DCHECK(coro != NULL, "xr_alloc: coro must not be NULL");
    XR_DCHECK(((XrGCHeader *) coro)->type == XR_TCOROUTINE,
              "xr_alloc: coro is not XrCoroutine (caller passed wrong type)");
    if (!coro)
        return NULL;

    // Lazy coro_gc creation on first heap allocation
    XrCoroGC *gc = xr_coro_ensure_gc(coro);
    if (gc) {
        XrGCHeader *obj = xr_coro_gc_newobj(gc, type, size);
        if (obj)
            return obj;
        xr_log_warning("gc", "xr_alloc: coro_gc allocation failed for type=%d size=%zu", type,
                       size);
        return NULL;
    }

    // Fallback: use runtime core's global GC (needed during early init
    // when coro_gc creation fails due to missing worker/machine).
    if (coro->core) {
        return xr_gc_alloc(&coro->core->gc, size, type);
    }
    return NULL;
}
