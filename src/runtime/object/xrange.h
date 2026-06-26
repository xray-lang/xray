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
 *   XrInstance header + 0 fields + native body:
 *   ┌─────────────────────┐
 *   │ XrInstance base     │
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

#include "../value/xvalue.h"
#include <stdint.h>
#include <stdbool.h>

struct XrCoroutine;
struct XrVMRuntime;
struct XrInstance;

/* ========== Range Native Body ========== */

typedef struct XrRange {
    int64_t start;
    int64_t end;
    int64_t step;
    bool inclusive_end;
} XrRange;

/* ========== Creation ========== */

// Create Range with step=1.
XR_FUNC XrValue xr_range_new(struct XrVMRuntime *X, int64_t start, int64_t end, bool inclusive_end);

// Create Range with explicit step.
XR_FUNC XrValue xr_range_new_with_step(struct XrVMRuntime *X, int64_t start, int64_t end,
                                       int64_t step, bool inclusive_end);

/* ========== Type Check ========== */

// Check if value is a Range instance (instanceof core->rangeClass)
XR_FUNC bool xr_value_is_range(struct XrVMRuntime *X, XrValue v);

// Extract native body pointer from a Range value (NULL if not range)
XR_FUNC XrRange *xr_value_get_range_body(struct XrVMRuntime *X, XrValue v);

/* ========== Properties ========== */

// Number of elements in the range (lazy, O(1)).
// Half-open forward ranges use `[start, end)`, inclusive forward ranges use
// `[start, end]`; reverse ranges mirror the boundary around `end`.
static inline int64_t xr_range_len_from_distance(uint64_t distance, uint64_t step,
                                                 bool inclusive_end) {
    if (step == 0)
        return 0;
    uint64_t base = distance / step;
    uint64_t extra = inclusive_end ? 1 : (distance % step != 0);
    if (base > UINT64_MAX - extra)
        return INT64_MAX;
    uint64_t len = base + extra;
    return len > (uint64_t) INT64_MAX ? INT64_MAX : (int64_t) len;
}

static inline int64_t xr_range_length(XrRange *r) {
    if (!r || r->step == 0)
        return 0;
    if (r->step > 0) {
        if (r->inclusive_end ? (r->end < r->start) : (r->end <= r->start))
            return 0;
        return xr_range_len_from_distance((uint64_t) r->end - (uint64_t) r->start,
                                          (uint64_t) r->step, r->inclusive_end);
    } else {
        if (r->inclusive_end ? (r->end > r->start) : (r->end >= r->start))
            return 0;
        int64_t neg_step = -r->step;
        return xr_range_len_from_distance((uint64_t) r->start - (uint64_t) r->end,
                                          (uint64_t) neg_step, r->inclusive_end);
    }
}

// Check if value is in the range under the stored end-boundary semantics.
static inline bool xr_range_contains(XrRange *r, int64_t value) {
    if (!r || r->step == 0)
        return false;
    if (r->step > 0) {
        if (value < r->start || (r->inclusive_end ? value > r->end : value >= r->end))
            return false;
        return ((uint64_t) value - (uint64_t) r->start) % (uint64_t) r->step == 0;
    } else {
        if (value > r->start || (r->inclusive_end ? value < r->end : value <= r->end))
            return false;
        return ((uint64_t) r->start - (uint64_t) value) % (uint64_t) (-r->step) == 0;
    }
}

/* ========== XrValue Conversion (legacy compatibility — will be removed) ========== */

static inline XrRange *xr_value_to_range(XrValue v) {
    /* After migration the ptr IS an XrInstance; callers that still use
     * this helper get the instance pointer, not the body. They must be
     * migrated to xr_value_get_range_body(). */
    return (XrRange *) XR_TO_PTR(v);
}

/* ========== Conversion ========== */

// Materialize range into an Array (caller must handle large ranges)
XR_FUNC XrValue xr_range_to_array(struct XrCoroutine *coro, XrRange *r);

/* Register Range class into core->rangeClass with native body. */
XR_FUNC void xr_register_range_class(struct XrVMRuntime *X);

#endif  // XRANGE_H
