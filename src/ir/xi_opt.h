/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt.h - SSA optimization passes for Xi IR
 *
 * KEY CONCEPT:
 *   Each pass is a function XiFunc* -> void that mutates the IR in place.
 *   Passes are composable and order-independent unless noted.
 *
 *   Constant folding evaluates pure arithmetic at compile time.
 *   Dead code elimination removes unused values.
 *   Copy propagation eliminates XI_COPY chains.
 *   Phi simplification collapses trivial phis.
 *
 * INVARIANTS (maintained by all passes):
 *   1. SSA form preserved: each XiValue has one definition.
 *   2. Type invariants preserved: value->type is never set to NULL.
 *   3. Block structure preserved: no blocks are added or removed.
 *      (Dead blocks may have their values cleared.)
 */

#ifndef XI_OPT_H
#define XI_OPT_H

#include "xi.h"

/* ========== Individual Passes ========== */

/* Constant folding: evaluate pure arithmetic/comparison on constants.
 * Replaces binary/unary ops on constant args with the result constant.
 * Single forward pass over each block. O(n) in value count. */
XR_FUNC void xi_opt_const_fold(XiFunc *f);

/* Copy propagation: replace XI_COPY chains with their source.
 * Rewrites all uses of a COPY value to use the original source.
 * Single forward pass. O(n) in value count. */
XR_FUNC void xi_opt_copy_prop(XiFunc *f);

/* Dead code elimination: remove values with zero uses.
 * Computes use counts, then iteratively removes dead values.
 * Values with XI_FLAG_SIDE_EFFECT are never removed.
 * O(n) amortized over all values. */
XR_FUNC void xi_opt_dce(XiFunc *f);

/* Phi simplification: collapse trivial phi nodes.
 * A phi is trivial if all operands are the same value (or self).
 * Replaces trivial phis with their unique operand. */
XR_FUNC void xi_opt_phi_simplify(XiFunc *f);

/* ========== Combined Pass ========== */

/* Run all optimization passes in the recommended order:
 *   1. Constant folding
 *   2. Copy propagation
 *   3. Phi simplification
 *   4. Dead code elimination
 * Safe to call multiple times (idempotent after convergence). */
XR_FUNC void xi_opt_run(XiFunc *f);

#endif  // XI_OPT_H
