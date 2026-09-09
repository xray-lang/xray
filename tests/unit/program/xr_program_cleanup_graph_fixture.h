/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_cleanup_graph_fixture.h - Independent reason-private cleanup CFG
 */

#ifndef XR_PROGRAM_CLEANUP_GRAPH_FIXTURE_H
#define XR_PROGRAM_CLEANUP_GRAPH_FIXTURE_H

#include "core/xr_core_spec_gen.h"
#include "plan/semantic/xr_semantic_ids.h"
#include "program/xr_program.h"

#include <assert.h>
#include <string.h>

enum {
    XR_CLEANUP_GRAPH_ENTRY = 0,
    XR_CLEANUP_GRAPH_NORMAL,
    XR_CLEANUP_GRAPH_CANCEL_ADAPTER,
    XR_CLEANUP_GRAPH_CANCEL_BRANCH,
    XR_CLEANUP_GRAPH_CANCEL_BODY,
    XR_CLEANUP_GRAPH_CANCEL_PUBLISH,
    XR_CLEANUP_GRAPH_TRAP_ADAPTER,
    XR_CLEANUP_GRAPH_TRAP_BRANCH,
    XR_CLEANUP_GRAPH_TRAP_FIRST,
    XR_CLEANUP_GRAPH_TRAP_SECOND,
    XR_CLEANUP_GRAPH_BLOCK_COUNT,
    XR_CLEANUP_GRAPH_AFFINE_TYPE = 62,
};

typedef struct XrProgramCleanupGraphFixture {
    XrCoreIrInstructionInput instructions[XR_CLEANUP_GRAPH_BLOCK_COUNT][8];
    XrCoreIrKey operands[XR_CLEANUP_GRAPH_BLOCK_COUNT][24];
    XrCoreIrKey successors[XR_CLEANUP_GRAPH_BLOCK_COUNT][8];
    uint32_t operand_counts[XR_CLEANUP_GRAPH_BLOCK_COUNT];
    uint32_t successor_counts[XR_CLEANUP_GRAPH_BLOCK_COUNT];
    XrCoreIrValueInput arguments[XR_CLEANUP_GRAPH_BLOCK_COUNT][2];
    XrCoreIrKey argument_keys[XR_CLEANUP_GRAPH_BLOCK_COUNT][2];
    XrCoreIrBlockInput blocks[XR_CLEANUP_GRAPH_BLOCK_COUNT];
    XrCoreIrCoroutineStateInput states[2];
    XrCoreIrCoroutineSafepointInput safepoint;
    XrCoreIrKey live;
    XrCoreIrConstantInput constants[5];
    XrCoreIrFunctionInput functions[2];
    XrCoreIrModuleInput module;
    XrCoreIrProviderRequirementInput requirement;
    XrStableId operation;
    uint16_t field_type;
    XrCoreIrTypeInput type;
} XrProgramCleanupGraphFixture;

static XrCoreIrKey xr_program_cleanup_graph_key(const char *kind, uint32_t index) {
    static const uint8_t prefix[] = "cleanup-graph:";
    uint8_t material[128];
    size_t kind_size = strlen(kind);
    size_t cursor = sizeof(prefix) - 1u;
    assert(kind_size <= sizeof(material) - cursor - 5u);
    memcpy(material, prefix, cursor);
    memcpy(material + cursor, kind, kind_size);
    cursor += kind_size;
    material[cursor++] = ':';
    material[cursor++] = (uint8_t) (index >> 24u);
    material[cursor++] = (uint8_t) (index >> 16u);
    material[cursor++] = (uint8_t) (index >> 8u);
    material[cursor++] = (uint8_t) index;
    return xr_core_ir_key(material, cursor);
}

