/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_plan.c - Immutable TargetPlan storage and canonical hashing
 */

#include "xr_target_plan_internal.h"
#include "xr_target_verify.h"
#include "../semantic/xr_semantic_verify.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include <stdio.h>
#include <string.h>

static void set_error(char *error, size_t size, const char *code, const char *detail) {
    if (error && size)
        snprintf(error, size, "%s: %s", code, detail);
}

static void hash_u64(XrSHA256Context *ctx, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(ctx, bytes, sizeof(bytes));
}

static void hash_id(XrSHA256Context *ctx, XrStableId id) {
    xr_sha256_update(ctx, id.bytes, sizeof(id.bytes));
}

static void hash_fingerprint(XrSHA256Context *ctx, XrFingerprint fingerprint) {
    xr_sha256_update(ctx, fingerprint.bytes, sizeof(fingerprint.bytes));
}

static bool copy_table(void **out, const void *source, uint32_t count, size_t item_size,
                       char *error, size_t error_size) {
    *out = NULL;
    if (!count)
        return true;
    if (!source || count > SIZE_MAX / item_size) {
        set_error(error, error_size, "XR_EXEC_5003", "target table size is invalid");
        return false;
    }
    void *copy = xr_malloc((size_t) count * item_size);
    if (!copy) {
        set_error(error, error_size, "XR_EXEC_5003", "target table allocation failed");
        return false;
    }
    memcpy(copy, source, (size_t) count * item_size);
    *out = copy;
    return true;
}

static bool draft_within_budget(const XrTargetPlanDraft *draft) {
    if (!(draft->machine_reps_count <= 256u && draft->value_reps_count <= 40000000u &&
           draft->extents_count <= 1000000u &&
           draft->layouts_count <= 1000000u && draft->fields_count <= 16000000u &&
           draft->storage_count <= 4000000u && draft->allocations_count <= 10000000u &&
           draft->extent_operands_count <= 40000000u && draft->functions_count <= 100000u &&
           draft->slots_count <= 16000000u && draft->instructions_count <= 40000000u &&
           draft->calls_count <= 10000000u &&
           draft->call_arguments_count <= 40000000u && draft->root_maps_count <= 10000000u &&
           draft->root_slots_count <= 40000000u && draft->cleanups_count <= 40000000u &&
           draft->adapters_count <= 1000000u && draft->capabilities_count <= 65536u &&
           draft->coroutines_count <= 10000000u))
        return false;
    size_t total = sizeof(XrTargetPlan);
#define XR_ADD_DRAFT_BYTES(name)                                                                  \
    do {                                                                                          \
        if (draft->name##_count > (SIZE_MAX - total) / sizeof(*draft->name))                      \
            return false;                                                                         \
        total += (size_t) draft->name##_count * sizeof(*draft->name);                             \
    } while (0)
    XR_ADD_DRAFT_BYTES(machine_reps);
    XR_ADD_DRAFT_BYTES(value_reps);
    XR_ADD_DRAFT_BYTES(extents);
    XR_ADD_DRAFT_BYTES(layouts);
    XR_ADD_DRAFT_BYTES(fields);
    XR_ADD_DRAFT_BYTES(storage);
    XR_ADD_DRAFT_BYTES(allocations);
    XR_ADD_DRAFT_BYTES(extent_operands);
    XR_ADD_DRAFT_BYTES(functions);
    XR_ADD_DRAFT_BYTES(slots);
    XR_ADD_DRAFT_BYTES(instructions);
    XR_ADD_DRAFT_BYTES(calls);
    XR_ADD_DRAFT_BYTES(call_arguments);
    XR_ADD_DRAFT_BYTES(root_maps);
    XR_ADD_DRAFT_BYTES(root_slots);
    XR_ADD_DRAFT_BYTES(cleanups);
    XR_ADD_DRAFT_BYTES(adapters);
    XR_ADD_DRAFT_BYTES(capabilities);
    XR_ADD_DRAFT_BYTES(coroutines);
#undef XR_ADD_DRAFT_BYTES
    return total <= (size_t) UINT32_MAX;
}

#define XR_COPY_DRAFT_TABLE(name, type)                                                            \
    do {                                                                                           \
        if (!copy_table((void **) &plan->name, draft->name, draft->name##_count, sizeof(type),     \
                        error, error_size))                                                        \
            goto fail;                                                                             \
        plan->name##_count = draft->name##_count;                                                  \
    } while (0)

static void hash_machine_rep(XrSHA256Context *ctx, const XrTargetMachineRepRecord *record) {
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->kind);
    hash_u64(ctx, record->register_bits);
    hash_u64(ctx, record->memory_size);
    hash_u64(ctx, record->memory_align);
    hash_u64(ctx, record->signedness);
    hash_u64(ctx, record->root_kind);
    hash_u64(ctx, record->ownership);
    hash_u64(ctx, record->null_encoding);
    hash_u64(ctx, record->detail);
    hash_u64(ctx, record->lane_count);
    hash_u64(ctx, record->reserved);
    for (uint32_t i = 0; i < 4; i++)
        hash_u64(ctx, record->legal_conversion_mask[i]);
}

