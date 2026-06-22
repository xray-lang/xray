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
#include "../../shared/xr_map_set_abi.h"
#include <stdint.h>
#include <stdbool.h>

/* ========== Set Object ========== */

typedef struct XrSet {
    XrGCHeader gc;
    struct XrCoroGC *owner_gc;
    XR_SET_ABI_FIELDS;
} XrSet;

// Macros
#define xr_set_entry(s, i) (&(s)->entries[i])

/* ========== Basic Operations ========== */

struct XrCoroutine;
struct XrCoroGC;
XR_FUNC XrSet *xr_set_new(struct XrCoroutine *coro);
XR_FUNC XrSet *xr_set_new_with_capacity(struct XrCoroutine *coro, uint32_t capacity);
XR_FUNC void xr_set_init_inplace(XrSet *set);
struct XrArray;
XR_FUNC XrSet *xr_set_from_array(struct XrCoroutine *coro, struct XrArray *arr);
XR_FUNC bool xr_set_add(XrSet *set, XrValue value);
XR_FUNC bool xr_set_has(XrSet *set, XrValue value);
XR_FUNC bool xr_set_delete(XrSet *set, XrValue value);
XR_FUNC uint32_t xr_set_purge_weak_target(XrSet *set, XrGCHeader *target);
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
