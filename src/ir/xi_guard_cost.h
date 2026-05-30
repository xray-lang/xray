/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_guard_cost.h - Deoptimization cost model for speculative guards
 *
 * KEY CONCEPT:
 *   Each XI_GUARD_TYPE instruction carries an estimated deoptimization
 *   cost.  The cost model considers:
 *
 *     1. Observed miss rate (from IC metadata) — how often the guard
 *        fails and triggers deoptimization.
 *     2. Estimated recovery cost — the cost of reconstructing state
 *        in the interpreter after a deopt (proportional to live
 *        values at the guard point).
 *     3. Call-site frequency — how often this guard is executed.
 *
 *   Inline and specialization decisions consume this cost to avoid
 *   speculating on unstable patterns where deopt cost outweighs the
 *   optimization benefit.
 *
 * INVARIANT:
 *   XI_INV_GUARD_COST is set after xi_guard_cost_fill() completes.
 *   The pass is a no-op when no guards exist.
 */

#ifndef XI_GUARD_COST_H
#define XI_GUARD_COST_H

#include "xi.h"
#include "xi_pass.h"

/* Deoptimization cost for a single guard instruction. */
typedef struct XiGuardCost {
    float miss_rate;        /* 0.0 .. 1.0 — observed deopt frequency */
    uint32_t recovery_cost; /* estimated interpreter restart cost */
    uint32_t frequency;     /* call-site execution count */
    float expected_penalty; /* = miss_rate * recovery_cost * frequency */
} XiGuardCost;

/* Guard cost invariant bit. */
#define XI_INV_GUARD_COST ((XiInvariantMask) (1u << 13))

/* Fill guard cost metadata for all XI_GUARD_TYPE instructions.
 * Uses IC metadata (miss rates from xi_ic_lookup) and live-value
 * estimation.  Sets XI_INV_GUARD_COST on f->invariant_mask. */
XR_FUNC XiPassChange xi_guard_cost_fill(XiFunc *f);

/* Query: get the expected deopt penalty for a guard.
 * Returns 0.0 if the guard has no cost annotation. */
XR_FUNC float xi_guard_expected_penalty(const XiValue *guard);

/* Compute penalty on demand from IC data and guard position.
 * Does not require xi_guard_cost_fill() to have been called.
 * Returns miss_rate * recovery_cost.  Higher = riskier to speculate. */
XR_FUNC float xi_guard_compute_penalty(const XiFunc *f, const XiBlock *blk, uint32_t guard_pos,
                                       const XiValue *guard);

/* Decision helper: returns true if the guard's expected penalty is
 * below the speculation threshold (i.e., worth speculating). */
XR_FUNC bool xi_guard_should_speculate(const XiFunc *f, const XiBlock *blk, uint32_t guard_pos,
                                       const XiValue *guard);

#endif /* XI_GUARD_COST_H */
