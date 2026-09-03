/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program.h - Compiler-private CoreIR and canonical XrProgram writer
 */

#ifndef XR_PROGRAM_H
#define XR_PROGRAM_H

#include "../base/xdefs.h"
#include "../shared/xr_param_mode.h"
#include "../shared/xr_view_origin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_PROGRAM_DIGEST_SIZE 32u
#define XR_CORE_IR_KEY_SIZE 32u
#define XR_PROGRAM_FUNCTION_ENTRY UINT32_C(1)
#define XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE UINT16_C(16)

typedef enum XrProgramTypeKind {
    XR_PROGRAM_TYPE_KIND_VOID = 0,
    XR_PROGRAM_TYPE_KIND_BOOL = 1,
    XR_PROGRAM_TYPE_KIND_I64 = 2,
    XR_PROGRAM_TYPE_KIND_U32 = 3,
    XR_PROGRAM_TYPE_KIND_ERROR = 4,
    XR_PROGRAM_TYPE_KIND_AGGREGATE = 16,
    XR_PROGRAM_TYPE_KIND_VARIANT = 17,
    XR_PROGRAM_TYPE_KIND_VIEW = 18,
    XR_PROGRAM_TYPE_KIND_CALLABLE = 19,
    XR_PROGRAM_TYPE_KIND_EXISTENTIAL = 20,
} XrProgramTypeKind;

typedef struct XrCoreIrKey {
    uint8_t bytes[XR_CORE_IR_KEY_SIZE];
} XrCoreIrKey;

typedef enum XrCoreIrTypeKind {
    XR_CORE_IR_TYPE_AGGREGATE = 1,
    XR_CORE_IR_TYPE_VARIANT = 2,
    XR_CORE_IR_TYPE_VIEW = 3,
    XR_CORE_IR_TYPE_CALLABLE = 4,
    XR_CORE_IR_TYPE_EXISTENTIAL = 5,
} XrCoreIrTypeKind;

typedef enum XrCoreIrNominalKind {
    XR_CORE_IR_NOMINAL_NONE = 0,
    XR_CORE_IR_NOMINAL_CLASS = 1,
    XR_CORE_IR_NOMINAL_STRUCT = 2,
    XR_CORE_IR_NOMINAL_ENUM = 3,
} XrCoreIrNominalKind;

typedef enum XrCoreIrInterfaceUseKind {
    XR_CORE_IR_INTERFACE_CONSTRAINT_BOUND = 0,
    XR_CORE_IR_INTERFACE_EXISTENTIAL_READ = 1,
    XR_CORE_IR_INTERFACE_EXISTENTIAL_REF = 2,
    XR_CORE_IR_INTERFACE_EXISTENTIAL_MOVE = 3,
    XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE = 4,
} XrCoreIrInterfaceUseKind;

typedef struct XrCoreIrCallableSignatureInput XrCoreIrCallableSignatureInput;

typedef enum XrCoreIrViewCapability {
    XR_CORE_IR_VIEW_READ = 1,
    XR_CORE_IR_VIEW_WRITE_EXCLUSIVE = 2,
} XrCoreIrViewCapability;

/* Logical ownership is part of TypeId semantics, while physical retain,
 * release, allocation, and layout remain executor-private. */
typedef enum XrCoreIrTypeOwnership {
    XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL = 0,
    XR_CORE_IR_TYPE_OWNERSHIP_AFFINE = 1,
} XrCoreIrTypeOwnership;

typedef enum XrCoreIrCopyContract {
    XR_CORE_IR_COPY_TRIVIAL = 0,
    XR_CORE_IR_COPY_EXPLICIT = 1,
    XR_CORE_IR_COPY_FORBIDDEN = 2,
} XrCoreIrCopyContract;

typedef struct XrCoreIrVariantInput {
    const uint16_t *payload_types;
    uint32_t payload_count;
} XrCoreIrVariantInput;

/* Dynamic CoreIR type IDs are compiler-local labels. The builder canonicalizes
 * them by semantic key and rewrites every reference before XrProgram encoding.
 * Field order and variant order are semantic declaration order; no offset,
 * alignment, slot, register, or target ABI fact is admitted here. */
typedef struct XrCoreIrTypeInput {
    XrCoreIrKey key;
    uint16_t local_id;
    XrCoreIrTypeKind kind;
    XrCoreIrNominalKind nominal_kind;
    XrCoreIrTypeOwnership ownership;
    XrCoreIrCopyContract copy_contract;
    const uint16_t *field_types;
    uint32_t field_count;
    const XrCoreIrVariantInput *variants;
    uint32_t variant_count;
    uint16_t view_element_type;
    XrCoreIrViewCapability view_capability;
    const XrCoreIrCallableSignatureInput *callable_signature;
    XrCoreIrKey existential_interface;
    XrCoreIrInterfaceUseKind interface_use_kind;
} XrCoreIrTypeInput;

