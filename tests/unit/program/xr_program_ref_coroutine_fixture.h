/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_ref_coroutine_fixture.h - Independent suspended field-ref inputs
 *
 * KEY CONCEPT:
 *   Two child ref parameters project distinct fields of one affine parent
 *   owner. Only that owner crosses the parent suspension and cleanup edges.
 */

#ifndef XR_PROGRAM_REF_COROUTINE_FIXTURE_H
#define XR_PROGRAM_REF_COROUTINE_FIXTURE_H

#include "core/xr_core_spec_gen.h"
#include "program/xr_program.h"

#include <string.h>

typedef enum XrProgramRefCoroutineMutation {
    XR_PROGRAM_REF_COROUTINE_VALID = 0,
    XR_PROGRAM_REF_COROUTINE_MISSING_ROOT,
    XR_PROGRAM_REF_COROUTINE_DUPLICATE_CANCEL_ROOT,
    XR_PROGRAM_REF_COROUTINE_RAW_PLACE_LIVE,
    XR_PROGRAM_REF_COROUTINE_VALUE_PROJECTION_BASE,
    XR_PROGRAM_REF_COROUTINE_SCALAR_PROJECTION_BASE,
    XR_PROGRAM_REF_COROUTINE_INVALID_FIELD,
} XrProgramRefCoroutineMutation;

enum {
    XR_PROGRAM_REF_COROUTINE_AGGREGATE = 62
};

typedef struct XrProgramRefCoroutineChild {
    uint16_t parameter_types[2];
    XrParamMode parameter_modes[2];
    XrCoreIrValueInput entry_arguments[2];
    XrCoreIrValueInput resume_arguments[2];
    XrCoreIrKey entry_keys[2];
    XrCoreIrKey resume_keys[2];
    XrCoreIrKey successors[2];
    XrCoreIrInstructionInput entry_instructions[2];
    XrCoreIrInstructionInput resume_instructions[4];
    XrCoreIrInstructionInput cancel_instruction;
    XrCoreIrBlockInput blocks[3];
    XrCoreIrCoroutineStateInput states[2];
    XrCoreIrCoroutineSafepointInput safepoint;
    XrCoreIrFunctionInput function;
} XrProgramRefCoroutineChild;

typedef struct XrProgramRefCoroutineParent {
    uint16_t parameter_type;
    XrParamMode parameter_mode;
    XrCoreIrValueInput arguments[4];
    XrCoreIrKey owner_operand[1];
    XrCoreIrKey projection_operands[2];
    XrCoreIrKey resume_operand[1];
    XrCoreIrKey cancel_operand[2];
    XrCoreIrKey call_operands[5];
    XrCoreIrKey live_values[1];
    XrCoreIrKey successors[2];
    XrCoreIrInstructionInput entry_instructions[5];
    XrCoreIrInstructionInput resume_instructions[3];
    XrCoreIrInstructionInput cancel_instructions[4];
    XrCoreIrBlockInput blocks[3];
    XrCoreIrCoroutineStateInput states[2];
    XrCoreIrCoroutineSafepointInput safepoint;
    XrCoreIrFunctionInput function;
} XrProgramRefCoroutineParent;

static XrCoreIrKey xr_program_ref_coroutine_key(const char *text) {
    return xr_core_ir_key(text, strlen(text));
}

