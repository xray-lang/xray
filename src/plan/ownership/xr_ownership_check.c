/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_ownership_check.c - Independent ownership certificate checker
 */

#include "xr_ownership_check.h"
#include "xr_ownership_certificate_internal.h"
#include "xr_ownership_replay.h"
#include "../semantic/xr_semantic_graph.h"
#include "../semantic/xr_semantic_plan_internal.h"
#include "../../base/xmalloc.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool report(char *error, size_t size, const char *code, const char *detail) {
    if (error && size)
        snprintf(error, size, "%s: %s", code, detail);
    return false;
}

static const XrOwnershipEdgeStateRecord *find_edge(const XrOwnershipCertificate *certificate,
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

static bool add_i32_checked(int32_t left, int32_t right, int32_t *out) {
    int64_t value = (int64_t) left + (int64_t) right;
    if (value < INT32_MIN || value > INT32_MAX)
        return false;
    *out = (int32_t) value;
    return true;
}

static const XrOwnershipEdgeStateRecord *find_block_state(const XrOwnershipCertificate *certificate,
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

static bool loop_invariant_id_for(const XrSemanticPlan *plan,
                                  const XrOwnershipCertificate *certificate, uint32_t owner,
                                  uint32_t header, uint32_t backedge, XrStableId *out) {
    char owner_id[XR_STABLE_ID_BYTES * 2 + 1];
    char header_id[XR_STABLE_ID_BYTES * 2 + 1];
    char backedge_id[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    xr_stable_id_hex(certificate->owners[owner].id, owner_id);
    xr_stable_id_hex(plan->blocks[header].id, header_id);
    xr_stable_id_hex(plan->blocks[backedge].id, backedge_id);
    int written = snprintf(key, sizeof(key),
                           "ownership-loop-v1:owner=%s:header=%s:backedge=%s", owner_id,
                           header_id, backedge_id);
    XrFingerprint digest;
    return written >= 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static const XrOwnershipLoopInvariantRecord *find_loop_invariant(
    const XrSemanticPlan *plan, const XrOwnershipCertificate *certificate, uint32_t owner,
    uint32_t header, uint32_t backedge) {
    XrStableId expected;
    if (!loop_invariant_id_for(plan, certificate, owner, header, backedge, &expected))
        return NULL;
    uint32_t low = 0;
    uint32_t high = certificate->loop_invariant_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (xr_stable_id_compare(certificate->loop_invariants[middle].id, expected) < 0)
            low = middle + 1;
        else
            high = middle;
    }
    return low < certificate->loop_invariant_count &&
                   xr_stable_id_equal(certificate->loop_invariants[low].id, expected)
               ? &certificate->loop_invariants[low]
               : NULL;
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

static bool edge_states_are_ordered(const XrOwnershipCertificate *certificate) {
    for (uint32_t i = 1; i < certificate->edge_state_count; i++) {
        const XrOwnershipEdgeStateRecord *previous = &certificate->edge_states[i - 1];
        const XrOwnershipEdgeStateRecord *current = &certificate->edge_states[i];
        if (previous->owner > current->owner ||
            (previous->owner == current->owner && previous->block > current->block) ||
            (previous->owner == current->owner && previous->block == current->block &&
             previous->successor >= current->successor))
            return false;
    }
    return true;
}

static uint8_t state_for_balance(const XrOwnershipOwnerRecord *owner, int32_t balance) {
    if (balance > 0)
        return XR_OWN_OWNED_LOCAL;
    if (owner->initial_state == XR_OWN_BORROWED ||
        owner->initial_state == XR_OWN_FOREIGN_BORROWED ||
        owner->initial_state == XR_OWN_IMMORTAL)
        return owner->initial_state;
    return XR_OWN_RELEASED;
}

static bool loop_invariant_key_valid(const XrSemanticPlan *plan,
                                     const XrOwnershipCertificate *certificate,
                                     const XrOwnershipLoopInvariantRecord *invariant) {
    char owner_id[XR_STABLE_ID_BYTES * 2 + 1];
    char header_id[XR_STABLE_ID_BYTES * 2 + 1];
    char backedge_id[XR_STABLE_ID_BYTES * 2 + 1];
    char expected[192];
    xr_stable_id_hex(certificate->owners[invariant->owner].id, owner_id);
    xr_stable_id_hex(plan->blocks[invariant->header].id, header_id);
    xr_stable_id_hex(plan->blocks[invariant->backedge].id, backedge_id);
    int written = snprintf(expected, sizeof(expected),
                           "ownership-loop-v1:owner=%s:header=%s:backedge=%s", owner_id,
                           header_id, backedge_id);
    return written >= 0 && (size_t) written < sizeof(expected) && invariant->canonical_key &&
           strcmp(invariant->canonical_key, expected) == 0;
}

static bool event_program_point_valid(const XrSemanticPlan *plan,
                                      const XrOwnershipCertificate *certificate,
                                      const XrOwnershipEventRecord *event) {
    if (event->owner >= certificate->owner_count || event->operation >= plan->operation_count ||
        event->block >= plan->block_count || event->program_point > XR_OWN_POINT_EDGE ||
        event->reserved != 0)
        return false;
    const XrOwnershipOwnerRecord *owner = &certificate->owners[event->owner];
    const XrSemanticOperationRecord *operation = &plan->operations[event->operation];
    const XrSemanticBlockRecord *block = &plan->blocks[event->block];
    if (operation->function != owner->function || block->function != owner->function)
        return false;
    if (event->program_point == XR_OWN_POINT_AFTER_OPERATION)
        return event->successor == XR_SEMANTIC_INDEX_NONE && operation->block == event->block;
    if (event->program_point == XR_OWN_POINT_BLOCK_EXIT)
        return event->successor == XR_SEMANTIC_INDEX_NONE;
    return event->successor != XR_SEMANTIC_INDEX_NONE &&
           (block->successors[0] == event->successor || block->successors[1] == event->successor);
}

static bool event_canonical_key_valid(const XrSemanticPlan *plan,
                                      const XrOwnershipCertificate *certificate,
                                      uint32_t event_index) {
    const XrOwnershipEventRecord *event = &certificate->events[event_index];
    char owner_id[XR_STABLE_ID_BYTES * 2 + 1];
    char operation_id[XR_STABLE_ID_BYTES * 2 + 1];
    char expected[224];
    xr_stable_id_hex(certificate->owners[event->owner].id, owner_id);
    xr_stable_id_hex(plan->operations[event->operation].id, operation_id);
    int written = snprintf(expected, sizeof(expected), "ownership-event-v4:%s:%s:%u:%u:%u:%u:%u",
                           owner_id, operation_id, event->block, event->successor, event->kind,
                           event->program_point, event_index);
    return written >= 0 && (size_t) written < sizeof(expected) && event->canonical_key &&
           strcmp(event->canonical_key, expected) == 0;
}

#define XR_OWNERSHIP_AUDIT_MAX_EVENTS UINT32_C(20000000)

enum {
    XR_OWN_LOAN_STATIC = -1,
    XR_OWN_LOAN_MULTIPLE = -2,
    XR_OWN_LOAN_CYCLE = -3,
};

typedef struct XrOwnershipExpectedEvent {
    uint32_t owner;
    uint32_t operation;
    uint32_t block;
    uint32_t successor;
    int16_t logical_delta;
    uint8_t kind;
    uint8_t state_after;
    uint8_t program_point;
} XrOwnershipExpectedEvent;

typedef struct XrOwnershipAudit {
    const XrSemanticPlan *plan;
    const XrOwnershipCertificate *certificate;
    uint32_t value_count;
    uint32_t *parent;
    uint8_t *rank;
    uint32_t *owner_by_root;
    uint32_t *producer_by_value;
    uint8_t *initial_state;
    uint8_t *exit_state;
    uint8_t *return_provenance;
    uint8_t *flags;
    uint8_t *loan_status;
    int16_t *loan_parameter;
    XrOwnershipExpectedEvent *events;
    uint32_t event_count;
    uint32_t event_capacity;
} XrOwnershipAudit;

static uint32_t audit_find_root(XrOwnershipAudit *audit, uint32_t value) {
    uint32_t root = value;
    while (audit->parent[root] != root)
        root = audit->parent[root];
    while (audit->parent[value] != value) {
        uint32_t next = audit->parent[value];
        audit->parent[value] = root;
        value = next;
    }
    return root;
}

static void audit_union_values(XrOwnershipAudit *audit, uint32_t left, uint32_t right) {
    uint32_t a = audit_find_root(audit, left);
    uint32_t b = audit_find_root(audit, right);
    if (a == b)
        return;
    if (audit->rank[a] < audit->rank[b]) {
        uint32_t swap = a;
        a = b;
        b = swap;
    }
    audit->parent[b] = a;
    if (audit->rank[a] == audit->rank[b])
        audit->rank[a]++;
}

static bool audit_type_is_owner(const XrSemanticPlan *plan, uint32_t type) {
    return type < plan->type_count &&
           (plan->types[type].flags & XR_SEM_TYPE_OWNERSHIP_ROOT) != 0;
}

static bool audit_operation_has_owner(const XrSemanticPlan *plan,
                                      const XrSemanticOperationRecord *operation) {
    return audit_type_is_owner(plan, operation->result_type) &&
           xi_generated_op_result_kind(operation->opcode) != XI_GEN_RESULT_VOID &&
           operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_NONE;
}

static uint32_t audit_owner_for_value(XrOwnershipAudit *audit, uint32_t value) {
    return value < audit->value_count ? audit->owner_by_root[audit_find_root(audit, value)]
                                      : XR_SEMANTIC_INDEX_NONE;
}

static bool audit_add_event(XrOwnershipAudit *audit, uint32_t owner, uint32_t operation,
                            uint32_t block, uint32_t successor, uint8_t kind, int16_t delta,
                            uint8_t state, uint8_t point) {
    if (audit->event_count >= audit->event_capacity)
        return false;
    XrOwnershipExpectedEvent *event = &audit->events[audit->event_count++];
    event->owner = owner;
    event->operation = operation;
    event->block = block;
    event->successor = successor;
    event->logical_delta = delta;
    event->kind = kind;
    event->state_after = state;
    event->program_point = point;
    return true;
}

static int compare_expected_event(const void *left, const void *right) {
    const XrOwnershipExpectedEvent *a = (const XrOwnershipExpectedEvent *) left;
    const XrOwnershipExpectedEvent *b = (const XrOwnershipExpectedEvent *) right;
#define XR_AUDIT_COMPARE(field)                                                                    \
    do {                                                                                           \
        if (a->field != b->field)                                                                  \
            return a->field < b->field ? -1 : 1;                                                   \
    } while (0)
    XR_AUDIT_COMPARE(owner);
    XR_AUDIT_COMPARE(operation);
    XR_AUDIT_COMPARE(block);
    XR_AUDIT_COMPARE(successor);
    XR_AUDIT_COMPARE(program_point);
    XR_AUDIT_COMPARE(kind);
    XR_AUDIT_COMPARE(logical_delta);
    XR_AUDIT_COMPARE(state_after);
#undef XR_AUDIT_COMPARE
    return 0;
}

static void audit_dispose(XrOwnershipAudit *audit) {
    xr_free(audit->parent);
    xr_free(audit->rank);
    xr_free(audit->owner_by_root);
    xr_free(audit->producer_by_value);
    xr_free(audit->initial_state);
    xr_free(audit->exit_state);
    xr_free(audit->return_provenance);
    xr_free(audit->flags);
    xr_free(audit->loan_status);
    xr_free(audit->loan_parameter);
    xr_free(audit->events);
}

static bool audit_prepare_equivalence(XrOwnershipAudit *audit, char *error, size_t error_size) {
    for (uint32_t f = 0; f < audit->plan->function_count; f++) {
        uint64_t end = (uint64_t) audit->plan->functions[f].value_begin +
                       audit->plan->functions[f].value_count;
        if (end > UINT32_MAX)
            return report(error, error_size, "XR_EXEC_5003",
                          "ownership value universe exceeds the checker schema");
        if (end > audit->value_count)
            audit->value_count = (uint32_t) end;
    }
    audit->parent = (uint32_t *) xr_malloc((size_t) audit->value_count * sizeof(*audit->parent));
    audit->rank = (uint8_t *) xr_calloc(audit->value_count, sizeof(*audit->rank));
    audit->owner_by_root =
        (uint32_t *) xr_malloc((size_t) audit->value_count * sizeof(*audit->owner_by_root));
    audit->producer_by_value =
        (uint32_t *) xr_malloc((size_t) audit->value_count * sizeof(*audit->producer_by_value));
    if (audit->value_count &&
        (!audit->parent || !audit->rank || !audit->owner_by_root || !audit->producer_by_value))
        return report(error, error_size, "XR_EXEC_5003",
                      "ownership equivalence audit allocation failed");
    for (uint32_t value = 0; value < audit->value_count; value++) {
        audit->parent[value] = value;
        audit->owner_by_root[value] = XR_SEMANTIC_INDEX_NONE;
        audit->producer_by_value[value] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < audit->plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &audit->plan->operations[i];
        if (operation->result_value < audit->value_count &&
            xi_generated_op_result_kind(operation->opcode) != XI_GEN_RESULT_VOID)
            audit->producer_by_value[operation->result_value] = i;
        if (!audit_operation_has_owner(audit->plan, operation) ||
            operation->result_value >= audit->value_count)
            continue;
        if (operation->ownership_use == XI_GEN_OWN_USE_PASS && operation->opcode != XI_PHI) {
            for (uint16_t a = 0; a < operation->operand_count; a++) {
                uint32_t operand =
                    audit->plan->operands[operation->operand_begin + a].value;
                if (operand < audit->value_count)
                    audit_union_values(audit, operation->result_value, operand);
            }
        } else if (operation->result_alias_operand >= 0 &&
                   (uint16_t) operation->result_alias_operand < operation->operand_count) {
            uint32_t operand = audit->plan
                                   ->operands[operation->operand_begin +
                                              (uint16_t) operation->result_alias_operand]
                                   .value;
            if (operand < audit->value_count)
                audit_union_values(audit, operation->result_value, operand);
        }
    }
    return true;
}

static bool audit_canonical_owners(XrOwnershipAudit *audit, char *error, size_t error_size) {
    uint32_t expected_count = 0;
    for (uint32_t i = 0; i < audit->plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &audit->plan->operations[i];
        if (!audit_operation_has_owner(audit->plan, operation) ||
            operation->result_value >= audit->value_count)
            continue;
        uint32_t root = audit_find_root(audit, operation->result_value);
        if (audit->owner_by_root[root] != XR_SEMANTIC_INDEX_NONE)
            continue;
        if (expected_count >= audit->certificate->owner_count)
            return report(error, error_size, "XR_OWN_3002",
                          "ownership owner coverage omits a semantic root");
        audit->owner_by_root[root] = expected_count;
        const XrOwnershipOwnerRecord *owner = &audit->certificate->owners[expected_count];
        char operation_id[XR_STABLE_ID_BYTES * 2 + 1];
        char expected_key[256];
        xr_stable_id_hex(operation->id, operation_id);
        int written = snprintf(expected_key, sizeof(expected_key), "owner-v2:%s:value=%u",
                               operation_id, root);
        if (written < 0 || (size_t) written >= sizeof(expected_key) ||
            !owner->canonical_key || strcmp(owner->canonical_key, expected_key) != 0 ||
            owner->function != operation->function ||
            owner->origin_value != operation->result_value)
            return report(error, error_size, "XR_OWN_3002",
                          "ownership owner origin or canonical root is not exact");
        expected_count++;
    }
    if (expected_count != audit->certificate->owner_count)
        return report(error, error_size, "XR_OWN_3002",
                      "ownership owner coverage contains an extra semantic root");
    uint32_t count = expected_count;
    audit->initial_state = (uint8_t *) xr_calloc(count, sizeof(*audit->initial_state));
    audit->exit_state = (uint8_t *) xr_malloc((size_t) count * sizeof(*audit->exit_state));
    audit->return_provenance =
        (uint8_t *) xr_calloc(count, sizeof(*audit->return_provenance));
    audit->flags = (uint8_t *) xr_calloc(count, sizeof(*audit->flags));
    audit->loan_status = (uint8_t *) xr_calloc(count, sizeof(*audit->loan_status));
    audit->loan_parameter =
        (int16_t *) xr_malloc((size_t) count * sizeof(*audit->loan_parameter));
    if (count && (!audit->initial_state || !audit->exit_state ||
                  !audit->return_provenance || !audit->flags || !audit->loan_status ||
                  !audit->loan_parameter))
        return report(error, error_size, "XR_EXEC_5003",
                      "ownership owner audit allocation failed");
    for (uint32_t owner = 0; owner < count; owner++) {
        audit->exit_state[owner] = XR_OWN_RELEASED;
        audit->loan_parameter[owner] = XR_OWN_LOAN_STATIC;
    }
    return true;
}

static bool audit_definition_events(XrOwnershipAudit *audit) {
    for (uint32_t i = 0; i < audit->plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &audit->plan->operations[i];
        if (!audit_operation_has_owner(audit->plan, operation))
            continue;
        uint32_t owner = audit_owner_for_value(audit, operation->result_value);
        if (owner == XR_SEMANTIC_INDEX_NONE)
            return false;
        uint8_t *initial = &audit->initial_state[owner];
        if (operation->opcode == XI_PARAM) {
            if (operation->parameter_ownership == XI_OWN_OWNED) {
                *initial = XR_OWN_OWNED_LOCAL;
                if (!audit_add_event(audit, owner, i, operation->block, XR_SEMANTIC_INDEX_NONE,
                                     XR_OWN_EVENT_ALLOC, 1, XR_OWN_OWNED_LOCAL,
                                     XR_OWN_POINT_AFTER_OPERATION))
                    return false;
            } else {
                if (*initial == XR_OWN_UNINITIALIZED)
                    *initial = XR_OWN_BORROWED;
                if (!audit_add_event(audit, owner, i, operation->block, XR_SEMANTIC_INDEX_NONE,
                                     XR_OWN_EVENT_BORROW, 0, XR_OWN_BORROWED,
                                     XR_OWN_POINT_AFTER_OPERATION))
                    return false;
            }
        } else if (operation->opcode == XI_CONST) {
            *initial = XR_OWN_IMMORTAL;
            audit->exit_state[owner] = XR_OWN_IMMORTAL;
        } else if (operation->opcode == XI_STACK_ALLOC) {
            *initial = XR_OWN_OWNED_UNIQUE;
            if (!audit_add_event(audit, owner, i, operation->block, XR_SEMANTIC_INDEX_NONE,
                                 XR_OWN_EVENT_ALLOC, 1, XR_OWN_OWNED_UNIQUE,
                                 XR_OWN_POINT_AFTER_OPERATION))
                return false;
        } else if (operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
                   operation->result_alias_operand < 0) {
            bool immortal = operation->opcode == XI_GET_SHARED ||
                            operation->opcode == XI_GET_GLOBAL ||
                            operation->opcode == XI_GET_BUILTIN ||
                            operation->opcode == XI_IMPORT_REF;
            if (immortal) {
                *initial = XR_OWN_IMMORTAL;
                audit->exit_state[owner] = XR_OWN_IMMORTAL;
            } else {
                if (*initial == XR_OWN_UNINITIALIZED)
                    *initial = XR_OWN_BORROWED;
                if (!audit_add_event(audit, owner, i, operation->block, XR_SEMANTIC_INDEX_NONE,
                                     XR_OWN_EVENT_BORROW, 0, XR_OWN_BORROWED,
                                     XR_OWN_POINT_AFTER_OPERATION))
                    return false;
            }
        } else if (operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT &&
                   operation->return_provenance != XI_RETURN_OWNERSHIP_OWNED) {
            if (*initial == XR_OWN_UNINITIALIZED)
                *initial = XR_OWN_FOREIGN_BORROWED;
            if (!audit_add_event(audit, owner, i, operation->block, XR_SEMANTIC_INDEX_NONE,
                                 XR_OWN_EVENT_BORROW, 0, XR_OWN_FOREIGN_BORROWED,
                                 XR_OWN_POINT_AFTER_OPERATION))
                return false;
        } else if (operation->ownership_use != XI_GEN_OWN_USE_PASS &&
                   operation->opcode != XI_RETAIN && operation->opcode != XI_RELEASE &&
                   operation->result_alias_operand < 0 &&
                   (operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED ||
                    operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT)) {
            if (*initial == XR_OWN_UNINITIALIZED)
                *initial = XR_OWN_OWNED_LOCAL;
            if (!audit_add_event(audit, owner, i, operation->block, XR_SEMANTIC_INDEX_NONE,
                                 XR_OWN_EVENT_ALLOC, 1, XR_OWN_OWNED_LOCAL,
                                 XR_OWN_POINT_AFTER_OPERATION))
                return false;
        }
    }
    return true;
}

static bool audit_owner_has_function_loan(XrOwnershipAudit *audit, uint32_t owner,
                                          uint32_t function, int16_t *parameter) {
    *parameter = -1;
    if (owner == XR_SEMANTIC_INDEX_NONE)
        return true;
    if (audit->certificate->owners[owner].function != function)
        return false;
    if (audit->loan_status[owner] == 2) {
        *parameter = audit->loan_parameter[owner];
        return true;
    }
    if (audit->loan_status[owner] == 3)
        return false;
    if (audit->loan_status[owner] == 1) {
        *parameter = XR_OWN_LOAN_CYCLE;
        return true;
    }
    audit->loan_status[owner] = 1;
    uint32_t value = audit->certificate->owners[owner].origin_value;
    uint32_t origin = value < audit->value_count ? audit->producer_by_value[value]
                                                 : XR_SEMANTIC_INDEX_NONE;
    if (origin == XR_SEMANTIC_INDEX_NONE) {
        audit->loan_status[owner] = 3;
        return false;
    }
    const XrSemanticOperationRecord *operation = &audit->plan->operations[origin];
    int16_t derived = XR_OWN_LOAN_STATIC;
    bool valid = false;
    if (operation->function != function) {
        valid = false;
    } else if (audit->initial_state[owner] == XR_OWN_IMMORTAL ||
               operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC) {
        valid = true;
    } else if (operation->opcode == XI_PARAM &&
               operation->parameter_ownership == XI_OWN_BORROWED &&
               operation->semantic_immediate >= 0 &&
               operation->semantic_immediate <= INT16_MAX) {
        derived = (int16_t) operation->semantic_immediate;
        valid = true;
    } else if (operation->opcode == XI_PHI) {
        bool saw_seed = false;
        for (uint16_t a = 0; a < operation->operand_count; a++) {
            uint32_t source = audit_owner_for_value(
                audit, audit->plan->operands[operation->operand_begin + a].value);
            if (source == owner)
                continue;
            int16_t candidate = XR_OWN_LOAN_STATIC;
            if (!audit_owner_has_function_loan(audit, source, function, &candidate)) {
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
    } else if ((audit->initial_state[owner] == XR_OWN_BORROWED ||
                audit->initial_state[owner] == XR_OWN_FOREIGN_BORROWED) &&
               operation->result_alias_operand >= 0 &&
               (uint16_t) operation->result_alias_operand < operation->operand_count) {
        uint32_t source = audit_owner_for_value(
            audit, audit->plan
                       ->operands[operation->operand_begin +
                                  (uint16_t) operation->result_alias_operand]
                       .value);
        if (source != owner)
            valid = audit_owner_has_function_loan(audit, source, function, &derived);
    }
    audit->loan_status[owner] = valid ? 2 : 3;
    audit->loan_parameter[owner] = derived;
    *parameter = derived;
    return valid;
}

static bool audit_owner_is_returned(XrOwnershipAudit *audit, uint32_t owner) {
    uint32_t function = audit->certificate->owners[owner].function;
    const XrSemanticFunctionRecord *record = &audit->plan->functions[function];
    for (uint32_t block = record->block_begin; block < record->block_begin + record->block_count;
         block++) {
        if (audit->plan->blocks[block].kind == XI_BLOCK_RETURN &&
            audit_owner_for_value(audit, audit->plan->blocks[block].control_value) == owner)
            return true;
    }
    return false;
}

static bool audit_owner_is_retained_in_block(XrOwnershipAudit *audit, uint32_t owner,
                                             uint32_t block_index) {
    const XrSemanticBlockRecord *block = &audit->plan->blocks[block_index];
    for (uint32_t i = 0; i < block->operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            &audit->plan->operations[block->operation_begin + i];
        if (operation->opcode != XI_RETAIN)
            continue;
        for (uint16_t a = 0; a < operation->operand_count; a++) {
            uint32_t value = audit->plan->operands[operation->operand_begin + a].value;
            if (audit_owner_for_value(audit, value) == owner)
                return true;
        }
    }
    return false;
}

static bool audit_operand_events(XrOwnershipAudit *audit, char *error, size_t error_size) {
    for (uint32_t i = 0; i < audit->plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &audit->plan->operations[i];
        if (operation->opcode == XI_PHI && audit_type_is_owner(audit->plan, operation->result_type)) {
            const XrSemanticBlockRecord *block = &audit->plan->blocks[operation->block];
            uint32_t result_owner = audit_owner_for_value(audit, operation->result_value);
            if (result_owner == XR_SEMANTIC_INDEX_NONE ||
                operation->operand_count != block->predecessor_count)
                return false;
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
                uint32_t source_value =
                    audit->plan->operands[operation->operand_begin + a].value;
                uint32_t source = audit_owner_for_value(audit, source_value);
                if (source == result_owner)
                    continue;
                uint8_t source_state = source == XR_SEMANTIC_INDEX_NONE
                                           ? XR_OWN_IMMORTAL
                                           : audit->initial_state[source];
                int16_t parameter = XR_OWN_LOAN_STATIC;
                bool borrowed_source = source_state == XR_OWN_BORROWED ||
                                       source_state == XR_OWN_FOREIGN_BORROWED;
                bool function_loan =
                    borrowed_source && audit_owner_has_function_loan(
                                           audit, source, operation->function, &parameter);
                if (borrowed_source) {
                    saw_borrowed_source = true;
                    if (!function_loan)
                        borrowed_sources_are_function_loans = false;
                    uint32_t predecessor =
                        audit->plan->predecessors[block->predecessor_begin + a];
                    if (!audit_owner_is_retained_in_block(audit, source, predecessor)) {
                        borrowed_sources_promoted = false;
                        if (!have_unpromoted_source) {
                            have_unpromoted_source = true;
                            unpromoted_operand = a;
                            unpromoted_value = source_value;
                            unpromoted_owner = source;
                            unpromoted_predecessor = predecessor;
                        }
                    }
                    if (function_loan && parameter == XR_OWN_LOAN_MULTIPLE &&
                        audit_owner_is_returned(audit, result_owner))
                        return report(error, error_size, "XR_OWN_3004",
                                      "returned borrowed PHI has multiple loan roots");
                    if (function_loan && parameter >= 0 && unique_parameter < 0)
                        unique_parameter = parameter;
                    else if (function_loan && parameter >= 0 &&
                             unique_parameter != parameter &&
                             audit_owner_is_returned(audit, result_owner))
                        return report(error, error_size, "XR_OWN_3004",
                                      "returned borrowed PHI joins different parameter loans");
                } else if (source_state != XR_OWN_IMMORTAL) {
                    saw_owned_source = true;
                }
            }
            bool borrowed_phi = saw_borrowed_source && !saw_owned_source &&
                                borrowed_sources_are_function_loans;
            if (saw_borrowed_source && !borrowed_phi && !borrowed_sources_promoted) {
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
                        &audit->certificate->owners[unpromoted_owner];
                    origin_value = source->origin_value;
                    source_state = audit->initial_state[unpromoted_owner];
                    origin_operation = audit->producer_by_value[source->origin_value];
                    if (origin_operation != XR_SEMANTIC_INDEX_NONE) {
                        const XrSemanticOperationRecord *origin =
                            &audit->plan->operations[origin_operation];
                        origin_opcode = origin->opcode;
                        origin_result_ownership = origin->result_ownership;
                        origin_alias = origin->result_alias_operand;
                        origin_return_provenance = origin->return_provenance;
                        origin_return_parameter = origin->return_parameter;
                    }
                }
                if (error && error_size)
                    snprintf(error, error_size,
                             "XR_OWN_3004: PHI non-function loan has no explicit predecessor "
                             "retain (func=%s op=%u operand=%u value=%u source-owner=%u "
                             "source-state=%u origin-value=%u origin-op=%u origin-opcode=%u "
                             "result-own=%u alias=%d return-prov=%u return-param=%d "
                             "predecessor=%u matching-retain=none)",
                             audit->plan->functions[operation->function].name, i,
                             unpromoted_operand, unpromoted_value, unpromoted_owner,
                             source_state, origin_value, origin_operation, origin_opcode,
                             origin_result_ownership, origin_alias, origin_return_provenance,
                             origin_return_parameter, unpromoted_predecessor);
                return false;
            }
            audit->initial_state[result_owner] =
                borrowed_phi ? XR_OWN_BORROWED : XR_OWN_OWNED_LOCAL;
            for (uint16_t a = 0; a < operation->operand_count; a++) {
                uint32_t source = audit_owner_for_value(
                    audit, audit->plan->operands[operation->operand_begin + a].value);
                uint32_t predecessor =
                    audit->plan->predecessors[block->predecessor_begin + a];
                uint8_t source_state =
                    source == XR_SEMANTIC_INDEX_NONE ? XR_OWN_IMMORTAL
                                                     : audit->initial_state[source];
                if (borrowed_phi) {
                    if (!audit_add_event(audit, result_owner, i, predecessor, operation->block,
                                         XR_OWN_EVENT_BORROW, 0, XR_OWN_BORROWED,
                                         XR_OWN_POINT_EDGE))
                        return false;
                    continue;
                }
                if (source == result_owner)
                    continue;
                if (source != XR_SEMANTIC_INDEX_NONE && source_state != XR_OWN_IMMORTAL &&
                    !audit_add_event(audit, source, i, predecessor, operation->block,
                                     XR_OWN_EVENT_MOVE, -1, XR_OWN_MOVED, XR_OWN_POINT_EDGE))
                    return false;
                if (!audit_add_event(audit, result_owner, i, predecessor, operation->block,
                                     XR_OWN_EVENT_MOVE, 1, XR_OWN_OWNED_LOCAL,
                                     XR_OWN_POINT_EDGE))
                    return false;
            }
            continue;
        }
        for (uint16_t a = 0; a < operation->operand_count; a++) {
            const XrSemanticOperandRecord *operand =
                &audit->plan->operands[operation->operand_begin + a];
            uint32_t owner = audit_owner_for_value(audit, operand->value);
            if (owner == XR_SEMANTIC_INDEX_NONE ||
                audit->initial_state[owner] == XR_OWN_IMMORTAL)
                continue;
            uint8_t kind = XR_OWN_EVENT_MOVE;
            int16_t delta = 0;
            uint8_t state = XR_OWN_MOVED;
            bool emit = false;
            if (operation->opcode == XI_RETAIN) {
                kind = XR_OWN_EVENT_RETAIN;
                delta = 1;
                state = XR_OWN_OWNED_LOCAL;
                emit = true;
            } else if (operation->opcode == XI_RELEASE) {
                uint32_t producer = operand->value < audit->value_count
                                        ? audit->producer_by_value[operand->value]
                                        : XR_SEMANTIC_INDEX_NONE;
                kind = producer != XR_SEMANTIC_INDEX_NONE &&
                               audit->plan->operations[producer].opcode == XI_STACK_ALLOC &&
                               audit->plan->operations[producer].semantic_immediate == XI_CLOSURE_NEW
                           ? XR_OWN_EVENT_DESTROY
                           : XR_OWN_EVENT_RELEASE;
                delta = -1;
                state = XR_OWN_RELEASED;
                emit = true;
            } else if (operation->opcode == XI_SOURCE_MOVE) {
                emit = true;
            } else {
                bool stored = operation->ownership_use == XI_GEN_OWN_USE_STORED_VALUE && a > 0;
                bool consumed = operand->ownership_action == XR_SEM_OPERAND_CONSUME;
                if (operation->ownership_use == XI_GEN_OWN_USE_PASS ||
                    (operation->result_alias_operand == (int16_t) a &&
                     audit_owner_for_value(audit, operation->result_value) == owner))
                    consumed = false;
                bool published = stored || operation->opcode == XI_SET_SHARED ||
                                 operation->opcode == XI_SET_GLOBAL ||
                                 operation->opcode == XI_STORE_UPVAL;
                if (published)
                    audit->flags[owner] |= 1u;
                if (consumed) {
                    uint32_t producer = operand->value < audit->value_count
                                            ? audit->producer_by_value[operand->value]
                                            : XR_SEMANTIC_INDEX_NONE;
                    bool stack_destroy =
                        producer != XR_SEMANTIC_INDEX_NONE &&
                        audit->plan->operations[producer].opcode == XI_STACK_ALLOC &&
                        (operation->opcode == XI_PAR_FOR || operation->opcode == XI_PAR_MAP ||
                         operation->opcode == XI_PAR_REDUCE);
                    kind = published ? XR_OWN_EVENT_PUBLISH
                                     : (stack_destroy ? XR_OWN_EVENT_DESTROY
                                                      : XR_OWN_EVENT_MOVE);
                    delta = -1;
                    state = published ? XR_OWN_PUBLISHED_SHARED
                                      : (stack_destroy ? XR_OWN_RELEASED : XR_OWN_MOVED);
                    emit = true;
                }
            }
            if (emit && !audit_add_event(audit, owner, i, operation->block,
                                         XR_SEMANTIC_INDEX_NONE, kind, delta, state,
                                         XR_OWN_POINT_AFTER_OPERATION))
                return false;
            if ((operation->effects & XI_EFFECT_MAY_SUSPEND) != 0 &&
                !audit_add_event(audit, owner, i, operation->block, XR_SEMANTIC_INDEX_NONE,
                                 XR_OWN_EVENT_SUSPEND, 0, XR_OWN_FRAME_OWNED,
                                 XR_OWN_POINT_AFTER_OPERATION))
                return false;
        }
    }
    return true;
}

static bool audit_borrowed_phi_parameter(XrOwnershipAudit *audit, uint32_t owner,
                                         int16_t *parameter) {
    *parameter = -1;
    uint32_t value = audit->certificate->owners[owner].origin_value;
    uint32_t origin = value < audit->value_count ? audit->producer_by_value[value]
                                                 : XR_SEMANTIC_INDEX_NONE;
    if (origin == XR_SEMANTIC_INDEX_NONE || audit->plan->operations[origin].opcode != XI_PHI)
        return false;
    const XrSemanticOperationRecord *phi = &audit->plan->operations[origin];
    bool saw_parameter = false;
    for (uint16_t a = 0; a < phi->operand_count; a++) {
        uint32_t source = audit_owner_for_value(
            audit, audit->plan->operands[phi->operand_begin + a].value);
        if (source == owner || source == XR_SEMANTIC_INDEX_NONE)
            continue;
        int16_t candidate = -1;
        if (!audit_owner_has_function_loan(audit, source, phi->function, &candidate))
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

static bool audit_owner_summaries(XrOwnershipAudit *audit, char *error, size_t error_size) {
    for (uint32_t f = 0; f < audit->plan->function_count; f++) {
        const XrSemanticFunctionRecord *function = &audit->plan->functions[f];
        for (uint32_t b = function->block_begin; b < function->block_begin + function->block_count;
             b++) {
            const XrSemanticBlockRecord *block = &audit->plan->blocks[b];
            if (block->kind != XI_BLOCK_RETURN || block->control_value == XR_SEMANTIC_INDEX_NONE)
                continue;
            uint32_t owner = audit_owner_for_value(audit, block->control_value);
            if (owner == XR_SEMANTIC_INDEX_NONE)
                continue;
            int16_t loan_parameter = -1;
            if (audit->initial_state[owner] == XR_OWN_BORROWED &&
                audit_borrowed_phi_parameter(audit, owner, &loan_parameter) &&
                (function->return_provenance != XR_SEM_RETURN_BORROWED_PARAM ||
                 function->return_parameter != loan_parameter))
                return report(error, error_size, "XR_OWN_3004",
                              "returned borrowed PHI disagrees with its parameter loan");
            audit->flags[owner] |= 2u;
            audit->return_provenance[owner] = function->return_provenance;
            audit->exit_state[owner] = function->return_provenance == XR_SEM_RETURN_OWNED
                                           ? XR_OWN_MOVED
                                           : XR_OWN_BORROWED;
        }
    }
    for (uint32_t owner = 0; owner < audit->certificate->owner_count; owner++) {
        const XrOwnershipOwnerRecord *actual = &audit->certificate->owners[owner];
        if (actual->initial_state != audit->initial_state[owner] ||
            actual->exit_state != audit->exit_state[owner] ||
            actual->return_provenance != audit->return_provenance[owner] ||
            actual->flags != audit->flags[owner])
            return report(error, error_size, "XR_OWN_3002",
                          "ownership owner state summary is not derived from semantic facts");
    }
    return true;
}

static bool audit_event_is_direct(const XrOwnershipAudit *audit,
                                  const XrOwnershipEventRecord *event) {
    if (event->program_point == XR_OWN_POINT_BLOCK_EXIT)
        return false;
    if (event->program_point != XR_OWN_POINT_EDGE || event->kind != XR_OWN_EVENT_RELEASE ||
        event->logical_delta >= 0 || event->owner >= audit->certificate->owner_count)
        return true;
    const XrOwnershipOwnerRecord *owner = &audit->certificate->owners[event->owner];
    uint32_t producer = owner->origin_value < audit->value_count
                            ? audit->producer_by_value[owner->origin_value]
                            : XR_SEMANTIC_INDEX_NONE;
    return producer == XR_SEMANTIC_INDEX_NONE ||
           audit->plan->operations[producer].opcode == XI_PHI ||
           event->successor != audit->plan->operations[producer].block;
}

static bool audit_direct_event_coverage(XrOwnershipAudit *audit, char *error,
                                        size_t error_size) {
    uint32_t actual_count = 0;
    for (uint32_t i = 0; i < audit->certificate->event_count; i++)
        if (audit_event_is_direct(audit, &audit->certificate->events[i]))
            actual_count++;
    if (actual_count != audit->event_count)
        return report(error, error_size, "XR_OWN_3002",
                      "direct ownership event coverage is not exact");
    XrOwnershipExpectedEvent *actual =
        (XrOwnershipExpectedEvent *) xr_malloc((size_t) actual_count * sizeof(*actual));
    if (actual_count && !actual)
        return report(error, error_size, "XR_EXEC_5003",
                      "ownership direct-event audit allocation failed");
    uint32_t at = 0;
    for (uint32_t i = 0; i < audit->certificate->event_count; i++) {
        const XrOwnershipEventRecord *event = &audit->certificate->events[i];
        if (!audit_event_is_direct(audit, event))
            continue;
        actual[at++] = (XrOwnershipExpectedEvent) {
            .owner = event->owner,
            .operation = event->operation,
            .block = event->block,
            .successor = event->successor,
            .logical_delta = event->logical_delta,
            .kind = event->kind,
            .state_after = event->state_after,
            .program_point = event->program_point,
        };
    }
    if (actual_count) {
        qsort(actual, actual_count, sizeof(*actual), compare_expected_event);
        qsort(audit->events, audit->event_count, sizeof(*audit->events), compare_expected_event);
    }
    bool exact = true;
    for (uint32_t i = 0; exact && i < actual_count; i++)
        exact = compare_expected_event(&actual[i], &audit->events[i]) == 0;
    xr_free(actual);
    return exact || report(error, error_size, "XR_OWN_3002",
                           "direct ownership events disagree with semantic operation facts");
}

static bool check_semantic_ownership_facts(const XrSemanticPlan *plan,
                                           const XrOwnershipCertificate *certificate,
                                           char *error, size_t error_size) {
    XrOwnershipAudit audit = {.plan = plan, .certificate = certificate};
    uint64_t event_capacity = (uint64_t) plan->operation_count +
                              (uint64_t) plan->operand_count * 2u;
    if (event_capacity > XR_OWNERSHIP_AUDIT_MAX_EVENTS) {
        report(error, error_size, "XR_EXEC_5003",
               "ownership direct-event audit exceeds its hard budget");
        return false;
    }
    audit.event_capacity = (uint32_t) event_capacity;
    audit.events = (XrOwnershipExpectedEvent *) xr_malloc(
        (size_t) audit.event_capacity * sizeof(*audit.events));
    bool valid = (!audit.event_capacity || audit.events) &&
                 audit_prepare_equivalence(&audit, error, error_size) &&
                 audit_canonical_owners(&audit, error, error_size) &&
                 audit_definition_events(&audit) &&
                 audit_operand_events(&audit, error, error_size) &&
                 audit_owner_summaries(&audit, error, error_size) &&
                 audit_direct_event_coverage(&audit, error, error_size);
    if (!valid && (!error || !error_size || !error[0]))
        report(error, error_size, "XR_EXEC_5003",
               "ownership semantic-fact audit exhausted its bounded workspace");
    audit_dispose(&audit);
    return valid;
}

static uint32_t edge_owner_lower_bound(const XrOwnershipCertificate *certificate,
                                       uint32_t owner) {
    uint32_t low = 0;
    uint32_t high = certificate->edge_state_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (certificate->edge_states[middle].owner < owner)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

static uint32_t owner_origin_operation(const XrSemanticPlan *plan,
                                       const XrOwnershipOwnerRecord *owner) {
    uint32_t found = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t operation = 0; operation < plan->operation_count; operation++) {
        const XrSemanticOperationRecord *record = &plan->operations[operation];
        if (record->function != owner->function || record->result_value != owner->origin_value)
            continue;
        if (found != XR_SEMANTIC_INDEX_NONE)
            return XR_SEMANTIC_INDEX_NONE;
        found = operation;
    }
    return found;
}

static bool build_owner_scope(const XrSemanticPlan *plan,
                              const XrOwnershipCertificate *certificate,
                              uint32_t owner_index, uint8_t **out_scope, char *error,
                              size_t error_size) {
    *out_scope = NULL;
    const XrOwnershipOwnerRecord *owner = &certificate->owners[owner_index];
    if (owner->function >= plan->function_count)
        return report(error, error_size, "XR_OWN_3002", "owner function index is invalid");
    const XrSemanticFunctionRecord *function = &plan->functions[owner->function];
    uint32_t origin_operation = owner_origin_operation(plan, owner);
    if (origin_operation == XR_SEMANTIC_INDEX_NONE)
        return report(error, error_size, "XR_OWN_3002", "owner origin is not unique");
    uint32_t origin_block = plan->operations[origin_operation].block;
    if (origin_block < function->block_begin ||
        origin_block >= function->block_begin + function->block_count)
        return report(error, error_size, "XR_OWN_3002",
                      "owner origin has no in-function value definition");
    uint8_t *scope = NULL;
    uint32_t *queue = NULL;
    if (function->block_count) {
        scope = (uint8_t *) xr_calloc(function->block_count, sizeof(*scope));
        queue = (uint32_t *) xr_malloc((size_t) function->block_count * sizeof(*queue));
    }
    if (function->block_count && (!scope || !queue)) {
        xr_free(scope);
        xr_free(queue);
        return report(error, error_size, "XR_EXEC_5003",
                      "ownership scope budget exhausted");
    }
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t origin_local = origin_block - function->block_begin;
    scope[origin_local] = 1;
    queue[tail++] = origin_local;
    while (head < tail) {
        uint32_t local = queue[head++];
        const XrSemanticBlockRecord *block =
            &plan->blocks[function->block_begin + local];
        for (unsigned successor_index = 0; successor_index < 2; successor_index++) {
            uint32_t successor = block->successors[successor_index];
            if (successor == XR_SEMANTIC_INDEX_NONE ||
                (successor_index == 1 && successor == block->successors[0]))
                continue;
            if (successor < function->block_begin ||
                successor >= function->block_begin + function->block_count) {
                xr_free(scope);
                xr_free(queue);
                return report(error, error_size, "XR_OWN_3002",
                              "ownership CFG edge crosses function");
            }
            uint32_t next = successor - function->block_begin;
            if (!scope[next]) {
                scope[next] = 1;
                queue[tail++] = next;
            }
        }
    }
    xr_free(queue);
    *out_scope = scope;
    return true;
}

static bool check_owner_dataflow(const XrSemanticPlan *plan, uint32_t owner_index, char *error,
                                 size_t error_size) {
    const XrOwnershipCertificate *certificate = plan->ownership;
    const XrOwnershipOwnerRecord *owner = &certificate->owners[owner_index];
    if (owner->function >= plan->function_count)
        return report(error, error_size, "XR_OWN_3002", "owner function index is invalid");
    const XrSemanticFunctionRecord *function = &plan->functions[owner->function];
    uint32_t origin_operation = owner_origin_operation(plan, owner);
    if (origin_operation == XR_SEMANTIC_INDEX_NONE)
        return report(error, error_size, "XR_OWN_3002", "owner origin is not unique");
    uint32_t origin_block = plan->operations[origin_operation].block;
    bool origin_is_phi = plan->operations[origin_operation].opcode == XI_PHI;
    uint8_t *scope = NULL;
    if (!build_owner_scope(plan, certificate, owner_index, &scope, error, error_size))
        return false;
    int32_t *delta = (int32_t *) xr_calloc(function->block_count, sizeof(*delta));
    int32_t *edge_delta =
        (int32_t *) xr_calloc((size_t) function->block_count * 2u, sizeof(*edge_delta));
    if (function->block_count && (!delta || !edge_delta)) {
        xr_free(scope);
        xr_free(delta);
        xr_free(edge_delta);
        return report(error, error_size, "XR_EXEC_5003", "checker dataflow budget exhausted");
    }
    for (uint32_t e = 0; e < certificate->event_count; e++) {
        const XrOwnershipEventRecord *event = &certificate->events[e];
        if (event->owner != owner_index)
            continue;
        if (event->block < function->block_begin ||
            event->block >= function->block_begin + function->block_count) {
            xr_free(scope);
            xr_free(delta);
            xr_free(edge_delta);
            return report(error, error_size, "XR_OWN_3002", "owner event crosses function");
        }
        uint32_t local = event->block - function->block_begin;
        if (event->successor == XR_SEMANTIC_INDEX_NONE) {
            if (!add_i32_checked(delta[local], event->logical_delta, &delta[local])) {
                xr_free(scope);
                xr_free(delta);
                xr_free(edge_delta);
                return report(error, error_size, "XR_OWN_3001",
                              "ownership event balance exceeds the certificate schema");
            }
        } else {
            const XrSemanticBlockRecord *block = &plan->blocks[event->block];
            uint32_t edge_index = UINT32_MAX;
            if (block->successors[0] == event->successor)
                edge_index = local * 2u;
            else if (block->successors[1] == event->successor)
                edge_index = local * 2u + 1u;
            else {
                xr_free(scope);
                xr_free(delta);
                xr_free(edge_delta);
                return report(error, error_size, "XR_OWN_3002",
                              "owner event references a non-CFG edge");
            }
            if (!add_i32_checked(edge_delta[edge_index], event->logical_delta,
                                 &edge_delta[edge_index])) {
                xr_free(scope);
                xr_free(delta);
                xr_free(edge_delta);
                return report(error, error_size, "XR_OWN_3001",
                              "ownership edge balance exceeds the certificate schema");
            }
        }
    }
    uint32_t expected_edge_count = 0;
    for (uint32_t local = 0; local < function->block_count; local++) {
        uint32_t block_index = function->block_begin + local;
        const XrSemanticBlockRecord *block = &plan->blocks[block_index];
        bool terminal = block->successors[0] == XR_SEMANTIC_INDEX_NONE &&
                        block->successors[1] == XR_SEMANTIC_INDEX_NONE;
        const XrOwnershipEdgeStateRecord *block_state =
            find_block_state(certificate, owner_index, block_index);
        for (unsigned s = 0; s < 2; s++) {
            uint32_t successor = terminal ? XR_SEMANTIC_INDEX_NONE : block->successors[s];
            if (!terminal && successor == XR_SEMANTIC_INDEX_NONE)
                continue;
            if (!terminal && s == 1 && successor == block->successors[0])
                continue;
            expected_edge_count++;
            const XrOwnershipEdgeStateRecord *edge =
                find_edge(certificate, owner_index, block_index, successor);
            int32_t expected_delta = 0;
            if (!add_i32_checked(delta[local], terminal ? 0 : edge_delta[local * 2u + s],
                                  &expected_delta)) {
                xr_free(scope);
                xr_free(delta);
                xr_free(edge_delta);
                return report(error, error_size, "XR_OWN_3001",
                              "ownership edge delta exceeds the checker schema");
            }
            bool owner_frontier = !scope[local] && !terminal && origin_is_phi &&
                                  successor == origin_block &&
                                  expected_delta ==
                                      (owner->initial_state == XR_OWN_BORROWED ? 0 : 1);
            uint16_t expected_flags = owner_frontier
                                          ? XR_OWN_EDGE_OWNER_FRONTIER
                                          : (scope[local] ? 0u : XR_OWN_EDGE_OUT_OF_SCOPE);
            if (!edge || edge->flags != expected_flags) {
                xr_free(scope);
                xr_free(delta);
                xr_free(edge_delta);
                return report(error, error_size, "XR_OWN_3002",
                               "certificate edge scope is not exact");
            }
            if (!block_state ||
                ((edge->flags & XR_OWN_EDGE_OWNER_FRONTIER) == 0 &&
                 (block_state->flags & XR_OWN_EDGE_OWNER_FRONTIER) == 0 &&
                 (edge->flags != block_state->flags ||
                  edge->entry_balance != block_state->entry_balance ||
                  edge->entry_state != block_state->entry_state))) {
                xr_free(scope);
                xr_free(delta);
                xr_free(edge_delta);
                return report(error, error_size, "XR_OWN_3001",
                              "ownership block has inconsistent outgoing entry state");
            }
            if (expected_flags == XR_OWN_EDGE_OUT_OF_SCOPE) {
                if (edge->entry_balance != 0 || edge->exit_balance != 0 ||
                    edge->entry_state != XR_OWN_UNINITIALIZED ||
                    edge->exit_state != XR_OWN_UNINITIALIZED) {
                    xr_free(scope);
                    xr_free(delta);
                    xr_free(edge_delta);
                    return report(error, error_size, "XR_OWN_3002",
                                  "out-of-scope ownership edge carries state");
                }
                if (terminal)
                    break;
                continue;
            }
            if (expected_flags == XR_OWN_EDGE_OWNER_FRONTIER &&
                (edge->entry_balance != 0 ||
                 edge->exit_balance !=
                     (owner->initial_state == XR_OWN_BORROWED ? 0 : 1) ||
                 edge->entry_state != XR_OWN_UNINITIALIZED ||
                 edge->exit_state !=
                     (owner->initial_state == XR_OWN_BORROWED ? XR_OWN_BORROWED
                                                              : XR_OWN_OWNED_LOCAL))) {
                xr_free(scope);
                xr_free(delta);
                xr_free(edge_delta);
                return report(error, error_size, "XR_OWN_3002",
                              "PHI owner frontier has a non-canonical loan/token state");
            }
            if (expected_flags == XR_OWN_EDGE_OWNER_FRONTIER) {
                const XrOwnershipEdgeStateRecord *next =
                    find_block_state(certificate, owner_index, successor);
                if (!next || next->flags == XR_OWN_EDGE_OUT_OF_SCOPE ||
                    next->entry_balance != edge->exit_balance) {
                    xr_free(scope);
                    xr_free(delta);
                    xr_free(edge_delta);
                    return report(error, error_size, "XR_OWN_3001",
                                  "PHI owner frontier does not reach the PHI entry state");
                }
                continue;
            }
            int64_t actual_delta =
                (int64_t) edge->exit_balance - (int64_t) edge->entry_balance;
            if (actual_delta != expected_delta ||
                edge->entry_state != state_for_balance(owner, edge->entry_balance) ||
                edge->exit_state != state_for_balance(owner, edge->exit_balance)) {
                xr_free(scope);
                xr_free(delta);
                xr_free(edge_delta);
                return report(error, error_size, "XR_OWN_3001",
                              "certificate edge state does not match independently summed events");
            }
            if ((edge->flags & 1u) == 0 && edge->exit_balance < 0) {
                xr_free(scope);
                xr_free(delta);
                xr_free(edge_delta);
                return report(error, error_size, "XR_OWN_3001", "certificate balance is negative");
            }
            if (terminal && (edge->flags & 1u) == 0 && edge->exit_balance != 0) {
                xr_free(scope);
                xr_free(delta);
                xr_free(edge_delta);
                return report(error, error_size, "XR_OWN_3001",
                              "certificate ownership is not balanced at a function exit");
            }
            if (!terminal) {
                const XrOwnershipEdgeStateRecord *next =
                    find_block_state(certificate, owner_index, successor);
                /* Ownership dataflow begins at the owner's defining
                 * operation, not at function entry.  CFG edges that precede
                 * that definition are intentionally marked out-of-scope and
                 * do not constrain the definition block's seeded state. */
                if (edge->flags != XR_OWN_EDGE_OUT_OF_SCOPE &&
                    (!next || next->flags == XR_OWN_EDGE_OUT_OF_SCOPE ||
                     next->entry_balance != edge->exit_balance)) {
                    if (error && error_size) {
                        snprintf(error, error_size,
                                 "XR_OWN_3001: certificate balance is inconsistent across a "
                                 "CFG edge (owner=%s from=%u to=%u exit=%d next-entry=%d "
                                 "next-flags=%u)",
                                 owner->canonical_key, block_index, successor, edge->exit_balance,
                                 next ? next->entry_balance : INT32_MIN,
                                 next ? next->flags : UINT32_MAX);
                    }
                    xr_free(scope);
                    xr_free(delta);
                    xr_free(edge_delta);
                    return false;
                }
            }
            if (terminal)
                break;
        }
    }
    uint32_t owner_edge_begin = edge_owner_lower_bound(certificate, owner_index);
    uint32_t owner_edge_end = edge_owner_lower_bound(certificate, owner_index + 1u);
    if (owner_edge_end - owner_edge_begin != expected_edge_count) {
        xr_free(scope);
        xr_free(delta);
        xr_free(edge_delta);
        return report(error, error_size, "XR_OWN_3002",
                      "ownership edge-state coverage is not exact");
    }
    xr_free(scope);
    xr_free(delta);
    xr_free(edge_delta);
    return true;
}

static bool check_loop_invariants(const XrSemanticPlan *plan, const XrSemanticGraph *graph,
                                  char *error, size_t error_size) {
    const XrOwnershipCertificate *certificate = plan->ownership;
    for (uint32_t i = 0; i < certificate->loop_invariant_count; i++) {
        const XrOwnershipLoopInvariantRecord *invariant = &certificate->loop_invariants[i];
        if (invariant->owner >= certificate->owner_count || invariant->header >= plan->block_count ||
            invariant->backedge >= plan->block_count || invariant->state > XR_OWN_IMMORTAL ||
            invariant->reserved[0] != 0 || invariant->reserved[1] != 0 ||
            invariant->reserved[2] != 0)
            return report(error, error_size, "XR_OWN_3006",
                          "ownership loop invariant contains an invalid index or state");
        const XrOwnershipOwnerRecord *owner = &certificate->owners[invariant->owner];
        const XrSemanticBlockRecord *header = &plan->blocks[invariant->header];
        const XrSemanticBlockRecord *backedge = &plan->blocks[invariant->backedge];
        bool is_successor = backedge->successors[0] == invariant->header ||
                            backedge->successors[1] == invariant->header;
        XrStableId expected_id;
        XrFingerprint digest;
        if (header->function != owner->function || backedge->function != owner->function ||
            !is_successor ||
            !xr_semantic_graph_dominates(graph, invariant->header, invariant->backedge) ||
            !loop_invariant_key_valid(plan, certificate, invariant) ||
            !xr_stable_id_from_key(invariant->canonical_key, &expected_id, &digest) ||
            !xr_stable_id_equal(expected_id, invariant->id) ||
            (i > 0 && xr_stable_id_compare(certificate->loop_invariants[i - 1].id,
                                           invariant->id) >= 0))
            return report(error, error_size, "XR_OWN_3006",
                          "ownership loop invariant identity or CFG relation is invalid");
        const XrOwnershipEdgeStateRecord *edge =
            find_edge(certificate, invariant->owner, invariant->backedge, invariant->header);
        const XrOwnershipEdgeStateRecord *entry =
            find_block_state(certificate, invariant->owner, invariant->header);
        if (!edge || !entry || (edge->flags & 1u) != 0 || (entry->flags & 1u) != 0 ||
            edge->exit_balance != entry->entry_balance ||
            invariant->balance != entry->entry_balance ||
            invariant->state != state_for_balance(owner, entry->entry_balance))
            return report(error, error_size, "XR_OWN_3006",
                          "ownership loop invariant does not match independently checked dataflow");
    }

    XrOwnershipOwnerFunctionRef *owners = NULL;
    if (certificate->owner_count) {
        owners = (XrOwnershipOwnerFunctionRef *) xr_malloc(
            (size_t) certificate->owner_count * sizeof(*owners));
        if (!owners)
            return report(error, error_size, "XR_EXEC_5003",
                          "ownership loop-invariant index allocation failed");
    }
    for (uint32_t owner = 0; owner < certificate->owner_count; owner++) {
        owners[owner].function = certificate->owners[owner].function;
        owners[owner].owner = owner;
    }
    if (certificate->owner_count)
        qsort(owners, certificate->owner_count, sizeof(*owners), compare_owner_function_ref);
    uint32_t expected_count = 0;
    bool complete = true;
    for (uint32_t backedge = 0; complete && backedge < plan->block_count; backedge++) {
        const XrSemanticBlockRecord *block = &plan->blocks[backedge];
        uint32_t owner_begin =
            owner_function_lower_bound(owners, certificate->owner_count, block->function);
        for (unsigned successor_index = 0; complete && successor_index < 2; successor_index++) {
            uint32_t header = block->successors[successor_index];
            if (header == XR_SEMANTIC_INDEX_NONE ||
                (successor_index == 1 && header == block->successors[0]) ||
                !xr_semantic_graph_dominates(graph, header, backedge))
                continue;
            for (uint32_t position = owner_begin;
                 position < certificate->owner_count &&
                 owners[position].function == block->function;
                 position++) {
                uint32_t owner = owners[position].owner;
                const XrOwnershipEdgeStateRecord *edge =
                    find_edge(certificate, owner, backedge, header);
                const XrOwnershipEdgeStateRecord *entry =
                    find_block_state(certificate, owner, header);
                if (!edge || !entry) {
                    complete = false;
                    break;
                }
                if ((edge->flags & 1u) != 0 || (entry->flags & 1u) != 0)
                    continue;
                if (expected_count == certificate->loop_invariant_count ||
                    !find_loop_invariant(plan, certificate, owner, header, backedge)) {
                    complete = false;
                    break;
                }
                expected_count++;
            }
        }
    }
    xr_free(owners);
    if (!complete || expected_count != certificate->loop_invariant_count)
        return report(error, error_size, "XR_OWN_3006",
                      "ownership loop invariant coverage is incomplete or not exact");
    return true;
}

bool xr_ownership_certificate_check(const XrSemanticPlan *plan, const XrSemanticGraph *graph,
                                    char *error, size_t error_size) {
    if (!plan || !graph || !plan->frozen || !plan->ownership || !plan->ownership->frozen)
        return report(error, error_size, "XR_OWN_3001",
                      "ownership checker requires a frozen plan and certificate");
    const XrOwnershipCertificate *certificate = plan->ownership;
    if (certificate->schema != plan->schema ||
        !xr_fingerprint_equal(certificate->semantic_fingerprint, plan->fingerprint))
        return report(error, error_size, "XR_OWN_3001",
                      "ownership certificate premise fingerprint does not match the plan");
    if (certificate->loop_invariant_count > UINT32_C(40000000) ||
        (certificate->loop_invariant_count != 0 && !certificate->loop_invariants))
        return report(error, error_size, "XR_EXEC_5003",
                      "ownership loop-invariant table exceeds its hard budget");
    if (certificate->owner_count > UINT32_C(2000000) ||
        (certificate->owner_count != 0 && !certificate->owners) ||
        certificate->event_count > UINT32_C(20000000) ||
        (certificate->event_count != 0 && !certificate->events) ||
        certificate->edge_state_count > UINT32_C(40000000) ||
        (certificate->edge_state_count != 0 && !certificate->edge_states))
        return report(error, error_size, "XR_EXEC_5003",
                      "ownership certificate table exceeds its hard budget");
    if (!check_semantic_ownership_facts(plan, certificate, error, error_size))
        return false;
    for (uint32_t i = 0; i < certificate->event_count; i++) {
        const XrOwnershipEventRecord *event = &certificate->events[i];
        XrStableId expected;
        XrFingerprint digest;
        if (!event->canonical_key ||
            !xr_stable_id_from_key(event->canonical_key, &expected, &digest) ||
            !xr_stable_id_equal(expected, event->id) || event->kind > XR_OWN_EVENT_RETURN ||
            event->state_after > XR_OWN_IMMORTAL ||
            !event_program_point_valid(plan, certificate, event) ||
            !event_canonical_key_valid(plan, certificate, i))
            return report(error, error_size, "XR_OWN_3002",
                          "ownership event contains an invalid index or state");
    }
    for (uint32_t i = 0; i < certificate->edge_state_count; i++) {
        const XrOwnershipEdgeStateRecord *edge = &certificate->edge_states[i];
        if (edge->owner >= certificate->owner_count || edge->block >= plan->block_count)
            return report(error, error_size, "XR_OWN_3002",
                          "ownership edge state contains an invalid CFG edge");
        const XrSemanticBlockRecord *block = &plan->blocks[edge->block];
        bool terminal = block->successors[0] == XR_SEMANTIC_INDEX_NONE &&
                        block->successors[1] == XR_SEMANTIC_INDEX_NONE;
        if (block->function != certificate->owners[edge->owner].function ||
            edge->entry_state > XR_OWN_IMMORTAL || edge->exit_state > XR_OWN_IMMORTAL ||
            (edge->flags != 0 && edge->flags != XR_OWN_EDGE_OUT_OF_SCOPE &&
             edge->flags != XR_OWN_EDGE_OWNER_FRONTIER) ||
            (edge->successor == XR_SEMANTIC_INDEX_NONE) != terminal ||
            (!terminal && block->successors[0] != edge->successor &&
             block->successors[1] != edge->successor))
            return report(error, error_size, "XR_OWN_3002",
                          "ownership edge state contains a non-canonical CFG tuple");
    }
    if (!edge_states_are_ordered(certificate))
        return report(error, error_size, "XR_OWN_3002",
                      "ownership edge states are not in canonical order");
    for (uint32_t i = 0; i < certificate->owner_count; i++) {
        const XrOwnershipOwnerRecord *owner = &certificate->owners[i];
        XrStableId expected;
        XrFingerprint digest;
        if (!owner->canonical_key ||
            !xr_stable_id_from_key(owner->canonical_key, &expected, &digest) ||
            !xr_stable_id_equal(expected, owner->id) || owner->initial_state > XR_OWN_IMMORTAL ||
            owner->exit_state > XR_OWN_IMMORTAL ||
            !check_owner_dataflow(plan, i, error, error_size))
            return false;
    }
    return check_loop_invariants(plan, graph, error, error_size) &&
           xr_ownership_replay_check(plan, graph, error, error_size);
}
