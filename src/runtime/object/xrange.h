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
 * MEMORY LAYOUT:
 *   ┌─────────────────────┐
 *   │ XrGCHeader (16B)    │
 *   ├─────────────────────┤
 *   │ start   (8B)        │ inclusive start
 *   │ end     (8B)        │ inclusive end
 *   │ step    (8B)        │ step (default 1, negative for reverse)
 *   └─────────────────────┘
 *   Total: 40 bytes (no GC-traced children)
 */

#ifndef XRANGE_H
#define XRANGE_H

#include "../gc/xgc_header.h"
#include "../value/xvalue.h"
#include <stdint.h>
#include <stdbool.h>

struct XrCoroutine;

/* ========== Range Structure ========== */

typedef struct XrRange {
    XrGCHeader gc;
    int64_t start;
    int64_t end;
    int64_t step;
} XrRange;

/* ========== Creation ========== */

// Create Range [start, end] with step=1
XR_FUNC XrRange *xr_range_new(struct XrCoroutine *coro, int64_t start, int64_t end);

// Create Range [start, end] with explicit step
XR_FUNC XrRange *xr_range_new_with_step(struct XrCoroutine *coro, int64_t start, int64_t end,
                                        int64_t step);

/* ========== Properties ========== */

// Number of elements in the range (lazy, O(1))
static inline int64_t xr_range_length(XrRange *r) {
    if (!r || r->step == 0)
        return 0;
    if (r->step > 0) {
        return (r->end >= r->start) ? (r->end - r->start) / r->step + 1 : 0;
    } else {
        return (r->start >= r->end) ? (r->start - r->end) / (-r->step) + 1 : 0;
    }
}

// Check if value is within the range
static inline bool xr_range_contains(XrRange *r, int64_t value) {
    if (!r || r->step == 0)
        return false;
    if (r->step > 0) {
        if (value < r->start || value > r->end)
            return false;
        return (value - r->start) % r->step == 0;
    } else {
        if (value > r->start || value < r->end)
            return false;
        return (r->start - value) % (-r->step) == 0;
    }
}

/* ========== XrValue Conversion ========== */

static inline XrValue xr_value_from_range(XrRange *r) {
    return XR_FROM_PTR(r);
}

static inline XrRange *xr_value_to_range(XrValue v) {
    return (XrRange *) XR_TO_PTR(v);
}

/* ========== Conversion ========== */

// Materialize range into an Array (caller must handle large ranges)
XR_FUNC XrValue xr_range_to_array(struct XrCoroutine *coro, XrRange *r);

#endif  // XRANGE_H