static void xr_program_ref_coroutine_child_init(XrProgramRefCoroutineChild *child) {
    memset(child, 0, sizeof(*child));
    child->entry_keys[0] = xr_program_ref_coroutine_key("ref-child:entry:left");
    child->entry_keys[1] = xr_program_ref_coroutine_key("ref-child:entry:right");
    child->resume_keys[0] = xr_program_ref_coroutine_key("ref-child:resume:left");
    child->resume_keys[1] = xr_program_ref_coroutine_key("ref-child:resume:right");
    XrCoreIrKey entry = xr_program_ref_coroutine_key("ref-child:block:entry");
    child->successors[0] = xr_program_ref_coroutine_key("ref-child:block:resume");
    child->successors[1] = xr_program_ref_coroutine_key("ref-child:block:cancel");
    for (uint32_t index = 0u; index < 2u; ++index) {
        child->parameter_types[index] = XR_CORE_TYPE_I64;
        child->parameter_modes[index] = XR_PARAM_REF;
        child->entry_arguments[index] = (XrCoreIrValueInput) {
            .key = child->entry_keys[index],
            .type_id = XR_CORE_TYPE_I64,
            .category = XR_CORE_IR_PLACE,
        };
        child->resume_arguments[index] = (XrCoreIrValueInput) {
            .key = child->resume_keys[index],
            .type_id = XR_CORE_TYPE_I64,
            .category = XR_CORE_IR_PLACE,
        };
    }
    child->entry_instructions[0] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = child->entry_keys,
        .operand_count = 2u,
    };
    child->entry_instructions[1] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_COROUTINE_YIELD,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = child->entry_keys,
        .operand_count = 2u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_U32,
        .immediate.u32 = 0u,
        .successors = child->successors,
        .successor_count = 2u,
    };
    child->resume_instructions[0] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = child->resume_keys,
        .operand_count = 2u,
    };
    XrCoreIrKey loaded[] = {xr_program_ref_coroutine_key("ref-child:loaded:left"),
                            xr_program_ref_coroutine_key("ref-child:loaded:right")};
    for (uint32_t index = 0u; index < 2u; ++index)
        child->resume_instructions[1u + index] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_PLACE_LOAD,
            .result = loaded[index],
            .result_type_id = XR_CORE_TYPE_I64,
            .operands = &child->resume_keys[index],
            .operand_count = 1u,
        };
    child->resume_instructions[3] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_RETURN,
        .result_type_id = XR_CORE_TYPE_VOID,
    };
    child->cancel_instruction = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CANCEL_PUBLISH,
        .result_type_id = XR_CORE_TYPE_VOID,
    };
    child->blocks[0] = (XrCoreIrBlockInput) {
        .key = entry,
        .arguments = child->entry_arguments,
        .argument_count = 2u,
        .instructions = child->entry_instructions,
        .instruction_count = 2u,
    };
    child->blocks[1] = (XrCoreIrBlockInput) {
        .key = child->successors[0],
        .arguments = child->resume_arguments,
        .argument_count = 2u,
        .instructions = child->resume_instructions,
        .instruction_count = 4u,
    };
    child->blocks[2] = (XrCoreIrBlockInput) {
        .key = child->successors[1],
        .instructions = &child->cancel_instruction,
        .instruction_count = 1u,
    };
    child->states[0] = (XrCoreIrCoroutineStateInput) {.state_id = 0u, .continuation_block = entry};
    child->states[1] =
        (XrCoreIrCoroutineStateInput) {.state_id = 1u, .continuation_block = child->successors[0]};
    child->safepoint = (XrCoreIrCoroutineSafepointInput) {
        .safepoint_id = 0u,
        .resume_state_id = 1u,
        .live_values = child->entry_keys,
        .live_value_count = 2u,
    };
    child->function = (XrCoreIrFunctionInput) {
        .key = xr_program_ref_coroutine_key("ref-child:function"),
        .parameter_types = child->parameter_types,
        .parameter_modes = child->parameter_modes,
        .parameter_count = 2u,
        .result_type_id = XR_CORE_TYPE_VOID,
        .effect_mask = XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_SUSPEND,
        .capability_mask = XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION,
        .entry_block = entry,
        .blocks = child->blocks,
        .block_count = 3u,
        .coroutine_states = child->states,
        .coroutine_state_count = 2u,
        .coroutine_safepoints = &child->safepoint,
        .coroutine_safepoint_count = 1u,
    };
}

static void xr_program_ref_coroutine_parent_blocks_init(XrProgramRefCoroutineParent *parent) {
    parent->resume_operand[0] = xr_program_ref_coroutine_key("ref-parent:resumed");
    parent->cancel_operand[0] = xr_program_ref_coroutine_key("ref-parent:cancelled");
    XrCoreIrKey argument_keys[] = {parent->owner_operand[0], parent->resume_operand[0],
                                   parent->cancel_operand[0]};
    for (uint32_t index = 0u; index < 3u; ++index)
        parent->arguments[index] = (XrCoreIrValueInput) {
            .key = argument_keys[index],
            .type_id = XR_PROGRAM_REF_COROUTINE_AGGREGATE,
            .ownership = XR_CORE_IR_OWNER,
        };
    parent->resume_instructions[0] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = parent->resume_operand,
        .operand_count = 1u,
    };
    parent->resume_instructions[1] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_OWNER_DROP,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = parent->resume_operand,
        .operand_count = 1u,
    };
    parent->resume_instructions[2] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_RETURN,
        .result_type_id = XR_CORE_TYPE_VOID,
    };
    parent->cancel_instructions[0] = parent->resume_instructions[0];
    parent->cancel_instructions[0].operands = parent->cancel_operand;
    parent->cancel_instructions[1] = parent->resume_instructions[1];
    parent->cancel_instructions[1].operands = parent->cancel_operand;
    parent->cancel_instructions[2] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_CANCEL_PUBLISH,
        .result_type_id = XR_CORE_TYPE_VOID,
    };
    parent->blocks[0] = (XrCoreIrBlockInput) {
        .key = xr_program_ref_coroutine_key("ref-parent:block:entry"),
        .arguments = &parent->arguments[0],
        .argument_count = 1u,
        .instructions = parent->entry_instructions,
        .instruction_count = 5u,
    };
    parent->blocks[1] = (XrCoreIrBlockInput) {
        .key = parent->successors[0],
        .arguments = &parent->arguments[1],
        .argument_count = 1u,
        .instructions = parent->resume_instructions,
        .instruction_count = 3u,
    };
    parent->blocks[2] = (XrCoreIrBlockInput) {
        .key = parent->successors[1],
        .arguments = &parent->arguments[2],
        .argument_count = 1u,
        .instructions = parent->cancel_instructions,
        .instruction_count = 3u,
    };
}

