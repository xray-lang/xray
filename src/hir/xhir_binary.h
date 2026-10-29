/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_binary.h - Typed binary operators in the canonical type pool
 *
 * KEY CONCEPT:
 *   Successful nodes own the selected operation and both operand conversions.
 *   Syntax and declaration proofs are borrowed only during checking.
 */
#ifndef XHIR_BINARY_H
#define XHIR_BINARY_H
#include "xhir_conversion.h"

typedef enum XrHirBinaryOperator {
    XR_HIR_BINARY_ADD,XR_HIR_BINARY_SUB,XR_HIR_BINARY_MUL,XR_HIR_BINARY_DIV,
    XR_HIR_BINARY_REM,XR_HIR_BINARY_AND,XR_HIR_BINARY_OR,XR_HIR_BINARY_XOR,
    XR_HIR_BINARY_SHL,XR_HIR_BINARY_SHR,XR_HIR_BINARY_EQ,XR_HIR_BINARY_NE,
    XR_HIR_BINARY_LT,XR_HIR_BINARY_LE,XR_HIR_BINARY_GT,XR_HIR_BINARY_GE
} XrHirBinaryOperator;

typedef struct XrHirBinaryInput {
    XrHirConversionOrigin origin;
    XrHirBinaryOperator operation;
    XrXirType left,right;
    bool argument_bound;
} XrHirBinaryInput;

typedef struct XrHirBinary {
    XrHirConversionOrigin origin;
    XrHirConversion left,right;
    XrXirOp operation;
    XrXirType result;
    int64_t immediate;
} XrHirBinary;

/* Failure preserves output. The context and static reason are never retained. */
XR_FUNC XrXirStatus xr_hir_binary_check(const XrHirConversionContext *,
    const XrHirBinaryInput *,XrHirBinary *,const char **reason);
/* Operands have already undergone the node's recorded conversions. */
XR_FUNC XrXirInstruction xr_hir_binary_instruction(const XrHirBinary *,uint32_t,uint32_t);
#endif // XHIR_BINARY_H