static XrCoreIrInstructionInput *
xr_program_cleanup_graph_emit(XrProgramCleanupGraphFixture *fixture, uint32_t block,
                              uint16_t operation, const XrCoreIrKey *operands, uint32_t count) {
    assert(block < XR_CLEANUP_GRAPH_BLOCK_COUNT);
    assert(fixture->blocks[block].instruction_count < 8u);
    assert(count <= 24u - fixture->operand_counts[block]);
    XrCoreIrInstructionInput *instruction =
        &fixture->instructions[block][fixture->blocks[block].instruction_count++];
    *instruction =
        (XrCoreIrInstructionInput) {.operation_id = operation, .result_type_id = XR_CORE_TYPE_VOID};
    if (count) {
        XrCoreIrKey *stored = &fixture->operands[block][fixture->operand_counts[block]];
        memcpy(stored, operands, count * sizeof(*stored));
        fixture->operand_counts[block] += count;
        instruction->operands = stored;
        instruction->operand_count = count;
    }
    return instruction;
}

static void xr_program_cleanup_graph_edges(XrProgramCleanupGraphFixture *fixture, uint32_t block,
                                           XrCoreIrInstructionInput *instruction,
                                           const uint32_t *targets, uint32_t count) {
    assert(count <= 8u - fixture->successor_counts[block]);
    XrCoreIrKey *stored = &fixture->successors[block][fixture->successor_counts[block]];
    for (uint32_t index = 0u; index < count; ++index)
        stored[index] = fixture->blocks[targets[index]].key;
    fixture->successor_counts[block] += count;
    instruction->successors = stored;
    instruction->successor_count = count;
}

static XrCoreIrKey xr_program_cleanup_graph_constant(XrProgramCleanupGraphFixture *fixture,
                                                     uint32_t block, uint32_t constant,
                                                     const char *kind) {
    bool boolean = fixture->constants[constant].type_id == XR_CORE_TYPE_BOOL;
    XrCoreIrInstructionInput *instruction = xr_program_cleanup_graph_emit(
        fixture, block, boolean ? XR_CORE_OP_CORE_CONSTANT_BOOL : XR_CORE_OP_CORE_CONSTANT_I64,
        NULL, 0u);
    instruction->result = xr_program_cleanup_graph_key(kind, block);
    instruction->result_type_id = boolean ? XR_CORE_TYPE_BOOL : XR_CORE_TYPE_I64;
    instruction->immediate_kind = XR_CORE_IR_IMMEDIATE_CONSTANT;
    instruction->immediate.key = fixture->constants[constant].key;
    return instruction->result;
}

static void xr_program_cleanup_graph_branch(XrProgramCleanupGraphFixture *fixture, uint32_t block,
                                            uint32_t target) {
    XrCoreIrInstructionInput *branch = xr_program_cleanup_graph_emit(
        fixture, block, XR_CORE_OP_CORE_BRANCH, fixture->argument_keys[block], 1u);
    xr_program_cleanup_graph_edges(fixture, block, branch, &target, 1u);
}

static void xr_program_cleanup_graph_conditional(XrProgramCleanupGraphFixture *fixture,
                                                 uint32_t block, const uint32_t *targets,
                                                 uint32_t constant) {
    XrCoreIrKey condition =
        xr_program_cleanup_graph_constant(fixture, block, constant, "condition");
    XrCoreIrKey owner = fixture->argument_keys[block][0];
    XrCoreIrKey operands[] = {condition, owner, owner};
    XrCoreIrInstructionInput *branch = xr_program_cleanup_graph_emit(
        fixture, block, XR_CORE_OP_CORE_CONDITIONAL_BRANCH, operands, 3u);
    xr_program_cleanup_graph_edges(fixture, block, branch, targets, 2u);
}

