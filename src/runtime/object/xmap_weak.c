/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmap_weak.c - WeakMap target purge hook.
 */

#include "xmap.h"
#include "../mem/xcoro_heap.h"
#include "../../shared/xr_swiss_index.h"

static void weak_map_tombstone_entry(XrMap *map, uint32_t eidx, XrCoroHeap *heap) {
    XrMapEntry *e = &map->entries[eidx];
    xr_rc_release_value(heap, e->value);
    e->key = xr_null();
    e->value = xr_null();
    e->key_tt = XR_MAP_ENTRY_NIL_KEY;

    for (uint32_t slot = 0; slot < map->indices_size; slot++) {
        if (map->indices[slot] == (int32_t) eidx) {
            map->indices[slot] = XR_MAP_IX_EMPTY;
            xr_swiss_ctrl_set(map->ctrl, map->indices_size, slot, XR_SWISS_CTRL_DELETED);
            break;
        }
    }
    if (map->count > 0)
        map->count--;
}

uint32_t xr_map_purge_weak_target(XrMap *map, XrObjHeader *target, XrCoroHeap *owner_heap) {
    if (!map || !target || !(map->flags & XR_MAP_FLAG_WEAK) || xr_map_isdummy(map) ||
        !map->entries || (map->hdr.extra & XR_OBJ_DEAD))
        return 0;

    uint32_t removed = 0;
    for (uint32_t i = 0; i < map->nentries; i++) {
        XrMapEntry *e = &map->entries[i];
        if (e->key_tt == XR_MAP_ENTRY_NIL_KEY || !XR_IS_PTR(e->key) ||
            XR_VALUE_GCPTR(e->key) != target)
            continue;
        weak_map_tombstone_entry(map, i, owner_heap);
        removed++;
    }
    return removed;
}
