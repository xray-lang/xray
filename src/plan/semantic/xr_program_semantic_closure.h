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

#define XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION UINT32_C(7)

#define XR_PROGRAM_SEMANTIC_CLOSURE_MAX_MODULES UINT32_C(256)
#define XR_PROGRAM_SEMANTIC_CLOSURE_MAX_DEPENDENCIES UINT32_C(4096)
#define XR_PROGRAM_SEMANTIC_CLOSURE_MAX_TYPES UINT32_C(16384)
#define XR_PROGRAM_SEMANTIC_CLOSURE_MAX_TYPE_FIELDS UINT32_C(65536)
#define XR_PROGRAM_SEMANTIC_CLOSURE_MAX_FUNCTIONS UINT32_C(16384)
#define XR_PROGRAM_SEMANTIC_CLOSURE_MAX_FUNCTION_PARAMETERS UINT32_C(65536)
#define XR_PROGRAM_SEMANTIC_CLOSURE_MAX_CALLS UINT32_C(65536)

typedef struct XrProgramSemanticClosure XrProgramSemanticClosure;

/* Target-neutral semantic family. This is an explicit durable discriminator;
 * table cardinality is never interpreted as a capability bit. */
typedef enum XrProgramSemanticFamily {
    XR_PROGRAM_SEMANTIC_FAMILY_GENERAL = 1,
    XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL,
    XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL,
    XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL,
    XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL,
    XR_PROGRAM_SEMANTIC_FAMILY_I64_OVERFLOW_PREDICATE,
    XR_PROGRAM_SEMANTIC_FAMILY_COUNT,
} XrProgramSemanticFamily;

typedef enum XrProgramSemanticClosureFailureKind {
    XR_PROGRAM_SEMANTIC_CLOSURE_FAILURE_NONE = 0,
    XR_PROGRAM_SEMANTIC_CLOSURE_FAILURE_INVALID,
    XR_PROGRAM_SEMANTIC_CLOSURE_FAILURE_RESOURCE,
} XrProgramSemanticClosureFailureKind;

typedef struct XrGenerationClosureId {
    uint8_t bytes[XR_STABLE_ID_BYTES];
} XrGenerationClosureId;

typedef struct XrProgramSemanticClosureLimits {
    uint32_t max_modules;
    uint32_t max_dependencies;
    uint32_t max_types;
    uint32_t max_type_fields;
    uint32_t max_functions;
    uint32_t max_function_parameters;
    uint32_t max_calls;
} XrProgramSemanticClosureLimits;

typedef struct XrProgramSemanticModuleInput {
    XrStableId module_identity;
    XrFingerprint module_authority_fingerprint;
    XrFingerprint source_fingerprint;
    XrFingerprint export_fingerprint;
} XrProgramSemanticModuleInput;

typedef struct XrProgramSemanticSourceLocator {
    /* Coordinates are 1-indexed and the end point is exclusive. */
    uint32_t kind;
    uint32_t start_line;
    uint32_t start_column;
    uint32_t end_line;
    uint32_t end_column;
} XrProgramSemanticSourceLocator;

typedef enum XrProgramSemanticDependencyKind {
    XR_PROGRAM_SEMANTIC_DEPENDENCY_OPAQUE = 0,
    XR_PROGRAM_SEMANTIC_DEPENDENCY_SELECTIVE_FUNCTION_IMPORT,
    XR_PROGRAM_SEMANTIC_DEPENDENCY_KIND_COUNT,
} XrProgramSemanticDependencyKind;

typedef struct XrProgramSemanticDependencyInput {
    XrStableId source_module;
    XrStableId dependency_module;
    XrProgramSemanticSourceLocator import_locator;
    XrStableId exported_declaration;
    XrStableId exported_function;
    XrStableId resolver_binding;
    XrFingerprint contract_fingerprint;
    uint8_t kind;
    uint8_t reserved[7];
} XrProgramSemanticDependencyInput;

