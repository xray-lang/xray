#ifndef XR_PROGRAM_CALLABLE_FIXTURE_H
#define XR_PROGRAM_CALLABLE_FIXTURE_H

#include "../../../src/core/xr_core_spec_gen.h"
#include "../../../src/program/xr_program.h"

#include <string.h>

typedef enum XrProgramCallableFixtureMutation {
    XR_CALLABLE_FIXTURE_VALID = 0,
    XR_CALLABLE_FIXTURE_PACK_SIGNATURE_MISMATCH,
    XR_CALLABLE_FIXTURE_DIRECT_FALLIBLE,
    XR_CALLABLE_FIXTURE_INVOKE_INFALLIBLE,
} XrProgramCallableFixtureMutation;

enum {
    XR_CALLABLE_FIXTURE_CAPTURE_TYPE = 100,
    XR_CALLABLE_FIXTURE_DIRECT_TYPE = 101,
    XR_CALLABLE_FIXTURE_FALLIBLE_TYPE = 102,
};

static XrCoreIrKey xr_callable_fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrProgramBuildStatus
xr_program_callable_fixture_write_mutated(XrProgramCallableFixtureMutation mutation,
                                          XrProgramArtifact *artifact, char *diagnostic,
                                          size_t diagnostic_size) {
    XrCoreIrKey captured_target_key = xr_callable_fixture_key("callable:target:captured");
    XrCoreIrKey captureless_target_key = xr_callable_fixture_key("callable:target:captureless");
    XrCoreIrKey fallible_target_key = xr_callable_fixture_key("callable:target:fallible");
    XrCoreIrKey captured_block_key = xr_callable_fixture_key("callable:block:captured");
    XrCoreIrKey captureless_block_key = xr_callable_fixture_key("callable:block:captureless");
    XrCoreIrKey fallible_block_key = xr_callable_fixture_key("callable:block:fallible");
    XrCoreIrKey entry_key = xr_callable_fixture_key("callable:entry");
    XrCoreIrKey entry_block_key = xr_callable_fixture_key("callable:entry:block");
    XrCoreIrKey normal_block_key = xr_callable_fixture_key("callable:entry:normal");
    XrCoreIrKey error_block_key = xr_callable_fixture_key("callable:entry:error");

    uint16_t capture_fields[] = {XR_CORE_TYPE_I64};
    XrCoreIrCallableSignatureInput direct_signature = {
        .result_type_id = XR_CORE_TYPE_I64,
        .result_ownership = XR_CORE_IR_NON_OWNER,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
    };
    XrCoreIrCallableSignatureInput fallible_signature = direct_signature;
    fallible_signature.error_type_id = XR_CORE_TYPE_ERROR;
    fallible_signature.effect_mask = XR_CORE_EFFECT_ERROR;
    XrCoreIrTypeInput types[] = {
        {
            .key = xr_callable_fixture_key("callable:type:capture"),
            .local_id = XR_CALLABLE_FIXTURE_CAPTURE_TYPE,
            .kind = XR_CORE_IR_TYPE_AGGREGATE,
            .nominal_kind = XR_CORE_IR_NOMINAL_STRUCT,
            .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
            .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
            .field_types = capture_fields,
            .field_count = 1u,
        },
        {
            .key = xr_callable_fixture_key("callable:type:direct"),
            .local_id = XR_CALLABLE_FIXTURE_DIRECT_TYPE,
            .kind = XR_CORE_IR_TYPE_CALLABLE,
            .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
            .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
            .callable_signature = &direct_signature,
        },
        {
            .key = xr_callable_fixture_key("callable:type:fallible"),
            .local_id = XR_CALLABLE_FIXTURE_FALLIBLE_TYPE,
            .kind = XR_CORE_IR_TYPE_CALLABLE,
            .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
            .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
            .callable_signature = &fallible_signature,
        },
    };

    XrCoreIrKey captured_receiver = xr_callable_fixture_key("callable:value:captured-receiver");
    XrCoreIrKey captured_result = xr_callable_fixture_key("callable:value:captured-result");
    XrCoreIrValueInput captured_argument = {
        .key = captured_receiver,
        .type_id = XR_CALLABLE_FIXTURE_CAPTURE_TYPE,
        .category = XR_CORE_IR_VALUE,
        .ownership = XR_CORE_IR_NON_OWNER,
    };
    XrCoreIrKey captured_block_operands[] = {captured_receiver};
    XrCoreIrKey captured_project_operands[] = {captured_receiver};
    XrCoreIrKey captured_return_operands[] = {captured_result};
    XrCoreIrInstructionInput captured_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = captured_block_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
         .result = captured_result,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .operands = captured_project_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = captured_return_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput captured_block = {
        .key = captured_block_key,
        .arguments = &captured_argument,
        .argument_count = 1u,
        .instructions = captured_instructions,
        .instruction_count = 3u,
    };
    uint16_t captured_parameters[] = {XR_CALLABLE_FIXTURE_CAPTURE_TYPE};
    XrParamMode captured_modes[] = {XR_PARAM_READ};
    XrCoreIrFunctionInput captured_target = {
        .key = captured_target_key,
        .parameter_types = captured_parameters,
        .parameter_modes = captured_modes,
        .parameter_count = 1u,
        .has_receiver = true,
        .receiver_mode = XR_PARAM_READ,
        .result_type_id = XR_CORE_TYPE_I64,
        .result_ownership = XR_CORE_IR_NON_OWNER,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
        .entry_block = captured_block_key,
        .blocks = &captured_block,
        .block_count = 1u,
    };

    XrCoreIrKey constant_key = xr_callable_fixture_key("callable:constant:42");
    XrCoreIrConstantInput constant = {
        .key = constant_key,
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };
    XrCoreIrKey captureless_result = xr_callable_fixture_key("callable:value:captureless-result");
    XrCoreIrKey captureless_return[] = {captureless_result};
    XrCoreIrInstructionInput captureless_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = captureless_result,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant_key},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = captureless_return,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput captureless_block = {
        .key = captureless_block_key,
        .instructions = captureless_instructions,
        .instruction_count = 3u,
    };
    XrCoreIrFunctionInput captureless_target = {
        .key = captureless_target_key,
        .result_type_id = XR_CORE_TYPE_I64,
        .result_ownership = XR_CORE_IR_NON_OWNER,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
        .entry_block = captureless_block_key,
        .blocks = &captureless_block,
        .block_count = 1u,
    };

    XrCoreIrKey fallible_receiver = xr_callable_fixture_key("callable:value:fallible-receiver");
    XrCoreIrKey fallible_result = xr_callable_fixture_key("callable:value:fallible-result");
    XrCoreIrValueInput fallible_argument = {
        .key = fallible_receiver,
        .type_id = XR_CALLABLE_FIXTURE_CAPTURE_TYPE,
        .category = XR_CORE_IR_VALUE,
        .ownership = XR_CORE_IR_NON_OWNER,
    };
    XrCoreIrKey fallible_block_operands[] = {fallible_receiver};
    XrCoreIrKey fallible_project_operands[] = {fallible_receiver};
    XrCoreIrKey fallible_return_operands[] = {fallible_result};
    XrCoreIrInstructionInput fallible_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = fallible_block_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
         .result = fallible_result,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .operands = fallible_project_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = fallible_return_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput fallible_block = {
        .key = fallible_block_key,
        .arguments = &fallible_argument,
        .argument_count = 1u,
        .instructions = fallible_instructions,
        .instruction_count = 3u,
    };
    XrCoreIrFunctionInput fallible_target = captured_target;
    fallible_target.key = fallible_target_key;
    fallible_target.error_type_id = XR_CORE_TYPE_ERROR;
    fallible_target.effect_mask = XR_CORE_EFFECT_ERROR;
    fallible_target.entry_block = fallible_block_key;
    fallible_target.blocks = &fallible_block;

    XrCoreIrKey value_42 = xr_callable_fixture_key("callable:value:42");
    XrCoreIrKey capture = xr_callable_fixture_key("callable:value:capture");
    XrCoreIrKey direct_callable = xr_callable_fixture_key("callable:value:direct");
    XrCoreIrKey direct_copy = xr_callable_fixture_key("callable:value:direct-copy");
    XrCoreIrKey direct_result = xr_callable_fixture_key("callable:value:direct-result");
    XrCoreIrKey captureless_callable = xr_callable_fixture_key("callable:value:captureless");
    XrCoreIrKey captureless_call_result =
        xr_callable_fixture_key("callable:value:captureless-call-result");
    XrCoreIrKey fallible_capture = xr_callable_fixture_key("callable:value:fallible-capture");
    XrCoreIrKey fallible_callable = xr_callable_fixture_key("callable:value:fallible");
    XrCoreIrKey construct_capture[] = {value_42};
    XrCoreIrKey pack_capture[] = {capture};
    XrCoreIrKey copy_direct[] = {direct_callable};
    XrCoreIrKey direct_call[] = {mutation == XR_CALLABLE_FIXTURE_DIRECT_FALLIBLE ? fallible_callable
                                                                                 : direct_copy};
    XrCoreIrKey drop_direct_copy[] = {direct_copy};
    XrCoreIrKey drop_direct[] = {direct_callable};
    XrCoreIrKey captureless_call[] = {captureless_callable};
    XrCoreIrKey drop_captureless[] = {captureless_callable};
    XrCoreIrKey construct_fallible_capture[] = {value_42};
    XrCoreIrKey pack_fallible_capture[] = {fallible_capture};
    XrCoreIrKey invoke_operands[] = {
        mutation == XR_CALLABLE_FIXTURE_INVOKE_INFALLIBLE ? direct_callable : fallible_callable,
        fallible_callable,
        fallible_callable,
        direct_result,
    };
    XrCoreIrKey invoke_successors[] = {normal_block_key, error_block_key};
    XrCoreIrInstructionInput entry_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = value_42,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant_key},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
         .result = capture,
         .result_type_id = XR_CALLABLE_FIXTURE_CAPTURE_TYPE,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = construct_capture,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CALLABLE_PACK,
         .result = direct_callable,
         .result_type_id = XR_CALLABLE_FIXTURE_DIRECT_TYPE,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = pack_capture,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = mutation == XR_CALLABLE_FIXTURE_PACK_SIGNATURE_MISMATCH
                              ? fallible_target_key
                              : captured_target_key},
        {.operation_id = XR_CORE_OP_CORE_OWNER_COPY,
         .result = direct_copy,
         .result_type_id = XR_CALLABLE_FIXTURE_DIRECT_TYPE,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = copy_direct,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CALLABLE_PACK,
         .result = captureless_callable,
         .result_type_id = XR_CALLABLE_FIXTURE_DIRECT_TYPE,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = captureless_target_key},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
         .result = fallible_capture,
         .result_type_id = XR_CALLABLE_FIXTURE_CAPTURE_TYPE,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = construct_fallible_capture,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CALLABLE_PACK,
         .result = fallible_callable,
         .result_type_id = XR_CALLABLE_FIXTURE_FALLIBLE_TYPE,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_OWNER,
         .operands = pack_fallible_capture,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = fallible_target_key},
        {.operation_id = XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT,
         .result = direct_result,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .operands = direct_call,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = drop_direct_copy,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CALL_INDIRECT_DIRECT,
         .result = captureless_call_result,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .operands = captureless_call,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = drop_captureless,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = drop_direct,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CALL_INDIRECT_INVOKE,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = invoke_operands,
         .operand_count = 4u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = invoke_successors,
         .successor_count = 2u},
    };
    XrCoreIrBlockInput entry_block = {
        .key = entry_block_key,
        .instructions = entry_instructions,
        .instruction_count = 14u,
    };

    XrCoreIrKey normal_result = xr_callable_fixture_key("callable:value:normal-result");
    XrCoreIrKey normal_callable = xr_callable_fixture_key("callable:value:normal-callable");
    XrCoreIrValueInput normal_arguments[] = {
        {.key = normal_result, .type_id = XR_CORE_TYPE_I64},
        {.key = normal_callable,
         .type_id = XR_CALLABLE_FIXTURE_FALLIBLE_TYPE,
         .ownership = XR_CORE_IR_OWNER},
    };
    XrCoreIrKey normal_block_operands[] = {normal_result, normal_callable};
    XrCoreIrKey normal_drop[] = {normal_callable};
    XrCoreIrKey normal_return[] = {normal_result};
    XrCoreIrInstructionInput normal_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = normal_block_operands,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = normal_drop,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = normal_return,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput normal_block = {
        .key = normal_block_key,
        .arguments = normal_arguments,
        .argument_count = 2u,
        .instructions = normal_instructions,
        .instruction_count = 3u,
    };

    XrCoreIrKey error_value = xr_callable_fixture_key("callable:value:error");
    XrCoreIrKey error_callable = xr_callable_fixture_key("callable:value:error-callable");
    XrCoreIrKey error_fallback = xr_callable_fixture_key("callable:value:error-fallback");
    XrCoreIrValueInput error_arguments[] = {
        {.key = error_value, .type_id = XR_CORE_TYPE_ERROR},
        {.key = error_callable,
         .type_id = XR_CALLABLE_FIXTURE_FALLIBLE_TYPE,
         .ownership = XR_CORE_IR_OWNER},
        {.key = error_fallback, .type_id = XR_CORE_TYPE_I64},
    };
    XrCoreIrKey error_block_operands[] = {error_value, error_callable, error_fallback};
    XrCoreIrKey error_drop[] = {error_callable};
    XrCoreIrKey error_return[] = {error_fallback};
    XrCoreIrInstructionInput error_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = error_block_operands,
         .operand_count = 3u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_OWNER_DROP,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = error_drop,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = error_return,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput error_block = {
        .key = error_block_key,
        .arguments = error_arguments,
        .argument_count = 3u,
        .instructions = error_instructions,
        .instruction_count = 3u,
    };
    XrCoreIrBlockInput entry_blocks[] = {entry_block, normal_block, error_block};
    XrCoreIrFunctionInput entry = {
        .key = entry_key,
        .result_type_id = XR_CORE_TYPE_I64,
        .result_ownership = XR_CORE_IR_NON_OWNER,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
        .effect_mask = XR_CORE_EFFECT_CALL,
        .entry_block = entry_block_key,
        .blocks = entry_blocks,
        .block_count = 3u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };

    XrCoreIrFunctionInput functions[] = {
        captured_target,
        captureless_target,
        fallible_target,
        entry,
    };
    XrCoreIrModuleInput module = {
        .key = xr_callable_fixture_key("callable:module"),
        .constants = &constant,
        .constant_count = 1u,
        .functions = functions,
        .function_count = 4u,
    };
    uint8_t profile[XR_PROGRAM_DIGEST_SIZE] = {0};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile,
        .required_features = &feature,
        .required_feature_count = 1u,
        .types = types,
        .type_count = 3u,
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

static XrProgramBuildStatus xr_program_callable_fixture_write(XrProgramArtifact *artifact,
                                                              char *diagnostic,
                                                              size_t diagnostic_size) {
    return xr_program_callable_fixture_write_mutated(XR_CALLABLE_FIXTURE_VALID, artifact,
                                                     diagnostic, diagnostic_size);
}

#endif /* XR_PROGRAM_CALLABLE_FIXTURE_H */
