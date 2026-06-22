/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xset.c - Compact ordered hash set implementation (CPython-style compact dict)
 *
 * KEY CONCEPT:
 *   - entries[] is a dense, append-only array in insertion order; iteration
 *     scans it directly so observable order is insertion order.
 *   - ctrl[] is a Swiss-style h2 control-byte table; indices[] maps FULL ctrl
 *     slots to entries[] indices.
 *   - Deletion tombstones the entry (val_tt=0) and marks its ctrl byte DELETED;
 *     dead slots are reclaimed when the table is resized/compacted.
 *   - Resizing is triggered when nentries reaches entries_cap. Because
 *     entries_cap = indices_size*2/3 and every append consumes one index slot,
 *     the index table always keeps an EMPTY slot, so probing always terminates.
 *   - Empty set allocates nothing (DUMMY); the first add allocates both arrays.
 *     Coroutine-heap sets allocate on the Region GC heap on first allocation;
 *     resizes use malloc (region recycling may overlap old blobs).
 */

#include "xset.h"
#include "../../base/xchecks.h"
#include "../value/xvalue_hash.h"
#include "../../base/xmalloc.h"
#include "../mem/xalloc_unified.h"
#include "../mem/xweak_registry.h"
#include "../class/xclass_system.h"
#include "../class/xclass.h"
#include "../mem/xheap.h"
#include "../mem/xcoro_heap.h"
#include "../../coro/xcoroutine.h"
#include "../../shared/xr_swiss_index.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../xvm_call.h"

/* ========== Helper Functions ========== */

// Get value type tag (+1 reserves 0 for empty/tombstone slots).
static inline uint8_t get_val_tt(XrValue value) {
    return (uint8_t) (xr_value_typeid(value) + 1);
}

// Compute a value's hash (canonical hasher, truncated to the index width).
static inline uint32_t hash_value(XrValue value) {
    return (uint32_t) xr_hash_value(value);
}

// Smallest power-of-two index size whose usable capacity (2/3) covers `needed`.
static uint32_t calc_indices_size(uint32_t needed) {
    uint32_t size = 8;
    while ((uint64_t) size * 2 / 3 < needed) {
        if (size >= (1u << XR_SET_MAXHBITS)) {
            size = 1u << XR_SET_MAXHBITS;
            break;
        }
        size <<= 1;
    }
    return size;
}

static inline void xr_set_release_entry(XrSetEntry *e, XrCoroHeap *heap) {
    xr_rc_release_value(heap, e->value);
    e->value = xr_null();
}

static inline bool set_is_weak(const XrSet *set) {
    return (set->flags & XR_SET_FLAG_WEAK) != 0;
}

static inline XrCoroHeap *set_current_or_owner_heap(XrSet *set) {
    XrCoroHeap *heap = xr_current_coro_heap();
    return heap ? heap : (set ? set->owner_heap : NULL);
}

static XrayIsolate *set_owning_isolate(XrCoroHeap *heap) {
    if (heap && heap->owner)
        return xr_coro_vm_owner(heap->owner);
    return NULL;
}

static void xr_set_release_stored_entry(XrSet *set, XrSetEntry *e, XrCoroHeap *heap) {
    if (!set_is_weak(set))
        xr_set_release_entry(e, heap);
    else
        e->value = xr_null();
}

static void xr_set_prepare_weak_value(XrSet *set, XrValue value, XrCoroHeap *heap) {
    if (!set_is_weak(set) || !XR_IS_PTR(value))
        return;
    XrObjHeader *target = XR_VALUE_GCPTR(value);
    XR_OBJ_SET_FLAG(target, XR_OBJ_WEAKABLE);
    xr_weak_registry_register_set(set_owning_isolate(heap), set);
}

/* ========== Swiss Index Lookup ========== */

// Candidate comparator for the shared Swiss probe: type tag then canonical
// equality. xr_value_eq is type-aware, so the tag pre-check never rejects a
// true match; it only short-circuits type-mismatched hash collisions.
static inline int vm_set_value_eq(const XrSetEntry *e, XrValue value, uint8_t val_tt) {
    return e->val_tt == val_tt && xr_value_eq(e->value, value);
}

// Returns the ctrl/indices slot for `value`, or UINT32_MAX if absent.
static uint32_t set_lookup_slot(XrSet *set, XrValue value, uint32_t hash, int32_t *out_eidx) {
    return xr_set_lookup_slot(set->ctrl, set->indices, set->entries, set->indices_size, value, hash,
                              get_val_tt(value), vm_set_value_eq, out_eidx);
}

