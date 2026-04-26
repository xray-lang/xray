/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xset.c - Hash-based Set implementation
 *
 * KEY CONCEPT:
 *   - Set object: GC allocated
 *   - Coroutine-heap sets: entries[] as GC blob on Immix heap (no malloc/free)
 *   - System-heap sets: entries[] via xr_malloc(freed by destructor)
 */

#include "xset.h"
#include "../xisolate_api.h"
#include "../../base/xchecks.h"
#include "../value/xvalue_hash.h"
#include "../../base/xmalloc.h"
#include "../gc/xalloc_unified.h"
#include "../class/xclass_system.h"
#include "../class/xclass.h"
#include "../gc/xgc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../xvm_call.h"

/* ========== Helper Functions ========== */

// Check if entry is empty
static inline bool entry_is_empty(XrSetEntry *entry) {
    return entry->state == XR_SET_EMPTY;
}

// Check if entry is tombstone
static inline bool entry_is_tombstone(XrSetEntry *entry) {
    return entry->state == XR_SET_TOMBSTONE;
}

// Check if entry is valid
static inline bool entry_is_valid(XrSetEntry *entry) {
    return entry->state >= XR_SET_VALID;
}

// Calculate next capacity (power of 2)
static uint32_t next_capacity(uint32_t current) {
    if (current < XR_SET_MIN_CAPACITY) {
        return XR_SET_MIN_CAPACITY;
    }

    // Prevent overflow
    if (current > (UINT32_MAX / XR_SET_GROW_FACTOR)) {
        return UINT32_MAX;
    }

    return current * XR_SET_GROW_FACTOR;
}

/* ========== Create and Destroy ========== */

XrSet *xr_set_new(struct XrCoroutine *coro) {
    XR_DCHECK(coro != NULL, "set_new: NULL coro");
    // Allocate Set on coroutine heap
    XrSet *set = (XrSet *) xr_alloc(coro, sizeof(XrSet), XR_TSET);

    if (!set) {
        return NULL;
    }

    xr_gc_header_init_type(&set->gc, XR_TSET);

    // Initialize Set data
    set->capacity = 0;
    set->count = 0;
    set->tombstones = 0;
    set->entries = NULL;
    set->flags = 0;
    set->elem_tid = 0;
    memset(set->_pad, 0, sizeof(set->_pad));

    return set;
}

XrSet *xr_set_new_with_capacity(struct XrCoroutine *coro, uint32_t capacity) {
    XrSet *set = xr_set_new(coro);
    if (!set)
        return NULL;

    if (capacity > 0) {
        // Round up to power of 2
        uint32_t cap = XR_SET_MIN_CAPACITY;
        while (cap < capacity)
            cap *= 2;
        xr_set_resize(set, cap);
    }
    return set;
}

// Initialize Set in-place on pre-allocated memory (for shared Set)
void xr_set_init_inplace(XrSet *set) {
    if (!set)
        return;

    // Initialize Set data
    set->capacity = 0;
    set->count = 0;
    set->tombstones = 0;
    set->entries = NULL;
    set->flags = 0;
    set->elem_tid = 0;
    memset(set->_pad, 0, sizeof(set->_pad));
}

/* ========== Small Set Linear Scan ========== */

XrSetEntry *xr_set_find_linear(XrSet *set, XrValue value, uint32_t *out_index) {
    XR_DCHECK(set != NULL, "set_find_linear: NULL set");
    XR_DCHECK(out_index != NULL, "set_find_linear: NULL out_index");
    // Empty Set
    if (set->capacity == 0) {
        *out_index = 0;
        return NULL;
    }

    uint32_t first_empty = UINT32_MAX;
    uint32_t first_tombstone = UINT32_MAX;

    // Linear scan all slots
    for (uint32_t i = 0; i < set->capacity; i++) {
        XrSetEntry *entry = &set->entries[i];

        if (entry_is_empty(entry)) {
            if (first_empty == UINT32_MAX) {
                first_empty = i;
            }
            // Empty slot found, value doesn't exist
            *out_index = (first_tombstone != UINT32_MAX) ? first_tombstone : first_empty;
            return NULL;
        } else if (entry_is_tombstone(entry)) {
            if (first_tombstone == UINT32_MAX) {
                first_tombstone = i;
            }
        } else if (entry_is_valid(entry)) {
            // Compare value
            if (xr_value_eq(entry->value, value)) {
                *out_index = i;
                return entry;
            }
        }
    }

    // Not found, return insertable position
    *out_index = (first_tombstone != UINT32_MAX) ? first_tombstone : first_empty;
    return NULL;
}