static void xr_program_ref_coroutine_parent_init(XrProgramRefCoroutineParent *parent,
                                                 XrCoreIrKey callee) {
    memset(parent, 0, sizeof(*parent));
    parent->owner_operand[0] = xr_program_ref_coroutine_key("ref-parent:owner");
    parent->projection_operands[0] = xr_program_ref_coroutine_key("ref-parent:aggregate-place");
    parent->projection_operands[1] = parent->projection_operands[0];
    parent->call_operands[0] = xr_program_ref_coroutine_key("ref-parent:left-place");
    parent->call_operands[1] = xr_program_ref_coroutine_key("ref-parent:right-place");
    parent->call_operands[2] = parent->owner_operand[0];
    parent->call_operands[3] = parent->owner_operand[0];
    parent->successors[0] = xr_program_ref_coroutine_key("ref-parent:block:resume");
    parent->successors[1] = xr_program_ref_coroutine_key("ref-parent:block:cancel");
    parent->entry_instructions[0] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_BLOCK_ARGUMENT,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = parent->owner_operand,
        .operand_count = 1u,
    };
    parent->entry_instructions[1] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_PLACE_LOCAL,
        .result = parent->projection_operands[0],
        .result_type_id = XR_PROGRAM_REF_COROUTINE_AGGREGATE,
        .result_category = XR_CORE_IR_PLACE,
        .operands = parent->owner_operand,
        .operand_count = 1u,
    };
    for (uint32_t index = 0u; index < 2u; ++index)
        parent->entry_instructions[2u + index] = (XrCoreIrInstructionInput) {
            .operation_id = XR_CORE_OP_CORE_PLACE_PROJECT,
            .result = parent->call_operands[index],
            .result_type_id = XR_CORE_TYPE_I64,
            .result_category = XR_CORE_IR_PLACE,
            .operands = &parent->projection_operands[index],
            .operand_count = 1u,
            .immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD,
            .immediate.field_ordinal = index,
        };
    parent->entry_instructions[4] = (XrCoreIrInstructionInput) {
        .operation_id = XR_CORE_OP_CORE_COROUTINE_CALL_SEALED,
        .result_type_id = XR_CORE_TYPE_VOID,
        .operands = parent->call_operands,
        .operand_count = 4u,
        .immediate_kind = XR_CORE_IR_IMMEDIATE_COROUTINE_CALL,
        .immediate.coroutine_call = {.callee = callee, .safepoint_id = 0u},
        .successors = parent->successors,
        .successor_count = 2u,
    };
    xr_program_ref_coroutine_parent_blocks_init(parent);
    parent->live_values[0] = parent->owner_operand[0];
    for (uint32_t index = 0u; index < 2u; ++index) {
        parent->states[index] = (XrCoreIrCoroutineStateInput) {
            .state_id = index,
            .continuation_block = parent->blocks[0].key,
        };
    }
    parent->safepoint = (XrCoreIrCoroutineSafepointInput) {
        .safepoint_id = 0u,
        .resume_state_id = 1u,
        .live_values = parent->live_values,
        .live_value_count = 1u,
    };
    parent->parameter_type = XR_PROGRAM_REF_COROUTINE_AGGREGATE;
    parent->parameter_mode = XR_PARAM_MOVE;
    parent->function = (XrCoreIrFunctionInput) {
        .key = xr_program_ref_coroutine_key("ref-parent:function"),
        .parameter_types = &parent->parameter_type,
        .parameter_modes = &parent->parameter_mode,
        .parameter_count = 1u,
        .result_type_id = XR_CORE_TYPE_VOID,
        .effect_mask = XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_SUSPEND,
        .capability_mask = XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION,
        .entry_block = parent->blocks[0].key,
        .blocks = parent->blocks,
        .block_count = 3u,
        .coroutine_states = parent->states,
        .coroutine_state_count = 2u,
        .coroutine_safepoints = &parent->safepoint,
        .coroutine_safepoint_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
}

