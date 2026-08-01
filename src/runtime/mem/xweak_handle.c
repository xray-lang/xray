/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xweak_handle.c - see xweak_handle.h
 */

#include "xweak_handle.h"
#include "xcoro_heap.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"

#include <string.h>

#define XR_WEAK_TABLE_INIT_CAP 16
#define XR_WEAK_TOMBSTONE ((XrWeakHandle *) (uintptr_t) 1)

static inline bool weak_slot_live(const XrWeakHandle *slot) {
    return slot != NULL && slot != XR_WEAK_TOMBSTONE;
}

/* Pointer hash: the low bits of a heap pointer are alignment zeros, so mix the
 * high bits down before masking. */
static inline uint32_t weak_hash(const XrObjHeader *target) {
    uintptr_t x = (uintptr_t) target;
    x ^= x >> 33;
    x *= (uintptr_t) 0xff51afd7ed558ccdULL;
    x ^= x >> 29;
    return (uint32_t) x;
}

static bool weak_table_grow(XrWeakTable *table, uint32_t new_cap) {
    XrWeakHandle **slots = (XrWeakHandle **) xr_calloc(new_cap, sizeof(XrWeakHandle *));
    if (!slots)
        return false;
    for (uint32_t i = 0; i < table->cap; i++) {
        XrWeakHandle *h = table->slots[i];
        if (!weak_slot_live(h))
            continue;
        uint32_t mask = new_cap - 1;
        uint32_t idx = weak_hash(h->target) & mask;
        while (slots[idx])
            idx = (idx + 1) & mask;
        slots[idx] = h;
    }
    xr_free(table->slots);
    table->slots = slots;
    table->cap = new_cap;
    table->tombstones = 0;
    return true;
}

static XrWeakHandle *weak_table_find(XrWeakTable *table, const XrObjHeader *target) {
    if (!table->cap)
        return NULL;
    uint32_t mask = table->cap - 1;
    uint32_t idx = weak_hash(target) & mask;
    for (uint32_t probe = 0; probe <= mask; probe++) {
        XrWeakHandle *h = table->slots[idx];
        if (!h)
            return NULL; /* empty slot ends the probe chain */
        if (h != XR_WEAK_TOMBSTONE && h->target == target)
            return h;
        idx = (idx + 1) & mask;
    }
    return NULL;
}

static bool weak_table_insert(XrWeakTable *table, XrWeakHandle *handle) {
    /* Rehash at 3/4 load, counting tombstones — a chain of them is as costly
     * to probe through as live entries. */
    if (!table->cap) {
        if (!weak_table_grow(table, XR_WEAK_TABLE_INIT_CAP))
            return false;
    } else if ((table->count + table->tombstones + 1) * 4 >= table->cap * 3) {
        if (!weak_table_grow(table, table->cap * 2))
            return false;
    }
    uint32_t mask = table->cap - 1;
    uint32_t idx = weak_hash(handle->target) & mask;
    while (weak_slot_live(table->slots[idx]))
        idx = (idx + 1) & mask;
    if (table->slots[idx] == XR_WEAK_TOMBSTONE)
        table->tombstones--;
    table->slots[idx] = handle;
    table->count++;
    return true;
}

static void weak_table_remove(XrWeakTable *table, const XrObjHeader *target) {
    if (!table->cap)
        return;
    uint32_t mask = table->cap - 1;
    uint32_t idx = weak_hash(target) & mask;
    for (uint32_t probe = 0; probe <= mask; probe++) {
        XrWeakHandle *h = table->slots[idx];
        if (!h)
            return;
        if (h != XR_WEAK_TOMBSTONE && h->target == target) {
            table->slots[idx] = XR_WEAK_TOMBSTONE;
            table->count--;
            table->tombstones++;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

XrWeakHandle *xr_weak_handle_acquire(XrCoroHeap *heap, XrObjHeader *target) {
    if (!heap || !target)
        return NULL;

    /* One handle per target, so the target's death is a single store rather
     * than a walk over every weak field that named it. */
    XrWeakHandle *existing = weak_table_find(&heap->weak_table, target);
    if (existing)
        return existing;

    XrObjHeader *obj = xr_coro_heap_new_obj(heap, XR_TWEAK_HANDLE, sizeof(XrWeakHandle));
    if (!obj)
        return NULL;
    XrWeakHandle *handle = (XrWeakHandle *) obj;
    handle->target = target;

    if (!weak_table_insert(&heap->weak_table, handle)) {
        /* Out of memory registering it. Returning a handle that the target's
         * destructor cannot find would leave a dangling pointer readable as if
         * it were live — far worse than failing the store. */
        xr_coro_heap_destroy_obj(heap, obj);
        return NULL;
    }

    /* The destroy path checks this bit before consulting the table, so an
     * object nobody weakly references pays one bit test. */
    target->extra |= XR_OBJ_HAS_WEAK;
    return handle;
}

XrObjHeader *xr_weak_handle_load(XrWeakHandle *handle) {
    if (!handle || !handle->target)
        return NULL;
    /* W1: reading promotes. Handing back a borrowed pointer would let the
     * target die mid-expression — `node.parent.render()` must be safe. */
    xr_obj_dup(handle->target);
    return handle->target;
}

void xr_weak_table_target_dying(XrCoroHeap *heap, XrObjHeader *target) {
    if (!heap || !target)
        return;
    XrWeakHandle *handle = weak_table_find(&heap->weak_table, target);
    if (!handle)
        return;
    /* W5: from this instant every reader sees null. The handle itself lives on
     * under its own refcount — the fields pointing at it are still valid, they
     * just read empty now. */
    handle->target = NULL;
    weak_table_remove(&heap->weak_table, target);
}

void xr_weak_table_destroy(XrCoroHeap *heap) {
    if (!heap || !heap->weak_table.slots)
        return;
    xr_free(heap->weak_table.slots);
    memset(&heap->weak_table, 0, sizeof(heap->weak_table));
}

void xr_obj_destroy_weak_handle(XrObjHeader *obj, XrCoroHeap *owner_heap) {
    XrWeakHandle *handle = (XrWeakHandle *) obj;
    /* A handle outliving its target is the normal case (the field still refers
     * to it). A handle dying FIRST — every weak field dropped while the target
     * is alive — has to leave the table, or the target's later death would
     * write through a freed handle. */
    if (owner_heap && handle->target)
        weak_table_remove(&owner_heap->weak_table, handle->target);
    handle->target = NULL;
}
