/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_math_ir.h - Fixed operation metadata for common instruction admission
 */
#ifndef XXIR_MATH_IR_H
#define XXIR_MATH_IR_H
#include "xxir.h"
#include "xxir_math.h"
static inline bool xr_xir_math_opcode(XrXirOp operation) {
    return operation>=XR_XIR_MATH_UNARY_F64 && operation<=XR_XIR_RANDOM_I64;
}
static inline bool xr_xir_math_ir_signature(XrXirOp operation,int64_t selector,XrXirMathSignature *output) {
    XrXirMathSignature signature;
    if (!output || selector<=0 || selector>=XR_XIR_MATH_COUNT ||
        !xr_xir_math_signature((uint32_t)selector,&signature)) return false;
    XrXirOp expected=signature.random ?
        (signature.result==XR_XIR_F64 ? XR_XIR_RANDOM_F64 : XR_XIR_RANDOM_I64) :
        signature.result==XR_XIR_I64 ? XR_XIR_MATH_UNARY_I64 :
        signature.arity==2 ? XR_XIR_MATH_BINARY_F64 : XR_XIR_MATH_UNARY_F64;
    if (operation!=expected) return false;
    *output=signature;return true;
}

#endif // XXIR_MATH_IR_H