typedef enum XrProgramSemanticTypeKind {
    /* Existing target-neutral identity row with producer-defined fingerprints. */
    XR_PROGRAM_SEMANTIC_TYPE_OPAQUE = 0,
    XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR,
    XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE,
    XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT,
    XR_PROGRAM_SEMANTIC_TYPE_KIND_COUNT,
} XrProgramSemanticTypeKind;

typedef enum XrProgramSemanticTypeFlag {
    XR_PROGRAM_SEMANTIC_TYPE_NONNULLABLE = 1u << 0,
    XR_PROGRAM_SEMANTIC_TYPE_NONGENERIC = 1u << 1,
    XR_PROGRAM_SEMANTIC_TYPE_VALUE = 1u << 2,
    XR_PROGRAM_SEMANTIC_TYPE_POINTER_FREE = 1u << 3,
} XrProgramSemanticTypeFlag;

typedef struct XrProgramSemanticTypeFieldInput {
    XrStableId field_type;
    uint32_t declaration_ordinal;
    uint32_t reserved;
} XrProgramSemanticTypeFieldInput;

typedef struct XrProgramSemanticTypeInput {
    XrStableId module_identity;
    XrStableId declaration_identity;
    XrStableId concrete_instance_identity;
    XrProgramSemanticSourceLocator declaration_locator;
    XrFingerprint shape_fingerprint;
    XrFingerprint ownership_fingerprint;
    const XrProgramSemanticTypeFieldInput *fields;
    uint32_t field_count;
    uint8_t kind;
    uint8_t exact_scalar;
    uint8_t flags;
    uint8_t reserved;
} XrProgramSemanticTypeInput;

typedef enum XrProgramSemanticFunctionFlag {
    XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY = 1u << 0,
    XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED = 1u << 1,
} XrProgramSemanticFunctionFlag;

typedef struct XrProgramSemanticFunctionInput {
    XrStableId module_identity;
    XrStableId declaration_identity;
    XrStableId concrete_instance_identity;
    XrProgramSemanticSourceLocator declaration_locator;
    XrFingerprint signature_fingerprint;
    XrFingerprint effect_fingerprint;
    XrStableId return_type;
    const struct XrProgramSemanticFunctionParameterInput *parameters;
    uint32_t parameter_count;
    uint64_t capability_mask;
    uint8_t flags;
    uint8_t reserved[7];
} XrProgramSemanticFunctionInput;

typedef struct XrProgramSemanticFunctionParameterInput {
    XrStableId type;
    uint32_t declaration_ordinal;
    uint8_t mode;
    uint8_t reserved[3];
} XrProgramSemanticFunctionParameterInput;

typedef struct XrProgramSemanticCallInput {
    XrStableId callsite_identity;
    XrProgramSemanticSourceLocator locator;
    XrStableId caller_function;
    XrStableId callee_function;
    XrStableId resolver_binding;
    XrFingerprint contract_fingerprint;
} XrProgramSemanticCallInput;

typedef struct XrProgramSemanticModuleRecord {
    XrStableId module_identity;
    XrFingerprint module_authority_fingerprint;
    XrFingerprint source_fingerprint;
    XrFingerprint export_fingerprint;
} XrProgramSemanticModuleRecord;

typedef struct XrProgramSemanticDependencyRecord {
    XrStableId source_module;
    XrStableId dependency_module;
    XrProgramSemanticSourceLocator import_locator;
    XrStableId exported_declaration;
    XrStableId exported_function;
    XrStableId resolver_binding;
    XrFingerprint contract_fingerprint;
    uint8_t kind;
    uint8_t reserved[7];
} XrProgramSemanticDependencyRecord;

