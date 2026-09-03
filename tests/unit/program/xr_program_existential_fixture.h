#ifndef XR_PROGRAM_EXISTENTIAL_FIXTURE_H
#define XR_PROGRAM_EXISTENTIAL_FIXTURE_H

#include "../../../src/core/xr_core_spec_gen.h"
#include "../../../src/program/xr_program.h"

#include <string.h>

typedef enum XrProgramExistentialFixtureMutation {
    XR_EXISTENTIAL_FIXTURE_VALID = 0,
    XR_EXISTENTIAL_FIXTURE_UNDOMINATED_PROJECT,
    XR_EXISTENTIAL_FIXTURE_FALSE_EDGE_PROJECT,
    XR_EXISTENTIAL_FIXTURE_NO_CONFORMANCE,
    XR_EXISTENTIAL_FIXTURE_REF_RESULT_ESCAPE,
    XR_EXISTENTIAL_FIXTURE_REF_FIELD_ESCAPE,
} XrProgramExistentialFixtureMutation;

enum {
    XR_EXISTENTIAL_FIXTURE_RECT_TYPE = 100,
    XR_EXISTENTIAL_FIXTURE_SHAPE_TYPE = 101,
    XR_EXISTENTIAL_FIXTURE_SHAPE_REF_TYPE = 102,
};