// Returns entries[] index for `value`, or -1 if absent.
static int32_t set_lookup(XrSet *set, XrValue value, uint32_t hash) {
    int32_t eidx = -1;
    return set_lookup_slot(set, value, hash, &eidx) == UINT32_MAX ? -1 : eidx;
}

/* ========== Grow / Compact ========== */

// Grow (and compact away tombstones) to hold at least `min_needed` live entries.
// Handles the dummy -> first-allocation case too. Returns false on OOM.
static bool set_resize(XrSet *set, uint32_t min_needed) {
    XrCoroHeap *heap = set_current_or_owner_heap(set);
    bool was_dummy = xr_set_isdummy(set);
    XrSetEntry *old_entries = was_dummy ? NULL : set->entries;
    uint8_t *old_ctrl = was_dummy ? NULL : set->ctrl;
    int32_t *old_indices = was_dummy ? NULL : set->indices;
    uint32_t old_nentries = was_dummy ? 0 : set->nentries;
    uint32_t old_isize = was_dummy ? 0 : set->indices_size;
    uint32_t old_ecap = was_dummy ? 0 : set->entries_cap;
    bool old_on_heap = (set->flags & XR_SET_FLAG_NODES_ON_GC) != 0;

    uint32_t needed = set->count > min_needed ? set->count : min_needed;
    if (needed < 1)
        needed = 1;
    uint32_t new_isize = calc_indices_size(needed);
    uint32_t new_ecap = (uint32_t) ((uint64_t) new_isize * 2 / 3);
    if (new_ecap < needed)
        new_ecap = needed;

    // First allocation from dummy may use the Region GC blob heap; subsequent
    // resizes force malloc (region recycling could overlap the old blob while
    // we copy live entries out of it).
    bool new_on_heap = was_dummy && old_on_heap;

    size_t cbytes = (size_t) new_isize + XR_SWISS_GROUP;
    size_t ibytes = sizeof(int32_t) * (size_t) new_isize;
    size_t ebytes = sizeof(XrSetEntry) * (size_t) new_ecap;
    uint8_t *new_ctrl = NULL;
    int32_t *new_indices = NULL;
    XrSetEntry *new_entries = NULL;

    if (new_on_heap && heap) {
        new_ctrl = (uint8_t *) xr_coro_alloc_blob(heap, cbytes);
        new_indices = (int32_t *) xr_coro_alloc_blob(heap, ibytes);
        new_entries = (XrSetEntry *) xr_coro_alloc_blob(heap, ebytes);
        if (!new_ctrl || !new_indices || !new_entries) {
            xr_coro_free_blob(heap, new_ctrl);
            xr_coro_free_blob(heap, new_indices);
            xr_coro_free_blob(heap, new_entries);
            new_on_heap = false;
            new_ctrl = (uint8_t *) xr_malloc(cbytes);
            new_indices = (int32_t *) xr_malloc(ibytes);
            new_entries = (XrSetEntry *) xr_malloc(ebytes);
        }
    } else {
        new_on_heap = false;
        new_ctrl = (uint8_t *) xr_malloc(cbytes);
        new_indices = (int32_t *) xr_malloc(ibytes);
        new_entries = (XrSetEntry *) xr_malloc(ebytes);
    }
    if (!new_ctrl || !new_indices || !new_entries) {
        if (!new_on_heap) {
            xr_free(new_ctrl);
            xr_free(new_indices);
            xr_free(new_entries);
        }
        return false;
    }
    if (!new_on_heap)
        xr_coro_heap_add_external(heap, (int64_t) (cbytes + ibytes + ebytes));

    memset(new_ctrl, (int) XR_SWISS_CTRL_EMPTY, cbytes);
    for (uint32_t i = 0; i < new_isize; i++)
        new_indices[i] = XR_SET_IX_EMPTY;
    memset(new_entries, 0, ebytes);

    // Compactly copy live entries (preserving insertion order), rebuild indices.
    uint32_t w = xr_set_rehash_into(new_entries, new_ctrl, new_indices, new_isize, old_entries,
                                    old_nentries);

    set->ctrl = new_ctrl;
    set->indices = new_indices;
    set->entries = new_entries;
    set->indices_size = new_isize;
    set->entries_cap = new_ecap;
    set->nentries = w;
    set->flags &= ~XR_SET_FLAG_DUMMY;
    set->owner_heap = heap;
    if (new_on_heap)
        set->flags |= XR_SET_FLAG_NODES_ON_GC;
    else
        set->flags &= ~XR_SET_FLAG_NODES_ON_GC;

    if (old_entries) {
        if (old_on_heap) {
            xr_coro_free_blob(heap, old_ctrl);
            xr_coro_free_blob(heap, old_indices);
            xr_coro_free_blob(heap, old_entries);
        } else {
            xr_free(old_ctrl);
            xr_free(old_indices);
            xr_free(old_entries);
            xr_coro_heap_sub_external(heap, (int64_t) ((size_t) old_isize + XR_SWISS_GROUP +
                                                       sizeof(int32_t) * (size_t) old_isize +
                                                       sizeof(XrSetEntry) * (size_t) old_ecap));
        }
    }
    return true;
}