static void hash_value_rep(XrSHA256Context *ctx, const XrTargetValueRepRecord *record) {
    hash_u64(ctx, record->semantic_value);
    hash_u64(ctx, record->register_rep);
    hash_u64(ctx, record->memory_rep);
    hash_u64(ctx, record->slot);
}

static void hash_extent(XrSHA256Context *ctx, const XrTargetExtentRecord *record) {
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->kind);
    hash_u64(ctx, record->operand_count);
    hash_u64(ctx, record->alignment);
    hash_u64(ctx, record->element_layout);
    hash_u64(ctx, record->stride);
    hash_u64(ctx, record->provider);
    hash_u64(ctx, record->flags);
}

static void hash_layout_base(XrSHA256Context *ctx, const XrTargetLayoutRecord *record) {
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->semantic_type);
    hash_u64(ctx, record->kind);
    hash_u64(ctx, record->reserved);
    hash_u64(ctx, record->align);
    hash_u64(ctx, record->fixed_prefix_size);
    hash_u64(ctx, record->extent);
    hash_u64(ctx, record->field_begin);
    hash_u64(ctx, record->field_count);
    hash_u64(ctx, record->root_field_count);
    hash_id(ctx, record->destructor);
    hash_id(ctx, record->clone);
    hash_id(ctx, record->equality_hash);
}

static void hash_field(XrSHA256Context *ctx, const XrTargetFieldRecord *record) {
    hash_u64(ctx, record->layout);
    hash_u64(ctx, record->semantic_field);
    hash_u64(ctx, record->offset);
    hash_u64(ctx, record->size);
    hash_u64(ctx, record->align);
    hash_u64(ctx, record->memory_rep);
    hash_u64(ctx, record->root_kind);
    hash_u64(ctx, record->flags);
    hash_u64(ctx, record->reserved);
}

static void hash_storage(XrSHA256Context *ctx, const XrTargetStorageRecord *record) {
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->kind);
    hash_u64(ctx, record->ownership);
    hash_u64(ctx, record->flags);
    hash_id(ctx, record->domain);
    hash_id(ctx, record->destructor);
}

static void hash_allocation(XrSHA256Context *ctx, const XrTargetAllocationRecord *record) {
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->semantic_operation);
    hash_u64(ctx, record->layout);
    hash_u64(ctx, record->storage);
    hash_u64(ctx, record->operand_begin);
    hash_u64(ctx, record->operand_count);
    hash_u64(ctx, record->flags);
}

static void hash_extent_operand(XrSHA256Context *ctx,
                                const XrTargetExtentOperandRecord *record) {
    hash_u64(ctx, record->allocation);
    hash_u64(ctx, record->semantic_value);
    hash_u64(ctx, record->ordinal);
    hash_u64(ctx, record->role);
    hash_u64(ctx, record->reserved);
}

static void hash_function(XrSHA256Context *ctx, const XrTargetFunctionRecord *record) {
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->semantic_function);
    hash_u64(ctx, record->slot_begin);
    hash_u64(ctx, record->slot_count);
    hash_u64(ctx, record->frame_size);
    hash_u64(ctx, record->frame_align);
    hash_u64(ctx, record->reserved);
    hash_u64(ctx, record->root_begin);
    hash_u64(ctx, record->root_count);
    hash_u64(ctx, record->cleanup_begin);
    hash_u64(ctx, record->cleanup_count);
}

