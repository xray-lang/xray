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

void xr_weak_registry_target_dying(XrayIsolate *isolate, XrObjHeader *target,
                                   XrCoroHeap *owner_heap) {
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
        xr_map_purge_weak_target(maps[i], target, owner_heap);
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
