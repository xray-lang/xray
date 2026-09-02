#ifndef XR_PROGRAM_INTERNAL_H
#define XR_PROGRAM_INTERNAL_H

#include "xr_program.h"

typedef struct XrCoreIrVariant {
    uint16_t *payload_types;
    uint32_t payload_count;
} XrCoreIrVariant;

typedef struct XrCoreIrType {
    XrCoreIrKey key;
    uint16_t type_id;
    XrCoreIrTypeKind kind;
    XrCoreIrTypeOwnership ownership;
    XrCoreIrCopyContract copy_contract;
    uint16_t *field_types;
    uint32_t field_count;
    XrCoreIrVariant *variants;
    uint32_t variant_count;
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

typedef struct XrCoreIrFunction {
    XrCoreIrKey key;
    uint16_t *parameter_types;
    XrParamMode *parameter_modes;
    uint32_t parameter_count;
    uint16_t result_type_id;
    XrCoreIrOwnershipDisposition result_ownership;
    uint16_t error_type_id;
    uint32_t effect_mask;
    uint32_t capability_mask;
    XrCoreIrKey entry_block;
    XrCoreIrBlock *blocks;
    uint32_t block_count;
    uint32_t flags;
} XrCoreIrFunction;

typedef struct XrCoreIrModule {
    XrCoreIrKey key;
    XrCoreIrConstantInput *constants;
    uint32_t constant_count;
    XrCoreIrFunction *functions;
    uint32_t function_count;
} XrCoreIrModule;

struct XrCoreIrProgram {
    uint8_t semantic_profile_fingerprint[XR_PROGRAM_DIGEST_SIZE];
    uint16_t *required_features;
    uint32_t required_feature_count;
    XrCoreIrType *types;
    uint32_t type_count;
    XrCoreIrModule *modules;
    uint32_t module_count;
};

void xr_program_set_diagnostic(char *diagnostic, size_t diagnostic_size, const char *format, ...);

#endif /* XR_PROGRAM_INTERNAL_H */
