/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_nullable.h - Owned outer-layer nullable expression types
 *
 * KEY CONCEPT:
 *   Context selection precedes checking the real default operand. Neither a
 *   context nor a result hint grants read claims, flow facts or loan authority.
 */
#ifndef XHIR_NULLABLE_H
#define XHIR_NULLABLE_H
#include "../xir/xxir.h"
#include "../base/xcompile_request.h"

typedef struct XrHirNullableOrigin {
    uint32_t node_id,module;
    int line,column,end_line,end_column;
} XrHirNullableOrigin;

typedef enum XrHirNullableRole {
    XR_HIR_NULLABLE_NULL=1,XR_HIR_NULLABLE_EQUAL_NULL,XR_HIR_NULLABLE_UNEQUAL_NULL,
    XR_HIR_NULLABLE_UNWRAP,XR_HIR_NULLABLE_FORCE,XR_HIR_NULLABLE_COALESCE,
    XR_HIR_NULLABLE_DEAD_DEFAULT
} XrHirNullableRole;
typedef enum XrHirNullableKind {
    XR_HIR_NULLABLE_NONE,XR_HIR_NULLABLE_PRESENCE,XR_HIR_NULLABLE_IDENTITY,
    XR_HIR_NULLABLE_ELEMENT,XR_HIR_NULLABLE_BRANCH
} XrHirNullableKind;
typedef struct XrHirNullableInput {
    XrHirNullableOrigin origin;
    XrHirNullableRole role;
    XrXirType source,expected,default_base;
    bool has_expected,null_default;
} XrHirNullableInput;
typedef XrXirStatus (*XrHirNullableIntern)(void *,const XrXirTypeNode *,XrXirType *);
typedef struct XrHirNullableContext {
    const XrXirTypes *types;
    XrCompileRequestControl *control;
    void *intern_owner;
    XrHirNullableIntern intern;
} XrHirNullableContext;
typedef struct XrHirNullablePlan {
    XrHirNullableOrigin origin;
    XrHirNullableRole role;
    XrHirNullableKind kind;
    XrXirType source,result,default_type;
    bool has_default,unwrap_present;
    int64_t immediate;
} XrHirNullablePlan;
typedef struct XrHirNullable {
    XrHirNullableOrigin origin;
    XrHirNullableKind kind;
    XrXirType source,result,default_type;
    bool unwrap_present;
    int64_t immediate;
} XrHirNullable;
/* DEAD_DEFAULT receives the default base selected by the existing read owner;
 * it only checks types and never authorizes claiming a nullable layer. The
 * interner is synchronously borrowed and may grow the shared type pool. */
XR_FUNC XrXirStatus xr_hir_nullable_prepare(const XrHirNullableContext *,const XrHirNullableInput *,
    XrHirNullablePlan *,const char **reason);
/* Only completion with the real default operand creates a checked coalescing
 * node. Failure preserves output and the first request failure. */
XR_FUNC XrXirStatus xr_hir_nullable_check(const XrHirNullablePlan *,XrXirType,
    XrCompileRequestControl *,XrHirNullable *,const char **reason);
XR_FUNC XrXirInstruction xr_hir_nullable_instruction(const XrHirNullable *,uint32_t operand);
/* Classification only; no checked node or operand qualification is produced. */
XR_FUNC XrXirType xr_hir_nullable_force_hint(const XrXirTypes *,XrXirType);
#endif // XHIR_NULLABLE_H
