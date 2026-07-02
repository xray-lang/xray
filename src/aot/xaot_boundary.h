/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_boundary.h - explicit AOT representation boundary plan
 */

#ifndef XAOT_BOUNDARY_H
#define XAOT_BOUNDARY_H

#include "xaot_rep.h"
#include "../ir/xi.h"
#include "../base/xdefs.h"
#include <stdint.h>

typedef enum XaotBoundaryReason {
    XAOT_BOUNDARY_NONE = 0,
    XAOT_BOUNDARY_DIRECT_CALL,
    XAOT_BOUNDARY_DYNAMIC_CALL,
    XAOT_BOUNDARY_CLOSURE_OBJECT,
    XAOT_BOUNDARY_MODULE_INIT,
    XAOT_BOUNDARY_EXCEPTION_FLOW,
    XAOT_BOUNDARY_CORO_FRAME,
    XAOT_BOUNDARY_TAGGED_TYPE,
    XAOT_BOUNDARY_BOX,
    XAOT_BOUNDARY_UNBOX,
    XAOT_BOUNDARY_SHARED_SLOT,
    XAOT_BOUNDARY_IMPORT_EXPORT,
    XAOT_BOUNDARY_REFLECTION,
    XAOT_BOUNDARY_UNION_NULLABLE,
    XAOT_BOUNDARY_RUNTIME_HELPER,
    XAOT_BOUNDARY_CORO_RESULT,
} XaotBoundaryReason;

typedef enum XaotBoundaryStepKind {
    XAOT_BOUNDARY_STEP_FUNC_ABI = 0,
    XAOT_BOUNDARY_STEP_VALUE_REP,
    XAOT_BOUNDARY_STEP_DIRECT_CALL_ARG,
    XAOT_BOUNDARY_STEP_DIRECT_CALL_RET,
} XaotBoundaryStepKind;

typedef struct XaotBoundaryStep {
    XaotBoundaryStepKind kind;
    const XiFunc *func;
    const XiFunc *target_func;
    const XiValue *value;
    const XiValue *input;
    XaotValueRep from_rep;
    XaotValueRep to_rep;
    XaotBoundaryReason reason;
    uint16_t arg_index;
} XaotBoundaryStep;

XR_FUNC const char *xaot_boundary_reason_name(XaotBoundaryReason reason);
XR_FUNC const char *xaot_boundary_step_kind_name(XaotBoundaryStepKind kind);

struct XaotBundle;
/* Resolves the statically-known callee of a direct call, when there is one.
 * `*first_arg_out` receives the index of the first call operand that maps to
 * a target ABI parameter: 1 for XI_CALL (args[0] is the callee) and for
 * module-member calls (args[0] is the namespace object, which the target
 * function never receives), 0 for real method calls (args[0] is `this`). */
XR_FUNC const XiFunc *xaot_boundary_resolve_direct_call_target(const struct XaotBundle *bundle,
                                                               const XiFunc *current,
                                                               const XiValue *call,
                                                               uint16_t *first_arg_out);

#endif  // XAOT_BOUNDARY_H
