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

static bool verify_id_collisions(const XrSemanticPlan *plan, char *error, size_t error_size) {
    size_t count = (size_t) plan->type_count + plan->function_count + plan->block_count +
                   plan->operation_count * 2u;
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
    for (uint32_t i = 0; i < plan->function_count; i++)
        XR_ADD_ID_KEY(plan->functions[i].id, plan->functions[i].canonical_key);
    for (uint32_t i = 0; i < plan->block_count; i++)
        XR_ADD_ID_KEY(plan->blocks[i].id, plan->blocks[i].canonical_key);
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        XR_ADD_ID_KEY(plan->operations[i].id, plan->operations[i].canonical_key);
        XR_ADD_ID_KEY(plan->operations[i].allocation_id, plan->operations[i].allocation_key);
    }
#undef XR_ADD_ID_KEY
    qsort(refs, n, sizeof(*refs), compare_id_key_ref);
    for (size_t i = 1; i < n; i++) {
        if (!xr_stable_id_equal(refs[i - 1].id, refs[i].id))
            continue;
        if (strcmp(refs[i - 1].key, refs[i].key) != 0) {
            xr_free(refs);
            set_error(error, error_size, "XR_SEM_0003",
                      "stable identifiers map to different canonical keys");
            return false;
        }
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
    hash_bytes(ctx, value, strlen(value));
}

