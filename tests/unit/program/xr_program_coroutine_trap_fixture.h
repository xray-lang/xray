/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_coroutine_trap_fixture.h - Independent child-failure cleanup inputs
 *
 * KEY CONCEPT:
 *   A child mutates its parent's aggregate field through REF. Parent cleanup
 *   needs both the unique aggregate owner and a separate scalar snapshot after
 *   child failure, whether failure happens while stepping or cancelling.
 */

#ifndef XR_PROGRAM_COROUTINE_TRAP_FIXTURE_H
#define XR_PROGRAM_COROUTINE_TRAP_FIXTURE_H

#include "core/xr_core_spec_gen.h"
#include "plan/semantic/xr_semantic_ids.h"
#include "program/xr_program.h"

#include <assert.h>
#include <string.h>

typedef enum XrProgramCoroutineTrapMutation {
    XR_PROGRAM_COROUTINE_TRAP_VALID = 0,
    XR_PROGRAM_COROUTINE_TRAP_NO_TRAP_EDGE,
    XR_PROGRAM_COROUTINE_TRAP_BEFORE_YIELD,
    XR_PROGRAM_COROUTINE_TRAP_OTHER_CHILD_TRAP,
    XR_PROGRAM_COROUTINE_TRAP_REVERSED_LIVE_TUPLE,
    XR_PROGRAM_COROUTINE_TRAP_CANCEL_NONOWNER,
    XR_PROGRAM_COROUTINE_TRAP_MISSING_OWNER,
    XR_PROGRAM_COROUTINE_TRAP_DUPLICATE_OWNER,
    XR_PROGRAM_COROUTINE_TRAP_OWNER_NOT_DROPPED,
    XR_PROGRAM_COROUTINE_TRAP_NONLIVE_INPUT,
    XR_PROGRAM_COROUTINE_TRAP_WRONG_TYPE,
    XR_PROGRAM_COROUTINE_TRAP_WRONG_CATEGORY,
    XR_PROGRAM_COROUTINE_TRAP_WRONG_OWNERSHIP,
    XR_PROGRAM_COROUTINE_TRAP_WRONG_TERMINAL,
    XR_PROGRAM_COROUTINE_TRAP_CANCEL_TARGET,
    XR_PROGRAM_COROUTINE_TRAP_TRUNCATED_INPUT,
    XR_PROGRAM_COROUTINE_TRAP_EXTRA_INPUT,
    XR_PROGRAM_COROUTINE_TRAP_PAYLOAD_WITHOUT_EDGE,
    XR_PROGRAM_COROUTINE_TRAP_DUPLICATE_CANCEL_OWNER,
    XR_PROGRAM_COROUTINE_TRAP_MOVED_CALLER_OWNER,
    XR_PROGRAM_COROUTINE_TRAP_DUPLICATE_LIVE_VALUE,
} XrProgramCoroutineTrapMutation;

enum {
    XR_PROGRAM_COROUTINE_TRAP_AGGREGATE = 62
};

typedef struct XrProgramCoroutineTrapBody {
    const char *scope;
    XrCoreIrInstructionInput instructions[4][16];
    XrCoreIrKey operands[4][40];
    uint32_t operand_counts[4];
    XrCoreIrValueInput arguments[4][4];
    XrCoreIrKey argument_keys[4][4];
    XrCoreIrBlockInput blocks[4];
    XrCoreIrKey successors[3];
    XrCoreIrKey live[2];
    XrCoreIrCoroutineStateInput states[2];
    XrCoreIrCoroutineSafepointInput safepoint;
    uint16_t parameter_types[2];
    XrParamMode parameter_modes[2];
    XrCoreIrFunctionInput function;
} XrProgramCoroutineTrapBody;

static XrCoreIrKey xr_program_coroutine_trap_key(const char *scope, const char *name) {
    static const uint8_t prefix[] = "coroutine-trap:";
    uint8_t material[160];
    size_t scope_size = strlen(scope);
    size_t name_size = strlen(name);
    size_t cursor = sizeof(prefix) - 1u;
    assert(scope_size <= sizeof(material) - cursor - 1u);
    assert(name_size <= sizeof(material) - cursor - scope_size - 1u);
    memcpy(material, prefix, cursor);
    memcpy(material + cursor, scope, scope_size);
    cursor += scope_size;
    material[cursor++] = ':';
    memcpy(material + cursor, name, name_size);
    cursor += name_size;
    return xr_core_ir_key(material, cursor);
}

