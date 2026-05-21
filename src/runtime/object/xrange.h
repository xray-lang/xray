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
 *   Range represents a lazy integer sequence [start, end] with step.
 *   No elements are materialized until iteration or toArray().
 *
 * MEMORY LAYOUT (unified class model):
 *   XrInstance header + 0 fields + native body:
 *   ┌─────────────────────┐
 *   │ XrInstance base     │
 *   ├─────────────────────┤
 *   │ start   (8B)        │ inclusive start
 *   │ end     (8B)        │ inclusive end
 *   │ step    (8B)        │ step (default 1, negative for reverse)
 *   └─────────────────────┘
 *   Native body: 24 bytes (no GC-traced children, no destroy)
 */

#ifndef XRANGE_H
#define XRANGE_H

#include "../value/xvalue.h"
#include <stdint.h>
#include <stdbool.h>

struct XrCoroutine;
struct XrayIsolate;
struct XrInstance;

/* ========== Range Native Body ========== */

typedef struct XrRange {
    int64_t start;
    int64_t end;
    int64_t step;
} XrRange;

/* ========== Creation ========== */

// Create Range [start, end] with step=1
XR_FUNC XrValue xr_range_new(struct XrayIsolate *X, int64_t start, int64_t end);

// Create Range [start, end] with explicit step
XR_FUNC XrValue xr_range_new_with_step(struct XrayIsolate *X, int64_t start, int64_t end,
                                       int64_t step);

/* ========== Type Check ========== */

// Check if value is a Range instance (instanceof core->rangeClass)
XR_FUNC bool xr_value_is_range(struct XrayIsolate *X, XrValue v);

// Extract native body pointer from a Range value (NULL if not range)
XR_FUNC XrRange *xr_value_get_range_body(struct XrayIsolate *X, XrValue v);

/* ========== Properties ========== */

// Number of elements in the range (lazy, O(1)).
// Range semantics is half-open `[start, end)` (matches spec §3.12 and
// the for-in / pattern lowering in xi_lower_stmt.c).  Negative step uses
// the symmetric `(end, start]` interpretation so iteration starts at
// `start` and stops strictly past `end`.
static inline int64_t xr_range_length(XrRange *r) {
    if (!r || r->step == 0)
        return 0;
    if (r->step > 0) {
        if (r->end <= r->start)
            return 0;
        // Number of strides i with start + i*step < end, i >= 0.
        return (r->end - r->start + r->step - 1) / r->step;
    } else {
        if (r->end >= r->start)
            return 0;
        int64_t neg_step = -r->step;
        return (r->start - r->end + neg_step - 1) / neg_step;
    }
}

// Check if value is in the range under half-open semantics.
// Forward (step > 0): value in [start, end) and (value - start) % step == 0.
// Reverse (step < 0): value in (end, start] and (start - value) % |step| == 0.
static inline bool xr_range_contains(XrRange *r, int64_t value) {
    if (!r || r->step == 0)
        return false;
    if (r->step > 0) {
        if (value < r->start || value >= r->end)
            return false;
        return (value - r->start) % r->step == 0;
    } else {
        if (value > r->start || value <= r->end)
            return false;
        return (r->start - value) % (-r->step) == 0;
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
XR_FUNC void xr_register_range_class(struct XrayIsolate *X);

#endif  // XRANGE_H
