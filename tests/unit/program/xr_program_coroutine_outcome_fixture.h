/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_coroutine_outcome_fixture.h - Independent suspended failure inputs
 */

#ifndef XR_PROGRAM_COROUTINE_OUTCOME_FIXTURE_H
#define XR_PROGRAM_COROUTINE_OUTCOME_FIXTURE_H

#include "xr_program_coroutine_trap_fixture.h"

typedef enum XrProgramCoroutineOutcomeMutation {
    XR_CORO_OUTCOME_VALID,
    XR_CORO_OUTCOME_MISSING_CHANNEL,
    XR_CORO_OUTCOME_REVERSED_CHANNELS,
    XR_CORO_OUTCOME_ERROR_CANCEL_ALIAS,
    XR_CORO_OUTCOME_PANIC_CANCEL_ALIAS,
    XR_CORO_OUTCOME_ERROR_WRONG_TYPE,
    XR_CORO_OUTCOME_PANIC_WRONG_OWNERSHIP,
    XR_CORO_OUTCOME_ERROR_NONLIVE,
    XR_CORO_OUTCOME_PANIC_NONLIVE,
    XR_CORO_OUTCOME_ERROR_MISSING_DROP,
    XR_CORO_OUTCOME_PANIC_MISSING_DROP,
    XR_CORO_OUTCOME_HANDLED,
    XR_CORO_OUTCOME_HANDLED_MISSING_EFFECT,
} XrProgramCoroutineOutcomeMutation;

static void xr_program_coroutine_outcome_child(XrProgramCoroutineTrapBody *child, bool panics,
                                               const XrCoreIrConstantInput *constants) {
    XrCoreIrKey input_value = xr_program_coroutine_trap_argument(child, 0u, "input",
        XR_CORE_TYPE_I64, false, false);
    xr_program_coroutine_trap_arguments(child, 0u);
    child->live[0] = input_value;
    child->safepoint.live_value_count = 1u;
    XrCoreIrInstructionInput *yield = xr_program_coroutine_trap_emit(child, 0u,
        XR_CORE_OP_CORE_COROUTINE_YIELD, NULL, XR_CORE_TYPE_VOID, &input_value, 1u);
    yield->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
    yield->successors = child->successors;
    yield->successor_count = 2u;
    XrCoreIrKey resumed = xr_program_coroutine_trap_argument(child, 1u, "resumed",
        XR_CORE_TYPE_I64, false, false);
    xr_program_coroutine_trap_arguments(child, 1u);
    if (panics) {
        XrCoreIrKey zero = xr_program_coroutine_trap_constant(child, 1u, "zero", constants[1].key);
        XrCoreIrKey operands[] = {resumed, zero};
        XrCoreIrInstructionInput *divide = xr_program_coroutine_trap_emit(child, 1u,
            XR_CORE_OP_CORE_INTEGER_DIVMOD, "quotient", XR_CORE_TYPE_I64, operands, 2u);
        divide->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
        divide->successors = &child->successors[2];
        divide->successor_count = 1u;
        xr_program_coroutine_trap_emit(child, 1u, XR_CORE_OP_CORE_RETURN,
            NULL, XR_CORE_TYPE_VOID, NULL, 0u);
        XrCoreIrKey panic = xr_program_coroutine_trap_argument(child, 3u, "panic",
            XR_CORE_TYPE_PANIC_INFO, false, true);
        xr_program_coroutine_trap_arguments(child, 3u);
        xr_program_coroutine_trap_emit(child, 3u, XR_CORE_OP_CORE_PANIC_PUBLISH,
            NULL, XR_CORE_TYPE_VOID, &panic, 1u);
    } else {
        xr_program_coroutine_trap_emit(child, 1u, XR_CORE_OP_CORE_ERROR_PUBLISH,
            NULL, XR_CORE_TYPE_VOID, &resumed, 1u);
    }
    xr_program_coroutine_trap_emit(child, 2u, XR_CORE_OP_CORE_CANCEL_PUBLISH,
        NULL, XR_CORE_TYPE_VOID, NULL, 0u);
}

