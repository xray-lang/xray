/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_block_simplify.h - CFG block simplification for Xi IR
 *
 * Two transformations:
 *   1. Block merge: single-pred + single-succ → merge into predecessor
 *   2. Empty block elimination: block with no values and PLAIN kind →
 *      redirect predecessors to its successor
 *
 * Both transformations maintain phi correctness by updating pred arrays
 * and phi argument lists.
 */

#ifndef XI_OPT_BLOCK_SIMPLIFY_H
#define XI_OPT_BLOCK_SIMPLIFY_H

#include "xi.h"
#include "xi_pass.h"

/* Simplify CFG by merging trivial blocks and eliminating empty ones.
 * Safe to run at any point; preserves SSA and phi correctness. */
XR_FUNC XiPassChange xi_opt_block_simplify(XiFunc *f);

#endif  // XI_OPT_BLOCK_SIMPLIFY_H
