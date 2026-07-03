/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xweak_registry.c - Runtime weak container registry.
 */

#include "xweak_registry.h"
#include "../core/xr_runtime_core.h"
#include "xcoro_heap.h"
#include "../object/xmap.h"
#include "../object/xset.h"
#include "../xisolate_api.h"
#include "../../base/xmalloc.h"
#include "../../base/xmutex.h"
#include <string.h>

typedef struct XrWeakContainerRegistry {
    XrAdaptiveMutex lock;
    XrMap **maps;
    uint32_t map_count;
    uint32_t map_cap;
    XrSet **sets;
    uint32_t set_count;
    uint32_t set_cap;
} XrWeakContainerRegistry;

static XrWeakContainerRegistry *weak_registry_get(XrVMRuntime *isolate, bool create) {
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

void xr_weak_registry_register_map(XrVMRuntime *isolate, XrMap *map) {
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

void xr_weak_registry_unregister_map(XrVMRuntime *isolate, XrMap *map) {
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

void xr_weak_registry_register_set(XrVMRuntime *isolate, XrSet *set) {
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

void xr_weak_registry_unregister_set(XrVMRuntime *isolate, XrSet *set) {
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

// Collects removed weak-map values so they can be released AFTER the registry
// lock is dropped. Releasing inline under the lock could recurse (a value's
// destruction re-enters xr_weak_registry_target_dying) and self-deadlock on the
// non-recursive registry lock.
typedef struct {
    XrValue inline_buf[32];
    XrValue *values;
    uint32_t count;
    uint32_t cap;
    bool heap_alloc;
} WeakPurgeSink;

static void weak_purge_sink_init(WeakPurgeSink *s) {
    s->values = s->inline_buf;
    s->count = 0;
    s->cap = (uint32_t) (sizeof(s->inline_buf) / sizeof(s->inline_buf[0]));
    s->heap_alloc = false;
}

static void weak_purge_sink_push(void *ctx, XrValue value) {
    if (!XR_IS_PTR(value))
        return;  // nothing to release for non-pointer values
    WeakPurgeSink *s = (WeakPurgeSink *) ctx;
    if (s->count == s->cap) {
        uint32_t new_cap = s->cap * 2u;
        XrValue *grown;
        if (s->heap_alloc) {
            grown = (XrValue *) xr_realloc(s->values, sizeof(XrValue) * new_cap);
        } else {
            grown = (XrValue *) xr_malloc(sizeof(XrValue) * new_cap);
            if (grown)
                memcpy(grown, s->values, sizeof(XrValue) * s->count);
        }
        if (!grown)
            return;  // OOM: drop this value (leaks one ref; never crashes)
        s->values = grown;
        s->cap = new_cap;
        s->heap_alloc = true;
    }
    s->values[s->count++] = value;
}

static void weak_purge_sink_free(WeakPurgeSink *s) {
    if (s->heap_alloc)
        xr_free(s->values);
}

void xr_weak_registry_target_dying(XrVMRuntime *isolate, XrObjHeader *target,
                                   XrCoroHeap *owner_heap) {
    if (!isolate || !target || !(target->extra & XR_OBJ_WEAKABLE))
        return;
    XrWeakContainerRegistry *registry = weak_registry_get(isolate, false);
    if (!registry)
        return;

    WeakPurgeSink sink;
    weak_purge_sink_init(&sink);

    // Purge under the lock so a concurrent unregister/destroy cannot free or
    // relocate a container mid-iteration (P1-2 UAF). WeakSet purge releases
    // nothing (the dying element IS the target), so it runs directly. WeakMap
    // purge would release each entry's VALUE, which can recurse back into this
    // function and self-deadlock — so we only tombstone + collect here and
    // release the collected values after dropping the lock.
    xr_amutex_lock(&registry->lock);
    for (uint32_t i = 0; i < registry->map_count; i++)
        xr_map_purge_weak_target_collect(registry->maps[i], target, weak_purge_sink_push, &sink);
    for (uint32_t i = 0; i < registry->set_count; i++)
        xr_set_purge_weak_target(registry->sets[i], target);
    xr_amutex_unlock(&registry->lock);

    for (uint32_t i = 0; i < sink.count; i++)
        xr_rc_release_value(owner_heap, sink.values[i]);

    weak_purge_sink_free(&sink);
}

void xr_weak_registry_destroy(XrVMRuntime *isolate) {
    XrRuntimeCore *core = xr_isolate_get_runtime_core(isolate);
    if (!core || !core->weak_registry)
        return;
    XrWeakContainerRegistry *registry = (XrWeakContainerRegistry *) core->weak_registry;
    xr_free(registry->maps);
    xr_free(registry->sets);
    xr_free(registry);
    core->weak_registry = NULL;
}
