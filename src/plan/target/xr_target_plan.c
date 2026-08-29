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
#include "xr_target_instruction_verify.h"
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
    if (!(draft->semantic_dependency_count <= XR_TARGET_MAX_SEMANTIC_DEPENDENCIES &&
          draft->semantic_module_count <= XR_TARGET_MAX_PROGRAM_MODULES &&
          draft->machine_reps_count <= 256u && draft->value_reps_count <= 40000000u &&
          draft->extents_count <= 1000000u && draft->layouts_count <= 1000000u &&
          draft->fields_count <= 16000000u && draft->storage_count <= 4000000u &&
          draft->allocations_count <= 10000000u && draft->extent_operands_count <= 40000000u &&
          draft->functions_count <= 100000u && draft->slots_count <= 16000000u &&
          draft->i64_overflow_predicates_count <= XR_PROGRAM_SEMANTIC_CLOSURE_MAX_CALLS &&
          draft->instructions_count <= 40000000u && draft->calls_count <= 10000000u &&
          draft->call_arguments_count <= 40000000u && draft->root_maps_count <= 10000000u &&
          draft->root_slots_count <= 40000000u && draft->cleanups_count <= 40000000u &&
          draft->adapters_count <= 1000000u && draft->capabilities_count <= 65536u &&
          draft->coroutines_count <= 10000000u && draft->entry_expectations_count <= 10000000u &&
          draft->debug_facts_count <= 40000000u && draft->program_graphs_count <= 1u &&
          draft->module_partitions_count <= XR_TARGET_MAX_PROGRAM_MODULES))
        return false;
    size_t total = sizeof(XrTargetPlan);
    if (draft->semantic_dependency_count >
        (SIZE_MAX - total) / sizeof(*draft->semantic_dependencies))
        return false;
    total += (size_t) draft->semantic_dependency_count * sizeof(*draft->semantic_dependencies);
    if (draft->semantic_module_count > (SIZE_MAX - total) / sizeof(*draft->semantic_modules))
        return false;
    total += (size_t) draft->semantic_module_count * sizeof(*draft->semantic_modules);
