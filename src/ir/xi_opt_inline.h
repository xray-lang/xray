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

/* Maximum callee cost (sum of values across all blocks) for inlining. */
#define XI_INLINE_MAX_COST 30

/* Run inlining on the function.  May inline multiple call sites. */
XR_FUNC XiPassChange xi_opt_inline(XiFunc *f);

#endif  // XI_OPT_INLINE_H
