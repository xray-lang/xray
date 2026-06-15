/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xset.h - Compact ordered hash set (CPython-style compact dict)
 *
 * KEY CONCEPT:
 *   - Insertion-ordered: entries[] is a dense, append-only array kept in the
 *     order values were first added; iteration scans it directly, so order
 *     matches the Map/Set iteration contract (insertion order).
 *   - ctrl[] is a Swiss-style h2 control-byte table; indices[] stores the
 *     corresponding entries[] index for FULL ctrl slots.
 *   - Deletion tombstones the entry (val_tt = 0) and marks its ctrl slot
 *     DELETED; dead entries are reclaimed when the table is resized/compacted.
 *   - Empty set allocates nothing (entries/indices NULL, DUMMY flag set); the
 *     first add allocates both arrays.
 */

#ifndef XSET_H
#define XSET_H

#include "../value/xvalue.h"
#include "../gc/xgc_header.h"
#include "xarray.h"
#include <stdint.h>
#include <stdbool.h>

/* ========== Set Entry (insertion-order dense slot) ========== */

typedef struct XrSetEntry {
    XrValue value;
    uint32_t hash;   // Cached value hash (avoids recompute on resize/lookup)
    uint8_t val_tt;  // Value type tag (+1); 0 = empty/tombstone slot
    uint8_t _pad[3];
} XrSetEntry;

// Entry state
#define XR_SET_ENTRY_NIL 0
#define XR_SET_ENTRY_EMPTY(e) ((e)->val_tt == XR_SET_ENTRY_NIL)

/* Debug sentinel for indices[] slots whose ctrl byte is not FULL. FULL slots
 * always store a direct entries[] index. */
#define XR_SET_IX_EMPTY (-1)

/* ========== Set Object ========== */

typedef struct XrSet {
    XrGCHeader gc;
    uint32_t count;         // Live entries (excludes tombstones)
    uint32_t nentries;      // Used entry slots incl. tombstones (= next append index)
    uint32_t entries_cap;   // Allocated entries[] capacity
    uint32_t indices_size;  // indices[] slot count (power of two, 0 = dummy)
    uint8_t *ctrl;          // Swiss control bytes, indices_size + XR_SWISS_GROUP
    int32_t *indices;       // FULL ctrl slots -> entries index
    XrSetEntry *entries;    // Dense insertion-order array
    uint8_t flags;
    uint8_t elem_tid;  // XrTypeId: element type for reified generics (0=any)
    uint8_t _pad[2];   // Alignment
} XrSet;

// Macros
#define xr_set_entry(s, i) (&(s)->entries[i])

// Flags
#define XR_SET_FLAG_WEAK 0x01
#define XR_SET_FLAG_DUMMY 0x02        // Empty set: no entries/indices allocation
#define XR_SET_FLAG_NODES_ON_GC 0x04  // entries[]/indices[] live on Region GC heap

#define xr_set_isdummy(s) ((s)->flags & XR_SET_FLAG_DUMMY)

// Max index-table bits
#define XR_SET_MAXHBITS 30

/* ========== Basic Operations ========== */

struct XrCoroutine;
XR_FUNC XrSet *xr_set_new(struct XrCoroutine *coro);
XR_FUNC XrSet *xr_set_new_with_capacity(struct XrCoroutine *coro, uint32_t capacity);
XR_FUNC void xr_set_init_inplace(XrSet *set);
struct XrArray;
XR_FUNC XrSet *xr_set_from_array(struct XrCoroutine *coro, struct XrArray *arr);
XR_FUNC bool xr_set_add(XrSet *set, XrValue value);
XR_FUNC bool xr_set_has(XrSet *set, XrValue value);
XR_FUNC bool xr_set_delete(XrSet *set, XrValue value);
XR_FUNC void xr_set_clear(XrSet *set);
XR_FUNC uint32_t xr_set_size(XrSet *set);
XR_FUNC bool xr_set_is_empty(XrSet *set);

/* ========== Set Operations ========== */

XR_FUNC XrSet *xr_set_union(struct XrCoroutine *coro, XrSet *set1, XrSet *set2);
XR_FUNC XrSet *xr_set_intersection(struct XrCoroutine *coro, XrSet *set1, XrSet *set2);
XR_FUNC XrSet *xr_set_difference(struct XrCoroutine *coro, XrSet *set1, XrSet *set2);
XR_FUNC XrSet *xr_set_symmetric_difference(struct XrCoroutine *coro, XrSet *set1, XrSet *set2);
XR_FUNC bool xr_set_is_subset(XrSet *set1, XrSet *set2);
XR_FUNC bool xr_set_is_superset(XrSet *set1, XrSet *set2);

/* ========== Iteration Methods ========== */

XR_FUNC XrArray *xr_set_values(struct XrCoroutine *coro, XrSet *set);

#endif  // XSET_H