#define XR_ADD_DRAFT_BYTES(name)                                                                   \
    do {                                                                                           \
        if (draft->name##_count > (SIZE_MAX - total) / sizeof(*draft->name))                       \
            return false;                                                                          \
        total += (size_t) draft->name##_count * sizeof(*draft->name);                              \
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
    XR_ADD_DRAFT_BYTES(i64_overflow_predicates);
    XR_ADD_DRAFT_BYTES(instructions);
    XR_ADD_DRAFT_BYTES(calls);
    XR_ADD_DRAFT_BYTES(call_arguments);
    XR_ADD_DRAFT_BYTES(root_maps);
    XR_ADD_DRAFT_BYTES(root_slots);
    XR_ADD_DRAFT_BYTES(cleanups);
    XR_ADD_DRAFT_BYTES(adapters);
    XR_ADD_DRAFT_BYTES(capabilities);
    XR_ADD_DRAFT_BYTES(coroutines);
    XR_ADD_DRAFT_BYTES(entry_expectations);
    XR_ADD_DRAFT_BYTES(debug_facts);
    XR_ADD_DRAFT_BYTES(program_graphs);
    XR_ADD_DRAFT_BYTES(module_partitions);
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
    hash_u64(ctx, record->array_element_storage);
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
    hash_u64(ctx, record->semantic_name);
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

static void hash_extent_operand(XrSHA256Context *ctx, const XrTargetExtentOperandRecord *record) {
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
    hash_u64(ctx, record->coroutine_begin);
    hash_u64(ctx, record->coroutine_count);
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

static void hash_instruction(XrSHA256Context *ctx, const XrTargetInstructionRecord *record) {
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

static void hash_i64_overflow_predicate(XrSHA256Context *ctx,
                                        const XrTargetI64OverflowPredicateRecord *record) {
    hash_id(ctx, record->identity);
    hash_id(ctx, record->program_call);
    hash_id(ctx, record->callsite);
    hash_id(ctx, record->caller_identity);
    hash_id(ctx, record->builtin_identity);
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->function);
    hash_u64(ctx, record->semantic_operation);
    hash_u64(ctx, record->program_row);
    hash_u64(ctx, record->result_slot);
    hash_u64(ctx, record->receiver_slot);
    hash_u64(ctx, record->argument_slot);
    hash_u64(ctx, record->kind);
    hash_u64(ctx, record->reserved[0]);
    hash_u64(ctx, record->reserved[1]);
    hash_u64(ctx, record->reserved[2]);
}

static void hash_call_argument(XrSHA256Context *ctx, const XrTargetCallArgumentRecord *record) {
    hash_id(ctx, record->identity);
    hash_u64(ctx, record->call);
    hash_u64(ctx, record->semantic_operand);
    hash_u64(ctx, record->semantic_value);
    hash_u64(ctx, record->callee_parameter);
    hash_u64(ctx, record->caller_slot);
    hash_u64(ctx, record->callee_slot);
    hash_u64(ctx, record->register_rep);
    hash_u64(ctx, record->memory_rep);
    hash_u64(ctx, record->callee_register_rep);
    hash_u64(ctx, record->callee_memory_rep);
    hash_u64(ctx, record->ordinal);
    hash_u64(ctx, record->mode);
    hash_u64(ctx, record->ownership);
    hash_u64(ctx, record->transfer_mode);
    hash_u64(ctx, record->flags);
    hash_u64(ctx, record->array_element_storage);
    hash_u64(ctx, record->reserved8[0]);
    hash_u64(ctx, record->reserved8[1]);
    hash_u64(ctx, record->reserved8[2]);
}

static void hash_call_base(XrSHA256Context *ctx, const XrTargetCallRecord *record) {
    hash_id(ctx, record->identity);
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->semantic_call_target);
    hash_u64(ctx, record->semantic_operation);
    hash_u64(ctx, record->caller_function);
    hash_u64(ctx, record->callee_function);
    hash_u64(ctx, record->source_dependency);
    hash_u64(ctx, record->source_export);
    hash_id(ctx, record->source_export_identity);
    hash_id(ctx, record->source_callee_identity);
    hash_id(ctx, record->native_callee_identity);
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
    hash_u64(ctx, record->native_leaf);
    hash_u64(ctx, record->flags);
    hash_u64(ctx, record->calling_convention);
    hash_u64(ctx, record->target_kind);
    hash_u64(ctx, record->result_mode);
    hash_u64(ctx, record->result_ownership);
    hash_u64(ctx, record->error_mode);
    hash_u64(ctx, record->array_intrinsic_kind);
    hash_u64(ctx, record->array_element_storage);
    hash_u64(ctx, record->array_hof_kind);
    hash_u64(ctx, record->array_result_element_storage);
    hash_u64(ctx, record->reserved8[0]);
    hash_u64(ctx, record->reserved8[1]);
    hash_u64(ctx, record->reserved8[2]);
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

static void hash_coroutine(XrSHA256Context *ctx, const XrTargetCoroutineStateRecord *record) {
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->function);
    hash_u64(ctx, record->semantic_entity);
    hash_u64(ctx, record->semantic_operation);
    hash_u64(ctx, record->logical_state);
    hash_u64(ctx, record->suspend_block);
    hash_u64(ctx, record->resume_block);
    hash_u64(ctx, record->resume_predecessor);
    hash_u64(ctx, record->resume_instruction);
    hash_u64(ctx, record->direct_call);
    hash_u64(ctx, record->result_slot);
    hash_u64(ctx, record->resume_predecessor_ordinal);
    hash_u64(ctx, record->flags);
}

static void hash_entry_expectation(XrSHA256Context *ctx,
                                   const XrTargetEntryExpectationRecord *record) {
    hash_id(ctx, record->identity);
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->call);
    hash_u64(ctx, record->abi_schema_version);
    hash_u64(ctx, record->parameter_count);
    hash_u64(ctx, record->native_abi);
    hash_u64(ctx, record->value_kind);
    hash_u64(ctx, record->adapter_kind);
    hash_u64(ctx, record->flags);
    hash_u64(ctx, record->reserved32);
    hash_u64(ctx, record->target_data_layout);
    hash_fingerprint(ctx, record->target_profile_fingerprint);
    hash_fingerprint(ctx, record->entry_abi_fingerprint);
    hash_fingerprint(ctx, record->adapter_fingerprint);
}

static void hash_debug_fact(XrSHA256Context *ctx, const XrTargetDebugFactRecord *record) {
    hash_u64(ctx, record->id);
    hash_u64(ctx, record->instruction);
    hash_u64(ctx, record->function);
    hash_u64(ctx, record->semantic_operation);
    hash_u64(ctx, record->coroutine_state);
    hash_u64(ctx, record->source_start_line);
    hash_u64(ctx, record->source_start_column);
    hash_u64(ctx, record->source_end_line);
    hash_u64(ctx, record->source_end_column);
    hash_id(ctx, record->semantic_operation_identity);
    hash_id(ctx, record->source_span_identity);
    hash_id(ctx, record->owner_identity);
    hash_id(ctx, record->coroutine_state_identity);
    hash_fingerprint(ctx, record->layout_fingerprint);
}

static void hash_program_graph(XrSHA256Context *ctx, const XrTargetProgramGraphRecord *record) {
#define XR_HASH_GRAPH_U32(name) hash_u64(ctx, record->name)
    XR_HASH_GRAPH_U32(schema);
    XR_HASH_GRAPH_U32(family);
    XR_HASH_GRAPH_U32(module_count);
    XR_HASH_GRAPH_U32(function_count);
    XR_HASH_GRAPH_U32(export_count);
    XR_HASH_GRAPH_U32(entry_count);
    XR_HASH_GRAPH_U32(call_count);
    XR_HASH_GRAPH_U32(argument_count);
    XR_HASH_GRAPH_U32(entry_partition);
    XR_HASH_GRAPH_U32(producer_partition);
    XR_HASH_GRAPH_U32(entry_target_function);
    XR_HASH_GRAPH_U32(producer_target_function);
    XR_HASH_GRAPH_U32(entry_semantic_function);
    XR_HASH_GRAPH_U32(producer_semantic_function);
    XR_HASH_GRAPH_U32(target_call);
    XR_HASH_GRAPH_U32(target_argument);
    XR_HASH_GRAPH_U32(entry_semantic_operation);
    XR_HASH_GRAPH_U32(producer_semantic_export);
    XR_HASH_GRAPH_U32(entry_semantic_dependency);
    XR_HASH_GRAPH_U32(producer_semantic_parameter);
    XR_HASH_GRAPH_U32(caller_slot);
    XR_HASH_GRAPH_U32(callee_slot);
    XR_HASH_GRAPH_U32(argument_ordinal);
    XR_HASH_GRAPH_U32(flags);
#undef XR_HASH_GRAPH_U32
    hash_fingerprint(ctx, record->program_fingerprint);
    hash_id(ctx, record->generation_identity);
    hash_fingerprint(ctx, record->target_profile_fingerprint);
    hash_id(ctx, record->entry_function_identity);
    hash_id(ctx, record->producer_function_identity);
    hash_u64(ctx, record->entry_function_flags);
    hash_u64(ctx, record->producer_function_flags);
    hash_u64(ctx, record->reserved16);
    hash_id(ctx, record->export_identity);
    hash_id(ctx, record->exported_function_identity);
    hash_id(ctx, record->entry_identity);
    hash_id(ctx, record->call_identity);
    hash_id(ctx, record->callsite_identity);
    hash_id(ctx, record->resolver_binding);
    hash_id(ctx, record->argument_identity);
    hash_id(ctx, record->parameter_identity);
}

static void hash_module_partition(XrSHA256Context *ctx,
                                  const XrTargetModulePartitionRecord *record) {
    hash_id(ctx, record->module_identity);
    hash_fingerprint(ctx, record->semantic_fingerprint);
    hash_u64(ctx, record->program_module_row);
    hash_u64(ctx, record->semantic_module);
#define XR_HASH_PARTITION_RANGE(name)                                                              \
    do {                                                                                           \
        hash_u64(ctx, record->name##_begin);                                                       \
        hash_u64(ctx, record->name##_count);                                                       \
    } while (0)
    XR_HASH_PARTITION_RANGE(value_reps);
    XR_HASH_PARTITION_RANGE(extents);
    XR_HASH_PARTITION_RANGE(layouts);
    XR_HASH_PARTITION_RANGE(fields);
    XR_HASH_PARTITION_RANGE(storage);
    XR_HASH_PARTITION_RANGE(allocations);
    XR_HASH_PARTITION_RANGE(extent_operands);
    XR_HASH_PARTITION_RANGE(functions);
    XR_HASH_PARTITION_RANGE(slots);
    XR_HASH_PARTITION_RANGE(i64_overflow_predicates);
    XR_HASH_PARTITION_RANGE(instructions);
    XR_HASH_PARTITION_RANGE(calls);
    XR_HASH_PARTITION_RANGE(call_arguments);
    XR_HASH_PARTITION_RANGE(root_maps);
    XR_HASH_PARTITION_RANGE(root_slots);
    XR_HASH_PARTITION_RANGE(cleanups);
    XR_HASH_PARTITION_RANGE(adapters);
    XR_HASH_PARTITION_RANGE(coroutines);
    XR_HASH_PARTITION_RANGE(entry_expectations);
    XR_HASH_PARTITION_RANGE(debug_facts);
#undef XR_HASH_PARTITION_RANGE
}

static const XrSemanticEntityRecord *target_semantic_module_entity(const XrSemanticPlan *plan) {
    const XrSemanticEntityRecord *found = NULL;
    size_t entity_count = xr_semantic_plan_entity_count(plan);
    for (size_t row = 0; row < entity_count; row++) {
        const XrSemanticEntityRecord *entity = xr_semantic_plan_entity(plan, row);
        if (!entity || entity->kind != XR_SEM_ENTITY_MODULE)
            continue;
        if (found)
            return NULL;
        found = entity;
    }
    return found;
}

bool xr_target_semantic_program_module_direct_dependencies(const XrSemanticPlan *const *modules,
                                                           uint32_t module_count,
                                                           uint32_t program_module_row,
                                                           const XrSemanticPlan ***dependencies,
                                                           uint32_t *dependency_count, char *error,
                                                           size_t error_size) {
    if (dependencies)
        *dependencies = NULL;
    if (dependency_count)
        *dependency_count = 0u;
    if (!modules || !module_count || module_count > XR_TARGET_MAX_PROGRAM_MODULES ||
        program_module_row >= module_count || !modules[program_module_row] || !dependencies ||
        !dependency_count) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "program semantic module fragment input is invalid");
        return false;
    }
    const XrSemanticPlan *fragment = modules[program_module_row];
    size_t required_size = xr_semantic_plan_dependency_count(fragment);
    if (required_size > XR_TARGET_MAX_SEMANTIC_DEPENDENCIES || required_size > UINT32_MAX) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "program semantic fragment direct dependency count is invalid");
        return false;
    }
    uint32_t required_count = (uint32_t) required_size;
    const XrSemanticPlan **direct =
        required_count ? (const XrSemanticPlan **) xr_calloc(required_count, sizeof(*direct))
                       : NULL;
    if (required_count && !direct) {
        set_error(error, error_size, "XR_EXEC_5003",
                  "program semantic direct dependency rebuild exhausted its budget");
        return false;
    }
    bool valid = true;
    for (uint32_t dependency = 0; valid && dependency < required_count; dependency++) {
        const XrSemanticDependencyRecord *required =
            xr_semantic_plan_dependency(fragment, dependency);
        const XrSemanticPlan *match = NULL;
        for (uint32_t row = 0; required && row < module_count; row++) {
            const XrSemanticEntityRecord *candidate = target_semantic_module_entity(modules[row]);
            if (!candidate || !xr_stable_id_equal(candidate->id, required->module) ||
                !xr_fingerprint_equal(xr_semantic_plan_fingerprint(modules[row]),
                                      required->semantic_fingerprint))
                continue;
            if (match) {
                valid = false;
                break;
            }
            match = modules[row];
        }
        direct[dependency] = match;
        valid = match != NULL;
    }
    if (!valid) {
        xr_free(direct);
        set_error(error, error_size, "XR_TARGET_1000",
                  "program semantic fragment direct dependency owner is missing or ambiguous");
        return false;
    }
    *dependencies = direct;
    *dependency_count = required_count;
    return true;
}