/* ========== Hash Lookup ========== */

// Core lookup with pre-computed hash (avoids duplicate hash computation)
static XrSetEntry *set_find_entry_hashed(XrSet *set, XrValue value, uint32_t hash_index,
                                         uint8_t hash_prefix, uint32_t *out_index) {
    uint32_t first_tombstone = UINT32_MAX;

    // Linear probing
    for (uint32_t i = 0; i < set->capacity; i++) {
        uint32_t probe_index = (hash_index + i) & (set->capacity - 1);
        XrSetEntry *entry = &set->entries[probe_index];

        if (entry_is_empty(entry)) {
            *out_index = (first_tombstone != UINT32_MAX) ? first_tombstone : probe_index;
            return NULL;
        } else if (entry_is_tombstone(entry)) {
            if (first_tombstone == UINT32_MAX) {
                first_tombstone = probe_index;
            }
        } else if (entry_is_valid(entry)) {
            // Short hash optimization: compare prefix first
            if (entry->state == hash_prefix) {
                if (xr_value_eq(entry->value, value)) {
                    *out_index = probe_index;
                    return entry;
                }
            }
        }
    }

    *out_index = (first_tombstone != UINT32_MAX) ? first_tombstone : 0;
    return NULL;
}

XrSetEntry *xr_set_find_entry(XrSet *set, XrValue value, uint32_t *out_index) {
    if (set->capacity == 0) {
        *out_index = 0;
        return NULL;
    }

    // Small Set optimization: linear scan
    if (set->count <= XR_SET_SMALL_SIZE) {
        return xr_set_find_linear(set, value, out_index);
    }

    uint64_t hash = xr_hash_value(value);
    uint32_t hash_index = hash & (set->capacity - 1);
    uint8_t hash_prefix = (uint8_t) ((hash >> 56) | 0x80);

    return set_find_entry_hashed(set, value, hash_index, hash_prefix, out_index);
}

/* ========== Resize ========== */

void xr_set_resize(XrSet *set, uint32_t new_capacity) {
    XR_DCHECK(set != NULL, "set_resize: NULL set");
    XR_DCHECK(new_capacity > 0, "set_resize: zero capacity");
    XR_DCHECK((new_capacity & (new_capacity - 1)) == 0, "set_resize: capacity not power-of-2");
    size_t alloc_bytes = sizeof(XrSetEntry) * new_capacity;
    // entries[] always lives on malloc — this avoids Immix line
    // recycling overlapping with the old entries during the rehash
    // loop below.
    XrSetEntry *new_entries = (XrSetEntry *) xr_malloc(alloc_bytes);
    if (!new_entries)
        return;
    // Tell the per-coro GC about the new external buffer; the matching
    // sub_external for the old buffer happens after the rehash below.
    xr_gc_add_external(xr_current_coro_gc(), (int64_t) alloc_bytes);

    // Initialize new array (XR_SET_EMPTY=0x00 and xr_null() are all-zeros)
    memset(new_entries, 0, alloc_bytes);

    // Save old data
    XrSetEntry *old_entries = set->entries;
    uint32_t old_capacity = set->capacity;
    size_t old_alloc_bytes = sizeof(XrSetEntry) * old_capacity;

    // Update Set
    set->entries = new_entries;
    set->capacity = new_capacity;
    set->count = 0;       // Recount
    set->tombstones = 0;  // Tombstones cleared by rehash

    // Re-insert all elements using hash-based placement.
    // Must NOT use xr_set_add here because it uses linear scan when
    // count <= XR_SET_SMALL_SIZE, placing entries at wrong positions
    // that hash-based lookup later cannot find.
    if (old_entries != NULL) {
        for (uint32_t i = 0; i < old_capacity; i++) {
            XrSetEntry *old_entry = &old_entries[i];
            if (entry_is_valid(old_entry)) {
                uint64_t hash = xr_hash_value(old_entry->value);
                uint32_t idx = hash & (new_capacity - 1);
                uint8_t prefix = (uint8_t) ((hash >> 56) | 0x80);
                // Linear probing into guaranteed-empty table
                while (!entry_is_empty(&set->entries[idx])) {
                    idx = (idx + 1) & (new_capacity - 1);
                }
                set->entries[idx].value = old_entry->value;
                set->entries[idx].state = prefix;
                set->count++;
            }
        }
        xr_free(old_entries);
        // Balance the add_external above: the old entries are gone.
        xr_gc_sub_external(xr_current_coro_gc(), (int64_t) old_alloc_bytes);
    }
}

