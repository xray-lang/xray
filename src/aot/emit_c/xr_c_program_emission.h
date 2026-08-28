/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_c_program_emission.h - Program TargetPlan to Xi C-emission binding
 *
 * KEY CONCEPT:
 *   A verified program TargetPlan owns the global function and call namespace.
 *   This transient view joins those rows to exact Xi nodes by stable PSC rows
 *   and gives the emitter deterministic C symbols without module-name lookup.
 */

#ifndef XR_C_PROGRAM_EMISSION_H
#define XR_C_PROGRAM_EMISSION_H

#include "xr_c_emission_schema.h"
#include "../../plan/target/xr_target_plan.h"

struct XiFunc;
struct XiModule;
struct XiValue;

#define XR_C_PROGRAM_FUNCTION_SYMBOL_CAPACITY                                                \
    (sizeof("xr_pf_") - 1u + XR_STABLE_ID_BYTES * 2u + 1u)

typedef struct XrCProgramXiFunctionBinding {
    const struct XiFunc *xi_function;
    uint32_t target_function;
    uint32_t target_partition;
    uint32_t semantic_function;
    uint32_t program_function;
    XrStableId identity;
    char c_symbol[XR_C_PROGRAM_FUNCTION_SYMBOL_CAPACITY];
} XrCProgramXiFunctionBinding;

typedef struct XrCProgramValueEmissionBinding {
    const struct XiFunc *xi_function;
    const struct XiValue *xi_value;
    uint32_t target_function;
    uint32_t target_partition;
    uint32_t semantic_function;
    XrCValueEmissionView emission;
} XrCProgramValueEmissionBinding;

typedef struct XrCProgramFunctionAbiEmissionBinding {
    const struct XiFunc *xi_function;
    uint32_t target_function;
    XrCFunctionAbiEmissionView emission;
} XrCProgramFunctionAbiEmissionBinding;

typedef struct XrCProgramDirectI64EmissionBinding {
    uint32_t schema_version;
    XrFingerprint target_fingerprint;
    XrCProgramXiFunctionBinding caller;
    XrCProgramXiFunctionBinding callee;
    XrCProgramXiFunctionBinding *initializers;
    uint32_t initializer_count;
    const struct XiValue *xi_call;
    const struct XiValue *xi_argument;
    const struct XiValue *xi_callee_operand;
    const struct XiValue *xi_native_leaf_callee_operand;
    const XrTargetFunctionRecord *caller_target_row;
    const XrTargetFunctionRecord *callee_target_row;
    const XrTargetCallRecord *call_row;
    const XrTargetCallArgumentRecord *argument_row;
    const XrTargetInstructionRecord *instruction_row;
    uint32_t target_call;
    uint32_t target_instruction;
    uint32_t target_argument;
    uint32_t semantic_operation;
    uint32_t program_call;
    bool callee_operand_elided;
    XrCProgramValueEmissionBinding *values;
    uint32_t value_count;
    XrCProgramFunctionAbiEmissionBinding *function_abis;
    uint32_t function_abi_count;
    bool verified;
} XrCProgramDirectI64EmissionBinding;

#define XR_C_PROGRAM_DIRECT_I64_EMISSION_SCHEMA_VERSION 3u

XR_FUNC bool xr_c_program_initializer_symbol_identity(
    XrStableId module_identity, XrStableId semantic_function_identity,
    XrStableId *out);
XR_FUNC bool xr_c_program_direct_i64_emission_bind(
    const XrTargetPlan *target_plan, struct XiModule *const *modules,
    uint32_t module_count, XrCProgramDirectI64EmissionBinding *out,
    char *error, size_t error_size);
XR_FUNC void xr_c_program_direct_i64_emission_release(
    XrCProgramDirectI64EmissionBinding *binding);
XR_FUNC bool xr_c_program_direct_i64_emission_verify(
    const XrCProgramDirectI64EmissionBinding *binding,
    const XrTargetPlan *target_plan, struct XiModule *const *modules,
    uint32_t module_count, char *error, size_t error_size);
XR_FUNC const XrCProgramXiFunctionBinding *
xr_c_program_direct_i64_function_binding(
    const XrCProgramDirectI64EmissionBinding *binding,
    const struct XiFunc *function);
XR_FUNC bool xr_c_program_direct_i64_value_view(
    const XrCProgramDirectI64EmissionBinding *binding,
    const struct XiFunc *function, const struct XiValue *value,
    XrCValueEmissionView *out);
XR_FUNC bool xr_c_program_direct_i64_function_abi_view(
    const XrCProgramDirectI64EmissionBinding *binding,
    const struct XiFunc *function, uint16_t ordinal,
    XrCFunctionAbiEmissionView *out);
XR_FUNC bool xr_c_program_direct_i64_call_is_exact(
    const XrCProgramDirectI64EmissionBinding *binding,
    const struct XiFunc *caller, const struct XiValue *call);
XR_FUNC bool xr_c_program_direct_i64_callee_operand_is_elided(
    const XrCProgramDirectI64EmissionBinding *binding,
    const struct XiFunc *caller, const struct XiValue *value);

#endif  // XR_C_PROGRAM_EMISSION_H
