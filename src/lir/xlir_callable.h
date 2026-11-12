/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_callable.h - Exact owned signatures for ordinary function values
 *
 * KEY CONCEPT:
 *   A callable type owns its ordered physical signature. It grants no code
 *   address, environment lifetime, or authority to invoke a foreign owner.
 */
#ifndef XLIR_CALLABLE_H
#define XLIR_CALLABLE_H
#include "xlir_types.h"
#include "../shared/xr_param_mode.h"

enum {
    XR_LIR_CALLABLE_NO_SUSPEND = 1u,
    XR_LIR_CALLABLE_C_ABI = 2u
};

typedef struct XrLirCallableParameter {
    XrLirType type, array_element;
    XrParamMode mode;
    bool escaping;
} XrLirCallableParameter;

typedef struct XrLirCallableType {
    const XrLirCallableParameter *parameters;
    uint32_t parameter_count;
    XrLirType result, result_element;
    uint32_t flags;
    bool throws; /* Throwing signatures require an error ABI and are unsupported. */
} XrLirCallableType;

/* Construction copies the complete signature and preserves output on failure.
 * Constructed dependencies must already belong to this receiving module.
 * Query slices borrow their module and never outlive it. */
XR_FUNC XrLirStatus xr_lir_callable_type(XrLirModule *, const XrLirCallableType *, XrLirType *);
XR_FUNC XrLirStatus xr_lir_callable_type_view(const XrLirModule *, XrLirType, XrLirCallableType *);

#endif // XLIR_CALLABLE_H
