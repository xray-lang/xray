/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_scalar_call_semantics.h - Canonical sealed scalar call semantics
 */

#ifndef XR_SCALAR_CALL_SEMANTICS_H
#define XR_SCALAR_CALL_SEMANTICS_H

#include "../../base/xdefs.h"
#include "../../base/xstable_id.h"
#include <stdbool.h>
#include <stdint.h>

#define XR_SCALAR_CALL_SEMANTICS_SCHEMA_VERSION UINT32_C(1)

typedef enum XrScalarI64FunctionShape {
    XR_SCALAR_I64_FUNCTION_NULLARY = 0,
    XR_SCALAR_I64_FUNCTION_UNARY = 1,
} XrScalarI64FunctionShape;

/* A complete value record, not an analyzer or target handle. */
typedef struct XrScalarI64FunctionContract {
    uint32_t schema;
    uint8_t parameter_count;
    uint8_t reserved[3];
    uint64_t capability_mask;
    XrFingerprint signature_fingerprint;
    XrFingerprint effect_fingerprint;
} XrScalarI64FunctionContract;

XR_FUNC bool xr_scalar_i64_function_contract(
    XrScalarI64FunctionShape shape, XrScalarI64FunctionContract *out);
XR_FUNC bool xr_scalar_i64_call_contract(
    const XrScalarI64FunctionContract *callee, XrFingerprint *out);
XR_FUNC bool xr_scalar_i64_function_contract_is_exact(
    const XrScalarI64FunctionContract *contract, XrScalarI64FunctionShape shape);

#endif  // XR_SCALAR_CALL_SEMANTICS_H
