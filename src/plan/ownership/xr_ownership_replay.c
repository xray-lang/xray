/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_ownership_replay.c - Independent ordered ownership/liveness replay
 */

#include "xr_ownership_replay.h"
#include "xr_ownership_certificate_internal.h"
#include "../semantic/xr_semantic_graph.h"
#include "../semantic/xr_semantic_plan_internal.h"
#include "../../base/xmalloc.h"
#include "../../ir/xi_ops_gen.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct XrOwnershipReplay {
    const XrSemanticPlan *plan;
    const XrSemanticGraph *graph;
    const XrOwnershipCertificate *certificate;
    uint32_t *parent;
    uint8_t *rank;
    uint32_t *owner_by_root;
    uint32_t value_count;
    char *error;
    size_t error_size;
} XrOwnershipReplay;

static bool fail(XrOwnershipReplay *replay, const char *code, const char *detail) {
    if (replay->error && replay->error_size)
        snprintf(replay->error, replay->error_size, "%s: %s", code, detail);
    return false;
}

static bool add_i64_checked(int64_t left, int64_t right, int64_t *out) {
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right))
        return false;
    *out = left + right;
    return true;
}

static bool subtract_i64_checked(int64_t left, int64_t right, int64_t *out) {
    if ((right > 0 && left < INT64_MIN + right) ||
        (right < 0 && left > INT64_MAX + right))
        return false;
    *out = left - right;
    return true;
}

static uint32_t find_root(XrOwnershipReplay *replay, uint32_t value) {
    uint32_t root = value;
    while (replay->parent[root] != root)
        root = replay->parent[root];
    while (replay->parent[value] != value) {
        uint32_t next = replay->parent[value];
        replay->parent[value] = root;
        value = next;
    }
    return root;
}

static void union_values(XrOwnershipReplay *replay, uint32_t left, uint32_t right) {
    uint32_t a = find_root(replay, left);
    uint32_t b = find_root(replay, right);
    if (a == b)
        return;
    if (replay->rank[a] < replay->rank[b]) {
        uint32_t swap = a;
        a = b;
        b = swap;
    }
    replay->parent[b] = a;
    if (replay->rank[a] == replay->rank[b])
        replay->rank[a]++;
}

static bool type_is_root(const XrOwnershipReplay *replay, uint32_t type) {
    return type < replay->plan->type_count &&
           (replay->plan->types[type].flags & XR_SEM_TYPE_OWNERSHIP_ROOT) != 0;
}

static bool operation_has_root_result(const XrOwnershipReplay *replay,
                                      const XrSemanticOperationRecord *operation) {
    return operation && type_is_root(replay, operation->result_type) &&
           xi_generated_op_result_kind(operation->opcode) != XI_GEN_RESULT_VOID &&
           operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_NONE;
}

static bool build_equivalence(XrOwnershipReplay *replay) {
    for (uint32_t i = 0; i < replay->plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &replay->plan->operations[i];
        if (!operation_has_root_result(replay, operation) ||
            operation->result_value >= replay->value_count)
            continue;
        if (operation->ownership_use == XI_GEN_OWN_USE_PASS && operation->opcode != XI_PHI) {
            for (uint16_t a = 0; a < operation->operand_count; a++) {
                uint32_t operand = replay->plan->operands[operation->operand_begin + a].value;
                if (operand < replay->value_count)
                    union_values(replay, operation->result_value, operand);
            }
            continue;
        }
        if (operation->result_alias_operand < 0 ||
            (uint16_t) operation->result_alias_operand >= operation->operand_count)
            continue;
        uint32_t operand =
            replay->plan
                ->operands[operation->operand_begin + (uint16_t) operation->result_alias_operand]
                .value;
        if (operand < replay->value_count)
            union_values(replay, operation->result_value, operand);
    }
    return true;
}

static uint32_t owner_for_value(XrOwnershipReplay *replay, uint32_t value) {
    if (value >= replay->value_count)
        return XR_SEMANTIC_INDEX_NONE;
    return replay->owner_by_root[find_root(replay, value)];
}