static void hash_plan(const XrSemanticPlan *plan, XrFingerprint *out) {
    static const uint8_t domain[] = "xray-semantic-plan-v1\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1);
    hash_u64(&ctx, plan->schema);
    hash_u64(&ctx, plan->type_count);
    hash_u64(&ctx, plan->function_count);
    hash_u64(&ctx, plan->block_count);
    hash_u64(&ctx, plan->operation_count);
    hash_u64(&ctx, plan->constant_count);
    for (uint32_t i = 0; i < plan->type_count; i++) {
        const XrSemanticTypeRecord *type = &plan->types[i];
        hash_bytes(&ctx, type->id.bytes, sizeof(type->id.bytes));
        hash_string(&ctx, type->canonical_key);
        hash_u64(&ctx, type->kind);
        hash_u64(&ctx, type->child_begin);
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
        hash_u64(&ctx, function->parameter_begin);
        hash_u64(&ctx, function->parameter_count);
        hash_u64(&ctx, function->child_count);
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
    for (uint32_t i = 0; i < plan->parameter_count; i++)
        hash_u64(&ctx, plan->parameters[i]);
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
        hash_u64(&ctx, plan->operands[i]);
        hash_u64(&ctx, plan->operand_transfer_modes[i]);
        hash_u64(&ctx, plan->operand_ownership_actions[i]);
        hash_u64(&ctx, plan->operand_contracts[i]);
    }
    for (uint32_t i = 0; i < plan->metadata_count; i++)
        hash_string(&ctx, plan->metadata[i]);
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
            hash_u64(&ctx, event->owner);
            hash_u64(&ctx, event->operation);
            hash_u64(&ctx, event->block);
            hash_u64(&ctx, event->successor);
            hash_u64(&ctx, (uint16_t) event->logical_delta);
            hash_u64(&ctx, event->kind);
            hash_u64(&ctx, event->state_after);
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
    if (!verify_id_collisions(plan, error, error_size))
        return false;
    hash_plan(plan, &plan->fingerprint);
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
    xr_free(plan->constants);
    xr_free(plan->type_children);
    xr_free(plan->parameters);
    xr_free(plan->predecessors);
    xr_free(plan->operands);
    xr_free(plan->operand_transfer_modes);
    xr_free(plan->operand_ownership_actions);
    xr_free(plan->operand_contracts);
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

#define XR_PLAN_COUNT_ACCESSOR(name, field)                                                        \
    size_t name(const XrSemanticPlan *plan) {                                                      \
        return plan ? plan->field : 0;                                                             \
    }
XR_PLAN_COUNT_ACCESSOR(xr_semantic_plan_type_count, type_count)
XR_PLAN_COUNT_ACCESSOR(xr_semantic_plan_function_count, function_count)
XR_PLAN_COUNT_ACCESSOR(xr_semantic_plan_block_count, block_count)
XR_PLAN_COUNT_ACCESSOR(xr_semantic_plan_operation_count, operation_count)
XR_PLAN_COUNT_ACCESSOR(xr_semantic_plan_constant_count, constant_count)
#undef XR_PLAN_COUNT_ACCESSOR

#define XR_PLAN_RECORD_ACCESSOR(name, type, field, count_field)                                    \
    const type *name(const XrSemanticPlan *plan, uint32_t index) {                                 \
        return plan && index < plan->count_field ? &plan->field[index] : NULL;                     \
    }
XR_PLAN_RECORD_ACCESSOR(xr_semantic_plan_type, XrSemanticTypeRecord, types, type_count)
XR_PLAN_RECORD_ACCESSOR(xr_semantic_plan_function, XrSemanticFunctionRecord, functions,
                        function_count)
XR_PLAN_RECORD_ACCESSOR(xr_semantic_plan_block, XrSemanticBlockRecord, blocks, block_count)
XR_PLAN_RECORD_ACCESSOR(xr_semantic_plan_operation, XrSemanticOperationRecord, operations,
                        operation_count)
XR_PLAN_RECORD_ACCESSOR(xr_semantic_plan_constant, XrSemanticConstantRecord, constants,
                        constant_count)
#undef XR_PLAN_RECORD_ACCESSOR

#define XR_PLAN_INDEX_ACCESSOR(name, field, count_field)                                           \
    const uint32_t *name(const XrSemanticPlan *plan, uint32_t *count) {                            \
        if (count)                                                                                 \
            *count = plan ? plan->count_field : 0;                                                 \
        return plan ? plan->field : NULL;                                                          \
    }
XR_PLAN_INDEX_ACCESSOR(xr_semantic_plan_type_children, type_children, type_child_count)
XR_PLAN_INDEX_ACCESSOR(xr_semantic_plan_parameters, parameters, parameter_count)
XR_PLAN_INDEX_ACCESSOR(xr_semantic_plan_predecessors, predecessors, predecessor_count)
XR_PLAN_INDEX_ACCESSOR(xr_semantic_plan_operands, operands, operand_count)
XR_PLAN_INDEX_ACCESSOR(xr_semantic_plan_operand_contracts, operand_contracts, operand_count)
#undef XR_PLAN_INDEX_ACCESSOR

const uint8_t *xr_semantic_plan_operand_transfer_modes(const XrSemanticPlan *plan,
                                                       uint32_t *count) {
    if (count)
        *count = plan ? plan->operand_count : 0;
    return plan ? plan->operand_transfer_modes : NULL;
}

const uint8_t *xr_semantic_plan_operand_ownership_actions(const XrSemanticPlan *plan,
                                                          uint32_t *count) {
    if (count)
        *count = plan ? plan->operand_count : 0;
    return plan ? plan->operand_ownership_actions : NULL;
}

const char *const *xr_semantic_plan_metadata(const XrSemanticPlan *plan, uint32_t *count) {
    if (count)
        *count = plan ? plan->metadata_count : 0;
    return plan ? plan->metadata : NULL;
}

const XrOwnershipCertificate *xr_semantic_plan_ownership(const XrSemanticPlan *plan) {
    return plan ? plan->ownership : NULL;
}

bool xr_semantic_plan_dump(const XrSemanticPlan *plan, FILE *out) {
    if (!plan || !out)
        return false;
    char fingerprint[XR_FINGERPRINT_BYTES * 2 + 1];
    xr_fingerprint_hex(plan->fingerprint, fingerprint);
    fprintf(out, "semantic-plan schema=%u frozen=%u fingerprint=%s\n", plan->schema,
            plan->frozen ? 1u : 0u, fingerprint);
    fprintf(out, "  types=%u functions=%u blocks=%u operations=%u constants=%u\n", plan->type_count,
            plan->function_count, plan->block_count, plan->operation_count, plan->constant_count);
    for (uint32_t i = 0; i < plan->function_count; i++) {
        char id[XR_STABLE_ID_BYTES * 2 + 1];
        xr_stable_id_hex(plan->functions[i].id, id);
        fprintf(out, "  fn[%u] id=%s name=%s blocks=%u values=%u\n", i, id, plan->functions[i].name,
                plan->functions[i].block_count, plan->functions[i].value_count);
    }
    return !ferror(out);
}
