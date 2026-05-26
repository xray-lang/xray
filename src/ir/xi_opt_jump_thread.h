/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_jump_thread.h - Jump threading for Xi IR
 *
 * Threads predictable jumps through intermediate blocks.  When a
 * conditional branch in block D tests a condition that is already
 * known along the incoming edge (e.g. because a dominating branch
 * in A tested the same condition), the edge can be redirected to
 * skip D and go directly to the known-true successor.
 *
 * Example:
 *   A: if (x==0) goto B else goto C
 *   B: ...; goto D
 *   D: if (x==0) goto E else goto F
 *   =>  B's goto D becomes goto E  (x is known zero along A->B->D)
 */

#ifndef XI_OPT_JUMP_THREAD_H
#define XI_OPT_JUMP_THREAD_H

#include "xi.h"
#include "xi_pass.h"

/* Thread predictable jumps.  Requires dominator tree (XI_PASS_NEEDS_DOM).
 * Uses SCCP lattice values when available to determine known conditions. */
XR_FUNC XiPassChange xi_opt_jump_thread(XiFunc *f);

#endif  // XI_OPT_JUMP_THREAD_H