static void xr_program_coroutine_outcome_exits(XrProgramCoroutineTrapBody *parent,
                                               XrProgramCoroutineOutcomeMutation mutation) {
    bool handled = mutation >= XR_CORO_OUTCOME_HANDLED;
    for (uint32_t block = 1u; block < 6u; ++block) {
        char owner_name[32], snapshot_name[32];
        (void) snprintf(owner_name, sizeof(owner_name), "live-owner-%u", block);
        (void) snprintf(snapshot_name, sizeof(snapshot_name), "snapshot-%u", block);
        XrCoreIrKey payload = {0};
        if (block >= 4u)
            payload = xr_program_coroutine_trap_argument(parent, block,
                block == 4u ? "error-payload" : "panic-payload",
                block == 4u ? (mutation == XR_CORO_OUTCOME_ERROR_WRONG_TYPE
                                  ? XR_CORE_TYPE_ERROR : XR_CORE_TYPE_I64) : XR_CORE_TYPE_PANIC_INFO,
                false, block == 5u && mutation != XR_CORO_OUTCOME_PANIC_WRONG_OWNERSHIP);
        XrCoreIrKey live = xr_program_coroutine_trap_argument(parent, block, owner_name,
            XR_CORE_TYPE_STRING, false, true);
        if (block != 2u)
            xr_program_coroutine_trap_argument(parent, block, snapshot_name, XR_CORE_TYPE_I64,
                                               false, false);
        xr_program_coroutine_trap_arguments(parent, block);
        if (!((block == 4u && mutation == XR_CORO_OUTCOME_ERROR_MISSING_DROP) ||
              (block == 5u && mutation == XR_CORO_OUTCOME_PANIC_MISSING_DROP)))
            xr_program_coroutine_trap_emit(parent, block, XR_CORE_OP_CORE_OWNER_DROP, NULL,
                                           XR_CORE_TYPE_VOID, &live, 1u);
        if (handled && block == 5u)
            xr_program_coroutine_trap_emit(parent, block, XR_CORE_OP_CORE_OWNER_DROP, NULL,
                                           XR_CORE_TYPE_VOID, &payload, 1u);
        XrCoreIrInstructionInput *exit = xr_program_coroutine_trap_emit(parent, block,
            block == 1u || (handled && block >= 4u) ? XR_CORE_OP_CORE_RETURN
            : block == 2u ? XR_CORE_OP_CORE_CANCEL_PUBLISH
            : block == 3u ? XR_CORE_OP_CORE_TRAP : block == 4u ? XR_CORE_OP_CORE_ERROR_PUBLISH
                                                                         : XR_CORE_OP_CORE_PANIC_PUBLISH,
            NULL, XR_CORE_TYPE_VOID, block >= 4u && !handled ? &payload : NULL,
            block >= 4u && !handled ? 1u : 0u);
        if (block == 3u) {
            exit->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
            exit->immediate.u32 = 7u;
        }
    }
}