static void hash_slot(XrSHA256Context *ctx, const XrTargetSlotRecord *record) {
    hash_id(ctx, record->identity);
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->function);
    hash_u64(ctx, record->semantic_value);
    hash_u64(ctx, record->semantic_operation);
    hash_u64(ctx, record->logical_slot);
    hash_u64(ctx, record->offset);
    hash_u64(ctx, record->size);
    hash_u64(ctx, record->align);
    hash_u64(ctx, record->register_rep);
    hash_u64(ctx, record->memory_rep);
    hash_u64(ctx, record->role);
    hash_u64(ctx, record->root_kind);
    hash_u64(ctx, record->ownership);
    hash_u64(ctx, record->reserved);
    hash_u64(ctx, record->debug_variable);
}

static void hash_instruction(XrSHA256Context *ctx,
                             const XrTargetInstructionRecord *record) {
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->function);
    hash_u64(ctx, record->result_slot);
    hash_u64(ctx, record->operand_slots[0]);
    hash_u64(ctx, record->operand_slots[1]);
    hash_u64(ctx, record->immediate_bits);
    hash_u64(ctx, record->opcode);
    hash_u64(ctx, record->operand_count);
    hash_u64(ctx, record->reserved);
}

static void hash_call_argument(XrSHA256Context *ctx,
                               const XrTargetCallArgumentRecord *record) {
    hash_id(ctx, record->identity);
    hash_u64(ctx, record->call);
    hash_u64(ctx, record->semantic_operand);
    hash_u64(ctx, record->semantic_value);
    hash_u64(ctx, record->callee_parameter);
    hash_u64(ctx, record->caller_slot);
    hash_u64(ctx, record->callee_slot);
    hash_u64(ctx, record->register_rep);
    hash_u64(ctx, record->memory_rep);
    hash_u64(ctx, record->ordinal);
    hash_u64(ctx, record->mode);
    hash_u64(ctx, record->ownership);
    hash_u64(ctx, record->transfer_mode);
    hash_u64(ctx, record->flags);
}

static void hash_call_base(XrSHA256Context *ctx, const XrTargetCallRecord *record) {
    hash_id(ctx, record->identity);
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->semantic_call_target);
    hash_u64(ctx, record->semantic_operation);
    hash_u64(ctx, record->caller_function);
    hash_u64(ctx, record->callee_function);
    hash_u64(ctx, record->result_value);
    hash_u64(ctx, record->result_slot);
    hash_u64(ctx, record->caller_storage_slot);
    hash_u64(ctx, record->error_slot);
    hash_u64(ctx, record->argument_begin);
    hash_u64(ctx, record->adapter_begin);
    hash_u64(ctx, record->result_register_rep);
    hash_u64(ctx, record->result_memory_rep);
    hash_u64(ctx, record->error_register_rep);
    hash_u64(ctx, record->error_memory_rep);
    hash_u64(ctx, record->argument_count);
    hash_u64(ctx, record->adapter_count);
    hash_u64(ctx, record->native_abi);
    hash_u64(ctx, record->flags);
    hash_u64(ctx, record->calling_convention);
    hash_u64(ctx, record->target_kind);
    hash_u64(ctx, record->result_mode);
    hash_u64(ctx, record->result_ownership);
    hash_u64(ctx, record->error_mode);
    hash_u64(ctx, record->reserved8);
}

static void hash_root_map(XrSHA256Context *ctx, const XrTargetRootMapRecord *record) {
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->function);
    hash_u64(ctx, record->semantic_operation);
    hash_u64(ctx, record->slot_begin);
    hash_u64(ctx, record->slot_count);
    hash_u64(ctx, record->flags);
}

static void hash_cleanup(XrSHA256Context *ctx, const XrTargetCleanupRecord *record) {
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->function);
    hash_u64(ctx, record->semantic_operation);
    hash_u64(ctx, record->slot);
    hash_u64(ctx, record->action);
    hash_u64(ctx, record->flags);
    hash_u64(ctx, record->provider);
}

static void hash_adapter(XrSHA256Context *ctx, const XrTargetAdapterRecord *record) {
    hash_id(ctx, record->identity);
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->call);
    hash_u64(ctx, record->input_rep);
    hash_u64(ctx, record->output_rep);
    hash_u64(ctx, record->layout);
    hash_u64(ctx, record->ordinal);
    hash_u64(ctx, record->flags);
    hash_u64(ctx, record->role);
    hash_u64(ctx, record->kind);
    hash_u64(ctx, record->ownership);
}

static void hash_capability(XrSHA256Context *ctx, const XrTargetCapabilityRecord *record) {
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->capability);
    hash_u64(ctx, record->provider);
    hash_u64(ctx, record->flags);
}

