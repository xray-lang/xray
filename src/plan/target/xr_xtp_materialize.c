/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xtp_materialize.c - Private typed artifact to verified TargetPlan bridge
 *
 * KEY CONCEPT:
 *   Materialization copies exact typed rows into a private draft and invokes
 *   the independent freeze/verifier boundary. It never invokes the production
 *   intent builder and never performs runtime activation.
 */

#include "xr_target_plan_internal.h"
#include "xr_target_verify.h"
#include "../format/xr_xtp_internal.h"
#include "../semantic/xr_program_semantic_closure.h"
#include "../semantic/xr_semantic_verify.h"
#include "../../base/xmalloc.h"
#include <string.h>

typedef struct XrXtpDecodedTables {
    XrTargetProfileDraft profile;
#define XR_XTP_TABLE_FIELD(name, type) type *name; uint32_t name##_count
    XR_XTP_TABLE_FIELD(machine_reps, XrTargetMachineRepRecord);
    XR_XTP_TABLE_FIELD(value_reps, XrTargetValueRepRecord);
    XR_XTP_TABLE_FIELD(extents, XrTargetExtentRecord);
    XR_XTP_TABLE_FIELD(layouts, XrTargetLayoutRecord);
    XR_XTP_TABLE_FIELD(fields, XrTargetFieldRecord);
    XR_XTP_TABLE_FIELD(storage, XrTargetStorageRecord);
    XR_XTP_TABLE_FIELD(allocations, XrTargetAllocationRecord);
    XR_XTP_TABLE_FIELD(extent_operands, XrTargetExtentOperandRecord);
    XR_XTP_TABLE_FIELD(functions, XrTargetFunctionRecord);
    XR_XTP_TABLE_FIELD(slots, XrTargetSlotRecord);
    XR_XTP_TABLE_FIELD(instructions, XrTargetInstructionRecord);
    XR_XTP_TABLE_FIELD(calls, XrTargetCallRecord);
    XR_XTP_TABLE_FIELD(call_arguments, XrTargetCallArgumentRecord);
    XR_XTP_TABLE_FIELD(root_maps, XrTargetRootMapRecord);
    XR_XTP_TABLE_FIELD(root_slots, uint32_t);
    XR_XTP_TABLE_FIELD(cleanups, XrTargetCleanupRecord);
    XR_XTP_TABLE_FIELD(adapters, XrTargetAdapterRecord);
    XR_XTP_TABLE_FIELD(capabilities, XrTargetCapabilityRecord);
    XR_XTP_TABLE_FIELD(coroutines, XrTargetCoroutineStateRecord);
    XR_XTP_TABLE_FIELD(entry_expectations, XrTargetEntryExpectationRecord);
    XR_XTP_TABLE_FIELD(debug_facts, XrTargetDebugFactRecord);
    XR_XTP_TABLE_FIELD(module_partitions, XrTargetModulePartitionRecord);
    XR_XTP_TABLE_FIELD(program_graphs, XrTargetProgramGraphRecord);
#undef XR_XTP_TABLE_FIELD
} XrXtpDecodedTables;

static void dispose_tables(XrXtpDecodedTables *tables) {
#define XR_XTP_FREE_TABLE(name) xr_free(tables->name)
    XR_XTP_FREE_TABLE(machine_reps);
    XR_XTP_FREE_TABLE(value_reps);
    XR_XTP_FREE_TABLE(extents);
    XR_XTP_FREE_TABLE(layouts);
    XR_XTP_FREE_TABLE(fields);
    XR_XTP_FREE_TABLE(storage);
    XR_XTP_FREE_TABLE(allocations);
    XR_XTP_FREE_TABLE(extent_operands);
    XR_XTP_FREE_TABLE(functions);
    XR_XTP_FREE_TABLE(slots);
    XR_XTP_FREE_TABLE(instructions);
    XR_XTP_FREE_TABLE(calls);
    XR_XTP_FREE_TABLE(call_arguments);
    XR_XTP_FREE_TABLE(root_maps);
    XR_XTP_FREE_TABLE(root_slots);
    XR_XTP_FREE_TABLE(cleanups);
    XR_XTP_FREE_TABLE(adapters);
    XR_XTP_FREE_TABLE(capabilities);
    XR_XTP_FREE_TABLE(coroutines);
    XR_XTP_FREE_TABLE(entry_expectations);
    XR_XTP_FREE_TABLE(debug_facts);
    XR_XTP_FREE_TABLE(module_partitions);
    XR_XTP_FREE_TABLE(program_graphs);
#undef XR_XTP_FREE_TABLE
    memset(tables, 0, sizeof(*tables));
}

