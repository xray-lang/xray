/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xset_weak.c - WeakSet target purge hook.
 */

#include "xset.h"
#include "../../shared/xr_swiss_index.h"

static void weak_set_tombstone_entry(XrSet *set, uint32_t eidx) {
    XrSetEntry *e = &set->entries[eidx];
    e->value = xr_null();
    e->val_tt = XR_SET_ENTRY_NIL;

    for (uint32_t slot = 0; slot < set->indices_size; slot++) {
        if (set->indices[slot] == (int32_t) eidx) {
            set->indices[slot] = XR_SET_IX_EMPTY;
            xr_swiss_ctrl_set(set->ctrl, set->indices_size, slot, XR_SWISS_CTRL_DELETED);
            break;
        }
    }
    if (set->count > 0)
        set->count--;
}

uint32_t xr_set_purge_weak_target(XrSet *set, XrGCHeader *target) {
    if (!set || !target || !(set->flags & XR_SET_FLAG_WEAK) || xr_set_isdummy(set) ||
        !set->entries || (set->gc.extra & XR_OBJ_DEAD))
        return 0;

    uint32_t removed = 0;
    for (uint32_t i = 0; i < set->nentries; i++) {
        XrSetEntry *e = &set->entries[i];
        if (e->val_tt == XR_SET_ENTRY_NIL || !XR_IS_PTR(e->value) ||
            XR_VALUE_GCPTR(e->value) != target)
            continue;
        weak_set_tombstone_entry(set, i);
        removed++;
    }
    return removed;
}
