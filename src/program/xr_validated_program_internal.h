#ifndef XR_VALIDATED_PROGRAM_INTERNAL_H
#define XR_VALIDATED_PROGRAM_INTERNAL_H

#include "xr_program_verify.h"

#include <stdatomic.h>

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
    uint32_t *operands;
    uint32_t operand_count;
    XrCoreIrImmediateKind immediate_kind;
    union {
        int64_t i64;
        uint32_t u32;
        bool boolean;
        uint32_t constant_id;
        uint32_t function_id;
    } immediate;
    uint32_t *successors;
    uint32_t successor_count;
} XrValidatedInstruction;

typedef struct XrValidatedBlock {
    uint32_t *argument_ids;
    uint16_t *argument_types;
    uint32_t argument_count;
    XrValidatedInstruction *instructions;
    uint32_t instruction_count;
} XrValidatedBlock;

typedef struct XrValidatedFunction {
    uint16_t *parameter_types;
    uint32_t parameter_count;
    uint16_t result_type_id;
    uint32_t effect_mask;
    uint32_t capability_mask;
    uint32_t entry_block;
    XrValidatedBlock *blocks;
    uint32_t block_count;
    uint16_t *value_types;
    uint32_t *value_blocks;
    uint32_t *value_positions;
    uint32_t value_count;
    uint32_t flags;
} XrValidatedFunction;

struct XrValidatedProgram {
    atomic_uint_least32_t references;
    uint8_t *bytes;
    size_t size;
    XrProgramId id;
    uint8_t semantic_profile_fingerprint[XR_PROGRAM_DIGEST_SIZE];
    XrValidatedConstant *constants;
    uint32_t constant_count;
    XrValidatedFunction *functions;
    uint32_t function_count;
    uint32_t entry_function;
    uint64_t verifier_work;
};

#endif /* XR_VALIDATED_PROGRAM_INTERNAL_H */
