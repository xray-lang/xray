/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_gvn_pre.h - Global Value Numbering with Partial Redundancy
 *                    Elimination for Xi IR
 *
 * KEY CONCEPT:
 *   Value-number-based redundancy elimination over the dominator tree.
 *   Two SSA values with the same opcode, equivalent operands (by VN,
 *   commutativity-normalized) and matching type / aux key share a value
 *   number. The pass performs:
 *
 *     1. Full Redundancy Elimination — when a leader with the same VN
 *        dominates the redundant use, replace the use with COPY(leader).
 *     2. Partial Redundancy Elimination — at multi-predecessor blocks,
 *        materialize a join phi over per-edge leaders, inserting a
 *        clone on edges where the value is missing (PLAIN single-succ
 *        predecessors only; never split critical edges; never speculate
 *        memory loads).
 *
 *   At least one predecessor must already provide the value to keep the
 *   transform anticipability-bounded.
 */

#ifndef XI_OPT_GVN_PRE_H
#define XI_OPT_GVN_PRE_H

#include "xi.h"
#include "xi_pass.h"

/* Run GVN-PRE on the function. Requires dominator tree computed
 * (xi_compute_rpo + xi_compute_dominators). */
XR_FUNC XiPassChange xi_opt_gvn_pre(XiFunc *f);

#endif  // XI_OPT_GVN_PRE_H
