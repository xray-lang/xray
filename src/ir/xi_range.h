/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_range.h - Integer value range analysis for Xi IR
 *
 * Each integer SSA value carries a signed [lo, hi] interval.
 * The lattice is: TOP (unknown) -> [lo, hi] -> BOT (unreachable).
 * Range queries enable bounds check elimination, dead branch
 * removal, and loop induction variable reasoning.
 */

#ifndef XI_RANGE_H
#define XI_RANGE_H

#include "xi.h"
#include "xi_pass.h"
#include "../base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

/* ========== Range Lattice ========== */

typedef struct {
    int64_t lo;
    int64_t hi;
    bool is_top; /* unknown — no information */
    bool is_bot; /* unreachable — provably empty */
} XiRange;

/* Sentinel values for unbounded ranges. */
#define XI_RANGE_MIN INT64_MIN
#define XI_RANGE_MAX INT64_MAX

/* ========== Constructors ========== */

/* TOP: no information (default for unanalyzed values). */
static inline XiRange xi_range_top(void) {
    return (XiRange) {.lo = 0, .hi = 0, .is_top = true, .is_bot = false};
}

/* BOT: provably unreachable. */
static inline XiRange xi_range_bot(void) {
    return (XiRange) {.lo = 0, .hi = 0, .is_top = false, .is_bot = true};
}

/* Exact constant range [c, c]. */
static inline XiRange xi_range_const(int64_t c) {
    return (XiRange) {.lo = c, .hi = c, .is_top = false, .is_bot = false};
}

/* Bounded range [lo, hi].  Caller must ensure lo <= hi. */
static inline XiRange xi_range_make(int64_t lo, int64_t hi) {
    return (XiRange) {.lo = lo, .hi = hi, .is_top = false, .is_bot = false};
}

/* ========== Lattice Operations ========== */

/* Join (union): widen to cover both ranges.
 * Used at phi nodes and dataflow merge points. */
XR_FUNC XiRange xi_range_union(XiRange a, XiRange b);

/* Meet (intersect): narrow to the overlap.
 * Used at branch narrowing points. */
XR_FUNC XiRange xi_range_intersect(XiRange a, XiRange b);

/* ========== Queries ========== */

/* True if the range is a single constant. */
static inline bool xi_range_is_const(XiRange r) {
    return !r.is_top && !r.is_bot && r.lo == r.hi;
}

/* True if range is provably >= 0. */
XR_FUNC bool xi_range_known_nonneg(XiRange r);

/* True if range is provably > 0. */
XR_FUNC bool xi_range_known_positive(XiRange r);

/* True if range is provably < k. */
XR_FUNC bool xi_range_known_less_than(XiRange r, int64_t k);

/* True if range is provably >= k. */
XR_FUNC bool xi_range_known_ge(XiRange r, int64_t k);

/* True if 'inner' is entirely contained within 'outer'. */
XR_FUNC bool xi_range_contains(XiRange outer, XiRange inner);

/* ========== Arithmetic Transfer Functions ========== */

/* Compute range of (a + b), (a - b), (a * b) with overflow to TOP. */
XR_FUNC XiRange xi_range_add(XiRange a, XiRange b);
XR_FUNC XiRange xi_range_sub(XiRange a, XiRange b);
XR_FUNC XiRange xi_range_mul(XiRange a, XiRange b);

/* Negate: range of (-a). */
XR_FUNC XiRange xi_range_neg(XiRange a);

/* ========== IR Query ========== */

/* Get the range of a value (from analysis results stored in aux). */
XR_FUNC XiRange xi_range_of(const XiValue *v);

/* ========== Analysis Pass ========== */

/* Run range analysis on the function.  Annotates integer values with
 * ranges via the per-value aux storage.  Sets XI_INV_RANGE_ANNOTATED. */
XR_FUNC XiPassChange xi_range_analyze(XiFunc *f);

#endif  // XI_RANGE_H
