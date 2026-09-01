/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xweak_handle.h - Weak field storage.
 *
 * `weak parent: Node?` does not keep its target alive. It is the ONLY way to
 * break a reference cycle: spec 16.8 says cycles are not reclaimed, and a
 * closure capture edge (which `weak` cannot reach) has only the defer idiom.
 *
 * WHY A HANDLE, NOT A SIDE TABLE KEYED BY THE SLOT
 *
 * A side table mapping "which slots point at this object" has to find every
 * slot when the target dies. But a slot's value gets copied into registers,
 * into other locals, across calls — the table would go stale the moment a
 * copy outlived the update. A handle is indirection instead: the field holds a
 * handle, the handle holds the target, and clearing one pointer invalidates
 * every reader at once.
 *
 * All weak references to the same target share one handle, so a target's death
 * is a single store rather than a walk.
 *
 * WHY NOT Weak<T>
 *
 * `weak` modifies how the SLOT holds the value, not what the value is. A type
 * would enter the type table, take part in monomorphization (against a
 * user-visible XR_MONO_MAX_INSTANCES ceiling), and raise questions with no
 * good answers — is Weak<Weak<T>> legal, what is Weak<T>?, how does a generic
 * bound say "must be a heap object". And every read would need a second
 * unwrap. Making it a type is a category error.
 *
 * THE FIVE RULES (spec 16.3, W1-W5)
 *
 *   W1  Reading promotes. A read returns a STRONG reference (+1) or null —
 *       never a borrowed pointer to something that may die mid-expression.
 *       `node.parent.render()` has to be safe.
 *   W2  Always nullable. The declared type must be `T?`.
 *   W3  Not contagious. No [weak self]; `weak` appears only on a field.
 *   W4  EXEC_LOCAL only. A weak field on a CONST_SHARED / SYNC_SHARED /
 *       MODULE_STATIC object is a compile error.
 *   W5  A definite clearing point: the instant the target's last strong
 *       reference is released. With no cycle collector there is no exception
 *       to this — collection timing was the only nondeterministic source.
 */

#ifndef XR_WEAK_HANDLE_H
#define XR_WEAK_HANDLE_H

#include <stdbool.h>
#include <stdint.h>

#include "xobj_header.h"
#include "../class/xinstance.h"
#include "../value/xvalue.h"
#include "xalloc_unified.h"
#include "xcoro_heap.h"

struct XrCoroHeap;

/* An ordinary RC-managed object. The field holds a strong reference to the
 * handle; the handle holds a NON-owning pointer to the target. */
typedef struct XrWeakHandle {
    XrObjHeader hdr;
    void *target; /* NULL once the target has been destroyed */
    uint8_t target_kind;
    uint8_t reserved[7];
} XrWeakHandle;

typedef enum XrWeakTargetKind {
    XR_WEAK_TARGET_LEGACY_OBJECT = 0,
    XR_WEAK_TARGET_RUNTIME_STRING = 1,
} XrWeakTargetKind;

/* Per-coroutine target -> handle map. Open addressing on the target pointer,
 * same shape as XrHeapPtrSet but carrying a value, so a target's death is one
 * lookup rather than a scan. Coroutine-local and therefore lock-free.
 *
 * The struct body is declared inline in XrCoroHeap so that header need not
 * depend on this one; the two must stay in step. */
typedef struct XrWeakTable XrWeakTable;

/* Get (or create) the shared handle for `target`, marking the target
 * XR_OBJ_HAS_WEAK so its destructor knows to look here. Returns NULL only on
 * allocation failure. The result is NOT retained; the caller stores it. */
XrWeakHandle *xr_weak_handle_acquire(struct XrCoroHeap *heap, XrObjHeader *target);

/* W1: read a weak slot, promoting to a strong reference. Returns NULL when the
 * target is gone; otherwise the target with its refcount raised, which the
 * caller owns. */
XrObjHeader *xr_weak_handle_load(XrWeakHandle *handle);

/* W5: called from the target's destroy path. Clears the handle's target so
 * every reader sees null from this instant. Only objects flagged
 * XR_OBJ_HAS_WEAK reach here, so the common path pays one bit test. */
void xr_weak_table_target_dying(struct XrCoroHeap *heap, XrObjHeader *target);
void xr_weak_table_runtime_target_dying(
    struct XrCoroHeap *heap, XrRuntimeObjectHeader *target);

/* Release the table itself at coroutine teardown. */
void xr_weak_table_destroy(struct XrCoroHeap *heap);

/* No destructor is registered for a handle. The table owns a reference, so a
 * handle cannot outlive its table entry and there is nothing to clean up when
 * one dies — which also keeps it out of the finalize set entirely. */

/* ========== Field-level API ==========
 *
 * A weak slot stores a handle, not the target, so neither side can use the
 * ordinary field accessors. Both are driven from XR_CLASS_HAS_WEAK_FIELDS at
 * the top of the property paths, so a class with no weak field pays one bit
 * test and keeps its inline caches.
 */

/* Read a weak slot as the language sees it: the target with a raised refcount,
 * or null (W1). `slot` is the raw stored value. */
XrValue xr_weak_field_load(XrValue slot);

/* Write a weak slot. Returns the value to store — a handle for the target, or
 * null. The caller releases whatever the slot held before. */
XrValue xr_weak_field_store(struct XrCoroHeap *heap, XrValue target);

/* ========== By-index entry points ==========
 *
 * The name-keyed property opcodes are not the only way into a field: once the
 * receiver's class is known the compiler emits index-based reads and writes
 * (and inline-cached ones), which would otherwise read the handle raw. Every
 * one of those paths has an instance and an index in hand, so they share these
 * two.
 *
 * Both answer false for a class with no weak field after a single bit test,
 * which is what keeps the ordinary paths and their inline caches intact. */

/* Reads a weak slot into *out and returns true; false if index is not weak. */
static inline bool xr_weak_instance_field_load(XrObjectInstance *inst, int index, XrValue *out) {
    if (!inst || !inst->klass || !(inst->klass->flags & XR_CLASS_HAS_WEAK_FIELDS))
        return false;
    if (!xr_class_is_field_weak(inst->klass, index))
        return false;
    *out = xr_weak_field_load(inst->fields[index]); /* W1: promotes, or null */
    return true;
}

/* Writes a weak slot and returns true; false if index is not weak.
 *
 * `val` is deliberately not released: XI_WEAK_STORE_FIELD is non-consuming,
 * so ARC keeps the source's own death point. The handle previously in the slot
 * IS released, so re-pointing a weak field does not leak it.
 *
 * The heap comes from the caller, not from xr_current_coro_heap(): that one
 * reads the worker's TLS, which is empty while top-level script code runs. A
 * NULL heap made the handle allocation fail, the slot took null, and every
 * read of a perfectly live target came back null. */
static inline bool xr_weak_instance_field_store(struct XrCoroHeap *heap, XrObjectInstance *inst,
                                                int index, XrValue val) {
    if (!inst || !inst->klass || !(inst->klass->flags & XR_CLASS_HAS_WEAK_FIELDS))
        return false;
    if (!xr_class_is_field_weak(inst->klass, index))
        return false;
    XrValue slot = xr_weak_field_store(heap, val);
    XrValue prev = inst->fields[index];
    inst->fields[index] = slot;
    xr_rc_release_value(heap, prev);
    return true;
}

#endif /* XR_WEAK_HANDLE_H */
