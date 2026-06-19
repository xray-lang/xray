/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_coro_lower.h - Shared coroutine CFG lowering for Xi IR
 *
 * KEY CONCEPT:
 *   Consumes the backend-neutral XiCoroPlan (xi_coro_analyze) and rewrites a
 *   suspendable function's control-flow graph into an explicit stackless state
 *   machine: an entry dispatch that jumps to the active suspend state, each
 *   suspend point split into "spill live set -> store state -> suspend", and an
 *   explicit coroutine frame carrying the values that survive a suspend.
 *
 *   Both the AOT compiler and (later) the VM consume the lowered IR, so the
 *   state machine is identical by construction.  The pass runs after ownership
 *   (xi_arc) and before representation selection so the frame slot operations it
 *   introduces are assigned representations by the normal select_rep pass.
 *
 *   The two genuinely context-dependent queries -- interprocedural callee
 *   resolution and stdlib module-import recognition -- are supplied through the
 *   XiCoroResolver, keeping this pass free of any AOT/VM bundle types.
 */

#ifndef XI_CORO_LOWER_H
#define XI_CORO_LOWER_H

#include "xi.h"
#include "xi_coro_analyze.h"

/* Lower every suspendable function in the tree rooted at 'f' into an explicit
 * stackless state machine, then advance the tree to XI_STAGE_CORO_LOWERED.
 * Non-suspendable functions are left structurally unchanged.  'resolver'
 * supplies the interprocedural / module-import queries (see XiCoroResolver);
 * it may be NULL for intraprocedural-only lowering.
 *
 * Requires: f at XI_STAGE_OWNED (ownership inserted, representations not yet
 * selected).  Returns true on success, false on a NULL function or allocation
 * failure. */
XR_FUNC bool xi_coro_lower(XiFunc *f, const XiCoroResolver *resolver);

#endif  // XI_CORO_LOWER_H
