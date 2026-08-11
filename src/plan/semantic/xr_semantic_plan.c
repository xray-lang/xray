/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_plan.c - Immutable target-neutral semantic plan storage
 */

#include "xr_semantic_plan_internal.h"
#include "xr_semantic_ops.h"
#include "../ownership/xr_ownership_certificate_internal.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include <stdio.h>
#include <string.h>

typedef struct XrSemanticIdKeyRef {
    XrStableId id;
    const char *key;
} XrSemanticIdKeyRef;

static void set_error(char *error, size_t size, const char *code, const char *detail) {
    if (error && size)
        snprintf(error, size, "%s: %s", code, detail);
}

static bool stable_id_is_zero(XrStableId id) {
    XrStableId zero = {{0}};
    return xr_stable_id_equal(id, zero);
}

static int compare_id_key_ref(const void *left, const void *right) {
    const XrSemanticIdKeyRef *a = (const XrSemanticIdKeyRef *) left;
    const XrSemanticIdKeyRef *b = (const XrSemanticIdKeyRef *) right;
    int order = xr_stable_id_compare(a->id, b->id);
    return order != 0 ? order : strcmp(a->key, b->key);
}

bool xr_semantic_plan_verify_identity_set(const XrSemanticPlan *plan, char *error,
                                          size_t error_size) {
    size_t count =
        (size_t) plan->entity_count + plan->type_count + plan->function_count + plan->block_count +
        plan->parameter_count + plan->capture_count + plan->operation_count * 2u +
        plan->edge_count +
        (plan->ownership ? (size_t) plan->ownership->owner_count + plan->ownership->event_count +
                               plan->ownership->loop_invariant_count
                         : 0u);
    XrSemanticIdKeyRef *refs = (XrSemanticIdKeyRef *) xr_calloc(count, sizeof(*refs));
    if (!refs) {
        set_error(error, error_size, "XR_EXEC_5003", "identity collision table allocation failed");
        return false;
    }
    size_t n = 0;
#define XR_ADD_ID_KEY(record_id, record_key)                                                       \
    do {                                                                                           \
        if (!stable_id_is_zero(record_id) && (record_key)) {                                       \
            refs[n].id = (record_id);                                                              \
            refs[n++].key = (record_key);                                                          \
        }                                                                                          \
    } while (0)
    for (uint32_t i = 0; i < plan->type_count; i++)
        XR_ADD_ID_KEY(plan->types[i].id, plan->types[i].canonical_key);
    for (uint32_t i = 0; i < plan->entity_count; i++)
        XR_ADD_ID_KEY(plan->entities[i].id, plan->entities[i].canonical_key);
    for (uint32_t i = 0; i < plan->function_count; i++)
        XR_ADD_ID_KEY(plan->functions[i].id, plan->functions[i].canonical_key);
    for (uint32_t i = 0; i < plan->parameter_count; i++)
        XR_ADD_ID_KEY(plan->parameters[i].id, plan->parameters[i].canonical_key);
    for (uint32_t i = 0; i < plan->capture_count; i++)
        XR_ADD_ID_KEY(plan->captures[i].id, plan->captures[i].canonical_key);
    for (uint32_t i = 0; i < plan->block_count; i++)
        XR_ADD_ID_KEY(plan->blocks[i].id, plan->blocks[i].canonical_key);
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        XR_ADD_ID_KEY(plan->operations[i].id, plan->operations[i].canonical_key);
        XR_ADD_ID_KEY(plan->operations[i].allocation_id, plan->operations[i].allocation_key);
    }
    for (uint32_t i = 0; i < plan->edge_count; i++)
        XR_ADD_ID_KEY(plan->edges[i].id, plan->edges[i].canonical_key);
    if (plan->ownership) {
        for (uint32_t i = 0; i < plan->ownership->owner_count; i++)
            XR_ADD_ID_KEY(plan->ownership->owners[i].id, plan->ownership->owners[i].canonical_key);
        for (uint32_t i = 0; i < plan->ownership->event_count; i++)
            XR_ADD_ID_KEY(plan->ownership->events[i].id, plan->ownership->events[i].canonical_key);
        for (uint32_t i = 0; i < plan->ownership->loop_invariant_count; i++)
            XR_ADD_ID_KEY(plan->ownership->loop_invariants[i].id,
                          plan->ownership->loop_invariants[i].canonical_key);
    }
#undef XR_ADD_ID_KEY
    qsort(refs, n, sizeof(*refs), compare_id_key_ref);
    for (size_t i = 1; i < n; i++) {
        if (!xr_stable_id_equal(refs[i - 1].id, refs[i].id))
            continue;
        bool different_keys = strcmp(refs[i - 1].key, refs[i].key) != 0;
        xr_free(refs);
        set_error(error, error_size, "XR_SEM_0003",
                  different_keys ? "stable identifiers map to different canonical keys"
                                 : "stable identifier and canonical key are duplicated");
        return false;
    }
    xr_free(refs);
    return true;
}

