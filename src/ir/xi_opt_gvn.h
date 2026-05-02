/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_gvn.h - Global Value Numbering for Xi IR
 *
 * KEY CONCEPT:
 *   Hash-based redundancy elimination.  Two values with the same
 *   opcode and identical operands (by pointer identity) are considered
 *   equivalent.  The earlier definition replaces later duplicates when
 *   it dominates them.  Commutative operands are normalized before
 *   hashing to improve hit rate.
 */

#ifndef XI_OPT_GVN_H
#define XI_OPT_GVN_H

#include "xi.h"
#include "xi_pass.h"

/* Run GVN on the function.  Requires dominator tree computed
 * (xi_compute_rpo + xi_compute_dominators). */
XR_FUNC XiPassChange xi_opt_gvn(XiFunc *f);

#endif // XI_OPT_GVN_H
