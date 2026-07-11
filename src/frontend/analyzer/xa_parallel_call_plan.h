/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_parallel_call_plan.h - Analyzer-owned parallel intrinsic call identity
 *
 * The analyzer resolves whether a call is the stdlib `parallel` API (including
 * selective imports and `Plan<S>` methods).  Lowering consumes this table
 * instead of rediscovering the public symbol identity from text.
 */

#ifndef XA_PARALLEL_CALL_PLAN_H
#define XA_PARALLEL_CALL_PLAN_H

#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>

struct AstNode;

typedef enum XaParallelCallKind {
    XA_PAR_CALL_NONE = 0,
    XA_PAR_CALL_FOR_EACH,
    XA_PAR_CALL_MAP,
    XA_PAR_CALL_MAP_INTO,
    XA_PAR_CALL_REDUCE,
} XaParallelCallKind;

typedef struct XaParallelCallPlan {
    XaParallelCallKind kind;
    bool is_plan_method;
} XaParallelCallPlan;

typedef struct XaParallelCallPlanTable XaParallelCallPlanTable;

XR_FUNC XaParallelCallPlanTable *xa_parallel_call_plan_table_new(void);
XR_FUNC void xa_parallel_call_plan_table_free(XaParallelCallPlanTable *t);
XR_FUNC void xa_parallel_call_plan_table_clear(XaParallelCallPlanTable *t);
XR_FUNC int xa_parallel_call_plan_table_size(const XaParallelCallPlanTable *t);

XR_FUNC void xa_parallel_call_plan_table_set(XaParallelCallPlanTable *t, struct AstNode *node,
                                             const XaParallelCallPlan *plan);
XR_FUNC const XaParallelCallPlan *xa_parallel_call_plan_table_get(const XaParallelCallPlanTable *t,
                                                                  const struct AstNode *node);

XR_FUNC XaParallelCallKind xa_parallel_call_kind_from_name(const char *name);
XR_FUNC const char *xa_parallel_call_kind_name(XaParallelCallKind kind);

#endif  // XA_PARALLEL_CALL_PLAN_H