static XrCoreIrKey xr_existential_fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrProgramBuildStatus
xr_program_existential_fixture_write_mutated(XrProgramExistentialFixtureMutation mutation,
                                             XrProgramArtifact *artifact, char *diagnostic,
                                             size_t diagnostic_size) {
    XrCoreIrKey interface_key = xr_existential_fixture_key("wave4:interface:shape");
    XrCoreIrKey implementation_key = xr_existential_fixture_key("wave4:function:rect-area");
    XrCoreIrKey factory_key = xr_existential_fixture_key("wave4:function:make-shape");
    XrCoreIrKey entry_key = xr_existential_fixture_key("wave4:function:entry");
    XrCoreIrKey implementation_block_key =
        xr_existential_fixture_key("wave4:block:rect-area-entry");
    XrCoreIrKey factory_block_key = xr_existential_fixture_key("wave4:block:make-shape-entry");
    XrCoreIrKey entry_block_key = xr_existential_fixture_key("wave4:block:entry");
    XrCoreIrKey true_block_key = xr_existential_fixture_key("wave4:block:true");
    XrCoreIrKey false_block_key = xr_existential_fixture_key("wave4:block:false");

    uint16_t rect_field_types[] = {
        mutation == XR_EXISTENTIAL_FIXTURE_REF_FIELD_ESCAPE ? XR_EXISTENTIAL_FIXTURE_SHAPE_REF_TYPE
                                                            : XR_CORE_TYPE_I64,
    };
    XrCoreIrTypeInput types[] = {
        {
            .key = xr_existential_fixture_key("wave4:type:rect"),
            .local_id = XR_EXISTENTIAL_FIXTURE_RECT_TYPE,
            .kind = XR_CORE_IR_TYPE_AGGREGATE,
            .nominal_kind = XR_CORE_IR_NOMINAL_STRUCT,
            .ownership = XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL,
            .copy_contract = XR_CORE_IR_COPY_TRIVIAL,
            .field_types = rect_field_types,
            .field_count = 1u,
        },
        {
            .key = xr_existential_fixture_key("wave4:type:shape-read"),
            .local_id = XR_EXISTENTIAL_FIXTURE_SHAPE_TYPE,
            .kind = XR_CORE_IR_TYPE_EXISTENTIAL,
            .nominal_kind = XR_CORE_IR_NOMINAL_NONE,
            .ownership = XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL,
            .copy_contract = XR_CORE_IR_COPY_TRIVIAL,
            .existential_interface = interface_key,
            .interface_use_kind = XR_CORE_IR_INTERFACE_EXISTENTIAL_READ,
        },
        {
            .key = xr_existential_fixture_key("wave4:type:shape-ref"),
            .local_id = XR_EXISTENTIAL_FIXTURE_SHAPE_REF_TYPE,
            .kind = XR_CORE_IR_TYPE_EXISTENTIAL,
            .nominal_kind = XR_CORE_IR_NOMINAL_NONE,
            .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
            .copy_contract = XR_CORE_IR_COPY_FORBIDDEN,
            .existential_interface = interface_key,
            .interface_use_kind = XR_CORE_IR_INTERFACE_EXISTENTIAL_REF,
        },
    };

    uint16_t interface_parameters[] = {XR_EXISTENTIAL_FIXTURE_SHAPE_TYPE};
    XrParamMode interface_modes[] = {XR_PARAM_READ};
    XrCoreIrCallableSignatureInput interface_slot = {
        .parameter_types = interface_parameters,
        .parameter_modes = interface_modes,
        .parameter_count = 1u,
        .has_receiver = true,
        .receiver_mode = XR_PARAM_READ,
        .result_type_id = XR_CORE_TYPE_I64,
        .result_ownership = XR_CORE_IR_NON_OWNER,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
    };
    XrCoreIrInterfaceInput interface = {
        .key = interface_key,
        .slots = &interface_slot,
        .slot_count = 1u,
    };

    XrCoreIrKey conformance_slots[] = {implementation_key};
    XrCoreIrConformanceInput conformance = {
        .key = xr_existential_fixture_key("wave4:conformance:rect-shape"),
        .implementor_type_id = XR_EXISTENTIAL_FIXTURE_RECT_TYPE,
        .implementor_kind = XR_CORE_IR_NOMINAL_STRUCT,
        .interface_key = interface_key,
        .slot_functions = conformance_slots,
        .slot_count = 1u,
    };

    XrCoreIrKey impl_receiver = xr_existential_fixture_key("wave4:value:impl-receiver");
    XrCoreIrKey impl_result = xr_existential_fixture_key("wave4:value:impl-result");
    XrCoreIrValueInput impl_arguments[] = {{
        .key = impl_receiver,
        .type_id = XR_EXISTENTIAL_FIXTURE_RECT_TYPE,
        .category = XR_CORE_IR_VALUE,
        .ownership = XR_CORE_IR_NON_OWNER,
    }};
    XrCoreIrKey impl_block_argument_operands[] = {impl_receiver};
    XrCoreIrKey impl_project_operands[] = {impl_receiver};
    XrCoreIrKey impl_return_operands[] = {impl_result};
    XrCoreIrInstructionInput impl_instructions[] = {
        {
            .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = impl_block_argument_operands,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        },
        {
            .operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
            .result = impl_result,
            .result_type_id = XR_CORE_TYPE_I64,
            .result_category = XR_CORE_IR_VALUE,
            .result_ownership = XR_CORE_IR_NON_OWNER,
            .operands = impl_project_operands,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
            .immediate.field_ordinal = 0u,
        },
        {
            .operation_id = XR_CORE_OP_CORE_RETURN,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = impl_return_operands,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        },
    };
    XrCoreIrBlockInput impl_block = {
        .key = implementation_block_key,
        .arguments = impl_arguments,
        .argument_count = 1u,
        .instructions = impl_instructions,
        .instruction_count = 3u,
    };
    uint16_t impl_parameter_types[] = {XR_EXISTENTIAL_FIXTURE_RECT_TYPE};
    XrParamMode impl_parameter_modes[] = {XR_PARAM_READ};
    XrCoreIrFunctionInput implementation = {
        .key = implementation_key,
        .parameter_types = impl_parameter_types,
        .parameter_modes = impl_parameter_modes,
        .parameter_count = 1u,
        .has_receiver = true,
        .receiver_mode = XR_PARAM_READ,
        .result_type_id = XR_CORE_TYPE_I64,
        .result_ownership = XR_CORE_IR_NON_OWNER,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
        .entry_block = implementation_block_key,
        .blocks = &impl_block,
        .block_count = 1u,
    };

    XrCoreIrKey constant_42_key = xr_existential_fixture_key("wave4:constant:42");
    XrCoreIrKey constant_0_key = xr_existential_fixture_key("wave4:constant:0");
    XrCoreIrKey constant_true_key = xr_existential_fixture_key("wave4:constant:true");
    XrCoreIrConstantInput constants[] = {
        {.key = constant_42_key,
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 42},
        {.key = constant_0_key,
         .type_id = XR_CORE_TYPE_I64,
         .kind = XR_CORE_IR_CONSTANT_I64,
         .value.i64 = 0},
        {.key = constant_true_key,
         .type_id = XR_CORE_TYPE_BOOL,
         .kind = XR_CORE_IR_CONSTANT_BOOL,
         .value.boolean = true},
    };

    XrCoreIrKey factory_42 = xr_existential_fixture_key("wave4:value:factory-42");
    XrCoreIrKey factory_rect = xr_existential_fixture_key("wave4:value:factory-rect");
    XrCoreIrKey factory_erased = xr_existential_fixture_key("wave4:value:factory-erased");
    XrCoreIrKey factory_rect_operands[] = {factory_42};
    XrCoreIrKey factory_pack_operands[] = {factory_rect};
    XrCoreIrKey factory_return_operands[] = {factory_erased};
    XrCoreIrInstructionInput factory_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = factory_42,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant_42_key},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
         .result = factory_rect,
         .result_type_id = XR_EXISTENTIAL_FIXTURE_RECT_TYPE,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .operands = factory_rect_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_EXISTENTIAL_PACK,
         .result = factory_erased,
         .result_type_id = XR_EXISTENTIAL_FIXTURE_SHAPE_TYPE,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .operands = factory_pack_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = factory_return_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput factory_block = {
        .key = factory_block_key,
        .instructions = factory_instructions,
        .instruction_count = 5u,
    };
    XrCoreIrFunctionInput factory = {
        .key = factory_key,
        .receiver_mode = XR_PARAM_READ,
        .result_type_id = mutation == XR_EXISTENTIAL_FIXTURE_REF_RESULT_ESCAPE
                              ? XR_EXISTENTIAL_FIXTURE_SHAPE_REF_TYPE
                              : XR_EXISTENTIAL_FIXTURE_SHAPE_TYPE,
        .result_ownership = mutation == XR_EXISTENTIAL_FIXTURE_REF_RESULT_ESCAPE
                                ? XR_CORE_IR_OWNER
                                : XR_CORE_IR_NON_OWNER,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
        .entry_block = factory_block_key,
        .blocks = &factory_block,
        .block_count = 1u,
    };

    XrCoreIrKey value_42 = xr_existential_fixture_key("wave4:value:42");
    XrCoreIrKey value_0 = xr_existential_fixture_key("wave4:value:0");
    XrCoreIrKey value_true = xr_existential_fixture_key("wave4:value:true");
    XrCoreIrKey erased = xr_existential_fixture_key("wave4:value:erased");
    XrCoreIrKey copied_erased = xr_existential_fixture_key("wave4:value:copied-erased");
    XrCoreIrKey test = xr_existential_fixture_key("wave4:value:test");
    XrCoreIrKey true_erased = xr_existential_fixture_key("wave4:value:true-erased");
    XrCoreIrKey true_fallback = xr_existential_fixture_key("wave4:value:true-fallback");
    XrCoreIrKey false_erased = xr_existential_fixture_key("wave4:value:false-erased");
    XrCoreIrKey false_fallback = xr_existential_fixture_key("wave4:value:false-fallback");
    XrCoreIrKey projected = xr_existential_fixture_key("wave4:value:projected");
    XrCoreIrKey projected_field = xr_existential_fixture_key("wave4:value:projected-field");

    XrCoreIrKey test_operands[] = {erased};
    XrCoreIrKey copy_operands[] = {erased};
    XrCoreIrKey branch_operands[] = {
        mutation == XR_EXISTENTIAL_FIXTURE_UNDOMINATED_PROJECT ? value_true : test,
        copied_erased,
        value_42,
        copied_erased,
        value_0,
    };
    XrCoreIrKey branch_successors[] = {true_block_key, false_block_key};
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
         .immediate.key = constant_42_key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
         .result = value_0,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant_0_key},
        {.operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL,
         .result = value_true,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
         .immediate.key = constant_true_key},
        {.operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
         .result = erased,
         .result_type_id = XR_EXISTENTIAL_FIXTURE_SHAPE_TYPE,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
         .immediate.key = factory_key},
        {.operation_id = XR_CORE_OP_CORE_OWNER_COPY,
         .result = copied_erased,
         .result_type_id = XR_EXISTENTIAL_FIXTURE_SHAPE_TYPE,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .operands = copy_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_EXISTENTIAL_TEST,
         .result = test,
         .result_type_id = XR_CORE_TYPE_BOOL,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .operands = test_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_TYPE,
         .immediate.type_id = XR_EXISTENTIAL_FIXTURE_RECT_TYPE},
        {.operation_id = XR_CORE_OP_CORE_CONDITIONAL_BRANCH,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = branch_operands,
         .operand_count = 5u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
         .successors = branch_successors,
         .successor_count = 2u},
    };
    XrCoreIrBlockInput entry_block = {
        .key = entry_block_key,
        .instructions = entry_instructions,
        .instruction_count = 8u,
    };

    XrCoreIrValueInput true_arguments[] = {
        {.key = true_erased,
         .type_id = XR_EXISTENTIAL_FIXTURE_SHAPE_TYPE,
         .category = XR_CORE_IR_VALUE,
         .ownership = XR_CORE_IR_NON_OWNER},
        {.key = true_fallback,
         .type_id = XR_CORE_TYPE_I64,
         .category = XR_CORE_IR_VALUE,
         .ownership = XR_CORE_IR_NON_OWNER},
    };
    XrCoreIrKey true_block_args[] = {true_erased, true_fallback};
    XrCoreIrKey true_project_operands[] = {true_erased};
    XrCoreIrKey true_field_operands[] = {projected};
    XrCoreIrKey true_return_projected[] = {projected_field};
    XrCoreIrKey true_return_fallback[] = {true_fallback};
    XrCoreIrInstructionInput true_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = true_block_args,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_EXISTENTIAL_PROJECT,
         .result = projected,
         .result_type_id = XR_EXISTENTIAL_FIXTURE_RECT_TYPE,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .operands = true_project_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_TYPE,
         .immediate.type_id = XR_EXISTENTIAL_FIXTURE_RECT_TYPE},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
         .result = projected_field,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .operands = true_field_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = true_return_projected,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    if (mutation == XR_EXISTENTIAL_FIXTURE_FALSE_EDGE_PROJECT) {
        true_instructions[1] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_RETURN,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = true_return_fallback,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
    }
    XrCoreIrBlockInput true_block = {
        .key = true_block_key,
        .arguments = true_arguments,
        .argument_count = 2u,
        .instructions = true_instructions,
        .instruction_count = mutation == XR_EXISTENTIAL_FIXTURE_FALSE_EDGE_PROJECT ? 2u : 4u,
    };

    XrCoreIrValueInput false_arguments[] = {
        {.key = false_erased,
         .type_id = XR_EXISTENTIAL_FIXTURE_SHAPE_TYPE,
         .category = XR_CORE_IR_VALUE,
         .ownership = XR_CORE_IR_NON_OWNER},
        {.key = false_fallback,
         .type_id = XR_CORE_TYPE_I64,
         .category = XR_CORE_IR_VALUE,
         .ownership = XR_CORE_IR_NON_OWNER},
    };
    XrCoreIrKey false_block_args[] = {false_erased, false_fallback};
    XrCoreIrKey false_project_operands[] = {false_erased};
    XrCoreIrKey false_field_operands[] = {projected};
    XrCoreIrKey false_return_projected[] = {projected_field};
    XrCoreIrKey false_return_fallback[] = {false_fallback};
    XrCoreIrInstructionInput false_instructions[] = {
        {.operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = false_block_args,
         .operand_count = 2u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = false_return_fallback,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrInstructionInput false_projecting_instructions[] = {
        false_instructions[0],
        {.operation_id = XR_CORE_OP_CORE_EXISTENTIAL_PROJECT,
         .result = projected,
         .result_type_id = XR_EXISTENTIAL_FIXTURE_RECT_TYPE,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .operands = false_project_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_TYPE,
         .immediate.type_id = XR_EXISTENTIAL_FIXTURE_RECT_TYPE},
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT,
         .result = projected_field,
         .result_type_id = XR_CORE_TYPE_I64,
         .result_category = XR_CORE_IR_VALUE,
         .result_ownership = XR_CORE_IR_NON_OWNER,
         .operands = false_field_operands,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
         .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_RETURN,
         .result_type_id = XR_CORE_TYPE_VOID,
         .operands = false_return_projected,
         .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE},
    };
    XrCoreIrBlockInput false_block = {
        .key = false_block_key,
        .arguments = false_arguments,
        .argument_count = 2u,
        .instructions = mutation == XR_EXISTENTIAL_FIXTURE_FALSE_EDGE_PROJECT
                            ? false_projecting_instructions
                            : false_instructions,
        .instruction_count = mutation == XR_EXISTENTIAL_FIXTURE_FALSE_EDGE_PROJECT ? 4u : 2u,
    };

    XrCoreIrBlockInput entry_blocks[] = {entry_block, true_block, false_block};
    XrCoreIrFunctionInput entry = {
        .key = entry_key,
        .receiver_mode = XR_PARAM_READ,
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

    XrCoreIrFunctionInput functions[] = {implementation, factory, entry};
    XrCoreIrModuleInput module = {
        .key = xr_existential_fixture_key("wave4:module"),
        .constants = constants,
        .constant_count = 3u,
        .functions = functions,
        .function_count = 3u,
    };
    XrCoreIrKey profile = xr_existential_fixture_key("wave4:semantic-profile");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile.bytes,
        .required_features = &feature,
        .required_feature_count = 1u,
        .types = types,
        .type_count = 3u,
        .interfaces = &interface,
        .interface_count = 1u,
        .conformances = mutation == XR_EXISTENTIAL_FIXTURE_NO_CONFORMANCE ? NULL : &conformance,
        .conformance_count = mutation == XR_EXISTENTIAL_FIXTURE_NO_CONFORMANCE ? 0u : 1u,
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

static XrProgramBuildStatus xr_program_existential_fixture_write(XrProgramArtifact *artifact,
                                                                 char *diagnostic,
                                                                 size_t diagnostic_size) {
    return xr_program_existential_fixture_write_mutated(XR_EXISTENTIAL_FIXTURE_VALID, artifact,
                                                        diagnostic, diagnostic_size);
}

#endif /* XR_PROGRAM_EXISTENTIAL_FIXTURE_H */