static void xr_program_coroutine_trap_body_init(XrProgramCoroutineTrapBody *body, const char *scope,
                                                uint32_t block_count) {
    memset(body, 0, sizeof(*body));
    body->scope = scope;
    const char *names[] = {"entry", "normal", "cancel", "trap"};
    for (uint32_t block = 0u; block < 4u; ++block) {
        body->blocks[block].key = xr_program_coroutine_trap_key(scope, names[block]);
        body->blocks[block].instructions = body->instructions[block];
        if (block != 0u)
            body->successors[block - 1u] = body->blocks[block].key;
    }
    body->states[0] =
        (XrCoreIrCoroutineStateInput) {.state_id = 0u, .continuation_block = body->blocks[0].key};
    body->states[1] =
        (XrCoreIrCoroutineStateInput) {.state_id = 1u, .continuation_block = body->blocks[1].key};
    body->safepoint = (XrCoreIrCoroutineSafepointInput) {.safepoint_id = 0u,
                                                         .resume_state_id = 1u,
                                                         .live_values = body->live,
                                                         .live_value_count = 2u};
    body->function = (XrCoreIrFunctionInput) {
        .key = xr_program_coroutine_trap_key(scope, "function"),
        .result_type_id = XR_CORE_TYPE_I64,
        .effect_mask = XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_SUSPEND | XR_CORE_EFFECT_CANCEL |
                       XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_PROVIDER_CALL,
        .capability_mask =
            XR_CORE_CAPABILITY_RUNTIME_COOPERATIVE_YIELD | XR_CORE_CAPABILITY_PROVIDER_BINDING,
        .entry_block = body->blocks[0].key,
        .blocks = body->blocks,
        .block_count = block_count,
        .coroutine_states = body->states,
        .coroutine_state_count = 2u,
        .coroutine_safepoints = &body->safepoint,
        .coroutine_safepoint_count = 1u,
    };
}

static XrCoreIrInstructionInput *xr_program_coroutine_trap_emit(XrProgramCoroutineTrapBody *body,
                                                                uint32_t block, uint16_t operation,
                                                                const char *result, uint16_t type,
                                                                const XrCoreIrKey *operands,
                                                                uint32_t count) {
    assert(block < 4u && body->blocks[block].instruction_count < 16u);
    assert(count <= 40u - body->operand_counts[block]);
    XrCoreIrInstructionInput *instruction =
        &body->instructions[block][body->blocks[block].instruction_count++];
    instruction->operation_id = operation;
    instruction->result_type_id = type;
    if (result)
        instruction->result = xr_program_coroutine_trap_key(body->scope, result);
    if (count != 0u) {
        XrCoreIrKey *stored = &body->operands[block][body->operand_counts[block]];
        memcpy(stored, operands, count * sizeof(*stored));
        instruction->operands = stored;
        instruction->operand_count = count;
        body->operand_counts[block] += count;
    }
    return instruction;
}

static XrCoreIrKey xr_program_coroutine_trap_argument(XrProgramCoroutineTrapBody *body,
                                                      uint32_t block, const char *name,
                                                      uint16_t type, bool place, bool owner) {
    uint32_t index = body->blocks[block].argument_count++;
    assert(index < 4u);
    body->blocks[block].arguments = body->arguments[block];
    XrCoreIrKey key = xr_program_coroutine_trap_key(body->scope, name);
    body->argument_keys[block][index] = key;
    body->arguments[block][index] = (XrCoreIrValueInput) {
        .key = key,
        .type_id = type,
        .category = place ? XR_CORE_IR_PLACE : XR_CORE_IR_VALUE,
        .ownership = owner ? XR_CORE_IR_OWNER : XR_CORE_IR_NON_OWNER,
    };
    return key;
}

static void xr_program_coroutine_trap_arguments(XrProgramCoroutineTrapBody *body, uint32_t block) {
    xr_program_coroutine_trap_emit(body, block, XR_CORE_OP_CORE_BLOCK_ARGUMENT, NULL,
                                   XR_CORE_TYPE_VOID, body->argument_keys[block],
                                   body->blocks[block].argument_count);
}