/* ========== Basic Operations ========== */

bool xr_set_add(XrSet *set, XrValue value) {
    XR_DCHECK(set != NULL, "set_add: NULL set");
    XR_DCHECK(XR_GC_GET_TYPE(&set->gc) == XR_TSET, "set_add: object is not a set");
    // Check if resize needed
    if (set->count + 1 > set->capacity * XR_SET_LOAD_FACTOR) {
        uint32_t new_capacity = next_capacity(set->capacity);
        xr_set_resize(set, new_capacity);
    }

    // Pre-compute hash once, reuse for both find and insert
    uint64_t hash = xr_hash_value(value);
    uint8_t hash_prefix = (uint8_t) ((hash >> 56) | 0x80);

    uint32_t index;
    XrSetEntry *entry;

    // Small Set: linear scan (no hash needed for lookup)
    if (set->count <= XR_SET_SMALL_SIZE) {
        entry = xr_set_find_linear(set, value, &index);
    } else {
        uint32_t hash_index = hash & (set->capacity - 1);
        entry = set_find_entry_hashed(set, value, hash_index, hash_prefix, &index);
    }

    if (entry != NULL) {
        return false;
    }

    set->entries[index].value = value;
    set->entries[index].state = hash_prefix;
    set->count++;
    XR_DCHECK(set->count <= set->capacity, "set_add: count > capacity");
    XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), set);

    return true;
}

bool xr_set_has(XrSet *set, XrValue value) {
    XR_DCHECK(set != NULL, "set_has: NULL set");
    XR_DCHECK(XR_GC_GET_TYPE(&set->gc) == XR_TSET, "set_has: object is not a set");
    if (set->count == 0) {
        return false;
    }

    uint32_t index;
    XrSetEntry *entry = xr_set_find_entry(set, value, &index);
    return entry != NULL;
}

bool xr_set_delete(XrSet *set, XrValue value) {
    XR_DCHECK(set != NULL, "set_delete: NULL set");
    XR_DCHECK(XR_GC_GET_TYPE(&set->gc) == XR_TSET, "set_delete: object is not a set");
    if (set->count == 0) {
        return false;
    }

    uint32_t index;
    XrSetEntry *entry = xr_set_find_entry(set, value, &index);

    if (entry == NULL) {
        return false;
    }

    // Mark as tombstone
    entry->state = XR_SET_TOMBSTONE;
    set->count--;
    XR_DCHECK(set->count + set->tombstones <= set->capacity,
              "set_delete: count+tombstones > capacity");
    set->tombstones++;

    // Rehash to clear tombstones when they exceed 25% of capacity
    if (set->capacity > XR_SET_MIN_CAPACITY && set->tombstones > set->capacity / 4) {
        xr_set_resize(set, set->capacity);
    }

    return true;
}