static void hash_bytes(XrSHA256Context *ctx, const void *bytes, size_t size) {
    uint8_t length[8];
    uint64_t width = (uint64_t) size;
    for (unsigned i = 0; i < sizeof(length); i++)
        length[i] = (uint8_t) (width >> (i * 8));
    xr_sha256_update(ctx, length, sizeof(length));
    xr_sha256_update(ctx, (const uint8_t *) bytes, size);
}

static void hash_u64(XrSHA256Context *ctx, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(ctx, bytes, sizeof(bytes));
}

static void hash_string(XrSHA256Context *ctx, const char *text) {
    const char *value = text ? text : "";
    size_t length = strlen(value);
    hash_u64(ctx, (uint64_t) length);
    hash_bytes(ctx, value, length);
}

void xr_semantic_plan_compute_fingerprint(const XrSemanticPlan *plan, XrFingerprint *out) {
    static const uint8_t domain[] = "xray-semantic-plan-v10\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1);
    hash_u64(&ctx, plan->schema);
    hash_bytes(&ctx, plan->operation_registry_fingerprint.bytes,
               sizeof(plan->operation_registry_fingerprint.bytes));
    hash_u64(&ctx, plan->type_count);
    hash_u64(&ctx, plan->function_count);
    hash_u64(&ctx, plan->parameter_count);
    hash_u64(&ctx, plan->capture_count);
    hash_u64(&ctx, plan->block_count);
    hash_u64(&ctx, plan->operation_count);
    hash_u64(&ctx, plan->edge_count);
    hash_u64(&ctx, plan->constant_count);
    hash_u64(&ctx, plan->entity_count);
    for (uint32_t i = 0; i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *entity = &plan->entities[i];
        hash_bytes(&ctx, entity->id.bytes, sizeof(entity->id.bytes));
        hash_string(&ctx, entity->canonical_key);
        hash_u64(&ctx, entity->parent);
        hash_u64(&ctx, entity->subject);
        hash_u64(&ctx, entity->ordinal);
        hash_u64(&ctx, entity->kind);
        hash_u64(&ctx, entity->subject_kind);
        hash_u64(&ctx, entity->flags);
    }
    for (uint32_t i = 0; i < plan->type_count; i++) {
        const XrSemanticTypeRecord *type = &plan->types[i];
        hash_bytes(&ctx, type->id.bytes, sizeof(type->id.bytes));
        hash_string(&ctx, type->canonical_key);
        hash_u64(&ctx, type->kind);
        hash_u64(&ctx, type->child_begin);
        hash_u64(&ctx, type->aggregate_extent);
        hash_u64(&ctx, type->aggregate_align);
        hash_u64(&ctx, type->child_count);
        hash_u64(&ctx, type->scalar_rep);
        hash_u64(&ctx, type->flags);
    }
    for (uint32_t i = 0; i < plan->type_child_count; i++)
        hash_u64(&ctx, plan->type_children[i]);
    for (uint32_t i = 0; i < plan->function_count; i++) {
        const XrSemanticFunctionRecord *function = &plan->functions[i];
        hash_bytes(&ctx, function->id.bytes, sizeof(function->id.bytes));
        hash_string(&ctx, function->canonical_key);
        hash_string(&ctx, function->name);
        hash_u64(&ctx, function->return_type);
        hash_u64(&ctx, function->parent);
        hash_u64(&ctx, function->parameter_begin);
        hash_u64(&ctx, function->parameter_count);
        hash_u64(&ctx, function->child_count);
        hash_u64(&ctx, function->capture_begin);
        hash_u64(&ctx, function->capture_count);
        hash_u64(&ctx, function->block_begin);
        hash_u64(&ctx, function->block_count);
        hash_u64(&ctx, function->value_begin);
        hash_u64(&ctx, function->value_count);
        hash_u64(&ctx, function->semantic_effects);
        hash_u64(&ctx, function->capability_mask);
        hash_u64(&ctx, (uint16_t) function->return_parameter);
        hash_u64(&ctx, function->return_provenance);
        hash_u64(&ctx, function->flags);
    }
    for (uint32_t i = 0; i < plan->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter = &plan->parameters[i];
        hash_bytes(&ctx, parameter->id.bytes, sizeof(parameter->id.bytes));
        hash_string(&ctx, parameter->canonical_key);
        hash_u64(&ctx, parameter->function);
        hash_u64(&ctx, parameter->type);
        hash_u64(&ctx, parameter->value);
        hash_u64(&ctx, parameter->ordinal);
        hash_u64(&ctx, parameter->mode);
        hash_u64(&ctx, parameter->ownership);
        hash_u64(&ctx, parameter->transfer_mode);
        hash_u64(&ctx, parameter->flags);
    }
    for (uint32_t i = 0; i < plan->capture_count; i++) {
        const XrSemanticCaptureRecord *capture = &plan->captures[i];
        hash_bytes(&ctx, capture->id.bytes, sizeof(capture->id.bytes));
        hash_string(&ctx, capture->canonical_key);
        hash_string(&ctx, capture->name);
        hash_u64(&ctx, capture->function);
        hash_u64(&ctx, capture->source_function);
        hash_u64(&ctx, capture->source_value);
        hash_u64(&ctx, capture->source_capture);
        hash_u64(&ctx, capture->type);
        hash_u64(&ctx, capture->source_type);
        hash_u64(&ctx, capture->source_index);
        hash_u64(&ctx, capture->ordinal);
        hash_u64(&ctx, capture->source);
        hash_u64(&ctx, capture->kind);
        hash_u64(&ctx, capture->storage_domain);
        hash_u64(&ctx, capture->value_capability);
        hash_u64(&ctx, capture->flags);
    }
    for (uint32_t i = 0; i < plan->block_count; i++) {
        const XrSemanticBlockRecord *block = &plan->blocks[i];
        hash_bytes(&ctx, block->id.bytes, sizeof(block->id.bytes));
        hash_string(&ctx, block->canonical_key);
        hash_u64(&ctx, block->function);
        hash_u64(&ctx, block->operation_begin);
        hash_u64(&ctx, block->operation_count);
        hash_u64(&ctx, block->predecessor_begin);
        hash_u64(&ctx, block->predecessor_count);
        hash_u64(&ctx, block->kind);
        hash_u64(&ctx, block->successors[0]);
        hash_u64(&ctx, block->successors[1]);
        hash_u64(&ctx, block->control_value);
        hash_u64(&ctx, block->source_line);
    }
    for (uint32_t i = 0; i < plan->predecessor_count; i++)
        hash_u64(&ctx, plan->predecessors[i]);
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        const XrSemanticOperationRecord *op = &plan->operations[i];
        hash_bytes(&ctx, op->id.bytes, sizeof(op->id.bytes));
        hash_bytes(&ctx, op->allocation_id.bytes, sizeof(op->allocation_id.bytes));
        hash_string(&ctx, op->canonical_key);
        hash_string(&ctx, op->allocation_key);
        hash_u64(&ctx, op->function);
        hash_u64(&ctx, op->block);
        hash_u64(&ctx, op->result_value);
        hash_u64(&ctx, op->result_type);
        hash_u64(&ctx, op->operand_begin);
        hash_u64(&ctx, op->operand_count);
        hash_u64(&ctx, op->opcode);
        hash_u64(&ctx, op->metadata_begin);
        hash_u64(&ctx, op->metadata_count);
        hash_u64(&ctx, op->auxiliary_kind);
        hash_u64(&ctx, op->effects);
        hash_u64(&ctx, op->source_line);
        hash_string(&ctx, op->source_file);
        hash_u64(&ctx, op->source_start_line);
        hash_u64(&ctx, op->source_start_column);
        hash_u64(&ctx, op->source_end_line);
        hash_u64(&ctx, op->source_end_column);
        hash_u64(&ctx, op->source_discriminator);
        hash_u64(&ctx, (uint64_t) op->semantic_immediate);
        hash_u64(&ctx, op->constant);
        for (unsigned e = 0; e < 8; e++)
            hash_u64(&ctx, op->evidence[e]);
        hash_u64(&ctx, op->ownership_use);
        hash_u64(&ctx, op->result_ownership);
        hash_u64(&ctx, op->transfer_mode);
        hash_u64(&ctx, op->parameter_mode);
        hash_u64(&ctx, op->parameter_ownership);
        hash_u64(&ctx, op->flags);
        hash_u64(&ctx, (uint16_t) op->result_alias_operand);
        hash_u64(&ctx, (uint16_t) op->return_parameter);
        hash_u64(&ctx, op->return_provenance);
        hash_u64(&ctx, op->return_complete);
    }
    for (uint32_t i = 0; i < plan->operand_count; i++) {
        const XrSemanticOperandRecord *operand = &plan->operands[i];
        hash_u64(&ctx, operand->value);
        hash_u64(&ctx, operand->type);
        hash_u64(&ctx, (uint16_t) operand->parameter);
        hash_u64(&ctx, operand->role);
        hash_u64(&ctx, operand->transfer_mode);
        hash_u64(&ctx, operand->ownership_action);
        hash_u64(&ctx, operand->parameter_mode);
        hash_u64(&ctx, operand->access);
        hash_u64(&ctx, operand->origin);
        hash_u64(&ctx, operand->lifetime);
        hash_u64(&ctx, operand->escape);
        hash_u64(&ctx, operand->flags);
    }
    for (uint32_t i = 0; i < plan->metadata_count; i++)
        hash_string(&ctx, plan->metadata[i]);
    for (uint32_t i = 0; i < plan->edge_count; i++) {
        const XrSemanticEdgeRecord *edge = &plan->edges[i];
        hash_bytes(&ctx, edge->id.bytes, sizeof(edge->id.bytes));
        hash_string(&ctx, edge->canonical_key);
        hash_u64(&ctx, edge->function);
        hash_u64(&ctx, edge->from_block);
        hash_u64(&ctx, edge->to_block);
        hash_u64(&ctx, edge->operation);
        hash_u64(&ctx, edge->kind);
        hash_u64(&ctx, edge->flags);
    }
    for (uint32_t i = 0; i < plan->constant_count; i++) {
        const XrSemanticConstantRecord *constant = &plan->constants[i];
        hash_u64(&ctx, constant->type);
        hash_u64(&ctx, constant->kind);
        hash_u64(&ctx, (uint64_t) constant->integer);
        hash_u64(&ctx, constant->float_bits);
        hash_string(&ctx, constant->string);
    }
    if (plan->ownership) {
        hash_u64(&ctx, plan->ownership->owner_count);
        hash_u64(&ctx, plan->ownership->event_count);
        hash_u64(&ctx, plan->ownership->edge_state_count);
        hash_u64(&ctx, plan->ownership->loop_invariant_count);
        for (uint32_t i = 0; i < plan->ownership->owner_count; i++) {
            const XrOwnershipOwnerRecord *owner = &plan->ownership->owners[i];
            hash_bytes(&ctx, owner->id.bytes, sizeof(owner->id.bytes));
            hash_string(&ctx, owner->canonical_key);
            hash_u64(&ctx, owner->function);
            hash_u64(&ctx, owner->origin_value);
            hash_u64(&ctx, owner->initial_state);
            hash_u64(&ctx, owner->exit_state);
            hash_u64(&ctx, owner->return_provenance);
            hash_u64(&ctx, owner->flags);
        }
        for (uint32_t i = 0; i < plan->ownership->event_count; i++) {
            const XrOwnershipEventRecord *event = &plan->ownership->events[i];
            hash_bytes(&ctx, event->id.bytes, sizeof(event->id.bytes));
            hash_string(&ctx, event->canonical_key);
            hash_u64(&ctx, event->owner);
            hash_u64(&ctx, event->operation);
            hash_u64(&ctx, event->block);
            hash_u64(&ctx, event->successor);
            hash_u64(&ctx, (uint16_t) event->logical_delta);
            hash_u64(&ctx, event->kind);
            hash_u64(&ctx, event->state_after);
            hash_u64(&ctx, event->program_point);
        }
        for (uint32_t i = 0; i < plan->ownership->edge_state_count; i++) {
            const XrOwnershipEdgeStateRecord *edge = &plan->ownership->edge_states[i];
            hash_u64(&ctx, edge->owner);
            hash_u64(&ctx, edge->block);
            hash_u64(&ctx, edge->successor);
            hash_u64(&ctx, (uint32_t) edge->entry_balance);
            hash_u64(&ctx, (uint32_t) edge->exit_balance);
            hash_u64(&ctx, edge->entry_state);
            hash_u64(&ctx, edge->exit_state);
            hash_u64(&ctx, edge->flags);
        }
        for (uint32_t i = 0; i < plan->ownership->loop_invariant_count; i++) {
            const XrOwnershipLoopInvariantRecord *invariant =
                &plan->ownership->loop_invariants[i];
            hash_bytes(&ctx, invariant->id.bytes, sizeof(invariant->id.bytes));
            hash_string(&ctx, invariant->canonical_key);
            hash_u64(&ctx, invariant->owner);
            hash_u64(&ctx, invariant->header);
            hash_u64(&ctx, invariant->backedge);
            hash_u64(&ctx, (uint32_t) invariant->balance);
            hash_u64(&ctx, invariant->state);
        }
    } else {
        hash_u64(&ctx, 0);
    }
    xr_sha256_final(&ctx, out->bytes);
}