static bool fingerprint_matches(XrFingerprint left, XrFingerprint right) {
    return xr_fingerprint_equal(left, right);
}

static bool validate_requirements(const XrXtpCandidate *candidate,
                                  const XrSemanticPlan *semantic_plan,
                                  const XrSemanticPlan *const *dependencies,
                                  uint32_t dependency_count,
                                  const XrTargetProfile *expected_profile,
                                  char *error, size_t error_size) {
    if (!candidate || !semantic_plan || !expected_profile) {
        xr_xtp_set_error(error, error_size, "XR_ARTIFACT_2004",
                         "materialization requirements are incomplete");
        return false;
    }
    if (dependency_count > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_DEPENDENCIES ||
        dependency_count != xr_semantic_plan_dependency_count(semantic_plan)) {
        xr_xtp_set_error(error, error_size, "XR_TARGET_1000",
                         "materialization requirements are not verified");
        return false;
    }
    char nested_error[512] = {0};
    bool semantic_verified = dependency_count == 0
                                 ? xr_semantic_plan_verify(
                                       semantic_plan, nested_error,
                                       sizeof(nested_error))
                                 : xr_semantic_plan_verify_module_set(
                                       semantic_plan, dependencies,
                                       dependency_count, nested_error,
                                       sizeof(nested_error));
    if (!semantic_verified ||
        !xr_target_profile_verify(expected_profile, nested_error, sizeof(nested_error))) {
        xr_xtp_set_error(error, error_size, "XR_TARGET_1000",
                         "materialization requirements are not verified");
        return false;
    }
    const XrTargetProfileDraft *facts = xr_target_profile_facts(expected_profile);
    const XrXtpIdentity *identity = &candidate->identity;
    XrFingerprint semantic_fingerprint = xr_semantic_plan_fingerprint(semantic_plan);
    const XrXtpSectionView *program_graphs =
        xr_xtp_candidate_section(candidate, XR_XTP_SECTION_PROGRAM_GRAPHS);
    const XrXtpSectionView *module_partitions =
        xr_xtp_candidate_section(candidate, XR_XTP_SECTION_MODULE_PARTITIONS);
    bool graph_module_set = program_graphs && module_partitions &&
                            (program_graphs->count || module_partitions->count);
    if ((!program_graphs || !module_partitions) ||
        (graph_module_set &&
         (dependency_count >= XR_PROGRAM_SEMANTIC_CLOSURE_MAX_MODULES ||
          !xr_target_semantic_module_set_fingerprint(
              semantic_plan, dependencies, dependency_count, &semantic_fingerprint)))) {
        xr_xtp_set_error(error, error_size, "XR_TARGET_1000",
                         "artifact module-set identity cannot be established");
        return false;
    }
    if (!facts || identity->semantic_schema != xr_semantic_plan_schema(semantic_plan) ||
        identity->profile_schema != facts->schema_version ||
        identity->plan_schema != XR_TARGET_PLAN_SCHEMA_VERSION ||
        identity->completed_family_mask != XR_TARGET_REQUIRED_FAMILIES ||
        !fingerprint_matches(identity->semantic_fingerprint, semantic_fingerprint) ||
        !fingerprint_matches(identity->operation_registry_fingerprint,
                             xr_semantic_plan_operation_registry_fingerprint(semantic_plan)) ||
        !fingerprint_matches(identity->profile_fingerprint,
                             xr_target_profile_fingerprint(expected_profile)) ||
        !fingerprint_matches(identity->runtime_fingerprint, facts->runtime_abi_fingerprint) ||
        !fingerprint_matches(identity->provider_fingerprint,
                             facts->provider_set_fingerprint) ||
        !fingerprint_matches(identity->object_fingerprint,
                             facts->object_header_fingerprint)) {
        xr_xtp_set_error(error, error_size, "XR_TARGET_1000",
                         "artifact identity does not match exact requirements");
        return false;
    }
    return true;
}

