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
static inline uint32_t weak_hash(const void *target) {
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

static XrWeakHandle *weak_table_find(XrWeakTable *table, const void *target) {
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

static void weak_table_remove(XrWeakTable *table, const void *target) {
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

static XrWeakHandle *weak_handle_acquire_target(
    XrCoroHeap *heap, void *target, XrWeakTargetKind target_kind) {
    if (!heap || !target)
        return NULL;

    /* One handle per target, so the target's death is a single store rather
     * than a walk over every weak field that named it.
     *
     * The caller owns one reference to the result either way: a freshly
     * allocated object already carries it, a reused one has to be dup'd. The
     * table itself does NOT hold a reference — it is a lookup index, and a
     * handle leaving it is exactly what xr_obj_destroy_weak_handle handles. */
    XrWeakHandle *existing = weak_table_find(&heap->weak_table, target);
    if (existing) {
        XR_CHECK(existing->target_kind == (uint8_t) target_kind,
                 "weak target representation changed for a live address");
        xr_obj_dup(&existing->hdr);
        return existing;
    }
    /* The table holds a strong reference of its own (see below), so a fresh
     * handle starts with two owners: the table and the caller. */

    XrObjHeader *obj = xr_coro_heap_new_obj(heap, XR_TWEAK_HANDLE, sizeof(XrWeakHandle));
    if (!obj)
        return NULL;
    XrWeakHandle *handle = (XrWeakHandle *) obj;
    handle->target = target;
    handle->target_kind = (uint8_t) target_kind;
    memset(handle->reserved, 0, sizeof(handle->reserved));

    if (!weak_table_insert(&heap->weak_table, handle)) {
        /* Out of memory registering it. Returning a handle that the target's
         * destructor cannot find would leave a dangling pointer readable as if
         * it were live — far worse than failing the store. */
        xr_coro_heap_destroy_obj(heap, obj);
        return NULL;
    }

    /* The table OWNS a reference. That is what removes the entire "handle dies
     * before its target" problem: with the table holding one, a handle cannot
     * outlive its table entry, so the target's death never writes through a
     * freed handle and no finalizer is needed to keep the two in step.
     * The cost is one small object living until its target does. */
    xr_obj_dup(&handle->hdr);

    /* The destroy path checks this bit before consulting the table, so an
     * object nobody weakly references pays one bit test. */
    return handle;
}

XrWeakHandle *xr_weak_handle_acquire(XrCoroHeap *heap,
                                     XrObjHeader *target) {
    XrWeakHandle *handle = weak_handle_acquire_target(
        heap, target, XR_WEAK_TARGET_LEGACY_OBJECT);
    if (handle)
        target->extra |= XR_OBJ_HAS_WEAK;
    return handle;
}

XrObjHeader *xr_weak_handle_load(XrWeakHandle *handle) {
    if (!handle || !handle->target ||
        handle->target_kind != XR_WEAK_TARGET_LEGACY_OBJECT)
        return NULL;
    /* W1: reading promotes. Handing back a borrowed pointer would let the
     * target die mid-expression — `node.parent.render()` must be safe. */
    xr_obj_dup((XrObjHeader *) handle->target);
    return (XrObjHeader *) handle->target;
}

static void weak_table_target_dying(XrCoroHeap *heap, void *target,
                                    XrWeakTargetKind target_kind) {
    if (!heap || !target)
        return;
    XrWeakHandle *handle = weak_table_find(&heap->weak_table, target);
    if (!handle)
        return;
    XR_CHECK(handle->target_kind == (uint8_t) target_kind,
             "weak target representation changed before destruction");
    /* W5: from this instant every reader sees null. The handle itself lives on
     * under its own refcount — the fields pointing at it are still valid, they
     * just read empty now. */
    handle->target = NULL;
    weak_table_remove(&heap->weak_table, target);
    /* Hand back the table's reference. A field still naming this handle keeps
     * it alive and simply reads null from here on; when the last one goes, so
     * does the handle. Holding the reference until heap teardown instead would
     * leak one handle per target that was ever weakly referenced — which is
     * exactly the leak `weak` exists to remove. */
    xr_rc_release(heap, &handle->hdr);
}

void xr_weak_table_target_dying(XrCoroHeap *heap, XrObjHeader *target) {
    weak_table_target_dying(heap, target, XR_WEAK_TARGET_LEGACY_OBJECT);
}

void xr_weak_table_runtime_target_dying(
    XrCoroHeap *heap, XrRuntimeObjectHeader *target) {
    weak_table_target_dying(heap, target, XR_WEAK_TARGET_RUNTIME_STRING);
}

void xr_weak_table_destroy(XrCoroHeap *heap) {
    if (!heap || !heap->weak_table.slots)
        return;
    /* Teardown frees every object in bulk, so the table's references are not
     * released one by one — just the index itself. */
    xr_free(heap->weak_table.slots);
    memset(&heap->weak_table, 0, sizeof(heap->weak_table));
}

/* ========== Field-level API ========== */

XrValue xr_weak_field_load(XrValue slot) {
    if (!XR_IS_PTR(slot))
        return xr_null(); /* never written, or written null */
    XrObjHeader *hdr = XR_VALUE_GCPTR(slot);
    if (!hdr || XR_OBJ_GET_TYPE(hdr) != XR_TWEAK_HANDLE)
        return xr_null();
    XrWeakHandle *handle = (XrWeakHandle *) hdr;
    if (handle->target_kind == XR_WEAK_TARGET_RUNTIME_STRING) {
        XrString *string = (XrString *) handle->target;
        if (!string)
            return xr_null();
        if (xr_runtime_object_header_retain(&string->header) !=
            XR_RUNTIME_ABI_OK)
            return xr_null();
        return XR_FROM_STR(string);
    }
    XrObjHeader *target = xr_weak_handle_load(handle);
    if (!target)
        return xr_null(); /* W5: the target is gone, so the slot reads null */
    return XR_FROM_PTR(target);
}

XrValue xr_weak_field_store(XrCoroHeap *heap, XrValue target) {
    if (!XR_IS_PTR(target))
        return xr_null(); /* storing null (or a scalar, which W3 rejects) */
    if (XR_IS_STRING(target)) {
        XrString *string = XR_TO_STRING(target);
        if (!string || string->header.domain_id !=
                           XR_RUNTIME_STRING_DOMAIN_EXEC_LOCAL ||
            xr_runtime_object_register_weak(&string->header, heap) !=
                XR_RUNTIME_ABI_OK)
            return xr_null();
        XrWeakHandle *handle = weak_handle_acquire_target(
            heap, string, XR_WEAK_TARGET_RUNTIME_STRING);
        return handle ? XR_FROM_PTR(&handle->hdr) : xr_null();
    }
    XrObjHeader *obj = XR_VALUE_GCPTR(target);
    if (!obj)
        return xr_null();
    /* acquire hands back one owned reference; the slot takes it. The slot owns
     * the HANDLE and never the target, which is the whole point. */
    XrWeakHandle *handle = xr_weak_handle_acquire(heap, obj);
    if (!handle)
        return xr_null(); /* out of memory: a null slot beats a dangling one */
    return XR_FROM_PTR(&handle->hdr);
}
