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
#include "xr_ownership_certificate_internal.h"
#include "../semantic/xr_semantic_plan_internal.h"
#include "../../base/xmalloc.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../shared/xr_param_mode.h"
#include <stdio.h>
#include <limits.h>
#include <string.h>

#define XR_OWNERSHIP_MAX_OWNERS UINT32_C(2000000)
#define XR_OWNERSHIP_MAX_EVENTS UINT32_C(20000000)
#define XR_OWNERSHIP_MAX_EDGE_STATES UINT32_C(40000000)

typedef struct XrOwnershipBuildContext {
    XrSemanticPlan *plan;
    XrOwnershipCertificate *certificate;
    uint32_t *parent;
    uint8_t *rank;
    uint32_t *owner_by_root;
    uint32_t value_count;
    char *error;
    size_t error_size;
} XrOwnershipBuildContext;

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

static bool type_is_ownership_root(const XrSemanticPlan *plan, uint32_t type_index) {
    return type_index < plan->type_count &&
           (plan->types[type_index].flags & XR_SEM_TYPE_OWNERSHIP_ROOT) != 0;
}

static uint32_t aliased_call_operand(const XrSemanticPlan *plan,
                                     const XrSemanticOperationRecord *operation) {
    if (operation->result_alias_operand < 0 ||
        (uint16_t) operation->result_alias_operand >= operation->operand_count)
        return XR_SEMANTIC_INDEX_NONE;
    return plan->operands[operation->operand_begin + (uint16_t) operation->result_alias_operand];
}

