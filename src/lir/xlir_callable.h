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

typedef enum XrLirClosureStorage {
    XR_LIR_CLOSURE_FRAME, XR_LIR_CLOSURE_OWNED
} XrLirClosureStorage;

typedef struct XrLirCallableParameter {
    XrLirType type, array_element;
    XrParamMode mode;
    bool escaping;
} XrLirCallableParameter;

typedef struct XrLirFunctionCallableParameter {
    uint32_t parameter;
    XrLirCallableParameter contract;
} XrLirFunctionCallableParameter;

typedef struct XrLirCallableType {
    const XrLirCallableParameter *parameters;
    uint32_t parameter_count;
    XrLirType result, result_element;
    uint32_t flags;
    bool throws; /* Throwing signatures require an error ABI and are unsupported. */
} XrLirCallableType;

typedef struct XrLirClosureTarget {
    XrLirType callable;
    uint32_t capture_count;
    bool state_capture;
} XrLirClosureTarget;

/* Construction copies the complete signature and preserves output on failure.
 * Constructed dependencies must already belong to this receiving module.
 * Query slices borrow their module and never outlive it. */
XR_FUNC XrLirStatus xr_lir_callable_type(XrLirModule *, const XrLirCallableType *, XrLirType *);
XR_FUNC XrLirStatus xr_lir_callable_type_view(const XrLirModule *, XrLirType, XrLirCallableType *);
/* Call operands can forget NoSuspend. All other signature components, including
 * parameter modes, escape contracts and the C ABI bit, remain exact. */
XR_FUNC bool xr_lir_callable_accepts(const XrLirModule *, XrLirType expected, XrLirType actual);
/* Captures are the declaration's ordered prefix. A hidden State parameter,
 * when present, remains its final parameter and is bound by the environment.
 * Setting this metadata never turns an ordinary function ID into a value. */
XR_FUNC XrLirStatus xr_lir_set_closure_target(XrLirModule *, XrLirFunction, XrLirClosureTarget);
XR_FUNC XrLirStatus xr_lir_closure_target_view(const XrLirModule *, XrLirFunction, XrLirClosureTarget *);

#endif // XLIR_CALLABLE_H