typedef enum XrCoreIrConstantKind {
    XR_CORE_IR_CONSTANT_I64 = 1,
    XR_CORE_IR_CONSTANT_BOOL = 2,
} XrCoreIrConstantKind;

typedef struct XrCoreIrConstantInput {
    XrCoreIrKey key;
    uint16_t type_id;
    XrCoreIrConstantKind kind;
    union {
        int64_t i64;
        bool boolean;
    } value;
} XrCoreIrConstantInput;

typedef enum XrCoreIrImmediateKind {
    XR_CORE_IR_IMMEDIATE_NONE = 0,
    XR_CORE_IR_IMMEDIATE_I64,
    XR_CORE_IR_IMMEDIATE_U32,
    XR_CORE_IR_IMMEDIATE_BOOL,
    XR_CORE_IR_IMMEDIATE_CONSTANT,
    XR_CORE_IR_IMMEDIATE_FUNCTION,
    XR_CORE_IR_IMMEDIATE_FIELD,
    XR_CORE_IR_IMMEDIATE_VARIANT,
    XR_CORE_IR_IMMEDIATE_VARIANT_FIELD,
    XR_CORE_IR_IMMEDIATE_TYPE,
} XrCoreIrImmediateKind;

/* A place is a verifier-confined SSA capability naming typed storage. It is
 * never a language type and therefore never enters the XrProgram type table. */
typedef enum XrCoreIrValueCategory {
    XR_CORE_IR_VALUE = 0,
    XR_CORE_IR_PLACE = 1,
} XrCoreIrValueCategory;

/* OWNER is an exactly-once logical token. NON_OWNER is either a trivial value
 * or a call-bound borrow, as determined by TypeId and the surrounding
 * signature. */
typedef enum XrCoreIrOwnershipDisposition {
    XR_CORE_IR_NON_OWNER = 0,
    XR_CORE_IR_OWNER = 1,
} XrCoreIrOwnershipDisposition;

/* A callable signature is the sole semantic call contract. Functions,
 * callable values, and interface slots are canonicalized to one SignatureId
 * table; none of those consumers owns a duplicate wire signature. */
struct XrCoreIrCallableSignatureInput {
    const uint16_t *parameter_types;
    const XrParamMode *parameter_modes;
    uint32_t parameter_count;
    bool has_receiver;
    XrParamMode receiver_mode;
    uint16_t result_type_id;
    XrCoreIrOwnershipDisposition result_ownership;
    const XrViewOrigin *result_borrow_origins;
    uint32_t result_borrow_origin_count;
    uint16_t error_type_id;
    uint16_t panic_type_id;
    uint32_t effect_mask;
    uint32_t capability_mask;
};

typedef struct XrCoreIrValueInput {
    XrCoreIrKey key;
    uint16_t type_id;
    XrCoreIrValueCategory category;
    XrCoreIrOwnershipDisposition ownership;
} XrCoreIrValueInput;

typedef struct XrCoreIrInstructionInput {
    uint16_t operation_id;
    XrCoreIrKey result;
    uint16_t result_type_id;
    XrCoreIrValueCategory result_category;
    XrCoreIrOwnershipDisposition result_ownership;
    const XrCoreIrKey *operands;
    uint32_t operand_count;
    XrCoreIrImmediateKind immediate_kind;
    union {
        int64_t i64;
        uint32_t u32;
        bool boolean;
        XrCoreIrKey key;
        uint32_t field_ordinal;
        uint32_t variant_ordinal;
        struct {
            uint32_t variant_ordinal;
            uint32_t field_ordinal;
        } variant_field;
        uint16_t type_id;
    } immediate;
    const XrCoreIrKey *successors;
    uint32_t successor_count;
} XrCoreIrInstructionInput;

typedef struct XrCoreIrBlockInput {
    XrCoreIrKey key;
    const XrCoreIrValueInput *arguments;
    uint32_t argument_count;
    const XrCoreIrInstructionInput *instructions;
    uint32_t instruction_count;
} XrCoreIrBlockInput;

/* RootId is a function-local semantic identity independent of SSA ValueId.
 * PARAMETER ordinals exclude an optional receiver. LOCAL roots name the value
 * that creates a fresh logical identity; no physical allocation fact is
 * encoded. STATIC has neither a parameter nor a source value. */
typedef enum XrCoreIrRootKind {
    XR_CORE_IR_ROOT_PARAMETER = 0,
    XR_CORE_IR_ROOT_RECEIVER = 1,
    XR_CORE_IR_ROOT_STATIC = 2,
    XR_CORE_IR_ROOT_LOCAL = 3,
} XrCoreIrRootKind;

typedef struct XrCoreIrRootInput {
    XrCoreIrKey key;
    XrCoreIrRootKind kind;
    int32_t parameter_ordinal;
    XrCoreIrKey source_value;
} XrCoreIrRootInput;

/* A root-bearing SSA value has one canonical RootId set. Affine owners and
 * places have exactly one root; views may carry multiple roots. */
