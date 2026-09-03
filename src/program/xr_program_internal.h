#ifndef XR_PROGRAM_INTERNAL_H
#define XR_PROGRAM_INTERNAL_H

#include "xr_program.h"

typedef struct XrCoreIrVariant {
    uint16_t *payload_types;
    uint32_t payload_count;
} XrCoreIrVariant;

typedef struct XrCoreIrCallableSignature {
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
} XrCoreIrCallableSignature;

typedef struct XrCoreIrType {
    XrCoreIrKey key;
    uint16_t type_id;
    XrCoreIrTypeKind kind;
    XrCoreIrNominalKind nominal_kind;
    XrCoreIrTypeOwnership ownership;
    XrCoreIrCopyContract copy_contract;
    uint16_t *field_types;
    uint32_t field_count;
    XrCoreIrVariant *variants;
    uint32_t variant_count;
    uint16_t view_element_type;
    XrCoreIrViewCapability view_capability;
    XrCoreIrCallableSignature callable_signature;
    XrCoreIrKey existential_interface;
    XrCoreIrInterfaceUseKind interface_use_kind;
} XrCoreIrType;

typedef struct XrCoreIrInstruction {
    uint16_t operation_id;
    XrCoreIrKey result;
    uint16_t result_type_id;
    XrCoreIrValueCategory result_category;
    XrCoreIrOwnershipDisposition result_ownership;
    XrCoreIrKey *operands;
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
    XrCoreIrKey *successors;
    uint32_t successor_count;
} XrCoreIrInstruction;

typedef struct XrCoreIrBlock {
    XrCoreIrKey key;
    XrCoreIrValueInput *arguments;
    uint32_t argument_count;
    XrCoreIrInstruction *instructions;
    uint32_t instruction_count;
} XrCoreIrBlock;

typedef struct XrCoreIrRoot {
    XrCoreIrKey key;
    XrCoreIrRootKind kind;
    int32_t parameter_ordinal;
    XrCoreIrKey source_value;
} XrCoreIrRoot;

typedef struct XrCoreIrValueRootSet {
    XrCoreIrKey value;
    XrCoreIrKey *roots;
    uint32_t root_count;
} XrCoreIrValueRootSet;

typedef struct XrCoreIrFunction {
    XrCoreIrKey key;
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
    XrCoreIrKey entry_block;
    XrCoreIrBlock *blocks;
    uint32_t block_count;
    XrCoreIrRoot *roots;
    uint32_t root_count;
    XrCoreIrValueRootSet *value_root_sets;
    uint32_t value_root_set_count;
    uint32_t flags;
} XrCoreIrFunction;

typedef struct XrCoreIrModule {
    XrCoreIrKey key;
    XrCoreIrConstantInput *constants;
    uint32_t constant_count;
    XrCoreIrFunction *functions;
    uint32_t function_count;
} XrCoreIrModule;

typedef struct XrCoreIrInterface {
    XrCoreIrKey key;
    XrCoreIrCallableSignature *slots;
    uint32_t slot_count;
} XrCoreIrInterface;

typedef struct XrCoreIrConformance {
    XrCoreIrKey key;
    uint16_t implementor_type_id;
    XrCoreIrNominalKind implementor_kind;
    XrCoreIrKey interface_key;
    XrCoreIrKey *slot_functions;
    uint32_t slot_count;
} XrCoreIrConformance;

struct XrCoreIrProgram {
    uint8_t semantic_profile_fingerprint[XR_PROGRAM_DIGEST_SIZE];
    uint16_t *required_features;
    uint32_t required_feature_count;
    XrCoreIrType *types;
    uint32_t type_count;
    XrCoreIrInterface *interfaces;
    uint32_t interface_count;
    XrCoreIrConformance *conformances;
    uint32_t conformance_count;
    XrCoreIrModule *modules;
    uint32_t module_count;
};

void xr_program_set_diagnostic(char *diagnostic, size_t diagnostic_size, const char *format, ...);

#endif /* XR_PROGRAM_INTERNAL_H */
