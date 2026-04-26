/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_pass_sccp.h - Sparse Conditional Constant Propagation pass.
 */

#ifndef XIR_PASS_SCCP_H
#define XIR_PASS_SCCP_H

#include "xir_pass.h"

/*
 * Run Sparse Conditional Constant Propagation on |func|.
 *
 * Performs constant folding, unreachable-block elimination, and
 * branch simplification in a single fixed-point computation.  This is
 * a drop-in replacement for the triple
 *
 *   xir_pass_const_prop + xir_pass_branch_simp + xir_pass_remove_unreachable
 *
 * and produces strictly at least as many folding opportunities as
 * that sequence (it is only stronger because phi-meet is conditional
 * on edge reachability).
 */
XR_FUNC XirPassChange xir_pass_sccp(XirFunc *func);

#endif  // XIR_PASS_SCCP_H