static void hash_coroutine(XrSHA256Context *ctx,
                           const XrTargetCoroutineStateRecord *record) {
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->function);
    hash_u64(ctx, record->logical_state);
    hash_u64(ctx, record->target_state);
    hash_u64(ctx, record->root_map);
    hash_u64(ctx, record->cleanup);
    hash_u64(ctx, record->normal_state);
    hash_u64(ctx, record->error_state);
    hash_u64(ctx, record->cancel_state);
    hash_u64(ctx, record->drop_state);
}

void xr_target_layout_compute_fingerprint(const XrTargetPlan *plan, uint32_t layout_index,
                                          XrFingerprint *out) {
    static const uint8_t domain[] = "xray-target-layout-v2\0";
    const XrTargetLayoutRecord *layout = &plan->layouts[layout_index];
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1);
    hash_fingerprint(&ctx, plan->semantic_fingerprint);
    hash_fingerprint(&ctx, xr_target_profile_fingerprint(plan->profile));
    const XrSemanticTypeRecord *semantic_type =
        xr_semantic_plan_type(plan->semantic_plan, layout->semantic_type);
    hash_id(&ctx, semantic_type->id);
    hash_u64(&ctx, semantic_type->kind);
    hash_u64(&ctx, semantic_type->scalar_rep);
    hash_u64(&ctx, semantic_type->flags);
    hash_layout_base(&ctx, layout);
    hash_extent(&ctx, &plan->extents[layout->extent]);
    hash_u64(&ctx, plan->machine_reps_count);
    for (uint32_t i = 0; i < plan->machine_reps_count; i++)
        hash_machine_rep(&ctx, &plan->machine_reps[i]);
    for (uint32_t i = 0; i < layout->field_count; i++) {
        const XrTargetFieldRecord *field = &plan->fields[layout->field_begin + i];
        hash_field(&ctx, field);
        hash_machine_rep(&ctx, &plan->machine_reps[field->memory_rep]);
    }
    xr_sha256_final(&ctx, out->bytes);
}

void xr_target_call_compute_fingerprint(const XrTargetPlan *plan, uint32_t call_index,
                                        XrFingerprint *out) {
    static const uint8_t domain[] = "xray-target-call-v2\0";
    const XrTargetCallRecord *call = &plan->calls[call_index];
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1);
    hash_fingerprint(&ctx, plan->semantic_fingerprint);
    hash_fingerprint(&ctx, xr_target_profile_fingerprint(plan->profile));
    const XrSemanticOperationRecord *semantic_operation =
        xr_semantic_plan_operation(plan->semantic_plan, call->semantic_operation);
    hash_id(&ctx, semantic_operation->id);
    hash_u64(&ctx, semantic_operation->function);
    if (semantic_operation->function < xr_semantic_plan_function_count(plan->semantic_plan))
        hash_id(&ctx, xr_semantic_plan_function(plan->semantic_plan,
                                                semantic_operation->function)->id);
    if (semantic_operation->result_type < xr_semantic_plan_type_count(plan->semantic_plan))
        hash_id(&ctx,
                xr_semantic_plan_type(plan->semantic_plan, semantic_operation->result_type)->id);
    hash_call_base(&ctx, call);
    hash_machine_rep(&ctx, &plan->machine_reps[call->result_register_rep]);
    hash_machine_rep(&ctx, &plan->machine_reps[call->result_memory_rep]);
    for (uint32_t i = 0; i < call->argument_count; i++) {
        const XrTargetCallArgumentRecord *argument =
            &plan->call_arguments[call->argument_begin + i];
        hash_call_argument(&ctx, argument);
        hash_machine_rep(&ctx, &plan->machine_reps[argument->register_rep]);
        hash_machine_rep(&ctx, &plan->machine_reps[argument->memory_rep]);
    }
    for (uint32_t i = 0; i < call->adapter_count; i++)
        hash_adapter(&ctx, &plan->adapters[call->adapter_begin + i]);
    xr_sha256_final(&ctx, out->bytes);
}