static XrCoreIrKey xr_program_coroutine_trap_constant(XrProgramCoroutineTrapBody *body,
                                                      uint32_t block, const char *name,
                                                      XrCoreIrKey constant) {
    XrCoreIrInstructionInput *instruction = xr_program_coroutine_trap_emit(
        body, block, XR_CORE_OP_CORE_CONSTANT_I64, name, XR_CORE_TYPE_I64, NULL, 0u);
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT;
    instruction->immediate.key = constant;
    return instruction->result;
}

static void xr_program_coroutine_trap_provider(XrProgramCoroutineTrapBody *body, uint32_t block,
                                               const char *name, XrCoreIrKey value,
                                               XrStableId contract, XrStableId operation) {
    XrCoreIrInstructionInput *instruction = xr_program_coroutine_trap_emit(
        body, block, XR_CORE_OP_CORE_PROVIDER_CALL, name, XR_CORE_TYPE_I64, &value, 1u);
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION;
    instruction->immediate.provider_operation.contract_id = contract;
    instruction->immediate.provider_operation.operation_id = operation;
}

static XrCoreIrKey xr_program_coroutine_trap_drop_payload(XrProgramCoroutineTrapBody *body,
                                                          uint32_t block, const char *name,
                                                          XrCoreIrKey owner, bool drop) {
    XrCoreIrInstructionInput *project = xr_program_coroutine_trap_emit(
        body, block, XR_CORE_OP_CORE_AGGREGATE_PROJECT, name, XR_CORE_TYPE_I64, &owner, 1u);
    project->immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD;
    project->immediate.field_ordinal = 0u;
    if (drop)
        xr_program_coroutine_trap_emit(body, block, XR_CORE_OP_CORE_OWNER_DROP, NULL,
                                       XR_CORE_TYPE_VOID, &owner, 1u);
    return project->result;
}

static void xr_program_coroutine_trap_child_init(XrProgramCoroutineTrapBody *child,
                                                 XrProgramCoroutineTrapMutation mutation,
                                                 const XrCoreIrConstantInput *constants,
                                                 XrStableId contract, XrStableId operation) {
    xr_program_coroutine_trap_body_init(child, "child", 3u);
    bool moved = mutation == XR_PROGRAM_COROUTINE_TRAP_MOVED_CALLER_OWNER;
    uint16_t token_type = moved ? XR_PROGRAM_COROUTINE_TRAP_AGGREGATE : XR_CORE_TYPE_I64;
    XrCoreIrKey token =
        xr_program_coroutine_trap_argument(child, 0u, "token", token_type, false, moved);
    XrCoreIrKey place =
        xr_program_coroutine_trap_argument(child, 0u, "field", XR_CORE_TYPE_I64, true, false);
    xr_program_coroutine_trap_arguments(child, 0u);
    if (mutation == XR_PROGRAM_COROUTINE_TRAP_BEFORE_YIELD)
        xr_program_coroutine_trap_provider(child, 0u, "before-result", token, contract, operation);
    XrCoreIrKey forty_four =
        xr_program_coroutine_trap_constant(child, 0u, "forty-four", constants[3].key);
    XrCoreIrKey stored[] = {place, forty_four};
    xr_program_coroutine_trap_emit(child, 0u, XR_CORE_OP_CORE_PLACE_STORE, NULL, XR_CORE_TYPE_VOID,
                                   stored, 2u);
    XrCoreIrKey owner = token;
    if (!moved) {
        XrCoreIrInstructionInput *construct =
            xr_program_coroutine_trap_emit(child, 0u, XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT, "owner",
                                           XR_PROGRAM_COROUTINE_TRAP_AGGREGATE, &token, 1u);
        construct->result_ownership = XR_CORE_IR_OWNER;
        owner = construct->result;
    }
    child->live[0] = moved ? owner : place;
    child->live[1] = moved ? place : owner;
    XrCoreIrKey yield_operands[] = {child->live[0], child->live[1], owner};
    XrCoreIrInstructionInput *yield = xr_program_coroutine_trap_emit(
        child, 0u, XR_CORE_OP_CORE_COROUTINE_YIELD, NULL, XR_CORE_TYPE_VOID, yield_operands, 3u);
    yield->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
    yield->successors = child->successors;
    yield->successor_count = 2u;
    child->parameter_types[0] = token_type;
    child->parameter_types[1] = XR_CORE_TYPE_I64;
    child->parameter_modes[0] = moved ? XR_PARAM_MOVE : XR_PARAM_READ;
    child->parameter_modes[1] = XR_PARAM_REF;
    child->function.parameter_types = child->parameter_types;
    child->function.parameter_modes = child->parameter_modes;
    child->function.parameter_count = 2u;

    for (uint32_t block = 1u; block <= 2u; ++block) {
        XrCoreIrKey resumed_place = {{0}};
        if (block == 1u && !moved)
            resumed_place = xr_program_coroutine_trap_argument(child, block, "normal-field",
                                                               XR_CORE_TYPE_I64, true, false);
        XrCoreIrKey resumed_owner = xr_program_coroutine_trap_argument(
            child, block, block == 1u ? "normal-owner" : "cancel-owner",
            XR_PROGRAM_COROUTINE_TRAP_AGGREGATE, false, true);
        if (block == 1u && moved)
            resumed_place = xr_program_coroutine_trap_argument(child, block, "normal-field",
                                                               XR_CORE_TYPE_I64, true, false);
        xr_program_coroutine_trap_arguments(child, block);
        if (block == 1u) {
            XrCoreIrKey fifty_five =
                xr_program_coroutine_trap_constant(child, block, "fifty-five", constants[4].key);
            XrCoreIrKey update[] = {resumed_place, fifty_five};
            xr_program_coroutine_trap_emit(child, block, XR_CORE_OP_CORE_PLACE_STORE, NULL,
                                           XR_CORE_TYPE_VOID, update, 2u);
        }
        XrCoreIrKey payload = xr_program_coroutine_trap_drop_payload(
            child, block, block == 1u ? "normal-payload" : "cancel-payload", resumed_owner, true);
        xr_program_coroutine_trap_provider(child, block,
                                           block == 1u ? "normal-result" : "cancel-result", payload,
                                           contract, operation);
        XrCoreIrInstructionInput *terminal = xr_program_coroutine_trap_emit(
            child, block, block == 1u ? XR_CORE_OP_CORE_RETURN : XR_CORE_OP_CORE_CANCEL_PUBLISH,
            NULL, XR_CORE_TYPE_VOID, block == 1u ? &payload : NULL, block == 1u ? 1u : 0u);
        if (block == 1u && mutation == XR_PROGRAM_COROUTINE_TRAP_OTHER_CHILD_TRAP) {
            terminal->operation_id = XR_CORE_OP_CORE_TRAP;
            terminal->operands = NULL;
            terminal->operand_count = 0u;
            terminal->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
            terminal->immediate.u32 = 4u;
        }
    }
}

