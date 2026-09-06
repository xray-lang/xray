#ifndef XR_PROGRAM_TRAP_FIXTURE_H
#define XR_PROGRAM_TRAP_FIXTURE_H

#include "core/xr_core_spec_gen.h"
#include "plan/semantic/xr_semantic_ids.h"
#include "program/xr_program.h"
#include "runtime/abi/xr_builtin_provider_contract.h"

#include <string.h>

typedef enum XrProgramTrapFixtureMutation {
    XR_PROGRAM_TRAP_FIXTURE_VALID = 0,
    XR_PROGRAM_TRAP_FIXTURE_EXTRA_SUCCESSOR,
    XR_PROGRAM_TRAP_FIXTURE_WRONG_TRAP,
} XrProgramTrapFixtureMutation;

static XrCoreIrKey xr_program_trap_fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrProgramBuildStatus xr_program_trap_fixture_write_with_ids_mutated(
    XrStableId contract_id, XrStableId operation_id, XrProgramTrapFixtureMutation mutation,
    XrProgramArtifact *artifact, char *diagnostic, size_t diagnostic_size) {
    XrCoreIrKey entry_key = xr_program_trap_fixture_key("trap:block:entry");
    XrCoreIrKey trap_key = xr_program_trap_fixture_key("trap:block:provider-failed");
    XrCoreIrKey result = xr_program_trap_fixture_key("trap:value:result");
    XrCoreIrKey returned[] = {result};
    XrCoreIrKey successors[] = {trap_key};
    XrCoreIrKey extra_successors[] = {trap_key, trap_key};
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_PROVIDER_CALL,
         .result = result,
         .result_type_id = XR_CORE_TYPE_I64,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
         .immediate.provider_operation = {.contract_id = contract_id,
                                          .operation_id = operation_id},
         .successors = mutation == XR_PROGRAM_TRAP_FIXTURE_EXTRA_SUCCESSOR ? extra_successors
                                                                           : successors,
         .successor_count = mutation == XR_PROGRAM_TRAP_FIXTURE_EXTRA_SUCCESSOR ? 2u : 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = returned,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrInstructionInput trap_instruction = {
        .operation_id = XR_CORE_OP_CORE_TRAP,
        .result_type_id = XR_CORE_TYPE_VOID,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
        .immediate.u32 = mutation == XR_PROGRAM_TRAP_FIXTURE_WRONG_TRAP ? 4u : 7u,
    };
    XrCoreIrBlockInput blocks[] = {
        {.key = entry_key,
         .instructions = entry_instructions,
         .instruction_count = sizeof(entry_instructions) / sizeof(entry_instructions[0])},
        {.key = trap_key, .instructions = &trap_instruction, .instruction_count = 1u},
    };
    XrCoreIrFunctionInput function = {
        .key = xr_program_trap_fixture_key("trap:function:entry"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL,
        .capability_mask = XR_CORE_CAPABILITY_PROVIDER_BINDING,
        .entry_block = entry_key,
        .blocks = blocks,
        .block_count = sizeof(blocks) / sizeof(blocks[0]),
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = xr_program_trap_fixture_key("trap:module"),
        .functions = &function,
        .function_count = 1u,
    };
    XrCoreIrProviderRequirementInput requirement = {
        .contract_id = contract_id,
        .operation_ids = &operation_id,
        .operation_count = 1u,
    };
    XrCoreIrKey semantic = xr_program_trap_fixture_key("trap:semantic");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = semantic.bytes,
        .required_features = &feature,
        .required_feature_count = 1u,
        .provider_requirements = &requirement,
        .provider_requirement_count = 1u,
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

static XrProgramBuildStatus xr_program_trap_fixture_write_mutated(
    XrProgramTrapFixtureMutation mutation, XrProgramArtifact *artifact, char *diagnostic,
    size_t diagnostic_size) {
    XrStableId contract_id = {{0}};
    XrStableId operation_id = {{0}};
    XrFingerprint key_digest;
    if (!xr_stable_id_from_key(XR_PROVIDER_CLOCK_CONTRACT_KEY, &contract_id, &key_digest) ||
        !xr_stable_id_from_key(XR_PROVIDER_CLOCK_REALTIME_NANOS_OPERATION_KEY, &operation_id,
                               &key_digest))
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    return xr_program_trap_fixture_write_with_ids_mutated(
        contract_id, operation_id, mutation, artifact, diagnostic, diagnostic_size);
}

static inline XrProgramBuildStatus xr_program_trap_fixture_write(
    XrProgramArtifact *artifact, char *diagnostic, size_t diagnostic_size) {
    return xr_program_trap_fixture_write_mutated(XR_PROGRAM_TRAP_FIXTURE_VALID, artifact,
                                                  diagnostic, diagnostic_size);
}

static inline XrProgramBuildStatus xr_program_trap_fixture_write_with_ids(
    XrStableId contract_id, XrStableId operation_id, XrProgramArtifact *artifact,
    char *diagnostic, size_t diagnostic_size) {
    return xr_program_trap_fixture_write_with_ids_mutated(
        contract_id, operation_id, XR_PROGRAM_TRAP_FIXTURE_VALID, artifact, diagnostic,
        diagnostic_size);
}

#endif /* XR_PROGRAM_TRAP_FIXTURE_H */
