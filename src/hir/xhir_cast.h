/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_cast.h - Owned explicit numeric and scalar text conversion facts
 */
#ifndef XHIR_CAST_H
#define XHIR_CAST_H
#include "../xir/xxir.h"
#include "../base/xcompile_request.h"
typedef struct XrHirCastOrigin {
    uint32_t node_id,module;
    int line,column,end_line,end_column;
} XrHirCastOrigin;
typedef enum XrHirCastRole {XR_HIR_CAST_EXPLICIT=1,XR_HIR_CAST_TEXT,XR_HIR_CAST_INTERPOLATION} XrHirCastRole;
typedef enum XrHirCastKind {XR_HIR_CAST_IDENTITY,XR_HIR_CAST_NUMBER,XR_HIR_CAST_TO_TEXT} XrHirCastKind;
typedef struct XrHirCastTargetInput {
    XrHirCastOrigin origin;
    XrHirCastRole role;
    XrXirType target;
    bool safe;
} XrHirCastTargetInput;
typedef struct XrHirCastTarget {
    XrHirCastOrigin origin;
    XrHirCastRole role;
    XrHirCastKind kind;
    XrXirType type;
} XrHirCastTarget;
typedef struct XrHirCastLiteralContext {bool present;XrXirType type;} XrHirCastLiteralContext;
typedef struct XrHirCast {
    XrHirCastOrigin origin;
    XrHirCastKind kind;
    XrXirType source,target;
} XrHirCast;
/* Target qualification precedes operand checking; it grants no operand or
 * value-flow authority. A successful final cast owns all facts by value.
 * Both checks synchronously borrow the pool/control and preserve failed output. */
XR_FUNC XrXirStatus xr_hir_cast_target_check(const XrXirTypes *,const XrHirCastTargetInput *,
    XrCompileRequestControl *,XrHirCastTarget *,const char **reason);
XR_FUNC XrXirStatus xr_hir_cast_check(const XrXirTypes *,const XrHirCastTarget *,XrXirType,
    XrCompileRequestControl *,XrHirCast *,const char **reason);
/* Numeric casts preserve the unsigned spelling of direct positive literals
 * above I64_MAX before performing the explicitly requested conversion. */
XR_FUNC XrHirCastLiteralContext xr_hir_cast_literal_context(const XrHirCastTarget *,bool,bool,uint64_t);
/* Identity forwards the existing value, without granting COPY/MOVE authority.
 * Only nonidentity trusted casts have a mechanical Built instruction. */
XR_FUNC XrXirInstruction xr_hir_cast_instruction(const XrHirCast *,uint32_t operand);
#endif // XHIR_CAST_H
