/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_semantic_closure_internal.h - Mutable program closure storage
 */

#ifndef XR_PROGRAM_SEMANTIC_CLOSURE_INTERNAL_H
#define XR_PROGRAM_SEMANTIC_CLOSURE_INTERNAL_H

#include "xr_program_semantic_closure.h"
#include <stdatomic.h>

typedef enum XrProgramSemanticClosureState {
    XR_PROGRAM_SEMANTIC_CLOSURE_COLLECTING = 0,
    XR_PROGRAM_SEMANTIC_CLOSURE_VERIFYING,
    XR_PROGRAM_SEMANTIC_CLOSURE_FROZEN,
    XR_PROGRAM_SEMANTIC_CLOSURE_FAILED,
} XrProgramSemanticClosureState;

struct XrProgramSemanticClosure {
    atomic_uint_least32_t references;
    uint32_t schema;
    uint32_t family;
    uint8_t state;
    uint8_t verified;
    uint8_t failure_kind;
    uint8_t reserved;
    XrProgramSemanticClosureLimits limits;
    XrFingerprint policy_fingerprint;
    XrFingerprint fingerprint;
    XrGenerationClosureId generation_id;
    XrProgramSemanticModuleRecord *modules;
    uint32_t module_count;
    uint32_t module_capacity;
    XrProgramSemanticDependencyRecord *dependencies;
    uint32_t dependency_count;
    uint32_t dependency_capacity;
    XrProgramSemanticTypeRecord *types;
    uint32_t type_count;
    uint32_t type_capacity;
    XrProgramSemanticTypeFieldRecord *type_fields;
    uint32_t type_field_count;
    uint32_t type_field_capacity;
    XrProgramSemanticFunctionRecord *functions;
    uint32_t function_count;
    uint32_t function_capacity;
    XrProgramSemanticFunctionParameterRecord *function_parameters;
    uint32_t function_parameter_count;
    uint32_t function_parameter_capacity;
    XrProgramSemanticCallRecord *calls;
    uint32_t call_count;
    uint32_t call_capacity;
};

#endif  // XR_PROGRAM_SEMANTIC_CLOSURE_INTERNAL_H