typedef struct XrProgramSemanticTypeRecord {
    XrStableId id;
    XrStableId module_identity;
    XrStableId declaration_identity;
    XrStableId concrete_instance_identity;
    XrProgramSemanticSourceLocator declaration_locator;
    XrFingerprint shape_fingerprint;
    XrFingerprint ownership_fingerprint;
    uint32_t field_begin;
    uint32_t field_count;
    uint8_t kind;
    uint8_t exact_scalar;
    uint8_t flags;
    uint8_t reserved;
} XrProgramSemanticTypeRecord;

typedef struct XrProgramSemanticTypeFieldRecord {
    XrStableId owner_type;
    XrStableId field_type;
    uint32_t declaration_ordinal;
    uint32_t reserved;
} XrProgramSemanticTypeFieldRecord;

typedef struct XrProgramSemanticFunctionRecord {
    XrStableId id;
    XrStableId module_identity;
    XrStableId declaration_identity;
    XrStableId concrete_instance_identity;
    XrProgramSemanticSourceLocator declaration_locator;
    XrFingerprint signature_fingerprint;
    XrFingerprint effect_fingerprint;
    XrStableId return_type;
    uint32_t parameter_begin;
    uint32_t parameter_count;
    uint64_t capability_mask;
    uint8_t flags;
    uint8_t reserved[7];
} XrProgramSemanticFunctionRecord;

typedef struct XrProgramSemanticFunctionParameterRecord {
    XrStableId owner_function;
    XrStableId type;
    uint32_t declaration_ordinal;
    uint8_t mode;
    uint8_t reserved[3];
} XrProgramSemanticFunctionParameterRecord;

typedef struct XrProgramSemanticCallRecord {
    XrStableId id;
    XrStableId callsite_identity;
    XrProgramSemanticSourceLocator locator;
    XrStableId caller_function;
    XrStableId callee_function;
    XrStableId resolver_binding;
    XrFingerprint contract_fingerprint;
} XrProgramSemanticCallRecord;

XR_FUNC bool xr_program_semantic_closure_create(const XrProgramSemanticClosureLimits *limits,
                                                XrFingerprint policy_fingerprint,
                                                XrProgramSemanticClosure **out, char *error,
                                                size_t error_size);
XR_FUNC bool xr_program_semantic_closure_set_family(XrProgramSemanticClosure *closure,
                                                    XrProgramSemanticFamily family, char *error,
                                                    size_t error_size);
/* Retaining mutable or unverified authority is forbidden. */
XR_FUNC XrProgramSemanticClosure *
xr_program_semantic_closure_retain(XrProgramSemanticClosure *closure);
XR_FUNC void xr_program_semantic_closure_free(XrProgramSemanticClosure *closure);
/* Typed construction failure state; diagnostics are never parsed as control
 * flow. This is meaningful only before a successful freeze. */
XR_FUNC XrProgramSemanticClosureFailureKind
xr_program_semantic_closure_failure_kind(const XrProgramSemanticClosure *closure);

XR_FUNC bool xr_program_semantic_closure_add_module(XrProgramSemanticClosure *closure,
                                                    const XrProgramSemanticModuleInput *input,
                                                    char *error, size_t error_size);
XR_FUNC bool
xr_program_semantic_closure_add_dependency(XrProgramSemanticClosure *closure,
                                           const XrProgramSemanticDependencyInput *input,
                                           char *error, size_t error_size);
XR_FUNC bool xr_program_semantic_closure_add_type(XrProgramSemanticClosure *closure,
                                                  const XrProgramSemanticTypeInput *input,
                                                  XrStableId *type_identity, char *error,
                                                  size_t error_size);
/* Construct the canonical registry-owned input for one exact scalar row. */
XR_FUNC bool xr_program_semantic_exact_scalar_type_input(uint8_t exact_scalar,
                                                         XrProgramSemanticTypeInput *out);
XR_FUNC bool xr_program_semantic_closure_add_function(XrProgramSemanticClosure *closure,
                                                      const XrProgramSemanticFunctionInput *input,
                                                      XrStableId *function_identity, char *error,
                                                      size_t error_size);
