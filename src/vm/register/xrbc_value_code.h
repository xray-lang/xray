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
#include "base/xchecks.h"
#include "xrbc_value_homes.h"
#include "xray_native_pod.h"
#include "lir/xlir_array_region_binding.h"

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
    XR_RBC_VALUE_CALL, XR_RBC_VALUE_RETURN, XR_RBC_VALUE_EDGE,
    XR_RBC_VALUE_ARRAY, XR_RBC_VALUE_BINDING_NEW, XR_RBC_VALUE_BINDING_READ,
    XR_RBC_VALUE_BINDING_WRITE, XR_RBC_VALUE_BINDING_DROP, XR_RBC_VALUE_MAP, XR_RBC_VALUE_DISPLAY,
    XR_RBC_VALUE_RECORD_NEW, XR_RBC_VALUE_RECORD_FIELD, XR_RBC_VALUE_NULLABLE_NONE,
    XR_RBC_VALUE_NULLABLE_SOME, XR_RBC_VALUE_NULLABLE_IS_SOME, XR_RBC_VALUE_NULLABLE_UNWRAP,
    XR_RBC_VALUE_FUNCTION_NEW, XR_RBC_VALUE_FUNCTION_CALL, XR_RBC_VALUE_FUNCTION_ALIAS,
    XR_RBC_VALUE_FUNCTION_SCOPE_EXIT, XR_RBC_VALUE_SLICE, XR_RBC_VALUE_STRING_APPEND_OWNED,
    XR_RBC_VALUE_CELL, XR_RBC_VALUE_MAP_LITERAL, XR_RBC_VALUE_STRING_APPEND_BINDING,
    XR_RBC_VALUE_SET,
    XR_RBC_VALUE_ASSERT_CONDITION
} XrRbcValueOperation;

typedef struct XrRbcValueAccess {
    XrRbcValueOperation operation;
    /* SSA ids in this function, including borrowed formals. Zero is absent.
     * A scalar operand/result resolves through locations; a managed one uses
     * the real common home, never a register address as an owning place. */
    uint32_t result, source, other;
    /* Literal/module-slot/callee/selected-home-edge ordinal, according to op.
     * CALL arguments are SSA ids in the immutable arguments span; window is
     * its ordinary physical call window. A Slice result also uses other for
     * its one-based common HomeCall, including the proven actual origin and
     * caller landing. RETURN source names its SSA result and Slice index its
     * one-based common RETURN_TRANSFER event.
     * EDGE uses first/count source/target SSA pairs, count measuring pairs,
     * for type-zero transport absent from common typed homes. Its index is
     * UINT32_MAX when there is no common typed edge, never ambiguous edge zero. */
    /* ARRAY/MAP index is the verified catalog role, first/count its complete
     * SSA operands. A mutator's window selects its common pending workspace. */
    /* SET index is the dedicated LIR operation and other its one-based common
     * SET_OPERATION event. Operands and workspace follow the same real homes. */
    /* SLICE index is its closed role, first/count its full SSA operand span.
     * Only NEW over a real State reference uses source=XR_LIR_STATE_REF as the
     * received closed READ binding role, not another SSA operand or write cap.
     * window is a one-based common loan event for construction/END, zero for
     * readers and materialization. Natural bytes never carry a keeper owner. */
    /* STRING_APPEND_OWNED index is its one-based APPEND_TRANSFER event.
     * source moves, other borrows, and result has its own real staging slot. */
    /* String BINDING accesses and STRING_APPEND_BINDING index their exact
     * common event. A writing access uses window=UINT32_MAX without a stage,
     * otherwise the zero-based independent VALUE workspace ordinal. Append
     * always has that candidate; a borrowed String formal copy uses its stage. */
    /* CELL index is its exact one-based common Cell event. Aliases use other
     * for a physical scope capability; operation hooks keep their normal SSA
     * operands. Scope exit and RELEASE never read retired natural bytes. */
    uint32_t index, first, count, window;
} XrRbcValueAccess;
XR_STATIC_ASSERT(sizeof(XrRbcValueAccess)==32,"ordinary typed access rows occupy thirty-two bytes");

/* Numeric roles belong only to code with projected Array slots. Stable access
 * ordinals survive compaction; ordinary operation rows keep their own ABI. */
typedef struct XrRbcArrayRegionAccess {
    uint32_t region;
    XrLirArrayRegionOperation operation;
} XrRbcArrayRegionAccess;

typedef struct XrRbcValueCode {
    const XrLirValueHomes *homes;
    const XrRbcValueLocation *locations; /* SSA indexed, including unused zero. */
    uint32_t value_count;
    const uint32_t *parameters; /* Logical LIR parameter SSA ids, in order. */
    uint32_t parameter_count;
    /* Common String bindings need cold setup before any checked operand borrow.
     * The compiler projects this bit once; ordinary scalar functions never
     * acquire a typed activation or execute a new dispatch path. */
    bool string_binding_setup;
    const XrRbcValueAccess *accesses;
    size_t access_count;
    const uint32_t *arguments;
    size_t argument_count;
    const struct XrtPodDisplay *displays;
    size_t display_count;
    const uint32_t *scope_values; /* Physical scope ordinal -> original SSA capability. */
    uint32_t scope_count;
    XrLirArrayRegionBindings *array_regions; /* Owned numeric projection, no live addresses. */
    bool array_region_runtime; /* Compile-time SET-window consumption; read-only facts stay ordinary. */
    const XrRbcArrayRegionAccess *array_region_accesses; /* Same ordinal/count as accesses, or absent. */
} XrRbcValueCode;

/* Descriptions borrow the actual code lease, as the existing common homes do.
 * Raw bytecode cannot acquire typed owning permissions from forged pointers.
 * Scalar-only code without any typed operation has no descriptor. */
XR_FUNC const XrRbcValueCode *xr_rbc_module_value_code(const XrRbcModule *, uint32_t function);
typedef struct XrRbcValueRuntime XrRbcValueRuntime;
#endif // XRBC_VALUE_CODE_H