void xr_target_plan_compute_fingerprint(const XrTargetPlan *plan, XrFingerprint *out) {
    static const uint8_t domain[] = "xray-target-plan-v5\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1);
    hash_u64(&ctx, plan->schema_version);
    hash_u64(&ctx, plan->completed_family_mask);
    hash_fingerprint(&ctx, plan->semantic_fingerprint);
    hash_fingerprint(&ctx, xr_target_profile_fingerprint(plan->profile));
#define XR_HASH_TABLE_COUNT(name) hash_u64(&ctx, plan->name##_count)
    XR_HASH_TABLE_COUNT(machine_reps);
    XR_HASH_TABLE_COUNT(value_reps);
    XR_HASH_TABLE_COUNT(extents);
    XR_HASH_TABLE_COUNT(layouts);
    XR_HASH_TABLE_COUNT(fields);
    XR_HASH_TABLE_COUNT(storage);
    XR_HASH_TABLE_COUNT(allocations);
    XR_HASH_TABLE_COUNT(extent_operands);
    XR_HASH_TABLE_COUNT(functions);
    XR_HASH_TABLE_COUNT(slots);
    XR_HASH_TABLE_COUNT(instructions);
    XR_HASH_TABLE_COUNT(calls);
    XR_HASH_TABLE_COUNT(call_arguments);
    XR_HASH_TABLE_COUNT(root_maps);
    XR_HASH_TABLE_COUNT(root_slots);
    XR_HASH_TABLE_COUNT(cleanups);
    XR_HASH_TABLE_COUNT(adapters);
    XR_HASH_TABLE_COUNT(capabilities);
    XR_HASH_TABLE_COUNT(coroutines);
#undef XR_HASH_TABLE_COUNT
    for (uint32_t i = 0; i < plan->machine_reps_count; i++)
        hash_machine_rep(&ctx, &plan->machine_reps[i]);
    for (uint32_t i = 0; i < plan->value_reps_count; i++)
        hash_value_rep(&ctx, &plan->value_reps[i]);
    for (uint32_t i = 0; i < plan->extents_count; i++)
        hash_extent(&ctx, &plan->extents[i]);
    for (uint32_t i = 0; i < plan->layouts_count; i++) {
        hash_layout_base(&ctx, &plan->layouts[i]);
        hash_fingerprint(&ctx, plan->layouts[i].fingerprint);
    }
    for (uint32_t i = 0; i < plan->fields_count; i++)
        hash_field(&ctx, &plan->fields[i]);
    for (uint32_t i = 0; i < plan->storage_count; i++)
        hash_storage(&ctx, &plan->storage[i]);
    for (uint32_t i = 0; i < plan->allocations_count; i++)
        hash_allocation(&ctx, &plan->allocations[i]);
    for (uint32_t i = 0; i < plan->extent_operands_count; i++)
        hash_extent_operand(&ctx, &plan->extent_operands[i]);
    for (uint32_t i = 0; i < plan->functions_count; i++)
        hash_function(&ctx, &plan->functions[i]);
    for (uint32_t i = 0; i < plan->slots_count; i++)
        hash_slot(&ctx, &plan->slots[i]);
    for (uint32_t i = 0; i < plan->instructions_count; i++)
        hash_instruction(&ctx, &plan->instructions[i]);
    for (uint32_t i = 0; i < plan->calls_count; i++) {
        hash_call_base(&ctx, &plan->calls[i]);
        hash_fingerprint(&ctx, plan->calls[i].fingerprint);
    }
    for (uint32_t i = 0; i < plan->call_arguments_count; i++)
        hash_call_argument(&ctx, &plan->call_arguments[i]);
    for (uint32_t i = 0; i < plan->root_maps_count; i++)
        hash_root_map(&ctx, &plan->root_maps[i]);
    for (uint32_t i = 0; i < plan->root_slots_count; i++)
        hash_u64(&ctx, plan->root_slots[i]);
    for (uint32_t i = 0; i < plan->cleanups_count; i++)
        hash_cleanup(&ctx, &plan->cleanups[i]);
    for (uint32_t i = 0; i < plan->adapters_count; i++)
        hash_adapter(&ctx, &plan->adapters[i]);
    for (uint32_t i = 0; i < plan->capabilities_count; i++)
        hash_capability(&ctx, &plan->capabilities[i]);
    for (uint32_t i = 0; i < plan->coroutines_count; i++)
        hash_coroutine(&ctx, &plan->coroutines[i]);
    xr_sha256_final(&ctx, out->bytes);
}

