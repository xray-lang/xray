/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrange.h - Lightweight lazy Range type
 *
 * KEY CONCEPT:
 *   Range represents a lazy integer sequence with either half-open or
 *   inclusive end semantics.
 *   No elements are materialized until iteration or toArray().
 *
 * MEMORY LAYOUT (unified class model):
 *   XrObjectInstance header + 0 fields + native body:
 *   ┌─────────────────────┐
 *   │ XrObjectInstance base     │
 *   ├─────────────────────┤
 *   │ start   (8B)        │ inclusive start
 *   │ end     (8B)        │ stored end bound
 *   │ step    (8B)        │ step (default 1, negative for reverse)
 *   │ inclusive_end       │ false: [start,end), true: [start,end]
 *   └─────────────────────┘
 *   Native body: 24 bytes (no GC-traced children, no destroy)
 */

#ifndef XRANGE_H
#define XRANGE_H

#include "../../shared/xr_range_core.h"
#include "../value/xvalue.h"
#include <stdint.h>
#include <stdbool.h>

struct XrCoroutine;
struct XrVMRuntime;
struct XrObjectInstance;

/* ========== Range Native Body ========== */

typedef struct XrRange {
    int64_t start;
    int64_t end;
    int64_t step;
    bool inclusive_end;
} XrRange;

/* ========== Creation ========== */

// Allocate the VM representation of an already-decided shared Range value.
XR_FUNC XrValue xr_range_from_core(struct XrVMRuntime *X, XrRangeCore core);

/* ========== Type Check ========== */

// Check if value is a Range instance (instanceof core->rangeClass)
XR_FUNC bool xr_value_is_range(struct XrVMRuntime *X, XrValue v);

// Extract native body pointer from a Range value (NULL if not range)
XR_FUNC XrRange *xr_value_get_range_body(struct XrVMRuntime *X, XrValue v);

/* ========== Properties ========== */

// Number of elements in the range (lazy, O(1)).
// Half-open forward ranges use `[start, end)`, inclusive forward ranges use
// `[start, end]`; reverse ranges mirror the boundary around `end`.
static inline XrRangeCore xr_range_core_view(const XrRange *r) {
    return r ? xr_range_core_make_with_bound(r->start, r->end, r->step, r->inclusive_end)
             : xr_range_core_make(0, 0, 0);
}

static inline int64_t xr_range_length(const XrRange *r) {
    return xr_range_core_length(xr_range_core_view(r));
}

// Check if value is in the range under the stored end-boundary semantics.
static inline bool xr_range_contains(const XrRange *r, int64_t value) {
    return xr_range_core_contains(xr_range_core_view(r), value);
}

/* ========== Conversion ========== */

// Materialize range into an Array, or raise a catchable panic if the range
// exceeds the shared materialization cap (XR_RANGE_CORE_MATERIALIZE_MAX).
XR_FUNC XrValue xr_range_to_array(struct XrVMRuntime *X, XrRange *r);

/* Register Range class into core->rangeClass with native body. */
XR_FUNC void xr_register_range_class(struct XrVMRuntime *X);

#endif  // XRANGE_H