XrSemanticPlan *xr_semantic_plan_create(void) {
    XrSemanticPlan *plan = (XrSemanticPlan *) xr_calloc(1, sizeof(*plan));
    if (plan) {
        atomic_init(&plan->references, 1);
        plan->schema = XR_SEMANTIC_SCHEMA_VERSION;
        xr_semantic_op_registry_fingerprint(&plan->operation_registry_fingerprint);
    }
    return plan;
}

char *xr_semantic_plan_copy_string(XrSemanticPlan *plan, const char *text) {
    if (!plan || plan->frozen || !text)
        return NULL;
    size_t length = strlen(text);
    char *copy = (char *) xr_malloc(length + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text, length + 1);
    if (plan->strings.count == plan->strings.capacity) {
        uint32_t capacity = plan->strings.capacity ? plan->strings.capacity * 2 : 32;
        char **items = (char **) xr_realloc(plan->strings.items, capacity * sizeof(*items));
        if (!items) {
            xr_free(copy);
            return NULL;
        }
        plan->strings.items = items;
        plan->strings.capacity = capacity;
    }
    plan->strings.items[plan->strings.count++] = copy;
    return copy;
}

void xr_semantic_plan_set_ownership(XrSemanticPlan *plan, XrOwnershipCertificate *ownership) {
    if (!plan || plan->frozen)
        return;
    xr_ownership_certificate_free(plan->ownership);
    plan->ownership = ownership;
}

