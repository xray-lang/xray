/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_inline.h - Function Inlining for Xi IR
 *
 * KEY CONCEPT:
 *   Inline small, known-target function calls at the Xi IR level.
 *   A call site is eligible when:
 *     1. Callee is a direct closure (XI_CLOSURE_NEW with known XiFunc)
 *     2. Callee body is small (≤ cost threshold)
 *     3. No recursion (callee != caller)
 *
 *   Inlining clones callee blocks into the caller, maps params
 *   to call arguments, and replaces return blocks with jumps to
 *   the continuation.  The cloned values are arena-allocated in
 *   the caller's arena so no separate free is needed.
 */

#ifndef XI_OPT_INLINE_H
#define XI_OPT_INLINE_H

#include "xi.h"
#include "xi_pass.h"

/* ========== Cost Model ========== */

/* Callee-intrinsic cost factors. */
typedef struct XiInlineCostModel {
    uint32_t value_count;  /* total values (incl. phis) */
    uint32_t call_count;   /* internal call instructions */
    uint32_t branch_count; /* IF/switch blocks */
    bool has_loop;         /* contains a back-edge */
    bool calls_self;       /* recursive (must never inline) */
    bool has_throw;        /* contains XI_THROW */
} XiInlineCostModel;

/* Call-site specific information (used to boost or penalize). */
typedef struct XiInlineCallSiteInfo {
    bool all_args_const;   /* all arguments are XI_CONST */
    bool single_call_site; /* callee only called once in whole program */
    uint32_t caller_size;  /* current caller value count (caps growth) */
} XiInlineCallSiteInfo;

/* Compute the inline benefit score.
 * Positive = beneficial to inline.  Decision: inline if benefit > 0. */
XR_FUNC int xi_inline_benefit(const XiInlineCostModel *cost, const XiInlineCallSiteInfo *site);

/* Per-pass inline budget scaled by caller's current value count.
 *   < 100        -> XI_INLINE_MAX_PER_PASS + 2  (small caller, cheap to grow)
 *   100..300     -> XI_INLINE_MAX_PER_PASS      (default)
 *   > 300        -> XI_INLINE_MAX_PER_PASS - 2  (large caller, cap growth)
 *
 * The xi_inline_benefit() function additionally penalises callers >300 for
 * general callees. Leaf straight-line helpers are scored by callee size only:
 * they are the hot-loop shape where call overhead is usually more expensive
 * than controlled code growth, while this per-pass budget still caps growth. */
XR_FUNC uint32_t xi_inline_budget(uint32_t caller_size);

/* Absolute upper bound on callee size (never inline above this). */
#define XI_INLINE_MAX_COST 60

/* Soft threshold for benefit calculation base penalty. */
#define XI_INLINE_BASE_THRESHOLD 30

/* Default per-pass inlines (used as the medium-caller anchor of
 * xi_inline_budget()). */
#define XI_INLINE_MAX_PER_PASS 4

/* Run inlining on the function.  May inline multiple call sites. */
XR_FUNC XiPassChange xi_opt_inline(XiFunc *f);

#endif  // XI_OPT_INLINE_H
