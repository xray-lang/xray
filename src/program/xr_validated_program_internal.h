#ifndef XR_VALIDATED_PROGRAM_INTERNAL_H
#define XR_VALIDATED_PROGRAM_INTERNAL_H

#include "../core/xr_core_spec_gen.h"
#include "xr_program_verify.h"

#include <stdatomic.h>

typedef struct XrValidatedVariant {
    uint16_t *payload_types;
    uint32_t payload_count;
} XrValidatedVariant;

typedef struct XrValidatedType {
    XrCoreIrKey key;
    uint16_t type_id;
    XrCoreIrTypeKind kind;
    XrCoreIrNominalKind nominal_kind;
    XrCoreIrTypeOwnership ownership;
    XrCoreIrCopyContract copy_contract;
    uint16_t *field_types;
    uint32_t field_count;
    XrValidatedVariant *variants;
    uint32_t variant_count;
    uint16_t view_element_type;
    XrCoreIrViewCapability view_capability;
    uint32_t signature_id;
    uint32_t interface_id;
    XrCoreIrInterfaceUseKind interface_use_kind;
} XrValidatedType;

typedef struct XrValidatedSignature {
    uint16_t *parameter_types;
    XrParamMode *parameter_modes;
    uint32_t parameter_count;
    bool has_receiver;
    XrParamMode receiver_mode;
    uint16_t result_type_id;
    XrCoreIrOwnershipDisposition result_ownership;
    XrViewOrigin *result_borrow_origins;
    uint32_t result_borrow_origin_count;
    uint16_t error_type_id;
    uint16_t panic_type_id;
    uint32_t effect_mask;
    uint32_t capability_mask;
} XrValidatedSignature;

typedef struct XrValidatedInterface {
    XrCoreIrKey key;
    uint32_t *slot_signature_ids;
    uint32_t slot_count;
} XrValidatedInterface;

typedef struct XrValidatedConformance {
    XrCoreIrKey key;
    uint16_t implementor_type_id;
    XrCoreIrNominalKind implementor_kind;
    uint32_t interface_id;
    uint32_t *slot_function_ids;
    uint32_t slot_count;
} XrValidatedConformance;

typedef struct XrValidatedRoot {
    XrCoreIrRootKind kind;
    uint32_t parameter_ordinal;
    uint32_t source_value_id;
} XrValidatedRoot;

typedef struct XrValidatedValueRootSet {
    uint32_t *root_ids;
    uint32_t root_count;
} XrValidatedValueRootSet;

typedef struct XrValidatedConstant {
    uint16_t type_id;
    XrCoreIrConstantKind kind;
    union {
        int64_t i64;
        bool boolean;
    } value;
} XrValidatedConstant;

typedef struct XrValidatedInstruction {
    uint16_t operation_id;
    uint32_t result_id;
    uint16_t result_type_id;
    XrCoreIrValueCategory result_category;
    XrCoreIrOwnershipDisposition result_ownership;
    uint32_t *operands;
    uint32_t operand_count;
    XrCoreIrImmediateKind immediate_kind;
    union {
        int64_t i64;
        uint32_t u32;
        bool boolean;
        uint32_t constant_id;
        uint32_t function_id;
        uint32_t field_ordinal;
        uint32_t variant_ordinal;
        struct {
            uint32_t variant_ordinal;
            uint32_t field_ordinal;
        } variant_field;
        uint16_t type_id;
    } immediate;
    uint32_t *successors;
    uint32_t successor_count;
} XrValidatedInstruction;

typedef struct XrValidatedBlock {
    uint32_t *argument_ids;
    uint16_t *argument_types;
    XrCoreIrValueCategory *argument_categories;
    XrCoreIrOwnershipDisposition *argument_ownerships;
    uint32_t argument_count;
    XrValidatedInstruction *instructions;
    uint32_t instruction_count;
} XrValidatedBlock;

typedef struct XrValidatedFunction {
    uint32_t signature_id;
    uint16_t *parameter_types;
    XrParamMode *parameter_modes;
    uint32_t parameter_count;
    bool has_receiver;
    XrParamMode receiver_mode;
    uint16_t result_type_id;
    XrCoreIrOwnershipDisposition result_ownership;
    XrViewOrigin *result_borrow_origins;
    uint32_t result_borrow_origin_count;
    uint16_t error_type_id;
    uint16_t panic_type_id;
    uint32_t effect_mask;
    uint32_t capability_mask;
    uint32_t entry_block;
    XrValidatedBlock *blocks;
    uint32_t block_count;
    uint16_t *value_types;
    XrCoreIrValueCategory *value_categories;
    XrCoreIrOwnershipDisposition *value_ownerships;
    uint32_t *value_blocks;
    uint32_t *value_positions;
    XrValidatedRoot *roots;
    uint32_t root_count;
    XrValidatedValueRootSet *value_root_sets;
    uint32_t value_count;
    uint32_t flags;
} XrValidatedFunction;

struct XrValidatedProgram {
    atomic_uint_least32_t references;
    uint8_t *bytes;
    size_t size;
    XrProgramId id;
    uint8_t semantic_profile_fingerprint[XR_PROGRAM_DIGEST_SIZE];
    XrValidatedType *types;
    uint32_t type_count;
    XrValidatedSignature *signatures;
    uint32_t signature_count;
    XrValidatedInterface *interfaces;
    uint32_t interface_count;
    XrValidatedConformance *conformances;
    uint32_t conformance_count;
    XrValidatedConstant *constants;
    uint32_t constant_count;
    XrValidatedFunction *functions;
    uint32_t function_count;
    uint32_t entry_function;
    uint64_t verifier_work;
};

static inline bool xr_program_type_id_is_dynamic(uint16_t type_id) {
    return type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE;
}

static inline const XrValidatedType *xr_validated_program_type(const XrValidatedProgram *program,
                                                               uint16_t type_id) {
    if (!program || !xr_program_type_id_is_dynamic(type_id))
        return NULL;
    uint32_t index = (uint32_t) type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE;
    return index < program->type_count ? &program->types[index] : NULL;
}

static inline XrCoreIrTypeOwnership
xr_validated_program_type_ownership(const XrValidatedProgram *program, uint16_t type_id) {
    if (type_id == XR_CORE_TYPE_PANIC_INFO)
        return XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
    const XrValidatedType *type = xr_validated_program_type(program, type_id);
    return type ? type->ownership : XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL;
}

static inline XrCoreIrCopyContract
xr_validated_program_copy_contract(const XrValidatedProgram *program, uint16_t type_id) {
    if (type_id == XR_CORE_TYPE_VOID || type_id == XR_CORE_TYPE_PANIC_INFO)
        return XR_CORE_IR_COPY_FORBIDDEN;
    const XrValidatedType *type = xr_validated_program_type(program, type_id);
    return type ? type->copy_contract : XR_CORE_IR_COPY_TRIVIAL;
}

#endif /* XR_VALIDATED_PROGRAM_INTERNAL_H */
