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
 *   Range represents a lazy integer sequence [start, end) with step.
 *   No elements are materialized until iteration or toArray().
 *
 * MEMORY LAYOUT (unified class model):
 *   XrInstance header + 0 fields + native body:
 *   ┌─────────────────────┐
 *   │ XrInstance base     │
 *   ├─────────────────────┤
 *   │ start   (8B)        │ inclusive start
 *   │ end     (8B)        │ exclusive end
 *   │ step    (8B)        │ step (default 1, negative for reverse)
 *   └─────────────────────┘
 *   Native body: 24 bytes (no GC-traced children, no destroy)
 */

#ifndef XRANGE_H
#define XRANGE_H

#include "../value/xvalue.h"
#include "../../shared/xr_range_core.h"
#include <stdbool.h>
#include <stdint.h>

struct XrCoroutine;
struct XrVMRuntime;
struct XrInstance;

/* ========== Range Native Body ========== */

typedef struct XrRange {
    int64_t start;
    int64_t end;
    int64_t step;
} XrRange;

/* ========== Creation ========== */

// Create Range [start, end] with step=1
XR_FUNC XrValue xr_range_new(struct XrVMRuntime *X, int64_t start, int64_t end);

// Create Range [start, end] with explicit step
XR_FUNC XrValue xr_range_new_with_step(struct XrVMRuntime *X, int64_t start, int64_t end,
                                       int64_t step);

/* ========== Type Check ========== */

// Check if value is a Range instance (instanceof core->rangeClass)
XR_FUNC bool xr_value_is_range(struct XrVMRuntime *X, XrValue v);

// Extract native body pointer from a Range value (NULL if not range)
XR_FUNC XrRange *xr_value_get_range_body(struct XrVMRuntime *X, XrValue v);

/* ========== Properties ========== */

// Number of elements in the range (lazy, O(1)).
// Range semantics is half-open `[start, end)` (matches spec §3.12 and
// the for-in / pattern lowering in xi_lower_stmt.c).  Negative step uses
// the symmetric `(end, start]` interpretation so iteration starts at
// `start` and stops strictly past `end`.
static inline XrRangeCore xr_range_core_from_body(const XrRange *r) {
    return xr_range_core_make(r->start, r->end, r->step);
}

static inline int64_t xr_range_length(const XrRange *r) {
    if (!r)
        return 0;
    return xr_range_core_length(xr_range_core_from_body(r));
}

// Check if value is in the range under half-open semantics.
// Forward (step > 0): value in [start, end) and (value - start) % step == 0.
// Reverse (step < 0): value in (end, start] and (start - value) % |step| == 0.
static inline bool xr_range_contains(const XrRange *r, int64_t value) {
    if (!r)
        return false;
    return xr_range_core_contains(xr_range_core_from_body(r), value);
}

static inline int64_t xr_range_index(const XrRange *r, int64_t index, bool *ok) {
    if (!r) {
        if (ok)
            *ok = false;
        return 0;
    }
    return xr_range_core_index(xr_range_core_from_body(r), index, ok);
}

static inline int xr_range_format_buf(const XrRange *r, char *buf, size_t cap) {
    if (!r)
        return snprintf(buf, cap, "<Range>");
    return xr_range_core_format_buf(xr_range_core_from_body(r), buf, cap);
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