/* ========== Create and Destroy ========== */

XrSet *xr_set_new(struct XrCoroutine *coro) {
    XR_DCHECK(coro != NULL, "set_new: NULL coro");
    XrSet *set = (XrSet *) xr_alloc(coro, sizeof(XrSet), XR_TSET);
    if (!set)
        return NULL;

    xr_obj_header_init_type(&set->hdr, XR_TSET);
    set->owner_heap = xr_coro_get_heap(coro);

    set->count = 0;
    set->nentries = 0;
    set->entries_cap = 0;
    set->indices_size = 0;
    set->ctrl = NULL;
    set->indices = NULL;
    set->entries = NULL;
    set->flags = XR_SET_FLAG_DUMMY | XR_SET_FLAG_NODES_ON_GC;
    set->elem_tid = 0;
    memset(set->_pad, 0, sizeof(set->_pad));

    return set;
}

XrSet *xr_set_new_with_capacity(struct XrCoroutine *coro, uint32_t capacity) {
    XrSet *set = xr_set_new(coro);
    if (!set)
        return NULL;
    if (capacity > 0)
        set_resize(set, capacity);
    return set;
}

// Initialize Set in-place on pre-allocated memory (for shared Set)
void xr_set_init_inplace(XrSet *set) {
    if (!set)
        return;

    set->count = 0;
    set->nentries = 0;
    set->entries_cap = 0;
    set->indices_size = 0;
    set->ctrl = NULL;
    set->indices = NULL;
    set->entries = NULL;
    set->owner_heap = NULL;
    set->flags = XR_SET_FLAG_DUMMY;  // system-heap: arrays via malloc
    set->elem_tid = 0;
    memset(set->_pad, 0, sizeof(set->_pad));
}

/* ========== Basic Operations ========== */

bool xr_set_add(XrSet *set, XrValue value) {
    XR_DCHECK(set != NULL, "set_add: NULL set");
    XR_DCHECK(XR_OBJ_GET_TYPE(&set->hdr) == XR_TSET, "set_add: object is not a set");

    uint32_t hash = hash_value(value);
    XrCoroHeap *heap = set_current_or_owner_heap(set);

    int32_t ix = set_lookup(set, value, hash);
    if (ix >= 0) {
        // Already present: drop the incoming reference, keep the stored one.
        xr_rc_release_value(heap, value);
        return false;
    }

    // New value: ensure entries[] has room (also compacts away tombstones).
    if (set->nentries >= set->entries_cap) {
        if (!set_resize(set, set->count + 1)) {
            xr_rc_release_value(heap, value);
            return false;  // OOM
        }
    }

    uint32_t eidx = set->nentries;
    XrSetEntry *e = &set->entries[eidx];
    e->value = value;
    e->hash = hash;
    e->val_tt = get_val_tt(value);
    set->nentries++;
    set->count++;

    xr_swiss_indices_put(set->ctrl, set->indices, set->indices_size, hash, (int32_t) eidx);
    if (set_is_weak(set)) {
        xr_set_prepare_weak_value(set, value, heap);
        xr_rc_release_value(heap, value);
    }
    return true;
}

bool xr_set_has(XrSet *set, XrValue value) {
    XR_DCHECK(set != NULL, "set_has: NULL set");
    XR_DCHECK(XR_OBJ_GET_TYPE(&set->hdr) == XR_TSET, "set_has: object is not a set");
    if (set->count == 0)
        return false;
    return set_lookup(set, value, hash_value(value)) >= 0;
}

bool xr_set_delete(XrSet *set, XrValue value) {
    XR_DCHECK(set != NULL, "set_delete: NULL set");
    XR_DCHECK(XR_OBJ_GET_TYPE(&set->hdr) == XR_TSET, "set_delete: object is not a set");
    if (xr_set_isdummy(set) || set->count == 0)
        return false;

    uint32_t hash = hash_value(value);
    int32_t ix = -1;
    uint32_t slot = set_lookup_slot(set, value, hash, &ix);
    if (slot == UINT32_MAX)
        return false;

    // Tombstone the entry (keeps its slot so order is preserved) and mark the
    // ctrl slot DELETED so probing skips past it.
    XrSetEntry *e = &set->entries[ix];
    xr_set_release_stored_entry(set, e, set_current_or_owner_heap(set));
    e->val_tt = XR_SET_ENTRY_NIL;
    set->indices[slot] = XR_SET_IX_EMPTY;
    xr_swiss_ctrl_set(set->ctrl, set->indices_size, slot, XR_SWISS_CTRL_DELETED);
    set->count--;
    return true;
}