bool xr_target_semantic_program_module_verify_fragment(const XrSemanticPlan *const *modules,
                                                       uint32_t module_count,
                                                       uint32_t program_module_row, char *error,
                                                       size_t error_size) {
    const XrSemanticPlan **direct = NULL;
    uint32_t dependency_count = 0;
    if (!xr_target_semantic_program_module_direct_dependencies(
            modules, module_count, program_module_row, &direct, &dependency_count, error,
            error_size))
        return false;
    const XrSemanticPlan *fragment = modules[program_module_row];
    char nested_error[512] = {0};
    bool valid = xr_semantic_plan_verify_module_set(fragment, direct, dependency_count,
                                                    nested_error, sizeof(nested_error));
    xr_free(direct);
    if (!valid)
        set_error(error, error_size, "XR_TARGET_1000",
                  "program semantic fragment direct dependencies are not exact");
    return valid;
}

bool xr_target_semantic_program_module_set_verify(const XrSemanticPlan *const *modules,
                                                  uint32_t module_count, char *error,
                                                  size_t error_size) {
    if (!modules || !module_count || module_count > XR_TARGET_MAX_PROGRAM_MODULES) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "program semantic module set input is invalid");
        return false;
    }
    const XrSemanticProgramProvenance *authority = xr_semantic_plan_program_provenance(modules[0]);
    if (!authority || authority->module_count != module_count) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "program semantic module-set authority is missing");
        return false;
    }
    for (uint32_t row = 0; row < module_count; row++) {
        const XrSemanticProgramProvenance *program =
            modules[row] ? xr_semantic_plan_program_provenance(modules[row]) : NULL;
        if (!program || program->module_count != module_count ||
            program->program_module_row != row ||
            program->program_schema != authority->program_schema ||
            program->program_family != authority->program_family ||
            !xr_fingerprint_equal(program->program_fingerprint, authority->program_fingerprint) ||
            !xr_stable_id_equal(program->generation_identity, authority->generation_identity) ||
            !xr_target_semantic_program_module_verify_fragment(modules, module_count, row, error,
                                                               error_size))
            return false;
        for (uint32_t prior = 0; prior < row; prior++) {
            const XrSemanticProgramProvenance *previous =
                xr_semantic_plan_program_provenance(modules[prior]);
            if (previous && xr_stable_id_equal(previous->program_module, program->program_module)) {
                set_error(error, error_size, "XR_TARGET_1000",
                          "program semantic module identity is duplicated");
                return false;
            }
        }
    }
    return true;
}

bool xr_target_semantic_module_set_fingerprint(const XrSemanticPlan *const *modules,
                                               uint32_t module_count, XrFingerprint *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!modules || !out || !module_count || module_count > XR_TARGET_MAX_PROGRAM_MODULES)
        return false;
    const XrSemanticProgramProvenance *entry_program =
        xr_semantic_plan_program_provenance(modules[0]);
    bool valid = entry_program && entry_program->module_count == module_count;
    for (uint32_t row = 0; valid && row < module_count; row++) {
        const XrSemanticProgramProvenance *program =
            modules[row] ? xr_semantic_plan_program_provenance(modules[row]) : NULL;
        valid =
            program && program->module_count == module_count &&
            program->program_module_row == row &&
            program->program_schema == entry_program->program_schema &&
            program->program_family == entry_program->program_family &&
            xr_fingerprint_equal(program->program_fingerprint,
                                 entry_program->program_fingerprint) &&
            xr_stable_id_equal(program->generation_identity, entry_program->generation_identity);
    }
    if (valid) {
        static const uint8_t domain[] = "xray-target-semantic-module-set-v1\0";
        XrSHA256Context ctx;
        xr_sha256_init(&ctx);
        xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
        hash_u64(&ctx, entry_program->program_schema);
        hash_u64(&ctx, entry_program->program_family);
        hash_u64(&ctx, module_count);
        hash_fingerprint(&ctx, entry_program->program_fingerprint);
        hash_id(&ctx, entry_program->generation_identity);
        for (uint32_t row = 0; row < module_count; row++) {
            const XrSemanticProgramProvenance *program =
                xr_semantic_plan_program_provenance(modules[row]);
            hash_u64(&ctx, row);
            hash_id(&ctx, program->program_module);
            hash_fingerprint(&ctx, xr_semantic_plan_fingerprint(modules[row]));
        }
        xr_sha256_final(&ctx, out->bytes);
    }
    return valid;
}

static bool target_module_partition_row_for_plan(const XrSemanticPlan *const *modules,
                                                 uint32_t module_count, const XrSemanticPlan *plan,
                                                 uint32_t *out_row) {
    if (out_row)
        *out_row = UINT32_MAX;
    if (!modules || !plan)
        return false;
    for (uint32_t row = 0; row < module_count; row++) {
        if (modules[row] != plan)
            continue;
        if (out_row)
            *out_row = row;
        return true;
    }
    return false;
}

/* Identity for a plan that covers several modules without claiming any proven
 * cross-module call. Which modules a plan covers and whether one direct-call
 * edge crosses between two of them are different facts, so this set is keyed by
 * each module's own SemanticPlan entity and needs no program provenance. */
bool xr_target_semantic_module_partition_set_verify(const XrSemanticPlan *const *modules,
                                                    uint32_t module_count, char *error,
                                                    size_t error_size) {
    if (!modules || !module_count || module_count > XR_TARGET_MAX_PROGRAM_MODULES) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "target module partition set input is invalid");
        return false;
    }
    for (uint32_t row = 0; row < module_count; row++) {
        const XrSemanticEntityRecord *entity = target_semantic_module_entity(modules[row]);
        if (!entity || !xr_semantic_plan_is_verified(modules[row])) {
            set_error(error, error_size, "XR_TARGET_1000",
                      "target module partition row has no exact module authority");
            return false;
        }
        for (uint32_t prior = 0; prior < row; prior++) {
            const XrSemanticEntityRecord *previous = target_semantic_module_entity(modules[prior]);
            if (previous && xr_stable_id_equal(previous->id, entity->id)) {
                set_error(error, error_size, "XR_TARGET_1000",
                          "target module partition identity is duplicated");
                return false;
            }
        }
        if (!xr_target_semantic_program_module_verify_fragment(modules, module_count, row, error,
                                                               error_size))
            return false;
    }
    return true;
}