static void xr_program_ref_coroutine_mutate(XrProgramRefCoroutineParent *parent,
                                            XrProgramRefCoroutineMutation mutation) {
    if (mutation == XR_PROGRAM_REF_COROUTINE_MISSING_ROOT ||
        mutation == XR_PROGRAM_REF_COROUTINE_RAW_PLACE_LIVE) {
        parent->blocks[2].arguments = NULL;
        parent->blocks[2].argument_count = 0u;
        parent->blocks[2].instructions = &parent->cancel_instructions[2];
        parent->blocks[2].instruction_count = 1u;
    }
    switch (mutation) {
        case XR_PROGRAM_REF_COROUTINE_MISSING_ROOT:
            parent->safepoint.live_values = NULL;
            parent->safepoint.live_value_count = 0u;
            parent->entry_instructions[4].operand_count = 2u;
            parent->blocks[1].arguments = NULL;
            parent->blocks[1].argument_count = 0u;
            parent->blocks[1].instructions = &parent->resume_instructions[2];
            parent->blocks[1].instruction_count = 1u;
            break;
        case XR_PROGRAM_REF_COROUTINE_DUPLICATE_CANCEL_ROOT:
            /* Keep the live table canonical so the semantic verifier must
             * reject duplicated owner transfer on the cancellation edge. */
            parent->cancel_operand[1] = xr_program_ref_coroutine_key("ref-parent:cancel-extra");
            parent->arguments[3] = parent->arguments[2];
            parent->arguments[3].key = parent->cancel_operand[1];
            parent->call_operands[4] = parent->owner_operand[0];
            parent->entry_instructions[4].operand_count = 5u;
            parent->blocks[2].argument_count = 2u;
            parent->cancel_instructions[0].operand_count = 2u;
            parent->cancel_instructions[3] = parent->cancel_instructions[2];
            parent->cancel_instructions[2] = parent->cancel_instructions[1];
            parent->cancel_instructions[2].operands = &parent->cancel_operand[1];
            parent->blocks[2].instruction_count = 4u;
            break;
        case XR_PROGRAM_REF_COROUTINE_RAW_PLACE_LIVE:
            parent->live_values[0] = parent->entry_instructions[1].result;
            parent->call_operands[2] = parent->live_values[0];
            parent->entry_instructions[4].operand_count = 3u;
            parent->arguments[1].category = XR_CORE_IR_PLACE;
            parent->arguments[1].ownership = XR_CORE_IR_NON_OWNER;
            parent->resume_instructions[1] = parent->resume_instructions[2];
            parent->blocks[1].instruction_count = 2u;
            break;
        case XR_PROGRAM_REF_COROUTINE_VALUE_PROJECTION_BASE:
            parent->projection_operands[1] = parent->owner_operand[0];
            break;
        case XR_PROGRAM_REF_COROUTINE_SCALAR_PROJECTION_BASE:
            parent->projection_operands[1] = parent->call_operands[0];
            break;
        case XR_PROGRAM_REF_COROUTINE_INVALID_FIELD:
            parent->entry_instructions[3].immediate.field_ordinal = 2u;
            break;
        case XR_PROGRAM_REF_COROUTINE_VALID:
            break;
    }
}

static XrProgramBuildStatus
xr_program_ref_coroutine_fixture_write(XrProgramRefCoroutineMutation mutation,
                                       XrProgramArtifact *artifact, char *diagnostic,
                                       size_t diagnostic_size) {
    XrProgramRefCoroutineChild child;
    xr_program_ref_coroutine_child_init(&child);
    XrProgramRefCoroutineParent parent;
    xr_program_ref_coroutine_parent_init(&parent, child.function.key);
    xr_program_ref_coroutine_mutate(&parent, mutation);
    uint16_t fields[] = {XR_CORE_TYPE_I64, XR_CORE_TYPE_I64};
    XrCoreIrTypeInput type = {
        .key = xr_program_ref_coroutine_key("ref-parent:aggregate-type"),
        .local_id = XR_PROGRAM_REF_COROUTINE_AGGREGATE,
        .kind = XR_CORE_IR_TYPE_AGGREGATE,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = fields,
        .field_count = 2u,
    };
    XrCoreIrFunctionInput functions[] = {parent.function, child.function};
    XrCoreIrModuleInput module = {
        .key = xr_program_ref_coroutine_key("ref-coroutine:module"),
        .functions = functions,
        .function_count = 2u,
    };
    uint8_t profile[XR_PROGRAM_DIGEST_SIZE] = {0};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile,
        .required_features = &feature,
        .required_feature_count = 1u,
        .types = &type,
        .type_count = 1u,
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

#endif  // XR_PROGRAM_REF_COROUTINE_FIXTURE_H