static void xr_program_cleanup_graph_provider(XrProgramCleanupGraphFixture *fixture, uint32_t block,
                                              uint32_t constant) {
    XrCoreIrKey token = xr_program_cleanup_graph_constant(fixture, block, constant, "token");
    XrCoreIrKey operands[] = {token, fixture->argument_keys[block][0]};
    XrCoreIrInstructionInput *call =
        xr_program_cleanup_graph_emit(fixture, block, XR_CORE_OP_CORE_PROVIDER_CALL, operands, 2u);
    call->result = xr_program_cleanup_graph_key("provider-result", block);
    call->result_type_id = XR_CORE_TYPE_I64;
    call->immediate_kind = XR_CORE_IR_IMMEDIATE_PROVIDER_OPERATION;
    call->immediate.provider_operation.contract_id = fixture->requirement.contract_id;
    call->immediate.provider_operation.operation_id = fixture->operation;
    uint32_t target = XR_CLEANUP_GRAPH_TRAP_ADAPTER;
    xr_program_cleanup_graph_edges(fixture, block, call, &target, 1u);
}

static void xr_program_cleanup_graph_terminal(XrProgramCleanupGraphFixture *fixture, uint32_t block,
                                              uint16_t operation) {
    xr_program_cleanup_graph_emit(fixture, block, XR_CORE_OP_CORE_OWNER_DROP,
                                  fixture->argument_keys[block], 1u);
    XrCoreIrInstructionInput *terminal =
        xr_program_cleanup_graph_emit(fixture, block, operation, NULL, 0u);
    if (operation == XR_CORE_OP_CORE_TRAP) {
        terminal->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
        terminal->immediate.u32 = 7u;
    }
}

static void xr_program_cleanup_graph_entry(XrProgramCleanupGraphFixture *fixture) {
    XrCoreIrKey seed = xr_program_cleanup_graph_constant(fixture, 0u, 0u, "seed");
    XrCoreIrInstructionInput *owner =
        xr_program_cleanup_graph_emit(fixture, 0u, XR_CORE_OP_CORE_AGGREGATE_CONSTRUCT, &seed, 1u);
    owner->result = xr_program_cleanup_graph_key("owner", 0u);
    owner->result_type_id = XR_CLEANUP_GRAPH_AFFINE_TYPE;
    owner->result_ownership = XR_CORE_IR_OWNER;
    fixture->live = owner->result;
    XrCoreIrKey operands[] = {owner->result, owner->result};
    XrCoreIrInstructionInput *yield =
        xr_program_cleanup_graph_emit(fixture, 0u, XR_CORE_OP_CORE_COROUTINE_YIELD, operands, 2u);
    yield->immediate_kind = XR_CORE_IR_IMMEDIATE_U32;
    yield->immediate.u32 = 0u;
    uint32_t targets[] = {XR_CLEANUP_GRAPH_NORMAL, XR_CLEANUP_GRAPH_CANCEL_ADAPTER};
    xr_program_cleanup_graph_edges(fixture, 0u, yield, targets, 2u);
}

/* The owning aggregate passes through every branch exactly once. The cancel
 * body has a real back-edge and a provider-refusal edge into a separate trap
 * graph. Both trap alternatives must publish the original provider failure.
 * Initial step: suspended, no provider events. Successful resume: [71], return.
 * Successful cancel: [72], cancelled. Refusal at either token: same trace,
 * trap 7. Constants make execution bounded; admission must still inspect every
 * explicit branch and allow the same-reason body cycle. */
