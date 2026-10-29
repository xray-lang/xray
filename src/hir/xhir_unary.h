/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xhir_unary.h - Owned numeric unary operators in the canonical type pool
 *
 * KEY CONCEPT:
 *   Typed unary nodes own the operation and any integer seed. Lowering reads
 *   no syntax, type-pool elements, compiler resources or request callbacks.
 */
#ifndef XHIR_UNARY_H
#define XHIR_UNARY_H
#include "xhir_conversion.h"

typedef enum XrHirUnaryOperator {
    XR_HIR_UNARY_NEGATE, XR_HIR_UNARY_COMPLEMENT
} XrHirUnaryOperator;

typedef struct XrHirUnaryInput {
    XrHirConversionOrigin origin;
    XrHirUnaryOperator operation;
    XrXirType operand;
} XrHirUnaryInput;

typedef struct XrHirUnary {
    XrHirConversionOrigin origin;
    XrXirType type;
    XrXirOp operation;
    int64_t seed;
    bool has_seed;
} XrHirUnary;

/* A synchronous check borrows the pool and control; failure preserves output. */
XR_FUNC XrXirStatus xr_hir_unary_check(const XrXirTypes *,const XrHirUnaryInput *,
    XrCompileRequestControl *,XrHirUnary *,const char **reason);
/* The seed instruction is needed only when a trusted node has_seed. */
XR_FUNC XrXirInstruction xr_hir_unary_seed_instruction(const XrHirUnary *);
XR_FUNC XrXirInstruction xr_hir_unary_instruction(const XrHirUnary *,uint32_t operand,uint32_t seed);
#endif // XHIR_UNARY_H