bool xr_target_plan_freeze(const XrTargetPlanDraft *draft, XrTargetPlan **out, char *error,
                           size_t error_size) {
    if (out)
        *out = NULL;
    if (!draft || !out || !draft->semantic_plan || !draft->profile) {
        set_error(error, error_size, "XR_TARGET_1000", "target plan input is incomplete");
        return false;
    }
    if (!draft_within_budget(draft)) {
        set_error(error, error_size, "XR_EXEC_5003", "target plan exceeds hard budgets");
        return false;
    }
    char semantic_error[512] = {0};
    if (!xr_semantic_plan_verify(draft->semantic_plan, semantic_error, sizeof(semantic_error))) {
        set_error(error, error_size, "XR_TARGET_1000", "semantic plan is not exactly verified");
        return false;
    }
    if (!xr_target_profile_verify(draft->profile, error, error_size))
        return false;
    XrTargetPlan *plan = (XrTargetPlan *) xr_calloc(1, sizeof(*plan));
    if (!plan) {
        set_error(error, error_size, "XR_EXEC_5003", "target plan allocation failed");
        return false;
    }
    atomic_init(&plan->references, 1);
    plan->schema_version = XR_TARGET_PLAN_SCHEMA_VERSION;
    plan->completed_family_mask = draft->completed_family_mask;
    plan->semantic_plan = xr_semantic_plan_retain((XrSemanticPlan *) draft->semantic_plan);
    plan->semantic_fingerprint = xr_semantic_plan_fingerprint(draft->semantic_plan);
    plan->profile = xr_target_profile_retain(draft->profile);
    XR_COPY_DRAFT_TABLE(machine_reps, XrTargetMachineRepRecord);
    XR_COPY_DRAFT_TABLE(value_reps, XrTargetValueRepRecord);
    XR_COPY_DRAFT_TABLE(extents, XrTargetExtentRecord);
    XR_COPY_DRAFT_TABLE(layouts, XrTargetLayoutRecord);
    XR_COPY_DRAFT_TABLE(fields, XrTargetFieldRecord);
    XR_COPY_DRAFT_TABLE(storage, XrTargetStorageRecord);
    XR_COPY_DRAFT_TABLE(allocations, XrTargetAllocationRecord);
    XR_COPY_DRAFT_TABLE(extent_operands, XrTargetExtentOperandRecord);
    XR_COPY_DRAFT_TABLE(functions, XrTargetFunctionRecord);
    XR_COPY_DRAFT_TABLE(slots, XrTargetSlotRecord);
    XR_COPY_DRAFT_TABLE(instructions, XrTargetInstructionRecord);
    XR_COPY_DRAFT_TABLE(calls, XrTargetCallRecord);
    XR_COPY_DRAFT_TABLE(call_arguments, XrTargetCallArgumentRecord);
    XR_COPY_DRAFT_TABLE(root_maps, XrTargetRootMapRecord);
    XR_COPY_DRAFT_TABLE(root_slots, uint32_t);
    XR_COPY_DRAFT_TABLE(cleanups, XrTargetCleanupRecord);
    XR_COPY_DRAFT_TABLE(adapters, XrTargetAdapterRecord);
    XR_COPY_DRAFT_TABLE(capabilities, XrTargetCapabilityRecord);
    XR_COPY_DRAFT_TABLE(coroutines, XrTargetCoroutineStateRecord);
    plan->frozen = true;
    for (uint32_t i = 0; i < plan->layouts_count; i++) {
        if (plan->layouts[i].extent >= plan->extents_count ||
            plan->layouts[i].semantic_type >= xr_semantic_plan_type_count(plan->semantic_plan) ||
            !((plan->layouts[i].field_begin <= plan->fields_count) &&
              (plan->layouts[i].field_count <=
               plan->fields_count - plan->layouts[i].field_begin)))
            goto invalid;
        for (uint32_t f = 0; f < plan->layouts[i].field_count; f++)
            if (plan->fields[plan->layouts[i].field_begin + f].memory_rep >=
                plan->machine_reps_count)
                goto invalid;
        xr_target_layout_compute_fingerprint(plan, i, &plan->layouts[i].fingerprint);
    }
    for (uint32_t i = 0; i < plan->calls_count; i++) {
        if (plan->calls[i].semantic_operation >=
                xr_semantic_plan_operation_count(plan->semantic_plan) ||
            plan->calls[i].semantic_call_target >=
                xr_semantic_plan_call_target_count(plan->semantic_plan) ||
            plan->calls[i].result_register_rep >= plan->machine_reps_count ||
            plan->calls[i].result_memory_rep >= plan->machine_reps_count ||
            plan->calls[i].error_register_rep >= plan->machine_reps_count ||
            plan->calls[i].error_memory_rep >= plan->machine_reps_count ||
            !((plan->calls[i].argument_begin <= plan->call_arguments_count) &&
              (plan->calls[i].argument_count <=
               plan->call_arguments_count - plan->calls[i].argument_begin)) ||
            !((plan->calls[i].adapter_begin <= plan->adapters_count) &&
              (plan->calls[i].adapter_count <=
               plan->adapters_count - plan->calls[i].adapter_begin)))
            goto invalid_call;
        for (uint32_t a = 0; a < plan->calls[i].argument_count; a++) {
            const XrTargetCallArgumentRecord *argument =
                &plan->call_arguments[plan->calls[i].argument_begin + a];
            if (argument->register_rep >= plan->machine_reps_count ||
                argument->memory_rep >= plan->machine_reps_count)
                goto invalid_call;
        }
        xr_target_call_compute_fingerprint(plan, i, &plan->calls[i].fingerprint);
    }
    xr_target_plan_compute_fingerprint(plan, &plan->fingerprint);
    if (!xr_target_plan_verify(plan, error, error_size))
        goto fail;
    plan->verified = true;
    *out = plan;
    return true;

invalid:
    set_error(error, error_size, "XR_TARGET_1002", "target plan references an invalid table row");
    goto fail;
invalid_call:
    set_error(error, error_size, "XR_TARGET_1003", "call plan references an invalid table row");
fail:
    xr_target_plan_free(plan);
    return false;
}