void xr_set_clear(XrSet *set) {
    XR_DCHECK(set != NULL, "set_clear: NULL set");
    if (set->entries != NULL) {
        for (uint32_t i = 0; i < set->capacity; i++) {
            set->entries[i].state = XR_SET_EMPTY;
            set->entries[i].value = xr_null();
        }
    }
    set->count = 0;
    set->tombstones = 0;
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
        xr_set_add(set, xr_array_get_element(arr, i));
    }

    return set;
}

/* ========== Iteration Methods ========== */

XrArray *xr_set_values(struct XrCoroutine *coro, XrSet *set) {
    XR_DCHECK(coro != NULL, "set_values: NULL coro");
    XR_DCHECK(set != NULL, "set_values: NULL set");
    XrArray *arr = xr_array_with_capacity(coro, (int32_t) set->count);

    // Traverse Set, add all values
    if (set->entries != NULL) {
        for (uint32_t i = 0; i < set->capacity; i++) {
            XrSetEntry *entry = &set->entries[i];
            if (entry_is_valid(entry)) {
                xr_array_push(arr, entry->value);
            }
        }
    }

    return arr;
}

void xr_set_foreach(XrSet *set, XrayIsolate *isolate, struct XrClosure *callback) {
    if (set->entries == NULL || callback == NULL) {
        return;
    }

    // Traverse all valid entries
    for (uint32_t i = 0; i < set->capacity; i++) {
        XrSetEntry *entry = &set->entries[i];
        if (entry_is_valid(entry)) {
            // Call callback: callback(value)
            XrValue args[1];
            args[0] = entry->value;
            xr_vm_call_closure(isolate, callback, args, 1);
        }
    }
}

XrSet *xr_set_map(XrSet *set, XrayIsolate *isolate, struct XrClosure *callback) {
    XrSet *new_set = xr_set_new(xr_current_coro(isolate));

    if (set->entries == NULL || callback == NULL) {
        return new_set;
    }

    // Traverse all valid entries
    for (uint32_t i = 0; i < set->capacity; i++) {
        XrSetEntry *entry = &set->entries[i];
        if (entry_is_valid(entry)) {
            // Call callback: callback(value)
            XrValue args[1];
            args[0] = entry->value;
            XrValue result = xr_vm_call_closure(isolate, callback, args, 1);

            // Add result to new Set
            xr_set_add(new_set, result);
        }
    }

    return new_set;
}

XrSet *xr_set_filter(XrSet *set, XrayIsolate *isolate, struct XrClosure *callback) {
    XrSet *new_set = xr_set_new(xr_current_coro(isolate));

    if (set->entries == NULL || callback == NULL) {
        return new_set;
    }

    // Traverse all valid entries
    for (uint32_t i = 0; i < set->capacity; i++) {
        XrSetEntry *entry = &set->entries[i];
        if (entry_is_valid(entry)) {
            // Call callback: callback(value)
            XrValue args[1];
            args[0] = entry->value;
            XrValue result = xr_vm_call_closure(isolate, callback, args, 1);

            // If truthy, add to new Set
            if (xr_vm_is_truthy(result)) {
                xr_set_add(new_set, entry->value);
            }
        }
    }

    return new_set;
}

/* ========== Set Operations ========== */

