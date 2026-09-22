#ifndef XR_PROGRAM_REBORROW_FIXTURE_H
#define XR_PROGRAM_REBORROW_FIXTURE_H

#include "../../../src/core/xr_core_spec_gen.h"
#include "../../../src/program/xr_program.h"

#include <string.h>

typedef enum XrProgramReborrowFixtureMutation {
    XR_REBORROW_FIXTURE_VALID = 0,
    XR_REBORROW_FIXTURE_RETURN_OWNER,
    XR_REBORROW_FIXTURE_VALID_LOOP,
    XR_REBORROW_FIXTURE_SOURCE_READ,
    XR_REBORROW_FIXTURE_INTERFACE_MISMATCH,
    XR_REBORROW_FIXTURE_RESULT_OWNER,
    XR_REBORROW_FIXTURE_LOST_OWNER,
    XR_REBORROW_FIXTURE_RETURN_ESCAPE,
    XR_REBORROW_FIXTURE_USE_AFTER_OWNER_DROP,
} XrProgramReborrowFixtureMutation;

enum {
    XR_REBORROW_FIXTURE_BOX_TYPE = 100,
    XR_REBORROW_FIXTURE_OWNED_TYPE = 101,
    XR_REBORROW_FIXTURE_READ_TYPE = 102,
    XR_REBORROW_FIXTURE_OTHER_READ_TYPE = 103,
};