static bool decode_profile(const XrXtpCandidate *candidate, XrXtpDecodedTables *tables,
                           char *error, size_t error_size) {
    const XrXtpSectionView *section =
        xr_xtp_candidate_section(candidate, XR_XTP_SECTION_TARGET_PROFILE);
    if (!section || section->count != 1 ||
        !xr_xtp_decode_rows(section->kind, candidate->bytes + section->offset, section->count,
                            &tables->profile)) {
        xr_xtp_set_error(error, error_size, "XR_ARTIFACT_2003",
                         "target profile table cannot be decoded");
        return false;
    }
    return true;
}

XR_FUNC bool xr_xtp_runtime_peak_within_budget(size_t artifact_bytes,
                                               size_t decoded_bytes) {
    return artifact_bytes <= XR_XTP_MAX_RUNTIME_LOAD_PEAK_BYTES / 2u &&
           decoded_bytes <= XR_XTP_MAX_RUNTIME_LOAD_PEAK_BYTES / 2u -
                                artifact_bytes;
}

static bool decoded_storage_within_budget(const XrXtpCandidate *candidate,
                                          size_t *decoded_bytes) {
    size_t total = sizeof(XrTargetProfileDraft);
#define XR_XTP_ADD_DECODED_BYTES(kind, type)                                                       \
    do {                                                                                           \
        const XrXtpSectionView *section =                                                         \
            xr_xtp_candidate_section(candidate, XR_XTP_SECTION_##kind);                           \
        if (!section || section->count > (XR_XTP_MAX_DECODED_TABLE_BYTES - total) / sizeof(type)) \
            return false;                                                                          \
        total += (size_t) section->count * sizeof(type);                                           \
    } while (0)
    XR_XTP_ADD_DECODED_BYTES(MACHINE_REPS, XrTargetMachineRepRecord);
    XR_XTP_ADD_DECODED_BYTES(VALUE_REPS, XrTargetValueRepRecord);
    XR_XTP_ADD_DECODED_BYTES(EXTENTS, XrTargetExtentRecord);
    XR_XTP_ADD_DECODED_BYTES(LAYOUTS, XrTargetLayoutRecord);
    XR_XTP_ADD_DECODED_BYTES(FIELDS, XrTargetFieldRecord);
    XR_XTP_ADD_DECODED_BYTES(STORAGE, XrTargetStorageRecord);
    XR_XTP_ADD_DECODED_BYTES(ALLOCATIONS, XrTargetAllocationRecord);
    XR_XTP_ADD_DECODED_BYTES(EXTENT_OPERANDS, XrTargetExtentOperandRecord);
    XR_XTP_ADD_DECODED_BYTES(FUNCTIONS, XrTargetFunctionRecord);
    XR_XTP_ADD_DECODED_BYTES(SLOTS, XrTargetSlotRecord);
    XR_XTP_ADD_DECODED_BYTES(INSTRUCTIONS, XrTargetInstructionRecord);
    XR_XTP_ADD_DECODED_BYTES(CALLS, XrTargetCallRecord);
    XR_XTP_ADD_DECODED_BYTES(CALL_ARGUMENTS, XrTargetCallArgumentRecord);
    XR_XTP_ADD_DECODED_BYTES(ROOT_MAPS, XrTargetRootMapRecord);
    XR_XTP_ADD_DECODED_BYTES(ROOT_SLOTS, uint32_t);
    XR_XTP_ADD_DECODED_BYTES(CLEANUPS, XrTargetCleanupRecord);
    XR_XTP_ADD_DECODED_BYTES(ADAPTERS, XrTargetAdapterRecord);
    XR_XTP_ADD_DECODED_BYTES(CAPABILITIES, XrTargetCapabilityRecord);
    XR_XTP_ADD_DECODED_BYTES(COROUTINES, XrTargetCoroutineStateRecord);
    XR_XTP_ADD_DECODED_BYTES(ENTRY_EXPECTATIONS, XrTargetEntryExpectationRecord);
    XR_XTP_ADD_DECODED_BYTES(DEBUG_FACTS, XrTargetDebugFactRecord);
    XR_XTP_ADD_DECODED_BYTES(MODULE_PARTITIONS, XrTargetModulePartitionRecord);
    XR_XTP_ADD_DECODED_BYTES(PROGRAM_GRAPHS, XrTargetProgramGraphRecord);
#undef XR_XTP_ADD_DECODED_BYTES
    *decoded_bytes = total;
    return true;
}

static bool allocate_and_decode(const XrXtpCandidate *candidate, XrXtpSectionKind kind,
                                size_t element_size, void **storage, uint32_t *count,
                                char *error, size_t error_size) {
    *storage = NULL;
    *count = 0;
    const XrXtpSectionView *section = xr_xtp_candidate_section(candidate, kind);
    if (!section || section->count > SIZE_MAX / element_size) {
        xr_xtp_set_error(error, error_size, "XR_EXEC_5003",
                         "typed table allocation size overflows");
        return false;
    }
    if (section->count) {
        *storage = xr_calloc(section->count, element_size);
        if (!*storage) {
            xr_xtp_set_error(error, error_size, "XR_EXEC_5003",
                             "typed table allocation failed");
            return false;
        }
    }
    *count = section->count;
    bool decoded = kind == XR_XTP_SECTION_INSTRUCTIONS
                       ? xr_xtp_instruction_stream_decode(
                             candidate->bytes + section->offset,
                             section->length, section->count,
                             (XrTargetInstructionRecord *) *storage)
                       : xr_xtp_decode_rows(kind,
                                            candidate->bytes + section->offset,
                                            section->count, *storage);
    if (!decoded) {
        xr_free(*storage);
        *storage = NULL;
        *count = 0;
        xr_xtp_set_error(error, error_size, "XR_ARTIFACT_2003",
                         "typed table cannot be decoded");
        return false;
    }
    return true;
}

static bool decode_tables(const XrXtpCandidate *candidate, XrXtpDecodedTables *tables,
                          char *error, size_t error_size) {
    if (!decode_profile(candidate, tables, error, error_size))
        return false;
#define XR_XTP_DECODE_TABLE(name, type, kind)                                                      \
    if (!allocate_and_decode(candidate, XR_XTP_SECTION_##kind, sizeof(type),                       \
                             (void **) &tables->name, &tables->name##_count, error, error_size))  \
        return false
    XR_XTP_DECODE_TABLE(machine_reps, XrTargetMachineRepRecord, MACHINE_REPS);
    XR_XTP_DECODE_TABLE(value_reps, XrTargetValueRepRecord, VALUE_REPS);
    XR_XTP_DECODE_TABLE(extents, XrTargetExtentRecord, EXTENTS);
    XR_XTP_DECODE_TABLE(layouts, XrTargetLayoutRecord, LAYOUTS);
    XR_XTP_DECODE_TABLE(fields, XrTargetFieldRecord, FIELDS);
    XR_XTP_DECODE_TABLE(storage, XrTargetStorageRecord, STORAGE);
    XR_XTP_DECODE_TABLE(allocations, XrTargetAllocationRecord, ALLOCATIONS);
    XR_XTP_DECODE_TABLE(extent_operands, XrTargetExtentOperandRecord, EXTENT_OPERANDS);
    XR_XTP_DECODE_TABLE(functions, XrTargetFunctionRecord, FUNCTIONS);
    XR_XTP_DECODE_TABLE(slots, XrTargetSlotRecord, SLOTS);
    XR_XTP_DECODE_TABLE(instructions, XrTargetInstructionRecord, INSTRUCTIONS);
    XR_XTP_DECODE_TABLE(calls, XrTargetCallRecord, CALLS);
    XR_XTP_DECODE_TABLE(call_arguments, XrTargetCallArgumentRecord, CALL_ARGUMENTS);
    XR_XTP_DECODE_TABLE(root_maps, XrTargetRootMapRecord, ROOT_MAPS);
    XR_XTP_DECODE_TABLE(root_slots, uint32_t, ROOT_SLOTS);
    XR_XTP_DECODE_TABLE(cleanups, XrTargetCleanupRecord, CLEANUPS);
    XR_XTP_DECODE_TABLE(adapters, XrTargetAdapterRecord, ADAPTERS);
    XR_XTP_DECODE_TABLE(capabilities, XrTargetCapabilityRecord, CAPABILITIES);
    XR_XTP_DECODE_TABLE(coroutines, XrTargetCoroutineStateRecord, COROUTINES);
    XR_XTP_DECODE_TABLE(entry_expectations, XrTargetEntryExpectationRecord,
                        ENTRY_EXPECTATIONS);
    XR_XTP_DECODE_TABLE(debug_facts, XrTargetDebugFactRecord, DEBUG_FACTS);
    XR_XTP_DECODE_TABLE(module_partitions, XrTargetModulePartitionRecord,
                        MODULE_PARTITIONS);
    XR_XTP_DECODE_TABLE(program_graphs, XrTargetProgramGraphRecord, PROGRAM_GRAPHS);
#undef XR_XTP_DECODE_TABLE
    return true;
}

static uint64_t compute_total_frame_bytes(const XrXtpDecodedTables *tables) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < tables->functions_count; i++) {
        if (total > UINT64_MAX - tables->functions[i].frame_size)
            return UINT64_MAX;
        total += tables->functions[i].frame_size;
    }
    return total;
}

static XrTargetPlanDraft make_draft(const XrXtpDecodedTables *tables,
                                    const XrSemanticPlan *semantic_plan,
                                    const XrSemanticPlan *const *dependencies,
                                    uint32_t dependency_count,
                                    XrTargetProfile *profile) {
    XrTargetPlanDraft draft = {
        .semantic_plan = semantic_plan,
        .semantic_dependencies = dependencies,
        .semantic_dependency_count = dependency_count,
        .profile = profile,
        .completed_family_mask = XR_TARGET_REQUIRED_FAMILIES,
#define XR_XTP_DRAFT_TABLE(name) .name = tables->name, .name##_count = tables->name##_count
        XR_XTP_DRAFT_TABLE(machine_reps),
        XR_XTP_DRAFT_TABLE(value_reps),
        XR_XTP_DRAFT_TABLE(extents),
        XR_XTP_DRAFT_TABLE(layouts),
        XR_XTP_DRAFT_TABLE(fields),
        XR_XTP_DRAFT_TABLE(storage),
        XR_XTP_DRAFT_TABLE(allocations),
        XR_XTP_DRAFT_TABLE(extent_operands),
        XR_XTP_DRAFT_TABLE(functions),
        XR_XTP_DRAFT_TABLE(slots),
        XR_XTP_DRAFT_TABLE(instructions),
        XR_XTP_DRAFT_TABLE(calls),
        XR_XTP_DRAFT_TABLE(call_arguments),
        XR_XTP_DRAFT_TABLE(root_maps),
        XR_XTP_DRAFT_TABLE(root_slots),
        XR_XTP_DRAFT_TABLE(cleanups),
        XR_XTP_DRAFT_TABLE(adapters),
        XR_XTP_DRAFT_TABLE(capabilities),
        XR_XTP_DRAFT_TABLE(coroutines),
        XR_XTP_DRAFT_TABLE(entry_expectations),
        XR_XTP_DRAFT_TABLE(debug_facts),
        XR_XTP_DRAFT_TABLE(module_partitions),
        XR_XTP_DRAFT_TABLE(program_graphs),
#undef XR_XTP_DRAFT_TABLE
    };
    return draft;
}

static bool derived_fingerprints_match(const XrXtpDecodedTables *tables,
                                       const XrTargetPlan *plan) {
    uint32_t count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(plan, &count);
    if (count != tables->layouts_count)
        return false;
    for (uint32_t i = 0; i < count; i++)
        if (!fingerprint_matches(layouts[i].fingerprint, tables->layouts[i].fingerprint))
            return false;
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &count);
    if (count != tables->calls_count)
        return false;
    for (uint32_t i = 0; i < count; i++)
        if (!fingerprint_matches(calls[i].fingerprint, tables->calls[i].fingerprint))
            return false;
    return true;
}

