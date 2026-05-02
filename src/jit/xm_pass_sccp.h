/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_pass_sccp.h - Sparse Conditional Constant Propagation pass.
 */

#ifndef XM_PASS_SCCP_H
#define XM_PASS_SCCP_H

#include "xm_pass.h"

/*
 * Run Sparse Conditional Constant Propagation on |func|.
 *
 * Performs constant folding, unreachable-block elimination, and
 * branch simplification in a single fixed-point computation.  This is
 * a drop-in replacement for the triple
 *
 *   xm_pass_const_prop + xm_pass_branch_simp + xm_pass_remove_unreachable
 *
 * and produces strictly at least as many folding opportunities as
 * that sequence (it is only stronger because phi-meet is conditional
 * on edge reachability).
 */
XR_FUNC XmPassChange xm_pass_sccp(XmFunc *func);

#endif  // XM_PASS_SCCP_H