/* Construct the canonical stable identity from one function input's identity
 * domain.
 * Row/cardinality validation remains owned by add/freeze/verify. */
XR_FUNC bool xr_program_semantic_function_identity(XrFingerprint policy_fingerprint,
                                                   const XrProgramSemanticFunctionInput *input,
                                                   XrStableId *function_identity);
XR_FUNC bool xr_program_semantic_closure_add_call(XrProgramSemanticClosure *closure,
                                                  const XrProgramSemanticCallInput *input,
                                                  XrStableId *call_identity, char *error,
                                                  size_t error_size);

XR_FUNC bool xr_program_semantic_closure_freeze(XrProgramSemanticClosure *closure, char *error,
                                                size_t error_size);
XR_FUNC bool xr_program_semantic_closure_verify(const XrProgramSemanticClosure *closure,
                                                char *error, size_t error_size);

XR_FUNC bool xr_program_semantic_closure_is_frozen(const XrProgramSemanticClosure *closure);
XR_FUNC bool xr_program_semantic_closure_is_verified(const XrProgramSemanticClosure *closure);
XR_FUNC uint32_t xr_program_semantic_closure_schema(const XrProgramSemanticClosure *closure);
XR_FUNC XrProgramSemanticFamily
xr_program_semantic_closure_family(const XrProgramSemanticClosure *closure);
XR_FUNC XrFingerprint
xr_program_semantic_closure_fingerprint(const XrProgramSemanticClosure *closure);
XR_FUNC XrGenerationClosureId
xr_program_semantic_closure_generation_id(const XrProgramSemanticClosure *closure);
XR_FUNC bool xr_generation_closure_id_equal(XrGenerationClosureId left,
                                            XrGenerationClosureId right);

XR_FUNC size_t xr_program_semantic_closure_module_count(const XrProgramSemanticClosure *closure);
XR_FUNC size_t
xr_program_semantic_closure_dependency_count(const XrProgramSemanticClosure *closure);
XR_FUNC size_t xr_program_semantic_closure_type_count(const XrProgramSemanticClosure *closure);
XR_FUNC size_t
xr_program_semantic_closure_type_field_count(const XrProgramSemanticClosure *closure);
XR_FUNC size_t xr_program_semantic_closure_function_count(const XrProgramSemanticClosure *closure);
XR_FUNC size_t
xr_program_semantic_closure_function_parameter_count(const XrProgramSemanticClosure *closure);
XR_FUNC size_t xr_program_semantic_closure_call_count(const XrProgramSemanticClosure *closure);

XR_FUNC const XrProgramSemanticModuleRecord *
xr_program_semantic_closure_module(const XrProgramSemanticClosure *closure, uint32_t index);
XR_FUNC const XrProgramSemanticDependencyRecord *
xr_program_semantic_closure_dependency(const XrProgramSemanticClosure *closure, uint32_t index);
XR_FUNC const XrProgramSemanticTypeRecord *
xr_program_semantic_closure_type(const XrProgramSemanticClosure *closure, uint32_t index);
XR_FUNC const XrProgramSemanticTypeFieldRecord *
xr_program_semantic_closure_type_field(const XrProgramSemanticClosure *closure, uint32_t index);
XR_FUNC const XrProgramSemanticFunctionRecord *
xr_program_semantic_closure_function(const XrProgramSemanticClosure *closure, uint32_t index);
XR_FUNC const XrProgramSemanticFunctionParameterRecord *
xr_program_semantic_closure_function_parameter(const XrProgramSemanticClosure *closure,
                                               uint32_t index);
XR_FUNC const XrProgramSemanticCallRecord *
xr_program_semantic_closure_call(const XrProgramSemanticClosure *closure, uint32_t index);

#endif  // XR_PROGRAM_SEMANTIC_CLOSURE_H
