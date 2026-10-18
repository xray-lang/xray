/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_math.h - Fixed scalar math and metered entropy boundaries
 */
#ifndef XXIR_MATH_H
#define XXIR_MATH_H
#include "xxir_scalar.h"

typedef enum XrXirMathOperation {
#define XR_XIR_MATH_UNARY(name,id,source,function) XR_XIR_MATH_##name=id,
#define XR_XIR_MATH_BINARY XR_XIR_MATH_UNARY
#define XR_XIR_MATH_INTEGER XR_XIR_MATH_UNARY
#define XR_XIR_MATH_ENTROPY XR_XIR_MATH_UNARY
#include "xxir_math.def"
#undef XR_XIR_MATH_UNARY
#undef XR_XIR_MATH_BINARY
#undef XR_XIR_MATH_INTEGER
#undef XR_XIR_MATH_ENTROPY
    XR_XIR_MATH_COUNT=29
} XrXirMathOperation;

typedef struct XrXirMathSignature {
    XrXirType parameter, result;
    uint32_t arity;
    bool random;
} XrXirMathSignature;
static inline bool xr_xir_math_signature(uint32_t operation,XrXirMathSignature *output) {
    if (!output) return false;
    XrXirMathSignature signature;
    switch (operation) {
#define XR_XIR_MATH_UNARY(name,id,source,function) \
    case id:signature=(XrXirMathSignature){XR_XIR_F64,XR_XIR_F64,1,false};break;
#define XR_XIR_MATH_BINARY(name,id,source,function) \
    case id:signature=(XrXirMathSignature){XR_XIR_F64,XR_XIR_F64,2,false};break;
#define XR_XIR_MATH_INTEGER(name,id,source,function) \
    case id:signature=(XrXirMathSignature){XR_XIR_F64,XR_XIR_I64,1,false};break;
#define XR_XIR_MATH_ENTROPY(name,id,source,result) \
    case id:signature=(XrXirMathSignature){XR_XIR_##result==XR_XIR_F64 ? XR_XIR_UNIT : XR_XIR_I64, \
        XR_XIR_##result,XR_XIR_##result==XR_XIR_F64 ? 0u : 2u,true};break;
#include "xxir_math.def"
#undef XR_XIR_MATH_UNARY
#undef XR_XIR_MATH_BINARY
#undef XR_XIR_MATH_INTEGER
#undef XR_XIR_MATH_ENTROPY
    default:return false;
    }
    *output=signature;return true;
}

typedef XrXirRunStatus (*XrXirMathEntropyFill)(void *context,unsigned char *bytes,size_t length);
typedef struct XrXirMathEntropy {
    void *context;
    /* The caller charges every requested draw and can stop on its own budget.
     * No callback or context survives this synchronous scalar operation. */
    XrXirMathEntropyFill fill;
} XrXirMathEntropy;

XR_FUNC XrXirRunStatus xr_xir_math_evaluate(uint32_t operation,int64_t left,int64_t right,int64_t *output);
XR_FUNC XrXirRunStatus xr_xir_math_random(uint32_t operation,XrXirMathEntropy entropy,
    int64_t minimum,int64_t maximum,int64_t *output);
#endif // XXIR_MATH_H
