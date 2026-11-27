/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (C) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_math_host_ops.h - Closed binary64 host math operations
 *
 * The checked standard-module identity selects a concrete runtime function
 * and its binary64 arity. Both execution targets use that same function.
 */
#ifndef XR_MATH_HOST_OPS_H
#define XR_MATH_HOST_OPS_H

#define XR_MATH_HOST_OPERATIONS(OP) \
    OP(SIN, sin, 1) \
    OP(COS, cos, 1) \
    OP(SQRT, sqrt, 1) \
    OP(HYPOT, hypot, 2) \
    OP(POW, pow, 2) \
    OP(TAN, tan, 1) \
    OP(ASIN, asin, 1) \
    OP(ACOS, acos, 1) \
    OP(ATAN, atan, 1) \
    OP(ATAN2, atan2, 2) \
    OP(LOG, log, 1) \
    OP(LOG10, log10, 1) \
    OP(LOG2, log2, 1) \
    OP(EXP, exp, 1) \
    OP(SINH, sinh, 1) \
    OP(COSH, cosh, 1) \
    OP(TANH, tanh, 1) \
    OP(CBRT, cbrt, 1) \
    OP(FMOD, fmod, 2) \
    OP(LOG1P, log1p, 1) \
    OP(EXPM1, expm1, 1)

#endif // XR_MATH_HOST_OPS_H