bool xr_semantic_plan_freeze(XrSemanticPlan *plan, char *error, size_t error_size) {
    if (!plan || plan->frozen) {
        set_error(error, error_size, "XR_SEM_0004", "semantic plan is null or already frozen");
        return false;
    }
    if (!plan->ownership) {
        set_error(error, error_size, "XR_OWN_3001", "semantic plan has no ownership certificate");
        return false;
    }
    XrFingerprint current_registry;
    xr_semantic_op_registry_fingerprint(&current_registry);
    if (!xr_semantic_op_registry_verify(error, error_size))
        return false;
    if (!xr_fingerprint_equal(plan->operation_registry_fingerprint, current_registry)) {
        set_error(error, error_size, "XR_SEM_0017",
                  "operation registry fingerprint does not match the compiler");
        return false;
    }
    if (!xr_semantic_plan_verify_identity_set(plan, error, error_size))
        return false;
    xr_semantic_plan_compute_fingerprint(plan, &plan->fingerprint);
    plan->ownership->semantic_fingerprint = plan->fingerprint;
    plan->ownership->fingerprint = plan->fingerprint;
    plan->ownership->frozen = true;
    plan->frozen = true;
    return true;
}

XrSemanticPlan *xr_semantic_plan_retain(XrSemanticPlan *plan) {
    if (plan)
        atomic_fetch_add_explicit(&plan->references, 1, memory_order_relaxed);
    return plan;
}