static bool xr_program_cleanup_graph_fixture_init(XrProgramCleanupGraphFixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    XrFingerprint fingerprint;
    if (!xr_stable_id_from_key("fixture.cleanup-graph.provider", &fixture->requirement.contract_id,
                               &fingerprint) ||
        !xr_stable_id_from_key("fixture.cleanup-graph.poll", &fixture->operation, &fingerprint))
        return false;
    fixture->requirement.operation_ids = &fixture->operation;
    fixture->requirement.operation_count = 1u;
    const int64_t values[] = {11, 0, 1, 71, 72};
    for (uint32_t index = 0u; index < 5u; ++index) {
        fixture->constants[index] = (XrCoreIrConstantInput) {
            .key = xr_program_cleanup_graph_key("constant", index),
            .type_id = index == 1u || index == 2u ? XR_CORE_TYPE_BOOL : XR_CORE_TYPE_I64,
            .kind = index == 1u || index == 2u ? XR_CORE_IR_CONSTANT_BOOL : XR_CORE_IR_CONSTANT_I64,
        };
        if (index == 1u || index == 2u)
            fixture->constants[index].value.boolean = values[index] != 0;
        else
            fixture->constants[index].value.i64 = values[index];
    }
    for (uint32_t block = 0u; block < XR_CLEANUP_GRAPH_BLOCK_COUNT; ++block) {
        fixture->blocks[block].key = xr_program_cleanup_graph_key("block", block);
        fixture->blocks[block].instructions = fixture->instructions[block];
        if (block == 0u)
            continue;
        fixture->arguments[block][0] = (XrCoreIrValueInput) {
            .key = xr_program_cleanup_graph_key("owner", block),
            .type_id = XR_CLEANUP_GRAPH_AFFINE_TYPE,
            .category = XR_CORE_IR_VALUE,
            .ownership = XR_CORE_IR_OWNER,
        };
        fixture->blocks[block].arguments = fixture->arguments[block];
        fixture->blocks[block].argument_count = 1u;
        fixture->argument_keys[block][0] = fixture->arguments[block][0].key;
        xr_program_cleanup_graph_emit(fixture, block, XR_CORE_OP_CORE_BLOCK_ARGUMENT,
                                      fixture->argument_keys[block], 1u);
    }
    xr_program_cleanup_graph_entry(fixture);
    xr_program_cleanup_graph_provider(fixture, XR_CLEANUP_GRAPH_NORMAL, 3u);
    xr_program_cleanup_graph_terminal(fixture, XR_CLEANUP_GRAPH_NORMAL, XR_CORE_OP_CORE_RETURN);
    xr_program_cleanup_graph_branch(fixture, XR_CLEANUP_GRAPH_CANCEL_ADAPTER,
                                    XR_CLEANUP_GRAPH_CANCEL_BRANCH);
    uint32_t cancel_targets[] = {XR_CLEANUP_GRAPH_CANCEL_BODY, XR_CLEANUP_GRAPH_CANCEL_PUBLISH};
    xr_program_cleanup_graph_conditional(fixture, XR_CLEANUP_GRAPH_CANCEL_BRANCH, cancel_targets,
                                         2u);
    xr_program_cleanup_graph_provider(fixture, XR_CLEANUP_GRAPH_CANCEL_BODY, 4u);
    xr_program_cleanup_graph_conditional(fixture, XR_CLEANUP_GRAPH_CANCEL_BODY, cancel_targets, 1u);
    xr_program_cleanup_graph_terminal(fixture, XR_CLEANUP_GRAPH_CANCEL_PUBLISH,
                                      XR_CORE_OP_CORE_CANCEL_PUBLISH);
    xr_program_cleanup_graph_branch(fixture, XR_CLEANUP_GRAPH_TRAP_ADAPTER,
                                    XR_CLEANUP_GRAPH_TRAP_BRANCH);
    uint32_t trap_targets[] = {XR_CLEANUP_GRAPH_TRAP_FIRST, XR_CLEANUP_GRAPH_TRAP_SECOND};
    xr_program_cleanup_graph_conditional(fixture, XR_CLEANUP_GRAPH_TRAP_BRANCH, trap_targets, 2u);
    xr_program_cleanup_graph_terminal(fixture, XR_CLEANUP_GRAPH_TRAP_FIRST, XR_CORE_OP_CORE_TRAP);
    xr_program_cleanup_graph_terminal(fixture, XR_CLEANUP_GRAPH_TRAP_SECOND, XR_CORE_OP_CORE_TRAP);
    fixture->states[0] = (XrCoreIrCoroutineStateInput) {
        .state_id = 0u, .continuation_block = fixture->blocks[XR_CLEANUP_GRAPH_ENTRY].key};
    fixture->states[1] = (XrCoreIrCoroutineStateInput) {
        .state_id = 1u, .continuation_block = fixture->blocks[XR_CLEANUP_GRAPH_NORMAL].key};
    fixture->safepoint = (XrCoreIrCoroutineSafepointInput) {.safepoint_id = 0u,
                                                            .resume_state_id = 1u,
                                                            .live_values = &fixture->live,
                                                            .live_value_count = 1u};
    fixture->functions[0] = (XrCoreIrFunctionInput) {
        .key = xr_program_cleanup_graph_key("function", 0u),
        .result_type_id = XR_CORE_TYPE_VOID,
        .effect_mask = XR_CORE_EFFECT_CALL | XR_CORE_EFFECT_TRAP | XR_CORE_EFFECT_CANCEL |
                       XR_CORE_EFFECT_SUSPEND | XR_CORE_EFFECT_PROVIDER_CALL,
        .capability_mask =
            XR_CORE_CAPABILITY_RUNTIME_COROUTINE_SUSPENSION | XR_CORE_CAPABILITY_PROVIDER_BINDING,
        .entry_block = fixture->blocks[XR_CLEANUP_GRAPH_ENTRY].key,
        .blocks = fixture->blocks,
        .block_count = XR_CLEANUP_GRAPH_BLOCK_COUNT,
        .coroutine_states = fixture->states,
        .coroutine_state_count = 2u,
        .coroutine_safepoints = &fixture->safepoint,
        .coroutine_safepoint_count = 1u,
        .flags = XR_PROGRAM_FUNCTION_ENTRY,
    };
    fixture->module = (XrCoreIrModuleInput) {
        .key = xr_program_cleanup_graph_key("module", 0u),
        .constants = fixture->constants,
        .constant_count = 5u,
        .functions = fixture->functions,
        .function_count = 1u,
    };
    fixture->field_type = XR_CORE_TYPE_I64;
    fixture->type = (XrCoreIrTypeInput) {
        .key = xr_program_cleanup_graph_key("type", 0u),
        .local_id = XR_CLEANUP_GRAPH_AFFINE_TYPE,
        .kind = XR_CORE_IR_TYPE_AGGREGATE,
        .ownership = XR_CORE_IR_TYPE_OWNERSHIP_AFFINE,
        .copy_contract = XR_CORE_IR_COPY_EXPLICIT,
        .field_types = &fixture->field_type,
        .field_count = 1u,
    };
    return true;
}