static XrProgramBuildStatus xr_program_coroutine_outcome_fixture_write(
    bool indirect, bool panics, XrProgramCoroutineOutcomeMutation mutation,
    XrProgramArtifact *artifact, char *diagnostic, size_t diagnostic_size) {
    XrProgramCoroutineTrapBody child, parent;
    xr_program_coroutine_trap_body_init(&child, "outcome-child", panics ? 4u : 3u);
    xr_program_coroutine_trap_body_init(&parent, "outcome-parent", 6u);
    uint16_t parameter = XR_CORE_TYPE_I64;
    XrParamMode mode = XR_PARAM_READ;
    uint32_t effects = XR_CORE_EFFECT_CANCEL | XR_CORE_EFFECT_SUSPEND |
                       XR_CORE_EFFECT_ERROR | XR_CORE_EFFECT_PANIC;
    child.function.parameter_types = &parameter;
    child.function.parameter_modes = &mode;
    child.function.parameter_count = 1u;
    child.function.result_type_id = parent.function.result_type_id = XR_CORE_TYPE_VOID;
    child.function.error_type_id = parent.function.error_type_id = XR_CORE_TYPE_I64;
    child.function.panic_type_id = parent.function.panic_type_id = XR_CORE_TYPE_PANIC_INFO;
    child.function.effect_mask = effects;
    parent.function.effect_mask = effects | XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_TRAP;
    if (mutation >= XR_CORO_OUTCOME_HANDLED) {
        parent.function.error_type_id = parent.function.panic_type_id = XR_CORE_TYPE_VOID;
        parent.function.effect_mask &= ~(XR_CORE_EFFECT_ERROR | XR_CORE_EFFECT_PANIC);
    }
    if (mutation == XR_CORO_OUTCOME_HANDLED_MISSING_EFFECT)
        child.function.effect_mask |= XR_CORE_EFFECT_TARGET_QUERY;
    child.function.capability_mask = parent.function.capability_mask =
        XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION;
    parent.function.flags = XR_PROGRAM_FUNCTION_ENTRY;
    parent.states[1].continuation_block = parent.blocks[0].key;
    XrCoreIrConstantInput constants[] = {
        {.key = xr_program_coroutine_trap_key("outcomes", "constant"),
         .type_id = XR_CORE_TYPE_I64, .kind = XR_CORE_IR_CONSTANT_I64, .value.i64 = 42},
        {.key = xr_program_coroutine_trap_key("outcomes", "zero"),
         .type_id = XR_CORE_TYPE_I64, .kind = XR_CORE_IR_CONSTANT_I64, .value.i64 = 0},
    };
    xr_program_coroutine_outcome_child(&child, panics, constants);
    XrCoreIrCallableSignatureInput signature = {
        .parameter_types = &parameter, .parameter_modes = &mode, .parameter_count = 1u,
        .result_type_id = XR_CORE_TYPE_VOID, .error_type_id = XR_CORE_TYPE_I64,
        .panic_type_id = XR_CORE_TYPE_PANIC_INFO, .effect_mask = child.function.effect_mask,
        .capability_mask = XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION,
    };
    XrCoreIrTypeInput callable = {
        .key = xr_program_coroutine_trap_key("outcomes", "callable"), .local_id = 70u,
        .kind = XR_CORE_IR_TYPE_CALLABLE, .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT, .callable_signature = &signature,
    };
    XrCoreIrKey number = xr_program_coroutine_trap_constant(&parent, 0u, "number", constants[0].key);
    XrCoreIrKey nonlive = xr_program_coroutine_trap_constant(&parent, 0u, "nonlive", constants[0].key);
    XrCoreIrInstructionInput *text = xr_program_coroutine_trap_emit(&parent, 0u,
        XR_CORE_OP_CORE_STRING_FROM_SCALAR, "owner", XR_CORE_TYPE_STRING, &number, 1u);
    text->result_ownership = XR_CORE_IR_OWNER;
    XrCoreIrKey owner = text->result;
    XrCoreIrKey carrier = {0};
    if (indirect) {
        XrCoreIrInstructionInput *pack = xr_program_coroutine_trap_emit(&parent, 0u,
            XR_CORE_OP_CORE_CALLABLE_PACK, "callable", 70u, NULL, 0u);
        pack->immediate_kind = XR_CORE_IR_IMMEDIATE_FUNCTION;
        pack->immediate.key = child.function.key;
        carrier = pack->result;
    }
    parent.live[0] = owner;
    parent.live[1] = number;
    XrCoreIrKey operands[] = {carrier, number, owner, number, owner,
        owner, mutation == XR_CORO_OUTCOME_ERROR_NONLIVE ? nonlive : number,
        owner, mutation == XR_CORO_OUTCOME_PANIC_NONLIVE ? nonlive : number, owner, number};
    XrCoreIrKey successors[] = {parent.blocks[1].key, parent.blocks[2].key,
                               parent.blocks[4].key, parent.blocks[5].key, parent.blocks[3].key};
    if (mutation == XR_CORO_OUTCOME_REVERSED_CHANNELS) {
        successors[2] = parent.blocks[5].key;
        successors[3] = parent.blocks[4].key;
    }
    if (mutation == XR_CORO_OUTCOME_ERROR_CANCEL_ALIAS) successors[2] = parent.blocks[2].key;
    if (mutation == XR_CORO_OUTCOME_PANIC_CANCEL_ALIAS) successors[3] = parent.blocks[2].key;
    XrCoreIrInstructionInput *call = xr_program_coroutine_trap_emit(&parent, 0u,
        indirect ? XR_CORE_OP_CORE_COROUTINE_CALL_INDIRECT : XR_CORE_OP_CORE_COROUTINE_CALL_SEALED,
        NULL, XR_CORE_TYPE_VOID, operands + (indirect ? 0u : 1u), indirect ? 11u : 10u);
    call->immediate_kind = indirect ? XR_CORE_IR_IMMEDIATE_U32 : XR_CORE_IR_IMMEDIATE_COROUTINE_CALL;
    if (!indirect) call->immediate.coroutine_call.callee = child.function.key;
    call->successors = successors;
    call->successor_count = mutation == XR_CORO_OUTCOME_MISSING_CHANNEL ? 3u : 5u;
    xr_program_coroutine_outcome_exits(&parent, mutation);
    XrCoreIrFunctionInput functions[] = {child.function, parent.function};
    XrCoreIrModuleInput module = {
        .key = xr_program_coroutine_trap_key("outcomes", "module"), .constants = constants,
        .constant_count = 2u, .functions = functions, .function_count = 2u,
    };
    uint8_t profile[XR_PROGRAM_DIGEST_SIZE] = {0};
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile, .required_features = &feature,
        .required_feature_count = 1u, .types = indirect ? &callable : NULL,
        .type_count = indirect ? 1u : 0u, .modules = &module, .module_count = 1u,
    };
    XrCoreIrProgram *program = NULL;
    XrProgramBuildStatus status = xr_core_ir_program_build(&input, &program, diagnostic, diagnostic_size);
    if (status == XR_PROGRAM_BUILD_OK)
        status = xr_program_write(program, artifact, diagnostic, diagnostic_size);
    xr_core_ir_program_free(program);
    return status;
}

#endif