static void xr_program_coroutine_trap_parent_cleanup(XrProgramCoroutineTrapBody *parent,
                                                     uint32_t block,
                                                     XrProgramCoroutineTrapMutation mutation,
                                                     XrStableId contract, XrStableId operation) {
    bool trap = block == 3u;
    bool missing = trap && mutation == XR_PROGRAM_COROUTINE_TRAP_MISSING_OWNER;
    bool duplicate = trap ? mutation == XR_PROGRAM_COROUTINE_TRAP_DUPLICATE_OWNER
                          : mutation == XR_PROGRAM_COROUTINE_TRAP_DUPLICATE_CANCEL_OWNER;
    uint32_t owners = missing ? 0u : duplicate ? 2u : 1u;
    const char *owner_names[] = {trap ? "trap-owner" : "cancel-owner",
                                 trap ? "trap-extra-owner" : "cancel-extra-owner"};
    for (uint32_t index = 0u; index < owners; ++index)
        xr_program_coroutine_trap_argument(
            parent, block, owner_names[index], XR_PROGRAM_COROUTINE_TRAP_AGGREGATE, false,
            !(trap && mutation == XR_PROGRAM_COROUTINE_TRAP_WRONG_OWNERSHIP));
    bool carries_snapshot = trap || mutation == XR_PROGRAM_COROUTINE_TRAP_CANCEL_NONOWNER;
    XrCoreIrKey snapshot = {{0}};
    if (carries_snapshot)
        snapshot = xr_program_coroutine_trap_argument(parent, block,
                                                      trap ? "trap-snapshot" : "cancel-snapshot",
                                                      XR_CORE_TYPE_I64, false, false);
    xr_program_coroutine_trap_arguments(parent, block);
    const char *payload_names[] = {trap ? "trap-payload" : "cancel-payload",
                                   trap ? "trap-extra-payload" : "cancel-extra-payload"};
    const char *result_names[] = {trap ? "trap-result" : "cancel-result",
                                  trap ? "trap-extra-result" : "cancel-extra-result"};
    for (uint32_t index = 0u; index < owners; ++index) {
        XrCoreIrKey payload = xr_program_coroutine_trap_drop_payload(
            parent, block, payload_names[index], parent->argument_keys[block][index],
            !(trap && mutation == XR_PROGRAM_COROUTINE_TRAP_OWNER_NOT_DROPPED));
        xr_program_coroutine_trap_provider(parent, block, result_names[index], payload, contract,
                                           operation);
    }
    if (carries_snapshot)
        xr_program_coroutine_trap_provider(parent, block,
                                           trap ? "snapshot-result" : "cancel-snapshot-result",
                                           snapshot, contract, operation);
    XrCoreIrInstructionInput *terminal = xr_program_coroutine_trap_emit(
        parent, block, trap ? XR_CORE_OP_CORE_TRAP : XR_CORE_OP_CORE_CANCEL_PUBLISH, NULL,
        XR_CORE_TYPE_VOID, NULL, 0u);
    if (trap) {
        terminal->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
        terminal->immediate.u32 = mutation == XR_PROGRAM_COROUTINE_TRAP_WRONG_TERMINAL ? 4u : 7u;
    }
}

