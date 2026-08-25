/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_semantic_closure.h - Target-neutral closed-world semantic identity
 *
 * KEY CONCEPT:
 *   This schema is the target-neutral instantiation-graph identity foundation:
 *   modules, dependencies, concrete types, concrete functions, and resolved
 *   calls supplied by an upstream semantic producer. It deliberately contains
 *   no member/fixpoint/export summary, artifact codec, target profile, layout,
 *   storage, or ABI fact.
 */

#ifndef XR_PROGRAM_SEMANTIC_CLOSURE_H
#define XR_PROGRAM_SEMANTIC_CLOSURE_H

#include "../../base/xdefs.h"
#include "../../base/xstable_id.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION UINT32_C(3)

#define XR_PROGRAM_SEMANTIC_CLOSURE_MAX_MODULES UINT32_C(256)
#define XR_PROGRAM_SEMANTIC_CLOSURE_MAX_DEPENDENCIES UINT32_C(4096)
#define XR_PROGRAM_SEMANTIC_CLOSURE_MAX_TYPES UINT32_C(16384)
#define XR_PROGRAM_SEMANTIC_CLOSURE_MAX_FUNCTIONS UINT32_C(16384)
#define XR_PROGRAM_SEMANTIC_CLOSURE_MAX_CALLS UINT32_C(65536)

typedef struct XrProgramSemanticClosure XrProgramSemanticClosure;

typedef struct XrGenerationClosureId {
    uint8_t bytes[XR_STABLE_ID_BYTES];
} XrGenerationClosureId;

typedef struct XrProgramSemanticClosureLimits {
    uint32_t max_modules;
    uint32_t max_dependencies;
    uint32_t max_types;
    uint32_t max_functions;
    uint32_t max_calls;
} XrProgramSemanticClosureLimits;

typedef struct XrProgramSemanticModuleInput {
    XrStableId module_identity;
    XrFingerprint source_fingerprint;
    XrFingerprint export_fingerprint;
} XrProgramSemanticModuleInput;

typedef struct XrProgramSemanticDependencyInput {
    XrStableId source_module;
    XrStableId dependency_module;
    XrFingerprint contract_fingerprint;
} XrProgramSemanticDependencyInput;

typedef struct XrProgramSemanticTypeInput {
    XrStableId module_identity;
    XrStableId declaration_identity;
    XrStableId concrete_instance_identity;
    XrFingerprint shape_fingerprint;
    XrFingerprint ownership_fingerprint;
} XrProgramSemanticTypeInput;

typedef enum XrProgramSemanticFunctionFlag {
    XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY = 1u << 0,
    XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED = 1u << 1,
} XrProgramSemanticFunctionFlag;

typedef struct XrProgramSemanticSourceLocator {
    /* Coordinates are 1-indexed and the end point is exclusive. */
    uint32_t kind;
    uint32_t start_line;
    uint32_t start_column;
    uint32_t end_line;
    uint32_t end_column;
} XrProgramSemanticSourceLocator;

typedef struct XrProgramSemanticFunctionInput {
    XrStableId module_identity;
    XrStableId declaration_identity;
    XrStableId concrete_instance_identity;
    XrProgramSemanticSourceLocator declaration_locator;
    XrFingerprint signature_fingerprint;
    XrFingerprint effect_fingerprint;
    uint64_t capability_mask;
    uint8_t flags;
    uint8_t reserved[7];
} XrProgramSemanticFunctionInput;

typedef struct XrProgramSemanticCallInput {
    XrStableId callsite_identity;
    XrProgramSemanticSourceLocator locator;
    XrStableId caller_function;
    XrStableId callee_function;
    XrFingerprint contract_fingerprint;
} XrProgramSemanticCallInput;

typedef struct XrProgramSemanticModuleRecord {
    XrStableId module_identity;
    XrFingerprint source_fingerprint;
    XrFingerprint export_fingerprint;
} XrProgramSemanticModuleRecord;

typedef struct XrProgramSemanticDependencyRecord {
    XrStableId source_module;
    XrStableId dependency_module;
    XrFingerprint contract_fingerprint;
} XrProgramSemanticDependencyRecord;

typedef struct XrProgramSemanticTypeRecord {
    XrStableId id;
    XrStableId module_identity;
    XrStableId declaration_identity;
    XrStableId concrete_instance_identity;
    XrFingerprint shape_fingerprint;
    XrFingerprint ownership_fingerprint;
} XrProgramSemanticTypeRecord;

