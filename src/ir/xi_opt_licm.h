/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_licm.h - Loop-Invariant Code Motion for Xi IR
 *
 * KEY CONCEPT:
 *   Hoist pure values whose operands are defined outside the loop
 *   into the loop's preheader block.  Operates on XiLoopInfo
 *   computed by xi_compute_loops(), processing innermost loops first.
 *   Iterates within each loop to handle chain-invariant propagation.
 */

#ifndef XI_OPT_LICM_H
#define XI_OPT_LICM_H

#include "xi.h"
#include "xi_pass.h"

/* Run LICM on the function.  Requires RPO, dominator tree,
 * and loop info (xi_compute_loops). */
XR_FUNC XiPassChange xi_opt_licm(XiFunc *f);

#endif  // XI_OPT_LICM_H