#undef XR_COPY_DRAFT_TABLE

XrTargetPlan *xr_target_plan_retain(XrTargetPlan *plan) {
    if (plan)
        atomic_fetch_add_explicit(&plan->references, 1, memory_order_relaxed);
    return plan;
}

void xr_target_plan_free(XrTargetPlan *plan) {
    if (!plan)
        return;
    if (atomic_fetch_sub_explicit(&plan->references, 1, memory_order_acq_rel) != 1)
        return;
    xr_semantic_plan_free(plan->semantic_plan);
    xr_target_profile_free(plan->profile);
#define XR_FREE_TARGET_TABLE(name) xr_free(plan->name)
    XR_FREE_TARGET_TABLE(machine_reps);
    XR_FREE_TARGET_TABLE(value_reps);
    XR_FREE_TARGET_TABLE(extents);
    XR_FREE_TARGET_TABLE(layouts);
    XR_FREE_TARGET_TABLE(fields);
    XR_FREE_TARGET_TABLE(storage);
    XR_FREE_TARGET_TABLE(allocations);
    XR_FREE_TARGET_TABLE(extent_operands);
    XR_FREE_TARGET_TABLE(functions);
    XR_FREE_TARGET_TABLE(slots);
    XR_FREE_TARGET_TABLE(instructions);
    XR_FREE_TARGET_TABLE(calls);
    XR_FREE_TARGET_TABLE(call_arguments);
    XR_FREE_TARGET_TABLE(root_maps);
    XR_FREE_TARGET_TABLE(root_slots);
    XR_FREE_TARGET_TABLE(cleanups);
    XR_FREE_TARGET_TABLE(adapters);
    XR_FREE_TARGET_TABLE(capabilities);
    XR_FREE_TARGET_TABLE(coroutines);
#undef XR_FREE_TARGET_TABLE
    xr_free(plan);
}

bool xr_target_plan_is_frozen(const XrTargetPlan *plan) {
    return plan && plan->frozen;
}

bool xr_target_plan_is_verified(const XrTargetPlan *plan) {
    return plan && plan->frozen && plan->verified;
}

uint32_t xr_target_plan_schema_version(const XrTargetPlan *plan) {
    return plan ? plan->schema_version : 0;
}

XrFingerprint xr_target_plan_fingerprint(const XrTargetPlan *plan) {
    XrFingerprint zero = {{0}};
    return plan ? plan->fingerprint : zero;
}

XrFingerprint xr_target_plan_semantic_fingerprint(const XrTargetPlan *plan) {
    XrFingerprint zero = {{0}};
    return plan ? plan->semantic_fingerprint : zero;
}

bool xr_target_plan_fingerprint_is_intact(const XrTargetPlan *plan) {
    if (!xr_target_plan_is_verified(plan) || !plan->profile)
        return false;
    XrFingerprint actual;
    xr_target_plan_compute_fingerprint(plan, &actual);
    return xr_fingerprint_equal(actual, plan->fingerprint);
}

uint64_t xr_target_plan_completed_family_mask(const XrTargetPlan *plan) {
    return plan ? plan->completed_family_mask : 0;
}