typedef struct XrProgramSemanticFunctionRecord {
    XrStableId id;
    XrStableId module_identity;
    XrStableId declaration_identity;
    XrStableId concrete_instance_identity;
    XrProgramSemanticSourceLocator declaration_locator;
    XrFingerprint signature_fingerprint;
    XrFingerprint effect_fingerprint;
    uint64_t capability_mask;
    uint8_t flags;
    uint8_t reserved[7];
} XrProgramSemanticFunctionRecord;

typedef struct XrProgramSemanticCallRecord {
    XrStableId id;
    XrStableId callsite_identity;
    XrProgramSemanticSourceLocator locator;
    XrStableId caller_function;
    XrStableId callee_function;
    XrFingerprint contract_fingerprint;
} XrProgramSemanticCallRecord;

XR_FUNC bool xr_program_semantic_closure_create(
    const XrProgramSemanticClosureLimits *limits, XrFingerprint policy_fingerprint,
    XrProgramSemanticClosure **out, char *error, size_t error_size);
XR_FUNC void xr_program_semantic_closure_free(XrProgramSemanticClosure *closure);

XR_FUNC bool xr_program_semantic_closure_add_module(
    XrProgramSemanticClosure *closure, const XrProgramSemanticModuleInput *input,
    char *error, size_t error_size);
XR_FUNC bool xr_program_semantic_closure_add_dependency(
    XrProgramSemanticClosure *closure, const XrProgramSemanticDependencyInput *input,
    char *error, size_t error_size);
XR_FUNC bool xr_program_semantic_closure_add_type(
    XrProgramSemanticClosure *closure, const XrProgramSemanticTypeInput *input,
    XrStableId *type_identity, char *error, size_t error_size);
XR_FUNC bool xr_program_semantic_closure_add_function(
    XrProgramSemanticClosure *closure, const XrProgramSemanticFunctionInput *input,
    XrStableId *function_identity, char *error, size_t error_size);
XR_FUNC bool xr_program_semantic_closure_add_call(
    XrProgramSemanticClosure *closure, const XrProgramSemanticCallInput *input,
    XrStableId *call_identity, char *error, size_t error_size);

XR_FUNC bool xr_program_semantic_closure_freeze(XrProgramSemanticClosure *closure,
                                                char *error, size_t error_size);
XR_FUNC bool xr_program_semantic_closure_verify(const XrProgramSemanticClosure *closure,
                                                char *error, size_t error_size);

XR_FUNC bool xr_program_semantic_closure_is_frozen(const XrProgramSemanticClosure *closure);
XR_FUNC bool xr_program_semantic_closure_is_verified(const XrProgramSemanticClosure *closure);
XR_FUNC uint32_t xr_program_semantic_closure_schema(const XrProgramSemanticClosure *closure);
XR_FUNC XrFingerprint
xr_program_semantic_closure_fingerprint(const XrProgramSemanticClosure *closure);
XR_FUNC XrGenerationClosureId
xr_program_semantic_closure_generation_id(const XrProgramSemanticClosure *closure);
XR_FUNC bool xr_generation_closure_id_equal(XrGenerationClosureId left,
                                            XrGenerationClosureId right);

XR_FUNC size_t xr_program_semantic_closure_module_count(
    const XrProgramSemanticClosure *closure);
XR_FUNC size_t xr_program_semantic_closure_dependency_count(
    const XrProgramSemanticClosure *closure);
XR_FUNC size_t xr_program_semantic_closure_type_count(
    const XrProgramSemanticClosure *closure);
XR_FUNC size_t xr_program_semantic_closure_function_count(
    const XrProgramSemanticClosure *closure);
XR_FUNC size_t xr_program_semantic_closure_call_count(
    const XrProgramSemanticClosure *closure);

XR_FUNC const XrProgramSemanticModuleRecord *xr_program_semantic_closure_module(
    const XrProgramSemanticClosure *closure, uint32_t index);
XR_FUNC const XrProgramSemanticDependencyRecord *xr_program_semantic_closure_dependency(
    const XrProgramSemanticClosure *closure, uint32_t index);
XR_FUNC const XrProgramSemanticTypeRecord *xr_program_semantic_closure_type(
    const XrProgramSemanticClosure *closure, uint32_t index);
XR_FUNC const XrProgramSemanticFunctionRecord *xr_program_semantic_closure_function(
    const XrProgramSemanticClosure *closure, uint32_t index);
XR_FUNC const XrProgramSemanticCallRecord *xr_program_semantic_closure_call(
    const XrProgramSemanticClosure *closure, uint32_t index);

#endif  // XR_PROGRAM_SEMANTIC_CLOSURE_H