XrSet *xr_set_union(struct XrCoroutine *coro, XrSet *set1, XrSet *set2) {
    XR_DCHECK(coro != NULL, "set_union: NULL coro");
    XR_DCHECK(set1 != NULL, "set_union: NULL set1");
    XR_DCHECK(set2 != NULL, "set_union: NULL set2");
    XrSet *result = xr_set_new_with_capacity(coro, set1->count + set2->count);

    // Add all elements from set1
    if (set1->entries != NULL) {
        for (uint32_t i = 0; i < set1->capacity; i++) {
            XrSetEntry *entry = &set1->entries[i];
            if (entry_is_valid(entry)) {
                xr_set_add(result, entry->value);
            }
        }
    }

    // Add all elements from set2
    if (set2->entries != NULL) {
        for (uint32_t i = 0; i < set2->capacity; i++) {
            XrSetEntry *entry = &set2->entries[i];
            if (entry_is_valid(entry)) {
                xr_set_add(result, entry->value);
            }
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

    if (smaller->entries != NULL) {
        for (uint32_t i = 0; i < smaller->capacity; i++) {
            XrSetEntry *entry = &smaller->entries[i];
            if (entry_is_valid(entry)) {
                // Check if in larger set
                if (xr_set_has(larger, entry->value)) {
                    xr_set_add(result, entry->value);
                }
            }
        }
    }

    return result;
}

XrSet *xr_set_difference(struct XrCoroutine *coro, XrSet *set1, XrSet *set2) {
    XR_DCHECK(coro != NULL, "set_difference: NULL coro");
    XR_DCHECK(set1 != NULL, "set_difference: NULL set1");
    XR_DCHECK(set2 != NULL, "set_difference: NULL set2");
    XrSet *result = xr_set_new_with_capacity(coro, set1->count);

    // Add elements in set1 but not in set2
    if (set1->entries != NULL) {
        for (uint32_t i = 0; i < set1->capacity; i++) {
            XrSetEntry *entry = &set1->entries[i];
            if (entry_is_valid(entry)) {
                if (!xr_set_has(set2, entry->value)) {
                    xr_set_add(result, entry->value);
                }
            }
        }
    }

    return result;
}

XrSet *xr_set_symmetric_difference(struct XrCoroutine *coro, XrSet *set1, XrSet *set2) {
    XR_DCHECK(coro != NULL, "set_symmetric_difference: NULL coro");
    XR_DCHECK(set1 != NULL, "set_symmetric_difference: NULL set1");
    XR_DCHECK(set2 != NULL, "set_symmetric_difference: NULL set2");
    XrSet *result = xr_set_new_with_capacity(coro, set1->count + set2->count);

    // Add elements in set1 but not in set2
    if (set1->entries != NULL) {
        for (uint32_t i = 0; i < set1->capacity; i++) {
            XrSetEntry *entry = &set1->entries[i];
            if (entry_is_valid(entry)) {
                if (!xr_set_has(set2, entry->value)) {
                    xr_set_add(result, entry->value);
                }
            }
        }
    }

    // Add elements in set2 but not in set1
    if (set2->entries != NULL) {
        for (uint32_t i = 0; i < set2->capacity; i++) {
            XrSetEntry *entry = &set2->entries[i];
            if (entry_is_valid(entry)) {
                if (!xr_set_has(set1, entry->value)) {
                    xr_set_add(result, entry->value);
                }
            }
        }
    }

    return result;
}

bool xr_set_is_subset(XrSet *set1, XrSet *set2) {
    XR_DCHECK(set1 != NULL, "set_is_subset: NULL set1");
    XR_DCHECK(set2 != NULL, "set_is_subset: NULL set2");
    // Empty set is subset of any set
    if (set1->count == 0) {
        return true;
    }

    // If set1 is larger than set2, cannot be subset
    if (set1->count > set2->count) {
        return false;
    }

    // Check if all elements of set1 are in set2
    if (set1->entries != NULL) {
        for (uint32_t i = 0; i < set1->capacity; i++) {
            XrSetEntry *entry = &set1->entries[i];
            if (entry_is_valid(entry)) {
                if (!xr_set_has(set2, entry->value)) {
                    return false;
                }
            }
        }
    }

    return true;
}

bool xr_set_is_superset(XrSet *set1, XrSet *set2) {
    return xr_set_is_subset(set2, set1);
}

/* ========== GC Integration ========== */

void xr_gc_destroy_set(XrGCHeader *obj, struct XrCoroGC *owning_gc) {
    XrSet *set = (XrSet *) obj;
    if (set->entries) {
        size_t bytes = sizeof(XrSetEntry) * set->capacity;
        xr_free(set->entries);
        set->entries = NULL;
        // Balance the add_external from xr_set_resize so totalbytes
        // returns to the correct value when the set is reclaimed.
        xr_gc_sub_external(owning_gc, (int64_t) bytes);
    }
}