void xr_semantic_plan_free(XrSemanticPlan *plan) {
    if (!plan)
        return;
    if (atomic_fetch_sub_explicit(&plan->references, 1, memory_order_acq_rel) != 1)
        return;
    for (uint32_t i = 0; i < plan->strings.count; i++)
        xr_free(plan->strings.items[i]);
    xr_free(plan->strings.items);
    xr_free(plan->types);
    xr_free(plan->functions);
    xr_free(plan->blocks);
    xr_free(plan->operations);
    xr_free(plan->edges);
    xr_free(plan->constants);
    xr_free(plan->entities);
    xr_free(plan->type_children);
    xr_free(plan->parameters);
    xr_free(plan->captures);
    xr_free(plan->predecessors);
    xr_free(plan->operands);
    xr_free(plan->metadata);
    xr_ownership_certificate_free(plan->ownership);
    xr_free(plan);
}

bool xr_semantic_plan_is_frozen(const XrSemanticPlan *plan) {
    return plan && plan->frozen;
}

bool xr_semantic_plan_is_verified(const XrSemanticPlan *plan) {
    return plan && plan->frozen && plan->verified;
}

uint32_t xr_semantic_plan_schema(const XrSemanticPlan *plan) {
    return plan ? plan->schema : 0;
}

XrFingerprint xr_semantic_plan_fingerprint(const XrSemanticPlan *plan) {
    XrFingerprint empty = {{0}};
    return plan ? plan->fingerprint : empty;
}

XrFingerprint xr_semantic_plan_operation_registry_fingerprint(const XrSemanticPlan *plan) {
    XrFingerprint zero = {{0}};
    return plan ? plan->operation_registry_fingerprint : zero;
}

#define XR_PLAN_COUNT_ACCESSOR(name, field)                                                        \
    size_t name(const XrSemanticPlan *plan) {                                                      \
        return plan ? plan->field : 0;                                                             \
    }
XR_PLAN_COUNT_ACCESSOR(xr_semantic_plan_type_count, type_count)
XR_PLAN_COUNT_ACCESSOR(xr_semantic_plan_function_count, function_count)
XR_PLAN_COUNT_ACCESSOR(xr_semantic_plan_parameter_count, parameter_count)
XR_PLAN_COUNT_ACCESSOR(xr_semantic_plan_capture_count, capture_count)
XR_PLAN_COUNT_ACCESSOR(xr_semantic_plan_block_count, block_count)
XR_PLAN_COUNT_ACCESSOR(xr_semantic_plan_operation_count, operation_count)
XR_PLAN_COUNT_ACCESSOR(xr_semantic_plan_edge_count, edge_count)
XR_PLAN_COUNT_ACCESSOR(xr_semantic_plan_constant_count, constant_count)
XR_PLAN_COUNT_ACCESSOR(xr_semantic_plan_entity_count, entity_count)
#undef XR_PLAN_COUNT_ACCESSOR

#define XR_PLAN_RECORD_ACCESSOR(name, type, field, count_field)                                    \
    const type *name(const XrSemanticPlan *plan, uint32_t index) {                                 \
        return plan && index < plan->count_field ? &plan->field[index] : NULL;                     \
    }
XR_PLAN_RECORD_ACCESSOR(xr_semantic_plan_type, XrSemanticTypeRecord, types, type_count)
XR_PLAN_RECORD_ACCESSOR(xr_semantic_plan_function, XrSemanticFunctionRecord, functions,
                        function_count)
XR_PLAN_RECORD_ACCESSOR(xr_semantic_plan_parameter, XrSemanticParameterRecord, parameters,
                        parameter_count)
XR_PLAN_RECORD_ACCESSOR(xr_semantic_plan_capture, XrSemanticCaptureRecord, captures, capture_count)
XR_PLAN_RECORD_ACCESSOR(xr_semantic_plan_block, XrSemanticBlockRecord, blocks, block_count)
XR_PLAN_RECORD_ACCESSOR(xr_semantic_plan_operation, XrSemanticOperationRecord, operations,
                        operation_count)