typedef struct XrCoreIrValueRootSetInput {
    XrCoreIrKey value;
    const XrCoreIrKey *roots;
    uint32_t root_count;
} XrCoreIrValueRootSetInput;

typedef struct XrCoreIrFunctionInput {
    XrCoreIrKey key;
    const uint16_t *parameter_types;
    const XrParamMode *parameter_modes;
    uint32_t parameter_count;
    bool has_receiver;
    XrParamMode receiver_mode;
    uint16_t result_type_id;
    XrCoreIrOwnershipDisposition result_ownership;
    const XrViewOrigin *result_borrow_origins;
    uint32_t result_borrow_origin_count;
    /* VOID denotes an infallible function. A non-VOID TypeId is the exact
     * value transferred by core.error.publish and sealed-invoke's error edge. */
    uint16_t error_type_id;
    /* VOID denotes panic-free. PANIC_INFO is transferred only by
     * core.panic.publish or sealed-invoke's panic edge. */
    uint16_t panic_type_id;
    uint32_t effect_mask;
    uint32_t capability_mask;
    XrCoreIrKey entry_block;
    const XrCoreIrBlockInput *blocks;
    uint32_t block_count;
    const XrCoreIrRootInput *roots;
    uint32_t root_count;
    const XrCoreIrValueRootSetInput *value_root_sets;
    uint32_t value_root_set_count;
    uint32_t flags;
} XrCoreIrFunctionInput;

typedef struct XrCoreIrModuleInput {
    XrCoreIrKey key;
    const XrCoreIrConstantInput *constants;
    uint32_t constant_count;
    const XrCoreIrFunctionInput *functions;
    uint32_t function_count;
} XrCoreIrModuleInput;

typedef struct XrCoreIrInterfaceInput {
    XrCoreIrKey key;
    const XrCoreIrCallableSignatureInput *slots;
    uint32_t slot_count;
} XrCoreIrInterfaceInput;

typedef struct XrCoreIrConformanceInput {
    XrCoreIrKey key;
    uint16_t implementor_type_id;
    XrCoreIrNominalKind implementor_kind;
    XrCoreIrKey interface_key;
    const XrCoreIrKey *slot_functions;
    uint32_t slot_count;
} XrCoreIrConformanceInput;

typedef struct XrCoreIrProgramInput {
    const uint8_t *semantic_profile_fingerprint;
    const uint16_t *required_features;
    uint32_t required_feature_count;
    const XrCoreIrTypeInput *types;
    uint32_t type_count;
    const XrCoreIrInterfaceInput *interfaces;
    uint32_t interface_count;
    const XrCoreIrConformanceInput *conformances;
    uint32_t conformance_count;
    const XrCoreIrModuleInput *modules;
    uint32_t module_count;
} XrCoreIrProgramInput;

typedef struct XrCoreIrProgram XrCoreIrProgram;

typedef struct XrProgramId {
    uint8_t bytes[XR_PROGRAM_DIGEST_SIZE];
} XrProgramId;

typedef struct XrProgramArtifact {
    uint8_t *bytes;
    size_t size;
    XrProgramId id;
} XrProgramArtifact;

typedef enum XrProgramBuildStatus {
    XR_PROGRAM_BUILD_OK = 0,
    XR_PROGRAM_BUILD_INVALID_INPUT,
    XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
    XR_PROGRAM_BUILD_DUPLICATE_IDENTITY,
    XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
    XR_PROGRAM_BUILD_RESOURCE_LIMIT,
    XR_PROGRAM_BUILD_OUT_OF_MEMORY,
} XrProgramBuildStatus;

XR_FUNC XrCoreIrKey xr_core_ir_key(const void *semantic_bytes, size_t semantic_size);
XR_FUNC bool xr_core_ir_key_equal(XrCoreIrKey left, XrCoreIrKey right);
XR_FUNC bool xr_core_ir_key_is_zero(XrCoreIrKey key);

XR_FUNC XrProgramBuildStatus xr_core_ir_program_build(const XrCoreIrProgramInput *input,
                                                      XrCoreIrProgram **program_out,
                                                      char *diagnostic, size_t diagnostic_size);
XR_FUNC void xr_core_ir_program_free(XrCoreIrProgram *program);

XR_FUNC XrProgramBuildStatus xr_program_write(const XrCoreIrProgram *program,
                                              XrProgramArtifact *artifact_out, char *diagnostic,
                                              size_t diagnostic_size);
XR_FUNC void xr_program_artifact_free(XrProgramArtifact *artifact);
XR_FUNC void xr_program_compute_id(const uint8_t *bytes, size_t size, XrProgramId *id_out);
XR_FUNC bool xr_program_id_equal(XrProgramId left, XrProgramId right);
XR_FUNC void xr_program_id_hex(XrProgramId id, char output[XR_PROGRAM_DIGEST_SIZE * 2u + 1u]);
XR_FUNC const char *xr_program_build_status_name(XrProgramBuildStatus status);

#endif /* XR_PROGRAM_H */