static bool build_equivalence_classes(XrOwnershipBuildContext *ctx) {
    for (uint32_t i = 0; i < ctx->plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &ctx->plan->operations[i];
        if (!type_is_ownership_root(ctx->plan, operation->result_type) ||
            operation->result_value >= ctx->value_count)
            continue;
        if ((operation->ownership_use == XI_GEN_OWN_USE_PASS && operation->opcode != XI_PHI) ||
            operation->opcode == XI_RETAIN || operation->opcode == XI_RELEASE) {
            for (uint16_t a = 0; a < operation->operand_count; a++) {
                uint32_t operand = ctx->plan->operands[operation->operand_begin + a];
                if (operand < ctx->value_count)
                    union_values(ctx, operation->result_value, operand);
            }
        } else if (operation->result_alias_operand >= 0 && operation->operand_count > 0) {
            uint32_t operand = ctx->plan->operands[operation->operand_begin +
                                                   (uint16_t) operation->result_alias_operand];
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
                         XrOwnershipEventKind kind, int16_t delta, XrOwnershipState state) {
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
    char owner_id[XR_STABLE_ID_BYTES * 2 + 1];
    char operation_id[XR_STABLE_ID_BYTES * 2 + 1];
    char key[224];
    uint32_t occurrence = 0;
    for (uint32_t i = 0; i < certificate->event_count; i++) {
        const XrOwnershipEventRecord *prior = &certificate->events[i];
        if (prior->owner == owner_index && prior->operation == operation_index &&
            prior->block == block && prior->successor == successor && prior->kind == (uint8_t) kind)
            occurrence++;
    }
    xr_stable_id_hex(certificate->owners[owner_index].id, owner_id);
    xr_stable_id_hex(ctx->plan->operations[operation_index].id, operation_id);
    int written = snprintf(key, sizeof(key), "ownership-event-v2:%s:%s:%u:%u:%u:%u", owner_id,
                           operation_id, block, successor, (unsigned) kind, occurrence);
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

static bool add_event_in_block(XrOwnershipBuildContext *ctx, uint32_t owner_index,
                               uint32_t operation_index, uint32_t block, XrOwnershipEventKind kind,
                               int16_t delta, XrOwnershipState state) {
    return add_event_at(ctx, owner_index, operation_index, block, XR_SEMANTIC_INDEX_NONE, kind,
                        delta, state);
}

static bool add_event_on_edge(XrOwnershipBuildContext *ctx, uint32_t owner_index,
                              uint32_t operation_index, uint32_t predecessor, uint32_t successor,
                              XrOwnershipEventKind kind, int16_t delta, XrOwnershipState state) {
    return add_event_at(ctx, owner_index, operation_index, predecessor, successor, kind, delta,
                        state);
}

static bool add_event(XrOwnershipBuildContext *ctx, uint32_t owner_index, uint32_t operation_index,
                      XrOwnershipEventKind kind, int16_t delta, XrOwnershipState state) {
    return add_event_in_block(ctx, owner_index, operation_index,
                              ctx->plan->operations[operation_index].block, kind, delta, state);
}

static uint32_t owner_for_value(XrOwnershipBuildContext *ctx, uint32_t value) {
    if (value >= ctx->value_count)
        return XR_SEMANTIC_INDEX_NONE;
    return ctx->owner_by_root[find_root(ctx, value)];
}

static bool ensure_owners(XrOwnershipBuildContext *ctx) {
    for (uint32_t i = 0; i < ctx->plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &ctx->plan->operations[i];
        if (!type_is_ownership_root(ctx->plan, operation->result_type) ||
            operation->result_value >= ctx->value_count)
            continue;
        uint32_t root = find_root(ctx, operation->result_value);
        if (ctx->owner_by_root[root] != XR_SEMANTIC_INDEX_NONE)
            continue;
        uint32_t ignored;
        if (!add_owner(ctx, root, operation, &ignored))
            return false;
    }
    return true;
}

static bool classify_definition(XrOwnershipBuildContext *ctx, uint32_t operation_index) {
    const XrSemanticOperationRecord *operation = &ctx->plan->operations[operation_index];
    if (!type_is_ownership_root(ctx->plan, operation->result_type))
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
    if (operation->ownership_use == XI_GEN_OWN_USE_PASS || operation->opcode == XI_RETAIN ||
        operation->opcode == XI_RELEASE || operation->result_alias_operand >= 0 ||
        (operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT &&
         operation->return_provenance != XI_RETURN_OWNERSHIP_OWNED))
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
        if (plan->operations[i].function == function && plan->operations[i].result_value == value)
            return i;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static bool add_operand_events(XrOwnershipBuildContext *ctx, uint32_t operation_index) {
    const XrSemanticOperationRecord *operation = &ctx->plan->operations[operation_index];
    if (operation->opcode == XI_PHI) {
        if (!type_is_ownership_root(ctx->plan, operation->result_type))
            return true;
        const XrSemanticBlockRecord *block = &ctx->plan->blocks[operation->block];
        if (operation->operand_count != block->predecessor_count)
            return fail(ctx, "XR_OWN_3002", "PHI ownership edge count is invalid");
        uint32_t result_owner = owner_for_value(ctx, operation->result_value);
        if (result_owner == XR_SEMANTIC_INDEX_NONE)
            return fail(ctx, "XR_OWN_3002", "reference PHI has no result owner");
        if (ctx->certificate->owners[result_owner].initial_state == XR_OWN_UNINITIALIZED)
            ctx->certificate->owners[result_owner].initial_state = XR_OWN_OWNED_LOCAL;
        for (uint16_t a = 0; a < operation->operand_count; a++) {
            uint32_t source_value = ctx->plan->operands[operation->operand_begin + a];
            uint32_t source_owner = owner_for_value(ctx, source_value);
            if (source_owner == result_owner)
                continue;
            uint32_t predecessor = ctx->plan->predecessors[block->predecessor_begin + a];
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
                                   operation->block, XR_OWN_EVENT_MOVE, 1,
                                   source_state == XR_OWN_IMMORTAL ? XR_OWN_IMMORTAL
                                                                   : XR_OWN_OWNED_LOCAL))
                return false;
        }
        return true;
    }
    for (uint16_t a = 0; a < operation->operand_count; a++) {
        uint32_t value = ctx->plan->operands[operation->operand_begin + a];
        uint32_t owner = owner_for_value(ctx, value);
        if (owner == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (ctx->certificate->owners[owner].initial_state == XR_OWN_IMMORTAL)
            continue;
        if (operation->opcode == XI_RETAIN) {
            if (!add_event(ctx, owner, operation_index, XR_OWN_EVENT_RETAIN, 1, XR_OWN_OWNED_LOCAL))
                return false;
        } else if (operation->opcode == XI_RELEASE) {
            if (!add_event(ctx, owner, operation_index, XR_OWN_EVENT_RELEASE, -1, XR_OWN_RELEASED))
                return false;
        } else if (operation->opcode == XI_SOURCE_MOVE) {
            if (!add_event(ctx, owner, operation_index, XR_OWN_EVENT_MOVE, 0, XR_OWN_MOVED))
                return false;
        } else {
            bool stored = operation->ownership_use == XI_GEN_OWN_USE_STORED_VALUE && a > 0;
            bool consumed = ctx->plan->operand_ownership_actions[operation->operand_begin + a] ==
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
                 * valid inert token: callers may apply the common owned ABI's
                 * release and the runtime operation is a no-op. */
                if (type_is_ownership_root(ctx->plan, function->return_type)) {
                    if (inferred_provenance == XR_SEM_RETURN_NONE ||
                        inferred_provenance == XR_SEM_RETURN_BORROWED_STATIC) {
                        inferred_provenance = XR_SEM_RETURN_BORROWED_STATIC;
                        inferred_parameter = -1;
                    } else if (inferred_provenance == XR_SEM_RETURN_OWNED) {
                        inferred_parameter = -1;
                    } else {
                        return fail(ctx, "XR_OWN_3000",
                                    "inline and borrowed-parameter return paths disagree");
                    }
                }
                continue;
            }
            XrOwnershipOwnerRecord *owner = &ctx->certificate->owners[owner_index];
            uint8_t provenance = function->return_provenance;
            int16_t parameter = function->return_parameter;
            int32_t balance = 0;
            for (uint32_t e = 0; e < ctx->certificate->event_count; e++) {
                if (ctx->certificate->events[e].owner == owner_index)
                    balance += ctx->certificate->events[e].logical_delta;
            }
            if (type_is_ownership_root(ctx->plan, function->return_type) &&
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
                }
            }
            if (type_is_ownership_root(ctx->plan, function->return_type) &&
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
            if (disposition_provenance == XR_SEM_RETURN_OWNED ||
                (disposition_provenance == XR_SEM_RETURN_BORROWED_STATIC && balance > 0)) {
                uint32_t operation = operation_for_value(ctx->plan, f, block->control_value);
                if (operation == XR_SEMANTIC_INDEX_NONE ||
                    !add_event_in_block(ctx, owner_index, operation, b, XR_OWN_EVENT_RETURN, -1,
                                        disposition_provenance == XR_SEM_RETURN_OWNED
                                            ? XR_OWN_MOVED
                                            : XR_OWN_IMMORTAL))
                    return fail(ctx, "XR_OWN_3002",
                                "returned reference has no semantic producer operation");
            }
        }
        if (type_is_ownership_root(ctx->plan, function->return_type)) {
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
                delta[local] += event->logical_delta;
                continue;
            }
            const XrSemanticBlockRecord *event_block = &ctx->plan->blocks[event->block];
            if (event_block->successors[0] == event->successor)
                edge_delta[local * 2u] += event->logical_delta;
            else if (event_block->successors[1] == event->successor)
                edge_delta[local * 2u + 1u] += event->logical_delta;
            else {
                xr_free(entry);
                xr_free(delta);
                xr_free(edge_delta);
                xr_free(queue);
                return fail(ctx, "XR_OWN_3002", "ownership event names a non-CFG edge");
            }
        }
        uint32_t head = 0, tail = 0;
        if (fn->block_count) {
            entry[0] = 0;
            queue[tail++] = 0;
        }
        while (head < tail) {
            uint32_t local = queue[head++];
            int32_t exit_balance = entry[local] + delta[local];
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
                if (ctx->error && ctx->error_size)
                    snprintf(ctx->error, ctx->error_size,
                             "XR_OWN_3001: ownership balance becomes negative "
                             "(owner=%s origin=%s func=%s block=%u entry=%d delta=%d)",
                             ctx->certificate->owners[owner].canonical_key, origin_op, fn->name,
                             fn->block_begin + local, entry[local], delta[local]);
                xr_free(entry);
                xr_free(delta);
                xr_free(edge_delta);
                xr_free(queue);
                return false;
            }
            const XrSemanticBlockRecord *block = &ctx->plan->blocks[fn->block_begin + local];
            for (unsigned s = 0; s < 2; s++) {
                uint32_t successor = block->successors[s];
                if (successor == XR_SEMANTIC_INDEX_NONE)
                    continue;
                if (successor < fn->block_begin || successor >= fn->block_begin + fn->block_count) {
                    xr_free(entry);
                    xr_free(delta);
                    xr_free(edge_delta);
                    xr_free(queue);
                    return fail(ctx, "XR_OWN_3002", "ownership edge crosses function boundary");
                }
                int32_t edge_exit_balance = exit_balance + edge_delta[local * 2u + s];
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
            int32_t balance = entry[local] + delta[local];
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
                edge->entry_balance = entry_balance;
                edge->exit_balance =
                    entry_balance + delta[local] + (terminal ? 0 : edge_delta[local * 2u + s]);
                edge->entry_state = ctx->certificate->owners[owner].initial_state;
                edge->exit_state = edge->exit_balance == 0 ? XR_OWN_RELEASED : edge->entry_state;
                edge->flags = entry[local] == INT32_MIN ? 1u : 0u;
                if (terminal)
                    break;
            }
        }
        xr_free(entry);
        xr_free(delta);
        xr_free(edge_delta);
        xr_free(queue);
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
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (!classify_definition(&ctx, i))
            goto failure;
    }
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (!add_operand_events(&ctx, i))
            goto failure;
    }
    if (!classify_returns(&ctx) || !build_edge_states(&ctx))
        goto failure;
    xr_free(ctx.parent);
    xr_free(ctx.rank);
    xr_free(ctx.owner_by_root);
    *out = ctx.certificate;
    return true;

failure:
    xr_free(ctx.parent);
    xr_free(ctx.rank);
    xr_free(ctx.owner_by_root);
    xr_ownership_certificate_free(ctx.certificate);
    return false;
}