static uint32_t definition_for_value(const XrOwnershipReplay *replay, uint32_t function,
                                     uint32_t value) {
    for (uint32_t i = 0; i < replay->plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &replay->plan->operations[i];
        if (operation->function == function && operation->result_value == value &&
            xi_generated_op_result_kind(operation->opcode) != XI_GEN_RESULT_VOID)
            return i;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static bool map_certificate_owners(XrOwnershipReplay *replay) {
    for (uint32_t i = 0; i < replay->certificate->owner_count; i++) {
        const XrOwnershipOwnerRecord *owner = &replay->certificate->owners[i];
        if (owner->origin_value >= replay->value_count)
            return fail(replay, "XR_OWN_3002", "owner origin value is outside the SSA table");
        uint32_t root = find_root(replay, owner->origin_value);
        if (replay->owner_by_root[root] != XR_SEMANTIC_INDEX_NONE)
            return fail(replay, "XR_OWN_3002", "owner equivalence class is duplicated");
        replay->owner_by_root[root] = i;
    }
    for (uint32_t i = 0; i < replay->plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &replay->plan->operations[i];
        if (operation_has_root_result(replay, operation) &&
            owner_for_value(replay, operation->result_value) == XR_SEMANTIC_INDEX_NONE)
            return fail(replay, "XR_OWN_3002",
                        "reference-capable SSA result has no certificate owner");
    }
    return true;
}

static const XrOwnershipEdgeStateRecord *find_block_entry(const XrOwnershipReplay *replay,
                                                          uint32_t owner, uint32_t block) {
    for (uint32_t i = 0; i < replay->certificate->edge_state_count; i++) {
        const XrOwnershipEdgeStateRecord *edge = &replay->certificate->edge_states[i];
        if (edge->owner == owner && edge->block == block)
            return edge;
    }
    return NULL;
}

static const XrOwnershipEdgeStateRecord *find_edge_state(const XrOwnershipReplay *replay,
                                                         uint32_t owner, uint32_t block,
                                                         uint32_t successor) {
    for (uint32_t i = 0; i < replay->certificate->edge_state_count; i++) {
        const XrOwnershipEdgeStateRecord *edge = &replay->certificate->edge_states[i];
        if (edge->owner == owner && edge->block == block && edge->successor == successor)
            return edge;
    }
    return NULL;
}

static bool state_can_be_used(uint8_t state) {
    return state == XR_OWN_OWNED_UNIQUE || state == XR_OWN_OWNED_LOCAL ||
           state == XR_OWN_BORROWED || state == XR_OWN_PUBLISHED_SHARED ||
           state == XR_OWN_FRAME_OWNED || state == XR_OWN_FOREIGN_OWNED ||
           state == XR_OWN_FOREIGN_BORROWED || state == XR_OWN_IMMORTAL;
}

static uint8_t entry_state(const XrOwnershipOwnerRecord *owner, int64_t balance) {
    if (balance > 0)
        return XR_OWN_OWNED_LOCAL;
    if (owner->initial_state == XR_OWN_BORROWED ||
        owner->initial_state == XR_OWN_FOREIGN_BORROWED || owner->initial_state == XR_OWN_IMMORTAL)
        return owner->initial_state;
    return XR_OWN_RELEASED;
}

static bool verify_operand_uses(XrOwnershipReplay *replay, uint32_t owner_index,
                                const XrSemanticOperationRecord *operation, int64_t balance,
                                uint8_t state) {
    for (uint16_t a = 0; a < operation->operand_count; a++) {
        const XrSemanticOperandRecord *operand =
            &replay->plan->operands[operation->operand_begin + a];
        if (owner_for_value(replay, operand->value) != owner_index)
            continue;
        if (!state_can_be_used(state) ||
            (balance <= 0 && state != XR_OWN_BORROWED && state != XR_OWN_FOREIGN_BORROWED &&
             state != XR_OWN_IMMORTAL)) {
            if (replay->error && replay->error_size) {
                const XrOwnershipOwnerRecord *owner = &replay->certificate->owners[owner_index];
                uint32_t definition =
                    definition_for_value(replay, owner->function, owner->origin_value);
                const char *definition_name =
                    definition < replay->plan->operation_count
                        ? xi_generated_op_name(replay->plan->operations[definition].opcode)
                        : "NONE";
                const XrSemanticOperationRecord *definition_record =
                    definition < replay->plan->operation_count
                        ? &replay->plan->operations[definition]
                        : NULL;
                const char *function_name = operation->function < replay->plan->function_count
                                                ? replay->plan->functions[operation->function].name
                                                : "<invalid>";
                snprintf(
                    replay->error, replay->error_size,
                    "XR_OWN_3003: ownership use occurs after move or release "
                    "(func=%s owner=%s initial=%u operand=%u operation=%u opcode=%s "
                    "block=%u line=%u balance=%lld state=%u origin=%u definition=%u "
                    "definition-opcode=%s definition-line=%u definition-immediate=%lld)",
                    function_name, owner->canonical_key, owner->initial_state, operand->value,
                    (uint32_t) (operation - replay->plan->operations),
                    xi_generated_op_name(operation->opcode), operation->block,
                    operation->source_line, (long long) balance, state, owner->origin_value,
                    definition,
                    definition_name, definition_record ? definition_record->source_line : 0,
                    (long long) (definition_record ? definition_record->semantic_immediate : 0));
            }
            return false;
        }
        if ((state == XR_OWN_BORROWED || state == XR_OWN_FOREIGN_BORROWED) &&
            operand->ownership_action == XR_SEM_OPERAND_CONSUME)
            return fail(replay, "XR_OWN_3004", "borrowed value escapes without promotion");
    }
    return true;
}

static bool verify_phi_edge_uses(XrOwnershipReplay *replay, uint32_t owner_index,
                                 const XrSemanticOperationRecord *operation) {
    const XrSemanticBlockRecord *block = &replay->plan->blocks[operation->block];
    for (uint16_t a = 0; a < operation->operand_count; a++) {
        const XrSemanticOperandRecord *operand =
            &replay->plan->operands[operation->operand_begin + a];
        if (owner_for_value(replay, operand->value) != owner_index)
            continue;
        uint32_t predecessor = replay->plan->predecessors[block->predecessor_begin + a];
        const XrOwnershipEdgeStateRecord *edge =
            find_edge_state(replay, owner_index, predecessor, operation->block);
        if (!edge || edge->flags == XR_OWN_EDGE_OUT_OF_SCOPE)
            return fail(replay, "XR_OWN_3003", "PHI use has no in-scope predecessor state");
        int64_t balance = edge->exit_balance;
        for (uint32_t e = 0; e < replay->certificate->event_count; e++) {
            const XrOwnershipEventRecord *event = &replay->certificate->events[e];
            if (event->owner == owner_index && event->block == predecessor &&
                event->successor == operation->block && event->program_point == XR_OWN_POINT_EDGE)
                if (!subtract_i64_checked(balance, event->logical_delta, &balance))
                    return fail(replay, "XR_EXEC_5003",
                                "PHI ownership balance exceeds replay schema");
        }
        uint8_t state = entry_state(&replay->certificate->owners[owner_index], balance);
        if (!state_can_be_used(state) ||
            (balance <= 0 && state != XR_OWN_BORROWED && state != XR_OWN_FOREIGN_BORROWED &&
             state != XR_OWN_IMMORTAL))
            return fail(replay, "XR_OWN_3003", "PHI uses ownership after edge disposition");
    }
    return true;
}

static bool apply_events_at_point(XrOwnershipReplay *replay, uint32_t owner_index,
                                  uint32_t operation, uint32_t block, uint8_t program_point,
                                  int64_t *balance, uint8_t *state) {
    const XrOwnershipOwnerRecord *owner = &replay->certificate->owners[owner_index];
    for (uint32_t e = 0; e < replay->certificate->event_count; e++) {
        const XrOwnershipEventRecord *event = &replay->certificate->events[e];
        if (event->owner != owner_index ||
            (operation != XR_SEMANTIC_INDEX_NONE && event->operation != operation) ||
            event->block != block || event->program_point != program_point)
            continue;
        if (!add_i64_checked(*balance, event->logical_delta, balance))
            return fail(replay, "XR_EXEC_5003",
                        "ordered ownership balance exceeds replay schema");
        if (*balance < 0)
            return fail(replay, "XR_OWN_3003", "ordered ownership balance becomes negative");
        if (event->kind != XR_OWN_EVENT_MOVE || event->logical_delta != 0)
            *state = *balance > 0 ? XR_OWN_OWNED_LOCAL : entry_state(owner, *balance);
    }
    return true;
}

static bool verify_control_use(XrOwnershipReplay *replay, uint32_t owner_index,
                               const XrSemanticBlockRecord *block, int64_t balance, uint8_t state) {
    if (block->control_value == XR_SEMANTIC_INDEX_NONE || block->kind == XI_BLOCK_UNREACHABLE ||
        owner_for_value(replay, block->control_value) != owner_index)
        return true;
    if (!state_can_be_used(state) ||
        (balance <= 0 && state != XR_OWN_BORROWED && state != XR_OWN_FOREIGN_BORROWED &&
         state != XR_OWN_IMMORTAL)) {
        if (replay->error && replay->error_size)
            snprintf(replay->error, replay->error_size,
                     "XR_OWN_3003: terminator uses ownership after disposition "
                     "(owner=%s block=%u balance=%lld state=%u)",
                     replay->certificate->owners[owner_index].canonical_key,
                     (uint32_t) (block - replay->plan->blocks), (long long) balance, state);
        return false;
    }
    return true;
}

static bool verify_edge_exit(XrOwnershipReplay *replay, uint32_t owner_index,
                             const XrOwnershipEdgeStateRecord *edge, int64_t block_balance) {
    int64_t balance = block_balance;
    for (uint32_t e = 0; e < replay->certificate->event_count; e++) {
        const XrOwnershipEventRecord *event = &replay->certificate->events[e];
        if (event->owner == owner_index && event->block == edge->block &&
            event->successor == edge->successor && event->program_point == XR_OWN_POINT_EDGE)
            if (!add_i64_checked(balance, event->logical_delta, &balance))
                return fail(replay, "XR_EXEC_5003",
                            "ownership edge replay balance exceeds schema");
    }
    if (balance != edge->exit_balance)
        return fail(replay, "XR_OWN_3003",
                    "ordered ownership replay disagrees with edge disposition");
    return true;
}

static bool replay_owner_block(XrOwnershipReplay *replay, uint32_t owner_index,
                               uint32_t block_index) {
    const XrOwnershipOwnerRecord *owner = &replay->certificate->owners[owner_index];
    const XrSemanticBlockRecord *block = &replay->plan->blocks[block_index];
    const XrOwnershipEdgeStateRecord *entry = find_block_entry(replay, owner_index, block_index);
    if (!entry)
        return fail(replay, "XR_OWN_3002", "owner has no block edge-state record");
    if (entry->flags == XR_OWN_EDGE_OUT_OF_SCOPE)
        return true;
    int64_t balance = entry->entry_balance;
    uint8_t state = entry_state(owner, balance);
    for (uint32_t i = 0; i < block->operation_count; i++) {
        uint32_t operation_index = block->operation_begin + i;
        const XrSemanticOperationRecord *operation = &replay->plan->operations[operation_index];
        bool uses_valid = operation->opcode == XI_PHI
                              ? verify_phi_edge_uses(replay, owner_index, operation)
                              : verify_operand_uses(replay, owner_index, operation, balance, state);
        if (!uses_valid || !apply_events_at_point(replay, owner_index, operation_index, block_index,
                                                  XR_OWN_POINT_AFTER_OPERATION, &balance, &state))
            return false;
    }
    if (!verify_control_use(replay, owner_index, block, balance, state))
        return false;
    if (!apply_events_at_point(replay, owner_index, XR_SEMANTIC_INDEX_NONE, block_index,
                               XR_OWN_POINT_BLOCK_EXIT, &balance, &state))
        return false;
    for (uint32_t i = 0; i < replay->certificate->edge_state_count; i++) {
        const XrOwnershipEdgeStateRecord *edge = &replay->certificate->edge_states[i];
        if (edge->owner == owner_index && edge->block == block_index &&
            !verify_edge_exit(replay, owner_index, edge, balance))
            return false;
    }
    return true;
}

static bool replay_ordered_liveness(XrOwnershipReplay *replay) {
    for (uint32_t owner = 0; owner < replay->certificate->owner_count; owner++) {
        uint32_t function = replay->certificate->owners[owner].function;
        if (function >= replay->plan->function_count)
            return fail(replay, "XR_OWN_3002", "owner function is outside the plan");
        const XrSemanticFunctionRecord *record = &replay->plan->functions[function];
        for (uint32_t e = 0; e < replay->certificate->edge_state_count; e++) {
            const XrOwnershipEdgeStateRecord *edge = &replay->certificate->edge_states[e];
            if (edge->owner != owner || edge->flags != XR_OWN_EDGE_OWNER_FRONTIER)
                continue;
            const XrOwnershipEdgeStateRecord *entry =
                find_block_entry(replay, owner, edge->successor);
            if (!verify_edge_exit(replay, owner, edge, 0) || !entry ||
                entry->flags == XR_OWN_EDGE_OUT_OF_SCOPE ||
                entry->entry_balance != edge->exit_balance)
                return fail(replay, "XR_OWN_3003",
                            "PHI owner frontier disagrees with its destination entry");
        }
        for (uint32_t local = 0; local < record->block_count; local++) {
            uint32_t block = record->block_begin + local;
            if (!replay_owner_block(replay, owner, block))
                return false;
        }
    }
    return true;
}

static bool verify_single_disposition_postdominates(XrOwnershipReplay *replay,
                                                    uint32_t owner_index) {
    const XrOwnershipOwnerRecord *owner = &replay->certificate->owners[owner_index];
    if (owner->initial_state == XR_OWN_BORROWED ||
        owner->initial_state == XR_OWN_FOREIGN_BORROWED || owner->initial_state == XR_OWN_IMMORTAL)
        return true;
    uint32_t close_block = XR_SEMANTIC_INDEX_NONE;
    uint32_t close_count = 0;
    for (uint32_t e = 0; e < replay->certificate->event_count; e++) {
        const XrOwnershipEventRecord *event = &replay->certificate->events[e];
        if (event->owner != owner_index || event->logical_delta >= 0)
            continue;
        close_count++;
        close_block = event->block;
    }
    if (close_count != 1)
        return true;
    uint32_t origin_block = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < replay->plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &replay->plan->operations[i];
        if (operation->function == owner->function &&
            operation->result_value == owner->origin_value) {
            origin_block = operation->block;
            break;
        }
    }
    if (origin_block == XR_SEMANTIC_INDEX_NONE ||
        !xr_semantic_graph_postdominates(replay->graph, close_block, origin_block))
        return fail(replay, "XR_OWN_3005",
                    "single ownership disposition does not post-dominate its origin");
    return true;
}

bool xr_ownership_replay_check(const XrSemanticPlan *plan, const XrSemanticGraph *graph,
                               char *error, size_t error_size) {
    if (!plan || !graph || !plan->ownership)
        return false;
    XrOwnershipReplay replay = {.plan = plan,
                                .graph = graph,
                                .certificate = plan->ownership,
                                .error = error,
                                .error_size = error_size};
    for (uint32_t f = 0; f < plan->function_count; f++) {
        uint64_t end = (uint64_t) plan->functions[f].value_begin + plan->functions[f].value_count;
        if (end > UINT32_MAX)
            return fail(&replay, "XR_EXEC_5003", "ownership SSA budget overflow");
        if (end > replay.value_count)
            replay.value_count = (uint32_t) end;
    }
    replay.parent = (uint32_t *) xr_malloc((size_t) replay.value_count * sizeof(*replay.parent));
    replay.rank = (uint8_t *) xr_calloc(replay.value_count, sizeof(*replay.rank));
    replay.owner_by_root =
        (uint32_t *) xr_malloc((size_t) replay.value_count * sizeof(*replay.owner_by_root));
    if (replay.value_count && (!replay.parent || !replay.rank || !replay.owner_by_root)) {
        fail(&replay, "XR_EXEC_5003", "ownership replay allocation budget exhausted");
        goto failure;
    }
    for (uint32_t i = 0; i < replay.value_count; i++) {
        replay.parent[i] = i;
        replay.owner_by_root[i] = XR_SEMANTIC_INDEX_NONE;
    }
    bool valid = build_equivalence(&replay) && map_certificate_owners(&replay) &&
                 replay_ordered_liveness(&replay);
    for (uint32_t i = 0; valid && i < replay.certificate->owner_count; i++)
        valid = verify_single_disposition_postdominates(&replay, i);
    xr_free(replay.parent);
    xr_free(replay.rank);
    xr_free(replay.owner_by_root);
    return valid;

failure:
    xr_free(replay.parent);
    xr_free(replay.rank);
    xr_free(replay.owner_by_root);
    return false;
}
