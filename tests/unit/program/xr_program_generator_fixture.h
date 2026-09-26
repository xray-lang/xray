#ifndef XR_PROGRAM_GENERATOR_FIXTURE_H
#define XR_PROGRAM_GENERATOR_FIXTURE_H

#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"

#include <string.h>

enum {
    XR_GENERATOR_FIXTURE_HANDLE_TYPE = 100,
    XR_GENERATOR_FIXTURE_OUTCOME_TYPE = 101,
};

typedef enum XrProgramGeneratorFixtureMutation {
    XR_PROGRAM_GENERATOR_FIXTURE_VALID = 0,
    XR_PROGRAM_GENERATOR_FIXTURE_CREATE_TARGET_NOT_GENERATOR,
    XR_PROGRAM_GENERATOR_FIXTURE_YIELD_TYPE_MISMATCH,
    XR_PROGRAM_GENERATOR_FIXTURE_RESUME_OUTCOME_MISMATCH,
} XrProgramGeneratorFixtureMutation;

static XrCoreIrKey xr_program_generator_fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrProgramBuildStatus xr_program_generator_fixture_write_mutated(
    XrProgramGeneratorFixtureMutation mutation, XrProgramArtifact *artifact, char *diagnostic,
    size_t diagnostic_size) {
    XrCoreIrVariantInput outcome_variants[] = {
        {.payload_types = (const uint16_t[]) {XR_CORE_TYPE_I64}, .payload_count = 1u},
        {0},
        {.payload_types = (const uint16_t[]) {XR_CORE_TYPE_ERROR}, .payload_count = 1u},
        {.payload_types = (const uint16_t[]) {XR_CORE_TYPE_PANIC_INFO}, .payload_count = 1u},
        {0},
    };
    if (mutation == XR_PROGRAM_GENERATOR_FIXTURE_RESUME_OUTCOME_MISMATCH)
        outcome_variants[0].payload_types = (const uint16_t[]) {XR_CORE_TYPE_BOOL};
    XrCoreIrTypeInput types[] = {
        {.key = xr_program_generator_fixture_key("generator:type:handle"),
         .local_id = XR_GENERATOR_FIXTURE_HANDLE_TYPE,
         .kind = XR_CORE_IR_TYPE_AGGREGATE,
         .nominal_kind = XR_CORE_IR_NOMINAL_CLASS,
         .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
         .copy_contract = XR_CORE_IR_COPY_FORBIDDEN},
        {.key = xr_program_generator_fixture_key("generator:type:outcome"),
         .local_id = XR_GENERATOR_FIXTURE_OUTCOME_TYPE,
         .kind = XR_CORE_IR_TYPE_VARIANT,
         .nominal_kind = XR_CORE_IR_NOMINAL_ENUM,
         .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
         .copy_contract = XR_CORE_IR_COPY_FORBIDDEN,
         .variants = outcome_variants,
         .variant_count = 5u},
    };

    XrCoreIrKey constant_key = xr_program_generator_fixture_key("generator:constant:42");
    XrCoreIrConstantInput constant = {.key = constant_key,
                                      .type_id = XR_CORE_TYPE_I64,
                                      .kind = XR_CORE_IR_CONSTANT_I64,
                                      .value.i64 = 42};
    XrCoreIrKey target_key = xr_program_generator_fixture_key("generator:function:target");
    XrCoreIrKey target_entry = xr_program_generator_fixture_key("generator:block:target-entry");
    XrCoreIrKey target_resume = xr_program_generator_fixture_key("generator:block:target-resume");
    XrCoreIrKey yielded = xr_program_generator_fixture_key("generator:value:yielded");
    XrCoreIrKey yield_operands[] = {yielded};
    XrCoreIrKey yield_successors[] = {target_resume};
    XrCoreIrInstructionInput target_entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = yielded,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant_key},
        {.operation_id = XR_CORE_OP_CORE_GENERATOR_YIELD,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = yield_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
         .immediate.u32 = 0u,
         .successors = yield_successors,
         .successor_count = 1u},
    };
    if (mutation == XR_PROGRAM_GENERATOR_FIXTURE_YIELD_TYPE_MISMATCH)
        target_entry_instructions[0].result_type_id = XR_CORE_TYPE_BOOL;
    XrCoreIrInstructionInput target_resume_instruction = {
        .operation_id = XR_CORE_OP_CORE_RETURN,
        .result_type_id = XR_CORE_TYPE_VOID,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    XrCoreIrBlockInput target_blocks[] = {
        {.key = target_entry,
         .instructions = target_entry_instructions,
         .instruction_count = 2u},
        {.key = target_resume, .instructions = &target_resume_instruction, .instruction_count = 1u},
    };
    XrCoreIrCoroutineStateInput states[] = {
        {.state_id = 0u, .continuation_block = target_entry},
        {.state_id = 1u, .continuation_block = target_resume},
    };
    XrCoreIrCoroutineSafepointInput safepoint = {.safepoint_id = 0u, .resume_state_id = 1u};
    XrCoreIrFunctionInput target = {
        .key = target_key,
        .result_type_id = XR_GENERATOR_FIXTURE_HANDLE_TYPE,
        .result_ownership = XR_CORE_IR_OWNER,
        .effect_mask = XR_CORE_EFFECT_SUSPEND,
        .capability_mask = XR_CORE_CAPABILITY_RUNTIME_COOPERATIVE_YIELD,
        .entry_block = target_entry,
        .blocks = target_blocks,
        .block_count = 2u,
        .coroutine_states = states,
        .coroutine_state_count = 2u,
        .coroutine_safepoints = &safepoint,
        .coroutine_safepoint_count = 1u,
        .flags = mutation == XR_PROGRAM_GENERATOR_FIXTURE_CREATE_TARGET_NOT_GENERATOR
                     ? 0u
                     : XR_PROGRAM_FUNCTION_GENERATOR,
    };

    XrCoreIrKey entry_key = xr_program_generator_fixture_key("generator:function:entry");
    XrCoreIrKey entry_block = xr_program_generator_fixture_key("generator:block:entry");
    XrCoreIrKey handle = xr_program_generator_fixture_key("generator:value:handle");
    XrCoreIrKey outcome = xr_program_generator_fixture_key("generator:value:outcome");
    XrCoreIrKey is_yielded = xr_program_generator_fixture_key("generator:value:is-yielded");
    XrCoreIrKey resume_operands[] = {handle};
    XrCoreIrKey test_operands[] = {outcome};
    XrCoreIrKey return_operands[] = {is_yielded};
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_GENERATOR_CREATE,
         .result = handle,
         .result_type_id = XR_GENERATOR_FIXTURE_HANDLE_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = target_key},
        {.operation_id = XR_CORE_OP_CORE_GENERATOR_RESUME,
         .result = outcome,
         .result_type_id = XR_GENERATOR_FIXTURE_OUTCOME_TYPE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = resume_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_VARIANT_TEST,
         .result = is_yielded,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = test_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT,
         .immediate.variant_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = test_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = resume_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = return_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput driver_block = {
        .key = entry_block,
        .instructions = entry_instructions,
        .instruction_count = 6u,
    };
    XrCoreIrFunctionInput entry = {
        .key = entry_key,
        .result_type_id = XR_CORE_TYPE_BOOL,
        .effect_mask = XR_CORE_EFFECT_CALL,
        .capability_mask = XR_CORE_CAPABILITY_RUNTIME_COOPERATIVE_YIELD,
        .entry_block = entry_block,
        .blocks = &driver_block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrFunctionInput functions[] = {target, entry};
    XrCoreIrModuleInput module = {
        .key = xr_program_generator_fixture_key("generator:module"),
        .constants = &constant,
        .constant_count = 1u,
        .functions = functions,
        .function_count = 2u,
    };
    uint8_t profile[XR_PROGRAM_DIGEST_SIZE] = {0};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile,
        .required_features = &feature,
        .required_feature_count = 1u,
        .types = types,
        .type_count = 2u,
        .modules = &module,
        .module_count = 1u,
    };
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status =
        xr_core_ir_program_build(&input, &program, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, diagnostic_size);
    xr_core_ir_program_free(program);
    return status;
}

static XrProgramBuildStatus xr_program_generator_fixture_write(XrProgramArtifact *artifact,
                                                               char *diagnostic,
                                                               size_t diagnostic_size) {
    return xr_program_generator_fixture_write_mutated(XR_PROGRAM_GENERATOR_FIXTURE_VALID, artifact,
                                                      diagnostic, diagnostic_size);
}

#endif