bool xr_target_semantic_module_partition_set_fingerprint(const XrSemanticPlan *const *modules,
                                                         uint32_t module_count,
                                                         XrFingerprint *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!modules || !out || !module_count || module_count > XR_TARGET_MAX_PROGRAM_MODULES)
        return false;
    for (uint32_t row = 0; row < module_count; row++)
        if (!target_semantic_module_entity(modules[row]))
            return false;
    static const uint8_t domain[] = "xray-target-semantic-module-partition-set-v1\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    hash_u64(&ctx, module_count);
    for (uint32_t row = 0; row < module_count; row++) {
        hash_u64(&ctx, row);
        hash_id(&ctx, target_semantic_module_entity(modules[row])->id);
        hash_fingerprint(&ctx, xr_semantic_plan_fingerprint(modules[row]));
    }
    xr_sha256_final(&ctx, out->bytes);
    return true;
}

bool xr_target_plan_program_module_set_fingerprint(const XrTargetPlan *plan, XrFingerprint *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!plan || !out || plan->program_graphs_count != 1u ||
        plan->module_partitions_count != plan->semantic_module_count ||
        plan->semantic_module_count != plan->program_graphs[0].module_count)
        return false;
    const XrSemanticPlan *const *modules = (const XrSemanticPlan *const *) plan->semantic_modules;
    if (!xr_target_semantic_module_set_fingerprint(modules, plan->semantic_module_count, out))
        return false;
    return xr_fingerprint_equal(*out, plan->semantic_fingerprint);
}

typedef enum XrTargetOwnedTable {
    XR_TARGET_OWNED_LAYOUT,
    XR_TARGET_OWNED_CALL,
    XR_TARGET_OWNED_DEBUG_FACT,
} XrTargetOwnedTable;

static const XrSemanticPlan *semantic_owner_for_row(const XrTargetPlan *plan, uint32_t row,
                                                    XrTargetOwnedTable table,
                                                    uint32_t *partition_index) {
    if (partition_index)
        *partition_index = UINT32_MAX;
    if (!plan->module_partitions_count)
        return plan->semantic_plan;
    for (uint32_t i = 0; i < plan->module_partitions_count; i++) {
        const XrTargetModulePartitionRecord *partition = &plan->module_partitions[i];
        uint32_t begin = 0, count = 0;
        switch (table) {
            case XR_TARGET_OWNED_LAYOUT:
                begin = partition->layouts_begin;
                count = partition->layouts_count;
                break;
            case XR_TARGET_OWNED_CALL:
                begin = partition->calls_begin;
                count = partition->calls_count;
                break;
            case XR_TARGET_OWNED_DEBUG_FACT:
                begin = partition->debug_facts_begin;
                count = partition->debug_facts_count;
                break;
        }
        if (row < begin || row - begin >= count)
            continue;
        if (partition_index)
            *partition_index = i;
        return partition->semantic_module < plan->semantic_module_count
                   ? plan->semantic_modules[partition->semantic_module]
                   : NULL;
    }
    return NULL;
}

void xr_target_layout_compute_fingerprint(const XrTargetPlan *plan, uint32_t layout_index,
                                          XrFingerprint *out) {
    static const uint8_t domain[] = "xray-target-layout-v5\0";
    const XrTargetLayoutRecord *layout = &plan->layouts[layout_index];
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1);
    hash_fingerprint(&ctx, plan->semantic_fingerprint);
    hash_fingerprint(&ctx, xr_target_profile_fingerprint(plan->profile));
    const XrSemanticPlan *semantic =
        semantic_owner_for_row(plan, layout_index, XR_TARGET_OWNED_LAYOUT, NULL);
    const XrSemanticTypeRecord *semantic_type =
        semantic ? xr_semantic_plan_type(semantic, layout->semantic_type) : NULL;
    if (!semantic_type) {
        memset(out, 0, sizeof(*out));
        return;
    }
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
    static const uint8_t domain[] = "xray-target-call-v6\0";
    const XrTargetCallRecord *call = &plan->calls[call_index];
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1);
    hash_fingerprint(&ctx, plan->semantic_fingerprint);
    hash_fingerprint(&ctx, xr_target_profile_fingerprint(plan->profile));
    const XrSemanticPlan *semantic =
        semantic_owner_for_row(plan, call_index, XR_TARGET_OWNED_CALL, NULL);
    const XrSemanticOperationRecord *semantic_operation =
        semantic ? xr_semantic_plan_operation(semantic, call->semantic_operation) : NULL;
    if (!semantic_operation) {
        memset(out, 0, sizeof(*out));
        return;
    }
    hash_id(&ctx, semantic_operation->id);
    hash_u64(&ctx, semantic_operation->function);
    if (semantic_operation->function < xr_semantic_plan_function_count(semantic))
        hash_id(&ctx, xr_semantic_plan_function(semantic, semantic_operation->function)->id);
    if (semantic_operation->result_type < xr_semantic_plan_type_count(semantic))
        hash_id(&ctx, xr_semantic_plan_type(semantic, semantic_operation->result_type)->id);
    hash_call_base(&ctx, call);
    hash_machine_rep(&ctx, &plan->machine_reps[call->result_register_rep]);
    hash_machine_rep(&ctx, &plan->machine_reps[call->result_memory_rep]);
    for (uint32_t i = 0; i < call->argument_count; i++) {
        const XrTargetCallArgumentRecord *argument =
            &plan->call_arguments[call->argument_begin + i];
        hash_call_argument(&ctx, argument);
        hash_machine_rep(&ctx, &plan->machine_reps[argument->register_rep]);
        hash_machine_rep(&ctx, &plan->machine_reps[argument->memory_rep]);
        hash_machine_rep(&ctx, &plan->machine_reps[argument->callee_register_rep]);
        hash_machine_rep(&ctx, &plan->machine_reps[argument->callee_memory_rep]);
    }
    for (uint32_t i = 0; i < call->adapter_count; i++)
        hash_adapter(&ctx, &plan->adapters[call->adapter_begin + i]);
    xr_sha256_final(&ctx, out->bytes);
}