static void xr_program_coroutine_trap_parent_normal(XrProgramCoroutineTrapBody *parent,
                                                    XrProgramCoroutineTrapMutation mutation,
                                                    XrStableId contract, XrStableId operation) {
    XrCoreIrKey result = xr_program_coroutine_trap_argument(parent, 1u, "child-result",
                                                            XR_CORE_TYPE_I64, false, false);
    XrCoreIrKey owner = {{0}};
    XrCoreIrKey snapshot = {{0}};
    if (mutation == XR_PROGRAM_COROUTINE_TRAP_REVERSED_LIVE_TUPLE) {
        snapshot = xr_program_coroutine_trap_argument(parent, 1u, "normal-snapshot",
                                                      XR_CORE_TYPE_I64, false, false);
        owner = xr_program_coroutine_trap_argument(
            parent, 1u, "normal-owner", XR_PROGRAM_COROUTINE_TRAP_AGGREGATE, false, true);
    } else {
        owner = xr_program_coroutine_trap_argument(
            parent, 1u, "normal-owner", XR_PROGRAM_COROUTINE_TRAP_AGGREGATE, false, true);
        snapshot = xr_program_coroutine_trap_argument(parent, 1u, "normal-snapshot",
                                                      XR_CORE_TYPE_I64, false, false);
    }
    xr_program_coroutine_trap_arguments(parent, 1u);
    XrCoreIrKey payload =
        xr_program_coroutine_trap_drop_payload(parent, 1u, "normal-payload", owner, true);
    xr_program_coroutine_trap_provider(parent, 1u, "normal-result", payload, contract, operation);
    XrCoreIrKey sum_operands[] = {result, snapshot};
    XrCoreIrInstructionInput *sum = xr_program_coroutine_trap_emit(
        parent, 1u, XR_CORE_OP_CORE_ADD_I64, "sum", XR_CORE_TYPE_I64, sum_operands, 2u);
    sum->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
    sum->immediate.u32 = 1u;
    xr_program_coroutine_trap_emit(parent, 1u, XR_CORE_OP_CORE_RETURN, NULL, XR_CORE_TYPE_VOID,
                                   &sum->result, 1u);
}

