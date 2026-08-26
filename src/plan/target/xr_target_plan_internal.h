/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_plan_internal.h - Mutable construction storage for TargetPlan
 */

#ifndef XR_TARGET_PLAN_INTERNAL_H
#define XR_TARGET_PLAN_INTERNAL_H

#include "xr_target_plan.h"
#include <stdatomic.h>

/* Mutable draft storage is private to the unified target builder and tests. */
typedef struct XrTargetPlanDraft {
    const XrSemanticPlan *semantic_plan;
    const XrSemanticPlan *const *semantic_dependencies;
    uint32_t semantic_dependency_count;
    const XrSemanticPlan *const *semantic_modules;
    uint32_t semantic_module_count;
    XrTargetProfile *profile;
    uint64_t completed_family_mask;
    const XrTargetMachineRepRecord *machine_reps;
    uint32_t machine_reps_count;
    const XrTargetValueRepRecord *value_reps;
    uint32_t value_reps_count;
    const XrTargetExtentRecord *extents;
    uint32_t extents_count;
    const XrTargetLayoutRecord *layouts;
    uint32_t layouts_count;
    const XrTargetFieldRecord *fields;
    uint32_t fields_count;
    const XrTargetStorageRecord *storage;
    uint32_t storage_count;
    const XrTargetAllocationRecord *allocations;
    uint32_t allocations_count;
    const XrTargetExtentOperandRecord *extent_operands;
    uint32_t extent_operands_count;
    const XrTargetFunctionRecord *functions;
    uint32_t functions_count;
    const XrTargetSlotRecord *slots;
    uint32_t slots_count;
    const XrTargetInstructionRecord *instructions;
    uint32_t instructions_count;
    const XrTargetCallRecord *calls;
    uint32_t calls_count;
    const XrTargetCallArgumentRecord *call_arguments;
    uint32_t call_arguments_count;
    const XrTargetRootMapRecord *root_maps;
    uint32_t root_maps_count;
    const uint32_t *root_slots;
    uint32_t root_slots_count;
    const XrTargetCleanupRecord *cleanups;
    uint32_t cleanups_count;
    const XrTargetAdapterRecord *adapters;
    uint32_t adapters_count;
    const XrTargetCapabilityRecord *capabilities;
    uint32_t capabilities_count;
    const XrTargetCoroutineStateRecord *coroutines;
    uint32_t coroutines_count;
    const XrTargetEntryExpectationRecord *entry_expectations;
    uint32_t entry_expectations_count;
    const XrTargetDebugFactRecord *debug_facts;
    uint32_t debug_facts_count;
    const XrTargetProgramGraphRecord *program_graphs;
    uint32_t program_graphs_count;
    const XrTargetModulePartitionRecord *module_partitions;
    uint32_t module_partitions_count;
} XrTargetPlanDraft;

XR_FUNC bool xr_target_plan_freeze(const XrTargetPlanDraft *draft, XrTargetPlan **out,
                                   char *error, size_t error_size);

struct XrTargetPlan {
    atomic_uint_least32_t references;
    uint32_t schema_version;
    uint64_t completed_family_mask;
    bool frozen;
    bool verified;
    XrFingerprint fingerprint;
    XrFingerprint semantic_fingerprint;
    XrSemanticPlan *semantic_plan;
    XrSemanticPlan **semantic_dependencies;
    uint32_t semantic_dependency_count;
    XrSemanticPlan **semantic_modules;
    uint32_t semantic_module_count;
    XrTargetProfile *profile;
#define XR_TARGET_TABLE_FIELD(name, type)                                                          \
    type *name;                                                                                    \
    uint32_t name##_count
    XR_TARGET_TABLE_FIELD(machine_reps, XrTargetMachineRepRecord);
    XR_TARGET_TABLE_FIELD(value_reps, XrTargetValueRepRecord);
    XR_TARGET_TABLE_FIELD(extents, XrTargetExtentRecord);
    XR_TARGET_TABLE_FIELD(layouts, XrTargetLayoutRecord);
    XR_TARGET_TABLE_FIELD(fields, XrTargetFieldRecord);
    XR_TARGET_TABLE_FIELD(storage, XrTargetStorageRecord);
    XR_TARGET_TABLE_FIELD(allocations, XrTargetAllocationRecord);
    XR_TARGET_TABLE_FIELD(extent_operands, XrTargetExtentOperandRecord);
    XR_TARGET_TABLE_FIELD(functions, XrTargetFunctionRecord);
    XR_TARGET_TABLE_FIELD(slots, XrTargetSlotRecord);
    XR_TARGET_TABLE_FIELD(instructions, XrTargetInstructionRecord);
    XR_TARGET_TABLE_FIELD(calls, XrTargetCallRecord);
    XR_TARGET_TABLE_FIELD(call_arguments, XrTargetCallArgumentRecord);
    XR_TARGET_TABLE_FIELD(root_maps, XrTargetRootMapRecord);
    XR_TARGET_TABLE_FIELD(root_slots, uint32_t);
    XR_TARGET_TABLE_FIELD(cleanups, XrTargetCleanupRecord);
    XR_TARGET_TABLE_FIELD(adapters, XrTargetAdapterRecord);
    XR_TARGET_TABLE_FIELD(capabilities, XrTargetCapabilityRecord);
    XR_TARGET_TABLE_FIELD(coroutines, XrTargetCoroutineStateRecord);
    XR_TARGET_TABLE_FIELD(entry_expectations, XrTargetEntryExpectationRecord);
    XR_TARGET_TABLE_FIELD(debug_facts, XrTargetDebugFactRecord);
    XR_TARGET_TABLE_FIELD(program_graphs, XrTargetProgramGraphRecord);
    XR_TARGET_TABLE_FIELD(module_partitions, XrTargetModulePartitionRecord);
#undef XR_TARGET_TABLE_FIELD
};

XR_FUNC void xr_target_plan_compute_fingerprint(const XrTargetPlan *plan, XrFingerprint *out);
XR_FUNC bool xr_target_semantic_program_module_set_verify(
    const XrSemanticPlan *const *modules, uint32_t module_count,
    char *error, size_t error_size);
XR_FUNC bool xr_target_plan_program_module_set_fingerprint(const XrTargetPlan *plan,
                                                           XrFingerprint *out);
XR_FUNC bool xr_target_semantic_program_module_verify_fragment(
    const XrSemanticPlan *const *modules, uint32_t module_count,
    uint32_t program_module_row, char *error, size_t error_size);
XR_FUNC bool xr_target_semantic_program_module_direct_dependencies(
    const XrSemanticPlan *const *modules, uint32_t module_count,
    uint32_t program_module_row, const XrSemanticPlan ***dependencies,
    uint32_t *dependency_count, char *error, size_t error_size);
XR_FUNC bool xr_target_semantic_module_set_fingerprint(
    const XrSemanticPlan *const *modules, uint32_t module_count, XrFingerprint *out);
XR_FUNC bool xr_target_semantic_capability_requirements(
    const XrSemanticPlan *const *modules, uint32_t module_count,
    const XrTargetProfile *profile, uint64_t *expected_mask,
    char *error, size_t error_size);
XR_FUNC void xr_target_layout_compute_fingerprint(const XrTargetPlan *plan, uint32_t layout,
                                                  XrFingerprint *out);
XR_FUNC void xr_target_call_compute_fingerprint(const XrTargetPlan *plan, uint32_t call,
                                                XrFingerprint *out);

#endif  // XR_TARGET_PLAN_INTERNAL_H