XR_PLAN_RECORD_ACCESSOR(xr_semantic_plan_edge, XrSemanticEdgeRecord, edges, edge_count)
XR_PLAN_RECORD_ACCESSOR(xr_semantic_plan_constant, XrSemanticConstantRecord, constants,
                        constant_count)
XR_PLAN_RECORD_ACCESSOR(xr_semantic_plan_entity, XrSemanticEntityRecord, entities, entity_count)
#undef XR_PLAN_RECORD_ACCESSOR

#define XR_PLAN_INDEX_ACCESSOR(name, field, count_field)                                           \
    const uint32_t *name(const XrSemanticPlan *plan, uint32_t *count) {                            \
        if (count)                                                                                 \
            *count = plan ? plan->count_field : 0;                                                 \
        return plan ? plan->field : NULL;                                                          \
    }
XR_PLAN_INDEX_ACCESSOR(xr_semantic_plan_type_children, type_children, type_child_count)
XR_PLAN_INDEX_ACCESSOR(xr_semantic_plan_predecessors, predecessors, predecessor_count)
#undef XR_PLAN_INDEX_ACCESSOR

const XrSemanticOperandRecord *xr_semantic_plan_operands(const XrSemanticPlan *plan,
                                                         uint32_t *count) {
    if (count)
        *count = plan ? plan->operand_count : 0;
    return plan ? plan->operands : NULL;
}

const char *const *xr_semantic_plan_metadata(const XrSemanticPlan *plan, uint32_t *count) {
    if (count)
        *count = plan ? plan->metadata_count : 0;
    return plan ? plan->metadata : NULL;
}

const XrOwnershipCertificate *xr_semantic_plan_ownership(const XrSemanticPlan *plan) {
    return plan ? plan->ownership : NULL;
}

static void dump_id(FILE *out, XrStableId id) {
    char hex[XR_STABLE_ID_BYTES * 2 + 1];
    xr_stable_id_hex(id, hex);
    fputs(hex, out);
}

static void dump_text(FILE *out, const char *text) {
    if (!text) {
        fputs("-", out);
        return;
    }
    size_t length = strlen(text);
    fprintf(out, "%zu:", length);
    for (size_t i = 0; i < length; i++)
        fprintf(out, "%02x", (unsigned char) text[i]);
}

bool xr_semantic_plan_dump_entity(const XrSemanticPlan *plan, XrStableId id, FILE *out) {
    if (!plan || !out || !plan->frozen)
        return false;
    uint32_t begin = 0;
    uint32_t end = plan->entity_count;
    while (begin < end) {
        uint32_t middle = begin + (end - begin) / 2u;
        int order = xr_stable_id_compare(plan->entities[middle].id, id);
        if (order < 0)
            begin = middle + 1u;
        else
            end = middle;
    }
    if (begin >= plan->entity_count || !xr_stable_id_equal(plan->entities[begin].id, id))
        return false;
    const XrSemanticEntityRecord *record = &plan->entities[begin];
    fputs("semantic-entity id=", out);
    dump_id(out, record->id);
    fputs(" key=", out);
    dump_text(out, record->canonical_key);
    fprintf(out, " kind=%u parent=%u subject=%u:%u ordinal=%u flags=%u",
            record->kind, record->parent, record->subject_kind, record->subject,
            record->ordinal, record->flags);
    if (record->subject_kind == XR_SEM_ENTITY_SUBJECT_OPERATION &&
        record->subject < plan->operation_count) {
        const XrSemanticOperationRecord *operation = &plan->operations[record->subject];
        fprintf(out, " source-line=%u", operation->source_line);
        if (operation->source_file) {
            fputs(" source-file=", out);
            dump_text(out, operation->source_file);
            fprintf(out, " source-span=%u:%u-%u:%u discriminator=%u",
                    operation->source_start_line, operation->source_start_column,
                    operation->source_end_line, operation->source_end_column,
                    operation->source_discriminator);
        }
    }
    fputc('\n', out);
    return !ferror(out);
}

