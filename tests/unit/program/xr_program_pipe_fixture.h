#ifndef XR_PROGRAM_PIPE_FIXTURE_H
#define XR_PROGRAM_PIPE_FIXTURE_H

#include "core/xr_core_spec_gen.h"
#include "plan/semantic/xr_semantic_ids.h"
#include "program/xr_program.h"
#include "xr_program_provider_fixture.h"
#include "runtime/abi/xr_builtin_provider_contract.h"

#include <string.h>

static XrCoreIrKey xr_program_pipe_fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

/* The entry returns whether pipe-open produced Some((read, write)).  This keeps
 * the public result scalar while forcing both executors to materialize and
 * inspect the complete optional-pair provider result. */
static XrProgramBuildStatus xr_program_pipe_fixture_write(
    XrProgramArtifact *artifact, char *diagnostic, size_t diagnostic_size) {
    enum {
        XR_PROGRAM_PIPE_PAIR_TYPE = 200,
        XR_PROGRAM_PIPE_OPTIONAL_TYPE = 201,
    };
    XrStableId contract_id = {{0}};
    XrStableId operation_id = {{0}};
    XrFingerprint key_digest;
    if (!xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY, &contract_id, &key_digest) ||
        !xr_stable_id_from_key(XR_PROVIDER_IO_PIPE_OPEN_OPERATION_KEY, &operation_id,
                               &key_digest))
        return XR_PROGRAM_BUILD_INVALID_INPUT;

    uint16_t pair_fields[] = {XR_CORE_TYPE_I64, XR_CORE_TYPE_I64};
    uint16_t some_payload[] = {XR_PROGRAM_PIPE_PAIR_TYPE};
    XrCoreIrVariantInput optional_variants[] = {
        {0},
        {.payload_types = some_payload, .payload_count = 1u},
    };
    XrCoreIrTypeInput types[] = {
        {.key = xr_program_pipe_fixture_key("pipe:type:pair"),
         .local_id = XR_PROGRAM_PIPE_PAIR_TYPE,
         .kind = XR_CORE_IR_TYPE_AGGREGATE,
         .field_types = pair_fields,
         .field_count = 2u},
        {.key = xr_program_pipe_fixture_key("pipe:type:optional"),
         .local_id = XR_PROGRAM_PIPE_OPTIONAL_TYPE,
         .kind = XR_CORE_IR_TYPE_VARIANT,
         .variants = optional_variants,
         .variant_count = 2u},
    };
    XrCoreIrKey optional = xr_program_pipe_fixture_key("pipe:value:optional");
    XrCoreIrKey present = xr_program_pipe_fixture_key("pipe:value:present");
    XrCoreIrKey optional_operand[] = {optional};
    XrCoreIrKey return_operand[] = {present};
    XrCoreIrInstructionInput instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_PROVIDER_CALL,
         .result = optional,
         .result_type_id = XR_PROGRAM_PIPE_OPTIONAL_TYPE,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION,
         .immediate.provider_operation = {.contract_id = contract_id,
                                          .operation_id = operation_id}},
        {.operation_id = XR_CORE_OP_CORE_VARIANT_TEST,
         .result = present,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .operands = optional_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_VARIANT,
         .immediate.variant_ordinal = 1u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = return_operand,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrKey block_key = xr_program_pipe_fixture_key("pipe:block:entry");
    XrCoreIrBlockInput block = {
        .key = block_key,
        .instructions = instructions,
        .instruction_count = sizeof(instructions) / sizeof(instructions[0]),
    };
    XrCoreIrFunctionInput function = {
        .key = xr_program_pipe_fixture_key("pipe:function:entry"),
        .result_type_id = XR_CORE_TYPE_BOOL,
        .effect_mask = XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_PROVIDER_CALL,
        .capability_mask = XR_CORE_CAPABILITY_PROVIDER_BINDING,
        .entry_block = block_key,
        .blocks = &block,
        .block_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    XrCoreIrModuleInput module = {
        .key = xr_program_pipe_fixture_key("pipe:module"),
        .functions = &function,
        .function_count = 1u,
    };
    XrProgramProviderOperationRequirement operation_requirement = {
        .operation_id = operation_id,
        .logical_contract = xr_program_fixture_pipe_contract(),
    };
    XrCoreIrProviderRequirementInput requirement = {
        .contract_id = contract_id,
        .operations = &operation_requirement,
        .operation_count = 1u,
    };
    XrCoreIrKey semantic = xr_program_pipe_fixture_key("pipe:semantic");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = semantic.bytes,
        .required_features = &feature,
        .required_feature_count = 1u,
        .provider_requirements = &requirement,
        .provider_requirement_count = 1u,
        .types = types,
        .type_count = sizeof(types) / sizeof(types[0]),
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

#endif /* XR_PROGRAM_PIPE_FIXTURE_H */