const XrSemanticPlan *xr_target_plan_semantic_plan(const XrTargetPlan *plan) {
    return plan ? plan->semantic_plan : NULL;
}

const XrTargetProfile *xr_target_plan_profile(const XrTargetPlan *plan) {
    return plan ? plan->profile : NULL;
}

const XrTargetMachineRepRecord *xr_target_plan_machine_rep(const XrTargetPlan *plan,
                                                            uint16_t rep) {
    if (!plan || rep >= plan->machine_reps_count)
        return NULL;
    return &plan->machine_reps[rep];
}

const XrTargetValueRepRecord *xr_target_plan_value_rep(const XrTargetPlan *plan,
                                                        uint32_t semantic_value) {
    if (!plan)
        return NULL;
    uint32_t begin = 0;
    uint32_t end = plan->value_reps_count;
    while (begin < end) {
        uint32_t middle = begin + (end - begin) / 2u;
        const XrTargetValueRepRecord *record = &plan->value_reps[middle];
        if (record->semantic_value == semantic_value)
            return record;
        if (record->semantic_value < semantic_value)
            begin = middle + 1u;
        else
            end = middle;
    }
    return NULL;
}

const XrTargetInstructionRecord *xr_target_plan_function_instructions(
    const XrTargetPlan *plan, uint32_t function, uint32_t *count) {
    if (count)
        *count = 0;
    if (!xr_target_plan_is_verified(plan) || function >= plan->functions_count)
        return NULL;
    uint32_t begin = 0;
    while (begin < plan->instructions_count &&
           plan->instructions[begin].function < function)
        begin++;
    uint32_t end = begin;
    while (end < plan->instructions_count &&
           plan->instructions[end].function == function)
        end++;
    if (begin == end)
        return NULL;
    if (count)
        *count = end - begin;
    return &plan->instructions[begin];
}

uint64_t xr_target_plan_function_execution_family_mask(const XrTargetPlan *plan,
                                                       uint32_t function) {
    uint32_t count = 0;
    return xr_target_plan_function_instructions(plan, function, &count) && count
               ? (uint64_t) XR_TARGET_EXECUTION_SCALAR_I64_STRAIGHT_LINE
               : 0;
}

#define XR_TARGET_TABLE_ACCESSOR(name, type)                                                       \
    const type *xr_target_plan_##name(const XrTargetPlan *plan, uint32_t *count) {                  \
        if (count)                                                                                 \
            *count = plan ? plan->name##_count : 0;                                                \
        return plan ? plan->name : NULL;                                                           \
    }
XR_TARGET_TABLE_ACCESSOR(machine_reps, XrTargetMachineRepRecord)
XR_TARGET_TABLE_ACCESSOR(value_reps, XrTargetValueRepRecord)
XR_TARGET_TABLE_ACCESSOR(extents, XrTargetExtentRecord)
XR_TARGET_TABLE_ACCESSOR(layouts, XrTargetLayoutRecord)
XR_TARGET_TABLE_ACCESSOR(fields, XrTargetFieldRecord)
XR_TARGET_TABLE_ACCESSOR(storage, XrTargetStorageRecord)
XR_TARGET_TABLE_ACCESSOR(allocations, XrTargetAllocationRecord)
XR_TARGET_TABLE_ACCESSOR(extent_operands, XrTargetExtentOperandRecord)
XR_TARGET_TABLE_ACCESSOR(functions, XrTargetFunctionRecord)
XR_TARGET_TABLE_ACCESSOR(slots, XrTargetSlotRecord)
XR_TARGET_TABLE_ACCESSOR(instructions, XrTargetInstructionRecord)
XR_TARGET_TABLE_ACCESSOR(calls, XrTargetCallRecord)
XR_TARGET_TABLE_ACCESSOR(call_arguments, XrTargetCallArgumentRecord)
XR_TARGET_TABLE_ACCESSOR(root_maps, XrTargetRootMapRecord)
XR_TARGET_TABLE_ACCESSOR(root_slots, uint32_t)
XR_TARGET_TABLE_ACCESSOR(cleanups, XrTargetCleanupRecord)
XR_TARGET_TABLE_ACCESSOR(adapters, XrTargetAdapterRecord)
XR_TARGET_TABLE_ACCESSOR(capabilities, XrTargetCapabilityRecord)
XR_TARGET_TABLE_ACCESSOR(coroutines, XrTargetCoroutineStateRecord)
#undef XR_TARGET_TABLE_ACCESSOR