void xr_set_clear(XrSet *set) {
    XR_DCHECK(set != NULL, "set_clear: NULL set");
    if (xr_set_isdummy(set))
        return;

    XrCoroHeap *heap = set_current_or_owner_heap(set);
    for (uint32_t i = 0; i < set->nentries; i++) {
        XrSetEntry *e = &set->entries[i];
        if (e->val_tt != XR_SET_ENTRY_NIL) {
            xr_set_release_stored_entry(set, e, heap);
            e->val_tt = XR_SET_ENTRY_NIL;
        }
    }
    memset(set->ctrl, (int) XR_SWISS_CTRL_EMPTY, (size_t) set->indices_size + XR_SWISS_GROUP);
    for (uint32_t i = 0; i < set->indices_size; i++)
        set->indices[i] = XR_SET_IX_EMPTY;
    set->nentries = 0;
    set->count = 0;
}

uint32_t xr_set_size(XrSet *set) {
    XR_DCHECK(set != NULL, "set_size: NULL set");
    return set->count;
}

bool xr_set_is_empty(XrSet *set) {
    XR_DCHECK(set != NULL, "set_is_empty: NULL set");
    return set->count == 0;
}

/* ========== Create from Array ========== */

XrSet *xr_set_from_array(struct XrCoroutine *coro, struct XrArray *arr) {
    XR_DCHECK(coro != NULL, "set_from_array: NULL coro");
    XR_DCHECK(arr != NULL, "set_from_array: NULL arr");
    XrSet *set = xr_set_new(coro);

    // Add all elements from array (auto dedup)
    for (int i = 0; i < arr->length; i++) {
        XrValue value = xr_array_get_element(arr, i);
        xr_rc_retain_value(value);
        xr_set_add(set, value);
    }

    return set;
}

/* ========== Iteration Methods (insertion order) ========== */

XrArray *xr_set_values(struct XrCoroutine *coro, XrSet *set) {
    XR_DCHECK(coro != NULL, "set_values: NULL coro");
    XR_DCHECK(set != NULL, "set_values: NULL set");
    XrArray *arr = xr_array_with_capacity(coro, (int32_t) set->count);

    if (!xr_set_isdummy(set)) {
        for (uint32_t i = 0; i < set->nentries; i++) {
            XrSetEntry *e = &set->entries[i];
            if (e->val_tt != XR_SET_ENTRY_NIL) {
                xr_rc_retain_value(e->value);
                xr_array_push(arr, e->value);
            }
        }
    }

    return arr;
}

/* ========== Set Operations ========== */

XrSet *xr_set_union(struct XrCoroutine *coro, XrSet *set1, XrSet *set2) {
    XR_DCHECK(coro != NULL, "set_union: NULL coro");
    XR_DCHECK(set1 != NULL, "set_union: NULL set1");
    XR_DCHECK(set2 != NULL, "set_union: NULL set2");
    XrSet *result = xr_set_new_with_capacity(coro, set1->count + set2->count);

    for (uint32_t i = 0; i < set1->nentries; i++) {
        XrSetEntry *e = &set1->entries[i];
        if (e->val_tt != XR_SET_ENTRY_NIL) {
            xr_rc_retain_value(e->value);
            xr_set_add(result, e->value);
        }
    }
    for (uint32_t i = 0; i < set2->nentries; i++) {
        XrSetEntry *e = &set2->entries[i];
        if (e->val_tt != XR_SET_ENTRY_NIL) {
            xr_rc_retain_value(e->value);
            xr_set_add(result, e->value);
        }
    }

    return result;
}

XrSet *xr_set_intersection(struct XrCoroutine *coro, XrSet *set1, XrSet *set2) {
    XR_DCHECK(coro != NULL, "set_intersection: NULL coro");
    XR_DCHECK(set1 != NULL, "set_intersection: NULL set1");
    XR_DCHECK(set2 != NULL, "set_intersection: NULL set2");
    uint32_t min_count = (set1->count < set2->count) ? set1->count : set2->count;
    XrSet *result = xr_set_new_with_capacity(coro, min_count);

    // Traverse smaller set for efficiency
    XrSet *smaller = (set1->count <= set2->count) ? set1 : set2;
    XrSet *larger = (set1->count <= set2->count) ? set2 : set1;

    for (uint32_t i = 0; i < smaller->nentries; i++) {
        XrSetEntry *e = &smaller->entries[i];
        if (e->val_tt != XR_SET_ENTRY_NIL && xr_set_has(larger, e->value)) {
            xr_rc_retain_value(e->value);
            xr_set_add(result, e->value);
        }
    }

    return result;
}

