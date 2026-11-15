/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_code.h - Cold typed operations over common stable SSA homes
 *
 * KEY CONCEPT:
 *   Code owns raw descriptions only. Managed operands name the common SSA
 *   homes, while scalar and POD locations retain their ordinary bytecode ABI.
 */
#ifndef XRBC_VALUE_CODE_H
#define XRBC_VALUE_CODE_H
#include "xrbc_value_homes.h"

typedef enum XrRbcValueLocationKind {
    XR_RBC_VALUE_NOWHERE, XR_RBC_VALUE_REGISTER, XR_RBC_VALUE_SPILL, XR_RBC_VALUE_AGGREGATE
} XrRbcValueLocationKind;
typedef struct XrRbcValueLocation {
    XrRbcValueLocationKind kind;
    uint32_t index;
} XrRbcValueLocation;

typedef enum XrRbcValueOperation {
    XR_RBC_VALUE_STRING_LITERAL, XR_RBC_VALUE_STRING_I64, XR_RBC_VALUE_STRING_F64,
    XR_RBC_VALUE_STRING_CONCAT, XR_RBC_VALUE_COPY, XR_RBC_VALUE_MOVE, XR_RBC_VALUE_DROP,
    XR_RBC_VALUE_STRING_PRINT, XR_RBC_VALUE_STRING_LENGTH,
    XR_RBC_VALUE_STATE_READ, XR_RBC_VALUE_STATE_WRITE,
    XR_RBC_VALUE_STATE_CONSTRUCT, XR_RBC_VALUE_STATE_INITIALIZE,
    XR_RBC_VALUE_CALL, XR_RBC_VALUE_RETURN, XR_RBC_VALUE_EDGE
} XrRbcValueOperation;

typedef struct XrRbcValueAccess {
    XrRbcValueOperation operation;
    /* SSA ids in this function, including borrowed formals. Zero is absent.
     * A scalar operand/result resolves through locations; a managed one uses
     * the real common home, never a register address as an owning place. */
    uint32_t result, source, other;
    /* Literal/module-slot/callee/selected-home-edge ordinal, according to op.
     * CALL arguments are SSA ids in the immutable arguments span; window is
     * its ordinary physical call window. RETURN source names its SSA result. */
    uint32_t index, first, count, window;
} XrRbcValueAccess;

typedef struct XrRbcValueCode {
    const XrLirValueHomes *homes;
    const XrRbcValueLocation *locations; /* SSA indexed, including unused zero. */
    uint32_t value_count;
    const uint32_t *parameters; /* Logical LIR parameter SSA ids, in order. */
    uint32_t parameter_count;
    const XrRbcValueAccess *accesses;
    size_t access_count;
    const uint32_t *arguments;
    size_t argument_count;
} XrRbcValueCode;

/* Descriptions borrow the actual code lease, as the existing common homes do.
 * Raw bytecode cannot acquire typed owning permissions from forged pointers.
 * Scalar-only code without any typed operation has no descriptor. */
XR_FUNC const XrRbcValueCode *xr_rbc_module_value_code(const XrRbcModule *, uint32_t function);
typedef struct XrRbcValueRuntime XrRbcValueRuntime;
#endif // XRBC_VALUE_CODE_H
