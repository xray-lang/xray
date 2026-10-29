/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_conversion.h - Owned contextual conversions in the canonical type pool
 *
 * KEY CONCEPT:
 *   Successful nodes contain complete conversion and callable identity facts.
 *   Syntax, declaration proofs and interning callbacks are borrowed only while
 *   checking; lowering never repeats the conversion relation.
 */
#ifndef XHIR_CONVERSION_H
#define XHIR_CONVERSION_H
#include "../xir/xxir_constraint_proof.h"
#include "../base/xcompile_request.h"

typedef struct XrHirConversionOrigin {
    uint32_t node_id,module;
    int line,column,end_line,end_column;
} XrHirConversionOrigin;
typedef enum XrHirConversionKind {
    XR_HIR_CONVERSION_IDENTITY,XR_HIR_CONVERSION_COPY,XR_HIR_CONVERSION_NULLABLE,
    XR_HIR_CONVERSION_CALLABLE,XR_HIR_CONVERSION_ERROR,XR_HIR_CONVERSION_NUMBER
} XrHirConversionKind;
typedef enum XrHirConversionRoot {
    XR_HIR_CONVERSION_ROOT_NONE,XR_HIR_CONVERSION_ROOT_FIXED,
    XR_HIR_CONVERSION_ROOT_INFERRED,XR_HIR_CONVERSION_ROOT_ARGUMENT
} XrHirConversionRoot;
typedef struct XrHirConversion {
    XrHirConversionOrigin origin;
    XrXirType source,target,declared_bound;
    XrHirConversionKind kind;
    XrHirConversionRoot root;
    bool copy_nullable_element;
} XrHirConversion;
typedef struct XrHirConversionInput {
    XrHirConversionOrigin origin;
    XrXirType actual,expected;
    bool has_expected,infer_result,inferred,argument_bound;
} XrHirConversionInput;
/* Interning uses the caller's existing pool and allocation owner. The callback
 * may grow its node table; no pointer into that table survives the call. */
typedef XrXirStatus (*XrHirConversionIntern)(void *,const XrXirTypeNode *,XrXirType *);
typedef struct XrHirConversionContext {
    const XrXirCompileContext *compile;
    const XrXirTypes *types;
    const XrXirProofContext *proof;
    XrCompileRequestControl *control;
    void *intern_owner;
    XrHirConversionIntern intern;
} XrHirConversionContext;
/* Failure preserves output and the first request failure. Reason is static. */
XR_FUNC XrXirStatus xr_hir_conversion_check(const XrHirConversionContext *,
    const XrHirConversionInput *,XrHirConversion *,const char **reason);
/* Pure structural classification grants no value-flow or borrow authority. */
XR_FUNC XrXirStatus xr_hir_conversion_contains_callable(const XrXirTypes *,XrXirType,
    XrCompileRequestControl *,bool *,const char **reason);
/* A trusted typed conversion is consumed mechanically, after any recorded
 * nullable-element COPY. Identity conversions have no instruction. */
XR_FUNC XrXirInstruction xr_hir_conversion_instruction(const XrHirConversion *,uint32_t);
#endif // XHIR_CONVERSION_H
