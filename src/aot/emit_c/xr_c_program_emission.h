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

typedef struct XrCProgramDirectI64EmissionBinding {
    XrFingerprint target_fingerprint;
    XrCProgramXiFunctionBinding caller;
    XrCProgramXiFunctionBinding callee;
    const struct XiValue *xi_call;
    uint32_t target_call;
    uint32_t target_instruction;
    uint32_t target_argument;
    uint32_t semantic_operation;
    uint32_t program_call;
} XrCProgramDirectI64EmissionBinding;

XR_FUNC bool xr_c_program_direct_i64_emission_bind(
    const XrTargetPlan *target_plan, struct XiModule *const *modules,
    uint32_t module_count, XrCProgramDirectI64EmissionBinding *out,
    char *error, size_t error_size);

#endif  // XR_C_PROGRAM_EMISSION_H