void xr_target_plan_compute_fingerprint(const XrTargetPlan *plan, XrFingerprint *out) {
    static const uint8_t domain[] = "xray-target-plan-v25\0";
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
    XR_HASH_TABLE_COUNT(i64_overflow_predicates);
    XR_HASH_TABLE_COUNT(instructions);
    XR_HASH_TABLE_COUNT(calls);
    XR_HASH_TABLE_COUNT(call_arguments);
    XR_HASH_TABLE_COUNT(root_maps);
    XR_HASH_TABLE_COUNT(root_slots);
    XR_HASH_TABLE_COUNT(cleanups);
    XR_HASH_TABLE_COUNT(adapters);
    XR_HASH_TABLE_COUNT(capabilities);
    XR_HASH_TABLE_COUNT(coroutines);
    XR_HASH_TABLE_COUNT(entry_expectations);
    XR_HASH_TABLE_COUNT(debug_facts);
    XR_HASH_TABLE_COUNT(program_graphs);
    XR_HASH_TABLE_COUNT(module_partitions);
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
    for (uint32_t i = 0; i < plan->i64_overflow_predicates_count; i++)
        hash_i64_overflow_predicate(&ctx, &plan->i64_overflow_predicates[i]);
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
    for (uint32_t i = 0; i < plan->entry_expectations_count; i++)
        hash_entry_expectation(&ctx, &plan->entry_expectations[i]);
    for (uint32_t i = 0; i < plan->debug_facts_count; i++)
        hash_debug_fact(&ctx, &plan->debug_facts[i]);
    for (uint32_t i = 0; i < plan->program_graphs_count; i++)
        hash_program_graph(&ctx, &plan->program_graphs[i]);
    for (uint32_t i = 0; i < plan->module_partitions_count; i++)
        hash_module_partition(&ctx, &plan->module_partitions[i]);
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
    bool semantic_verified =
        draft->semantic_dependency_count == 0
            ? xr_semantic_plan_verify(draft->semantic_plan, semantic_error, sizeof(semantic_error))
            : xr_semantic_plan_verify_module_set(draft->semantic_plan, draft->semantic_dependencies,
                                                 draft->semantic_dependency_count, semantic_error,
                                                 sizeof(semantic_error));
    if (!semantic_verified) {
        set_error(error, error_size, "XR_TARGET_1000", "semantic plan is not exactly verified");
        return false;
    }
    /* A program graph claims one proven cross-module call and is keyed by the
     * program semantic provenance. A partition-only plan claims nothing but its
     * own module coverage, so it is keyed by each module's SemanticPlan entity. */
    bool program_graph = draft->program_graphs_count != 0u;
    bool module_partitioned = draft->module_partitions_count != 0u;
    const XrSemanticProgramProvenance *entry_program =
        xr_semantic_plan_program_provenance(draft->semantic_plan);
    if (program_graph &&
        (!entry_program ||
         !xr_target_semantic_program_module_set_verify(draft->semantic_modules,
                                                       draft->semantic_module_count, semantic_error,
                                                       sizeof(semantic_error)) ||
         entry_program->program_module_row >= draft->semantic_module_count ||
         draft->semantic_modules[entry_program->program_module_row] != draft->semantic_plan)) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "program semantic module set is not exactly verified");
        return false;
    }
    if (!program_graph && module_partitioned &&
        (!xr_target_semantic_module_partition_set_verify(draft->semantic_modules,
                                                         draft->semantic_module_count,
                                                         semantic_error, sizeof(semantic_error)) ||
         !target_module_partition_row_for_plan(
             draft->semantic_modules, draft->semantic_module_count, draft->semantic_plan, NULL))) {
        set_error(error, error_size, "XR_TARGET_1000",
                  "target module partition set is not exactly verified");
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
    if (program_graph && !xr_target_semantic_module_set_fingerprint(draft->semantic_modules,
                                                                    draft->semantic_module_count,
                                                                    &plan->semantic_fingerprint)) {
        set_error(error, error_size, "XR_TARGET_1002",
                  "target module-set semantic identity is invalid");
        goto fail;
    }
    if (!program_graph && module_partitioned &&
        !xr_target_semantic_module_partition_set_fingerprint(
            draft->semantic_modules, draft->semantic_module_count, &plan->semantic_fingerprint)) {
        set_error(error, error_size, "XR_TARGET_1002",
                  "target module partition identity is invalid");
        goto fail;
    }
    plan->semantic_dependency_count = draft->semantic_dependency_count;
    if (plan->semantic_dependency_count) {
        if (!draft->semantic_dependencies ||
            plan->semantic_dependency_count > SIZE_MAX / sizeof(*plan->semantic_dependencies))
            goto fail;
        plan->semantic_dependencies = (XrSemanticPlan **) xr_calloc(
            plan->semantic_dependency_count, sizeof(*plan->semantic_dependencies));
        if (!plan->semantic_dependencies)
            goto fail;
        for (uint32_t i = 0; i < plan->semantic_dependency_count; i++) {
            plan->semantic_dependencies[i] =
                xr_semantic_plan_retain((XrSemanticPlan *) draft->semantic_dependencies[i]);
            if (!plan->semantic_dependencies[i])
                goto fail;
        }
    }
    plan->semantic_module_count = draft->semantic_module_count;
    if (plan->semantic_module_count) {
        if (!draft->semantic_modules ||
            plan->semantic_module_count > SIZE_MAX / sizeof(*plan->semantic_modules))
            goto fail;
        plan->semantic_modules = (XrSemanticPlan **) xr_calloc(plan->semantic_module_count,
                                                               sizeof(*plan->semantic_modules));
        if (!plan->semantic_modules)
            goto fail;
        for (uint32_t i = 0; i < plan->semantic_module_count; i++) {
            plan->semantic_modules[i] =
                xr_semantic_plan_retain((XrSemanticPlan *) draft->semantic_modules[i]);
            if (!plan->semantic_modules[i])
                goto fail;
        }
    }
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
    XR_COPY_DRAFT_TABLE(i64_overflow_predicates, XrTargetI64OverflowPredicateRecord);
    XR_COPY_DRAFT_TABLE(instructions, XrTargetInstructionRecord);
    XR_COPY_DRAFT_TABLE(calls, XrTargetCallRecord);
    XR_COPY_DRAFT_TABLE(call_arguments, XrTargetCallArgumentRecord);
    XR_COPY_DRAFT_TABLE(root_maps, XrTargetRootMapRecord);
    XR_COPY_DRAFT_TABLE(root_slots, uint32_t);
    XR_COPY_DRAFT_TABLE(cleanups, XrTargetCleanupRecord);
    XR_COPY_DRAFT_TABLE(adapters, XrTargetAdapterRecord);
    XR_COPY_DRAFT_TABLE(capabilities, XrTargetCapabilityRecord);
    XR_COPY_DRAFT_TABLE(coroutines, XrTargetCoroutineStateRecord);
    XR_COPY_DRAFT_TABLE(entry_expectations, XrTargetEntryExpectationRecord);
    XR_COPY_DRAFT_TABLE(debug_facts, XrTargetDebugFactRecord);
    XR_COPY_DRAFT_TABLE(program_graphs, XrTargetProgramGraphRecord);
    XR_COPY_DRAFT_TABLE(module_partitions, XrTargetModulePartitionRecord);
    plan->frozen = true;
    for (uint32_t i = 0; i < plan->layouts_count; i++) {
        const XrSemanticPlan *semantic =
            semantic_owner_for_row(plan, i, XR_TARGET_OWNED_LAYOUT, NULL);
        if (plan->layouts[i].extent >= plan->extents_count || !semantic ||
            plan->layouts[i].semantic_type >= xr_semantic_plan_type_count(semantic) ||
            !((plan->layouts[i].field_begin <= plan->fields_count) &&
              (plan->layouts[i].field_count <= plan->fields_count - plan->layouts[i].field_begin)))
            goto invalid;
        for (uint32_t f = 0; f < plan->layouts[i].field_count; f++)
            if (plan->fields[plan->layouts[i].field_begin + f].memory_rep >=
                plan->machine_reps_count)
                goto invalid;
        xr_target_layout_compute_fingerprint(plan, i, &plan->layouts[i].fingerprint);
    }
    for (uint32_t i = 0; i < plan->debug_facts_count; i++) {
        XrTargetDebugFactRecord *fact = &plan->debug_facts[i];
        uint32_t partition_index = UINT32_MAX;
        const XrSemanticPlan *semantic =
            semantic_owner_for_row(plan, i, XR_TARGET_OWNED_DEBUG_FACT, &partition_index);
        if (!semantic)
            goto invalid;
        if (fact->semantic_operation == XR_SEMANTIC_INDEX_NONE)
            continue;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, fact->semantic_operation);
        if (!operation)
            goto invalid;
        bool found = false;
        uint32_t layout_begin = 0, layout_count = plan->layouts_count;
        if (plan->module_partitions_count) {
            if (partition_index >= plan->module_partitions_count)
                goto invalid;
            layout_begin = plan->module_partitions[partition_index].layouts_begin;
            layout_count = plan->module_partitions[partition_index].layouts_count;
            if (layout_begin > plan->layouts_count ||
                layout_count > plan->layouts_count - layout_begin)
                goto invalid;
        }
        for (uint32_t layout = layout_begin; layout < layout_begin + layout_count; layout++) {
            if (plan->layouts[layout].semantic_type != operation->result_type)
                continue;
            if (found)
                goto invalid;
            fact->layout_fingerprint = plan->layouts[layout].fingerprint;
            found = true;
        }
    }
    for (uint32_t i = 0; i < plan->calls_count; i++) {
        const XrSemanticPlan *semantic =
            semantic_owner_for_row(plan, i, XR_TARGET_OWNED_CALL, NULL);
        bool direct_local = plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_DIRECT_LOCAL;
        bool program_direct = plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_PROGRAM_DIRECT;
        bool channel_close = plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_CHANNEL_CLOSE;
        bool source_export = plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_SOURCE_EXPORT;
        bool stringbuilder_constructor =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_STRINGBUILDER_CONSTRUCTOR;
        bool string_byte_slice_view =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_STRING_BYTE_SLICE_VIEW;
        bool stringbuilder_append_rune =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_RUNE;
        bool string_runes = plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_STRING_RUNES;
        bool iterator_rune_has_next =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_ITERATOR_RUNE_HAS_NEXT;
        bool iterator_rune_next =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NEXT;
        bool iterator_rune_nth =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NTH;
        bool rune_to_uint32 = plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_RUNE_TO_UINT32;
        bool rune_to_string = plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_RUNE_TO_STRING;
        bool rune_is_whitespace =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_RUNE_IS_WHITESPACE;
        bool string_slice_range =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_STRING_SLICE_RANGE;
        bool stringbuilder_to_string =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_STRINGBUILDER_TO_STRING;
        bool stringbuilder_append_string =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_STRING;
        bool json_namespace_value =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_JSON_NAMESPACE_VALUE;
        bool array_member_scalar =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_ARRAY_MEMBER_SCALAR;
        bool native_module_scalar =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_NATIVE_MODULE_SCALAR;
        bool native_namespace_yieldable =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE;
        bool native_target_leaf =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_NATIVE_TARGET_LEAF_SCALAR;
        /* The construction is one of the rows that names a SemanticPlan call
         * target rather than a sealed builtin, so its target index must index
         * that table. */
        bool source_class_constructor =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR;
        bool adt_enum_constructor =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_ADT_ENUM_CONSTRUCTOR;
        bool array_intrinsic = plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_ARRAY_INTRINSIC;
        bool array_fill = plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_ARRAY_FILL_SCALAR;
        bool array_hof = plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_ARRAY_HOF;
        bool panic_info_constructor =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_PANIC_INFO_CONSTRUCTOR;
        bool container_copy = plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_CONTAINER_COPY;
        bool scalar_copy = plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_SCALAR_COPY;
        bool map_entries_iterator =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_MAP_ENTRIES_ITERATOR;
        bool map_entry_iterator_has_next =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_MAP_ENTRY_ITERATOR_HAS_NEXT;
        bool map_entry_iterator_next =
            plan->calls[i].target_kind == XR_TARGET_CALL_TARGET_MAP_ENTRY_ITERATOR_NEXT;
        if (!semantic ||
            (!direct_local && !program_direct && !channel_close && !source_export &&
             !stringbuilder_constructor && !string_byte_slice_view && !stringbuilder_append_rune &&
             !string_runes && !iterator_rune_has_next && !iterator_rune_next &&
             !iterator_rune_nth && !rune_to_uint32 && !rune_to_string && !rune_is_whitespace &&
             !string_slice_range && !stringbuilder_to_string && !stringbuilder_append_string &&
             !json_namespace_value && !array_member_scalar && !native_module_scalar &&
             !native_namespace_yieldable && !native_target_leaf && !source_class_constructor &&
             !adt_enum_constructor && !array_intrinsic && !array_fill && !array_hof &&
             !panic_info_constructor && !scalar_copy && !container_copy && !map_entries_iterator &&
             !map_entry_iterator_has_next && !map_entry_iterator_next) ||
            plan->calls[i].semantic_operation >= xr_semantic_plan_operation_count(semantic) ||
            ((direct_local || program_direct || source_export || native_namespace_yieldable ||
              source_class_constructor) &&
             plan->calls[i].semantic_call_target >= xr_semantic_plan_call_target_count(semantic)) ||
            ((channel_close || stringbuilder_constructor || string_byte_slice_view ||
              stringbuilder_append_rune || stringbuilder_to_string || stringbuilder_append_string ||
              string_runes || iterator_rune_has_next || iterator_rune_next || iterator_rune_nth ||
              rune_to_uint32 || rune_to_string || rune_is_whitespace || string_slice_range ||
              json_namespace_value || array_member_scalar || native_module_scalar ||
              native_target_leaf || adt_enum_constructor || array_intrinsic || array_fill ||
              array_hof || panic_info_constructor || scalar_copy || container_copy ||
              map_entries_iterator || map_entry_iterator_has_next || map_entry_iterator_next) &&
             plan->calls[i].semantic_call_target != XR_SEMANTIC_INDEX_NONE) ||
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
                argument->memory_rep >= plan->machine_reps_count ||
                argument->callee_register_rep >= plan->machine_reps_count ||
                argument->callee_memory_rep >= plan->machine_reps_count)
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
    for (uint32_t i = 0; i < plan->semantic_dependency_count; i++)
        xr_semantic_plan_free(plan->semantic_dependencies[i]);
    xr_free(plan->semantic_dependencies);
    for (uint32_t i = 0; i < plan->semantic_module_count; i++)
        xr_semantic_plan_free(plan->semantic_modules[i]);
    xr_free(plan->semantic_modules);
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
    XR_FREE_TARGET_TABLE(i64_overflow_predicates);
    XR_FREE_TARGET_TABLE(instructions);
    XR_FREE_TARGET_TABLE(calls);
    XR_FREE_TARGET_TABLE(call_arguments);
    XR_FREE_TARGET_TABLE(root_maps);
    XR_FREE_TARGET_TABLE(root_slots);
    XR_FREE_TARGET_TABLE(cleanups);
    XR_FREE_TARGET_TABLE(adapters);
    XR_FREE_TARGET_TABLE(capabilities);
    XR_FREE_TARGET_TABLE(coroutines);
    XR_FREE_TARGET_TABLE(entry_expectations);
    XR_FREE_TARGET_TABLE(debug_facts);
    XR_FREE_TARGET_TABLE(program_graphs);
    XR_FREE_TARGET_TABLE(module_partitions);
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

const XrTargetMachineRepRecord *xr_target_plan_machine_rep(const XrTargetPlan *plan, uint16_t rep) {
    if (!plan || rep >= plan->machine_reps_count)
        return NULL;
    return &plan->machine_reps[rep];
}

const XrTargetValueRepRecord *xr_target_plan_value_rep(const XrTargetPlan *plan,
                                                       uint32_t semantic_value) {
    if (!plan || plan->module_partitions_count > 1u)
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

const XrSemanticPlan *xr_target_plan_semantic_dependency(const XrTargetPlan *plan,
                                                         uint32_t dependency) {
    return plan && dependency < plan->semantic_dependency_count
               ? plan->semantic_dependencies[dependency]
               : NULL;
}

uint32_t xr_target_plan_program_module_count(const XrTargetPlan *plan) {
    if (!xr_target_plan_is_verified(plan))
        return 0u;
    return plan->semantic_module_count ? plan->semantic_module_count : 1u;
}

const XrSemanticPlan *xr_target_plan_program_module(const XrTargetPlan *plan,
                                                    uint32_t program_module) {
    if (!xr_target_plan_is_verified(plan))
        return NULL;
    if (!plan->semantic_module_count)
        return program_module == 0u ? plan->semantic_plan : NULL;
    return program_module < plan->semantic_module_count ? plan->semantic_modules[program_module]
                                                        : NULL;
}

const XrSemanticPlan *xr_target_plan_semantic_module(const XrTargetPlan *plan, uint32_t partition) {
    if (!xr_target_plan_is_verified(plan))
        return NULL;
    if (!plan->module_partitions_count)
        return partition == 0u ? plan->semantic_plan : NULL;
    if (partition >= plan->module_partitions_count)
        return NULL;
    uint32_t semantic_module = plan->module_partitions[partition].semantic_module;
    return semantic_module < plan->semantic_module_count ? plan->semantic_modules[semantic_module]
                                                         : NULL;
}

bool xr_target_plan_partition_for_semantic(const XrTargetPlan *plan,
                                           const XrSemanticPlan *semantic_plan,
                                           uint32_t *partition) {
    if (partition)
        *partition = UINT32_MAX;
    if (!xr_target_plan_is_verified(plan) || !semantic_plan || !partition ||
        !xr_semantic_plan_is_frozen(semantic_plan) || !xr_semantic_plan_is_verified(semantic_plan))
        return false;
    XrFingerprint semantic_fingerprint = xr_semantic_plan_fingerprint(semantic_plan);
    const XrSemanticProgramProvenance *program = xr_semantic_plan_program_provenance(semantic_plan);
    if (!plan->module_partitions_count) {
        if (semantic_plan != plan->semantic_plan || plan->semantic_module_count != 0u ||
            !xr_fingerprint_equal(plan->semantic_fingerprint, semantic_fingerprint) ||
            (program && (program->module_count != 1u || program->program_module_row != 0u)))
            return false;
        *partition = 0u;
        return true;
    }
    if (!program || !program->module_count ||
        program->program_module_row >= program->module_count ||
        plan->module_partitions_count != plan->semantic_module_count ||
        plan->semantic_module_count != program->module_count)
        return false;
    uint32_t match = UINT32_MAX;
    for (uint32_t i = 0u; i < plan->module_partitions_count; i++) {
        const XrTargetModulePartitionRecord *candidate = &plan->module_partitions[i];
        if (candidate->program_module_row != program->program_module_row ||
            !xr_stable_id_equal(candidate->module_identity, program->program_module) ||
            !xr_fingerprint_equal(candidate->semantic_fingerprint, semantic_fingerprint) ||
            xr_target_plan_semantic_module(plan, i) != semantic_plan)
            continue;
        if (match != UINT32_MAX)
            return false;
        match = i;
    }
    if (match == UINT32_MAX)
        return false;
    *partition = match;
    return true;
}

const XrSemanticPlan *xr_target_plan_module_for_function(const XrTargetPlan *plan,
                                                         uint32_t target_function,
                                                         uint32_t *partition) {
    if (partition)
        *partition = UINT32_MAX;
    if (!xr_target_plan_is_verified(plan) || target_function >= plan->functions_count)
        return NULL;
    if (!plan->module_partitions_count) {
        if (partition)
            *partition = 0u;
        return plan->semantic_plan;
    }
    for (uint32_t i = 0; i < plan->module_partitions_count; i++) {
        const XrTargetModulePartitionRecord *module = &plan->module_partitions[i];
        if (target_function >= module->functions_begin &&
            target_function - module->functions_begin < module->functions_count) {
            if (partition)
                *partition = i;
            return xr_target_plan_semantic_module(plan, i);
        }
    }
    return NULL;
}

const XrTargetValueRepRecord *xr_target_plan_value_rep_for_module(const XrTargetPlan *plan,
                                                                  uint32_t partition,
                                                                  uint32_t semantic_value) {
    if (!xr_target_plan_is_verified(plan))
        return NULL;
    uint32_t begin = 0u;
    uint32_t count = plan->value_reps_count;
    if (plan->module_partitions_count) {
        if (partition >= plan->module_partitions_count)
            return NULL;
        begin = plan->module_partitions[partition].value_reps_begin;
        count = plan->module_partitions[partition].value_reps_count;
    } else if (partition != 0u) {
        return NULL;
    }
    if (begin > plan->value_reps_count || count > plan->value_reps_count - begin)
        return NULL;
    uint32_t end = begin + count;
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

bool xr_target_plan_function_semantic_binding(const XrTargetPlan *plan, uint32_t target_function,
                                              const XrSemanticPlan **semantic_plan,
                                              uint32_t *semantic_function) {
    if (semantic_plan)
        *semantic_plan = NULL;
    if (semantic_function)
        *semantic_function = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticPlan *owner = xr_target_plan_module_for_function(plan, target_function, NULL);
    if (!owner)
        return false;
    uint32_t local = plan->functions[target_function].semantic_function;
    if (local >= xr_semantic_plan_function_count(owner))
        return false;
    if (semantic_plan)
        *semantic_plan = owner;
    if (semantic_function)
        *semantic_function = local;
    return true;
}

bool xr_target_plan_find_function(const XrTargetPlan *plan, const XrSemanticPlan *semantic_plan,
                                  uint32_t semantic_function, uint32_t *target_function) {
    if (target_function)
        *target_function = UINT32_MAX;
    if (!xr_target_plan_is_verified(plan) || !semantic_plan || !target_function)
        return false;
    uint32_t begin = 0u;
    uint32_t count = plan->functions_count;
    if (plan->module_partitions_count) {
        bool found_module = false;
        for (uint32_t i = 0; i < plan->module_partitions_count; i++) {
            if (xr_target_plan_semantic_module(plan, i) != semantic_plan)
                continue;
            begin = plan->module_partitions[i].functions_begin;
            count = plan->module_partitions[i].functions_count;
            found_module = true;
            break;
        }
        if (!found_module)
            return false;
    } else if (semantic_plan != plan->semantic_plan) {
        return false;
    }
    if (begin > plan->functions_count || count > plan->functions_count - begin)
        return false;
    for (uint32_t i = begin; i < begin + count; i++) {
        if (plan->functions[i].semantic_function != semantic_function)
            continue;
        *target_function = i;
        return true;
    }
    return false;
}

const XrTargetInstructionRecord *
xr_target_plan_function_instructions(const XrTargetPlan *plan, uint32_t function, uint32_t *count) {
    if (count)
        *count = 0;
    if (!xr_target_plan_is_verified(plan) || function >= plan->functions_count)
        return NULL;
    uint32_t begin = 0;
    while (begin < plan->instructions_count && plan->instructions[begin].function < function)
        begin++;
    uint32_t end = begin;
    while (end < plan->instructions_count && plan->instructions[end].function == function)
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
    const XrTargetInstructionRecord *rows =
        xr_target_plan_function_instructions(plan, function, &count);
    if (!rows || !count)
        return 0;
    bool has_overflow = false;
    for (uint32_t i = 0; i < count; i++)
        has_overflow |= rows[i].opcode == XR_TARGET_INSTRUCTION_I64_OVERFLOW_PREDICATE;
    if (has_overflow) {
        const XrSemanticProgramProvenance *program =
            xr_semantic_plan_program_provenance(plan->semantic_plan);
        uint32_t predicate_count = 0;
        const XrTargetI64OverflowPredicateRecord *predicates =
            xr_target_plan_i64_overflow_predicates(plan, &predicate_count);
        if (!program ||
            program->program_family != XR_PROGRAM_SEMANTIC_FAMILY_I64_OVERFLOW_PREDICATE ||
            !predicates || predicate_count != program->call_count)
            return 0;
        for (uint32_t i = 0; i < predicate_count; i++)
            if (predicates[i].function != function)
                return 0;
        return (uint64_t) XR_TARGET_EXECUTION_I64_OVERFLOW_PREDICATE;
    }
    if (count == 4 && rows[0].opcode == XR_TARGET_INSTRUCTION_PARAM_DYN_BORROW &&
        rows[1].opcode == XR_TARGET_INSTRUCTION_PARAM_DYN_OWNED &&
        rows[2].opcode == XR_TARGET_INSTRUCTION_ARRAY_PUSH_TAGGED &&
        rows[3].opcode == XR_TARGET_INSTRUCTION_RETURN_UNIT)
        return (uint64_t) XR_TARGET_EXECUTION_MANAGED_ARRAY_PUSH_TAGGED;
    bool product_caller =
        count == 15u && rows[0].opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_AGGREGATE;
    bool product_callee = count == 14u;
    if (product_caller || product_callee) {
        uint32_t scalar_begin = product_caller ? 1u : 0u;
        uint32_t init = product_caller ? 7u : 6u;
        bool exact = rows[init].opcode == XR_TARGET_INSTRUCTION_VALUE_PRODUCT_INIT &&
                     rows[count - 1u].opcode == XR_TARGET_INSTRUCTION_RETURN_AGGREGATE;
        for (uint32_t ordinal = 0; exact && ordinal < 6u; ordinal++) {
            uint16_t scalar_opcode =
                product_caller ? (ordinal == 2u ? XR_TARGET_INSTRUCTION_VALUE_PRODUCT_GET_U8
                                                : XR_TARGET_INSTRUCTION_AGGREGATE_GET_I64)
                               : (ordinal == 2u ? XR_TARGET_INSTRUCTION_CONST_U8
                                                : XR_TARGET_INSTRUCTION_CONST_I64);
            uint16_t set_opcode = ordinal == 2u ? XR_TARGET_INSTRUCTION_VALUE_PRODUCT_SET_U8
                                                : XR_TARGET_INSTRUCTION_VALUE_PRODUCT_SET_I64;
            exact = rows[scalar_begin + ordinal].opcode == scalar_opcode &&
                    rows[init + 1u + ordinal].opcode == set_opcode;
        }
        if (exact && xr_target_plan_fingerprint_is_intact(plan) &&
            xr_target_instruction_program_verify(plan, NULL, 0))
            return (uint64_t) XR_TARGET_EXECUTION_LEAF_VALUE_PRODUCT_TUPLE6;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint16_t opcode = rows[i].opcode;
        if (opcode == XR_TARGET_INSTRUCTION_CONST_U8 ||
            opcode == XR_TARGET_INSTRUCTION_VALUE_PRODUCT_INIT ||
            opcode == XR_TARGET_INSTRUCTION_VALUE_PRODUCT_SET_I64 ||
            opcode == XR_TARGET_INSTRUCTION_VALUE_PRODUCT_SET_U8 ||
            opcode == XR_TARGET_INSTRUCTION_VALUE_PRODUCT_GET_U8)
            return 0;
    }
    bool leaf_aggregate = false;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t opcode = rows[i].opcode;
        leaf_aggregate |= opcode == XR_TARGET_INSTRUCTION_PARAM_AGGREGATE ||
                          opcode == XR_TARGET_INSTRUCTION_AGGREGATE_GET_I64 ||
                          opcode == XR_TARGET_INSTRUCTION_AGGREGATE_MAKE_I64X2 ||
                          opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_AGGREGATE ||
                          opcode == XR_TARGET_INSTRUCTION_RETURN_AGGREGATE;
    }
    if (leaf_aggregate)
        return (uint64_t) XR_TARGET_EXECUTION_LEAF_AGGREGATE_I64X2;
    bool suspends = false;
    for (uint32_t i = 0; i < count; i++)
        suspends |= rows[i].opcode == XR_TARGET_INSTRUCTION_SUSPEND;
    for (uint32_t i = 0; i < plan->entry_expectations_count; i++) {
        uint32_t call = plan->entry_expectations[i].call;
        if (call < plan->calls_count && plan->calls[call].caller_function == function)
            return suspends ? 0 : (uint64_t) XR_TARGET_EXECUTION_SCALAR_I64_DYNAMIC;
    }
    return suspends ? (uint64_t) XR_TARGET_EXECUTION_SCALAR_I64_COROUTINE
                    : (uint64_t) XR_TARGET_EXECUTION_SCALAR_I64_CLOSED;
}

#define XR_TARGET_TABLE_ACCESSOR(name, type)                                                       \
    const type *xr_target_plan_##name(const XrTargetPlan *plan, uint32_t *count) {                 \
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
XR_TARGET_TABLE_ACCESSOR(i64_overflow_predicates, XrTargetI64OverflowPredicateRecord)
XR_TARGET_TABLE_ACCESSOR(instructions, XrTargetInstructionRecord)
XR_TARGET_TABLE_ACCESSOR(calls, XrTargetCallRecord)
XR_TARGET_TABLE_ACCESSOR(call_arguments, XrTargetCallArgumentRecord)
XR_TARGET_TABLE_ACCESSOR(root_maps, XrTargetRootMapRecord)
XR_TARGET_TABLE_ACCESSOR(root_slots, uint32_t)
XR_TARGET_TABLE_ACCESSOR(cleanups, XrTargetCleanupRecord)
XR_TARGET_TABLE_ACCESSOR(adapters, XrTargetAdapterRecord)
XR_TARGET_TABLE_ACCESSOR(capabilities, XrTargetCapabilityRecord)
XR_TARGET_TABLE_ACCESSOR(coroutines, XrTargetCoroutineStateRecord)
XR_TARGET_TABLE_ACCESSOR(entry_expectations, XrTargetEntryExpectationRecord)
XR_TARGET_TABLE_ACCESSOR(debug_facts, XrTargetDebugFactRecord)
XR_TARGET_TABLE_ACCESSOR(program_graphs, XrTargetProgramGraphRecord)
XR_TARGET_TABLE_ACCESSOR(module_partitions, XrTargetModulePartitionRecord)
#undef XR_TARGET_TABLE_ACCESSOR