static XrCoreIrKey xr_reborrow_fixture_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static XrProgramBuildStatus
xr_program_reborrow_fixture_write_mutated(XrProgramReborrowFixtureMutation mutation,
                                          XrProgramArtifact *artifact, char *diagnostic,
                                          size_t diagnostic_size) {
    XrCoreIrKey interface_key = xr_reborrow_fixture_key("reborrow:interface:reader");
    XrCoreIrKey other_interface_key = xr_reborrow_fixture_key("reborrow:interface:other");
    uint16_t interface_slot_types[] = {XR_REBORROW_FIXTURE_READ_TYPE};
    uint16_t other_interface_slot_types[] = {XR_REBORROW_FIXTURE_OTHER_READ_TYPE};
    XrParamMode interface_slot_modes[] = {XR_PARAM_READ};
    XrCoreIrCallableSignatureInput interface_slot = {
        .parameter_types = interface_slot_types,
        .parameter_modes = interface_slot_modes,
        .parameter_count = 1u,
        .has_receiver = true,
        .receiver_mode = XR_PARAM_READ,
        .result_type_id = XR_CORE_TYPE_I64,
        .result_ownership = XR_CORE_IR_NON_OWNER,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
    };
    XrCoreIrCallableSignatureInput other_interface_slot = interface_slot;
    other_interface_slot.parameter_types = other_interface_slot_types;
    XrCoreIrInterfaceInput interfaces[] = {
        {.key = interface_key, .slots = &interface_slot, .slot_count = 1u},
        {.key = other_interface_key, .slots = &other_interface_slot, .slot_count = 1u},
    };
    XrCoreIrTypeInput types[] = {
        {
            .key = xr_reborrow_fixture_key("reborrow:type:box"),
            .local_id = XR_REBORROW_FIXTURE_BOX_TYPE,
            .kind = XR_CORE_IR_TYPE_AGGREGATE,
            .nominal_kind = XR_CORE_IR_NOMINAL_STRUCT,
            .ownership = XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL,
            .copy_contract = XR_CORE_IR_COPY_TRIVIAL,
        },
        {
            .key = xr_reborrow_fixture_key("reborrow:type:owned"),
            .local_id = XR_REBORROW_FIXTURE_OWNED_TYPE,
            .kind = XR_CORE_IR_TYPE_EXISTENTIAL,
            .nominal_kind = XR_CORE_IR_NOMINAL_NONE,
            .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
            .copy_contract = XR_CORE_IR_COPY_FORBIDDEN,
            .existential_interface = interface_key,
            .interface_use_kind = XR_CORE_IR_INTERFACE_EXISTENTIAL_OWNED_STORAGE,
        },
        {
            .key = xr_reborrow_fixture_key("reborrow:type:read"),
            .local_id = XR_REBORROW_FIXTURE_READ_TYPE,
            .kind = XR_CORE_IR_TYPE_EXISTENTIAL,
            .nominal_kind = XR_CORE_IR_NOMINAL_NONE,
            .ownership = XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL,
            .copy_contract = XR_CORE_IR_COPY_TRIVIAL,
            .existential_interface = interface_key,
            .interface_use_kind = XR_CORE_IR_INTERFACE_EXISTENTIAL_READ,
        },
        {
            .key = xr_reborrow_fixture_key("reborrow:type:other-read"),
            .local_id = XR_REBORROW_FIXTURE_OTHER_READ_TYPE,
            .kind = XR_CORE_IR_TYPE_EXISTENTIAL,
            .nominal_kind = XR_CORE_IR_NOMINAL_NONE,
            .ownership = XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL,
            .copy_contract = XR_CORE_IR_COPY_TRIVIAL,
            .existential_interface = other_interface_key,
            .interface_use_kind = XR_CORE_IR_INTERFACE_EXISTENTIAL_READ,
        },
    };
    XrCoreIrKey method_key = xr_reborrow_fixture_key("reborrow:function:method");
    XrCoreIrKey conformance_slots[] = {method_key};
    XrCoreIrConformanceInput conformance = {
        .key = xr_reborrow_fixture_key("reborrow:conformance:box-reader"),
        .implementor_type_id = XR_REBORROW_FIXTURE_BOX_TYPE,
        .implementor_kind = XR_CORE_IR_NOMINAL_STRUCT,
        .interface_key = interface_key,
        .slot_functions = conformance_slots,
        .slot_count = 1u,
    };

    XrCoreIrKey method_block_key = xr_reborrow_fixture_key("reborrow:block:method");
    XrCoreIrKey method_receiver = xr_reborrow_fixture_key("reborrow:value:method-receiver");
    XrCoreIrKey method_result = xr_reborrow_fixture_key("reborrow:value:method-result");
    XrCoreIrKey constant_key = xr_reborrow_fixture_key("reborrow:constant:42");
    XrCoreIrValueInput method_arguments[] = {{
        .key = method_receiver,
        .type_id = XR_REBORROW_FIXTURE_BOX_TYPE,
        .category = XR_CORE_IR_VALUE,
        .ownership = XR_CORE_IR_NON_OWNER,
    }};
    XrCoreIrKey method_block_arguments[] = {method_receiver};
    XrCoreIrKey method_return_operands[] = {method_result};
    XrCoreIrInstructionInput method_instructions[] = {
        {
            .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = method_block_arguments,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        },
        {
            .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
            .result = method_result,
            .result_type_id = XR_CORE_TYPE_I64,
            .result_category = XR_CORE_IR_VALUE,
            .result_ownership = XR_CORE_IR_NON_OWNER,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
            .immediate.key = constant_key,
        },
        {
            .operation_id = XR_CORE_OP_CORE_RETURN,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = method_return_operands,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        },
    };
    XrCoreIrBlockInput method_block = {
        .key = method_block_key,
        .arguments = method_arguments,
        .argument_count = 1u,
        .instructions = method_instructions,
        .instruction_count = 3u,
    };
    uint16_t method_parameter_types[] = {XR_REBORROW_FIXTURE_BOX_TYPE};
    XrParamMode method_parameter_modes[] = {XR_PARAM_READ};
    XrCoreIrFunctionInput method = {
        .key = method_key,
        .parameter_types = method_parameter_types,
        .parameter_modes = method_parameter_modes,
        .parameter_count = 1u,
        .has_receiver = true,
        .receiver_mode = XR_PARAM_READ,
        .result_type_id = XR_CORE_TYPE_I64,
        .result_ownership = XR_CORE_IR_NON_OWNER,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
        .entry_block = method_block_key,
        .blocks = &method_block,
        .block_count = 1u,
    };

    XrCoreIrKey helper_key = xr_reborrow_fixture_key("reborrow:function:read");
    XrCoreIrKey helper_block_key = xr_reborrow_fixture_key("reborrow:block:read");
    XrCoreIrKey helper_argument = xr_reborrow_fixture_key("reborrow:value:helper-argument");
    XrCoreIrKey helper_result = xr_reborrow_fixture_key("reborrow:value:helper-result");
    XrCoreIrValueInput helper_arguments[] = {{
        .key = helper_argument,
        .type_id = mutation == XR_REBORROW_FIXTURE_INTERFACE_MISMATCH
                       ? XR_REBORROW_FIXTURE_OTHER_READ_TYPE
                       : XR_REBORROW_FIXTURE_READ_TYPE,
        .category = XR_CORE_IR_VALUE,
        .ownership = XR_CORE_IR_NON_OWNER,
    }};
    XrCoreIrKey helper_block_arguments[] = {helper_argument};
    XrCoreIrKey helper_return_operands[] = {helper_result};
    XrCoreIrInstructionInput helper_instructions[] = {
        {
            .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = helper_block_arguments,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        },
        {
            .operation_id = XR_CORE_OP_CORE_CONSTANT_I64,
            .result = helper_result,
            .result_type_id = XR_CORE_TYPE_I64,
            .result_category = XR_CORE_IR_VALUE,
            .result_ownership = XR_CORE_IR_NON_OWNER,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
            .immediate.key = constant_key,
        },
        {
            .operation_id = XR_CORE_OP_CORE_RETURN,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = helper_return_operands,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        },
    };
    XrCoreIrBlockInput helper_block = {
        .key = helper_block_key,
        .arguments = helper_arguments,
        .argument_count = 1u,
        .instructions = helper_instructions,
        .instruction_count = 3u,
    };
    uint16_t helper_parameter_types[] = {helper_arguments[0].type_id};
    XrParamMode helper_parameter_modes[] = {XR_PARAM_READ};
    XrCoreIrFunctionInput helper = {
        .key = helper_key,
        .parameter_types = helper_parameter_types,
        .parameter_modes = helper_parameter_modes,
        .parameter_count = 1u,
        .receiver_mode = XR_PARAM_READ,
        .result_type_id = XR_CORE_TYPE_I64,
        .result_ownership = XR_CORE_IR_NON_OWNER,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
        .entry_block = helper_block_key,
        .blocks = &helper_block,
        .block_count = 1u,
    };

    XrCoreIrKey entry_key = xr_reborrow_fixture_key("reborrow:function:entry");
    XrCoreIrKey entry_block_key = xr_reborrow_fixture_key("reborrow:block:entry");
    XrCoreIrKey call_block_key = xr_reborrow_fixture_key("reborrow:block:call");
    XrCoreIrKey loop_block_key = xr_reborrow_fixture_key("reborrow:block:loop");
    XrCoreIrKey box = xr_reborrow_fixture_key("reborrow:value:box");
    XrCoreIrKey owned = xr_reborrow_fixture_key("reborrow:value:owned");
    XrCoreIrKey borrowed = xr_reborrow_fixture_key("reborrow:value:borrowed");
    XrCoreIrKey forwarded_owner = xr_reborrow_fixture_key("reborrow:value:forwarded-owner");
    XrCoreIrKey forwarded_borrow = xr_reborrow_fixture_key("reborrow:value:forwarded-borrow");
    XrCoreIrKey call_result = xr_reborrow_fixture_key("reborrow:value:call-result");
    uint16_t packed_type = mutation == XR_REBORROW_FIXTURE_SOURCE_READ
                               ? XR_REBORROW_FIXTURE_READ_TYPE
                               : XR_REBORROW_FIXTURE_OWNED_TYPE;
    uint16_t borrowed_type = mutation == XR_REBORROW_FIXTURE_INTERFACE_MISMATCH
                                 ? XR_REBORROW_FIXTURE_OTHER_READ_TYPE
                                 : XR_REBORROW_FIXTURE_READ_TYPE;
    XrCoreIrKey pack_operands[] = {box};
    XrCoreIrKey reborrow_operands[] = {owned};
    XrCoreIrKey owner_drop_operands[] = {
        mutation == XR_REBORROW_FIXTURE_SOURCE_READ ? box : owned,
    };
    XrCoreIrKey branch_operands[] = {owned, borrowed};
    XrCoreIrKey lost_owner_branch_operands[] = {borrowed};
    XrCoreIrKey branch_successors[] = {
        mutation == XR_REBORROW_FIXTURE_VALID_LOOP ? loop_block_key : call_block_key,
    };
    XrCoreIrKey escape_return_operands[] = {borrowed};
    XrCoreIrInstructionInput entry_instructions[7] = {0};
    uint32_t entry_instruction_count = 0u;
    entry_instructions[entry_instruction_count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
        .result_type_id = XR_CORE_TYPE_VOID,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    entry_instructions[entry_instruction_count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT,
        .result = box,
        .result_type_id = XR_REBORROW_FIXTURE_BOX_TYPE,
        .result_category = XR_CORE_IR_VALUE,
        .result_ownership = XR_CORE_IR_NON_OWNER,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    entry_instructions[entry_instruction_count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_EXISTENTIAL_PACK,
        .result = owned,
        .result_type_id = packed_type,
        .result_category = XR_CORE_IR_VALUE,
        .result_ownership =
            mutation == XR_REBORROW_FIXTURE_SOURCE_READ ? XR_CORE_IR_NON_OWNER : XR_CORE_IR_OWNER,
        .operands = pack_operands,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    entry_instructions[entry_instruction_count++] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_EXISTENTIAL_REBORROW_READ,
        .result = borrowed,
        .result_type_id = borrowed_type,
        .result_category = XR_CORE_IR_VALUE,
        .result_ownership =
            mutation == XR_REBORROW_FIXTURE_RESULT_OWNER ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER,
        .operands = reborrow_operands,
        .operand_count = 1u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
    };
    if (mutation == XR_REBORROW_FIXTURE_LOST_OWNER ||
        mutation == XR_REBORROW_FIXTURE_USE_AFTER_OWNER_DROP ||
        mutation == XR_REBORROW_FIXTURE_SOURCE_READ)
        entry_instructions[entry_instruction_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = owner_drop_operands,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
    if (mutation == XR_REBORROW_FIXTURE_RETURN_ESCAPE) {
        entry_instructions[entry_instruction_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_RETURN,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = escape_return_operands,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        };
    } else {
        entry_instructions[entry_instruction_count++] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_BRANCH,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = mutation == XR_REBORROW_FIXTURE_LOST_OWNER ? lost_owner_branch_operands
                                                                   : branch_operands,
            .operand_count = mutation == XR_REBORROW_FIXTURE_LOST_OWNER ? 1u : 2u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
            .successors = branch_successors,
            .successor_count = 1u,
        };
    }
    XrCoreIrBlockInput entry_block = {
        .key = entry_block_key,
        .instructions = entry_instructions,
        .instruction_count = entry_instruction_count,
    };

    XrCoreIrValueInput call_arguments[] = {
        {
            .key = forwarded_owner,
            .type_id = XR_REBORROW_FIXTURE_OWNED_TYPE,
            .category = XR_CORE_IR_VALUE,
            .ownership = XR_CORE_IR_OWNER,
        },
        {
            .key = forwarded_borrow,
            .type_id = borrowed_type,
            .category = XR_CORE_IR_VALUE,
            .ownership = XR_CORE_IR_NON_OWNER,
        },
    };
    XrCoreIrKey call_block_arguments[] = {forwarded_owner, forwarded_borrow};
    XrCoreIrKey call_operands[] = {forwarded_borrow};
    XrCoreIrKey forwarded_owner_drop_operands[] = {forwarded_owner};
    XrCoreIrKey call_return_operands[] = {call_result};
    XrCoreIrInstructionInput call_instructions[] = {
        {
            .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = call_block_arguments,
            .operand_count = 2u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        },
        {
            .operation_id = XR_CORE_OP_CORE_CALL_SEALED_DIRECT,
            .result = call_result,
            .result_type_id = XR_CORE_TYPE_I64,
            .result_category = XR_CORE_IR_VALUE,
            .result_ownership = XR_CORE_IR_NON_OWNER,
            .operands = call_operands,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION,
            .immediate.key = helper_key,
        },
        {
            .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = forwarded_owner_drop_operands,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        },
        {
            .operation_id = XR_CORE_OP_CORE_RETURN,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = call_return_operands,
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        },
    };
    XrCoreIrBlockInput call_block = {
        .key = call_block_key,
        .arguments =
            mutation == XR_REBORROW_FIXTURE_LOST_OWNER ? &call_arguments[1] : call_arguments,
        .argument_count = mutation == XR_REBORROW_FIXTURE_LOST_OWNER ? 1u : 2u,
        .instructions = call_instructions,
        .instruction_count = 4u,
    };
    if (mutation == XR_REBORROW_FIXTURE_LOST_OWNER) {
        call_instructions[0].operands = &call_block_arguments[1];
        call_instructions[0].operand_count = 1u;
        call_instructions[2] = call_instructions[3];
        call_block.instruction_count = 3u;
    }

    XrCoreIrKey loop_owner = xr_reborrow_fixture_key("reborrow:value:loop-owner");
    XrCoreIrKey loop_borrow = xr_reborrow_fixture_key("reborrow:value:loop-borrow");
    XrCoreIrKey loop_condition = xr_reborrow_fixture_key("reborrow:value:loop-condition");
    XrCoreIrKey false_constant_key = xr_reborrow_fixture_key("reborrow:constant:false");
    XrCoreIrValueInput loop_arguments[] = {
        {
            .key = loop_owner,
            .type_id = XR_REBORROW_FIXTURE_OWNED_TYPE,
            .category = XR_CORE_IR_VALUE,
            .ownership = XR_CORE_IR_OWNER,
        },
        {
            .key = loop_borrow,
            .type_id = XR_REBORROW_FIXTURE_READ_TYPE,
            .category = XR_CORE_IR_VALUE,
            .ownership = XR_CORE_IR_NON_OWNER,
        },
    };
    XrCoreIrKey loop_block_arguments[] = {loop_owner, loop_borrow};
    XrCoreIrKey loop_branch_operands[] = {
        loop_condition, loop_owner, loop_borrow, loop_owner, loop_borrow,
    };
    XrCoreIrKey loop_successors[] = {loop_block_key, call_block_key};
    XrCoreIrInstructionInput loop_instructions[] = {
        {
            .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = loop_block_arguments,
            .operand_count = 2u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
        },
        {
            .operation_id = XR_CORE_OP_CORE_CONSTANT_BOOL,
            .result = loop_condition,
            .result_type_id = XR_CORE_TYPE_BOOL,
            .result_category = XR_CORE_IR_VALUE,
            .result_ownership = XR_CORE_IR_NON_OWNER,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT,
            .immediate.key = false_constant_key,
        },
        {
            .operation_id = XR_CORE_OP_CORE_CONDITIONAL_BRANCH,
            .result_type_id = XR_CORE_TYPE_VOID,
            .operands = loop_branch_operands,
            .operand_count = 5u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_NONE,
            .successors = loop_successors,
            .successor_count = 2u,
        },
    };
    XrCoreIrBlockInput loop_block = {
        .key = loop_block_key,
        .arguments = loop_arguments,
        .argument_count = 2u,
        .instructions = loop_instructions,
        .instruction_count = 3u,
    };

    XrCoreIrBlockInput entry_blocks[] = {entry_block, call_block, loop_block};
    XrCoreIrFunctionInput entry = {
        .key = entry_key,
        .receiver_mode = XR_PARAM_READ,
        .result_type_id =
            mutation == XR_REBORROW_FIXTURE_RETURN_ESCAPE ? borrowed_type : XR_CORE_TYPE_I64,
        .result_ownership = XR_CORE_IR_NON_OWNER,
        .error_type_id = XR_CORE_TYPE_VOID,
        .panic_type_id = XR_CORE_TYPE_VOID,
        .effect_mask = mutation == XR_REBORROW_FIXTURE_RETURN_ESCAPE ? 0u : XR_CORE_EFFECT_CALL,
        .entry_block = entry_block_key,
        .blocks = entry_blocks,
        .block_count = mutation == XR_REBORROW_FIXTURE_RETURN_ESCAPE ? 1u
                       : mutation == XR_REBORROW_FIXTURE_VALID_LOOP  ? 3u
                                                                     : 2u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };

    uint16_t payload_field = XR_CORE_TYPE_STRING;
    XrCoreIrKey payload_key = xr_reborrow_fixture_key("reborrow:value:payload");
    XrCoreIrKey payload_read = xr_reborrow_fixture_key("reborrow:value:payload-read");
    XrCoreIrInstructionInput payload_method[] = {
        method_instructions[0],
        {.operation_id = XR_CORE_OP_CORE_AGGREGATE_PROJECT, .result = payload_read,
         .result_type_id = XR_CORE_TYPE_STRING, .operands = &method_receiver, .operand_count = 1u,
         .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD, .immediate.field_ordinal = 0u},
        {.operation_id = XR_CORE_OP_CORE_SEQUENCE_LENGTH, .result = method_result,
         .result_type_id = XR_CORE_TYPE_I64, .operands = &payload_read, .operand_count = 1u},
        method_instructions[2],
    };
    if (mutation == XR_REBORROW_FIXTURE_RETURN_OWNER) {
        types[0].ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE;
        types[0].copy_contract = XR_CORE_IR_COPY_EXPLICIT;
        types[0].field_types = &payload_field;
        types[0].field_count = 1u;
        method_block.instructions = payload_method;
        method_block.instruction_count = XR_COUNTOF(payload_method);
        entry_instructions[3] = entry_instructions[2];
        entry_instructions[3].result_ownership = XR_CORE_IR_OWNER;
        entry_instructions[2] = entry_instructions[1];
        entry_instructions[2].result_ownership = XR_CORE_IR_OWNER;
        entry_instructions[2].operands = &payload_key;
        entry_instructions[2].operand_count = 1u;
        entry_instructions[1] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CONSTANT_STRING, .result = payload_key,
            .result_type_id = XR_CORE_TYPE_STRING, .result_ownership = XR_CORE_IR_OWNER,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT, .immediate.key = payload_key};
        entry_instructions[4] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_RETURN, .operands = &owned, .operand_count = 1u};
        entry_blocks[0].instruction_count = 5u;
        entry.block_count = 1u;
        entry.result_type_id = XR_REBORROW_FIXTURE_OWNED_TYPE;
        entry.result_ownership = XR_CORE_IR_OWNER;
        entry.effect_mask = 0u;
        helper_arguments[0].type_id = XR_REBORROW_FIXTURE_OWNED_TYPE;
        helper_parameter_types[0] = XR_REBORROW_FIXTURE_OWNED_TYPE;
        helper_instructions[1] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_CALL_WITNESS_DIRECT, .result = helper_result,
            .result_type_id = XR_CORE_TYPE_I64, .operands = &helper_argument, .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_U32, .immediate.u32 = 0u};
        helper.effect_mask = XR_CORE_EFFECT_CALL;
    }
    XrCoreIrConstantInput constant = {
        .key = constant_key,
        .type_id = XR_CORE_TYPE_I64,
        .kind = XR_CORE_IR_CONSTANT_I64,
        .value.i64 = 42,
    };
    static const uint8_t payload_text[] = "012345678901234567890123456789012345678901";
    XrCoreIrConstantInput constants[] = {
        constant,
        {
            .key = false_constant_key,
            .type_id = XR_CORE_TYPE_BOOL,
            .kind = XR_CORE_IR_CONSTANT_BOOL,
            .value.boolean = false,
        },
        {.key = payload_key, .type_id = XR_CORE_TYPE_STRING, .kind = XR_CORE_IR_CONSTANT_STRING,
         .value.string = {.bytes = payload_text, .size = sizeof(payload_text) - 1u}},
    };
    XrCoreIrFunctionInput functions[] = {method, helper, entry};
    XrCoreIrModuleInput module = {
        .key = xr_reborrow_fixture_key("reborrow:module"),
        .constants = constants,
        .constant_count = mutation == XR_REBORROW_FIXTURE_RETURN_OWNER ? 3u : 2u,
        .functions = functions,
        .function_count = 3u,
    };
    XrCoreIrKey profile = xr_reborrow_fixture_key("reborrow:semantic-profile");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile.bytes,
        .required_features = &feature,
        .required_feature_count = 1u,
        .types = types,
        .type_count = 4u,
        .interfaces = interfaces,
        .interface_count = 2u,
        .conformances = &conformance,
        .conformance_count = 1u,
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

static XrProgramBuildStatus xr_program_reborrow_fixture_write(XrProgramArtifact *artifact,
                                                              char *diagnostic,
                                                              size_t diagnostic_size) {
    return xr_program_reborrow_fixture_write_mutated(XR_REBORROW_FIXTURE_VALID, artifact,
                                                     diagnostic, diagnostic_size);
}

#endif /* XR_PROGRAM_REBORROW_FIXTURE_H */