static XrProgramBuildStatus
xr_program_cleanup_graph_fixture_write_input(const XrProgramCleanupGraphFixture *fixture,
                                             XrProgramArtifact *artifact, char *diagnostic,
                                             size_t diagnostic_size) {
    XrCoreIrKey profile = xr_program_cleanup_graph_key("profile", 0u);
    uint16_t feature = XR_CORE_FEATURE_CORE_BASE;
    XrCoreIrProgramInput input = {
        .semantic_profile_fingerprint = profile.bytes,
        .required_features = &feature,
        .required_feature_count = 1u,
        .types = &fixture->type,
        .type_count = 1u,
        .provider_requirements = &fixture->requirement,
        .provider_requirement_count = 1u,
        .modules = &fixture->module,
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

static inline XrProgramBuildStatus
xr_program_cleanup_graph_fixture_write(XrProgramArtifact *artifact, char *diagnostic,
                                       size_t diagnostic_size) {
    XrProgramCleanupGraphFixture fixture;
    if (!xr_program_cleanup_graph_fixture_init(&fixture))
        return XR_PROGRAM_BUILD_INVALID_INPUT;
    return xr_program_cleanup_graph_fixture_write_input(&fixture, artifact, diagnostic,
                                                        diagnostic_size);
}

#endif  // XR_PROGRAM_CLEANUP_GRAPH_FIXTURE_H
