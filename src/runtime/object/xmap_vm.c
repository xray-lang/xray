/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmap_vm.c - VM-facing Map adapters
 */

#include "xmap_vm.h"

#include "xiterator.h"
#include "../../base/xchecks.h"
#include "../../coro/xcoroutine.h"
#include "../xvm_call.h"

struct XrIterator *xr_map_entries_iterator(struct XrVMRuntime *iso, XrMap *map) {
    return xr_iterator_new_from_map(xr_current_coro(iso), map);
}

void xr_map_foreach(struct XrVMRuntime *isolate, XrMap *map, struct XrClosure *callback) {
    XR_DCHECK(map != NULL, "xr_map_foreach: NULL map");
    XR_DCHECK(callback != NULL, "xr_map_foreach: NULL callback");
    if (xr_map_isdummy(map))
        return;

    XrValue args[2];
    for (uint32_t i = 0; i < map->nentries; i++) {
        XrMapEntry *e = &map->entries[i];
        if (e->key_tt != XR_MAP_ENTRY_NIL_KEY) {
            args[0] = e->key;
            args[1] = e->value;
            xr_vm_call_closure(isolate, callback, args, 2);
        }
    }
}
