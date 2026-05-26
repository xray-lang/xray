/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_tail_call.h - Self-tail-call to loop transformation
 *
 * KEY CONCEPT:
 *   Detects self-recursive tail calls (XI_CALL where callee == self
 *   and the call result is immediately returned) and rewrites them
 *   as jumps back to the function entry with updated parameters.
 *   This eliminates stack growth for tail-recursive functions.
 *
 *   Pattern:
 *     blk: v = XI_CALL(self_closure, arg1, ..., argN)
 *          kind = RETURN, control = v
 *   Rewritten to:
 *     blk: params[i] = XI_COPY(arg_i)  ; update params
 *          kind = PLAIN, succs[0] = entry  ; loop back
 */

#ifndef XI_OPT_TAIL_CALL_H
#define XI_OPT_TAIL_CALL_H

#include "xi.h"
#include "xi_pass.h"

/* Transform self-tail-calls into loops.
 * Returns change record (cfg_changed if any tail call was converted). */
XR_FUNC XiPassChange xi_opt_tail_call(XiFunc *f);

#endif  // XI_OPT_TAIL_CALL_H