bool xr_semantic_plan_dump(const XrSemanticPlan *plan, FILE *out) {
    if (!plan || !out)
        return false;
    char fingerprint[XR_FINGERPRINT_BYTES * 2 + 1];
    char registry_fingerprint[XR_FINGERPRINT_BYTES * 2 + 1];
    xr_fingerprint_hex(plan->fingerprint, fingerprint);
    xr_fingerprint_hex(plan->operation_registry_fingerprint, registry_fingerprint);
    fprintf(out, "semantic-plan schema=%u frozen=%u fingerprint=%s operation-registry=%s\n",
            plan->schema, plan->frozen ? 1u : 0u, fingerprint, registry_fingerprint);
    fprintf(out,
            "  types=%u functions=%u parameters=%u captures=%u blocks=%u operations=%u "
            "edges=%u constants=%u entities=%u\n",
            plan->type_count, plan->function_count, plan->parameter_count, plan->capture_count,
            plan->block_count, plan->operation_count, plan->edge_count, plan->constant_count,
            plan->entity_count);
    for (uint32_t i = 0; i < plan->entity_count; i++) {
        const XrSemanticEntityRecord *record = &plan->entities[i];
        fprintf(out, "  entity[%u] id=", i);
        dump_id(out, record->id);
        fputs(" key=", out);
        dump_text(out, record->canonical_key);
        fprintf(out, " kind=%u parent=%u subject=%u:%u ordinal=%u flags=%u\n", record->kind,
                record->parent, record->subject_kind, record->subject, record->ordinal,
                record->flags);
    }
    for (uint32_t i = 0; i < plan->type_count; i++) {
        const XrSemanticTypeRecord *record = &plan->types[i];
        fprintf(out, "  type[%u] id=", i);
        dump_id(out, record->id);
        fputs(" key=", out);
        dump_text(out, record->canonical_key);
        fprintf(out, " kind=%u scalar=%u flags=%u aggregate=%u:%u children=[", record->kind,
                record->scalar_rep, record->flags, record->aggregate_extent,
                record->aggregate_align);
        for (uint16_t c = 0; c < record->child_count; c++)
            fprintf(out, "%s%u", c ? "," : "", plan->type_children[record->child_begin + c]);
        fputs("]\n", out);
    }
    for (uint32_t i = 0; i < plan->function_count; i++) {
        const XrSemanticFunctionRecord *record = &plan->functions[i];
        fprintf(out, "  fn[%u] id=", i);
        dump_id(out, record->id);
        fputs(" key=", out);
        dump_text(out, record->canonical_key);
        fputs(" name=", out);
        dump_text(out, record->name);
        fprintf(out,
                " return=%u parent=%u children=%u captures=%u+%u blocks=%u+%u values=%u+%u "
                "effects=%u caps=%u return-provenance=%u:%d flags=%u params=[",
                record->return_type, record->parent, record->child_count, record->capture_begin,
                record->capture_count, record->block_begin, record->block_count,
                record->value_begin, record->value_count, record->semantic_effects,
                record->capability_mask, record->return_provenance, record->return_parameter,
                record->flags);
        for (uint16_t p = 0; p < record->parameter_count; p++)
            fprintf(out, "%s%u", p ? "," : "", record->parameter_begin + p);
        fputs("]\n", out);
    }
    for (uint32_t i = 0; i < plan->parameter_count; i++) {
        const XrSemanticParameterRecord *record = &plan->parameters[i];
        fprintf(out, "  parameter[%u] id=", i);
        dump_id(out, record->id);
        fputs(" key=", out);
        dump_text(out, record->canonical_key);
        fprintf(out,
                " fn=%u ordinal=%u type=%u value=%u mode=%u ownership=%u transfer=%u "
                "flags=%u\n",
                record->function, record->ordinal, record->type, record->value, record->mode,
                record->ownership, record->transfer_mode, record->flags);
    }
    for (uint32_t i = 0; i < plan->capture_count; i++) {
        const XrSemanticCaptureRecord *record = &plan->captures[i];
        fprintf(out, "  capture[%u] id=", i);
        dump_id(out, record->id);
        fputs(" key=", out);
        dump_text(out, record->canonical_key);
        fputs(" name=", out);
        dump_text(out, record->name);
        fprintf(out,
                " fn=%u source-fn=%u ordinal=%u source=%u:%u value=%u capture=%u type=%u/%u "
                "kind=%u domain=%u capability=%u flags=%u\n",
                record->function, record->source_function, record->ordinal, record->source,
                record->source_index, record->source_value, record->source_capture, record->type,
                record->source_type, record->kind, record->storage_domain, record->value_capability,
                record->flags);
    }
    for (uint32_t i = 0; i < plan->block_count; i++) {
        const XrSemanticBlockRecord *record = &plan->blocks[i];
        fprintf(out, "  block[%u] id=", i);
        dump_id(out, record->id);
        fputs(" key=", out);
        dump_text(out, record->canonical_key);
        fprintf(out, " fn=%u ops=%u+%u kind=%u successors=%u,%u control=%u line=%u preds=[",
                record->function, record->operation_begin, record->operation_count, record->kind,
                record->successors[0], record->successors[1], record->control_value,
                record->source_line);
        for (uint16_t p = 0; p < record->predecessor_count; p++)
            fprintf(out, "%s%u", p ? "," : "", plan->predecessors[record->predecessor_begin + p]);
        fputs("]\n", out);
    }
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        const XrSemanticOperationRecord *record = &plan->operations[i];
        fprintf(out, "  op[%u] id=", i);
        dump_id(out, record->id);
        fputs(" allocation-id=", out);
        dump_id(out, record->allocation_id);
        fputs(" key=", out);
        dump_text(out, record->canonical_key);
        fputs(" allocation-key=", out);
        dump_text(out, record->allocation_key);
        fprintf(out,
                " fn=%u block=%u result=%u:%u opcode=%u aux=%u immediate=%lld effects=%u "
                "line=%u constant=%u own=%u:%u transfer=%u param=%u:%u flags=%u "
                "alias=%d return=%u:%d:%u",
                record->function, record->block, record->result_value, record->result_type,
                record->opcode, record->auxiliary_kind, (long long) record->semantic_immediate,
                record->effects, record->source_line, record->constant, record->ownership_use,
                record->result_ownership, record->transfer_mode, record->parameter_mode,
                record->parameter_ownership, record->flags, record->result_alias_operand,
                record->return_provenance, record->return_parameter, record->return_complete);
        fputs(" source-file=", out);
        dump_text(out, record->source_file);
        fprintf(out, " source-span=%u:%u-%u:%u:%u evidence=[", record->source_start_line,
                record->source_start_column, record->source_end_line, record->source_end_column,
                record->source_discriminator);
        for (unsigned e = 0; e < 8; e++)
            fprintf(out, "%s%u", e ? "," : "", record->evidence[e]);
        fputs("] operands=[", out);
        for (uint16_t a = 0; a < record->operand_count; a++) {
            uint32_t cursor = record->operand_begin + a;
            const XrSemanticOperandRecord *operand = &plan->operands[cursor];
            fprintf(out, "%s%u:%u:%d:%u:%u:%u:%u:%u:%u:%u:%u:%u", a ? "," : "", operand->value,
                    operand->type, operand->parameter, operand->role, operand->transfer_mode,
                    operand->ownership_action, operand->parameter_mode, operand->access,
                    operand->origin, operand->lifetime, operand->escape, operand->flags);
        }
        fputs("] metadata=[", out);
        for (uint16_t m = 0; m < record->metadata_count; m++) {
            if (m)
                fputc(',', out);
            dump_text(out, plan->metadata[record->metadata_begin + m]);
        }
        fputs("]\n", out);
    }
    for (uint32_t i = 0; i < plan->edge_count; i++) {
        const XrSemanticEdgeRecord *record = &plan->edges[i];
        fprintf(out, "  edge[%u] id=", i);
        dump_id(out, record->id);
        fputs(" key=", out);
        dump_text(out, record->canonical_key);
        fprintf(out, " fn=%u kind=%u flags=%u from=%u to=%u operation=%u\n", record->function,
                record->kind, record->flags, record->from_block, record->to_block,
                record->operation);
    }
    for (uint32_t i = 0; i < plan->constant_count; i++) {
        const XrSemanticConstantRecord *record = &plan->constants[i];
        fprintf(out, "  const[%u] type=%u kind=%u integer=%lld float=%llu text=", i, record->type,
                record->kind, (long long) record->integer, (unsigned long long) record->float_bits);
        dump_text(out, record->string);
        fputc('\n', out);
    }
    if (plan->ownership) {
        const XrOwnershipCertificate *certificate = plan->ownership;
        fprintf(out,
                "  ownership owners=%u events=%u edge-states=%u loop-invariants=%u\n",
                certificate->owner_count, certificate->event_count, certificate->edge_state_count,
                certificate->loop_invariant_count);
        for (uint32_t i = 0; i < certificate->owner_count; i++) {
            const XrOwnershipOwnerRecord *record = &certificate->owners[i];
            fprintf(out, "  owner[%u] id=", i);
            dump_id(out, record->id);
            fputs(" key=", out);
            dump_text(out, record->canonical_key);
            fprintf(out, " fn=%u origin=%u initial=%u exit=%u return=%u flags=%u\n",
                    record->function, record->origin_value, record->initial_state,
                    record->exit_state, record->return_provenance, record->flags);
        }
        for (uint32_t i = 0; i < certificate->event_count; i++) {
            const XrOwnershipEventRecord *record = &certificate->events[i];
            fprintf(out, "  owner-event[%u] id=", i);
            dump_id(out, record->id);
            fputs(" key=", out);
            dump_text(out, record->canonical_key);
            fprintf(out,
                    " owner=%u operation=%u block=%u successor=%u delta=%d kind=%u state=%u "
                    "point=%u\n",
                    record->owner, record->operation, record->block, record->successor,
                    record->logical_delta, record->kind, record->state_after,
                    record->program_point);
        }
        for (uint32_t i = 0; i < certificate->edge_state_count; i++) {
            const XrOwnershipEdgeStateRecord *record = &certificate->edge_states[i];
            fprintf(out,
                    "  owner-edge[%u] owner=%u block=%u successor=%u balance=%d:%d "
                    "state=%u:%u flags=%u\n",
                    i, record->owner, record->block, record->successor, record->entry_balance,
                    record->exit_balance, record->entry_state, record->exit_state, record->flags);
        }
        for (uint32_t i = 0; i < certificate->loop_invariant_count; i++) {
            const XrOwnershipLoopInvariantRecord *record = &certificate->loop_invariants[i];
            fprintf(out, "  owner-loop[%u] id=", i);
            dump_id(out, record->id);
            fputs(" key=", out);
            dump_text(out, record->canonical_key);
            fprintf(out, " owner=%u header=%u backedge=%u balance=%d state=%u\n", record->owner,
                    record->header, record->backedge, record->balance, record->state);
        }
    }
    return !ferror(out);
}