XrSet *xr_set_difference(struct XrCoroutine *coro, XrSet *set1, XrSet *set2) {
    XR_DCHECK(coro != NULL, "set_difference: NULL coro");
    XR_DCHECK(set1 != NULL, "set_difference: NULL set1");
    XR_DCHECK(set2 != NULL, "set_difference: NULL set2");
    XrSet *result = xr_set_new_with_capacity(coro, set1->count);

    for (uint32_t i = 0; i < set1->nentries; i++) {
        XrSetEntry *e = &set1->entries[i];
        if (e->val_tt != XR_SET_ENTRY_NIL && !xr_set_has(set2, e->value)) {
            xr_rc_retain_value(e->value);
            xr_set_add(result, e->value);
        }
    }

    return result;
}

XrSet *xr_set_symmetric_difference(struct XrCoroutine *coro, XrSet *set1, XrSet *set2) {
    XR_DCHECK(coro != NULL, "set_symmetric_difference: NULL coro");
    XR_DCHECK(set1 != NULL, "set_symmetric_difference: NULL set1");
    XR_DCHECK(set2 != NULL, "set_symmetric_difference: NULL set2");
    XrSet *result = xr_set_new_with_capacity(coro, set1->count + set2->count);

    for (uint32_t i = 0; i < set1->nentries; i++) {
        XrSetEntry *e = &set1->entries[i];
        if (e->val_tt != XR_SET_ENTRY_NIL && !xr_set_has(set2, e->value)) {
            xr_rc_retain_value(e->value);
            xr_set_add(result, e->value);
        }
    }
    for (uint32_t i = 0; i < set2->nentries; i++) {
        XrSetEntry *e = &set2->entries[i];
        if (e->val_tt != XR_SET_ENTRY_NIL && !xr_set_has(set1, e->value)) {
            xr_rc_retain_value(e->value);
            xr_set_add(result, e->value);
        }
    }

    return result;
}

bool xr_set_is_subset(XrSet *set1, XrSet *set2) {
    XR_DCHECK(set1 != NULL, "set_is_subset: NULL set1");
    XR_DCHECK(set2 != NULL, "set_is_subset: NULL set2");
    // Empty set is subset of any set
    if (set1->count == 0)
        return true;
    // If set1 is larger than set2, cannot be subset
    if (set1->count > set2->count)
        return false;

    for (uint32_t i = 0; i < set1->nentries; i++) {
        XrSetEntry *e = &set1->entries[i];
        if (e->val_tt != XR_SET_ENTRY_NIL && !xr_set_has(set2, e->value))
            return false;
    }

    return true;
}

bool xr_set_is_superset(XrSet *set1, XrSet *set2) {
    return xr_set_is_subset(set2, set1);
}

/* ========== GC Integration ========== */

void xr_obj_destroy_set(XrObjHeader *obj, struct XrCoroHeap *owner_heap) {
    XrSet *set = (XrSet *) obj;
    if (set->flags & XR_SET_FLAG_WEAK_REGISTERED)
        xr_weak_registry_unregister_set(set_owning_isolate(owner_heap), set);
    if (!xr_set_isdummy(set) && set->entries) {
        for (uint32_t i = 0; i < set->nentries; i++) {
            XrSetEntry *e = &set->entries[i];
            if (e->val_tt != XR_SET_ENTRY_NIL)
                xr_set_release_stored_entry(set, e, owner_heap);
        }
        size_t bytes = (size_t) set->indices_size + XR_SWISS_GROUP +
                       sizeof(int32_t) * (size_t) set->indices_size +
                       sizeof(XrSetEntry) * (size_t) set->entries_cap;
        if (set->flags & XR_SET_FLAG_NODES_ON_GC) {
            xr_coro_free_blob(owner_heap, set->ctrl);
            xr_coro_free_blob(owner_heap, set->indices);
            xr_coro_free_blob(owner_heap, set->entries);
        } else {
            xr_free(set->ctrl);
            xr_free(set->indices);
            xr_free(set->entries);
            xr_coro_heap_sub_external(owner_heap, (int64_t) bytes);
        }
        set->ctrl = NULL;
        set->indices = NULL;
        set->entries = NULL;
    }
}
