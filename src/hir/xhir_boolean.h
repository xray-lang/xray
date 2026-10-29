/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_boolean.h - Owned Bool qualifications without control-flow authority
 */
#ifndef XHIR_BOOLEAN_H
#define XHIR_BOOLEAN_H
#include "../xir/xxir.h"
#include "../base/xcompile_request.h"

typedef struct XrHirBooleanOrigin {
    uint32_t node_id,module;
    int line,column,end_line,end_column;
} XrHirBooleanOrigin;
typedef enum XrHirBooleanOperation {
    XR_HIR_BOOLEAN_INVALID,XR_HIR_BOOLEAN_NOT,XR_HIR_BOOLEAN_AND,XR_HIR_BOOLEAN_OR,
    XR_HIR_BOOLEAN_CONDITIONAL,XR_HIR_BOOLEAN_IF,XR_HIR_BOOLEAN_LOOP
} XrHirBooleanOperation;
typedef struct XrHirBooleanInput {
    XrHirBooleanOrigin origin;
    XrHirBooleanOperation operation;
    XrXirType operand;
} XrHirBooleanInput;
typedef struct XrHirBoolean {
    XrHirBooleanOrigin origin;
    XrHirBooleanOperation operation;
    XrXirType type;
} XrHirBoolean;
/* Successful qualifications own concrete Bool facts, never null-flow, loans,
 * epochs or branches. Failure preserves output and the request's first error. */
XR_FUNC XrXirStatus xr_hir_boolean_check(const XrXirTypes *,const XrHirBooleanInput *,
    XrCompileRequestControl *,XrHirBoolean *,const char **reason);
/* A logical expression's declared result hint grants no operand qualification
 * and creates no checked node. Conditions do not determine their arms' types. */
XR_FUNC XrXirType xr_hir_boolean_result_type(XrHirBooleanOperation);
#endif // XHIR_BOOLEAN_H