static void xr_program_coroutine_trap_parent_init(XrProgramCoroutineTrapBody *parent,
                                                  const XrProgramCoroutineTrapBody *child,
                                                  XrProgramCoroutineTrapMutation mutation,
                                                  const XrCoreIrConstantInput *constants,
                                                  XrStableId contract, XrStableId operation) {
    bool no_trap = mutation == XR_PROGRAM_COROUTINE_TRAP_NO_TRAP_EDGE ||
                   mutation == XR_PROGRAM_COROUTINE_TRAP_PAYLOAD_WITHOUT_EDGE;
    bool cancel_target = mutation == XR_PROGRAM_COROUTINE_TRAP_CANCEL_TARGET;
    xr_program_coroutine_trap_body_init(parent, "parent", no_trap || cancel_target ? 3u : 4u);
    parent->function.flags = XR_PROGRAM_FUNCTION_ENTRY;
    parent->states[1].continuation_block = parent->blocks[0].key;
    XrCoreIrKey token = xr_program_coroutine_trap_constant(parent, 0u, "token", constants[0].key);
    XrCoreIrKey initial =
        xr_program_coroutine_trap_constant(parent, 0u, "initial", constants[1].key);
    XrCoreIrInstructionInput *construct =
        xr_program_coroutine_trap_emit(parent, 0u, XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT, "owner",
                                       XR_PROGRAM_COROUTINE_TRAP_AGGREGATE, &initial, 1u);
    construct->result_ownership = XR_CORE_IR_OWNER;
    XrCoreIrKey owner = construct->result;
    XrCoreIrKey snapshot =
        xr_program_coroutine_trap_constant(parent, 0u, "snapshot", constants[2].key);
    XrCoreIrInstructionInput *place =
        xr_program_coroutine_trap_emit(parent, 0u, XR_CORE_OP_CORE_PLACE_LOCAL, "owner-place",
                                       XR_PROGRAM_COROUTINE_TRAP_AGGREGATE, &owner, 1u);
    place->result_category = XR_CORE_IR_PLACE;
    XrCoreIrInstructionInput *field = xr_program_coroutine_trap_emit(
        parent, 0u, XR_CORE_OP_CORE_PLACE_PROJECT, "field", XR_CORE_TYPE_I64, &place->result, 1u);
    field->result_category = XR_CORE_IR_PLACE;
    field->immediate_kind = XR_CORE_IR_IMMEDIATE_FIELD;
    field->immediate.field_ordinal = 0u;
    parent->live[0] = owner;
    parent->live[1] = snapshot;
    xr_program_coroutine_trap_parent_normal(parent, mutation, contract, operation);
    xr_program_coroutine_trap_parent_cleanup(parent, 2u, mutation, contract, operation);
    if (!no_trap && !cancel_target)
        xr_program_coroutine_trap_parent_cleanup(parent, 3u, mutation, contract, operation);
    if (cancel_target)
        parent->successors[2] = parent->successors[1];

    XrCoreIrKey call_values[9] = {token, field->result, owner, snapshot};
    if (mutation == XR_PROGRAM_COROUTINE_TRAP_REVERSED_LIVE_TUPLE) {
        call_values[2] = snapshot;
        call_values[3] = owner;
    }
    if (mutation == XR_PROGRAM_COROUTINE_TRAP_DUPLICATE_LIVE_VALUE)
        call_values[3] = owner;
    if (mutation == XR_PROGRAM_COROUTINE_TRAP_MOVED_CALLER_OWNER)
        call_values[0] = owner;
    uint32_t count = 4u;
    for (uint32_t cancel = 0u; cancel < parent->blocks[2].argument_count; ++cancel)
        call_values[count++] =
            parent->blocks[2].arguments[cancel].type_id == XR_CORE_TYPE_I64 ? snapshot : owner;
    uint32_t trap_start = count;
    if (cancel_target)
        call_values[count++] = owner;
    else if (!no_trap || mutation == XR_PROGRAM_COROUTINE_TRAP_PAYLOAD_WITHOUT_EDGE) {
        if (mutation != XR_PROGRAM_COROUTINE_TRAP_MISSING_OWNER)
            call_values[count++] = owner;
        if (mutation == XR_PROGRAM_COROUTINE_TRAP_DUPLICATE_OWNER)
            call_values[count++] = owner;
        call_values[count++] = snapshot;
    }
    if (mutation == XR_PROGRAM_COROUTINE_TRAP_NONLIVE_INPUT)
        call_values[count - 1u] = token;
    if (mutation == XR_PROGRAM_COROUTINE_TRAP_WRONG_TYPE)
        call_values[trap_start] = snapshot;
    if (mutation == XR_PROGRAM_COROUTINE_TRAP_WRONG_CATEGORY) {
        XrCoreIrInstructionInput *raw =
            xr_program_coroutine_trap_emit(parent, 0u, XR_CORE_OP_CORE_PLACE_LOCAL,
                                           "snapshot-place", XR_CORE_TYPE_I64, &snapshot, 1u);
        raw->result_category = XR_CORE_IR_PLACE;
        call_values[count - 1u] = raw->result;
    }
    if (mutation == XR_PROGRAM_COROUTINE_TRAP_TRUNCATED_INPUT)
        --count;
    if (mutation == XR_PROGRAM_COROUTINE_TRAP_EXTRA_INPUT)
        call_values[count++] = token;
    XrCoreIrInstructionInput *call =
        xr_program_coroutine_trap_emit(parent, 0u, XR_CORE_OP_CORE_COROUTINE_CALL_SEALED, NULL,
                                       XR_CORE_TYPE_VOID, call_values, count);
    call->immediate_kind = XR_CORE_IR_IMMEDIATE_COROUTINE_CALL;
    call->immediate.coroutine_call.callee = child->function.key;
    call->immediate.coroutine_call.safepoint_id = 0u;
    call->successors = parent->successors;
    call->successor_count = no_trap ? 2u : 3u;
}

