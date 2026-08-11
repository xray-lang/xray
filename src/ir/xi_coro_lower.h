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
 *   Consumes the backend-neutral XiCoroPlan (xi_coro_analyze), partitions each
 *   suspension point into pre/suspend/resume CFG blocks, and records dense
 *   logical dispatch, spill, root, drop, and child-continuation obligations.
 *   TargetPlan must later select executable frame operations, scheduler exits,
 *   and entry dispatch; this pass deliberately does not freeze physical ABI.
 *
 *   The plan is target-neutral so AOT and VM TargetPlans can consume the same
 *   logical machine.  The pass runs after ownership (xi_arc) and before
 *   representation selection.
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
 * stackless state machine and record per-function XiLoweringFacts.
 * Non-suspendable functions are left structurally unchanged.  'resolver'
 * supplies the interprocedural / module-import queries (see XiCoroResolver);
 * it may be NULL for intraprocedural-only lowering.
 *
 * Requires ownership-explicit semantic IR; the consuming stage API owns stage
 * advancement. Exception regions are rejected until their handler/defer
 * continuation contract is represented explicitly. Returns true on success,
 * false on a NULL function, unsupported exception region, or allocation
 * failure. */
XR_FUNC bool xi_coro_lower(XiFunc *f, const XiCoroResolver *resolver);

#endif  // XI_CORO_LOWER_H
