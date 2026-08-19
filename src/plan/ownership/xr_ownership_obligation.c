/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_ownership_obligation.c - Semantic ownership obligation construction
 */

#include "xr_ownership_obligation.h"
#include "xr_ownership_owner_shape.h"
#include "xr_ownership_certificate_internal.h"
#include "../semantic/xr_semantic_graph.h"
#include "../semantic/xr_semantic_plan_internal.h"
#include "../../base/xmalloc.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../shared/xr_param_mode.h"
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define XR_OWNERSHIP_MAX_OWNERS UINT32_C(2000000)
#define XR_OWNERSHIP_MAX_EVENTS UINT32_C(20000000)
#define XR_OWNERSHIP_MAX_EDGE_STATES UINT32_C(40000000)
#define XR_OWNERSHIP_MAX_LOOP_INVARIANTS UINT32_C(40000000)

typedef struct XrOwnershipBuildContext {
    XrSemanticPlan *plan;
    XrOwnershipCertificate *certificate;
    uint32_t *parent;
    uint8_t *rank;
    uint32_t *owner_by_root;
    uint8_t *loan_status;
    int16_t *loan_parameter;
    uint32_t value_count;
    char *error;
    size_t error_size;
} XrOwnershipBuildContext;

static bool add_i32_checked(int32_t left, int32_t right, int32_t *out);
static bool add_i32_three_checked(int32_t first, int32_t second, int32_t third, int32_t *out);
static bool subtract_i32_checked(int32_t left, int32_t right, int32_t *out);

static bool fail(XrOwnershipBuildContext *ctx, const char *code, const char *detail) {
    if (ctx->error && ctx->error_size)
        snprintf(ctx->error, ctx->error_size, "%s: %s", code, detail);
    return false;
}

static bool reserve_array(void **items, uint32_t *capacity, uint32_t required, size_t item_size,
                          uint32_t limit) {
    if (required > limit)
        return false;
    if (required <= *capacity)
        return true;
    uint32_t next = *capacity ? *capacity : 16;
    while (next < required) {
        if (next > limit / 2) {
            next = limit;
            break;
        }
        next *= 2;
    }
    void *grown = xr_realloc(*items, (size_t) next * item_size);
    if (!grown)
        return false;
    *items = grown;
    *capacity = next;
    return true;
}

static uint32_t find_root(XrOwnershipBuildContext *ctx, uint32_t value) {
    uint32_t root = value;
    while (ctx->parent[root] != root)
        root = ctx->parent[root];
    while (ctx->parent[value] != value) {
        uint32_t next = ctx->parent[value];
        ctx->parent[value] = root;
        value = next;
    }
    return root;
}

static void union_values(XrOwnershipBuildContext *ctx, uint32_t left, uint32_t right) {
    uint32_t a = find_root(ctx, left);
    uint32_t b = find_root(ctx, right);
    if (a == b)
        return;
    if (ctx->rank[a] < ctx->rank[b]) {
        uint32_t swap = a;
        a = b;
        b = swap;
    }
    ctx->parent[b] = a;
    if (ctx->rank[a] == ctx->rank[b])
        ctx->rank[a]++;
}

static uint32_t aliased_call_operand(const XrSemanticPlan *plan,
                                     const XrSemanticOperationRecord *operation) {
    if (operation->result_alias_operand < 0 ||
        (uint16_t) operation->result_alias_operand >= operation->operand_count)
        return XR_SEMANTIC_INDEX_NONE;
    return plan->operands[operation->operand_begin + (uint16_t) operation->result_alias_operand]
        .value;
}

static bool build_equivalence_classes(XrOwnershipBuildContext *ctx) {
    for (uint32_t i = 0; i < ctx->plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &ctx->plan->operations[i];
        if (!xr_ownership_operation_has_owner(ctx->plan, operation) ||
            operation->result_value >= ctx->value_count)
            continue;
        if (operation->ownership_use == XI_GEN_OWN_USE_PASS && operation->opcode != XI_PHI) {
            for (uint16_t a = 0; a < operation->operand_count; a++) {
                uint32_t operand = ctx->plan->operands[operation->operand_begin + a].value;
                if (operand < ctx->value_count)
                    union_values(ctx, operation->result_value, operand);
            }
        } else if (operation->result_alias_operand >= 0 && operation->operand_count > 0) {
            uint32_t operand = xr_ownership_alias_class_operand(ctx->plan, operation);
            if (operand < ctx->value_count)
                union_values(ctx, operation->result_value, operand);
        } else if (operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT) {
            uint32_t operand = aliased_call_operand(ctx->plan, operation);
            if (operand < ctx->value_count)
                union_values(ctx, operation->result_value, operand);
        }
    }
    return true;
}

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = (char *) xr_malloc(size);
    if (copy)
        memcpy(copy, text, size);
    return copy;
}

