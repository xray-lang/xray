/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_sccp.h - Sparse Conditional Constant Propagation for Xi IR
 *
 * KEY CONCEPT:
 *   Combines constant folding, unreachable-block elimination, and
 *   branch simplification in a single Wegman-Zadeck fixed-point
 *   computation.  Strictly stronger than running constfold + copy_prop
 *   + DCE separately because phi-meet is conditional on edge
 *   reachability.
 */

#ifndef XI_OPT_SCCP_H
#define XI_OPT_SCCP_H

#include "xi.h"
#include "xi_pass.h"

/* Run SCCP on the function.  Rewrites constants in-place, simplifies
 * branches with known conditions, and removes unreachable blocks. */
XR_FUNC XiPassChange xi_opt_sccp(XiFunc *f);

#endif  // XI_OPT_SCCP_H