/* Provider shape is i64(i64); its return value is deliberately ignored.
 * Initial step: suspended, no events. Successful resume: [11, 55], return 44.
 * Successful cancel: [11, 44], cancelled. Refusal at 11: resume [11, 55, 33],
 * cancel [11, 44, 33], both trap 7. Before-yield refusal: [11, 22, 33], trap 7.
 * Without the parent trap edge, refusal returns trap 7 after [11] only.
 * OTHER_CHILD_TRAP resumes through [11] then trap 4 without parent cleanup. */
static XrProgramBuildStatus xr_program_coroutine_trap_fixture_write_with_ids(
    XrStableId contract, XrStableId operation, XrProgramCoroutineTrapMutation mutation,
    XrProgramArtifact *artifact, char *diagnostic, size_t diagnostic_size) {
    const int64_t values[] = {11, 22, 33, 44, 55};
    const char *names[] = {"eleven", "twenty-two", "thirty-three", "forty-four", "fifty-five"};
    XrCoreIrConstantInput constants[5] = {0};
    for (uint32_t index = 0u; index < 5u; ++index) {
        constants[index].key = xr_program_coroutine_trap_key("constant", names[index]);
        constants[index].type_id = XR_CORE_TYPE_I64;
        constants[index].kind = XR_CORE_IR_CONSTANT_I64;
        constants[index].value.i64 = values[index];
    }
    XrProgramCoroutineTrapBody child;
    XrProgramCoroutineTrapBody parent;
    xr_program_coroutine_trap_child_init(&child, mutation, constants, contract, operation);
    xr_program_coroutine_trap_parent_init(&parent, &child, mutation, constants, contract,
                                          operation);
    uint16_t field_type = XR_CORE_TYPE_I64;
    XrCoreIrTypeInput type = {
        .key = xr_program_coroutine_trap_key("type", "owner"),
        .local_id = XR_PROGRAM_COROUTINE_TRAP_AGGREGATE,
        .kind = XR_CORE_IR_TYPE_AGGREGATE,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = &field_type,
        .field_count = 1u,
    };
    XrCoreIrFunctionInput functions[] = {parent.function, child.function};
    XrCoreIrModuleInput module = {
        .key = xr_program_coroutine_trap_key("module", "fixture"),
        .constants = constants,
        .constant_count = 5u,
        .functions = functions,
        .function_count = 2u,
    };
    XrCoreIrProviderRequirementInput requirement = {
        .contract_id = contract, .operation_ids = &operation, .operation_count = 1u};
    XrCoreIrKey semantic = xr_program_coroutine_trap_key("profile", "fixture");
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = semantic.bytes,
        .required_features = &feature,
        .required_feature_count = 1u,
        .types = &type,
        .type_count = 1u,
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
    if (status != XR_PROGRAM_BUILD_OK)
        fprintf(stderr, "coroutine trap fixture %u build failed: %s\n", (unsigned) mutation,
                diagnostic ? diagnostic : "no diagnostic");
    xr_core_ir_program_free(program);
    return status;
}

static inline XrProgramBuildStatus
xr_program_coroutine_trap_fixture_write(XrProgramCoroutineTrapMutation mutation,
                                        XrProgramArtifact *artifact, char *diagnostic,
                                        size_t diagnostic_size) {
    XrStableId contract = {{0}};
    XrStableId operation = {{0}};
    XrFingerprint fingerprint;
    if (!xr_stable_id_from_key("fixture.coroutine-trap.provider", &contract, &fingerprint) ||
        !xr_stable_id_from_key("fixture.coroutine-trap.record", &operation, &fingerprint))
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    return xr_program_coroutine_trap_fixture_write_with_ids(contract, operation, mutation, artifact,
                                                            diagnostic, diagnostic_size);
}

#endif /* XR_PROGRAM_COROUTINE_TRAP_FIXTURE_H */