static bool add_owner(XrOwnershipBuildContext *ctx, uint32_t root,
                      const XrSemanticOperationRecord *origin, uint32_t *out) {
    XrOwnershipCertificate *certificate = ctx->certificate;
    if (!reserve_array((void **) &certificate->owners, &certificate->owner_capacity,
                       certificate->owner_count + 1, sizeof(*certificate->owners),
                       XR_OWNERSHIP_MAX_OWNERS))
        return fail(ctx, "XR_EXEC_5003", "ownership owner budget exhausted");
    XrOwnershipOwnerRecord *owner = &certificate->owners[certificate->owner_count];
    memset(owner, 0, sizeof(*owner));
    char key[256];
    char operation_id[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(origin->id, operation_id);
    int written = snprintf(key, sizeof(key), "owner-v2:%s:value=%u", operation_id, root);
    if (written < 0 || (size_t) written >= sizeof(key))
        return fail(ctx, "XR_EXEC_5003", "ownership key exceeds its hard bound");
    owner->canonical_key = copy_text(key);
    XrFingerprint digest;
    if (!owner->canonical_key || !xr_stable_id_from_key(owner->canonical_key, &owner->id, &digest))
        return fail(ctx, "XR_EXEC_5003", "ownership identity allocation failed");
    owner->function = origin->function;
    owner->origin_value = origin->result_value;
    owner->initial_state = XR_OWN_UNINITIALIZED;
    owner->exit_state = XR_OWN_RELEASED;
    owner->return_provenance = XR_SEM_RETURN_NONE;
    uint32_t index = certificate->owner_count++;
    ctx->owner_by_root[root] = index;
    *out = index;
    return true;
}

static bool add_event_at(XrOwnershipBuildContext *ctx, uint32_t owner_index,
                         uint32_t operation_index, uint32_t block, uint32_t successor,
                         XrOwnershipEventKind kind, int16_t delta, XrOwnershipState state,
                         XrOwnershipProgramPoint program_point) {
    XrOwnershipCertificate *certificate = ctx->certificate;
    if (!reserve_array((void **) &certificate->events, &certificate->event_capacity,
                       certificate->event_count + 1, sizeof(*certificate->events),
                       XR_OWNERSHIP_MAX_EVENTS))
        return fail(ctx, "XR_EXEC_5003", "ownership event budget exhausted");
    XrOwnershipEventRecord *event = &certificate->events[certificate->event_count];
    memset(event, 0, sizeof(*event));
    event->owner = owner_index;
    event->operation = operation_index;
    event->block = block;
    event->successor = successor;
    event->logical_delta = delta;
    event->kind = (uint8_t) kind;
    event->state_after = (uint8_t) state;
    event->program_point = (uint8_t) program_point;
    char owner_id[XR_STABLE_ID_BYTES * 2 + 1];
    char operation_id[XR_STABLE_ID_BYTES * 2 + 1];
    char key[224];
    uint32_t ordinal = certificate->event_count;
    xr_stable_id_hex(certificate->owners[owner_index].id, owner_id);
    xr_stable_id_hex(ctx->plan->operations[operation_index].id, operation_id);
    int written = snprintf(key, sizeof(key), "ownership-event-v4:%s:%s:%u:%u:%u:%u:%u", owner_id,
                           operation_id, block, successor, (unsigned) kind,
                           (unsigned) program_point, ordinal);
    if (written < 0 || (size_t) written >= sizeof(key))
        return fail(ctx, "XR_EXEC_5003", "ownership event identity failed");
    event->canonical_key = copy_text(key);
    XrFingerprint digest;
    if (!event->canonical_key ||
        !xr_stable_id_from_key(event->canonical_key, &event->id, &digest)) {
        xr_free((void *) event->canonical_key);
        event->canonical_key = NULL;
        return fail(ctx, "XR_EXEC_5003", "ownership event identity failed");
    }
    certificate->event_count++;
    return true;
}

static bool add_event_at_block_exit(XrOwnershipBuildContext *ctx, uint32_t owner_index,
                                    uint32_t operation_index, uint32_t block,
                                    XrOwnershipEventKind kind, int16_t delta,
                                    XrOwnershipState state) {
    return add_event_at(ctx, owner_index, operation_index, block, XR_SEMANTIC_INDEX_NONE, kind,
                        delta, state, XR_OWN_POINT_BLOCK_EXIT);
}

static bool add_event_on_edge(XrOwnershipBuildContext *ctx, uint32_t owner_index,
                              uint32_t operation_index, uint32_t predecessor, uint32_t successor,
                              XrOwnershipEventKind kind, int16_t delta, XrOwnershipState state) {
    return add_event_at(ctx, owner_index, operation_index, predecessor, successor, kind, delta,
                        state, XR_OWN_POINT_EDGE);
}

static bool add_event(XrOwnershipBuildContext *ctx, uint32_t owner_index, uint32_t operation_index,
                      XrOwnershipEventKind kind, int16_t delta, XrOwnershipState state) {
    return add_event_at(ctx, owner_index, operation_index,
                        ctx->plan->operations[operation_index].block, XR_SEMANTIC_INDEX_NONE, kind,
                        delta, state, XR_OWN_POINT_AFTER_OPERATION);
}

static uint32_t owner_for_value(XrOwnershipBuildContext *ctx, uint32_t value) {
    if (value >= ctx->value_count)
        return XR_SEMANTIC_INDEX_NONE;
    return ctx->owner_by_root[find_root(ctx, value)];
}

static bool ensure_owners(XrOwnershipBuildContext *ctx) {
    /* Block storage order is not execution or dominance order.  An alias block
     * may therefore precede the operation which creates its owner, as happens
     * after CFG splitting.  Bind each equivalence class to its real ownership
     * definition first; choosing the first alias would seed dataflow at a use
     * and turn a valid release into a negative balance. */
    for (uint32_t i = 0; i < ctx->plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &ctx->plan->operations[i];
        if (!xr_ownership_operation_defines_owner(ctx->plan, operation) ||
            operation->result_value >= ctx->value_count)
            continue;
        uint32_t root = find_root(ctx, operation->result_value);
        if (ctx->owner_by_root[root] != XR_SEMANTIC_INDEX_NONE)
            continue;
        uint32_t ignored;
        if (!add_owner(ctx, root, operation, &ignored))
            return false;
    }
    for (uint32_t i = 0; i < ctx->plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &ctx->plan->operations[i];
        if (!xr_ownership_operation_has_owner(ctx->plan, operation) ||
            operation->result_value >= ctx->value_count)
            continue;
        uint32_t root = find_root(ctx, operation->result_value);
        if (ctx->owner_by_root[root] == XR_SEMANTIC_INDEX_NONE)
            return fail(ctx, "XR_OWN_3002",
                        "owner equivalence class has no ownership-defining operation");
    }
    return true;
}

static bool classify_definition(XrOwnershipBuildContext *ctx, uint32_t operation_index) {
    const XrSemanticOperationRecord *operation = &ctx->plan->operations[operation_index];
    if (!xr_ownership_operation_has_owner(ctx->plan, operation))
        return true;
    uint32_t owner_index = owner_for_value(ctx, operation->result_value);
    if (owner_index == XR_SEMANTIC_INDEX_NONE)
        return fail(ctx, "XR_OWN_3002", "reference value has no owner equivalence class");
    XrOwnershipOwnerRecord *owner = &ctx->certificate->owners[owner_index];
    if (operation->opcode == XI_PARAM) {
        if (operation->parameter_ownership == XI_OWN_OWNED) {
            owner->initial_state = XR_OWN_OWNED_LOCAL;
            return add_event(ctx, owner_index, operation_index, XR_OWN_EVENT_ALLOC, 1,
                             XR_OWN_OWNED_LOCAL);
        }
        if (operation->parameter_ownership != XI_OWN_BORROWED)
            return fail(ctx, "XR_OWN_3000",
                        "reference parameter has no effective ownership contract");
        if (owner->initial_state == XR_OWN_UNINITIALIZED)
            owner->initial_state = XR_OWN_BORROWED;
        return add_event(ctx, owner_index, operation_index, XR_OWN_EVENT_BORROW, 0,
                         XR_OWN_BORROWED);
    }
    if (operation->opcode == XI_CONST) {
        owner->initial_state = XR_OWN_IMMORTAL;
        owner->exit_state = XR_OWN_IMMORTAL;
        return true;
    }
    if (operation->opcode == XI_STACK_ALLOC) {
        owner->initial_state = XR_OWN_OWNED_UNIQUE;
        return add_event(ctx, owner_index, operation_index, XR_OWN_EVENT_ALLOC, 1,
                         XR_OWN_OWNED_UNIQUE);
    }
    if (operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
        operation->result_alias_operand < 0) {
        if (operation->opcode == XI_GET_SHARED || operation->opcode == XI_GET_GLOBAL ||
            operation->opcode == XI_GET_BUILTIN || operation->opcode == XI_IMPORT_REF) {
            owner->initial_state = XR_OWN_IMMORTAL;
            owner->exit_state = XR_OWN_IMMORTAL;
            return true;
        }
        if (owner->initial_state == XR_OWN_UNINITIALIZED)
            owner->initial_state = XR_OWN_BORROWED;
        return add_event(ctx, owner_index, operation_index, XR_OWN_EVENT_BORROW, 0,
                         XR_OWN_BORROWED);
    }
    if (operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT &&
        operation->return_provenance != XI_RETURN_OWNERSHIP_OWNED &&
        operation->result_alias_operand < 0) {
        /* An unresolved call result is usable at +0 but cannot be dropped as
         * an owned local. ARC preserves exactly that contract by retaining it
         * before every consuming use. Model the incoming token as a foreign
         * borrow so ordered replay accepts intervening reads while still
         * rejecting a consume that lacks the explicit promotion. */
        if (owner->initial_state == XR_OWN_UNINITIALIZED)
            owner->initial_state = XR_OWN_FOREIGN_BORROWED;
        return add_event(ctx, owner_index, operation_index, XR_OWN_EVENT_BORROW, 0,
                         XR_OWN_FOREIGN_BORROWED);
    }
    if (operation->ownership_use == XI_GEN_OWN_USE_PASS || operation->opcode == XI_RETAIN ||
        operation->opcode == XI_RELEASE || operation->result_alias_operand >= 0)
        return true;
    if (operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED &&
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT)
        return true;
    if (owner->initial_state == XR_OWN_UNINITIALIZED)
        owner->initial_state = XR_OWN_OWNED_LOCAL;
    return add_event(ctx, owner_index, operation_index, XR_OWN_EVENT_ALLOC, 1, XR_OWN_OWNED_LOCAL);
}

static uint32_t operation_for_value(const XrSemanticPlan *plan, uint32_t function, uint32_t value) {
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (plan->operations[i].function == function && plan->operations[i].result_value == value &&
            xi_generated_op_result_kind(plan->operations[i].opcode) != XI_GEN_RESULT_VOID)
            return i;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

enum {
    XR_OWN_LOAN_STATIC = -1,
    XR_OWN_LOAN_MULTIPLE = -2,
    XR_OWN_LOAN_CYCLE = -3,
};

static bool owner_has_function_loan(XrOwnershipBuildContext *ctx, uint32_t owner_index,
                                    uint32_t function, int16_t *parameter) {
    *parameter = -1;
    if (owner_index == XR_SEMANTIC_INDEX_NONE)
        return true;
    const XrOwnershipOwnerRecord *owner = &ctx->certificate->owners[owner_index];
    if (owner->function != function)
        return false;
    if (ctx->loan_status[owner_index] == 2) {
        *parameter = ctx->loan_parameter[owner_index];
        return true;
    }
    if (ctx->loan_status[owner_index] == 3)
        return false;
    if (ctx->loan_status[owner_index] == 1) {
        *parameter = XR_OWN_LOAN_CYCLE;
        return true;
    }
    ctx->loan_status[owner_index] = 1;
    uint32_t origin = operation_for_value(ctx->plan, function, owner->origin_value);
    if (origin == XR_SEMANTIC_INDEX_NONE) {
        ctx->loan_status[owner_index] = 3;
        return false;
    }
    const XrSemanticOperationRecord *operation = &ctx->plan->operations[origin];
    int16_t derived = XR_OWN_LOAN_STATIC;
    bool valid = false;
    if (owner->initial_state == XR_OWN_IMMORTAL ||
        operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC) {
        valid = true;
    } else if (operation->opcode == XI_PARAM && operation->parameter_ownership == XI_OWN_BORROWED &&
               operation->semantic_immediate >= 0 && operation->semantic_immediate <= INT16_MAX) {
        derived = (int16_t) operation->semantic_immediate;
        valid = true;
    } else if (operation->opcode == XI_PHI) {
        bool saw_seed = false;
        for (uint16_t a = 0; a < operation->operand_count; a++) {
            uint32_t source =
                owner_for_value(ctx, ctx->plan->operands[operation->operand_begin + a].value);
            if (source == owner_index)
                continue;
            int16_t candidate = XR_OWN_LOAN_STATIC;
            if (!owner_has_function_loan(ctx, source, function, &candidate)) {
                valid = false;
                saw_seed = true;
                break;
            }
            if (candidate == XR_OWN_LOAN_CYCLE)
                continue;
            saw_seed = true;
            valid = true;
            if (candidate >= 0) {
                if (derived == XR_OWN_LOAN_STATIC)
                    derived = candidate;
                else if (derived != candidate)
                    derived = XR_OWN_LOAN_MULTIPLE;
            } else if (candidate == XR_OWN_LOAN_MULTIPLE) {
                derived = XR_OWN_LOAN_MULTIPLE;
            }
        }
        valid = valid && saw_seed;
    } else if ((owner->initial_state == XR_OWN_BORROWED ||
                owner->initial_state == XR_OWN_FOREIGN_BORROWED) &&
               operation->result_alias_operand >= 0 &&
               (uint16_t) operation->result_alias_operand < operation->operand_count) {
        uint32_t source = owner_for_value(
            ctx,
            ctx->plan
                ->operands[operation->operand_begin + (uint16_t) operation->result_alias_operand]
                .value);
        if (source != owner_index)
            valid = owner_has_function_loan(ctx, source, function, &derived);
    }
    ctx->loan_status[owner_index] = valid ? 2 : 3;
    ctx->loan_parameter[owner_index] = derived;
    *parameter = derived;
    if (!valid)
        return false;
    return true;
}

static bool owner_is_returned(XrOwnershipBuildContext *ctx, uint32_t owner_index) {
    uint32_t function = ctx->certificate->owners[owner_index].function;
    const XrSemanticFunctionRecord *record = &ctx->plan->functions[function];
    for (uint32_t block = record->block_begin; block < record->block_begin + record->block_count;
         block++) {
        if (ctx->plan->blocks[block].kind == XI_BLOCK_RETURN &&
            owner_for_value(ctx, ctx->plan->blocks[block].control_value) == owner_index)
            return true;
    }
    return false;
}

static bool owner_is_retained_in_block(XrOwnershipBuildContext *ctx, uint32_t owner_index,
                                       uint32_t block_index) {
    const XrSemanticBlockRecord *block = &ctx->plan->blocks[block_index];
    for (uint32_t i = 0; i < block->operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            &ctx->plan->operations[block->operation_begin + i];
        if (operation->opcode != XI_RETAIN)
            continue;
        for (uint16_t a = 0; a < operation->operand_count; a++) {
            uint32_t value = ctx->plan->operands[operation->operand_begin + a].value;
            if (owner_for_value(ctx, value) == owner_index)
                return true;
        }
    }
    return false;
}

static bool borrowed_phi_parameter(XrOwnershipBuildContext *ctx, uint32_t owner_index,
                                   int16_t *parameter) {
    *parameter = -1;
    const XrOwnershipOwnerRecord *owner = &ctx->certificate->owners[owner_index];
    uint32_t origin = operation_for_value(ctx->plan, owner->function, owner->origin_value);
    if (origin == XR_SEMANTIC_INDEX_NONE || ctx->plan->operations[origin].opcode != XI_PHI)
        return false;
    const XrSemanticOperationRecord *phi = &ctx->plan->operations[origin];
    bool saw_parameter = false;
    for (uint16_t a = 0; a < phi->operand_count; a++) {
        uint32_t source = owner_for_value(ctx, ctx->plan->operands[phi->operand_begin + a].value);
        if (source == owner_index || source == XR_SEMANTIC_INDEX_NONE)
            continue;
        int16_t candidate = -1;
        if (!owner_has_function_loan(ctx, source, owner->function, &candidate))
            return false;
        if (candidate < 0)
            continue;
        if (saw_parameter && *parameter != candidate)
            return false;
        *parameter = candidate;
        saw_parameter = true;
    }
    return saw_parameter;
}

static bool add_operand_events(XrOwnershipBuildContext *ctx, uint32_t operation_index) {
    const XrSemanticOperationRecord *operation = &ctx->plan->operations[operation_index];
    if (operation->opcode == XI_PHI) {
        if (!xr_ownership_type_is_root(ctx->plan, operation->result_type))
            return true;
        const XrSemanticBlockRecord *block = &ctx->plan->blocks[operation->block];
        if (operation->operand_count != block->predecessor_count)
            return fail(ctx, "XR_OWN_3002", "PHI ownership edge count is invalid");
        uint32_t result_owner = owner_for_value(ctx, operation->result_value);
        if (result_owner == XR_SEMANTIC_INDEX_NONE)
            return fail(ctx, "XR_OWN_3002", "reference PHI has no result owner");
        bool saw_borrowed_source = false;
        bool saw_owned_source = false;
        bool borrowed_sources_are_function_loans = true;
        bool borrowed_sources_promoted = true;
        bool have_unpromoted_source = false;
        uint16_t unpromoted_operand = 0;
        uint32_t unpromoted_value = XR_SEMANTIC_INDEX_NONE;
        uint32_t unpromoted_owner = XR_SEMANTIC_INDEX_NONE;
        uint32_t unpromoted_predecessor = XR_SEMANTIC_INDEX_NONE;
        int16_t unique_parameter = -1;
        for (uint16_t a = 0; a < operation->operand_count; a++) {
            uint32_t source_value = ctx->plan->operands[operation->operand_begin + a].value;
            uint32_t source_owner = owner_for_value(ctx, source_value);
            if (source_owner == result_owner)
                continue;
            XrOwnershipState source_state = XR_OWN_IMMORTAL;
            if (source_owner != XR_SEMANTIC_INDEX_NONE)
                source_state =
                    (XrOwnershipState) ctx->certificate->owners[source_owner].initial_state;
            int16_t parameter = XR_OWN_LOAN_STATIC;
            bool borrowed_source =
                source_state == XR_OWN_BORROWED || source_state == XR_OWN_FOREIGN_BORROWED;
            bool function_loan =
                borrowed_source &&
                owner_has_function_loan(ctx, source_owner, operation->function, &parameter);
            if (borrowed_source) {
                saw_borrowed_source = true;
                if (!function_loan)
                    borrowed_sources_are_function_loans = false;
                uint32_t predecessor = ctx->plan->predecessors[block->predecessor_begin + a];
                if (!owner_is_retained_in_block(ctx, source_owner, predecessor)) {
                    borrowed_sources_promoted = false;
                    if (!have_unpromoted_source) {
                        have_unpromoted_source = true;
                        unpromoted_operand = a;
                        unpromoted_value = source_value;
                        unpromoted_owner = source_owner;
                        unpromoted_predecessor = predecessor;
                    }
                }
                if (function_loan && parameter == XR_OWN_LOAN_MULTIPLE &&
                    owner_is_returned(ctx, result_owner))
                    return fail(ctx, "XR_OWN_3004",
                                "returned borrowed PHI has multiple parameter loan roots");
                if (function_loan && parameter >= 0 && unique_parameter < 0)
                    unique_parameter = parameter;
                else if (function_loan && parameter >= 0 && unique_parameter != parameter &&
                         owner_is_returned(ctx, result_owner))
                    return fail(ctx, "XR_OWN_3004",
                                "returned borrowed PHI joins different parameter loans");
            } else if (source_state != XR_OWN_IMMORTAL) {
                saw_owned_source = true;
            }
        }
        bool borrowed_phi =
            saw_borrowed_source && !saw_owned_source && borrowed_sources_are_function_loans;
        if (saw_borrowed_source && !borrowed_phi && !borrowed_sources_promoted) {
            if (ctx->error && ctx->error_size) {
                uint32_t origin_value = XR_SEMANTIC_INDEX_NONE;
                uint32_t origin_operation = XR_SEMANTIC_INDEX_NONE;
                uint8_t source_state = XR_OWN_IMMORTAL;
                uint16_t origin_opcode = 0;
                uint8_t origin_result_ownership = XI_GEN_RESULT_OWNERSHIP_NONE;
                int16_t origin_alias = -1;
                uint8_t origin_return_provenance = XR_SEM_RETURN_NONE;
                int16_t origin_return_parameter = -1;
                if (unpromoted_owner != XR_SEMANTIC_INDEX_NONE) {
                    const XrOwnershipOwnerRecord *source =
                        &ctx->certificate->owners[unpromoted_owner];
                    origin_value = source->origin_value;
                    source_state = source->initial_state;
                    origin_operation =
                        operation_for_value(ctx->plan, operation->function, source->origin_value);
                    if (origin_operation != XR_SEMANTIC_INDEX_NONE) {
                        const XrSemanticOperationRecord *origin =
                            &ctx->plan->operations[origin_operation];
                        origin_opcode = origin->opcode;
                        origin_result_ownership = origin->result_ownership;
                        origin_alias = origin->result_alias_operand;
                        origin_return_provenance = origin->return_provenance;
                        origin_return_parameter = origin->return_parameter;
                    }
                }
                snprintf(ctx->error, ctx->error_size,
                         "XR_OWN_3004: PHI non-function loan has no explicit predecessor "
                         "retain (func=%s op=%u operand=%u value=%u source-owner=%u "
                         "source-state=%u origin-value=%u origin-op=%u origin-opcode=%u "
                         "result-own=%u alias=%d return-prov=%u return-param=%d "
                         "predecessor=%u matching-retain=none)",
                         ctx->plan->functions[operation->function].name, operation_index,
                         unpromoted_operand, unpromoted_value, unpromoted_owner, source_state,
                         origin_value, origin_operation, origin_opcode, origin_result_ownership,
                         origin_alias, origin_return_provenance, origin_return_parameter,
                         unpromoted_predecessor);
            }
            return false;
        }
        ctx->certificate->owners[result_owner].initial_state =
            borrowed_phi ? XR_OWN_BORROWED : XR_OWN_OWNED_LOCAL;
        for (uint16_t a = 0; a < operation->operand_count; a++) {
            uint32_t source_value = ctx->plan->operands[operation->operand_begin + a].value;
            uint32_t source_owner = owner_for_value(ctx, source_value);
            uint32_t predecessor = ctx->plan->predecessors[block->predecessor_begin + a];
            if (borrowed_phi) {
                if (!add_event_on_edge(ctx, result_owner, operation_index, predecessor,
                                       operation->block, XR_OWN_EVENT_BORROW, 0, XR_OWN_BORROWED))
                    return false;
                continue;
            }
            if (source_owner == result_owner)
                continue;
            XrOwnershipState source_state = XR_OWN_IMMORTAL;
            if (source_owner != XR_SEMANTIC_INDEX_NONE)
                source_state =
                    (XrOwnershipState) ctx->certificate->owners[source_owner].initial_state;
            /* A reference-capable PHI may receive an inline/null scalar that
             * has no RC owner class. The result slot still carries one
             * abstract disposition token on that edge: consuming or releasing
             * the nullable result is valid and a runtime no-op for the inline
             * value. Model that source like an immortal token instead of
             * leaving the PHI result uninitialized on the scalar edge. */
            if (source_owner != XR_SEMANTIC_INDEX_NONE && source_state != XR_OWN_IMMORTAL &&
                !add_event_on_edge(ctx, source_owner, operation_index, predecessor,
                                   operation->block, XR_OWN_EVENT_MOVE, -1, XR_OWN_MOVED))
                return false;
            if (!add_event_on_edge(ctx, result_owner, operation_index, predecessor,
                                   operation->block, XR_OWN_EVENT_MOVE, 1, XR_OWN_OWNED_LOCAL))
                return false;
        }
        return true;
    }
    for (uint16_t a = 0; a < operation->operand_count; a++) {
        uint32_t value = ctx->plan->operands[operation->operand_begin + a].value;
        uint32_t owner = owner_for_value(ctx, value);
        if (owner == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (ctx->certificate->owners[owner].initial_state == XR_OWN_IMMORTAL)
            continue;
        if (operation->opcode == XI_RETAIN) {
            if (!add_event(ctx, owner, operation_index, XR_OWN_EVENT_RETAIN, 1, XR_OWN_OWNED_LOCAL))
                return false;
        } else if (operation->opcode == XI_RELEASE) {
            uint32_t producer = operation_for_value(ctx->plan, operation->function, value);
            bool destroys_stack_closure =
                producer != XR_SEMANTIC_INDEX_NONE &&
                ctx->plan->operations[producer].opcode == XI_STACK_ALLOC &&
                ctx->plan->operations[producer].semantic_immediate == XI_CLOSURE_NEW;
            /* A stack closure's physical RELEASE runs its capture destructor;
             * it does not release frame storage.  Preserve that distinction in
             * the logical certificate as an extent DESTROY. */
            if (!add_event(ctx, owner, operation_index,
                           destroys_stack_closure ? XR_OWN_EVENT_DESTROY : XR_OWN_EVENT_RELEASE, -1,
                           XR_OWN_RELEASED))
                return false;
        } else if (operation->opcode == XI_SOURCE_MOVE) {
            if (!add_event(ctx, owner, operation_index, XR_OWN_EVENT_MOVE, 0, XR_OWN_MOVED))
                return false;
        } else {
            bool stored = operation->ownership_use == XI_GEN_OWN_USE_STORED_VALUE && a > 0;
            bool consumed = ctx->plan->operands[operation->operand_begin + a].ownership_action ==
                            XR_SEM_OPERAND_CONSUME;
            /* PASS operations rename or merge the same logical token. Their
             * operands are consume sites for ARC placement, but not ownership
             * dispositions in the post-ARC certificate. */
            if (operation->ownership_use == XI_GEN_OWN_USE_PASS)
                consumed = false;
            if (operation->result_alias_operand == (int16_t) a &&
                owner_for_value(ctx, operation->result_value) == owner)
                consumed = false;
            bool published = stored || operation->opcode == XI_SET_SHARED ||
                             operation->opcode == XI_SET_GLOBAL ||
                             operation->opcode == XI_STORE_UPVAL;
            if (consumed) {
                uint32_t producer = operation_for_value(ctx->plan, operation->function, value);
                bool destroys_scoped_stack_closure =
                    producer != XR_SEMANTIC_INDEX_NONE &&
                    ctx->plan->operations[producer].opcode == XI_STACK_ALLOC &&
                    (operation->opcode == XI_PAR_FOR || operation->opcode == XI_PAR_MAP ||
                     operation->opcode == XI_PAR_REDUCE);
                XrOwnershipEventKind kind = published                       ? XR_OWN_EVENT_PUBLISH
                                            : destroys_scoped_stack_closure ? XR_OWN_EVENT_DESTROY
                                                                            : XR_OWN_EVENT_MOVE;
                XrOwnershipState state = published                       ? XR_OWN_PUBLISHED_SHARED
                                         : destroys_scoped_stack_closure ? XR_OWN_RELEASED
                                                                         : XR_OWN_MOVED;
                if (!add_event(ctx, owner, operation_index, kind, -1, state))
                    return false;
            }
            if (published)
                ctx->certificate->owners[owner].flags |= 1u;
        }
        if ((operation->effects & XI_EFFECT_MAY_SUSPEND) != 0 &&
            !add_event(ctx, owner, operation_index, XR_OWN_EVENT_SUSPEND, 0, XR_OWN_FRAME_OWNED))
            return false;
    }
    return true;
}

static bool classify_returns(XrOwnershipBuildContext *ctx) {
    for (uint32_t f = 0; f < ctx->plan->function_count; f++) {
        XrSemanticFunctionRecord *function = &ctx->plan->functions[f];
        uint8_t inferred_provenance = XR_SEM_RETURN_NONE;
        int16_t inferred_parameter = -1;
        for (uint32_t b = function->block_begin; b < function->block_begin + function->block_count;
             b++) {
            const XrSemanticBlockRecord *block = &ctx->plan->blocks[b];
            if (block->kind != XI_BLOCK_RETURN || block->control_value == XR_SEMANTIC_INDEX_NONE)
                continue;
            uint32_t owner_index = owner_for_value(ctx, block->control_value);
            if (owner_index == XR_SEMANTIC_INDEX_NONE) {
                /* A dynamic/reference-capable ABI may return an inline scalar
                 * such as int or null. It carries no RC owner, but it is a
                 * valid inert token: callers may apply the function's exact
                 * return convention and every retain/release remains a no-op.
                 * A nullable BORROWED_PARAM return must therefore keep that
                 * sealed convention on its null path rather than inventing a
                 * conflicting BORROWED_STATIC provenance. */
                if (xr_ownership_type_is_root(ctx->plan, function->return_type)) {
                    uint8_t inline_provenance = function->return_provenance != XR_SEM_RETURN_NONE
                                                    ? function->return_provenance
                                                    : XR_SEM_RETURN_BORROWED_STATIC;
                    int16_t inline_parameter = inline_provenance == XR_SEM_RETURN_BORROWED_PARAM
                                                   ? function->return_parameter
                                                   : -1;
                    if (inferred_provenance == XR_SEM_RETURN_NONE ||
                        (inferred_provenance == inline_provenance &&
                         inferred_parameter == inline_parameter)) {
                        inferred_provenance = inline_provenance;
                        inferred_parameter = inline_parameter;
                    } else if (!(inferred_provenance == XR_SEM_RETURN_OWNED &&
                                 inline_provenance == XR_SEM_RETURN_BORROWED_STATIC)) {
                        return fail(ctx, "XR_OWN_3000",
                                    "inline return path disagrees with sealed provenance");
                    }
                }
                continue;
            }
            XrOwnershipOwnerRecord *owner = &ctx->certificate->owners[owner_index];
            uint8_t provenance = function->return_provenance;
            int16_t parameter = function->return_parameter;
            int32_t balance = 0;
            for (uint32_t e = 0; e < ctx->certificate->event_count; e++) {
                if (ctx->certificate->events[e].owner == owner_index &&
                    !add_i32_checked(balance, ctx->certificate->events[e].logical_delta, &balance))
                    return fail(ctx, "XR_EXEC_5003", "ownership return balance exceeds schema");
            }
            if (xr_ownership_type_is_root(ctx->plan, function->return_type) &&
                provenance == XR_SEM_RETURN_NONE) {
                if (owner->initial_state == XR_OWN_IMMORTAL) {
                    provenance = XR_SEM_RETURN_BORROWED_STATIC;
                } else if (balance > 0 || owner->initial_state == XR_OWN_OWNED_LOCAL ||
                           owner->initial_state == XR_OWN_OWNED_UNIQUE) {
                    provenance = XR_SEM_RETURN_OWNED;
                } else if (owner->initial_state == XR_OWN_BORROWED) {
                    for (uint32_t o = 0; o < ctx->plan->operation_count; o++) {
                        const XrSemanticOperationRecord *origin = &ctx->plan->operations[o];
                        if (origin->function == f && origin->result_value == owner->origin_value &&
                            origin->opcode == XI_PARAM) {
                            provenance = XR_SEM_RETURN_BORROWED_PARAM;
                            parameter = (int16_t) origin->semantic_immediate;
                            break;
                        }
                    }
                    if (provenance == XR_SEM_RETURN_NONE &&
                        borrowed_phi_parameter(ctx, owner_index, &parameter))
                        provenance = XR_SEM_RETURN_BORROWED_PARAM;
                }
            }
            if (xr_ownership_type_is_root(ctx->plan, function->return_type) &&
                (provenance == XR_SEM_RETURN_NONE || provenance > XR_SEM_RETURN_BORROWED_STATIC)) {
                if (ctx->error && ctx->error_size) {
                    const XrSemanticTypeRecord *return_type =
                        &ctx->plan->types[function->return_type];
                    snprintf(ctx->error, ctx->error_size,
                             "XR_OWN_3000: reference-capable return has unknown provenance "
                             "(func=%s type-kind=%u type-flags=%u owner=%s balance=%d "
                             "initial=%u summary=%u)",
                             function->name, return_type->kind, return_type->flags,
                             owner->canonical_key, balance, owner->initial_state,
                             function->return_provenance);
                }
                return false;
            }
            uint8_t disposition_provenance = provenance;
            bool owned_static_join = (inferred_provenance == XR_SEM_RETURN_OWNED &&
                                      provenance == XR_SEM_RETURN_BORROWED_STATIC) ||
                                     (inferred_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
                                      provenance == XR_SEM_RETURN_OWNED);
            if (owned_static_join) {
                /* Immortal values satisfy an owned return ABI: callers may
                 * release the token and the runtime release is a no-op. This
                 * gives a single exact contract to mixed fresh/static paths. */
                inferred_provenance = XR_SEM_RETURN_OWNED;
                inferred_parameter = -1;
                provenance = XR_SEM_RETURN_OWNED;
                parameter = -1;
            } else if (inferred_provenance != XR_SEM_RETURN_NONE &&
                       (inferred_provenance != provenance || inferred_parameter != parameter)) {
                if (ctx->error && ctx->error_size)
                    snprintf(ctx->error, ctx->error_size,
                             "XR_OWN_3000: reference return paths have inconsistent provenance "
                             "(func=%s block=%u prior-kind=%u prior-param=%d kind=%u param=%d)",
                             function->name, b, inferred_provenance, inferred_parameter, provenance,
                             parameter);
                return false;
            }
            inferred_provenance = provenance;
            inferred_parameter = parameter;
            owner->return_provenance = provenance;
            owner->exit_state = provenance == XR_SEM_RETURN_OWNED ? XR_OWN_MOVED : XR_OWN_BORROWED;
            owner->flags |= 2u;
            if ((disposition_provenance == XR_SEM_RETURN_OWNED &&
                 owner->initial_state != XR_OWN_IMMORTAL) ||
                (disposition_provenance == XR_SEM_RETURN_BORROWED_STATIC && balance > 0)) {
                uint32_t operation = operation_for_value(ctx->plan, f, block->control_value);
                if (operation == XR_SEMANTIC_INDEX_NONE ||
                    !add_event_at_block_exit(
                        ctx, owner_index, operation, b, XR_OWN_EVENT_RETURN, -1,
                        disposition_provenance == XR_SEM_RETURN_OWNED ? XR_OWN_MOVED
                                                                      : XR_OWN_IMMORTAL))
                    return fail(ctx, "XR_OWN_3002",
                                "returned reference has no semantic producer operation");
            }
        }
        if (xr_ownership_type_is_root(ctx->plan, function->return_type)) {
            if ((function->flags & 4u) != 0) {
                if (function->return_provenance != XR_SEM_RETURN_OWNED)
                    return fail(ctx, "XR_OWN_3000",
                                "generator handle has no owned return disposition");
                continue;
            }
            if (inferred_provenance == XR_SEM_RETURN_NONE)
                return fail(ctx, "XR_OWN_3000",
                            "reference-capable function has no proven return disposition");
            function->return_provenance = inferred_provenance;
            function->return_parameter = inferred_parameter;
            if (inferred_provenance == XR_SEM_RETURN_OWNED) {
                for (uint32_t b = function->block_begin;
                     b < function->block_begin + function->block_count; b++) {
                    const XrSemanticBlockRecord *block = &ctx->plan->blocks[b];
                    if (block->kind != XI_BLOCK_RETURN ||
                        block->control_value == XR_SEMANTIC_INDEX_NONE)
                        continue;
                    uint32_t owner_index = owner_for_value(ctx, block->control_value);
                    if (owner_index == XR_SEMANTIC_INDEX_NONE)
                        continue;
                    ctx->certificate->owners[owner_index].return_provenance = XR_SEM_RETURN_OWNED;
                    ctx->certificate->owners[owner_index].exit_state = XR_OWN_MOVED;
                }
            }
        }
    }
    return true;
}

static bool stack_extent_has_destroy(const XrOwnershipBuildContext *ctx, uint32_t owner,
                                     uint32_t block, uint32_t successor) {
    for (uint32_t e = 0; e < ctx->certificate->event_count; e++) {
        const XrOwnershipEventRecord *event = &ctx->certificate->events[e];
        if (event->owner == owner && event->block == block && event->successor == successor &&
            event->kind == XR_OWN_EVENT_DESTROY && event->logical_delta < 0)
            return true;
    }
    return false;
}

/* XI_STACK_ALLOC is physically reclaimed by the function frame, so ARC must
 * not manufacture a runtime release for it.  The ownership certificate still
 * needs an exact logical extent.  Record DESTROY at each reachable terminal
 * while rejecting any attempt to move, publish, return, or otherwise dispose
 * the stack identity through an ordinary RC operation. */
static bool classify_stack_extents(XrOwnershipBuildContext *ctx) {
    bool *processed = (bool *) xr_calloc(ctx->certificate->owner_count, sizeof(*processed));
    if (ctx->certificate->owner_count && !processed)
        return fail(ctx, "XR_EXEC_5003", "stack extent worklist allocation failed");
    for (uint32_t operation_index = 0; operation_index < ctx->plan->operation_count;
         operation_index++) {
        const XrSemanticOperationRecord *operation = &ctx->plan->operations[operation_index];
        if (operation->opcode != XI_STACK_ALLOC)
            continue;
        uint32_t owner = owner_for_value(ctx, operation->result_value);
        if (owner == XR_SEMANTIC_INDEX_NONE || processed[owner]) {
            if (owner == XR_SEMANTIC_INDEX_NONE) {
                xr_free(processed);
                return fail(ctx, "XR_OWN_3002", "stack allocation has no owner identity");
            }
            continue;
        }
        processed[owner] = true;
        for (uint32_t e = 0; e < ctx->certificate->event_count; e++) {
            const XrOwnershipEventRecord *event = &ctx->certificate->events[e];
            if (event->owner != owner || event->logical_delta >= 0)
                continue;
            if (event->kind != XR_OWN_EVENT_DESTROY) {
                xr_free(processed);
                return fail(ctx, "XR_OWN_3001", "stack allocation escapes its proven frame extent");
            }
        }

        uint32_t function_index = operation->function;
        if (function_index >= ctx->plan->function_count) {
            xr_free(processed);
            return fail(ctx, "XR_OWN_3002", "stack allocation function is invalid");
        }
        const XrSemanticFunctionRecord *function = &ctx->plan->functions[function_index];
        if (operation->block < function->block_begin ||
            operation->block >= function->block_begin + function->block_count) {
            xr_free(processed);
            return fail(ctx, "XR_OWN_3002", "stack allocation block crosses its function");
        }
        bool *visited = (bool *) xr_calloc((size_t) function->block_count * 2u, sizeof(*visited));
        bool *terminal_live = (bool *) xr_calloc(function->block_count, sizeof(*terminal_live));
        bool *terminal_dead = (bool *) xr_calloc(function->block_count, sizeof(*terminal_dead));
        uint32_t *queue =
            (uint32_t *) xr_malloc((size_t) function->block_count * 2u * sizeof(*queue));
        if (function->block_count && (!visited || !terminal_live || !terminal_dead || !queue)) {
            xr_free(visited);
            xr_free(terminal_live);
            xr_free(terminal_dead);
            xr_free(queue);
            xr_free(processed);
            return fail(ctx, "XR_EXEC_5003", "stack extent worklist allocation failed");
        }
        uint32_t head = 0, tail = 0;
        uint32_t origin_local = operation->block - function->block_begin;
        queue[tail++] = origin_local * 2u + 1u;
        visited[origin_local * 2u + 1u] = true;
        while (head < tail) {
            uint32_t state = queue[head++];
            uint32_t local = state / 2u;
            bool live = (state & 1u) != 0;
            uint32_t block_index = function->block_begin + local;
            const XrSemanticBlockRecord *block = &ctx->plan->blocks[block_index];
            if (live && stack_extent_has_destroy(ctx, owner, block_index, XR_SEMANTIC_INDEX_NONE))
                live = false;
            bool terminal = block->successors[0] == XR_SEMANTIC_INDEX_NONE &&
                            block->successors[1] == XR_SEMANTIC_INDEX_NONE;
            if (terminal) {
                if (live)
                    terminal_live[local] = true;
                else
                    terminal_dead[local] = true;
                continue;
            }
            for (unsigned s = 0; s < 2; s++) {
                uint32_t successor = block->successors[s];
                if (successor == XR_SEMANTIC_INDEX_NONE ||
                    (s == 1 && successor == block->successors[0]))
                    continue;
                if (successor < function->block_begin ||
                    successor >= function->block_begin + function->block_count) {
                    xr_free(visited);
                    xr_free(terminal_live);
                    xr_free(terminal_dead);
                    xr_free(queue);
                    xr_free(processed);
                    return fail(ctx, "XR_OWN_3002", "stack extent edge crosses its function");
                }
                bool edge_live = live;
                if (edge_live && stack_extent_has_destroy(ctx, owner, block_index, successor))
                    edge_live = false;
                uint32_t next = (successor - function->block_begin) * 2u + (edge_live ? 1u : 0u);
                if (!visited[next]) {
                    visited[next] = true;
                    queue[tail++] = next;
                }
            }
        }
        for (uint32_t local = 0; local < function->block_count; local++) {
            if (terminal_live[local] && terminal_dead[local]) {
                xr_free(visited);
                xr_free(terminal_live);
                xr_free(terminal_dead);
                xr_free(queue);
                xr_free(processed);
                return fail(ctx, "XR_OWN_3001",
                            "stack extent reaches an exit in conflicting states");
            }
            if (terminal_live[local] &&
                !add_event_at_block_exit(ctx, owner, operation_index, function->block_begin + local,
                                         XR_OWN_EVENT_DESTROY, -1, XR_OWN_RELEASED)) {
                xr_free(visited);
                xr_free(terminal_live);
                xr_free(terminal_dead);
                xr_free(queue);
                xr_free(processed);
                return false;
            }
        }
        ctx->certificate->owners[owner].exit_state = XR_OWN_RELEASED;
        xr_free(visited);
        xr_free(terminal_live);
        xr_free(terminal_dead);
        xr_free(queue);
    }
    xr_free(processed);
    return true;
}

static uint32_t terminal_disposition_operation(const XrOwnershipBuildContext *ctx,
                                               uint32_t function, uint32_t block,
                                               uint32_t fallback) {
    const XrSemanticBlockRecord *record = &ctx->plan->blocks[block];
    if (record->control_value != XR_SEMANTIC_INDEX_NONE) {
        uint32_t control = operation_for_value(ctx->plan, function, record->control_value);
        if (control != XR_SEMANTIC_INDEX_NONE)
            return control;
    }
    for (uint32_t operation = 0; operation < ctx->plan->operation_count; operation++) {
        const XrSemanticOperationRecord *candidate = &ctx->plan->operations[operation];
        if (candidate->function == function && candidate->block == block)
            fallback = operation;
    }
    return fallback;
}

/* Close every still-owned logical token at a reachable function exit.  This
 * makes exit disposition explicit even when a diagnostic/check-only pipeline
 * does not run physical ARC insertion.  Target planning later chooses the
 * concrete last-use/edge cleanup that realizes these obligations. */
static bool classify_implicit_exit_dispositions(XrOwnershipBuildContext *ctx) {
    for (uint32_t owner = 0; owner < ctx->certificate->owner_count; owner++) {
        XrOwnershipOwnerRecord *owner_record = &ctx->certificate->owners[owner];
        if (owner_record->initial_state == XR_OWN_IMMORTAL ||
            owner_record->initial_state == XR_OWN_BORROWED ||
            owner_record->initial_state == XR_OWN_FOREIGN_BORROWED)
            continue;
        uint32_t function = owner_record->function;
        if (function >= ctx->plan->function_count)
            return fail(ctx, "XR_OWN_3002", "owner references an invalid function");
        const XrSemanticFunctionRecord *fn = &ctx->plan->functions[function];
        int32_t *entry = (int32_t *) xr_malloc((size_t) fn->block_count * sizeof(*entry));
        int32_t *delta = (int32_t *) xr_calloc(fn->block_count, sizeof(*delta));
        int32_t *edge_delta =
            (int32_t *) xr_calloc((size_t) fn->block_count * 2u, sizeof(*edge_delta));
        uint32_t *queue = (uint32_t *) xr_malloc((size_t) fn->block_count * sizeof(*queue));
        if (fn->block_count && (!entry || !delta || !edge_delta || !queue)) {
            xr_free(entry);
            xr_free(delta);
            xr_free(edge_delta);
            xr_free(queue);
            return fail(ctx, "XR_EXEC_5003", "ownership disposition allocation failed");
        }
        for (uint32_t i = 0; i < fn->block_count; i++)
            entry[i] = INT32_MIN;
        for (uint32_t e = 0; e < ctx->certificate->event_count; e++) {
            const XrOwnershipEventRecord *event = &ctx->certificate->events[e];
            if (event->owner != owner || event->block < fn->block_begin ||
                event->block >= fn->block_begin + fn->block_count)
                continue;
            uint32_t local = event->block - fn->block_begin;
            if (event->successor == XR_SEMANTIC_INDEX_NONE) {
                if (!add_i32_checked(delta[local], event->logical_delta, &delta[local])) {
                    xr_free(entry);
                    xr_free(delta);
                    xr_free(edge_delta);
                    xr_free(queue);
                    return fail(ctx, "XR_EXEC_5003",
                                "ownership disposition balance exceeds schema");
                }
            } else {
                const XrSemanticBlockRecord *block = &ctx->plan->blocks[event->block];
                uint32_t edge_index = UINT32_MAX;
                if (block->successors[0] == event->successor)
                    edge_index = local * 2u;
                else if (block->successors[1] == event->successor)
                    edge_index = local * 2u + 1u;
                if (edge_index == UINT32_MAX) {
                    xr_free(entry);
                    xr_free(delta);
                    xr_free(edge_delta);
                    xr_free(queue);
                    return fail(ctx, "XR_OWN_3002", "ownership event names a non-CFG edge");
                }
                if (!add_i32_checked(edge_delta[edge_index], event->logical_delta,
                                     &edge_delta[edge_index])) {
                    xr_free(entry);
                    xr_free(delta);
                    xr_free(edge_delta);
                    xr_free(queue);
                    return fail(ctx, "XR_EXEC_5003", "ownership edge disposition exceeds schema");
                }
            }
        }
        uint32_t origin_operation =
            operation_for_value(ctx->plan, function, owner_record->origin_value);
        if (origin_operation == XR_SEMANTIC_INDEX_NONE) {
            xr_free(entry);
            xr_free(delta);
            xr_free(edge_delta);
            xr_free(queue);
            return fail(ctx, "XR_OWN_3002", "ownership origin has no value-producing operation");
        }
        uint32_t origin_block = ctx->plan->operations[origin_operation].block;
        if (origin_block < fn->block_begin || origin_block >= fn->block_begin + fn->block_count) {
            xr_free(entry);
            xr_free(delta);
            xr_free(edge_delta);
            xr_free(queue);
            return fail(ctx, "XR_OWN_3002", "ownership origin crosses a function boundary");
        }
        uint32_t head = 0, tail = 0;
        uint32_t origin_local = origin_block - fn->block_begin;
        bool origin_is_phi = ctx->plan->operations[origin_operation].opcode == XI_PHI;
        entry[origin_local] =
            origin_is_phi && owner_record->initial_state != XR_OWN_BORROWED ? 1 : 0;
        queue[tail++] = origin_local;
        while (head < tail) {
            uint32_t local = queue[head++];
            int32_t exit_balance = 0;
            if (!add_i32_checked(entry[local], delta[local], &exit_balance)) {
                xr_free(entry);
                xr_free(delta);
                xr_free(edge_delta);
                xr_free(queue);
                return fail(ctx, "XR_EXEC_5003", "ownership balance exceeds schema");
            }
            if (exit_balance < 0) {
                if (ctx->error && ctx->error_size)
                    snprintf(ctx->error, ctx->error_size,
                             "XR_OWN_3001: logical ownership balance becomes negative "
                             "(owner=%s origin=%s func=%s block=%u entry=%d delta=%d initial=%u)",
                             owner_record->canonical_key,
                             xi_generated_op_name(ctx->plan->operations[origin_operation].opcode),
                             fn->name, fn->block_begin + local, entry[local], delta[local],
                             owner_record->initial_state);
                xr_free(entry);
                xr_free(delta);
                xr_free(edge_delta);
                xr_free(queue);
                return false;
            }
            const XrSemanticBlockRecord *block = &ctx->plan->blocks[fn->block_begin + local];
            for (unsigned s = 0; s < 2; s++) {
                uint32_t successor = block->successors[s];
                if (successor == XR_SEMANTIC_INDEX_NONE ||
                    (s == 1 && successor == block->successors[0]))
                    continue;
                if (successor < fn->block_begin || successor >= fn->block_begin + fn->block_count) {
                    xr_free(entry);
                    xr_free(delta);
                    xr_free(edge_delta);
                    xr_free(queue);
                    return fail(ctx, "XR_OWN_3002", "ownership edge crosses function boundary");
                }
                int32_t incoming = 0;
                if (!add_i32_checked(exit_balance, edge_delta[local * 2u + s], &incoming)) {
                    xr_free(entry);
                    xr_free(delta);
                    xr_free(edge_delta);
                    xr_free(queue);
                    return fail(ctx, "XR_EXEC_5003", "ownership edge balance exceeds schema");
                }
                if (incoming < 0) {
                    if (ctx->error && ctx->error_size)
                        snprintf(ctx->error, ctx->error_size,
                                 "XR_OWN_3001: logical ownership balance becomes negative on "
                                 "an edge (owner=%s func=%s from=%u to=%u balance=%d initial=%u)",
                                 owner_record->canonical_key, fn->name, fn->block_begin + local,
                                 successor, incoming, owner_record->initial_state);
                    xr_free(entry);
                    xr_free(delta);
                    xr_free(edge_delta);
                    xr_free(queue);
                    return false;
                }
                /* Re-entering a non-PHI SSA definition creates a new dynamic
                 * owner instance. Close the previous iteration on the edge;
                 * otherwise its token is incorrectly joined with the next
                 * allocation at the definition block. A PHI is different: it
                 * explicitly models loop-carried identity and must preserve
                 * the incoming balance. */
                if (!origin_is_phi && successor == origin_block && incoming > 0) {
                    if (incoming > INT16_MAX) {
                        xr_free(entry);
                        xr_free(delta);
                        xr_free(edge_delta);
                        xr_free(queue);
                        return fail(ctx, "XR_EXEC_5003",
                                    "ownership redefinition disposition exceeds schema");
                    }
                    uint32_t operation = terminal_disposition_operation(
                        ctx, function, fn->block_begin + local, origin_operation);
                    if (!add_event_on_edge(ctx, owner, operation, fn->block_begin + local,
                                           successor, XR_OWN_EVENT_RELEASE, (int16_t) -incoming,
                                           XR_OWN_RELEASED)) {
                        xr_free(entry);
                        xr_free(delta);
                        xr_free(edge_delta);
                        xr_free(queue);
                        return false;
                    }
                    if (!subtract_i32_checked(edge_delta[local * 2u + s], incoming,
                                              &edge_delta[local * 2u + s])) {
                        xr_free(entry);
                        xr_free(delta);
                        xr_free(edge_delta);
                        xr_free(queue);
                        return fail(ctx, "XR_EXEC_5003",
                                    "ownership redefinition balance exceeds schema");
                    }
                    incoming = 0;
                }
                uint32_t next = successor - fn->block_begin;
                if (entry[next] == INT32_MIN) {
                    entry[next] = incoming;
                    queue[tail++] = next;
                } else if (entry[next] != incoming) {
                    uint32_t phi_source = XR_SEMANTIC_INDEX_NONE;
                    uint32_t phi_source_owner = XR_SEMANTIC_INDEX_NONE;
                    if (origin_is_phi && successor == origin_block) {
                        const XrSemanticOperationRecord *origin =
                            &ctx->plan->operations[origin_operation];
                        const XrSemanticBlockRecord *origin_record =
                            &ctx->plan->blocks[origin_block];
                        for (uint32_t p = 0;
                             p < origin_record->predecessor_count && p < origin->operand_count;
                             p++) {
                            if (ctx->plan->predecessors[origin_record->predecessor_begin + p] !=
                                fn->block_begin + local)
                                continue;
                            phi_source = ctx->plan->operands[origin->operand_begin + p].value;
                            phi_source_owner = owner_for_value(ctx, phi_source);
                            break;
                        }
                    }
                    if (ctx->error && ctx->error_size)
                        snprintf(
                            ctx->error, ctx->error_size,
                            "XR_OWN_3001: ownership balance differs across a CFG join or "
                            "loop (owner=%s origin=%s func=%s from=%u to=%u prior=%d "
                            "incoming=%d exit=%d edge-delta=%d phi-source=%u "
                            "phi-source-owner=%u function-begin=%u from-local=%u "
                            "to-local=%u origin-local=%u)",
                            owner_record->canonical_key,
                            xi_generated_op_name(ctx->plan->operations[origin_operation].opcode),
                            fn->name, fn->block_begin + local, successor, entry[next], incoming,
                            exit_balance, edge_delta[local * 2u + s], phi_source, phi_source_owner,
                            fn->block_begin, local, successor - fn->block_begin, origin_local);
                    xr_free(entry);
                    xr_free(delta);
                    xr_free(edge_delta);
                    xr_free(queue);
                    return false;
                }
            }
        }
        for (uint32_t local = 0; local < fn->block_count; local++) {
            if (entry[local] == INT32_MIN)
                continue;
            uint32_t block_index = fn->block_begin + local;
            const XrSemanticBlockRecord *block = &ctx->plan->blocks[block_index];
            if (block->successors[0] != XR_SEMANTIC_INDEX_NONE ||
                block->successors[1] != XR_SEMANTIC_INDEX_NONE)
                continue;
            int32_t balance = 0;
            if (!add_i32_checked(entry[local], delta[local], &balance)) {
                xr_free(entry);
                xr_free(delta);
                xr_free(edge_delta);
                xr_free(queue);
                return fail(ctx, "XR_EXEC_5003", "ownership exit balance exceeds schema");
            }
            if (balance <= 0)
                continue;
            if (balance > INT16_MAX) {
                xr_free(entry);
                xr_free(delta);
                xr_free(edge_delta);
                xr_free(queue);
                return fail(ctx, "XR_EXEC_5003", "ownership exit disposition exceeds schema");
            }
            uint32_t operation =
                terminal_disposition_operation(ctx, function, block_index, origin_operation);
            if (!add_event_at_block_exit(ctx, owner, operation, block_index, XR_OWN_EVENT_RELEASE,
                                         (int16_t) -balance, XR_OWN_RELEASED)) {
                xr_free(entry);
                xr_free(delta);
                xr_free(edge_delta);
                xr_free(queue);
                return false;
            }
        }
        if ((owner_record->flags & 3u) == 0)
            owner_record->exit_state = XR_OWN_RELEASED;
        xr_free(entry);
        xr_free(delta);
        xr_free(edge_delta);
        xr_free(queue);
    }
    return true;
}

static int compare_edge_state(const void *left, const void *right);
static uint8_t ownership_state_for_balance(const XrOwnershipOwnerRecord *owner, int32_t balance);

static bool build_edge_states(XrOwnershipBuildContext *ctx) {
    for (uint32_t owner = 0; owner < ctx->certificate->owner_count; owner++) {
        uint32_t function = ctx->certificate->owners[owner].function;
        if (function >= ctx->plan->function_count)
            return fail(ctx, "XR_OWN_3002", "owner references an invalid function");
        const XrSemanticFunctionRecord *fn = &ctx->plan->functions[function];
        int32_t *entry = (int32_t *) xr_malloc((size_t) fn->block_count * sizeof(*entry));
        int32_t *delta = (int32_t *) xr_calloc(fn->block_count, sizeof(*delta));
        int32_t *edge_delta =
            (int32_t *) xr_calloc((size_t) fn->block_count * 2u, sizeof(*edge_delta));
        uint32_t *queue = (uint32_t *) xr_malloc((size_t) fn->block_count * sizeof(*queue));
        if (fn->block_count && (!entry || !delta || !edge_delta || !queue)) {
            xr_free(entry);
            xr_free(delta);
            xr_free(edge_delta);
            xr_free(queue);
            return fail(ctx, "XR_EXEC_5003", "ownership dataflow allocation failed");
        }
        for (uint32_t i = 0; i < fn->block_count; i++)
            entry[i] = INT32_MIN;
        for (uint32_t e = 0; e < ctx->certificate->event_count; e++) {
            const XrOwnershipEventRecord *event = &ctx->certificate->events[e];
            if (event->owner != owner || event->block < fn->block_begin ||
                event->block >= fn->block_begin + fn->block_count)
                continue;
            uint32_t local = event->block - fn->block_begin;
            if (event->successor == XR_SEMANTIC_INDEX_NONE) {
                if (!add_i32_checked(delta[local], event->logical_delta, &delta[local])) {
                    xr_free(entry);
                    xr_free(delta);
                    xr_free(edge_delta);
                    xr_free(queue);
                    return fail(ctx, "XR_EXEC_5003", "ownership event balance exceeds schema");
                }
                continue;
            }
            const XrSemanticBlockRecord *event_block = &ctx->plan->blocks[event->block];
            uint32_t edge_index = UINT32_MAX;
            if (event_block->successors[0] == event->successor)
                edge_index = local * 2u;
            else if (event_block->successors[1] == event->successor)
                edge_index = local * 2u + 1u;
            if (edge_index == UINT32_MAX) {
                xr_free(entry);
                xr_free(delta);
                xr_free(edge_delta);
                xr_free(queue);
                return fail(ctx, "XR_OWN_3002", "ownership event names a non-CFG edge");
            }
            if (!add_i32_checked(edge_delta[edge_index], event->logical_delta,
                                 &edge_delta[edge_index])) {
                xr_free(entry);
                xr_free(delta);
                xr_free(edge_delta);
                xr_free(queue);
                return fail(ctx, "XR_EXEC_5003", "ownership edge balance exceeds schema");
            }
        }
        uint32_t origin_operation =
            operation_for_value(ctx->plan, function, ctx->certificate->owners[owner].origin_value);
        if (origin_operation == XR_SEMANTIC_INDEX_NONE) {
            xr_free(entry);
            xr_free(delta);
            xr_free(edge_delta);
            xr_free(queue);
            return fail(ctx, "XR_OWN_3002", "ownership origin has no value-producing operation");
        }
        uint32_t origin_block = ctx->plan->operations[origin_operation].block;
        if (origin_block < fn->block_begin || origin_block >= fn->block_begin + fn->block_count) {
            xr_free(entry);
            xr_free(delta);
            xr_free(edge_delta);
            xr_free(queue);
            return fail(ctx, "XR_OWN_3002", "ownership origin crosses a function boundary");
        }
        uint32_t head = 0, tail = 0;
        uint32_t origin_local = origin_block - fn->block_begin;
        entry[origin_local] =
            ctx->plan->operations[origin_operation].opcode == XI_PHI &&
                    ctx->certificate->owners[owner].initial_state != XR_OWN_BORROWED
                ? 1
                : 0;
        queue[tail++] = origin_local;
        while (head < tail) {
            uint32_t local = queue[head++];
            int32_t exit_balance = 0;
            if (!add_i32_checked(entry[local], delta[local], &exit_balance)) {
                xr_free(entry);
                xr_free(delta);
                xr_free(edge_delta);
                xr_free(queue);
                return fail(ctx, "XR_EXEC_5003", "ownership balance exceeds schema");
            }
            if (exit_balance < 0) {
                const char *origin_op = "unknown";
                for (uint32_t i = 0; i < ctx->plan->operation_count; i++) {
                    const XrSemanticOperationRecord *operation = &ctx->plan->operations[i];
                    if (operation->function == function &&
                        operation->result_value == ctx->certificate->owners[owner].origin_value) {
                        origin_op = xi_generated_op_name(operation->opcode);
                        break;
                    }
                }
                char event_summary[192] = {0};
                size_t event_used = 0;
                for (uint32_t e = 0; e < ctx->certificate->event_count; e++) {
                    const XrOwnershipEventRecord *event = &ctx->certificate->events[e];
                    if (event->owner != owner)
                        continue;
                    const XrSemanticOperationRecord *event_operation =
                        &ctx->plan->operations[event->operation];
                    int n = snprintf(event_summary + event_used, sizeof(event_summary) - event_used,
                                     "%s%s@%u:%d", event_used ? "," : "",
                                     xi_generated_op_name(event_operation->opcode), event->block,
                                     event->logical_delta);
                    if (n < 0 || (size_t) n >= sizeof(event_summary) - event_used)
                        break;
                    event_used += (size_t) n;
                }
                if (ctx->error && ctx->error_size)
                    snprintf(ctx->error, ctx->error_size,
                             "XR_OWN_3001: ownership balance becomes negative "
                             "(owner=%s origin=%s func=%s block=%u entry=%d delta=%d "
                             "events=[%s])",
                             ctx->certificate->owners[owner].canonical_key, origin_op, fn->name,
                             fn->block_begin + local, entry[local], delta[local], event_summary);
                xr_free(entry);
                xr_free(delta);
                xr_free(edge_delta);
                xr_free(queue);
                return false;
            }
            const XrSemanticBlockRecord *block = &ctx->plan->blocks[fn->block_begin + local];
            for (unsigned s = 0; s < 2; s++) {
                uint32_t successor = block->successors[s];
                if (successor == XR_SEMANTIC_INDEX_NONE ||
                    (s == 1 && successor == block->successors[0]))
                    continue;
                if (successor < fn->block_begin || successor >= fn->block_begin + fn->block_count) {
                    xr_free(entry);
                    xr_free(delta);
                    xr_free(edge_delta);
                    xr_free(queue);
                    return fail(ctx, "XR_OWN_3002", "ownership edge crosses function boundary");
                }
                int32_t edge_exit_balance = 0;
                if (!add_i32_checked(exit_balance, edge_delta[local * 2u + s],
                                     &edge_exit_balance)) {
                    xr_free(entry);
                    xr_free(delta);
                    xr_free(edge_delta);
                    xr_free(queue);
                    return fail(ctx, "XR_EXEC_5003", "ownership edge balance exceeds schema");
                }
                if (edge_exit_balance < 0) {
                    xr_free(entry);
                    xr_free(delta);
                    xr_free(edge_delta);
                    xr_free(queue);
                    return fail(ctx, "XR_OWN_3001",
                                "ownership balance becomes negative on a CFG edge");
                }
                uint32_t next = successor - fn->block_begin;
                if (entry[next] == INT32_MIN) {
                    entry[next] = edge_exit_balance;
                    queue[tail++] = next;
                } else if (entry[next] != edge_exit_balance) {
                    const char *origin_op = "unknown";
                    uint32_t origin_block = XR_SEMANTIC_INDEX_NONE;
                    uint32_t origin_local_value = XR_SEMANTIC_INDEX_NONE;
                    for (uint32_t i = 0; i < ctx->plan->operation_count; i++) {
                        const XrSemanticOperationRecord *operation = &ctx->plan->operations[i];
                        if (operation->function == function &&
                            operation->result_value ==
                                ctx->certificate->owners[owner].origin_value) {
                            origin_op = xi_generated_op_name(operation->opcode);
                            origin_block = operation->block;
                            origin_local_value = operation->result_value - fn->value_begin;
                            break;
                        }
                    }
                    char event_summary[160] = {0};
                    size_t event_used = 0;
                    for (uint32_t e = 0; e < ctx->certificate->event_count; e++) {
                        const XrOwnershipEventRecord *event = &ctx->certificate->events[e];
                        if (event->owner != owner || event->block != fn->block_begin + local)
                            continue;
                        const XrSemanticOperationRecord *event_operation =
                            &ctx->plan->operations[event->operation];
                        int n =
                            snprintf(event_summary + event_used, sizeof(event_summary) - event_used,
                                     "%s%s:%u:%d", event_used ? "," : "",
                                     xi_generated_op_name(event_operation->opcode), event->kind,
                                     event->logical_delta);
                        if (n < 0 || (size_t) n >= sizeof(event_summary) - event_used)
                            break;
                        event_used += (size_t) n;
                    }
                    if (ctx->error && ctx->error_size)
                        snprintf(ctx->error, ctx->error_size,
                                 "XR_OWN_3001: ownership balance differs across a CFG join or "
                                 "loop (owner=%s origin=%s origin-block=%u local-value=%u "
                                 "func=%s from-block=%u to-block=%u existing=%d incoming=%d "
                                 "events=[%s])",
                                 ctx->certificate->owners[owner].canonical_key, origin_op,
                                 origin_block, origin_local_value, fn->name,
                                 fn->block_begin + local, successor, entry[next], edge_exit_balance,
                                 event_summary);
                    xr_free(entry);
                    xr_free(delta);
                    xr_free(edge_delta);
                    xr_free(queue);
                    return false;
                }
            }
        }
        const XrOwnershipOwnerRecord *owner_record = &ctx->certificate->owners[owner];
        for (uint32_t local = 0; local < fn->block_count; local++) {
            if (entry[local] == INT32_MIN)
                continue;
            const XrSemanticBlockRecord *block = &ctx->plan->blocks[fn->block_begin + local];
            if (block->successors[0] != XR_SEMANTIC_INDEX_NONE ||
                block->successors[1] != XR_SEMANTIC_INDEX_NONE)
                continue;
            int32_t balance = 0;
            if (!add_i32_checked(entry[local], delta[local], &balance)) {
                xr_free(entry);
                xr_free(delta);
                xr_free(edge_delta);
                xr_free(queue);
                return fail(ctx, "XR_EXEC_5003", "ownership exit balance exceeds schema");
            }
            if (balance != 0) {
                char event_summary[192] = {0};
                size_t event_used = 0;
                for (uint32_t e = 0; e < ctx->certificate->event_count; e++) {
                    const XrOwnershipEventRecord *event = &ctx->certificate->events[e];
                    if (event->owner != owner)
                        continue;
                    const XrSemanticOperationRecord *event_operation =
                        &ctx->plan->operations[event->operation];
                    int n = snprintf(event_summary + event_used, sizeof(event_summary) - event_used,
                                     "%s%s@%u:%d", event_used ? "," : "",
                                     xi_generated_op_name(event_operation->opcode), event->block,
                                     event->logical_delta);
                    if (n < 0 || (size_t) n >= sizeof(event_summary) - event_used)
                        break;
                    event_used += (size_t) n;
                }
                if (ctx->error && ctx->error_size)
                    snprintf(ctx->error, ctx->error_size,
                             "XR_OWN_3001: ownership disposition does not balance at a "
                             "function exit (owner=%s func=%s block=%u balance=%d initial=%u "
                             "flags=%u events=[%s])",
                             owner_record->canonical_key, fn->name, fn->block_begin + local,
                             balance, owner_record->initial_state, owner_record->flags,
                             event_summary);
                xr_free(entry);
                xr_free(delta);
                xr_free(edge_delta);
                xr_free(queue);
                return false;
            }
        }
        for (uint32_t b = fn->block_begin; b < fn->block_begin + fn->block_count; b++) {
            uint32_t local = b - fn->block_begin;
            int32_t entry_balance = entry[local] == INT32_MIN ? 0 : entry[local];
            const XrSemanticBlockRecord *block = &ctx->plan->blocks[b];
            bool terminal = block->successors[0] == XR_SEMANTIC_INDEX_NONE &&
                            block->successors[1] == XR_SEMANTIC_INDEX_NONE;
            for (unsigned s = 0; s < 2; s++) {
                uint32_t successor = terminal ? XR_SEMANTIC_INDEX_NONE : block->successors[s];
                if (!terminal && successor == XR_SEMANTIC_INDEX_NONE)
                    continue;
                if (!terminal && s == 1 && successor == block->successors[0])
                    continue;
                if (!reserve_array((void **) &ctx->certificate->edge_states,
                                   &ctx->certificate->edge_state_capacity,
                                   ctx->certificate->edge_state_count + 1,
                                   sizeof(*ctx->certificate->edge_states),
                                   XR_OWNERSHIP_MAX_EDGE_STATES)) {
                    xr_free(entry);
                    xr_free(delta);
                    xr_free(edge_delta);
                    xr_free(queue);
                    return fail(ctx, "XR_EXEC_5003", "ownership edge-state budget exhausted");
                }
                XrOwnershipEdgeStateRecord *edge =
                    &ctx->certificate->edge_states[ctx->certificate->edge_state_count++];
                edge->owner = owner;
                edge->block = b;
                edge->successor = successor;
                bool owner_frontier =
                    entry[local] == INT32_MIN && !terminal &&
                    ctx->plan->operations[origin_operation].opcode == XI_PHI &&
                    successor == origin_block &&
                    (ctx->certificate->owners[owner].initial_state == XR_OWN_BORROWED
                         ? edge_delta[local * 2u + s] == 0
                         : edge_delta[local * 2u + s] == 1);
                edge->flags = owner_frontier
                                  ? XR_OWN_EDGE_OWNER_FRONTIER
                                  : (entry[local] == INT32_MIN ? XR_OWN_EDGE_OUT_OF_SCOPE : 0u);
                if (edge->flags == XR_OWN_EDGE_OUT_OF_SCOPE) {
                    edge->entry_balance = 0;
                    edge->exit_balance = 0;
                    edge->entry_state = XR_OWN_UNINITIALIZED;
                    edge->exit_state = XR_OWN_UNINITIALIZED;
                } else if (edge->flags == XR_OWN_EDGE_OWNER_FRONTIER) {
                    edge->entry_balance = 0;
                    edge->exit_balance =
                        ctx->certificate->owners[owner].initial_state == XR_OWN_BORROWED ? 0 : 1;
                    edge->entry_state = XR_OWN_UNINITIALIZED;
                    edge->exit_state =
                        ctx->certificate->owners[owner].initial_state == XR_OWN_BORROWED
                            ? XR_OWN_BORROWED
                            : XR_OWN_OWNED_LOCAL;
                } else {
                    edge->entry_balance = entry_balance;
                    if (!add_i32_three_checked(entry_balance, delta[local],
                                               terminal ? 0 : edge_delta[local * 2u + s],
                                               &edge->exit_balance)) {
                        xr_free(entry);
                        xr_free(delta);
                        xr_free(edge_delta);
                        xr_free(queue);
                        return fail(ctx, "XR_EXEC_5003", "ownership edge state exceeds schema");
                    }
                    const XrOwnershipOwnerRecord *owner_record = &ctx->certificate->owners[owner];
                    edge->entry_state =
                        ownership_state_for_balance(owner_record, edge->entry_balance);
                    edge->exit_state =
                        ownership_state_for_balance(owner_record, edge->exit_balance);
                }
                if (terminal)
                    break;
            }
        }
        xr_free(entry);
        xr_free(delta);
        xr_free(edge_delta);
        xr_free(queue);
    }
    if (ctx->certificate->edge_state_count)
        qsort(ctx->certificate->edge_states, ctx->certificate->edge_state_count,
              sizeof(*ctx->certificate->edge_states), compare_edge_state);
    return true;
}

static bool add_i32_checked(int32_t left, int32_t right, int32_t *out) {
    int64_t value = (int64_t) left + (int64_t) right;
    if (value < INT32_MIN || value > INT32_MAX)
        return false;
    *out = (int32_t) value;
    return true;
}

static bool add_i32_three_checked(int32_t first, int32_t second, int32_t third, int32_t *out) {
    int64_t value = (int64_t) first + (int64_t) second + (int64_t) third;
    if (value < INT32_MIN || value > INT32_MAX)
        return false;
    *out = (int32_t) value;
    return true;
}

static bool subtract_i32_checked(int32_t left, int32_t right, int32_t *out) {
    int64_t value = (int64_t) left - (int64_t) right;
    if (value < INT32_MIN || value > INT32_MAX)
        return false;
    *out = (int32_t) value;
    return true;
}

static int compare_edge_state(const void *left, const void *right) {
    const XrOwnershipEdgeStateRecord *a = (const XrOwnershipEdgeStateRecord *) left;
    const XrOwnershipEdgeStateRecord *b = (const XrOwnershipEdgeStateRecord *) right;
    if (a->owner != b->owner)
        return a->owner < b->owner ? -1 : 1;
    if (a->block != b->block)
        return a->block < b->block ? -1 : 1;
    if (a->successor != b->successor)
        return a->successor < b->successor ? -1 : 1;
    return 0;
}

static const XrOwnershipEdgeStateRecord *edge_state_for(const XrOwnershipCertificate *certificate,
                                                        uint32_t owner, uint32_t block,
                                                        uint32_t successor) {
    uint32_t low = 0;
    uint32_t high = certificate->edge_state_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        const XrOwnershipEdgeStateRecord *edge = &certificate->edge_states[middle];
        if (edge->owner < owner ||
            (edge->owner == owner &&
             (edge->block < block || (edge->block == block && edge->successor < successor))))
            low = middle + 1;
        else
            high = middle;
    }
    if (low == certificate->edge_state_count)
        return NULL;
    const XrOwnershipEdgeStateRecord *edge = &certificate->edge_states[low];
    return edge->owner == owner && edge->block == block && edge->successor == successor ? edge
                                                                                        : NULL;
}

static const XrOwnershipEdgeStateRecord *block_state_for(const XrOwnershipCertificate *certificate,
                                                         uint32_t owner, uint32_t block) {
    uint32_t low = 0;
    uint32_t high = certificate->edge_state_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        const XrOwnershipEdgeStateRecord *edge = &certificate->edge_states[middle];
        if (edge->owner < owner || (edge->owner == owner && edge->block < block))
            low = middle + 1;
        else
            high = middle;
    }
    if (low == certificate->edge_state_count)
        return NULL;
    const XrOwnershipEdgeStateRecord *edge = &certificate->edge_states[low];
    return edge->owner == owner && edge->block == block ? edge : NULL;
}

static uint8_t ownership_state_for_balance(const XrOwnershipOwnerRecord *owner, int32_t balance) {
    if (balance > 0)
        return XR_OWN_OWNED_LOCAL;
    if (owner->initial_state == XR_OWN_BORROWED ||
        owner->initial_state == XR_OWN_FOREIGN_BORROWED || owner->initial_state == XR_OWN_IMMORTAL)
        return owner->initial_state;
    return XR_OWN_RELEASED;
}

static int compare_loop_invariant(const void *left, const void *right) {
    const XrOwnershipLoopInvariantRecord *a = (const XrOwnershipLoopInvariantRecord *) left;
    const XrOwnershipLoopInvariantRecord *b = (const XrOwnershipLoopInvariantRecord *) right;
    int order = xr_stable_id_compare(a->id, b->id);
    return order != 0 ? order : strcmp(a->canonical_key, b->canonical_key);
}

typedef struct XrOwnershipOwnerFunctionRef {
    uint32_t function;
    uint32_t owner;
} XrOwnershipOwnerFunctionRef;

static int compare_owner_function_ref(const void *left, const void *right) {
    const XrOwnershipOwnerFunctionRef *a = (const XrOwnershipOwnerFunctionRef *) left;
    const XrOwnershipOwnerFunctionRef *b = (const XrOwnershipOwnerFunctionRef *) right;
    if (a->function != b->function)
        return a->function < b->function ? -1 : 1;
    return a->owner == b->owner ? 0 : (a->owner < b->owner ? -1 : 1);
}

static uint32_t owner_function_lower_bound(const XrOwnershipOwnerFunctionRef *owners,
                                           uint32_t count, uint32_t function) {
    uint32_t low = 0;
    uint32_t high = count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (owners[middle].function < function)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static bool add_loop_invariant(XrOwnershipBuildContext *ctx, uint32_t owner, uint32_t header,
                               uint32_t backedge, int32_t balance, uint8_t state) {
    XrOwnershipCertificate *certificate = ctx->certificate;
    if (!reserve_array((void **) &certificate->loop_invariants,
                       &certificate->loop_invariant_capacity, certificate->loop_invariant_count + 1,
                       sizeof(*certificate->loop_invariants), XR_OWNERSHIP_MAX_LOOP_INVARIANTS))
        return fail(ctx, "XR_EXEC_5003", "ownership loop-invariant budget exhausted");
    XrOwnershipLoopInvariantRecord *record =
        &certificate->loop_invariants[certificate->loop_invariant_count];
    memset(record, 0, sizeof(*record));
    char owner_id[XR_STABLE_ID_BYTES * 2 + 1];
    char header_id[XR_STABLE_ID_BYTES * 2 + 1];
    char backedge_id[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    xr_stable_id_hex(certificate->owners[owner].id, owner_id);
    xr_stable_id_hex(ctx->plan->blocks[header].id, header_id);
    xr_stable_id_hex(ctx->plan->blocks[backedge].id, backedge_id);
    int written = snprintf(key, sizeof(key), "ownership-loop-v1:owner=%s:header=%s:backedge=%s",
                           owner_id, header_id, backedge_id);
    if (written < 0 || (size_t) written >= sizeof(key))
        return fail(ctx, "XR_EXEC_5003", "ownership loop-invariant identity exceeds schema");
    record->canonical_key = copy_text(key);
    XrFingerprint digest;
    if (!record->canonical_key ||
        !xr_stable_id_from_key(record->canonical_key, &record->id, &digest)) {
        xr_free((void *) record->canonical_key);
        record->canonical_key = NULL;
        return fail(ctx, "XR_EXEC_5003", "ownership loop-invariant identity allocation failed");
    }
    record->owner = owner;
    record->header = header;
    record->backedge = backedge;
    record->balance = balance;
    record->state = state;
    certificate->loop_invariant_count++;
    return true;
}

static bool build_loop_invariants(XrOwnershipBuildContext *ctx) {
    XrSemanticGraph graph = {0};
    if (!xr_semantic_graph_build(ctx->plan, &graph, ctx->error, ctx->error_size))
        return false;
    XrOwnershipOwnerFunctionRef *owners = NULL;
    if (ctx->certificate->owner_count) {
        owners = (XrOwnershipOwnerFunctionRef *) xr_malloc((size_t) ctx->certificate->owner_count *
                                                           sizeof(*owners));
        if (!owners) {
            xr_semantic_graph_dispose(&graph);
            return fail(ctx, "XR_EXEC_5003", "ownership loop-invariant index allocation failed");
        }
    }
    for (uint32_t owner = 0; owner < ctx->certificate->owner_count; owner++) {
        owners[owner].function = ctx->certificate->owners[owner].function;
        owners[owner].owner = owner;
    }
    if (ctx->certificate->owner_count)
        qsort(owners, ctx->certificate->owner_count, sizeof(*owners), compare_owner_function_ref);
    bool valid = true;
    for (uint32_t backedge = 0; valid && backedge < ctx->plan->block_count; backedge++) {
        const XrSemanticBlockRecord *block = &ctx->plan->blocks[backedge];
        uint32_t owner_begin =
            owner_function_lower_bound(owners, ctx->certificate->owner_count, block->function);
        for (unsigned successor_index = 0; valid && successor_index < 2; successor_index++) {
            uint32_t header = block->successors[successor_index];
            if (header == XR_SEMANTIC_INDEX_NONE ||
                (successor_index == 1 && header == block->successors[0]) ||
                !xr_semantic_graph_dominates(&graph, header, backedge))
                continue;
            for (uint32_t position = owner_begin;
                 valid && position < ctx->certificate->owner_count &&
                 owners[position].function == block->function;
                 position++) {
                uint32_t owner = owners[position].owner;
                const XrOwnershipOwnerRecord *owner_record = &ctx->certificate->owners[owner];
                const XrOwnershipEdgeStateRecord *edge =
                    edge_state_for(ctx->certificate, owner, backedge, header);
                const XrOwnershipEdgeStateRecord *entry =
                    block_state_for(ctx->certificate, owner, header);
                if (!edge || !entry) {
                    valid = fail(ctx, "XR_OWN_3006",
                                 "natural backedge has no ownership dataflow state");
                    break;
                }
                if ((edge->flags & 1u) != 0 || (entry->flags & 1u) != 0)
                    continue;
                if (edge->exit_balance != entry->entry_balance) {
                    valid = fail(ctx, "XR_OWN_3006",
                                 "natural backedge does not preserve its ownership fixed point");
                    break;
                }
                uint8_t state = ownership_state_for_balance(owner_record, entry->entry_balance);
                valid =
                    add_loop_invariant(ctx, owner, header, backedge, entry->entry_balance, state);
            }
        }
    }
    xr_semantic_graph_dispose(&graph);
    xr_free(owners);
    if (!valid)
        return false;
    if (ctx->certificate->loop_invariant_count)
        qsort(ctx->certificate->loop_invariants, ctx->certificate->loop_invariant_count,
              sizeof(*ctx->certificate->loop_invariants), compare_loop_invariant);
    for (uint32_t i = 1; i < ctx->certificate->loop_invariant_count; i++) {
        if (xr_stable_id_equal(ctx->certificate->loop_invariants[i - 1].id,
                               ctx->certificate->loop_invariants[i].id))
            return fail(ctx, "XR_SEM_0003", "ownership loop-invariant identity is duplicated");
    }
    return true;
}

bool xr_ownership_certificate_build(XrSemanticPlan *plan, XrOwnershipCertificate **out, char *error,
                                    size_t error_size) {
    if (out)
        *out = NULL;
    if (!plan || !out || plan->frozen) {
        if (error && error_size)
            snprintf(error, error_size,
                     "XR_OWN_3001: ownership builder requires a mutable semantic plan");
        return false;
    }
    XrOwnershipBuildContext ctx = {0};
    ctx.plan = plan;
    ctx.error = error;
    ctx.error_size = error_size;
    for (uint32_t f = 0; f < plan->function_count; f++) {
        uint64_t end = (uint64_t) plan->functions[f].value_begin + plan->functions[f].value_count;
        if (end > UINT32_MAX)
            return false;
        if (end > ctx.value_count)
            ctx.value_count = (uint32_t) end;
    }
    ctx.certificate = (XrOwnershipCertificate *) xr_calloc(1, sizeof(*ctx.certificate));
    ctx.parent = (uint32_t *) xr_malloc((size_t) ctx.value_count * sizeof(*ctx.parent));
    ctx.rank = (uint8_t *) xr_calloc(ctx.value_count, sizeof(*ctx.rank));
    ctx.owner_by_root =
        (uint32_t *) xr_malloc((size_t) ctx.value_count * sizeof(*ctx.owner_by_root));
    if (!ctx.certificate || (ctx.value_count && (!ctx.parent || !ctx.rank || !ctx.owner_by_root))) {
        fail(&ctx, "XR_EXEC_5003", "ownership worklist allocation failed");
        goto failure;
    }
    ctx.certificate->schema = XR_SEMANTIC_SCHEMA_VERSION;
    for (uint32_t i = 0; i < ctx.value_count; i++) {
        ctx.parent[i] = i;
        ctx.owner_by_root[i] = XR_SEMANTIC_INDEX_NONE;
    }
    if (!build_equivalence_classes(&ctx) || !ensure_owners(&ctx))
        goto failure;
    ctx.loan_status = (uint8_t *) xr_calloc(ctx.certificate->owner_count, sizeof(*ctx.loan_status));
    ctx.loan_parameter =
        (int16_t *) xr_malloc((size_t) ctx.certificate->owner_count * sizeof(*ctx.loan_parameter));
    if (ctx.certificate->owner_count && (!ctx.loan_status || !ctx.loan_parameter)) {
        fail(&ctx, "XR_EXEC_5003", "ownership loan-source audit allocation failed");
        goto failure;
    }
    for (uint32_t owner = 0; owner < ctx.certificate->owner_count; owner++)
        ctx.loan_parameter[owner] = XR_OWN_LOAN_STATIC;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (!classify_definition(&ctx, i))
            goto failure;
    }
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (!add_operand_events(&ctx, i))
            goto failure;
    }
    if (!classify_returns(&ctx) || !classify_stack_extents(&ctx) ||
        !classify_implicit_exit_dispositions(&ctx) || !build_edge_states(&ctx) ||
        !build_loop_invariants(&ctx))
        goto failure;
    xr_free(ctx.parent);
    xr_free(ctx.rank);
    xr_free(ctx.owner_by_root);
    xr_free(ctx.loan_status);
    xr_free(ctx.loan_parameter);
    *out = ctx.certificate;
    return true;

failure:
    xr_free(ctx.parent);
    xr_free(ctx.rank);
    xr_free(ctx.owner_by_root);
    xr_free(ctx.loan_status);
    xr_free(ctx.loan_parameter);
    xr_ownership_certificate_free(ctx.certificate);
    return false;
}