XR_FUNC bool xr_xtp_materialize_target_plan(const XrXtpCandidate *candidate,
                                            const XrSemanticPlan *semantic_plan,
                                            const XrTargetProfile *expected_profile,
                                            XrTargetPlan **plan, char *error,
                                            size_t error_size) {
    return xr_xtp_materialize_target_plan_module_set(
        candidate, semantic_plan, NULL, 0, expected_profile, plan, error,
        error_size);
}

XR_FUNC bool xr_xtp_materialize_target_plan_module_set(
    const XrXtpCandidate *candidate, const XrSemanticPlan *semantic_plan,
    const XrSemanticPlan *const *dependencies, uint32_t dependency_count,
    const XrTargetProfile *expected_profile, XrTargetPlan **plan, char *error,
    size_t error_size) {
    if (plan)
        *plan = NULL;
    if (!plan || !validate_requirements(candidate, semantic_plan, dependencies,
                                        dependency_count, expected_profile,
                                        error, error_size))
        return false;
    size_t decoded_bytes = 0;
    if (!decoded_storage_within_budget(candidate, &decoded_bytes) ||
        !xr_xtp_runtime_peak_within_budget(candidate->size, decoded_bytes)) {
        xr_xtp_set_error(error, error_size, "XR_EXEC_5003",
                         "read, snapshot, decode, and freeze peak exceeds its hard budget");
        return false;
    }
    XrXtpDecodedTables tables = {0};
    if (!decode_tables(candidate, &tables, error, error_size)) {
        dispose_tables(&tables);
        return false;
    }
    if (compute_total_frame_bytes(&tables) != candidate->resources.total_frame_bytes) {
        dispose_tables(&tables);
        xr_xtp_set_error(error, error_size, "XR_ARTIFACT_2001",
                         "frame byte manifest does not match functions");
        return false;
    }
    XrTargetProfile *decoded_profile = NULL;
    if (!xr_target_profile_freeze(&tables.profile, &decoded_profile, error, error_size) ||
        !fingerprint_matches(xr_target_profile_fingerprint(decoded_profile),
                             candidate->identity.profile_fingerprint) ||
        !fingerprint_matches(xr_target_profile_fingerprint(decoded_profile),
                             xr_target_profile_fingerprint(expected_profile))) {
        xr_target_profile_free(decoded_profile);
        dispose_tables(&tables);
        if (!error || !error_size || !error[0])
            xr_xtp_set_error(error, error_size, "XR_TARGET_1000",
                             "decoded target profile identity is invalid");
        return false;
    }
    XrTargetPlanDraft draft = make_draft(&tables, semantic_plan, dependencies,
                                         dependency_count, decoded_profile);
    XrTargetPlan *materialized = NULL;
    bool frozen = xr_target_plan_freeze(&draft, &materialized, error, error_size);
    xr_target_profile_free(decoded_profile);
    if (!frozen || !materialized || !xr_target_plan_is_verified(materialized) ||
        xr_target_plan_completed_family_mask(materialized) != XR_TARGET_REQUIRED_FAMILIES ||
        !fingerprint_matches(xr_target_plan_fingerprint(materialized),
                             candidate->identity.plan_fingerprint) ||
        !derived_fingerprints_match(&tables, materialized)) {
        xr_target_plan_free(materialized);
        dispose_tables(&tables);
        if (!error || !error_size || !error[0])
            xr_xtp_set_error(error, error_size, "XR_TARGET_1000",
                             "typed TargetPlan failed exact verification");
        return false;
    }
    dispose_tables(&tables);
    *plan = materialized;
    return true;
}
