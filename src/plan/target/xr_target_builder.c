/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_builder.c - Composable TargetPlan construction
 *
 * KEY CONCEPT:
 *   Family collectors append backend-neutral intents only. A single final
 *   canonical materialization assigns dense IDs, packs frames, and freezes
 *   the immutable plan, so later families never depend on append order.
 */

#include "../semantic/xr_semantic_heap_literal_shape.h"
#include "xr_target_builder.h"
#include "xr_target_capability.h"
#include "xr_target_instruction_verify.h"
#include "xr_target_entry_abi.h"
#include "xr_target_plan_internal.h"
#include "../semantic/xr_semantic_enum_shape.h"
#include "../semantic/xr_semantic_range_slice_shape.h"
#include "xr_target_profile_internal.h"
#include "../../base/xmalloc.h"
#include "../../frontend/analyzer/xa_intrinsic_registry.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../semantic/xr_semantic_graph.h"
#include "../semantic/xr_semantic_verify.h"
#include "../semantic/xr_semantic_allocation_shape.h"
#include "../semantic/xr_semantic_array_type_shape.h"
#include "../semantic/xr_semantic_class_shape.h"
#include "../semantic/xr_semantic_coroutine_lifecycle_shape.h"
#include "../semantic/xr_semantic_string_shape.h"
#include "../semantic/xr_semantic_cleanup_shape.h"
#include "../semantic/xr_semantic_task_shape.h"
#include "../semantic/xr_semantic_string_runes_shape.h"
#include "../semantic/xr_semantic_iterator_rune_has_next_shape.h"
#include "../semantic/xr_semantic_iterator_rune_next_shape.h"
#include "../semantic/xr_semantic_map_entry_iterator_shape.h"
#include "../semantic/xr_semantic_iterator_rune_nth_shape.h"
#include "../semantic/xr_semantic_rune_to_string_shape.h"
#include "../semantic/xr_semantic_rune_to_uint32_shape.h"
#include "../semantic/xr_semantic_rune_is_whitespace_shape.h"
#include "../semantic/xr_semantic_string_slice_shape.h"
#include "../semantic/xr_semantic_native_module_shape.h"
#include "../semantic/xr_semantic_container_copy_shape.h"
#include "../semantic/xr_semantic_identity_copy_shape.h"
#include "../semantic/xr_semantic_owner_forward_shape.h"
#include "../semantic/xr_semantic_dynamic_value_shape.h"
#include "../semantic/xr_semantic_const_variant_shape.h"
#include "../semantic/xr_semantic_direct_callee_shape.h"
#include "../semantic/xr_semantic_local_call_target_shape.h"
#include "../semantic/xr_semantic_class_seal_shape.h"
#include "xr_target_scalar_rep_shape.h"
#include "../semantic/xr_semantic_local_addr_shape.h"
#include "../semantic/xr_semantic_panic_catch_shape.h"
#include "../semantic/xr_semantic_type_admission_shape.h"
#include "../semantic/xr_semantic_panic_info_shape.h"
#include "../semantic/xr_semantic_scalar_copy_shape.h"
#include "../semantic/xr_program_semantic_closure.h"
#include "../semantic/xr_semantic_value_aggregate_shape.h"
#include "../ownership/xr_ownership_certificate.h"
#include "../../runtime/value/xtype.h"
#include "../../stdlib/xstdlib_metadata.h"
#include "../semantic/xr_semantic_array_member_shape.h"
#include "xr_target_array_storage_shape.h"
#include "../../shared/xr_align_guard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum XrTargetScalarEligibility {
    XR_TARGET_SCALAR_INVALID = -1,
    XR_TARGET_SCALAR_NOT_APPLICABLE = 0,
    XR_TARGET_SCALAR_VALUE = 1,
} XrTargetScalarEligibility;

typedef struct XrTargetRepIntent {
    XrTargetMachineRepRecord record;
} XrTargetRepIntent;

typedef struct XrTargetLayoutIntent {
    uint32_t semantic_type;
    XrTargetMachineRepRecord memory_rep;
    uint32_t element_count;
    uint8_t kind;
    uint8_t array_element_storage;
} XrTargetLayoutIntent;

typedef struct XrTargetValueIntent {
    uint32_t semantic_value;
    uint32_t semantic_function;
    uint32_t semantic_type;
    XrTargetMachineRepRecord register_rep;
    XrTargetMachineRepRecord memory_rep;
    XrStableId slot_identity;
    bool has_slot;
    bool resolve_type_rep;
    /* The family that claimed this value. Two families that each prove storage
     * for the same value are a coverage overlap, not an allocation failure, and
     * the refusal can only say which two if the claim carries its author. */
    uint64_t family;
} XrTargetValueIntent;

typedef struct XrTargetSlotIntent {
    XrStableId identity;
    uint32_t function;
    uint32_t semantic_value;
    uint32_t semantic_operation;
    uint32_t logical_slot;
    XrTargetMachineRepRecord register_rep;
    XrTargetMachineRepRecord memory_rep;
    uint8_t role;
    uint8_t root_kind;
    uint8_t ownership;
    uint32_t debug_variable;
    uint32_t semantic_type;
    bool resolve_type_rep;
} XrTargetSlotIntent;

typedef struct XrTargetCallIntent {
    XrStableId identity;
    uint32_t semantic_call_target;
    uint32_t semantic_operation;
    uint32_t caller_function;
    uint32_t callee_function;
    uint32_t source_dependency;
    uint32_t source_export;
    XrStableId source_export_identity;
    XrStableId source_callee_identity;
    uint32_t result_value;
    uint32_t argument_begin;
    uint16_t argument_count;
    uint8_t result_mode;
    uint8_t result_ownership;
    uint8_t calling_convention;
    uint8_t target_kind;
    uint8_t array_intrinsic_kind;
    uint8_t array_element_storage;
    uint8_t array_hof_kind;
    uint8_t array_result_element_storage;
    bool suspends;
    bool tail;
} XrTargetCallIntent;

typedef struct XrTargetCallArgumentIntent {
    XrStableId identity;
    uint32_t call_intent;
    uint32_t semantic_operand;
    uint32_t semantic_value;
    uint32_t caller_storage_value;
    uint32_t callee_parameter;
    uint16_t ordinal;
    uint8_t mode;
    uint8_t ownership;
    uint8_t transfer_mode;
    uint8_t flags;
    uint8_t array_element_storage;
} XrTargetCallArgumentIntent;

typedef struct XrTargetValueStorageAnalysis {
    uint8_t *defined_values;
    uint8_t *used_types;
    uint32_t *value_types;
    uint32_t *value_functions;
    uint32_t *value_operations;
    uint16_t *type_rep_kinds;
    uint32_t total_values;
    uint32_t type_count;
} XrTargetValueStorageAnalysis;

typedef struct XrDirectLocalCalleeStorageAnalysis {
    uint32_t *target_by_operation;
    uint32_t *target_by_value;
    uint32_t *use_count_by_value;
    uint8_t *invalid_value;
    uint32_t operation_count;
    uint32_t value_count;
} XrDirectLocalCalleeStorageAnalysis;

/* A shared slot is allocated once, on the module root, and every XI_GET_SHARED
 * or XI_SET_SHARED names a slot in that single module-wide table whatever
 * function the instruction happens to sit in. The slot index alone is therefore
 * the slot's identity, and this table is indexed by it directly. The function
 * holding the store is recorded rather than matched, so the judgement below can
 * require it to be the callee's lexical owner instead of assuming the loading
 * function stores its own callee. */
typedef struct XrDirectLocalGoStoreEntry {
    uint32_t function;
    uint32_t operation;
    uint8_t ambiguous;
} XrDirectLocalGoStoreEntry;

typedef struct XrDirectLocalGoCalleeStorageAnalysis {
    XrDirectLocalGoStoreEntry *stores;
    uint32_t slot_count;
    uint32_t *target_by_value;
    uint32_t *use_count_by_value;
    uint8_t *candidate_value;
    uint8_t *invalid_value;
    uint32_t operation_count;
    uint32_t value_count;
    XrSemanticGraph graph;
} XrDirectLocalGoCalleeStorageAnalysis;

typedef struct XrSourceNamespaceStorageAnalysis {
    uint8_t *exact_value;
    uint32_t *dependency_by_value;
    uint32_t value_count;
} XrSourceNamespaceStorageAnalysis;

typedef struct XrTargetMaterializedPlan {
    XrTargetMachineRepRecord *machine_reps;
    uint32_t machine_rep_count;
    XrTargetValueRepRecord *value_reps;
    uint32_t value_rep_count;
    XrTargetExtentRecord *extents;
    uint32_t extent_count;
    XrTargetLayoutRecord *layouts;
    uint32_t layout_count;
    XrTargetFieldRecord *fields;
    uint32_t field_count;
    XrTargetFunctionRecord *functions;
    uint32_t function_count;
    XrTargetSlotRecord *slots;
    uint32_t slot_count;
    XrTargetInstructionRecord *instructions;
    uint32_t instruction_count;
    XrTargetCallRecord *calls;
    uint32_t call_count;
    XrTargetCallArgumentRecord *call_arguments;
    uint32_t call_argument_count;
    XrTargetRootMapRecord *root_maps;
    uint32_t root_map_count;
    uint32_t *root_slots;
    uint32_t root_slot_count;
    XrTargetCleanupRecord *cleanups;
    uint32_t cleanup_count;
    XrTargetAdapterRecord *adapters;
    uint32_t adapter_count;
    XrTargetCapabilityRecord *capabilities;
    uint32_t capability_count;
    XrTargetCoroutineStateRecord *coroutines;
    uint32_t coroutine_count;
    XrTargetEntryExpectationRecord *entry_expectations;
    uint32_t entry_expectation_count;
    XrTargetDebugFactRecord *debug_facts;
    uint32_t debug_fact_count;
} XrTargetMaterializedPlan;

typedef struct XrTargetPlanBuilder XrTargetPlanBuilder;

struct XrTargetPlanBuilder {
    XrSemanticPlan *semantic_plan;
    XrSemanticPlan **semantic_dependencies;
    uint32_t semantic_dependency_count;
    XrTargetProfile *profile;
    XrTargetRepIntent *rep_intents;
    uint32_t rep_intent_count;
    uint32_t rep_intent_capacity;
    XrTargetLayoutIntent *layout_intents;
    uint32_t layout_intent_count;
    uint32_t layout_intent_capacity;
    XrTargetValueIntent *value_intents;
    uint32_t value_intent_count;
    uint32_t value_intent_capacity;
    uint64_t active_family;
    XrTargetSlotIntent *slot_intents;
    uint32_t slot_intent_count;
    uint32_t slot_intent_capacity;
    XrTargetCallIntent *call_intents;
    uint32_t call_intent_count;
    uint32_t call_intent_capacity;
    XrTargetCallArgumentIntent *call_argument_intents;
    uint32_t call_argument_intent_count;
    uint32_t call_argument_intent_capacity;
    uint64_t started_family_mask;
    uint64_t completed_family_mask;
    bool materialized;
    bool poisoned;
};

static void builder_free(XrTargetPlanBuilder *builder);

static bool fail(char *error, size_t error_size, const char *code, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "%s: %s", code, detail);
    return false;
}

/* Setting XRAY_COLLECT_ALL_REFUSALS makes this pass keep walking after a
 * refusal instead of stopping at the first one, printing every refusal it finds
 * on stderr under [refusal-survey]. The build still fails either way: a
 * judgement that refused stated no authority, so the plan is incomplete
 * whatever the rows after it answer. What changes is that one compile says how
 * many gaps a module has rather than only which one it reaches first, which is
 * the difference between reading a refusal as "this construct needs a fix" and
 * as "this construct needs five, and fixing one moves nothing".
 *
 * The walk continues over a builder that is missing the authority the refused
 * row would have stated, so a later refusal can be a consequence of an earlier
 * one rather than an independent gap. The survey is a census carrying that
 * bias, not a proof: its count is an upper bound on independent gaps, and what
 * it establishes firmly is which judgements a module reaches at all.
 *
 * Unset, the variable is never read: every call sits on a path that has already
 * refused, so a compile that succeeds pays nothing. */
static bool target_survey_enabled(void) {
    return getenv("XRAY_COLLECT_ALL_REFUSALS") != NULL;
}

static void target_survey_row(const char *family, const char *detail) {
    fprintf(stderr, "[refusal-survey] owner=target-plan-builder family=%s %s\n", family,
            detail && detail[0] ? detail : "refused without a detail");
}

/* Stable refusal-evidence bits. They describe which exact storage judgements
 * accepted a result or argument; the live refusal manifest freezes the numeric
 * mask together with the decision facts below, so adding a family requires a
 * new bit rather than reusing an old one. */
enum {
    XR_TARGET_SURVEY_STORAGE_SCALAR = 1u << 0,
    XR_TARGET_SURVEY_STORAGE_NULLABLE_SCALAR = 1u << 1,
    XR_TARGET_SURVEY_STORAGE_STRING = 1u << 2,
    XR_TARGET_SURVEY_STORAGE_ADT_ENUM = 1u << 3,
    XR_TARGET_SURVEY_STORAGE_UNIT_ENUM = 1u << 4,
    XR_TARGET_SURVEY_STORAGE_ARRAY = 1u << 5,
    XR_TARGET_SURVEY_STORAGE_CLASS_INSTANCE = 1u << 6,
    XR_TARGET_SURVEY_STORAGE_TAGGED_REFERENCE = 1u << 7,
    XR_TARGET_SURVEY_STORAGE_U8_SLICE = 1u << 8,
    XR_TARGET_SURVEY_STORAGE_LEAF_AGGREGATE = 1u << 9,
    XR_TARGET_SURVEY_STORAGE_LEAF_PRODUCT = 1u << 10,
};

/* Setting XRAY_TARGET_TRACE prints, on stderr, why this pass refused to build a
 * TargetPlan. Unset, the variable is never read and nothing is printed: every
 * call below sits on a path that is already failing the build, so a compile
 * that succeeds pays nothing for it.
 *
 * It exists because the refusals this pass emits are one-line verdicts over
 * compound conditions. "direct-local argument contract needs unsupported
 * storage or ownership" is a single sentence standing for eight storage
 * judgements, five contract equalities and an ownership table, and it names
 * neither the argument that failed nor which of those it failed on. Read alone
 * it cannot distinguish a String passed where the family binds only scalars
 * from a scalar passed with the wrong transfer mode. The trace prints the whole
 * question -- the operation with its source position, every operand under its
 * role, and each sub-judgement with the answer it gave -- so a refusal can be
 * classified without adding temporary printfs. */
static bool target_trace_enabled(void) {
    return getenv("XRAY_TARGET_TRACE") != NULL;
}

static const char *target_trace_role_name(uint8_t role) {
    switch (role) {
        case XR_SEM_OPERAND_VALUE:
            return "value";
        case XR_SEM_OPERAND_CALLEE:
            return "callee";
        case XR_SEM_OPERAND_RECEIVER:
            return "receiver";
        case XR_SEM_OPERAND_ARGUMENT:
            return "argument";
        default:
            return "unnamed";
    }
}

/* The SemanticPlan call-target kinds. Four of them have an adapter family here;
 * the rest are named so a refusal says which authority went unconsumed rather
 * than printing a bare number. */
static const char *target_trace_call_target_kind_name(uint8_t kind) {
    switch (kind) {
        case XR_SEM_CALL_TARGET_DIRECT_LOCAL:
            return "DIRECT_LOCAL";
        case XR_SEM_CALL_TARGET_NATIVE_YIELDABLE:
            return "NATIVE_YIELDABLE";
        case XR_SEM_CALL_TARGET_SOURCE_EXPORT:
            return "SOURCE_EXPORT";
        case XR_SEM_CALL_TARGET_INDIRECT_CALLABLE:
            return "INDIRECT_CALLABLE";
        case XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE:
            return "NATIVE_NAMESPACE_YIELDABLE";
        case XR_SEM_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE:
            return "BUILTIN_INSTANCE_YIELDABLE";
        case XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_LOCAL:
            return "SOURCE_INSTANCE_METHOD_LOCAL";
        case XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_SEALED_CANDIDATE:
            return "SOURCE_INSTANCE_METHOD_SEALED_CANDIDATE";
        case XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_OPEN:
            return "SOURCE_INSTANCE_METHOD_OPEN";
        case XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR:
            return "SOURCE_CLASS_CONSTRUCTOR";
        default:
            return "unnamed";
    }
}

/* One sub-judgement of a compound condition: what it asked, and whether the
 * enclosing condition got the answer it needed. */
static void target_trace_judgement(const char *question, bool held) {
    fprintf(stderr, "[target]     %-52s %s\n", question, held ? "yes" : "NO");
}

/* A semantic type index says nothing on its own, and every storage family in
 * this pass is selected by what the type is. The canonical key carries the
 * whole shape, so it is printed verbatim rather than decoded into a name that
 * would then have to be kept in step with it. */
static void target_trace_type(const XrSemanticPlan *plan, const char *label, uint32_t type_index) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    if (!type) {
        fprintf(stderr, "[target]     %-52s type %u <no record at this index>\n", label,
                type_index);
        return;
    }
    fprintf(stderr, "[target]     %-52s type %u kind=%u builtin=%u scalar_rep=%u children=%u %s\n",
            label, type_index, type->kind, type->builtin_type, type->scalar_rep, type->child_count,
            type->canonical_key ? type->canonical_key : "<no canonical key>");
}

/* One equality a contract demands, printed as both sides so the reader never
 * has to guess which number was the requirement. */
static void target_trace_equality(const char *label, unsigned long long expected,
                                  unsigned long long actual) {
    fprintf(stderr, "[target]     %-52s wanted %llu, got %llu%s\n", label, expected, actual,
            expected == actual ? "" : "   <-- differs");
}

/* The operation whole: where it is in the source, what it produces, and every
 * operand under its role. The operand list is the part a one-line refusal
 * always drops, and it is what decides which family should have claimed the
 * call. */
static void target_trace_operation(const XrSemanticPlan *plan, uint32_t operation_index,
                                   const XrSemanticOperationRecord *operation) {
    if (!operation) {
        fprintf(stderr, "[target]   operation=%u      <no record at this index>\n",
                operation_index);
        return;
    }
    fprintf(stderr, "[target]   operation=%u      %s in function %u, block %u\n", operation_index,
            xi_generated_op_name(operation->opcode), operation->function, operation->block);
    if (operation->source_file)
        fprintf(stderr, "[target]     source                       %s:%u\n", operation->source_file,
                operation->source_line);
    fprintf(stderr,
            "[target]     result                       value %u, semantic type %u, ownership %u, "
            "alias operand %d, provenance %u, parameter %d, complete %u\n",
            operation->result_value, operation->result_type, operation->result_ownership,
            operation->result_alias_operand, operation->return_provenance,
            operation->return_parameter, operation->return_complete);
    fprintf(stderr,
            "[target]     op facts                     intrinsic %u, immediate %lld, effects "
            "0x%08x, flags 0x%08x, ownership use %u, import resolution %u, auxiliary %u\n",
            operation->intrinsic_kind, (long long) operation->semantic_immediate,
            operation->effects, operation->flags, operation->ownership_use,
            operation->import_resolution, operation->auxiliary_kind);
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    for (uint32_t i = 0; i < operation->metadata_count; i++) {
        uint32_t index = operation->metadata_begin + i;
        fprintf(stderr, "[target]     metadata[%u]                  %s\n", i,
                index < metadata_count && metadata[index] ? metadata[index] : "<out of range>");
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    for (uint32_t i = 0; i < operation->operand_count; i++) {
        uint32_t index = operation->operand_begin + i;
        if (index >= operand_count) {
            fprintf(stderr, "[target]     operand[%u]                   <out of range>\n", i);
            continue;
        }
        const XrSemanticOperandRecord *operand = &operands[index];
        fprintf(stderr,
                "[target]     operand[%u]                   role=%s value=%u type=%u parameter=%d "
                "mode=%u transfer=%u ownership_action=%u access=%u origin=%u lifetime=%u escape=%u "
                "flags=0x%02x\n",
                i, target_trace_role_name(operand->role), operand->value, operand->type,
                operand->parameter, operand->parameter_mode, operand->transfer_mode,
                operand->ownership_action, operand->access, operand->origin, operand->lifetime,
                operand->escape, operand->flags);
    }
}

static void *allocate_records(uint32_t count, size_t size) {
    if (!count)
        return NULL;
    if (size && count > SIZE_MAX / size)
        return NULL;
    return xr_calloc(count, size);
}

static bool reserve_records(void **records, uint32_t *capacity, uint32_t required, uint32_t limit,
                            size_t record_size) {
    if (required > limit)
        return false;
    if (required <= *capacity)
        return true;
    uint32_t next = *capacity < 8u ? 8u : *capacity;
    while (next < required) {
        if (next > limit / 2u) {
            next = limit;
            break;
        }
        next *= 2u;
    }
    if (next < required || (record_size && next > SIZE_MAX / record_size))
        return false;
    void *grown = xr_realloc(*records, (size_t) next * record_size);
    if (!grown)
        return false;
    *records = grown;
    *capacity = next;
    return true;
}

static void value_storage_analysis_dispose(XrTargetValueStorageAnalysis *analysis) {
    if (!analysis)
        return;
    xr_free(analysis->defined_values);
    xr_free(analysis->used_types);
    xr_free(analysis->value_types);
    xr_free(analysis->value_functions);
    xr_free(analysis->value_operations);
    xr_free(analysis->type_rep_kinds);
    memset(analysis, 0, sizeof(*analysis));
}

static void materialized_dispose(XrTargetMaterializedPlan *materialized) {
    if (!materialized)
        return;
    xr_free(materialized->machine_reps);
    xr_free(materialized->value_reps);
    xr_free(materialized->extents);
    xr_free(materialized->layouts);
    xr_free(materialized->fields);
    xr_free(materialized->functions);
    xr_free(materialized->slots);
    xr_free(materialized->instructions);
    xr_free(materialized->calls);
    xr_free(materialized->call_arguments);
    xr_free(materialized->root_maps);
    xr_free(materialized->root_slots);
    xr_free(materialized->cleanups);
    xr_free(materialized->adapters);
    xr_free(materialized->capabilities);
    xr_free(materialized->coroutines);
    xr_free(materialized->entry_expectations);
    xr_free(materialized->debug_facts);
    memset(materialized, 0, sizeof(*materialized));
}

/* The mapping itself is shared with the verifier; only the raw-pointer test
 * stays local, because the two sides derive that one by independent routes and
 * a disagreement between them is exactly what it is there to catch. */
static XrTargetScalarEligibility classify_scalar_type(const XrSemanticTypeRecord *type,
                                                      uint16_t *out_kind) {
    switch (xr_target_scalar_rep_for_type(type, type && xr_semantic_raw_pointer_type_is_exact(type),
                                          out_kind)) {
        case XR_TARGET_SCALAR_REP_EXACT:
            return XR_TARGET_SCALAR_VALUE;
        case XR_TARGET_SCALAR_REP_NOT_APPLICABLE:
            return XR_TARGET_SCALAR_NOT_APPLICABLE;
        default:
            return XR_TARGET_SCALAR_INVALID;
    }
}

/* `T?` is `T | null`, and the language surface requires the nullable primitives
 * to carry `null` in the tagged representation so a null renders as "null" and
 * not as the payload's zero, with the interpreter and the native backend
 * agreeing. The payload admitted here is exactly one machine scalar that cannot
 * hold a reference, so the tagged carrier owes no reference count. A nullable
 * String, object, or container is reference capable and stays refused: its
 * storage would carry a release obligation this family states no row for. */
static bool semantic_nullable_scalar_type_is_exact(const XrSemanticTypeRecord *type) {
    const uint8_t allowed = (uint8_t) (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_CONST);
    XrStableId zero = {{0}};
    if (!type || (type->flags & (uint8_t) ~allowed) != 0 || type->builtin_type != XR_TID_NULL ||
        type->child_count != 0 || type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(type->source_class_identity, zero) || type->source_enum_key ||
        type->enum_layout_id != 0 || type->enum_member_count != 0 || type->enum_flags != 0 ||
        type->reserved_enum != 0)
        return false;
    /* The type of the `null` spelling itself is the degenerate member of this
     * shape: its carrier holds the null tag and nothing else, which is the same
     * storage the payload-carrying members use and owes no reference count
     * either. It is what a nullable scalar is initialized from and compared
     * against, so leaving it out would refuse every program that names null. */
    if (type->kind == XR_KIND_NULL)
        return type->scalar_rep == XR_SCALAR_REP_NONE;
    if ((type->flags & XR_SEM_TYPE_NULLABLE) == 0)
        return false;
    /* The authority stops at the three payload spellings whose whole path is
     * proved, `int`, `float` and `bool`. A narrower or unsigned integer, a
     * single-precision float, and a rune each imply a boxing recipe this
     * authority does not state, so they stay refused along with every other
     * payload. */
    switch ((XrTypeKind) type->kind) {
        case XR_KIND_INT:
            return type->scalar_rep == XR_NATIVE_I64;
        case XR_KIND_FLOAT:
            return type->scalar_rep == XR_NATIVE_F64;
        case XR_KIND_BOOL:
            return type->scalar_rep == XR_SCALAR_REP_NONE;
        default:
            return false;
    }
}

static bool rep_layout_for_kind(const XrTargetMachineFacts *profile, uint16_t kind,
                                XrTargetTypeLayout *out, uint16_t *register_bits,
                                uint8_t *signedness) {
    if (!profile || !out || !register_bits || !signedness)
        return false;
    *signedness = XR_TARGET_SIGN_NONE;
    switch (kind) {
        case XR_MACHINE_REP_I1:
            *out = profile->data_layout.boolean;
            *register_bits = 1;
            return true;
        case XR_MACHINE_REP_I8:
            *out = profile->data_layout.i8;
            *signedness = XR_TARGET_SIGN_SIGNED;
            break;
        case XR_MACHINE_REP_U8:
            *out = profile->data_layout.u8;
            *signedness = XR_TARGET_SIGN_UNSIGNED;
            break;
        case XR_MACHINE_REP_I16:
            *out = profile->data_layout.i16;
            *signedness = XR_TARGET_SIGN_SIGNED;
            break;
        case XR_MACHINE_REP_U16:
            *out = profile->data_layout.u16;
            *signedness = XR_TARGET_SIGN_UNSIGNED;
            break;
        case XR_MACHINE_REP_I32:
            *out = profile->data_layout.i32;
            *signedness = XR_TARGET_SIGN_SIGNED;
            break;
        case XR_MACHINE_REP_U32:
        case XR_MACHINE_REP_RUNE:
            *out = profile->data_layout.u32;
            *signedness = XR_TARGET_SIGN_UNSIGNED;
            break;
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_ENUM_ORDINAL:
            *out = profile->data_layout.i64;
            *signedness = XR_TARGET_SIGN_SIGNED;
            break;
        case XR_MACHINE_REP_U64:
            *out = profile->data_layout.u64;
            *signedness = XR_TARGET_SIGN_UNSIGNED;
            break;
        case XR_MACHINE_REP_ISIZE:
            *out = profile->data_layout.isize;
            *signedness = XR_TARGET_SIGN_SIGNED;
            break;
        case XR_MACHINE_REP_USIZE:
            *out = profile->data_layout.usize;
            *signedness = XR_TARGET_SIGN_UNSIGNED;
            break;
        case XR_MACHINE_REP_F32:
            *out = profile->data_layout.f32;
            break;
        case XR_MACHINE_REP_F64:
            *out = profile->data_layout.f64;
            break;
        case XR_MACHINE_REP_RAW_PTR:
            *out = profile->data_layout.pointer;
            break;
        default:
            return false;
    }
    if (out->size > UINT16_MAX / 8u)
        return false;
    *register_bits = (uint16_t) (out->size * 8u);
    return true;
}

static bool make_machine_rep(const XrTargetMachineFacts *profile, uint16_t kind,
                             XrTargetMachineRepRecord *out) {
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    if (kind == XR_MACHINE_REP_VOID)
        return true;
    XrTargetTypeLayout layout = {0};
    uint16_t register_bits = 0;
    uint8_t signedness = XR_TARGET_SIGN_NONE;
    if (!rep_layout_for_kind(profile, kind, &layout, &register_bits, &signedness) || !layout.size ||
        layout.size > UINT32_MAX || !layout.align || layout.align > UINT16_MAX)
        return false;
    out->register_bits = register_bits;
    out->memory_size = (uint32_t) layout.size;
    out->memory_align = (uint16_t) layout.align;
    out->signedness = signedness;
    out->ownership = XR_TARGET_OWNERSHIP_TRIVIAL;
    if (kind == XR_MACHINE_REP_RAW_PTR)
        out->null_encoding = XR_TARGET_NULL_ZERO;
    return true;
}

/* A scalar representation is not fully identified by its physical geometry.
 * Unit enums share i64 storage, but their nominal semantic type is part of the
 * representation identity and is required by the independent verifier. Keep
 * that typed decision here so every storage family publishes the same row. */
static bool make_scalar_type_rep(const XrTargetPlanBuilder *builder, uint32_t semantic_type,
                                 uint16_t kind, XrTargetMachineRepRecord *out) {
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(builder ? builder->semantic_plan : NULL, semantic_type);
    uint16_t exact_kind = XR_MACHINE_REP_COUNT;
    if (!builder || !out || classify_scalar_type(type, &exact_kind) != XR_TARGET_SCALAR_VALUE ||
        exact_kind != kind ||
        !make_machine_rep(xr_target_profile_machine_facts(builder->profile), kind, out))
        return false;
    if (kind == XR_MACHINE_REP_ENUM_ORDINAL) {
        if (!xr_semantic_unit_enum_type_is_exact(type))
            return false;
        out->detail = semantic_type;
    }
    return true;
}

static bool make_dynamic_value_rep(const XrTargetMachineFacts *profile,
                                   XrTargetMachineRepRecord *out) {
    if (!profile || !out || !profile->data_layout.xr_value.size ||
        profile->data_layout.xr_value.size > UINT16_MAX / 8u ||
        !profile->data_layout.xr_value.align || profile->data_layout.xr_value.align > UINT16_MAX)
        return false;
    /* This freezes only the outer XrValue slot representation. OWNED and
     * DYNAMIC are storage facts; Semantic ownership and the existing AOT
     * closure lifetime path still own allocation, roots, and cleanup. */
    *out = (XrTargetMachineRepRecord) {
        .kind = XR_MACHINE_REP_DYN_VALUE,
        .register_bits = (uint16_t) (profile->data_layout.xr_value.size * 8u),
        .memory_size = (uint32_t) profile->data_layout.xr_value.size,
        .memory_align = (uint16_t) profile->data_layout.xr_value.align,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_OWNED,
        .null_encoding = XR_TARGET_NULL_TAGGED,
    };
    return true;
}

static bool make_static_callable_value_rep(const XrTargetMachineFacts *profile,
                                           XrTargetMachineRepRecord *out) {
    if (!make_dynamic_value_rep(profile, out))
        return false;
    /* A frozen direct-local shared function is a borrowed static callable
     * token. This row owns only its outer XrValue storage; it does not claim
     * a closure body, allocation, root map, or cleanup. */
    out->ownership = XR_TARGET_OWNERSHIP_BORROWED;
    return true;
}

static bool make_borrowed_dynamic_value_rep(const XrTargetMachineFacts *profile,
                                            XrTargetMachineRepRecord *out) {
    if (!make_dynamic_value_rep(profile, out))
        return false;
    out->ownership = XR_TARGET_OWNERSHIP_BORROWED;
    return true;
}

static bool make_string_byte_slice_view_rep(const XrTargetMachineFacts *profile,
                                            XrTargetMachineRepRecord *out) {
    if (!profile || !out || profile->data_layout.pointer.size != 8u ||
        profile->data_layout.pointer.align != 8u || profile->data_layout.i64.size != 8u ||
        profile->data_layout.i64.align != 8u)
        return false;
    *out = (XrTargetMachineRepRecord) {
        .kind = XR_MACHINE_REP_VIEW,
        .register_bits = 128,
        .memory_size = 16,
        .memory_align = 8,
        .root_kind = XR_TARGET_ROOT_VIEW_OWNER,
        .ownership = XR_TARGET_OWNERSHIP_BORROWED,
        .null_encoding = XR_TARGET_NULL_NOT_NULLABLE,
        .detail = XR_SEMANTIC_INDEX_NONE,
    };
    return true;
}

static int compare_rep_record(const XrTargetMachineRepRecord *left,
                              const XrTargetMachineRepRecord *right) {
#define XR_COMPARE_REP_FIELD(field)                                                                \
    do {                                                                                           \
        if (left->field < right->field)                                                            \
            return -1;                                                                             \
        if (left->field > right->field)                                                            \
            return 1;                                                                              \
    } while (0)
    XR_COMPARE_REP_FIELD(kind);
    XR_COMPARE_REP_FIELD(register_bits);
    XR_COMPARE_REP_FIELD(memory_size);
    XR_COMPARE_REP_FIELD(memory_align);
    XR_COMPARE_REP_FIELD(signedness);
    XR_COMPARE_REP_FIELD(root_kind);
    XR_COMPARE_REP_FIELD(ownership);
    XR_COMPARE_REP_FIELD(null_encoding);
    XR_COMPARE_REP_FIELD(detail);
    XR_COMPARE_REP_FIELD(lane_count);
    for (uint32_t i = 0; i < 4; i++) {
        if (left->legal_conversion_mask[i] < right->legal_conversion_mask[i])
            return -1;
        if (left->legal_conversion_mask[i] > right->legal_conversion_mask[i])
            return 1;
    }
#undef XR_COMPARE_REP_FIELD
    return 0;
}

static int compare_rep_intent(const void *left, const void *right) {
    return compare_rep_record(&((const XrTargetRepIntent *) left)->record,
                              &((const XrTargetRepIntent *) right)->record);
}

static int compare_layout_intent(const void *left, const void *right) {
    const XrTargetLayoutIntent *a = (const XrTargetLayoutIntent *) left;
    const XrTargetLayoutIntent *b = (const XrTargetLayoutIntent *) right;
    if (a->semantic_type < b->semantic_type)
        return -1;
    if (a->semantic_type > b->semantic_type)
        return 1;
    return 0;
}

static int compare_value_intent(const void *left, const void *right) {
    const XrTargetValueIntent *a = (const XrTargetValueIntent *) left;
    const XrTargetValueIntent *b = (const XrTargetValueIntent *) right;
    if (a->semantic_value < b->semantic_value)
        return -1;
    if (a->semantic_value > b->semantic_value)
        return 1;
    return 0;
}

static int compare_slot_intent(const void *left, const void *right) {
    const XrTargetSlotIntent *a = (const XrTargetSlotIntent *) left;
    const XrTargetSlotIntent *b = (const XrTargetSlotIntent *) right;
    if (a->function < b->function)
        return -1;
    if (a->function > b->function)
        return 1;
    return xr_stable_id_compare(a->identity, b->identity);
}

static bool append_rep_intent(XrTargetPlanBuilder *builder, const XrTargetMachineRepRecord *record,
                              char *error, size_t error_size) {
    for (uint32_t i = 0; i < builder->rep_intent_count; i++)
        if (compare_rep_record(&builder->rep_intents[i].record, record) == 0)
            return true;
    if (!reserve_records((void **) &builder->rep_intents, &builder->rep_intent_capacity,
                         builder->rep_intent_count + 1u, 256u, sizeof(*builder->rep_intents)))
        return fail(error, error_size, "XR_EXEC_5003",
                    "machine representation intent budget exhausted");
    builder->rep_intents[builder->rep_intent_count++].record = *record;
    return true;
}

static bool semantic_direct_local_array_type_is_exact(const XrSemanticPlan *plan,
                                                      uint32_t type_index,
                                                      bool indexes_elements, uint8_t *storage);

static bool append_layout_intent(XrTargetPlanBuilder *builder, uint32_t semantic_type, uint8_t kind,
                                 uint32_t element_count, const XrTargetMachineRepRecord *memory_rep,
                                 char *error, size_t error_size) {
    /* Element storage is a property of the exact semantic Array type, not of
     * whichever producer family first needs its layout. Generic shared-value
     * and owner-forward families therefore project the same answer as Array
     * allocation/call families, while non-Array layouts retain NONE. */
    uint8_t array_element_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    (void) semantic_direct_local_array_type_is_exact(
        builder ? builder->semantic_plan : NULL, semantic_type, false, &array_element_storage);
    for (uint32_t i = 0; i < builder->layout_intent_count; i++) {
        XrTargetLayoutIntent *existing = &builder->layout_intents[i];
        if (existing->semantic_type != semantic_type)
            continue;
        bool same_dynamic_geometry = kind == XR_TARGET_LAYOUT_DYNAMIC &&
                                     existing->kind == XR_TARGET_LAYOUT_DYNAMIC &&
                                     existing->memory_rep.kind == XR_MACHINE_REP_DYN_VALUE &&
                                     memory_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
                                     existing->memory_rep.memory_size == memory_rep->memory_size &&
                                     existing->memory_rep.memory_align == memory_rep->memory_align;
        if (existing->kind == kind && existing->element_count == element_count &&
            existing->array_element_storage == array_element_storage &&
            (compare_rep_record(&existing->memory_rep, memory_rep) == 0 || same_dynamic_geometry))
            return true;
        return fail(error, error_size, "XR_TARGET_1002",
                    "semantic type has conflicting layout intents");
    }
    if (!reserve_records((void **) &builder->layout_intents, &builder->layout_intent_capacity,
                         builder->layout_intent_count + 1u, 1000000u,
                         sizeof(*builder->layout_intents)))
        return fail(error, error_size, "XR_EXEC_5003", "layout intent budget exhausted");
    XrTargetLayoutIntent *intent = &builder->layout_intents[builder->layout_intent_count++];
    intent->semantic_type = semantic_type;
    intent->memory_rep = *memory_rep;
    intent->element_count = element_count;
    intent->kind = kind;
    intent->array_element_storage = array_element_storage;
    return true;
}

static bool append_value_intent(XrTargetPlanBuilder *builder, const XrTargetValueIntent *intent,
                                char *error, size_t error_size) {
    if (!reserve_records((void **) &builder->value_intents, &builder->value_intent_capacity,
                         builder->value_intent_count + 1u, 40000000u,
                         sizeof(*builder->value_intents)))
        return fail(error, error_size, "XR_EXEC_5003",
                    "value representation intent budget exhausted");
    builder->value_intents[builder->value_intent_count] = *intent;
    builder->value_intents[builder->value_intent_count++].family = builder->active_family;
    return true;
}

static bool append_slot_intent(XrTargetPlanBuilder *builder, const XrTargetSlotIntent *intent,
                               char *error, size_t error_size) {
    if (!reserve_records((void **) &builder->slot_intents, &builder->slot_intent_capacity,
                         builder->slot_intent_count + 1u, 16000000u,
                         sizeof(*builder->slot_intents)))
        return fail(error, error_size, "XR_EXEC_5003", "slot intent budget exhausted");
    builder->slot_intents[builder->slot_intent_count++] = *intent;
    return true;
}

static bool append_call_intent(XrTargetPlanBuilder *builder, const XrTargetCallIntent *intent,
                               char *error, size_t error_size) {
    if (!reserve_records((void **) &builder->call_intents, &builder->call_intent_capacity,
                         builder->call_intent_count + 1u, 10000000u,
                         sizeof(*builder->call_intents)))
        return fail(error, error_size, "XR_EXEC_5003", "call intent budget exhausted");
    builder->call_intents[builder->call_intent_count++] = *intent;
    return true;
}

static bool append_call_argument_intent(XrTargetPlanBuilder *builder,
                                        const XrTargetCallArgumentIntent *intent, char *error,
                                        size_t error_size) {
    if (!reserve_records((void **) &builder->call_argument_intents,
                         &builder->call_argument_intent_capacity,
                         builder->call_argument_intent_count + 1u, 40000000u,
                         sizeof(*builder->call_argument_intents)))
        return fail(error, error_size, "XR_EXEC_5003", "call argument intent budget exhausted");
    builder->call_argument_intents[builder->call_argument_intent_count++] = *intent;
    return true;
}

static bool stable_identity_from_pair(const char *domain, XrStableId first, XrStableId second,
                                      uint32_t ordinal, XrStableId *out) {
    char first_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char second_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    XrFingerprint digest;
    xr_stable_id_hex(first, first_hex);
    xr_stable_id_hex(second, second_hex);
    int written = snprintf(key, sizeof(key), "%s:first=%s:second=%s:ordinal=%u", domain, first_hex,
                           second_hex, ordinal);
    return out && written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static bool make_slot_identity(const XrSemanticPlan *plan, uint32_t function, uint8_t role,
                               XrStableId source, uint32_t logical_slot, XrStableId *out) {
    const XrSemanticFunctionRecord *semantic_function = xr_semantic_plan_function(plan, function);
    if (!semantic_function || !out)
        return false;
    char function_id[XR_STABLE_ID_BYTES * 2 + 1];
    char source_id[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    xr_stable_id_hex(semantic_function->id, function_id);
    xr_stable_id_hex(source, source_id);
    int written =
        snprintf(key, sizeof(key), "xray-target-slot-v2:function=%s:role=%u:source=%s:logical=%u",
                 function_id, (unsigned) role, source_id, logical_slot);
    XrFingerprint digest;
    return written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static bool value_storage_analysis_init(const XrSemanticPlan *plan,
                                        XrTargetValueStorageAnalysis *analysis, char *error,
                                        size_t error_size) {
    size_t type_count = xr_semantic_plan_type_count(plan);
    size_t function_count = xr_semantic_plan_function_count(plan);
    if (type_count > 1000000u || function_count > 100000u)
        return fail(error, error_size, "XR_EXEC_5003",
                    "target value-storage input exceeds hard budgets");
    uint32_t total_values = 0;
    if (function_count) {
        const XrSemanticFunctionRecord *last =
            xr_semantic_plan_function(plan, (uint32_t) function_count - 1u);
        if (!last || last->value_begin > UINT32_MAX - last->value_count)
            return fail(error, error_size, "XR_EXEC_5003",
                        "semantic value identity budget overflow");
        total_values = last->value_begin + last->value_count;
    }
    if (total_values > 40000000u)
        return fail(error, error_size, "XR_EXEC_5003", "target value-storage budget exhausted");
    analysis->total_values = total_values;
    analysis->type_count = (uint32_t) type_count;
    analysis->defined_values = (uint8_t *) allocate_records(total_values, sizeof(uint8_t));
    analysis->used_types = (uint8_t *) allocate_records((uint32_t) type_count, sizeof(uint8_t));
    analysis->value_types = (uint32_t *) allocate_records(total_values, sizeof(uint32_t));
    analysis->value_functions = (uint32_t *) allocate_records(total_values, sizeof(uint32_t));
    analysis->value_operations = (uint32_t *) allocate_records(total_values, sizeof(uint32_t));
    analysis->type_rep_kinds =
        (uint16_t *) allocate_records((uint32_t) type_count, sizeof(uint16_t));
    if ((total_values && (!analysis->defined_values || !analysis->value_types ||
                          !analysis->value_functions || !analysis->value_operations)) ||
        (type_count && (!analysis->used_types || !analysis->type_rep_kinds))) {
        value_storage_analysis_dispose(analysis);
        return fail(error, error_size, "XR_EXEC_5003",
                    "target value-storage analysis allocation failed");
    }
    for (uint32_t i = 0; i < total_values; i++)
        analysis->value_operations[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < (uint32_t) type_count; i++)
        analysis->type_rep_kinds[i] = XR_MACHINE_REP_COUNT;
    return true;
}

static bool index_value_operations(const XrSemanticPlan *plan,
                                   XrTargetValueStorageAnalysis *analysis, char *error,
                                   size_t error_size) {
    size_t operation_count = xr_semantic_plan_operation_count(plan);
    if (operation_count > UINT32_MAX)
        return fail(error, error_size, "XR_EXEC_5003", "semantic operation budget exhausted");
    for (uint32_t i = 0; i < (uint32_t) operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(plan, i);
        if (!operation)
            return fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
        if (operation->result_value == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (operation->result_value >= analysis->total_values ||
            analysis->value_operations[operation->result_value] != XR_SEMANTIC_INDEX_NONE)
            return fail(error, error_size, "XR_TARGET_1001",
                        "semantic value has ambiguous defining operations");
        analysis->value_operations[operation->result_value] = i;
    }
    return true;
}

static bool semantic_heap_closure_is_exact(const XrSemanticPlan *plan,
                                           const XrSemanticOperationRecord *operation) {
    if (!plan || !operation || operation->opcode != XI_CLOSURE_NEW ||
        operation->callable_function >= xr_semantic_plan_function_count(plan) ||
        operation->operand_count != 0 || !xr_semantic_allocation_identity_is_canonical(operation) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
        return false;
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(plan, operation->callable_function);
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, operation->result_type);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    bool typed_function = type && type->kind == XR_KIND_FUNCTION;
    bool opaque_closure = type && type->kind == XR_KIND_UNKNOWN && type->child_count == 0;
    if (!callee || !type || callee->parent != operation->function || callee->capture_count != 0 ||
        (!typed_function && !opaque_closure) || type->aggregate_extent != 0 ||
        type->aggregate_align != 0 ||
        (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE | XR_SEM_TYPE_BORROW_VIEW |
                        XR_SEM_TYPE_AGGREGATE_EXACT)) != 0 ||
        (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT)) !=
            (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        callee->parameter_count == UINT16_MAX ||
        (typed_function && type->child_count != (uint32_t) callee->parameter_count + 1u) ||
        type->child_begin > child_count || type->child_count > child_count - type->child_begin ||
        callee->parameter_begin > xr_semantic_plan_parameter_count(plan) ||
        callee->parameter_count > xr_semantic_plan_parameter_count(plan) - callee->parameter_begin)
        return false;
    for (uint32_t i = 0; i < callee->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan, callee->parameter_begin + i);
        if (!parameter || parameter->function != operation->callable_function ||
            parameter->ordinal != i ||
            (typed_function && children[type->child_begin + i] != parameter->type))
            return false;
    }
    return opaque_closure ||
           children[type->child_begin + callee->parameter_count] == callee->return_type;
}

/* A direct-local call returns an owned String only when the callee, the call
 * operation, and the result type all agree. The result must be a fresh owner:
 * an aliased or parameter-forwarded return would need a borrow whose extent
 * this plan cannot yet state, so it stays fail closed. */
static bool semantic_direct_local_string_result_is_exact(const XrSemanticPlan *plan,
                                                         const XrSemanticOperationRecord *operation,
                                                         const XrSemanticFunctionRecord *callee) {
    return plan && operation && callee &&
           (operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL) &&
           operation->result_type == callee->return_type &&
           operation->result_value != XR_SEMANTIC_INDEX_NONE &&
           operation->result_alias_operand == -1 && operation->return_parameter == -1 &&
           operation->return_complete == 1 && operation->return_provenance == XR_SEM_RETURN_OWNED &&
           callee->return_parameter == -1 && callee->return_provenance == XR_SEM_RETURN_OWNED &&
           xr_semantic_tagged_string_type_is_exact(
               xr_semantic_plan_type(plan, operation->result_type));
}

/* Resolve the unique DIRECT_LOCAL call target that owns this operation. A
 * duplicated or foreign target row leaves the operation unclaimed. */
static const XrSemanticFunctionRecord *
semantic_direct_local_callee_for_operation(const XrSemanticPlan *plan, uint32_t operation_index) {
    size_t target_count = xr_semantic_plan_call_target_count(plan);
    const XrSemanticFunctionRecord *callee = NULL;
    for (size_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(plan, i);
        if (!target || target->operation != operation_index ||
            target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL)
            continue;
        if (callee)
            return NULL;
        callee = xr_semantic_plan_function(plan, target->function);
        if (!callee)
            return NULL;
    }
    return callee;
}

static bool semantic_stringbuilder_type_is_exact(const XrSemanticTypeRecord *type) {
    char expected_type_key[160];
    int written = snprintf(expected_type_key, sizeof(expected_type_key),
                           "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:13:StringBuilder[0]",
                           (unsigned) XR_KIND_INSTANCE, (unsigned) XR_TID_STRINGBUILDER,
                           (unsigned) XR_SCALAR_REP_NONE);
    return type && written > 0 && (size_t) written < sizeof(expected_type_key) &&
           type->kind == XR_KIND_INSTANCE && type->builtin_type == XR_TID_STRINGBUILDER &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->canonical_key && strcmp(type->canonical_key, expected_type_key) == 0;
}

static bool
semantic_stringbuilder_constructor_is_exact(const XrSemanticPlan *plan,
                                            const XrSemanticOperationRecord *operation) {
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || operation->opcode != XI_CALL_BUILTIN ||
        operation->operand_count != 0 || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count || !metadata ||
        strcmp(metadata[operation->metadata_begin], "StringBuilder") != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE || operation->semantic_immediate != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_BUILTIN) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_BUILTIN) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_BUILTIN) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1 || !xr_semantic_allocation_identity_is_canonical(operation))
        return false;
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, operation->result_type);
    return semantic_stringbuilder_type_is_exact(type);
}

static bool
semantic_stringbuilder_append_result_is_exact(const XrSemanticPlan *plan,
                                              const XrSemanticOperationRecord *operation) {
    const XrSemanticFunctionRecord *function =
        operation ? xr_semantic_plan_function(plan, operation->function) : NULL;
    return operation &&
           ((operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
             operation->return_parameter == -1 &&
             ((operation->return_provenance == XR_SEM_RETURN_OWNED &&
               operation->return_complete == 1) ||
              (operation->return_provenance == XR_SEM_RETURN_NONE &&
               operation->return_complete == 0))) ||
            (operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
             ((((operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
                 operation->return_parameter == -1) ||
                (operation->return_provenance == XR_SEM_RETURN_BORROWED_PARAM && function &&
                 operation->return_parameter >= 0 &&
                 (uint16_t) operation->return_parameter < function->parameter_count)) &&
               operation->return_complete == 1) ||
              (operation->return_provenance == XR_SEM_RETURN_NONE &&
               operation->return_parameter == -1 && operation->return_complete == 0))));
}

static bool semantic_stringbuilder_append_rune_is_exact(const XrSemanticPlan *plan,
                                                        const XrSemanticOperationRecord *operation,
                                                        uint32_t *receiver_value,
                                                        uint32_t *argument_value) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_RUNE ||
        operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & 1) != 0 || operation->operand_count != 2 ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "append") != 0 ||
        operation->result_alias_operand != 0 ||
        !semantic_stringbuilder_append_result_is_exact(plan, operation))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *argument = receiver + 1;
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(plan, receiver->type);
    const XrSemanticTypeRecord *argument_type = xr_semantic_plan_type(plan, argument->type);
    if (!semantic_stringbuilder_type_is_exact(receiver_type) ||
        operation->result_type != receiver->type || !argument_type ||
        argument_type->kind != XR_KIND_RUNE || argument_type->builtin_type != XR_TID_NULL ||
        argument_type->child_count != 0 || argument_type->scalar_rep != XR_SCALAR_REP_NONE ||
        argument_type->flags != 0 || receiver->role != XR_SEM_OPERAND_RECEIVER ||
        receiver->parameter != -1 || receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != 0 ||
        argument->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return false;
    if (receiver_value)
        *receiver_value = receiver->value;
    if (argument_value)
        *argument_value = argument->value;
    return true;
}

static bool semantic_stringbuilder_to_string_is_exact(const XrSemanticPlan *plan,
                                                      const XrSemanticOperationRecord *operation,
                                                      uint32_t *receiver_value) {
    uint32_t operand_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_STRINGBUILDER_TO_STRING ||
        operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & 1) != 0 || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "toString") != 0 ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(plan, receiver->type);
    const XrSemanticTypeRecord *result_type = xr_semantic_plan_type(plan, operation->result_type);
    if (!semantic_stringbuilder_type_is_exact(receiver_type) || !result_type ||
        result_type->kind != XR_KIND_STRING || result_type->builtin_type != XR_TID_NULL ||
        result_type->child_count != 0 || result_type->scalar_rep != XR_SCALAR_REP_NONE ||
        result_type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return false;
    if (receiver_value)
        *receiver_value = receiver->value;
    return true;
}

static bool
semantic_stringbuilder_append_string_is_exact(const XrSemanticPlan *plan,
                                              const XrSemanticOperationRecord *operation,
                                              uint32_t *receiver_value, uint32_t *argument_value) {
    uint32_t operands_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operands_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_STRING ||
        operation->operand_count != 2 || operation->operand_begin + 1u >= operands_count ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "append") != 0 ||
        operation->result_alias_operand != 0 ||
        !semantic_stringbuilder_append_result_is_exact(plan, operation))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *argument = receiver + 1;
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(plan, receiver->type);
    const XrSemanticTypeRecord *argument_type = xr_semantic_plan_type(plan, argument->type);
    if (!semantic_stringbuilder_type_is_exact(receiver_type) || !argument_type ||
        argument_type->kind != XR_KIND_STRING || operation->result_type != receiver->type ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != 0 ||
        argument->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return false;
    if (receiver_value)
        *receiver_value = receiver->value;
    if (argument_value)
        *argument_value = argument->value;
    return true;
}

static bool semantic_array_reserve_is_exact(const XrSemanticPlan *plan,
                                            const XrSemanticOperationRecord *operation,
                                            uint32_t *receiver_type_index,
                                            uint32_t *capacity_value) {
    uint32_t operands_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operands_count);
    if (!plan || !operation || !operands ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR ||
        operation->evidence[1] != XA_INTRINSIC_ARRAY_RESERVE ||
        operation->opcode != XI_CALL_BUILTIN || operation->operand_count != 2 ||
        operation->operand_begin > operands_count ||
        operation->operand_count > operands_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->semantic_immediate != 0 ||
        operation->effects != xi_generated_op_effects(XI_CALL_BUILTIN))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *capacity = receiver + 1;
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(plan, receiver->type);
    const XrSemanticTypeRecord *capacity_type = xr_semantic_plan_type(plan, capacity->type);
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(plan, operation->function);
    if (!function || !xr_semantic_array_type_row_is_exact(receiver_type) ||
        !xr_semantic_array_member_i64_type_is_exact(capacity_type) ||
        operation->result_type != receiver->type || operation->result_alias_operand != 0 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1 || receiver->role != XR_SEM_OPERAND_ARGUMENT ||
        receiver->parameter != 0 || receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        capacity->role != XR_SEM_OPERAND_ARGUMENT || capacity->parameter != 1 ||
        capacity->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        capacity->ownership_action != XR_SEM_OPERAND_CONSUME ||
        receiver->value < function->value_begin ||
        receiver->value >= function->value_begin + function->value_count ||
        capacity->value < function->value_begin ||
        capacity->value >= function->value_begin + function->value_count ||
        operation->result_value < function->value_begin ||
        operation->result_value >= function->value_begin + function->value_count)
        return false;
    if (receiver_type_index)
        *receiver_type_index = receiver->type;
    if (capacity_value)
        *capacity_value = capacity->value;
    return true;
}

/* `Array<T>` is a compiler-owned container: no declaration produces the array
 * kind and the language admits no member declaration on it, so a frozen
 * selector below on that receiver names one implementation.  Each row states
 * the whole shape one selector may present: the operand count range, which
 * operand carries the element, and what the result is.  The element clause is
 * proven against the receiver's own element entry, every other argument is an
 * exact signed 64-bit bound. Reference-capable elements are admitted only
 * when the frozen shape states a complete ownership and drop lifecycle. */

static bool builder_array_member_reference_contract_is_exact(
    const XrSemanticPlan *plan, const XrArrayMemberShape *shape,
    const XrSemanticOperationRecord *operation, uint32_t element_type_index,
    const XrSemanticTypeRecord *element_type) {
    if (!plan || !shape || !operation || !element_type)
        return false;
    if ((element_type->flags & XR_SEM_TYPE_REFERENCE_CAPABLE) == 0)
        return true;
    if (shape->reference_action == XR_ARRAY_MEMBER_REFERENCE_PRESERVE)
        return shape->element_operand == 0 &&
               (shape->element_access == XR_ARRAY_MEMBER_ELEMENT_ACCESS_READ ||
                shape->element_access == XR_ARRAY_MEMBER_ELEMENT_ACCESS_MOVE) &&
               shape->reference_drop == XR_ARRAY_MEMBER_REFERENCE_DROP_NONE;
    if (shape->reference_action != XR_ARRAY_MEMBER_REFERENCE_CONSUME_INTO_STORAGE ||
        shape->element_access != XR_ARRAY_MEMBER_ELEMENT_ACCESS_STORE ||
        shape->reference_drop != XR_ARRAY_MEMBER_REFERENCE_DROP_RELEASE_ON_ERASE_OR_DESTROY ||
        shape->element_operand == 0 || shape->element_operand >= operation->operand_count ||
        xr_semantic_class_instance_type_source_class(plan, element_type) == XR_SEMANTIC_INDEX_NONE)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    uint32_t semantic_operand = operation->operand_begin + shape->element_operand;
    if (!operands || semantic_operand >= operand_count)
        return false;
    const XrSemanticOperandRecord *element = &operands[semantic_operand];
    return element->type == element_type_index && element->role == XR_SEM_OPERAND_ARGUMENT &&
           element->parameter == (int16_t) (shape->element_operand - 1u) &&
           element->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
           element->ownership_action == XR_SEM_OPERAND_CONSUME;
}

static bool semantic_array_member_scalar_is_exact(const XrSemanticPlan *plan,
                                                  const XrSemanticOperationRecord *operation,
                                                  uint32_t *element_value, bool *receiver_result) {
    uint32_t reserve_receiver_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t reserve_capacity = XR_SEMANTIC_INDEX_NONE;
    if (semantic_array_reserve_is_exact(plan, operation, &reserve_receiver_type,
                                        &reserve_capacity)) {
        if (element_value)
            *element_value = reserve_capacity;
        if (receiver_result)
            *receiver_result = true;
        return true;
    }
    uint32_t operands_count = 0, metadata_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operands_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    if (!plan || !operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR ||
        operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & 1) != 0 || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count || !metadata || !children || !operands ||
        operation->operand_begin >= operands_count ||
        operation->operand_count > operands_count - operation->operand_begin ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD))
        return false;
    const XrArrayMemberShape *shape =
        xr_array_member_shape(metadata[operation->metadata_begin], operation->operand_count);
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(plan, receiver->type);
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(plan, operation->function);
    if (!shape || !function || !xr_semantic_array_type_row_is_exact(receiver_type) ||
        receiver_type->child_begin >= child_count || receiver->role != XR_SEM_OPERAND_RECEIVER ||
        receiver->parameter != -1 || receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->value < function->value_begin ||
        receiver->value >= function->value_begin + function->value_count ||
        operation->result_value < function->value_begin ||
        operation->result_value >= function->value_begin + function->value_count)
        return false;
    uint32_t element_type_index = children[receiver_type->child_begin];
    const XrSemanticTypeRecord *element_type = xr_semantic_plan_type(plan, element_type_index);
    bool source_class_fill_result =
        element_type && strcmp(shape->selector, "fill") == 0 && operation->operand_count == 4 &&
        operation->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_FILL << 1 &&
        xr_semantic_class_instance_type_source_class(plan, element_type) !=
            XR_SEMANTIC_INDEX_NONE &&
        operation->result_type == receiver->type && operation->result_alias_operand == 0 &&
        ((operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
          operation->return_provenance == XR_SEM_RETURN_BORROWED_PARAM &&
          operation->return_parameter == 0 && operation->return_complete == 1) ||
         (operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT &&
          operation->return_provenance == XR_SEM_RETURN_NONE && operation->return_parameter == -1 &&
          operation->return_complete == 0));
    if (!element_type ||
        (!xr_semantic_array_member_result_is_exact(
             operation, shape, xr_semantic_plan_type(plan, operation->result_type),
             receiver->type) &&
         !source_class_fill_result) ||
        !builder_array_member_reference_contract_is_exact(plan, shape, operation,
                                                          element_type_index, element_type))
        return false;
    uint32_t element = XR_SEMANTIC_INDEX_NONE;
    for (uint16_t i = 1; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = receiver + i;
        const XrSemanticTypeRecord *argument_type = xr_semantic_plan_type(plan, argument->type);
        bool is_element = i == shape->element_operand;
        if (!xr_semantic_array_member_argument_is_exact(shape, argument, argument_type, i,
                                                        element_type_index))
            return false;
        if (is_element)
            element = argument->value;
    }
    if (element_value)
        *element_value = element;
    if (receiver_result)
        /* Both spellings need the same dynamic owned binding: one hands the
         * receiver back, the other builds a string, and neither is a scalar
         * the row states outright. */
        *receiver_result = shape->result_shape == XR_ARRAY_MEMBER_RESULT_RECEIVER ||
                           shape->result_shape == XR_ARRAY_MEMBER_RESULT_STRING;
    return true;
}

/* The tagged-store lane is deliberately narrower than the scalar member
 * family. It is exactly the shape whose frozen lifecycle consumes one local
 * source-class instance into Array storage and releases it when erased or when
 * the container is destroyed. */
static bool semantic_array_member_tagged_store_is_exact(const XrSemanticPlan *plan,
                                                        const XrSemanticOperationRecord *operation,
                                                        uint32_t *semantic_operand,
                                                        uint32_t *element_value,
                                                        uint32_t *element_type_index) {
    uint32_t element = XR_SEMANTIC_INDEX_NONE;
    uint32_t operand_count = 0, metadata_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    if (!semantic_array_member_scalar_is_exact(plan, operation, &element, NULL) || !operands ||
        !metadata || !children || operation->metadata_begin >= metadata_count ||
        operation->operand_begin >= operand_count)
        return false;
    const XrArrayMemberShape *shape =
        xr_array_member_shape(metadata[operation->metadata_begin], operation->operand_count);
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(plan, receiver->type);
    if (!shape || shape->element_access != XR_ARRAY_MEMBER_ELEMENT_ACCESS_STORE ||
        shape->reference_action != XR_ARRAY_MEMBER_REFERENCE_CONSUME_INTO_STORAGE ||
        shape->reference_drop != XR_ARRAY_MEMBER_REFERENCE_DROP_RELEASE_ON_ERASE_OR_DESTROY ||
        shape->element_operand == 0 || shape->element_operand >= operation->operand_count ||
        !receiver_type || receiver_type->child_begin >= child_count)
        return false;
    bool exact_push = strcmp(shape->selector, "push") == 0 && operation->operand_count == 2 &&
                      operation->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_PUSH << 1;
    bool exact_fill = strcmp(shape->selector, "fill") == 0 && operation->operand_count == 4 &&
                      operation->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_FILL << 1;
    if (!exact_push && !exact_fill)
        return false;
    uint32_t type_index = children[receiver_type->child_begin];
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    uint32_t operand_index = operation->operand_begin + shape->element_operand;
    if (!type || operand_index >= operand_count ||
        xr_semantic_class_instance_type_source_class(plan, type) == XR_SEMANTIC_INDEX_NONE ||
        operands[operand_index].value != element)
        return false;
    if (semantic_operand)
        *semantic_operand = operand_index;
    if (element_value)
        *element_value = element;
    if (element_type_index)
        *element_type_index = type_index;
    return true;
}

static bool stable_id_is_zero(XrStableId id);

/* The module-init import reference of a native stdlib namespace. Its frozen
 * import classification is resolved against the native definition registry
 * rather than against a compiled module, and its metadata pair names the
 * module path with an empty member, so a member import and a source-module
 * namespace both stay outside this authority. */
static bool semantic_native_module_import_is_exact(const XrSemanticPlan *plan,
                                                   const XrSemanticOperationRecord *record,
                                                   const char **out_module_path) {
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const XrSemanticTypeRecord *type =
        record ? xr_semantic_plan_type(plan, record->result_type) : NULL;
    if (!record || !type || !metadata || record->opcode != XI_IMPORT_REF || record->function != 0 ||
        record->operand_count != 0 || record->metadata_count != 2 ||
        record->metadata_begin + 1u >= metadata_count ||
        record->import_resolution != XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB ||
        record->semantic_immediate < -1 || record->semantic_immediate > UINT16_MAX ||
        record->allocation_key || !stable_id_is_zero(record->allocation_id) ||
        record->constant != XR_SEMANTIC_INDEX_NONE ||
        record->callable_function != XR_SEMANTIC_INDEX_NONE || record->auxiliary_kind != 0 ||
        record->effects != xi_generated_op_effects(XI_IMPORT_REF) ||
        record->flags != xi_generated_op_default_flags(XI_IMPORT_REF) ||
        record->ownership_use != xi_generated_op_own_use(XI_IMPORT_REF) ||
        record->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        record->result_alias_operand != -1 ||
        record->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        record->return_parameter != -1 || record->return_complete != 1 ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    const char *module_path = metadata[record->metadata_begin];
    const char *member = metadata[record->metadata_begin + 1u];
    if (!module_path || !member || member[0] != '\0' ||
        !xr_stdlib_metadata_module_known(module_path))
        return false;
    if (out_module_path)
        *out_module_path = module_path;
    return true;
}

/* The shared-slot read that republishes the namespace inside a function. */
static bool semantic_native_module_load_is_exact(const XrSemanticPlan *plan,
                                                 const XrSemanticOperationRecord *record) {
    const XrSemanticTypeRecord *type =
        record ? xr_semantic_plan_type(plan, record->result_type) : NULL;
    return record && type && record->opcode == XI_GET_SHARED && record->operand_count == 0 &&
           record->metadata_count == 0 && record->semantic_immediate >= 0 &&
           record->semantic_immediate <= UINT16_MAX && !record->allocation_key &&
           stable_id_is_zero(record->allocation_id) && record->constant == XR_SEMANTIC_INDEX_NONE &&
           record->callable_function == XR_SEMANTIC_INDEX_NONE && record->auxiliary_kind == 0 &&
           record->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
           record->effects == xi_generated_op_effects(XI_GET_SHARED) &&
           record->flags == xi_generated_op_default_flags(XI_GET_SHARED) &&
           record->ownership_use == xi_generated_op_own_use(XI_GET_SHARED) &&
           record->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
           record->result_alias_operand == -1 &&
           record->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
           record->return_parameter == -1 && record->return_complete == 1 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT);
}

/* Rebuilt from the frozen rows: the load reads a module shared slot, exactly
 * one module-init store publishes that slot, and the stored value is the
 * module-init import reference above. The returned module path is the frozen
 * metadata string, never a backend guess. */
static const char *semantic_native_module_namespace_path(const XrSemanticPlan *plan,
                                                         uint32_t receiver_value) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    const XrSemanticOperationRecord *load = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->result_value != receiver_value)
            continue;
        if (load)
            return NULL;
        load = candidate;
    }
    if (!semantic_native_module_load_is_exact(plan, load))
        return NULL;
    const XrSemanticOperationRecord *store = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->opcode != XI_SET_SHARED || candidate->function != 0 ||
            candidate->semantic_immediate != load->semantic_immediate)
            continue;
        if (store)
            return NULL;
        store = candidate;
    }
    if (!store || store->operand_count != 1 || store->operand_begin >= operand_count)
        return NULL;
    const XrSemanticOperandRecord *stored = &operands[store->operand_begin];
    if (stored->role != XR_SEM_OPERAND_VALUE || stored->parameter != -1 ||
        stored->ownership_action != XR_SEM_OPERAND_CONSUME || stored->flags != 0 ||
        stored->type != load->result_type)
        return NULL;
    const XrSemanticOperationRecord *import = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->result_value != stored->value)
            continue;
        if (import)
            return NULL;
        import = candidate;
    }
    const char *module_path = NULL;
    return import && import->result_type == load->result_type &&
                   semantic_native_module_import_is_exact(plan, import, &module_path)
               ? module_path
               : NULL;
}

/* A native stdlib namespace member call with a plain scalar contract. The
 * frozen definition registry names one implementation for the module path plus
 * the selector, the receiver is a namespace handle rather than a value, and
 * every argument and the result cross the boundary as one plain scalar, so the
 * row states no ownership obligation of its own. */
static bool semantic_native_module_scalar_call_shape_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const char **out_selector, uint32_t *out_receiver_value, uint32_t *argument_count) {
    uint32_t operands_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operands_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NATIVE_MODULE_SCALAR_CALL ||
        operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & 1) != 0 || operation->operand_count == 0 ||
        operation->operand_begin >= operands_count ||
        operation->operand_count > operands_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        !metadata || (operation->flags & XI_FLAG_MAY_SUSPEND) != 0 ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        !xr_semantic_native_module_boundary_type_is_exact(
            xr_semantic_plan_type(plan, operation->result_type), true))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(plan, operation->function);
    if (!function || receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->value < function->value_begin ||
        receiver->value >= function->value_begin + function->value_count ||
        operation->result_value < function->value_begin ||
        operation->result_value >= function->value_begin + function->value_count)
        return false;
    for (uint16_t i = 1; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = receiver + i;
        if (argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != (int16_t) (i - 1) ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            !xr_semantic_native_module_boundary_type_is_exact(
                xr_semantic_plan_type(plan, argument->type), false))
            return false;
    }
    if (out_selector)
        *out_selector = metadata[operation->metadata_begin];
    if (out_receiver_value)
        *out_receiver_value = receiver->value;
    if (argument_count)
        *argument_count = (uint32_t) (operation->operand_count - 1u);
    return true;
}

static bool semantic_native_module_scalar_call_is_exact(const XrSemanticPlan *plan,
                                                        const XrSemanticOperationRecord *operation,
                                                        uint32_t *argument_count) {
    const char *selector = NULL;
    uint32_t receiver_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t arity = 0;
    if (!semantic_native_module_scalar_call_shape_is_exact(plan, operation, &selector,
                                                           &receiver_value, &arity))
        return false;
    const char *module_path = semantic_native_module_namespace_path(plan, receiver_value);
    if (!module_path ||
        !xr_stdlib_metadata_exact_native_direct_member(module_path, selector, (uint16_t) arity))
        return false;
    if (argument_count)
        *argument_count = arity;
    return true;
}

/* A yieldable namespace member is identified by the frozen SemanticPlan call
 * target, not by its spelling alone. Rebuild the registry tuple and stable
 * target identity here so the namespace storage family consumes the same
 * exact invocation authority as the call family. */
static bool
semantic_native_module_yieldable_call_is_exact(const XrSemanticPlan *plan, uint32_t operation_index,
                                               const XrSemanticOperationRecord *operation,
                                               const char *module_path, uint32_t receiver_value,
                                               uint32_t receiver_type) {
    uint32_t operand_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || !module_path || !module_path[0] || !operands || !metadata ||
        operation->opcode != XI_CALL_METHOD || (operation->semantic_immediate & 1) != 0 ||
        operation->operand_count == 0 || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    if (receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW || receiver->value != receiver_value ||
        receiver->type != receiver_type)
        return false;
    for (uint16_t i = 1; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = receiver + i;
        if (argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != (int16_t) (i - 1) ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT)
            return false;
    }
    const char *selector = metadata[operation->metadata_begin];
    const XrStdlibDefEntry *binding =
        selector ? xr_stdlib_metadata_unique_func(module_path, selector) : NULL;
    if (!binding || !binding->signature || !binding->vm || !binding->vm_binding ||
        strcmp(binding->vm_binding, "yieldable") != 0 ||
        operation->operand_count != (uint16_t) (binding->argc + 1u))
        return false;
    const XrSemanticCallTargetRecord *target = NULL;
    uint32_t target_count = (uint32_t) xr_semantic_plan_call_target_count(plan);
    for (uint32_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *candidate = xr_semantic_plan_call_target(plan, i);
        if (!candidate || candidate->operation != operation_index)
            continue;
        if (target)
            return false;
        target = candidate;
    }
    XrStableId zero = {{0}};
    if (!target || target->kind != XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE ||
        target->function != XR_SEMANTIC_INDEX_NONE ||
        target->dependency != XR_SEMANTIC_INDEX_NONE ||
        target->source_export != XR_SEMANTIC_INDEX_NONE ||
        target->callable_type != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(target->export_identity, zero) ||
        !xr_stable_id_equal(target->callee_function, zero))
        return false;
    char operation_id[XR_STABLE_ID_BYTES * 2 + 1];
    char key[320];
    xr_stable_id_hex(operation->id, operation_id);
    int length = snprintf(
        key, sizeof(key), "call-target-v5:schema=%u:operation=%s:native-namespace=%s.%s:kind=%u",
        XR_SEMANTIC_SCHEMA_VERSION, operation_id, module_path, selector, (unsigned) target->kind);
    XrStableId expected_id;
    XrFingerprint digest;
    return length > 0 && (size_t) length < sizeof(key) && target->canonical_key &&
           strcmp(target->canonical_key, key) == 0 &&
           xr_stable_id_from_key(key, &expected_id, &digest) &&
           xr_stable_id_equal(target->id, expected_id);
}

/* A native stdlib namespace value is one of the two rows above, and it is
 * admitted only when a proven member call consumes it: the namespace handle is
 * a borrowed compiler-owned reference with no other statable use. */
static bool
semantic_native_module_namespace_value_is_exact(const XrSemanticPlan *plan,
                                                const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    if (!operation)
        return false;
    if (operation->opcode == XI_IMPORT_REF) {
        if (!semantic_native_module_import_is_exact(plan, operation, NULL))
            return false;
    } else if (operation->opcode == XI_GET_SHARED) {
        if (!semantic_native_module_load_is_exact(plan, operation) ||
            !semantic_native_module_namespace_path(plan, operation->result_value))
            return false;
    } else {
        return false;
    }
    /* Prove the whole namespace lifetime from the rows: every use is a
     * reference-count edge, the module-init store, or the receiver of a member
     * call this plan already proved exact. Anything else, including a call this
     * authority refused, leaves the value without storage. */
    bool consumed = false;
    const char *module_path = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(plan, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            return false;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand = &operands[use->operand_begin + a];
            if (operand->value != operation->result_value)
                continue;
            if ((use->opcode == XI_RETAIN || use->opcode == XI_RELEASE ||
                 use->opcode == XI_SET_SHARED) &&
                a == 0)
                continue;
            const char *selector = NULL;
            uint32_t receiver_value = XR_SEMANTIC_INDEX_NONE;
            uint32_t arity = 0;
            if (use->opcode != XI_CALL_METHOD || a != 0)
                return false;
            if (!module_path)
                module_path = semantic_native_module_namespace_path(plan, operation->result_value);
            bool scalar = semantic_native_module_scalar_call_shape_is_exact(
                              plan, use, &selector, &receiver_value, &arity) &&
                          receiver_value == operation->result_value && module_path &&
                          xr_stdlib_metadata_exact_native_direct_member(module_path, selector,
                                                                        (uint16_t) arity);
            bool yieldable = semantic_native_module_yieldable_call_is_exact(
                plan, i, use, module_path, operation->result_value, operation->result_type);
            if (!scalar && !yieldable)
                return false;
            consumed = true;
        }
    }
    return operation->opcode == XI_IMPORT_REF || consumed;
}

/* A freshly allocated `Array<T>` carries an owned tagged outer value plus one
 * exact element-storage identity. Scalar elements use their direct storage;
 * source-class elements use tagged storage because their member-store family
 * freezes consume-on-write and release-on-erase/destroy. Other reference
 * elements remain outside this family until they state an equally complete
 * lifecycle contract. The capacity operand is the sole ordered value operand. */
static bool semantic_array_allocation_is_exact(const XrSemanticPlan *plan,
                                               const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    if (!plan || !operation || !operands || !children || operation->opcode != XI_ARRAY_NEW ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE || operation->metadata_count != 0 ||
        operation->semantic_immediate != 0 || operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->effects != xi_generated_op_effects(XI_ARRAY_NEW) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_complete != 1 ||
        operation->return_parameter != -1 || operation->result_alias_operand != -1 ||
        !xr_semantic_allocation_identity_is_canonical(operation))
        return false;
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, operation->result_type);
    if (!xr_semantic_array_type_row_is_exact(type) || type->child_begin >= child_count)
        return false;
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(plan, children[type->child_begin]);
    const XrSemanticOperandRecord *capacity = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *capacity_type = xr_semantic_plan_type(plan, capacity->type);
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(plan, operation->function);
    uint16_t capacity_kind = XR_MACHINE_REP_COUNT;
    uint8_t semantic_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t type_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    bool source_class_element =
        xr_semantic_class_instance_type_source_class(plan, element) != XR_SEMANTIC_INDEX_NONE;
    bool element_storage_exact = false;
    bool semantic_storage_exact = false;
    if (element && (element->flags & XR_SEM_TYPE_REFERENCE_CAPABLE) == 0) {
        element_storage_exact = xr_target_array_storage_from_type(element, &type_storage);
        semantic_storage_exact = xr_target_array_storage_from_semantic(
                                     operation->array_element_storage, &semantic_storage) &&
                                 semantic_storage == type_storage;
    } else if (source_class_element) {
        /* SemanticPlan spells the full tagged XrValue lane as XR_ELEM_ANY;
         * together with the exact source-class child that answer is no longer
         * ambiguous, so TargetPlan can freeze its backend-neutral TAGGED name. */
        type_storage = XR_TARGET_ARRAY_STORAGE_TAGGED;
        element_storage_exact = true;
        semantic_storage_exact = operation->array_element_storage == XR_ELEM_ANY;
    }
    return element && capacity_type && function && element_storage_exact &&
           semantic_storage_exact &&
           classify_scalar_type(capacity_type, &capacity_kind) == XR_TARGET_SCALAR_VALUE &&
           capacity_kind == XR_MACHINE_REP_I64 && capacity->role == XR_SEM_OPERAND_VALUE &&
           capacity->parameter == -1 && capacity->flags == 0 &&
           capacity->ownership_action == XR_SEM_OPERAND_CONSUME &&
           operation->result_value >= function->value_begin &&
           operation->result_value < function->value_begin + function->value_count;
}

/* An exact tagged value at a direct-local boundary, and for `Array<T>` the
 * element storage that boundary is entitled to know.
 *
 * Two kinds of carrier cross such a boundary and they ask different questions
 * of the element. A carrier that holds the tagged outer value -- a by-value
 * parameter, an owned result, a shared read -- copies one tagged value and
 * shares one allocation; it never reaches an element, so what the elements are
 * is not its question and `Array<String>` is as carryable as `Array<i64>`. A
 * carrier that hands the callee a pointer into the caller's cell does reach
 * them: the callee may index and rewrite elements, so it needs an exact element
 * storage identity. Scalars state their direct storage. A source-class element
 * states tagged storage because its nominal class identity and the Array
 * member-store lifecycle are both frozen by SemanticPlan; other reference-
 * capable elements remain outside this boundary.
 *
 * `indexes_elements` and `admits_instance` state which questions the carrier is
 * allowed to ask. Keeping them explicit prevents value carriers from inheriting
 * ref-only class admission or element-storage requirements.
 *
 * `storage` is the element's scalar storage when the element has one and NONE
 * otherwise -- exactly the value the layout row records for the type. */
static bool semantic_direct_local_tagged_boundary_type_is_exact(const XrSemanticPlan *plan,
                                                                uint32_t type_index,
                                                                bool indexes_elements,
                                                                bool admits_instance,
                                                                uint8_t *storage) {
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    uint8_t element = XR_TARGET_ARRAY_STORAGE_NONE;
    /* A class instance crosses a *ref boundary* in exactly the carrier an Array
     * does -- a tagged value the caller owns, reached through a pointer -- but
     * it has no elements, so it states no element storage.  Only the boundary
     * call sites ask for it: the member-selector and result judgements below
     * are about Arrays as containers, and admitting an instance there would
     * name a receiver those families do not describe. */
    if (admits_instance &&
        (xr_semantic_class_instance_type_source_class(plan, type) != XR_SEMANTIC_INDEX_NONE ||
         semantic_stringbuilder_type_is_exact(type))) {
        if (storage)
            *storage = XR_TARGET_ARRAY_STORAGE_NONE;
        return true;
    }
    if (!children || !xr_semantic_array_type_row_is_exact(type) || type->child_begin >= child_count)
        return false;
    const XrSemanticTypeRecord *element_type =
        xr_semantic_plan_type(plan, children[type->child_begin]);
    if (!xr_target_array_storage_from_type(element_type, &element)) {
        bool source_class_element = xr_semantic_class_instance_type_source_class(
                                        plan, element_type) != XR_SEMANTIC_INDEX_NONE;
        if (indexes_elements && !source_class_element)
            return false;
        element = xr_semantic_tagged_string_type_is_exact(element_type) || source_class_element
                      ? XR_TARGET_ARRAY_STORAGE_TAGGED
                      : XR_TARGET_ARRAY_STORAGE_NONE;
    }
    if (storage)
        *storage = element;
    return true;
}

static bool semantic_direct_local_array_type_is_exact(const XrSemanticPlan *plan,
                                                      uint32_t type_index, bool indexes_elements,
                                                      uint8_t *storage) {
    return semantic_direct_local_tagged_boundary_type_is_exact(plan, type_index, indexes_elements,
                                                               false, storage);
}

static bool append_tagged_boundary_layout_intent(XrTargetPlanBuilder *builder,
                                                 uint32_t semantic_type, uint8_t storage,
                                                 const XrTargetMachineRepRecord *memory_rep,
                                                 bool admits_instance, char *error,
                                                 size_t error_size) {
    uint8_t expected = XR_TARGET_ARRAY_STORAGE_NONE;
    /* This layout belongs to the boundary family alone, so it admits the same
     * set that family does -- including a class instance, which lays out as the
     * tagged value it is and states no element storage. */
    if (!semantic_direct_local_tagged_boundary_type_is_exact(
            builder ? builder->semantic_plan : NULL, semantic_type, false, admits_instance,
            &expected) ||
        expected != storage ||
        !append_layout_intent(builder, semantic_type, XR_TARGET_LAYOUT_DYNAMIC, 0, memory_rep,
                              error, error_size))
        return false;
    for (uint32_t i = 0; i < builder->layout_intent_count; i++) {
        XrTargetLayoutIntent *intent = &builder->layout_intents[i];
        if (intent->semantic_type != semantic_type)
            continue;
        if (intent->array_element_storage != storage)
            return fail(error, error_size, "XR_TARGET_1002",
                        "Array layout has conflicting element storage");
        return true;
    }
    return fail(error, error_size, "XR_TARGET_1002", "Array layout intent is missing");
}

/* A borrowed `Array<T>` parameter, in whichever of the two passing modes the
 * declaration asked for.
 *
 * Both modes borrow, and for the same reason: an Array is a reference-capable
 * container, so the callee sees the caller's allocation either way and releases
 * nothing. The modes differ only in what the callee may do to the binding. A
 * ref parameter names the caller's cell and may rebind it, so it arrives as a
 * pointer to that cell; a by-value parameter arrives as the tagged value
 * itself. Everything else the two demand of the declaration is identical, which
 * is why they are one judgement with the mode as its parameter rather than two
 * that could drift apart.
 *
 * The mode is also what decides how much of the element the boundary has to
 * know: only the ref parameter reaches elements, so only it demands one scalar
 * element storage. */
static bool
semantic_direct_local_array_parameter_is_exact(const XrSemanticPlan *plan,
                                               const XrSemanticParameterRecord *parameter,
                                               uint8_t mode, uint8_t *storage) {
    return parameter && parameter->function < xr_semantic_plan_function_count(plan) &&
           parameter->value != XR_SEMANTIC_INDEX_NONE && parameter->mode == mode &&
           parameter->ownership == XI_OWN_BORROWED &&
           parameter->transfer_mode == XR_TRANSFER_SHARE &&
           (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) == 0 && parameter->reserved == 0 &&
           semantic_direct_local_array_type_is_exact(plan, parameter->type, mode == XR_PARAM_REF,
                                                     storage);
}

/* What a ref parameter may bind. An Array states an element storage because the
 * callee reaches its elements; a class instance has no elements to reach, so it
 * states none -- but it crosses the boundary in exactly the carrier an Array
 * does: a tagged value the caller owns, reached through a pointer, written back
 * through the same place read. Only the ref spelling is widened; a class passed
 * by value is already the argument family's to bind, and taking it here would
 * claim a value that family answers for. */
static bool semantic_direct_local_tagged_ref_parameter_is_exact(
    const XrSemanticPlan *plan, const XrSemanticParameterRecord *parameter, uint8_t *storage) {
    return parameter && parameter->function < xr_semantic_plan_function_count(plan) &&
           parameter->value != XR_SEMANTIC_INDEX_NONE && parameter->mode == XR_PARAM_REF &&
           parameter->ownership == XI_OWN_BORROWED &&
           parameter->transfer_mode == XR_TRANSFER_SHARE &&
           (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) == 0 && parameter->reserved == 0 &&
           semantic_direct_local_tagged_boundary_type_is_exact(plan, parameter->type, true, true,
                                                               storage);
}

static bool semantic_direct_local_array_value_parameter_is_exact(
    const XrSemanticPlan *plan, const XrSemanticParameterRecord *parameter, uint8_t *storage) {
    return semantic_direct_local_array_parameter_is_exact(plan, parameter, XR_PARAM_READ, storage);
}

/* A direct-local call that returns `T?`.
 *
 * The nullable carrier is one tagged word holding either the null tag or a
 * scalar payload, and it owns no reference either way -- which is exactly why
 * the family that binds nullable scalars elsewhere can bind this result too.
 * The return still has to be a whole, fresh answer: a parameter-forwarded or
 * aliased return would hand back something whose extent this plan cannot
 * state, so those stay refused. */
static bool
semantic_direct_local_nullable_scalar_result_is_exact(const XrSemanticPlan *plan,
                                                      const XrSemanticOperationRecord *operation,
                                                      const XrSemanticFunctionRecord *callee) {
    return plan && operation && callee &&
           (operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL) &&
           operation->result_type == callee->return_type &&
           operation->result_value != XR_SEMANTIC_INDEX_NONE &&
           operation->result_alias_operand == -1 && operation->return_parameter == -1 &&
           callee->return_parameter == -1 &&
           semantic_nullable_scalar_type_is_exact(
               xr_semantic_plan_type(plan, operation->result_type));
}

/* A direct-local call that returns a freshly owned `Array<T>`.
 *
 * The container is a dynamic value, not an aggregate slot the caller owns, so
 * the result is a transfer: the callee hands back the allocation and the caller
 * takes ownership of the outer tagged value, exactly as it does for an owned
 * String. The return must be fresh and whole -- an aliased or parameter-
 * forwarded return would hand back a borrow whose extent this plan cannot
 * state, so it stays fail closed. */
static bool semantic_direct_local_array_result_is_exact(const XrSemanticPlan *plan,
                                                        const XrSemanticOperationRecord *operation,
                                                        const XrSemanticFunctionRecord *callee) {
    return plan && operation && callee &&
           (operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL) &&
           operation->result_type == callee->return_type &&
           operation->result_value != XR_SEMANTIC_INDEX_NONE &&
           operation->result_alias_operand == -1 && operation->return_parameter == -1 &&
           operation->return_complete == 1 && operation->return_provenance == XR_SEM_RETURN_OWNED &&
           callee->return_parameter == -1 && callee->return_provenance == XR_SEM_RETURN_OWNED &&
           semantic_direct_local_array_type_is_exact(plan, operation->result_type, false, NULL);
}

static bool
semantic_direct_local_tagged_ref_place_is_exact(const XrSemanticPlan *plan,
                                                const XrSemanticOperandRecord *call_operand,
                                                uint32_t *storage_value) {
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticOperationRecord *definition = NULL;
    for (uint32_t i = 0; operands && i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->result_value != call_operand->value)
            continue;
        if (definition)
            return false;
        definition = candidate;
    }
    if (!definition || definition->opcode != XI_LOCAL_ADDR ||
        definition->result_type != call_operand->type || definition->operand_count != 1 ||
        definition->operand_begin >= operand_count ||
        definition->effects != xi_generated_op_effects(XI_LOCAL_ADDR) ||
        definition->flags != xi_generated_op_default_flags(XI_LOCAL_ADDR) ||
        definition->ownership_use != xi_generated_op_own_use(XI_LOCAL_ADDR) ||
        definition->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        definition->result_alias_operand != -1 ||
        definition->intrinsic_kind != XR_SEM_INTRINSIC_NONE || definition->metadata_count != 0 ||
        definition->auxiliary_kind != XI_AUX_KIND_NONE || definition->semantic_immediate != 0 ||
        definition->constant != XR_SEMANTIC_INDEX_NONE ||
        definition->callable_function != XR_SEMANTIC_INDEX_NONE ||
        definition->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE)
        return false;
    const XrSemanticOperandRecord *source = &operands[definition->operand_begin];
    if (source->type != call_operand->type || source->role != XR_SEM_OPERAND_VALUE ||
        source->parameter != -1 || source->parameter_mode != XR_PARAM_READ ||
        source->access != XR_CALL_ARG_PLAIN || source->origin != XI_PLACE_ORIGIN_NONE ||
        source->lifetime != XI_PLACE_LIFETIME_NONE || source->escape != XI_PLACE_ESCAPE_NONE ||
        source->flags != 0 || source->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    if (storage_value)
        *storage_value = source->value;
    return true;
}

/* The borrowed read of an `Array<T>` held in a shared cell. A local variable is
 * a shared cell, so every mention of the array after its binding is a GET_SHARED
 * whose result is the same allocation the array construction produced. Nothing
 * in this judgement is about a call boundary: the read is a borrowed dynamic
 * value wherever it appears, and the ref-argument boundary is only one place
 * that has to prove it.
 *
 * The read type is proved here rather than left to the caller, because the
 * verifier that re-proves this shape names the Array type itself: a judgement
 * that admitted every shared read and relied on its one caller to have narrowed
 * the type would claim String and nested-container reads the moment it was
 * applied anywhere else. */
static bool semantic_direct_local_tagged_ref_place_load_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t operation_index, uint32_t place_value, uint32_t array_type) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!operation || !operands || operation->opcode != XI_PLACE_LOAD ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE || operation->result_type != array_type ||
        operation->effects != xi_generated_op_effects(XI_PLACE_LOAD) ||
        operation->flags != xi_generated_op_default_flags(XI_PLACE_LOAD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_PLACE_LOAD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_alias_operand != -1 ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE || operation->metadata_count != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE || operation->semantic_immediate != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation_index >= xr_semantic_plan_operation_count(plan))
        return false;
    const XrSemanticOperandRecord *place = &operands[operation->operand_begin];
    return place->value == place_value && place->type == array_type &&
           place->role == XR_SEM_OPERAND_VALUE && place->parameter == -1 &&
           place->parameter_mode == XR_PARAM_READ && place->access == XR_CALL_ARG_PLAIN &&
           place->origin == XI_PLACE_ORIGIN_NONE && place->lifetime == XI_PLACE_LIFETIME_NONE &&
           place->escape == XI_PLACE_ESCAPE_NONE && place->flags == 0 &&
           place->ownership_action == XR_SEM_OPERAND_BORROW;
}

static bool target_array_fill_type_is_exact(const XrSemanticTypeRecord *type,
                                            uint8_t element_storage) {
    uint8_t ignored_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!type)
        return false;
    /* The same shape gate the semantic layer applies before reading the
     * storage: a row carrying a builtin id, children, an aggregate extent or
     * any flag at all is not the bare scalar a fill element must be, whatever
     * storage it would map to. */
    if (type->builtin_type != XR_TID_NULL || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 || type->flags != 0)
        return false;
    if (element_storage == XR_TARGET_ARRAY_STORAGE_RUNE)
        return type->kind == XR_KIND_RUNE &&
               xr_target_array_storage_from_type(type, &ignored_storage);
    return element_storage > XR_TARGET_ARRAY_STORAGE_NONE &&
           element_storage < XR_TARGET_ARRAY_STORAGE_RUNE &&
           (type->kind == XR_KIND_INT || type->kind == XR_KIND_FLOAT ||
            type->kind == XR_KIND_BOOL) &&
           xr_target_array_storage_from_type(type, &ignored_storage);
}

/* The SemanticPlan intrinsic kind is the dispatch identity. Metadata is
 * intentionally only required to be structurally present; neither selector
 * spelling, live Xi type recovery, nor operand-count inference participates in
 * the TargetPlan proof. */
static bool semantic_array_fill_scalar_is_exact(const XrSemanticPlan *plan,
                                                const XrSemanticOperationRecord *operation,
                                                uint32_t *receiver_value, uint32_t *fill_value,
                                                uint8_t *target_storage) {
    uint32_t operand_count = 0, child_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    (void) xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || !operands || !children ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_FILL_SCALAR ||
        operation->opcode != XI_CALL_METHOD || operation->operand_count != 2 ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        operation->semantic_immediate != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->result_alias_operand != 0 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *fill = receiver + 1;
    const XrSemanticTypeRecord *array = xr_semantic_plan_type(plan, receiver->type);
    if (!xr_semantic_array_type_row_is_exact(array) || array->child_begin >= child_count)
        return false;
    uint32_t element_index = children[array->child_begin];
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(plan, element_index);
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t semantic_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!element || operation->result_type != receiver->type || fill->type != element_index ||
        !xr_target_array_storage_from_type(element, &storage) ||
        !xr_target_array_storage_from_semantic(operation->array_element_storage,
                                               &semantic_storage) ||
        storage != semantic_storage || receiver->role != XR_SEM_OPERAND_RECEIVER ||
        receiver->parameter != -1 || receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        fill->role != XR_SEM_OPERAND_ARGUMENT || fill->parameter != 0 ||
        fill->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        fill->ownership_action != XR_SEM_OPERAND_CONSUME)
        return false;
    if (receiver_value)
        *receiver_value = receiver->value;
    if (fill_value)
        *fill_value = fill->value;
    if (target_storage)
        *target_storage = storage;
    return true;
}

static bool semantic_array_hof_is_exact(const XrSemanticPlan *plan,
                                        const XrSemanticOperationRecord *operation,
                                        uint8_t *target_kind, uint8_t *receiver_storage,
                                        uint8_t *result_storage, uint32_t *receiver_value,
                                        uint32_t *callback_value, uint32_t *initial_value) {
    uint32_t operand_count = 0, child_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    (void) xr_semantic_plan_metadata(plan, &metadata_count);
    uint8_t kind = XR_TARGET_ARRAY_HOF_NONE;
    if (operation) {
        switch (operation->array_hof_kind) {
            case XR_SEM_ARRAY_HOF_MAP:
                kind = XR_TARGET_ARRAY_HOF_MAP;
                break;
            case XR_SEM_ARRAY_HOF_FILTER:
                kind = XR_TARGET_ARRAY_HOF_FILTER;
                break;
            case XR_SEM_ARRAY_HOF_REDUCE:
                kind = XR_TARGET_ARRAY_HOF_REDUCE;
                break;
            default:
                break;
        }
    }
    uint16_t expected_operands = kind == XR_TARGET_ARRAY_HOF_REDUCE ? 3u : 2u;
    if (!plan || !operation || !operands || !children ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_HOF ||
        kind == XR_TARGET_ARRAY_HOF_NONE || operation->opcode != XI_CALL_METHOD ||
        operation->operand_count != expected_operands || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        operation->semantic_immediate != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function >= xr_semantic_plan_function_count(plan) ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1)
        return false;
    const XrSemanticOperandRecord *rows = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *array = xr_semantic_plan_type(plan, rows[0].type);
    if (!xr_semantic_array_type_row_is_exact(array) || array->child_begin >= child_count)
        return false;
    uint32_t source_element = children[array->child_begin];
    const XrSemanticTypeRecord *source_type = xr_semantic_plan_type(plan, source_element);
    uint8_t source = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t frozen_source = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!xr_target_array_storage_from_type(source_type, &source) ||
        !xr_target_array_storage_from_semantic(operation->array_element_storage, &frozen_source) ||
        source != frozen_source)
        return false;
    uint32_t result_element = operation->result_type;
    if (kind != XR_TARGET_ARRAY_HOF_REDUCE) {
        const XrSemanticTypeRecord *result_array =
            xr_semantic_plan_type(plan, operation->result_type);
        if (!xr_semantic_array_type_row_is_exact(result_array) ||
            result_array->child_begin >= child_count)
            return false;
        result_element = children[result_array->child_begin];
        if (kind == XR_TARGET_ARRAY_HOF_FILTER &&
            (operation->result_type != rows[0].type || result_element != source_element))
            return false;
    } else if (rows[2].type != operation->result_type) {
        return false;
    }
    const XrSemanticTypeRecord *result_type = xr_semantic_plan_type(plan, result_element);
    uint8_t result = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t frozen_result = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!xr_target_array_storage_from_type(result_type, &result) ||
        !xr_target_array_storage_from_semantic(operation->array_result_element_storage,
                                               &frozen_result) ||
        result != frozen_result)
        return false;
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(plan, operation->callable_function);
    uint16_t expected_parameters = kind == XR_TARGET_ARRAY_HOF_REDUCE ? 2u : 1u;
    if (!callee || callee->parent != operation->function || callee->capture_count != 0 ||
        callee->parameter_count != expected_parameters ||
        (callee->semantic_effects & (XI_EFFECT_SIDE_EFFECT | XI_EFFECT_MEMORY_WRITE |
                                     XI_EFFECT_MAY_THROW | XI_EFFECT_MAY_SUSPEND)) != 0 ||
        callee->parameter_begin > xr_semantic_plan_parameter_count(plan) ||
        callee->parameter_count > xr_semantic_plan_parameter_count(plan) - callee->parameter_begin)
        return false;
    const XrSemanticParameterRecord *first =
        xr_semantic_plan_parameter(plan, callee->parameter_begin);
    const XrSemanticParameterRecord *second =
        expected_parameters == 2u ? xr_semantic_plan_parameter(plan, callee->parameter_begin + 1u)
                                  : NULL;
    if (!first || first->function != operation->callable_function || first->ordinal != 0 ||
        first->type != (kind == XR_TARGET_ARRAY_HOF_REDUCE ? result_element : source_element) ||
        (second && (second->function != operation->callable_function || second->ordinal != 1 ||
                    second->type != source_element)))
        return false;
    if (kind == XR_TARGET_ARRAY_HOF_FILTER) {
        const XrSemanticTypeRecord *return_type = xr_semantic_plan_type(plan, callee->return_type);
        uint16_t rep = XR_MACHINE_REP_COUNT;
        if (!return_type || classify_scalar_type(return_type, &rep) != XR_TARGET_SCALAR_VALUE ||
            rep != XR_MACHINE_REP_I1)
            return false;
    } else if (callee->return_type != result_element) {
        return false;
    }
    const XrSemanticTypeRecord *callback_type = xr_semantic_plan_type(plan, rows[1].type);
    if (!callback_type || callback_type->kind != XR_KIND_FUNCTION ||
        callback_type->child_count != (uint32_t) expected_parameters + 1u ||
        callback_type->child_begin > child_count ||
        callback_type->child_count > child_count - callback_type->child_begin)
        return false;
    for (uint16_t i = 0; i < expected_parameters; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan, callee->parameter_begin + i);
        if (!parameter || children[callback_type->child_begin + i] != parameter->type)
            return false;
    }
    if (children[callback_type->child_begin + expected_parameters] != callee->return_type ||
        rows[0].role != XR_SEM_OPERAND_RECEIVER || rows[0].parameter != -1 ||
        rows[0].flags != XR_SEM_OPERAND_CALL_CONTRACT || rows[1].role != XR_SEM_OPERAND_ARGUMENT ||
        rows[1].parameter != 0 || rows[1].flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        (kind == XR_TARGET_ARRAY_HOF_REDUCE &&
         (rows[2].role != XR_SEM_OPERAND_ARGUMENT || rows[2].parameter != 1 ||
          rows[2].flags != XR_SEM_OPERAND_CALL_CONTRACT)))
        return false;
    bool result_exact = kind == XR_TARGET_ARRAY_HOF_REDUCE
                            ? operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT &&
                                  operation->return_provenance == XR_SEM_RETURN_NONE &&
                                  operation->return_complete == 0
                            : operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
                                  operation->return_provenance == XR_SEM_RETURN_OWNED &&
                                  operation->return_complete == 1;
    const XrSemanticOperationRecord *producer = NULL;
    uint32_t uses = 0;
    for (uint32_t i = 0; result_exact && i < xr_semantic_plan_operation_count(plan); i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (candidate && candidate->function == operation->function &&
            candidate->result_value == rows[1].value) {
            if (producer)
                return false;
            producer = candidate;
        }
    }
    for (uint32_t i = 0; result_exact && i < operand_count; i++)
        uses += operands[i].value == rows[1].value;
    if (!result_exact || !producer || producer >= operation || uses != 1 ||
        (producer->opcode != XI_CLOSURE_NEW &&
         (producer->opcode != XI_STACK_ALLOC || producer->semantic_immediate != XI_CLOSURE_NEW)) ||
        producer->callable_function != operation->callable_function ||
        producer->result_type != rows[1].type)
        return false;
    if (target_kind)
        *target_kind = kind;
    if (receiver_storage)
        *receiver_storage = source;
    if (result_storage)
        *result_storage = result;
    if (receiver_value)
        *receiver_value = rows[0].value;
    if (callback_value)
        *callback_value = rows[1].value;
    if (initial_value)
        *initial_value =
            kind == XR_TARGET_ARRAY_HOF_REDUCE ? rows[2].value : XR_SEMANTIC_INDEX_NONE;
    return true;
}

static bool semantic_array_intrinsic_is_exact(const XrSemanticPlan *plan,
                                              const XrSemanticOperationRecord *operation,
                                              uint8_t *target_kind, uint8_t *target_storage) {
    uint32_t operand_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    bool with_capacity =
        operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_WITH_CAPACITY;
    bool filled = operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_FILLED_NEW;
    uint16_t expected = with_capacity ? 1u : 2u;
    if (!plan || !operation || (!with_capacity && !filled) || !operands || !children ||
        operation->opcode != XI_CALL_BUILTIN || operation->operand_count != expected ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->semantic_immediate != 0 || operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_BUILTIN) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_BUILTIN) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_BUILTIN) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1 || !xr_semantic_allocation_identity_is_canonical(operation))
        return false;
    const XrSemanticTypeRecord *array = xr_semantic_plan_type(plan, operation->result_type);
    if (!xr_semantic_array_type_row_is_exact(array) || array->child_begin >= child_count)
        return false;
    uint32_t element_index = children[array->child_begin];
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(plan, element_index);
    const XrSemanticOperandRecord *count = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *count_type = xr_semantic_plan_type(plan, count->type);
    uint16_t count_rep = XR_MACHINE_REP_COUNT;
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t semantic_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!count_type || classify_scalar_type(count_type, &count_rep) != XR_TARGET_SCALAR_VALUE ||
        count_rep != XR_MACHINE_REP_I64 || count->role != XR_SEM_OPERAND_ARGUMENT ||
        count->parameter != 0 || count->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        count->ownership_action != XR_SEM_OPERAND_CONSUME ||
        !xr_target_array_storage_from_semantic(operation->array_element_storage,
                                               &semantic_storage) ||
        !xr_target_array_storage_from_type(element, &storage) || storage != semantic_storage)
        return false;
    if (filled) {
        const XrSemanticOperandRecord *fill = count + 1;
        const XrSemanticTypeRecord *fill_type = xr_semantic_plan_type(plan, fill->type);
        if (!target_array_fill_type_is_exact(fill_type, storage) ||
            fill->role != XR_SEM_OPERAND_ARGUMENT || fill->parameter != 1 ||
            fill->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            fill->ownership_action != XR_SEM_OPERAND_CONSUME)
            return false;
    }
    if (target_kind)
        *target_kind = with_capacity ? XR_TARGET_ARRAY_INTRINSIC_WITH_CAPACITY
                                     : XR_TARGET_ARRAY_INTRINSIC_FILLED_NEW;
    if (target_storage)
        *target_storage = storage;
    return true;
}

static bool semantic_json_namespace_type_is_exact(const XrSemanticTypeRecord *type) {
    char expected_type_key[160];
    int written =
        snprintf(expected_type_key, sizeof(expected_type_key),
                 "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:4:JSON[0]", (unsigned) XR_KIND_CLASS,
                 (unsigned) XR_TID_NULL, (unsigned) XR_SCALAR_REP_NONE);
    XrStableId zero = {{0}};
    return type && written > 0 && (size_t) written < sizeof(expected_type_key) &&
           type->kind == XR_KIND_CLASS && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) && type->canonical_key &&
           strcmp(type->canonical_key, expected_type_key) == 0;
}

/* The JSON class namespace is compiler owned: its receiver is a reserved
 * builtin global, so the frozen selector alone names one implementation.  The
 * receiver is a namespace handle rather than a value, which is why the call
 * carries no argument intent and folds the argument value into its identity. */
static bool semantic_json_namespace_value_is_exact(const XrSemanticPlan *plan,
                                                   const XrSemanticOperationRecord *operation,
                                                   uint32_t *argument_value) {
    uint32_t operands_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operands_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_JSON_NAMESPACE_VALUE ||
        operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & 1) != 0 || operation->operand_count != 2 ||
        operation->operand_begin + 1u >= operands_count || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "value") != 0 ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *argument = receiver + 1;
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(plan, receiver->type);
    const XrSemanticTypeRecord *result_type = xr_semantic_plan_type(plan, operation->result_type);
    if (!semantic_json_namespace_type_is_exact(receiver_type) || !result_type ||
        result_type->kind != XR_KIND_JSON || result_type->builtin_type != XR_TID_NULL ||
        result_type->child_count != 0 || result_type->scalar_rep != XR_SCALAR_REP_NONE ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != 0 ||
        argument->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return false;
    if (argument_value)
        *argument_value = argument->value;
    return true;
}

static bool stable_id_is_zero(XrStableId id) {
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++)
        if (id.bytes[i] != 0)
            return false;
    return true;
}

typedef enum XrTargetLeafProgramTypeKind {
    XR_TARGET_LEAF_PROGRAM_TYPE_INVALID = -1,
    XR_TARGET_LEAF_PROGRAM_TYPE_NOT_APPLICABLE = 0,
    XR_TARGET_LEAF_PROGRAM_TYPE_SCALAR,
    XR_TARGET_LEAF_PROGRAM_TYPE_AGGREGATE,
} XrTargetLeafProgramTypeKind;

/* The leaf-value aggregate family is the only program-wide authority for this
 * direct-call aggregate slice. A SemanticPlan from another family remains
 * outside this judgement; once the family is present, every count and row used
 * below must come from its pointer-free typed bindings. */
static const XrSemanticProgramProvenance *
semantic_leaf_program_provenance(const XrSemanticPlan *plan) {
    const XrSemanticProgramProvenance *provenance = xr_semantic_plan_program_provenance(plan);
    if (!provenance ||
        provenance->program_family != XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL)
        return NULL;
    if (provenance->schema != XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION ||
        provenance->program_schema != XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION ||
        provenance->type_count != 2 || provenance->type_field_count != 2 ||
        provenance->function_count != 2 || provenance->call_count != 1 ||
        provenance->module_count != 1 || provenance->dependency_count != 0 ||
        provenance->program_module_row != 0 ||
        provenance->program_dependency_binding_count != 0 ||
        provenance->reserved != 0 ||
        provenance->type_count != xr_semantic_plan_program_type_binding_count(plan) ||
        provenance->type_field_count != xr_semantic_plan_program_type_field_binding_count(plan) ||
        provenance->function_count != xr_semantic_plan_program_function_binding_count(plan) ||
        provenance->call_count != xr_semantic_plan_program_call_binding_count(plan))
        return NULL;
    return provenance;
}

static XrTargetLeafProgramTypeKind
semantic_leaf_program_type_kind(const XrSemanticPlan *plan, uint32_t semantic_type,
                                const XrSemanticProgramTypeBinding **out_binding) {
    if (out_binding)
        *out_binding = NULL;
    const XrSemanticProgramProvenance *published = xr_semantic_plan_program_provenance(plan);
    const XrSemanticProgramProvenance *provenance = semantic_leaf_program_provenance(plan);
    if (published &&
        published->program_family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL &&
        !provenance)
        return XR_TARGET_LEAF_PROGRAM_TYPE_INVALID;
    if (!provenance)
        return XR_TARGET_LEAF_PROGRAM_TYPE_NOT_APPLICABLE;
    const XrSemanticProgramTypeBinding *binding =
        xr_semantic_plan_program_type_for_semantic_type(plan, semantic_type);
    if (!binding)
        return XR_TARGET_LEAF_PROGRAM_TYPE_NOT_APPLICABLE;
    const XrSemanticProgramTypeBinding *row =
        xr_semantic_plan_program_type_for_row(plan, binding->program_row);
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, semantic_type);
    const uint8_t required = XR_PROGRAM_SEMANTIC_TYPE_NONNULLABLE |
                             XR_PROGRAM_SEMANTIC_TYPE_NONGENERIC | XR_PROGRAM_SEMANTIC_TYPE_VALUE |
                             XR_PROGRAM_SEMANTIC_TYPE_POINTER_FREE;
    if (!type || row != binding || binding->program_row >= provenance->type_count ||
        stable_id_is_zero(binding->program_type) || binding->flags != required ||
        binding->reserved != 0 || binding->field_begin > provenance->type_field_count ||
        binding->field_count > provenance->type_field_count - binding->field_begin)
        return XR_TARGET_LEAF_PROGRAM_TYPE_INVALID;
    if (binding->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR) {
        uint16_t machine_rep = XR_MACHINE_REP_COUNT;
        if (binding->exact_scalar == 0 || binding->field_count != 0 ||
            !stable_id_is_zero(binding->source_class_identity) ||
            classify_scalar_type(type, &machine_rep) != XR_TARGET_SCALAR_VALUE ||
            machine_rep == XR_MACHINE_REP_VOID)
            return XR_TARGET_LEAF_PROGRAM_TYPE_INVALID;
        if (out_binding)
            *out_binding = binding;
        return XR_TARGET_LEAF_PROGRAM_TYPE_SCALAR;
    }
    if (binding->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE ||
        binding->exact_scalar != 0 || binding->field_count == 0 ||
        stable_id_is_zero(binding->source_class_identity) || type->kind != XR_KIND_INSTANCE ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->builtin_type != XR_TID_NULL ||
        type->child_count != binding->field_count ||
        type->aggregate_extent != binding->field_count || type->aggregate_align != 0 ||
        type->flags != (XR_SEM_TYPE_VALUE | XR_SEM_TYPE_AGGREGATE_EXACT) ||
        !xr_stable_id_equal(binding->source_class_identity, type->source_class_identity))
        return XR_TARGET_LEAF_PROGRAM_TYPE_INVALID;
    if (out_binding)
        *out_binding = binding;
    return XR_TARGET_LEAF_PROGRAM_TYPE_AGGREGATE;
}

static bool semantic_leaf_program_field_type(const XrSemanticPlan *plan,
                                             const XrSemanticProgramTypeBinding *owner,
                                             uint32_t ordinal, uint32_t *out_semantic_type) {
    if (out_semantic_type)
        *out_semantic_type = XR_SEMANTIC_INDEX_NONE;
    if (!plan || !owner || !out_semantic_type || ordinal >= owner->field_count)
        return false;
    const XrSemanticProgramTypeFieldBinding *field =
        xr_semantic_plan_program_type_field_binding(plan, owner->field_begin + ordinal);
    const XrSemanticProgramTypeBinding *child =
        field ? xr_semantic_plan_program_type_for_row(plan, field->field_program_row) : NULL;
    const XrSemanticProgramTypeBinding *semantic_child =
        field ? xr_semantic_plan_program_type_for_semantic_type(plan, field->semantic_field_type)
              : NULL;
    if (!field || !child || semantic_child != child ||
        field->owner_program_row != owner->program_row || field->declaration_ordinal != ordinal ||
        !xr_stable_id_equal(field->program_owner_type, owner->program_type) ||
        !xr_stable_id_equal(field->program_field_type, child->program_type) ||
        child->kind != XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR || child->field_count != 0 ||
        semantic_leaf_program_type_kind(plan, child->semantic_type, NULL) !=
            XR_TARGET_LEAF_PROGRAM_TYPE_SCALAR)
        return false;
    *out_semantic_type = child->semantic_type;
    return true;
}

/* Join one frozen program call binding to the exact SemanticPlan skeleton. No
 * source name, analyzer node, or Xi pointer participates: stable function/call
 * identities select rows, and the typed aggregate binding selects the value
 * representation. */
static bool
semantic_leaf_program_direct_call_is_exact(const XrSemanticPlan *plan, uint32_t operation_index,
                                           const XrSemanticOperationRecord *operation,
                                           const XrSemanticFunctionRecord *callee,
                                           const XrSemanticProgramTypeBinding **out_aggregate) {
    if (out_aggregate)
        *out_aggregate = NULL;
    const XrSemanticProgramProvenance *provenance = semantic_leaf_program_provenance(plan);
    const XrSemanticProgramCallBinding *call =
        provenance ? xr_semantic_plan_program_call_for_operation(plan, operation_index) : NULL;
    const XrSemanticProgramFunctionBinding *caller_binding =
        operation
            ? xr_semantic_plan_program_function_for_semantic_function(plan, operation->function)
            : NULL;
    const XrSemanticProgramFunctionBinding *callee_binding =
        call ? xr_semantic_plan_program_function_for_semantic_function(plan, call->target_function)
             : NULL;
    const XrSemanticFunctionRecord *caller =
        operation ? xr_semantic_plan_function(plan, operation->function) : NULL;
    const XrSemanticFunctionRecord *bound_callee =
        call ? xr_semantic_plan_function(plan, call->target_function) : NULL;
    const XrSemanticProgramTypeBinding *aggregate = NULL;
    if (!provenance || !call || !operation || !caller_binding || !callee_binding || !caller ||
        !bound_callee || bound_callee != callee || call->program_row != 0 || call->reserved != 0 ||
        stable_id_is_zero(call->program_call) || stable_id_is_zero(call->callsite) ||
        !xr_stable_id_equal(call->caller_program_function, caller_binding->program_function) ||
        !xr_stable_id_equal(call->callee_program_function, callee_binding->program_function) ||
        (caller_binding->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) == 0 ||
        callee_binding->flags != 0 ||
        operation->opcode != XI_CALL || operation->function == call->target_function ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->result_type != bound_callee->return_type ||
        caller->return_type != operation->result_type || caller->parameter_count != 0 ||
        bound_callee->parameter_count != 1 ||
        semantic_leaf_program_type_kind(plan, operation->result_type, &aggregate) !=
            XR_TARGET_LEAF_PROGRAM_TYPE_AGGREGATE)
        return false;
    const XrSemanticParameterRecord *parameter =
        xr_semantic_plan_parameter(plan, bound_callee->parameter_begin);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!parameter || !operands || operation->operand_count != 2 ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin)
        return false;
    const XrSemanticOperandRecord *callee_operand = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *argument = callee_operand + 1;
    if (parameter->function != call->target_function || parameter->ordinal != 0 ||
        parameter->type != operation->result_type || parameter->mode != XR_PARAM_READ ||
        parameter->ownership != XI_OWN_NONE || parameter->transfer_mode != XR_TRANSFER_SHARE ||
        parameter->flags != XR_SEM_PARAMETER_REQUIRED || parameter->reserved != 0 ||
        callee_operand->role != XR_SEM_OPERAND_CALLEE || callee_operand->parameter != -1 ||
        callee_operand->flags != 0 || argument->role != XR_SEM_OPERAND_ARGUMENT ||
        argument->parameter != 0 || argument->type != parameter->type ||
        argument->parameter_mode != XR_PARAM_READ ||
        argument->ownership_action != XR_SEM_OPERAND_BORROW ||
        argument->transfer_mode != XR_TRANSFER_SHARE || argument->access != XR_CALL_ARG_PLAIN ||
        argument->origin != XI_PLACE_ORIGIN_NONE || argument->lifetime != XI_PLACE_LIFETIME_NONE ||
        argument->escape != XI_PLACE_ESCAPE_NONE ||
        argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->return_provenance != XR_SEM_RETURN_NONE || operation->return_complete != 0 ||
        bound_callee->return_parameter != -1 ||
        bound_callee->return_provenance != XR_SEM_RETURN_NONE)
        return false;
    if (out_aggregate)
        *out_aggregate = aggregate;
    return true;
}

static const XrSemanticProgramProvenance *
semantic_product_program_provenance(const XrSemanticPlan *plan) {
    const XrSemanticProgramProvenance *provenance =
        xr_semantic_plan_program_provenance(plan);
    return provenance &&
                   provenance->program_family ==
                       XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL &&
                   provenance->schema == XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION &&
                   provenance->program_schema == XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION &&
                   provenance->type_count == 3 && provenance->type_field_count == 6 &&
                   provenance->function_count == 3 && provenance->call_count == 2 &&
                   provenance->module_count == 1 && provenance->dependency_count == 0 &&
                   provenance->program_module_row == 0 &&
                   provenance->program_dependency_binding_count == 0 &&
                   xr_semantic_plan_program_type_binding_count(plan) == 3 &&
                   xr_semantic_plan_program_type_field_binding_count(plan) == 6 &&
                   xr_semantic_plan_program_function_binding_count(plan) == 3 &&
                   xr_semantic_plan_program_call_binding_count(plan) == 2
               ? provenance
               : NULL;
}

static bool semantic_product_direct_call_is_exact(
    const XrSemanticPlan *plan, uint32_t operation_index,
    const XrSemanticOperationRecord *operation,
    const XrSemanticFunctionRecord *callee,
    const XrSemanticProgramTypeBinding **out_product) {
    if (out_product)
        *out_product = NULL;
    const XrSemanticProgramProvenance *provenance =
        semantic_product_program_provenance(plan);
    const XrSemanticProgramCallBinding *call =
        provenance ? xr_semantic_plan_program_call_for_operation(plan, operation_index) : NULL;
    const XrSemanticProgramFunctionBinding *caller_binding =
        operation ? xr_semantic_plan_program_function_for_semantic_function(
                        plan, operation->function)
                  : NULL;
    const XrSemanticProgramFunctionBinding *callee_binding =
        call ? xr_semantic_plan_program_function_for_semantic_function(plan,
                                                                       call->target_function)
             : NULL;
    const XrSemanticFunctionRecord *caller =
        operation ? xr_semantic_plan_function(plan, operation->function) : NULL;
    const XrSemanticFunctionRecord *bound_callee =
        call ? xr_semantic_plan_function(plan, call->target_function) : NULL;
    const XrSemanticProgramTypeBinding *product =
        operation ? xr_semantic_plan_program_type_for_semantic_type(
                        plan, operation->result_type)
                  : NULL;
    const XrSemanticTypeRecord *product_type =
        product ? xr_semantic_plan_type(plan, product->semantic_type) : NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticOperandRecord *callee_operand =
        operation && operation->operand_count == 1 &&
                operation->operand_begin < operand_count
            ? &operands[operation->operand_begin]
            : NULL;
    if (!provenance || !call || !operation || !caller_binding || !callee_binding || !caller ||
        !bound_callee || bound_callee != callee || !product || !product_type || !callee_operand ||
        call->program_row >= 2 || call->reserved != 0 ||
        call->target_function == operation->function ||
        !xr_stable_id_equal(call->caller_program_function,
                            caller_binding->program_function) ||
        !xr_stable_id_equal(call->callee_program_function,
                            callee_binding->program_function) ||
        caller_binding->flags != XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY ||
        callee_binding->flags != 0 || operation->opcode != XI_CALL ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->result_type != bound_callee->return_type ||
        caller->return_type != operation->result_type || caller->parameter_count != 0 ||
        bound_callee->parameter_count != 0 ||
        product->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT ||
        product->exact_scalar != XR_EXACT_SCALAR_NONE || product->field_count != 6 ||
        product_type->kind != XR_KIND_TUPLE || product_type->child_count != 6 ||
        product_type->aggregate_extent != 6 || product_type->flags != XR_SEM_TYPE_VALUE ||
        !stable_id_is_zero(product->source_class_identity) ||
        callee_operand->role != XR_SEM_OPERAND_CALLEE || callee_operand->parameter != -1 ||
        callee_operand->flags != 0 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->return_provenance != XR_SEM_RETURN_NONE ||
        bound_callee->return_parameter != -1 ||
        bound_callee->return_provenance != XR_SEM_RETURN_NONE)
        return false;
    if (out_product)
        *out_product = product;
    return true;
}

static bool semantic_channel_type_is_exact(const XrSemanticPlan *plan, uint32_t type_index,
                                           uint32_t *element_type) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    uint8_t allowed = required | XR_SEM_TYPE_CONST;
    if (!type || type->kind != XR_KIND_CHANNEL || type->scalar_rep != XR_SCALAR_REP_NONE ||
        type->child_count != 1 || type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        (type->flags & required) != required || (type->flags & ~allowed) != 0 ||
        type->child_begin >= child_count ||
        children[type->child_begin] >= xr_semantic_plan_type_count(plan))
        return false;
    if (element_type)
        *element_type = children[type->child_begin];
    return true;
}

static bool semantic_channel_capacity_type_is_exact(const XrSemanticPlan *plan,
                                                    uint32_t type_index) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    uint16_t kind = XR_MACHINE_REP_COUNT;
    if (!type || type->kind != XR_KIND_INT ||
        classify_scalar_type(type, &kind) != XR_TARGET_SCALAR_VALUE)
        return false;
    return kind >= XR_MACHINE_REP_I8 && kind <= XR_MACHINE_REP_USIZE;
}

static bool semantic_channel_allocation_is_exact(const XrSemanticPlan *plan,
                                                 const XrSemanticOperationRecord *operation) {
    if (!plan || !operation)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    uint32_t element_type = XR_SEMANTIC_INDEX_NONE;
    if (operation->opcode != XI_CHAN_NEW || operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        !xr_semantic_allocation_identity_is_canonical(operation) ||
        !semantic_channel_type_is_exact(plan, operation->result_type, &element_type) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE || operation->auxiliary_kind != 0 ||
        operation->effects != xi_generated_op_effects(XI_CHAN_NEW) ||
        operation->flags != xi_generated_op_default_flags(XI_CHAN_NEW) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_CHAN_NEW) ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1)
        return false;
    const XrSemanticOperandRecord *capacity = &operands[operation->operand_begin];
    return capacity->value != XR_SEMANTIC_INDEX_NONE &&
           capacity->type < xr_semantic_plan_type_count(plan) &&
           capacity->role == XR_SEM_OPERAND_VALUE && capacity->parameter == -1 &&
           capacity->flags == 0 && semantic_channel_capacity_type_is_exact(plan, capacity->type) &&
           element_type < xr_semantic_plan_type_count(plan);
}

static bool semantic_channel_identity_copy_is_exact(const XrSemanticPlan *plan,
                                                    const XrSemanticOperationRecord *operation,
                                                    const uint8_t *exact_channel_values,
                                                    uint32_t value_count) {
    if (!plan || !operation || !exact_channel_values)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (operation->opcode != XI_COPY || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count ||
        operation->semantic_immediate != XI_COPY_KIND_IDENTITY ||
        operation->allocation_key != NULL || !stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->effects != xi_generated_op_effects(XI_COPY) ||
        operation->flags != xi_generated_op_default_flags(XI_COPY) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_alias_operand != 0 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1)
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    uint32_t source_element = XR_SEMANTIC_INDEX_NONE;
    uint32_t result_element = XR_SEMANTIC_INDEX_NONE;
    return source->value < value_count && exact_channel_values[source->value] &&
           source->role == XR_SEM_OPERAND_VALUE && source->parameter == -1 && source->flags == 0 &&
           semantic_channel_type_is_exact(plan, source->type, &source_element) &&
           semantic_channel_type_is_exact(plan, operation->result_type, &result_element) &&
           source_element == result_element;
}

/* CHANNEL_RECEIVE_STORAGE owns the boundary between the runtime's tagged
 * receive payload and the exact machine slot selected for Channel<T>'s T.
 * This predicate deliberately accepts only channels already proven by the
 * channel-allocation family; a nominal Channel type is not allocation or
 * lifetime authority. */
static bool semantic_channel_receive_storage_is_exact(const XrSemanticPlan *plan,
                                                      const XrSemanticOperationRecord *operation,
                                                      const uint8_t *exact_channel_values,
                                                      uint32_t value_count) {
    if (!plan || !operation || !exact_channel_values || operation->opcode != XI_CHAN_TRY_RECV ||
        operation->operand_count != 1 || operation->result_value >= value_count ||
        operation->allocation_key || !stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE || operation->auxiliary_kind != 0 ||
        operation->semantic_immediate != 0 ||
        operation->effects != xi_generated_op_effects(XI_CHAN_TRY_RECV) ||
        operation->flags != xi_generated_op_default_flags(XI_CHAN_TRY_RECV) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CHAN_TRY_RECV) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_CHAN_TRY_RECV) ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (operation->operand_begin >= operand_count)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    uint32_t element_type = XR_SEMANTIC_INDEX_NONE;
    uint16_t result_kind = XR_MACHINE_REP_COUNT;
    return receiver->value < value_count && exact_channel_values[receiver->value] != 0 &&
           receiver->role == XR_SEM_OPERAND_VALUE && receiver->parameter == -1 &&
           receiver->transfer_mode == XR_TRANSFER_SHARE &&
           receiver->ownership_action == XR_SEM_OPERAND_BORROW &&
           receiver->parameter_mode == XR_PARAM_READ && receiver->access == XR_CALL_ARG_PLAIN &&
           receiver->origin == XI_PLACE_ORIGIN_NONE &&
           receiver->lifetime == XI_PLACE_LIFETIME_NONE &&
           receiver->escape == XI_PLACE_ESCAPE_NONE && receiver->flags == 0 &&
           semantic_channel_type_is_exact(plan, receiver->type, &element_type) &&
           element_type == operation->result_type &&
           classify_scalar_type(xr_semantic_plan_type(plan, operation->result_type),
                                &result_kind) == XR_TARGET_SCALAR_VALUE &&
           result_kind != XR_MACHINE_REP_VOID;
}

static void
direct_local_callee_storage_analysis_dispose(XrDirectLocalCalleeStorageAnalysis *analysis) {
    if (!analysis)
        return;
    xr_free(analysis->target_by_operation);
    xr_free(analysis->target_by_value);
    xr_free(analysis->use_count_by_value);
    xr_free(analysis->invalid_value);
    memset(analysis, 0, sizeof(*analysis));
}

static bool direct_local_callee_storage_analysis_init(const XrSemanticPlan *plan,
                                                      const XrTargetValueStorageAnalysis *values,
                                                      XrDirectLocalCalleeStorageAnalysis *analysis,
                                                      char *error, size_t error_size) {
    if (!plan || !values || !analysis || xr_semantic_plan_operation_count(plan) > UINT32_MAX ||
        xr_semantic_plan_call_target_count(plan) > UINT32_MAX)
        return fail(error, error_size, "XR_EXEC_5003",
                    "direct-local callee-storage budget is invalid");
    analysis->operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    analysis->value_count = values->total_values;
    analysis->target_by_operation = (uint32_t *) allocate_records(
        analysis->operation_count, sizeof(*analysis->target_by_operation));
    analysis->target_by_value =
        (uint32_t *) allocate_records(analysis->value_count, sizeof(*analysis->target_by_value));
    analysis->use_count_by_value =
        (uint32_t *) allocate_records(analysis->value_count, sizeof(*analysis->use_count_by_value));
    analysis->invalid_value =
        (uint8_t *) allocate_records(analysis->value_count, sizeof(*analysis->invalid_value));
    if ((analysis->operation_count && !analysis->target_by_operation) ||
        (analysis->value_count && (!analysis->target_by_value || !analysis->use_count_by_value ||
                                   !analysis->invalid_value))) {
        direct_local_callee_storage_analysis_dispose(analysis);
        return fail(error, error_size, "XR_EXEC_5003",
                    "direct-local callee-storage allocation failed");
    }
    for (uint32_t i = 0; i < analysis->operation_count; i++)
        analysis->target_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < analysis->value_count; i++)
        analysis->target_by_value[i] = XR_SEMANTIC_INDEX_NONE;
    uint32_t target_count = (uint32_t) xr_semantic_plan_call_target_count(plan);
    for (uint32_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(plan, i);
        if (target && target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL)
            continue;
        if (!target || target->operation >= analysis->operation_count ||
            analysis->target_by_operation[target->operation] != XR_SEMANTIC_INDEX_NONE) {
            direct_local_callee_storage_analysis_dispose(analysis);
            return fail(error, error_size, "XR_TARGET_1001",
                        "direct-local callee target authority is ambiguous");
        }
        analysis->target_by_operation[target->operation] = i;
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    for (uint32_t i = 0; i < analysis->operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(plan, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin) {
            direct_local_callee_storage_analysis_dispose(analysis);
            return fail(error, error_size, "XR_TARGET_1001",
                        "direct-local callee operand range is invalid");
        }
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand = &operands[use->operand_begin + a];
            if (operand->value >= analysis->value_count)
                continue;
            uint32_t source_index = values->value_operations[operand->value];
            const XrSemanticOperationRecord *source =
                source_index == XR_SEMANTIC_INDEX_NONE
                    ? NULL
                    : xr_semantic_plan_operation(plan, source_index);
            if (!source || source->opcode != XI_GET_SHARED)
                continue;
            uint32_t target_index = analysis->target_by_operation[i];
            const XrSemanticCallTargetRecord *target =
                target_index == XR_SEMANTIC_INDEX_NONE
                    ? NULL
                    : xr_semantic_plan_call_target(plan, target_index);
            bool exact = a == 0 && (use->opcode == XI_CALL || use->opcode == XI_TAIL_CALL) &&
                         use->function == source->function && target && target->operation == i &&
                         target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
                         operand->role == XR_SEM_OPERAND_CALLEE && operand->parameter == -1 &&
                         (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0 &&
                         operand->type == source->result_type;
            uint32_t value = source->result_value;
            if (!exact || value != operand->value ||
                (analysis->target_by_value[value] != XR_SEMANTIC_INDEX_NONE &&
                 analysis->target_by_value[value] != target->function) ||
                analysis->use_count_by_value[value] == UINT32_MAX) {
                analysis->invalid_value[value] = 1;
                continue;
            }
            analysis->target_by_value[value] = target->function;
            analysis->use_count_by_value[value]++;
        }
    }
    uint32_t block_count = (uint32_t) xr_semantic_plan_block_count(plan);
    for (uint32_t i = 0; i < block_count; i++) {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(plan, i);
        if (!block || block->control_value == XR_SEMANTIC_INDEX_NONE ||
            block->control_value >= analysis->value_count)
            continue;
        uint32_t source_index = values->value_operations[block->control_value];
        const XrSemanticOperationRecord *source =
            source_index == XR_SEMANTIC_INDEX_NONE ? NULL
                                                   : xr_semantic_plan_operation(plan, source_index);
        if (source && source->opcode == XI_GET_SHARED)
            analysis->invalid_value[block->control_value] = 1;
    }
    return true;
}

static bool
direct_local_callee_storage_value_is_exact(const XrSemanticPlan *plan,
                                           const XrDirectLocalCalleeStorageAnalysis *analysis,
                                           const XrSemanticOperationRecord *operation) {
    if (!plan || !analysis || !operation || operation->result_value >= analysis->value_count ||
        analysis->invalid_value[operation->result_value] != 0 ||
        analysis->use_count_by_value[operation->result_value] == 0 ||
        analysis->target_by_value[operation->result_value] == XR_SEMANTIC_INDEX_NONE)
        return false;
    return xr_semantic_direct_local_callee_type_is_exact(
        plan, operation, analysis->target_by_value[operation->result_value]);
}

static void
direct_local_go_callee_storage_analysis_dispose(XrDirectLocalGoCalleeStorageAnalysis *analysis) {
    if (!analysis)
        return;
    xr_free(analysis->stores);
    xr_free(analysis->target_by_value);
    xr_free(analysis->use_count_by_value);
    xr_free(analysis->candidate_value);
    xr_free(analysis->invalid_value);
    xr_semantic_graph_dispose(&analysis->graph);
    memset(analysis, 0, sizeof(*analysis));
}

static XrDirectLocalGoStoreEntry *
direct_local_go_find_store(XrDirectLocalGoCalleeStorageAnalysis *analysis, int64_t slot) {
    if (!analysis || !analysis->stores || slot < 0 || slot >= (int64_t) analysis->slot_count)
        return NULL;
    return &analysis->stores[slot];
}

/* Whether the store that fills the callee's slot is guaranteed to have run
 * before any read of that slot in the same function.
 *
 * Dominance is the whole judgement. Two earlier spellings of it were structural
 * and both were wrong in the same direction -- they described one shape a
 * correct program can take rather than the property that makes it correct.
 * Requiring the store to sit in the entry block refused a module whose function
 * declarations lower into a later block, which is most of them; requiring no
 * call before the store refused `const c = Channel<int>(1)` written above a
 * declaration, on a hazard the module-wide ambiguity check has already ruled
 * out -- a slot written exactly once cannot be changed by any call.
 *
 * Reads in other functions are not checked here and need not be: the callee is
 * lexically owned by this function and the slot is written once, so every path
 * that reaches such a read passes through a read in this function first, and
 * that one is dominated. */
static bool direct_local_go_store_dominates_slot_reads(const XrSemanticPlan *plan,
                                                       const XrSemanticGraph *graph, int64_t slot,
                                                       uint32_t store_index) {
    const XrSemanticOperationRecord *store = xr_semantic_plan_operation(plan, store_index);
    if (!plan || !graph || !store)
        return false;
    uint32_t count = (uint32_t) xr_semantic_plan_operation_count(plan);
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticOperationRecord *read = xr_semantic_plan_operation(plan, i);
        if (!read)
            return false;
        if (read->opcode != XI_GET_SHARED || read->semantic_immediate != slot ||
            read->function != store->function)
            continue;
        if (read->block == store->block
                ? i <= store_index
                : !xr_semantic_graph_dominates(graph, store->block, read->block))
            return false;
    }
    return true;
}

static bool semantic_direct_local_go_store_target(
    const XrSemanticPlan *plan, const XrTargetValueStorageAnalysis *values,
    const XrSemanticOperationRecord *load, uint32_t load_index,
    const XrDirectLocalGoStoreEntry *entry, const XrSemanticGraph *graph, uint32_t *out_target) {
    if (out_target)
        *out_target = XR_SEMANTIC_INDEX_NONE;
    if (!plan || !values || !load || !entry || !out_target || entry->ambiguous ||
        entry->operation == XR_SEMANTIC_INDEX_NONE || entry->operation >= load_index) {
        return false;
    }
    const XrSemanticOperationRecord *store = xr_semantic_plan_operation(plan, entry->operation);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    /* A store the loading function makes itself has to dominate the load. A
     * store in the slot's owning scope carries no dominance relation to a load
     * in a nested function, and needs none: the owner judgement below pins the
     * store to the callee's lexical parent, and the entry-prefix judgement
     * proves it ran before any operation that could activate that nested
     * function at all. */
    bool initialized = store && load &&
                       (store->function != load->function ||
                        (store->block == load->block
                             ? entry->operation < load_index
                             : xr_semantic_graph_dominates(graph, store->block, load->block)));
    if (!store || store->opcode != XI_SET_SHARED || !initialized ||
        store->semantic_immediate != load->semantic_immediate || store->operand_count != 1 ||
        store->operand_begin >= operand_count || store->allocation_key ||
        !stable_id_is_zero(store->allocation_id) || store->constant != XR_SEMANTIC_INDEX_NONE ||
        store->callable_function != XR_SEMANTIC_INDEX_NONE ||
        store->effects != xi_generated_op_effects(XI_SET_SHARED) ||
        store->result_ownership != xi_generated_op_result_ownership(XI_SET_SHARED) ||
        !direct_local_go_store_dominates_slot_reads(plan, graph, store->semantic_immediate,
                                                    entry->operation)) {
        return false;
    }
    const XrSemanticOperandRecord *source = &operands[store->operand_begin];
    if (source->value >= values->total_values || source->role != XR_SEM_OPERAND_VALUE ||
        source->parameter != -1 || source->transfer_mode != XR_TRANSFER_SHARE ||
        source->ownership_action != XR_SEM_OPERAND_CONSUME ||
        source->parameter_mode != XR_PARAM_READ || source->access != XR_CALL_ARG_PLAIN ||
        source->origin != XI_PLACE_ORIGIN_NONE || source->lifetime != XI_PLACE_LIFETIME_NONE ||
        source->escape != XI_PLACE_ESCAPE_NONE || source->flags != 0) {
        return false;
    }
    uint32_t producer_index = values->value_operations[source->value];
    const XrSemanticOperationRecord *producer =
        producer_index == XR_SEMANTIC_INDEX_NONE ? NULL
                                                 : xr_semantic_plan_operation(plan, producer_index);
    const XrSemanticFunctionRecord *callee =
        producer ? xr_semantic_plan_function(plan, producer->callable_function) : NULL;
    if (!producer || producer_index >= entry->operation ||
        producer->result_value != source->value || producer->result_type != source->type ||
        producer->function != store->function || !semantic_heap_closure_is_exact(plan, producer) ||
        !callee || callee->parent != store->function) {
        return false;
    }
    *out_target = producer->callable_function;
    return true;
}

static bool semantic_direct_local_go_use_is_exact(const XrSemanticPlan *plan,
                                                  const XrSemanticOperationRecord *source,
                                                  const XrSemanticOperationRecord *use,
                                                  const XrSemanticOperandRecord *callee,
                                                  uint16_t operand_index,
                                                  uint32_t target_function) {
    const XrSemanticFunctionRecord *target = xr_semantic_plan_function(plan, target_function);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!source || !use || !callee || !target || operand_index != 0 || use->opcode != XI_GO ||
        use->function != source->function ||
        use->operand_count != (uint16_t) (target->parameter_count + 1u) ||
        use->operand_begin > operand_count ||
        use->operand_count > operand_count - use->operand_begin || use->allocation_key ||
        !stable_id_is_zero(use->allocation_id) || use->constant != XR_SEMANTIC_INDEX_NONE ||
        use->callable_function != XR_SEMANTIC_INDEX_NONE ||
        use->effects != xi_generated_op_effects(XI_GO) || callee->value != source->result_value ||
        callee->type != source->result_type || callee->role != XR_SEM_OPERAND_VALUE ||
        callee->parameter != -1 || callee->transfer_mode != XR_TRANSFER_SHARE ||
        callee->ownership_action != XR_SEM_OPERAND_BORROW ||
        callee->parameter_mode != XR_PARAM_READ || callee->access != XR_CALL_ARG_PLAIN ||
        callee->origin != XI_PLACE_ORIGIN_NONE || callee->lifetime != XI_PLACE_LIFETIME_NONE ||
        callee->escape != XI_PLACE_ESCAPE_NONE || callee->flags != 0)
        return false;
    for (uint16_t i = 1; i < use->operand_count; i++) {
        const XrSemanticOperandRecord *argument = &operands[use->operand_begin + i];
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan, target->parameter_begin + i - 1u);
        if (!parameter ||
            !xr_semantic_type_is_const_variant(plan, argument->type, parameter->type) ||
            argument->role != XR_SEM_OPERAND_VALUE || argument->parameter != -1 ||
            argument->parameter_mode != XR_PARAM_READ || argument->access != XR_CALL_ARG_PLAIN ||
            argument->origin != XI_PLACE_ORIGIN_NONE ||
            argument->lifetime != XI_PLACE_LIFETIME_NONE ||
            argument->escape != XI_PLACE_ESCAPE_NONE || argument->flags != 0)
            return false;
    }
    return xr_semantic_direct_local_callee_type_is_exact(plan, source, target_function);
}

static bool direct_local_go_callee_storage_analysis_init(
    const XrSemanticPlan *plan, const XrTargetValueStorageAnalysis *values,
    XrDirectLocalGoCalleeStorageAnalysis *analysis, char *error, size_t error_size) {
    if (!plan || !values || !analysis || xr_semantic_plan_operation_count(plan) > UINT32_MAX)
        return fail(error, error_size, "XR_EXEC_5003",
                    "direct-local go callee-storage budget is invalid");
    analysis->operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    analysis->value_count = values->total_values;
    if (analysis->operation_count > (1u << 24))
        return fail(error, error_size, "XR_EXEC_5003",
                    "direct-local go shared-store budget is exhausted");
    for (uint32_t i = 0; i < analysis->operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(plan, i);
        if (!operation ||
            (operation->opcode != XI_SET_SHARED && operation->opcode != XI_GET_SHARED))
            continue;
        if (operation->semantic_immediate < 0 || operation->semantic_immediate > UINT16_MAX)
            continue;
        uint32_t slot = (uint32_t) operation->semantic_immediate;
        if (slot + 1u > analysis->slot_count)
            analysis->slot_count = slot + 1u;
    }
    analysis->stores = (XrDirectLocalGoStoreEntry *) allocate_records(analysis->slot_count,
                                                                      sizeof(*analysis->stores));
    analysis->target_by_value =
        (uint32_t *) allocate_records(analysis->value_count, sizeof(*analysis->target_by_value));
    analysis->use_count_by_value =
        (uint32_t *) allocate_records(analysis->value_count, sizeof(*analysis->use_count_by_value));
    analysis->candidate_value =
        (uint8_t *) allocate_records(analysis->value_count, sizeof(*analysis->candidate_value));
    analysis->invalid_value =
        (uint8_t *) allocate_records(analysis->value_count, sizeof(*analysis->invalid_value));
    if (!xr_semantic_graph_build(plan, &analysis->graph, error, error_size) ||
        (analysis->slot_count && !analysis->stores) ||
        (analysis->value_count && (!analysis->target_by_value || !analysis->use_count_by_value ||
                                   !analysis->candidate_value || !analysis->invalid_value))) {
        direct_local_go_callee_storage_analysis_dispose(analysis);
        return fail(error, error_size, "XR_EXEC_5003",
                    "direct-local go callee-storage allocation failed");
    }
    for (uint32_t i = 0; i < analysis->value_count; i++)
        analysis->target_by_value[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < analysis->slot_count; i++)
        analysis->stores[i].operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < analysis->operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(plan, i);
        if (!operation)
            goto invalid_authority;
        if (operation->opcode != XI_SET_SHARED || operation->semantic_immediate < 0 ||
            operation->semantic_immediate > UINT16_MAX)
            continue;
        XrDirectLocalGoStoreEntry *entry =
            direct_local_go_find_store(analysis, operation->semantic_immediate);
        if (!entry)
            goto invalid_authority;
        if (entry->operation != XR_SEMANTIC_INDEX_NONE)
            entry->ambiguous = 1;
        else {
            entry->operation = i;
            entry->function = operation->function;
        }
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    for (uint32_t i = 0; i < analysis->operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(plan, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            goto invalid_authority;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand = &operands[use->operand_begin + a];
            if (operand->value >= analysis->value_count)
                goto invalid_authority;
            uint32_t source_index = values->value_operations[operand->value];
            const XrSemanticOperationRecord *source =
                source_index == XR_SEMANTIC_INDEX_NONE
                    ? NULL
                    : xr_semantic_plan_operation(plan, source_index);
            /* Only the spawn's callee operand is a candidate. An argument that
             * happens to be a shared read of its own is not one, and admitting
             * it made this pass contradict itself: it was collected here, no
             * store could be found for it because none was looked for, and the
             * sweep below then invalidated it for appearing in a position this
             * one had just accepted. */
            if (!source || source->opcode != XI_GET_SHARED || use->opcode != XI_GO || a != 0)
                continue;
            uint32_t value = source->result_value;
            analysis->candidate_value[value] = 1;
            uint32_t target = XR_SEMANTIC_INDEX_NONE;
            XrDirectLocalGoStoreEntry *entry =
                direct_local_go_find_store(analysis, source->semantic_immediate);
            bool probe_store =
                entry && semantic_direct_local_go_store_target(plan, values, source, source_index,
                                                               entry, &analysis->graph, &target);
            bool exact = probe_store && semantic_direct_local_go_use_is_exact(plan, source, use,
                                                                              operand, a, target);
            if (!exact ||
                (analysis->target_by_value[value] != XR_SEMANTIC_INDEX_NONE &&
                 analysis->target_by_value[value] != target) ||
                analysis->use_count_by_value[value] == UINT32_MAX) {
                analysis->invalid_value[value] = 1;
                continue;
            }
            analysis->target_by_value[value] = target;
            analysis->use_count_by_value[value]++;
        }
    }
    for (uint32_t i = 0; i < analysis->operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(plan, i);
        for (uint16_t a = 0; use && a < use->operand_count; a++) {
            uint32_t value = operands[use->operand_begin + a].value;
            uint32_t source_index = value < analysis->value_count ? values->value_operations[value]
                                                                  : XR_SEMANTIC_INDEX_NONE;
            const XrSemanticOperationRecord *source =
                source_index == XR_SEMANTIC_INDEX_NONE
                    ? NULL
                    : xr_semantic_plan_operation(plan, source_index);
            if (source && source->opcode == XI_GET_SHARED && analysis->candidate_value[value] &&
                (use->opcode != XI_GO || a != 0))
                analysis->invalid_value[value] = 1;
        }
    }
    for (uint32_t i = 0; i < (uint32_t) xr_semantic_plan_block_count(plan); i++) {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(plan, i);
        if (block && block->control_value < analysis->value_count &&
            analysis->candidate_value[block->control_value])
            analysis->invalid_value[block->control_value] = 1;
    }
    return true;

invalid_authority:
    direct_local_go_callee_storage_analysis_dispose(analysis);
    return fail(error, error_size, "XR_TARGET_1001",
                "direct-local go callee-storage authority is invalid");
}

static bool
direct_local_go_callee_storage_value_is_exact(const XrSemanticPlan *plan,
                                              const XrDirectLocalGoCalleeStorageAnalysis *analysis,
                                              const XrSemanticOperationRecord *operation) {
    return plan && analysis && operation && operation->opcode == XI_GET_SHARED &&
           operation->result_value < analysis->value_count &&
           analysis->candidate_value[operation->result_value] &&
           !analysis->invalid_value[operation->result_value] &&
           analysis->use_count_by_value[operation->result_value] != 0 &&
           analysis->target_by_value[operation->result_value] != XR_SEMANTIC_INDEX_NONE &&
           xr_semantic_direct_local_callee_type_is_exact(
               plan, operation, analysis->target_by_value[operation->result_value]);
}

/* Taking a local's address produces an address, not the value at it.  This
 * family binds every operation result from its semantic type and never asks
 * what the opcode was, so the address came out carrying the pointee's own
 * representation -- right only when the pointee happened to be a pointer
 * already, and wrong for every other local.  The pointee's type still governs
 * the type-wide representation: an `int` local is I64 whether or not something
 * takes its address, so the address is bound without touching that record. */
static bool note_scalar_value_ex(XrTargetPlanBuilder *builder,
                                 XrTargetValueStorageAnalysis *analysis, uint32_t semantic_value,
                                 uint32_t semantic_type, uint32_t semantic_function,
                                 uint32_t semantic_operation, uint8_t role,
                                 XrStableId source_identity, bool result_is_address, char *error,
                                 size_t error_size) {
    if (semantic_value >= analysis->total_values || semantic_type >= analysis->type_count ||
        semantic_function >= xr_semantic_plan_function_count(builder->semantic_plan))
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic scalar value identity is out of range");
    if (analysis->defined_values[semantic_value]) {
        if (analysis->value_types[semantic_value] != semantic_type ||
            analysis->value_functions[semantic_value] != semantic_function)
            return fail(error, error_size, "XR_TARGET_1001",
                        "semantic scalar value identity is ambiguous");
        return true;
    }
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(builder->semantic_plan, semantic_type);
    uint16_t kind = XR_MACHINE_REP_COUNT;
    const XrSemanticOperationRecord *operation =
        semantic_operation < xr_semantic_plan_operation_count(builder->semantic_plan)
            ? xr_semantic_plan_operation(builder->semantic_plan, semantic_operation)
            : NULL;
    bool operation_result_void = xr_semantic_operation_result_void_governs_storage(
        builder->semantic_plan, operation, semantic_value, semantic_type, semantic_function);
    if (operation_result_void &&
        (operation->effects != xi_generated_op_effects(operation->opcode) ||
         operation->result_ownership != xi_generated_op_result_ownership(operation->opcode)))
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic result-void operation contract is inconsistent");
    if (semantic_heap_closure_is_exact(builder->semantic_plan, operation))
        return fail(error, error_size, "XR_TARGET_1001",
                    "closure storage cannot be claimed by the scalar family");
    XrTargetScalarEligibility eligibility =
        operation_result_void ? XR_TARGET_SCALAR_VALUE : classify_scalar_type(type, &kind);
    if (operation_result_void)
        kind = XR_MACHINE_REP_VOID;
    if (eligibility == XR_TARGET_SCALAR_INVALID)
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic scalar type has no exact machine representation");
    analysis->defined_values[semantic_value] = 1;
    analysis->value_types[semantic_value] = semantic_type;
    analysis->value_functions[semantic_value] = semantic_function;
    if (eligibility == XR_TARGET_SCALAR_NOT_APPLICABLE)
        return true;
    if (!operation_result_void) {
        if (analysis->type_rep_kinds[semantic_type] != XR_MACHINE_REP_COUNT &&
            analysis->type_rep_kinds[semantic_type] != kind)
            return fail(error, error_size, "XR_TARGET_1001",
                        "semantic type has conflicting scalar representations");
        analysis->type_rep_kinds[semantic_type] = kind;
    }
    (void) result_is_address;
    XrTargetMachineRepRecord rep;
    if (!(operation_result_void
              ? make_machine_rep(xr_target_profile_machine_facts(builder->profile), kind, &rep)
              : make_scalar_type_rep(builder, semantic_type, kind, &rep)) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize a scalar representation");
    XrTargetValueIntent value = {
        .semantic_value = semantic_value,
        .semantic_function = semantic_function,
        .semantic_type = semantic_type,
        .register_rep = rep,
        .memory_rep = rep,
    };
    if (kind != XR_MACHINE_REP_VOID) {
        XrStableId slot_identity;
        const bool parameter_slot = role == XR_TARGET_SLOT_PARAMETER;
        if ((!parameter_slot &&
             semantic_operation >= xr_semantic_plan_operation_count(builder->semantic_plan)) ||
            !make_slot_identity(builder->semantic_plan, semantic_function, role, source_identity,
                                XR_SEMANTIC_INDEX_NONE, &slot_identity))
            return fail(error, error_size, "XR_TARGET_1001", "scalar slot identity is incomplete");
        XrTargetSlotIntent slot = {
            .identity = slot_identity,
            .function = semantic_function,
            .semantic_value = semantic_value,
            .semantic_operation = parameter_slot ? XR_SEMANTIC_INDEX_NONE : semantic_operation,
            .logical_slot = XR_SEMANTIC_INDEX_NONE,
            .register_rep = rep,
            .memory_rep = rep,
            .role = role,
            .root_kind = rep.root_kind,
            .ownership = rep.ownership,
            .debug_variable = XR_SEMANTIC_INDEX_NONE,
        };
        if (!append_slot_intent(builder, &slot, error, error_size))
            return false;
        value.has_slot = true;
        value.slot_identity = slot_identity;
        if (!analysis->used_types[semantic_type]) {
            analysis->used_types[semantic_type] = 1;
            if (!append_layout_intent(builder, semantic_type, XR_TARGET_LAYOUT_SCALAR, 0, &rep,
                                      error, error_size))
                return false;
        }
    }
    return append_value_intent(builder, &value, error, error_size);
}

static bool note_closure_storage_value(XrTargetPlanBuilder *builder,
                                       XrTargetValueStorageAnalysis *analysis,
                                       uint32_t semantic_operation, char *error,
                                       size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    if (!semantic_heap_closure_is_exact(builder->semantic_plan, operation))
        return fail(error, error_size, "XR_TARGET_1001",
                    "closure-storage family requires exact closure authority");
    if (operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >= xr_semantic_plan_function_count(builder->semantic_plan))
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic closure-storage identity is out of range");
    if (analysis->defined_values[operation->result_value]) {
        if (analysis->value_types[operation->result_value] != operation->result_type ||
            analysis->value_functions[operation->result_value] != operation->function)
            return fail(error, error_size, "XR_TARGET_1001",
                        "semantic closure-storage identity is ambiguous");
        return true;
    }
    if (analysis->type_rep_kinds[operation->result_type] != XR_MACHINE_REP_COUNT &&
        analysis->type_rep_kinds[operation->result_type] != XR_MACHINE_REP_DYN_VALUE)
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic closure type has conflicting storage representations");
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize exact dynamic closure storage");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "closure-storage slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = rep.root_kind,
        .ownership = rep.ownership,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[operation->result_type] &&
         !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                               error, error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool semantic_u8_slice_type_is_exact(const XrSemanticPlan *plan, uint32_t type_index);

static bool semantic_string_byte_slice_view_is_exact(const XrSemanticPlan *plan,
                                                     const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    if (!plan || !operation || operation->opcode != XI_CALL_BUILTIN ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_STRING_BYTE_SLICE_VIEW ||
        operation->evidence[1] != XA_INTRINSIC_STRING_BYTE_SLICE_VIEW ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->view_source_operand != 0 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_RECEIVER || operation->view_capability != 1 ||
        operation->view_lifetime != 1 || operation->view_complete != 1 ||
        operation->view_element_type >= xr_semantic_plan_type_count(plan))
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *source_type = xr_semantic_plan_type(plan, source->type);
    const XrSemanticTypeRecord *result_type = xr_semantic_plan_type(plan, operation->result_type);
    const XrSemanticTypeRecord *element_type =
        xr_semantic_plan_type(plan, operation->view_element_type);
    return source->value == operation->view_source_value && source->parameter == 0 &&
           source->role == XR_SEM_OPERAND_ARGUMENT &&
           (source->flags & XR_SEM_OPERAND_CALL_CONTRACT) != 0 && source_type &&
           source_type->kind == XR_KIND_STRING && source_type->scalar_rep == XR_SCALAR_REP_NONE &&
           /* The row itself is judged by the one function that answers this
            * question, so the carrier a view produces here can never be one a
            * use site refuses. Only the tie between the row's element and the
            * operation's own record is checked separately. */
           semantic_u8_slice_type_is_exact(plan, operation->result_type) && result_type &&
           result_type->child_begin < child_count &&
           children[result_type->child_begin] == operation->view_element_type &&
           element_type != NULL;
}

static bool semantic_u8_slice_type_is_exact(const XrSemanticPlan *plan, uint32_t type_index) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    const uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW;
    const uint8_t allowed = required | XR_SEM_TYPE_CONST;
    if (!type || type->kind != XR_KIND_SLICE || type->builtin_type != XR_TID_NULL ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->aggregate_extent != 0 ||
        type->aggregate_align != 0 || type->child_count != 1 || type->child_begin >= child_count ||
        (type->flags & required) != required || (type->flags & ~allowed) != 0)
        return false;
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(plan, children[type->child_begin]);
    return element && element->kind == XR_KIND_INT && element->builtin_type == XR_TID_NULL &&
           element->scalar_rep == XR_NATIVE_U8 && element->flags == 0 &&
           element->child_count == 0 && element->aggregate_extent == 0 &&
           element->aggregate_align == 0;
}

static bool semantic_u8_slice_parameter_is_exact(const XrSemanticPlan *plan,
                                                 const XrSemanticParameterRecord *parameter) {
    return parameter && parameter->function < xr_semantic_plan_function_count(plan) &&
           parameter->value != XR_SEMANTIC_INDEX_NONE && parameter->mode == XR_PARAM_READ &&
           parameter->ownership == XI_OWN_BORROWED &&
           parameter->transfer_mode == XR_TRANSFER_SHARE &&
           (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) == 0 && parameter->reserved == 0 &&
           semantic_u8_slice_type_is_exact(plan, parameter->type);
}

static bool note_u8_slice_view_parameter_storage_value(XrTargetPlanBuilder *builder,
                                                       XrTargetValueStorageAnalysis *analysis,
                                                       const XrSemanticParameterRecord *parameter,
                                                       char *error, size_t error_size) {
    if (!semantic_u8_slice_parameter_is_exact(builder->semantic_plan, parameter) ||
        parameter->value >= analysis->total_values || parameter->type >= analysis->type_count)
        return fail(error, error_size, "XR_TARGET_1001",
                    "byte-slice view parameter storage requires exact authority");
    if (analysis->defined_values[parameter->value])
        return fail(error, error_size, "XR_TARGET_1001",
                    "byte-slice view parameter storage is duplicated");
    XrTargetMachineRepRecord rep;
    if (!make_string_byte_slice_view_rep(xr_target_profile_machine_facts(builder->profile), &rep))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize byte-slice parameter ABI");
    rep.detail = parameter->type;
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, parameter->function, XR_TARGET_SLOT_PARAMETER,
                            parameter->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "byte-slice parameter slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = parameter->function,
        .semantic_value = parameter->value,
        .semantic_operation = XR_SEMANTIC_INDEX_NONE,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_PARAMETER,
        .root_kind = rep.root_kind,
        .ownership = rep.ownership,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = parameter->value,
        .semantic_function = parameter->function,
        .semantic_type = parameter->type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    analysis->defined_values[parameter->value] = 1;
    analysis->value_types[parameter->value] = parameter->type;
    analysis->value_functions[parameter->value] = parameter->function;
    return append_rep_intent(builder, &rep, error, error_size) &&
           append_slot_intent(builder, &slot, error, error_size) &&
           append_layout_intent(builder, parameter->type, XR_TARGET_LAYOUT_VIEW, 0, &rep, error,
                                error_size) &&
           append_value_intent(builder, &value, error, error_size);
}

static bool note_string_byte_slice_view_storage_value(XrTargetPlanBuilder *builder,
                                                      XrTargetValueStorageAnalysis *analysis,
                                                      uint32_t semantic_operation, char *error,
                                                      size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    if (!semantic_string_byte_slice_view_is_exact(builder->semantic_plan, operation) ||
        operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count)
        return fail(error, error_size, "XR_TARGET_1001",
                    "string byte-slice view storage requires exact authority");
    if (analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1001",
                    "string byte-slice view storage is duplicated");
    XrTargetMachineRepRecord rep;
    if (!make_string_byte_slice_view_rep(xr_target_profile_machine_facts(builder->profile), &rep))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize string byte-slice view ABI");
    rep.detail = operation->result_type;
    if (!append_rep_intent(builder, &rep, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001", "view slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = rep.root_kind,
        .ownership = rep.ownership,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    return append_slot_intent(builder, &slot, error, error_size) &&
           append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_VIEW, 0, &rep,
                                error, error_size) &&
           append_value_intent(builder, &value, error, error_size);
}

/* The storage answer for a heap literal, written once for the two families
 * that reach it. Each caller has already established which literal it holds,
 * and reports its own family by name on refusal; what remains is the same for
 * both -- a dynamic value carrier, a temporary slot, a dynamic layout --
 * because a heap constant is a heap constant however its payload is spelled. */
static bool note_heap_literal_storage_value(XrTargetPlanBuilder *builder,
                                            XrTargetValueStorageAnalysis *analysis,
                                            uint32_t semantic_operation, char *error,
                                            size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    if (!operation)
        return fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
    if (operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >= xr_semantic_plan_function_count(builder->semantic_plan))
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic heap-literal identity is out of range");
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize exact heap literal storage");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "heap literal slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = rep.root_kind,
        .ownership = rep.ownership,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[operation->result_type] &&
         !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                               error, error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_string_literal_storage_value(XrTargetPlanBuilder *builder,
                                              XrTargetValueStorageAnalysis *analysis,
                                              uint32_t semantic_operation, char *error,
                                              size_t error_size) {
    if (!xr_semantic_string_literal_is_exact(
            builder->semantic_plan,
            xr_semantic_plan_operation(builder->semantic_plan, semantic_operation)))
        return fail(error, error_size, "XR_TARGET_1001",
                    "string-literal-storage family requires exact literal authority");
    return note_heap_literal_storage_value(builder, analysis, semantic_operation, error,
                                           error_size);
}

static bool note_bigint_value_storage_value(XrTargetPlanBuilder *builder,
                                            XrTargetValueStorageAnalysis *analysis,
                                            uint32_t semantic_operation, char *error,
                                            size_t error_size) {
    if (!xr_semantic_bigint_value_is_exact(
            builder->semantic_plan,
            xr_semantic_plan_operation(builder->semantic_plan, semantic_operation)))
        return fail(error, error_size, "XR_TARGET_1001",
                    "bigint-value-storage family requires exact BigInt authority");
    return note_heap_literal_storage_value(builder, analysis, semantic_operation, error,
                                           error_size);
}

static bool note_stringbuilder_constructor_storage_value(XrTargetPlanBuilder *builder,
                                                         XrTargetValueStorageAnalysis *analysis,
                                                         uint32_t semantic_operation, char *error,
                                                         size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    if (!semantic_stringbuilder_constructor_is_exact(builder->semantic_plan, operation) ||
        operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >= xr_semantic_plan_function_count(builder->semantic_plan) ||
        analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder constructor result authority is incomplete");
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1003",
                    "target profile cannot materialize StringBuilder constructor result");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder constructor result slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_OWNED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    /* A type being mentioned by an earlier value does not prove that a layout
     * owner published its dynamic row. The registry deduplicates an exact row
     * and refuses any conflicting geometry. */
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                              error, error_size) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_stringbuilder_append_rune_storage_value(XrTargetPlanBuilder *builder,
                                                         XrTargetValueStorageAnalysis *analysis,
                                                         uint32_t semantic_operation, char *error,
                                                         size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!semantic_stringbuilder_append_rune_is_exact(builder->semantic_plan, operation, &receiver,
                                                     NULL) ||
        operation->result_value >= analysis->total_values ||
        operation->function >= xr_semantic_plan_function_count(builder->semantic_plan) ||
        receiver >= analysis->total_values || analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.append(rune) result authority is incomplete");
    XrTargetMachineRepRecord rep;
    bool borrowed = operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED;
    if (!make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep))
        return fail(error, error_size, "XR_TARGET_1003",
                    "target profile cannot materialize StringBuilder.append(rune) result");
    rep.ownership = borrowed ? XR_TARGET_OWNERSHIP_BORROWED : XR_TARGET_OWNERSHIP_OWNED;
    if (!append_rep_intent(builder, &rep, error, error_size))
        return false;
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.append(rune) result slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = borrowed ? XR_TARGET_OWNERSHIP_BORROWED : XR_TARGET_OWNERSHIP_OWNED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    return true;
}

static bool note_string_runes_storage_value(XrTargetPlanBuilder *builder,
                                            XrTargetValueStorageAnalysis *analysis,
                                            uint32_t semantic_operation, char *error,
                                            size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_string_runes_is_exact(builder->semantic_plan, operation, &receiver) ||
        operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >= xr_semantic_plan_function_count(builder->semantic_plan) ||
        receiver >= analysis->total_values || analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1003",
                    "String.runes result authority is incomplete");
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1003",
                    "target profile cannot materialize String.runes result");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "String.runes result slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_OWNED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    /* A prior value intent only proves that some family mentioned this type;
     * it does not prove that family published a layout row. Ask the layout
     * registry directly: it deduplicates the exact dynamic row and rejects
     * any competing kind, extent, geometry, root, or ownership. */
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                              error, error_size) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_string_slice_range_storage_value(XrTargetPlanBuilder *builder,
                                                  XrTargetValueStorageAnalysis *analysis,
                                                  uint32_t semantic_operation, char *error,
                                                  size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    uint32_t start = XR_SEMANTIC_INDEX_NONE;
    uint32_t end = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_string_slice_range_is_exact(builder->semantic_plan, operation, &receiver,
                                                 &start, &end) ||
        operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >= xr_semantic_plan_function_count(builder->semantic_plan) ||
        receiver >= analysis->total_values || start >= analysis->total_values ||
        end >= analysis->total_values || analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1003",
                    "String.slice(start, end) result authority is incomplete");
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1003",
                    "target profile cannot materialize String.slice(start, end) result");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "String.slice(start, end) result slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_OWNED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[operation->result_type] &&
         !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                               error, error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

/* A rune conversion allocates a new immutable String. Its exact storage is the
 * owned tagged value returned by that one operation; the Rune receiver remains
 * independently owned by the scalar family and contributes no dynamic slot. */
static bool note_rune_to_string_storage_value(XrTargetPlanBuilder *builder,
                                              XrTargetValueStorageAnalysis *analysis,
                                              uint32_t semantic_operation, char *error,
                                              size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_rune_to_string_is_exact(builder->semantic_plan, operation, &receiver) ||
        operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >= xr_semantic_plan_function_count(builder->semantic_plan) ||
        receiver >= analysis->total_values || analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1003",
                    "rune.toString result authority is incomplete");
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1003",
                    "target profile cannot materialize rune.toString result");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "rune.toString result slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_OWNED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[operation->result_type] &&
         !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                               error, error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_stringbuilder_to_string_storage_value(XrTargetPlanBuilder *builder,
                                                       XrTargetValueStorageAnalysis *analysis,
                                                       uint32_t semantic_operation, char *error,
                                                       size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    if (!semantic_stringbuilder_to_string_is_exact(builder->semantic_plan, operation, NULL) ||
        operation->result_value >= analysis->total_values ||
        analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.toString result authority is incomplete");
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1003",
                    "target profile cannot materialize StringBuilder.toString result");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.toString result slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_OWNED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[operation->result_type] &&
         !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                               error, error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_json_namespace_value_storage_value(XrTargetPlanBuilder *builder,
                                                    XrTargetValueStorageAnalysis *analysis,
                                                    uint32_t semantic_operation, char *error,
                                                    size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    if (!semantic_json_namespace_value_is_exact(builder->semantic_plan, operation, NULL) ||
        operation->result_value >= analysis->total_values ||
        analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1003",
                    "JSON.value result authority is incomplete");
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1003",
                    "target profile cannot materialize JSON.value result");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "JSON.value result slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_OWNED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    /* The dynamic layout is appended unconditionally: append_layout_intent
     * already deduplicates a compatible intent and rejects a conflicting one,
     * so the result type is guaranteed to carry the dynamic layout the value
     * binding demands even when another value already referenced the type. */
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                              error, error_size) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

/* The panic record is a heap object the constructor allocates and hands back
 * owned, so the result is a dynamic root exactly like every other allocating
 * builtin: the value binding states the dynamic layout, and the slot owns what
 * it holds until the throw path consumes it. */
static bool note_panic_info_constructor_storage_value(XrTargetPlanBuilder *builder,
                                                      XrTargetValueStorageAnalysis *analysis,
                                                      uint32_t semantic_operation, char *error,
                                                      size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    if (!xr_semantic_panic_info_constructor_is_exact(builder->semantic_plan, operation, NULL) ||
        operation->result_value >= analysis->total_values ||
        analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1003",
                    "PanicInfo constructor result authority is incomplete");
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1003",
                    "target profile cannot materialize PanicInfo constructor result");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "PanicInfo constructor result slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_OWNED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                              error, error_size) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_direct_local_callee_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
    const XrDirectLocalCalleeStorageAnalysis *callee_analysis, uint32_t semantic_operation,
    char *error, size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    if (!direct_local_callee_storage_value_is_exact(builder->semantic_plan, callee_analysis,
                                                    operation))
        return fail(error, error_size, "XR_TARGET_1001",
                    "direct-local callee-storage family requires exact shared callable authority");
    if (operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >= xr_semantic_plan_function_count(builder->semantic_plan))
        return fail(error, error_size, "XR_TARGET_1001",
                    "direct-local callee-storage identity is out of range");
    XrTargetMachineRepRecord rep;
    if (!make_static_callable_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize static callable storage");
    /* Exact direct-local callee uses are resolved by the call record. Keep the
     * immutable representation fact, but do not allocate runtime frame storage
     * for a value no executable instruction or call argument can access. */
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
    };
    if (!append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                              error, error_size) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_direct_local_go_callee_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
    const XrDirectLocalGoCalleeStorageAnalysis *callee_analysis, uint32_t semantic_operation,
    char *error, size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    if (!direct_local_go_callee_storage_value_is_exact(builder->semantic_plan, callee_analysis,
                                                       operation))
        return fail(
            error, error_size, "XR_TARGET_1001",
            "direct-local go callee-storage family requires exact shared callable authority");
    if (operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >= xr_semantic_plan_function_count(builder->semantic_plan) ||
        analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1001",
                    "direct-local go callee-storage identity is ambiguous");
    XrTargetMachineRepRecord rep;
    if (!make_static_callable_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize static go callable storage");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "direct-local go callee slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_BORROWED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                              error, error_size) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_channel_allocation_storage_value(XrTargetPlanBuilder *builder,
                                                  XrTargetValueStorageAnalysis *analysis,
                                                  const uint8_t *exact_channel_values,
                                                  uint32_t semantic_operation, char *error,
                                                  size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    bool allocation = semantic_channel_allocation_is_exact(builder->semantic_plan, operation);
    bool alias = semantic_channel_identity_copy_is_exact(
        builder->semantic_plan, operation, exact_channel_values, analysis->total_values);
    if (!allocation && !alias)
        return fail(error, error_size, "XR_TARGET_1001",
                    "channel-allocation-storage family requires exact allocation or identity-copy "
                    "authority");
    if (operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >= xr_semantic_plan_function_count(builder->semantic_plan) ||
        analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic channel outer-storage identity is ambiguous");
    XrTargetMachineRepRecord rep;
    bool rep_ok =
        allocation ? make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep)
                   : make_borrowed_dynamic_value_rep(
                         xr_target_profile_machine_facts(builder->profile), &rep);
    if (!rep_ok || !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize exact channel outer storage");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "channel outer-storage slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = rep.root_kind,
        .ownership = rep.ownership,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[operation->result_type] &&
         !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                               error, error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_channel_receive_storage_value(XrTargetPlanBuilder *builder,
                                               XrTargetValueStorageAnalysis *analysis,
                                               const uint8_t *exact_channel_values,
                                               uint32_t semantic_operation, char *error,
                                               size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    if (!semantic_channel_receive_storage_is_exact(builder->semantic_plan, operation,
                                                   exact_channel_values, analysis->total_values))
        return fail(error, error_size, "XR_TARGET_1001",
                    "channel-receive-storage family requires exact Channel<T> payload authority");
    if (operation->result_type >= analysis->type_count ||
        operation->function >= xr_semantic_plan_function_count(builder->semantic_plan) ||
        analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic channel receive identity is ambiguous");
    uint16_t kind = XR_MACHINE_REP_COUNT;
    if (classify_scalar_type(xr_semantic_plan_type(builder->semantic_plan, operation->result_type),
                             &kind) != XR_TARGET_SCALAR_VALUE ||
        kind == XR_MACHINE_REP_VOID)
        return fail(error, error_size, "XR_TARGET_1001",
                    "channel receive result has no exact scalar representation");
    XrTargetMachineRepRecord rep;
    if (!make_scalar_type_rep(builder, operation->result_type, kind, &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize channel receive storage");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "channel receive slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_NONE,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[operation->result_type] &&
         !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_SCALAR, 0, &rep,
                               error, error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] = kind;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_scalar_value(XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
                              uint32_t semantic_value, uint32_t semantic_type,
                              uint32_t semantic_function, uint32_t semantic_operation, uint8_t role,
                              XrStableId source_identity, char *error, size_t error_size) {
    return note_scalar_value_ex(builder, analysis, semantic_value, semantic_type, semantic_function,
                                semantic_operation, role, source_identity, false, error,
                                error_size);
}

static bool collect_scalar_intents(XrTargetPlanBuilder *builder,
                                   XrTargetValueStorageAnalysis *analysis, char *error,
                                   size_t error_size) {
    if (!index_value_operations(builder->semantic_plan, analysis, error, error_size))
        return false;
    uint32_t parameter_count = (uint32_t) xr_semantic_plan_parameter_count(builder->semantic_plan);
    for (uint32_t i = 0; i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(builder->semantic_plan, i);
        if (!parameter || parameter->value >= analysis->total_values)
            return fail(error, error_size, "XR_TARGET_1001", "semantic parameter is invalid");
        uint32_t operation_index = analysis->value_operations[parameter->value];
        const XrSemanticOperationRecord *operation =
            operation_index == XR_SEMANTIC_INDEX_NONE
                ? NULL
                : xr_semantic_plan_operation(builder->semantic_plan, operation_index);
        if (operation &&
            (operation->opcode != XI_PARAM || operation->function != parameter->function ||
             operation->result_value != parameter->value ||
             operation->result_type != parameter->type))
            return fail(error, error_size, "XR_TARGET_1001",
                        "semantic parameter operation is inconsistent");
        if (!note_scalar_value(builder, analysis, parameter->value, parameter->type,
                               parameter->function, XR_SEMANTIC_INDEX_NONE,
                               XR_TARGET_SLOT_PARAMETER, parameter->id, error, error_size))
            return error && error_size && error[0]
                       ? false
                       : fail(error, error_size, "XR_TARGET_1001",
                              "semantic parameter scalar binding failed");
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation)
            return fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
        if (operation->result_value == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (operation->opcode == XI_PARAM) {
            if (operation->result_value >= analysis->total_values ||
                !analysis->defined_values[operation->result_value])
                return fail(error, error_size, "XR_TARGET_1001",
                            "parameter operation has no semantic parameter");
            continue;
        }
        if (semantic_heap_closure_is_exact(builder->semantic_plan, operation))
            continue;
        /* An address is the one result whose semantic type describes the wrong
         * value: the plan records the subject's type on both sides, because
         * source cannot write "pointer to int". Binding it from that type gives
         * the subject's storage to the address, so the family that knows an
         * address is an address answers for it instead. */
        if (xr_semantic_local_addr_is_exact(builder->semantic_plan, operation, NULL))
            continue;
        if (operation->opcode == XI_CHAN_TRY_RECV) {
            uint16_t receive_kind = XR_MACHINE_REP_COUNT;
            if (classify_scalar_type(
                    xr_semantic_plan_type(builder->semantic_plan, operation->result_type),
                    &receive_kind) == XR_TARGET_SCALAR_VALUE &&
                receive_kind != XR_MACHINE_REP_VOID)
                continue;
        }
        uint8_t role = operation->opcode == XI_PHI ? XR_TARGET_SLOT_PHI : XR_TARGET_SLOT_TEMPORARY;
        if (!note_scalar_value(builder, analysis, operation->result_value, operation->result_type,
                               operation->function, i, role, operation->id, error, error_size))
            return false;
    }
    return true;
}

static bool builder_begin_family(XrTargetPlanBuilder *builder, uint64_t family, char *error,
                                 size_t error_size) {
    /* A poisoned builder refuses every family after the one that poisoned it,
     * which is what keeps a partial plan from reaching a backend. A survey is
     * asking a different question -- which families this module needs that do
     * not exist yet -- and one poisoned answer would hide every family after
     * it, so the survey walks on. Nothing it builds is published: the caller
     * fails the build on the refusal count regardless. */
    if (!builder || builder->materialized || !family ||
        (family & ~XR_TARGET_REQUIRED_FAMILIES) != 0 ||
        (builder->started_family_mask & family) != 0 ||
        (builder->poisoned && !target_survey_enabled()))
        return fail(error, error_size, "XR_TARGET_1001", "target family collector cannot start");
    builder->started_family_mask |= family;
    builder->active_family = family;
    return true;
}

static bool builder_add_scalars(XrTargetPlanBuilder *builder, char *error, size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_SCALAR, error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    XrTargetMachineRepRecord void_rep;
    if (!make_machine_rep(xr_target_profile_machine_facts(builder->profile), XR_MACHINE_REP_VOID,
                          &void_rep) ||
        !append_rep_intent(builder, &void_rep, error, error_size) ||
        !value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size) ||
        !collect_scalar_intents(builder, &analysis, error, error_size)) {
        value_storage_analysis_dispose(&analysis);
        builder->poisoned = true;
        return false;
    }
    value_storage_analysis_dispose(&analysis);
    builder->completed_family_mask |= XR_TARGET_FAMILY_SCALAR;
    return true;
}

static bool builder_add_closure_storage(XrTargetPlanBuilder *builder, char *error,
                                        size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_CLOSURE_STORAGE, error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        if (!semantic_heap_closure_is_exact(builder->semantic_plan, operation))
            continue;
        valid = note_closure_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_CLOSURE_STORAGE;
    return true;
}

/* Binds the allocation to its own owned dynamic slot. Ownership stays a storage
 * fact here exactly as it is for a heap closure: Semantic ownership and the
 * existing AOT array lifetime path still own the allocation, the roots, and the
 * cleanup, so this family adds no root or cleanup row. */
/* The namespace handle is a borrowed compiler-owned reference: it is loaded
 * from a module shared slot and never adapted to a native representation, so
 * its storage stays the borrowed tagged value in its own temporary slot. */
static bool note_native_module_namespace_storage_value(XrTargetPlanBuilder *builder,
                                                       XrTargetValueStorageAnalysis *analysis,
                                                       uint32_t semantic_operation, char *error,
                                                       size_t error_size) {
    const XrSemanticPlan *plan = builder->semantic_plan;
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(plan, semantic_operation);
    if (!operation || operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >= xr_semantic_plan_function_count(plan) ||
        analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1001",
                    "native module namespace storage authority is incomplete");
    XrTargetMachineRepRecord rep;
    if (!make_borrowed_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize native module namespace storage");
    XrStableId slot_identity;
    if (!make_slot_identity(plan, operation->function, XR_TARGET_SLOT_TEMPORARY, operation->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "native module namespace slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_BORROWED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[operation->result_type] &&
         !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                               error, error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->used_types[operation->result_type] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool builder_add_native_module_namespace_storage(XrTargetPlanBuilder *builder, char *error,
                                                        size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_NATIVE_MODULE_NAMESPACE_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    /* A value another family already bound is not this family's to claim. The
     * layout intent stays unseeded: a void reference-count result carrying the
     * namespace type marks no layout of its own, and the layout appender is
     * idempotent and refuses a conflicting geometry on its own. */
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values)
            analysis.defined_values[value->semantic_value] = 1;
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        if ((operation->opcode != XI_IMPORT_REF && operation->opcode != XI_GET_SHARED) ||
            !semantic_native_module_namespace_value_is_exact(builder->semantic_plan, operation))
            continue;
        valid =
            note_native_module_namespace_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_NATIVE_MODULE_NAMESPACE_STORAGE;
    return true;
}

static bool note_array_allocation_storage_value(XrTargetPlanBuilder *builder,
                                                XrTargetValueStorageAnalysis *analysis,
                                                uint32_t semantic_operation, char *error,
                                                size_t error_size) {
    const XrSemanticPlan *plan = builder->semantic_plan;
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(plan, semantic_operation);
    uint8_t array_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t hof_kind = XR_TARGET_ARRAY_HOF_NONE;
    bool exact_array_allocation = semantic_array_allocation_is_exact(plan, operation);
    if (exact_array_allocation) {
        uint32_t child_count = 0;
        const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
        const XrSemanticTypeRecord *array = xr_semantic_plan_type(plan, operation->result_type);
        const XrSemanticTypeRecord *element =
            children && array && array->child_begin < child_count
                ? xr_semantic_plan_type(plan, children[array->child_begin])
                : NULL;
        if (xr_semantic_class_instance_type_source_class(plan, element) != XR_SEMANTIC_INDEX_NONE)
            array_storage = XR_TARGET_ARRAY_STORAGE_TAGGED;
        else if (!xr_target_array_storage_from_semantic(operation->array_element_storage,
                                                        &array_storage))
            exact_array_allocation = false;
    }
    if (!operation || operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >= xr_semantic_plan_function_count(plan) ||
        analysis->defined_values[operation->result_value] ||
        !(exact_array_allocation ||
          semantic_array_intrinsic_is_exact(plan, operation, NULL, &array_storage) ||
          (semantic_array_hof_is_exact(plan, operation, &hof_kind, NULL, &array_storage, NULL, NULL,
                                       NULL) &&
           hof_kind != XR_TARGET_ARRAY_HOF_REDUCE) ||
          xr_target_container_copy_storage(plan, operation, &array_storage)))
        return fail(error, error_size, "XR_TARGET_1001",
                    "array allocation storage authority is incomplete");
    if (analysis->type_rep_kinds[operation->result_type] != XR_MACHINE_REP_COUNT &&
        analysis->type_rep_kinds[operation->result_type] != XR_MACHINE_REP_DYN_VALUE)
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic array type has conflicting storage representations");
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize exact array allocation storage");
    XrStableId slot_identity;
    if (!make_slot_identity(plan, operation->function, XR_TARGET_SLOT_TEMPORARY, operation->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "array allocation slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_OWNED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        !append_tagged_boundary_layout_intent(builder, operation->result_type, array_storage, &rep,
                                              false, error, error_size) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->used_types[operation->result_type] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool builder_add_array_allocation_storage(XrTargetPlanBuilder *builder, char *error,
                                                 size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_ARRAY_ALLOCATION_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    /* A value another family already bound is not this family's to claim. The
     * layout intent stays unseeded: a void result carrying the array type marks
     * no layout of its own, and the layout appender is idempotent and refuses a
     * conflicting geometry on its own. */
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values)
            analysis.defined_values[value->semantic_value] = 1;
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        if (!semantic_array_allocation_is_exact(builder->semantic_plan, operation))
            continue;
        valid = note_array_allocation_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_ARRAY_ALLOCATION_STORAGE;
    return true;
}

static bool builder_add_array_intrinsic_storage(XrTargetPlanBuilder *builder, char *error,
                                                size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_ARRAY_INTRINSIC_STORAGE, error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values)
            analysis.defined_values[value->semantic_value] = 1;
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        if (!semantic_array_intrinsic_is_exact(builder->semantic_plan, operation, NULL, NULL))
            continue;
        valid = note_array_allocation_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_ARRAY_INTRINSIC_STORAGE;
    return true;
}

static bool builder_add_array_hof_result_storage(XrTargetPlanBuilder *builder, char *error,
                                                 size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_ARRAY_HOF_RESULT_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values)
            analysis.defined_values[value->semantic_value] = 1;
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        uint8_t kind = XR_TARGET_ARRAY_HOF_NONE;
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        if (!semantic_array_hof_is_exact(builder->semantic_plan, operation, &kind, NULL, NULL, NULL,
                                         NULL, NULL) ||
            kind == XR_TARGET_ARRAY_HOF_REDUCE)
            continue;
        valid = note_array_allocation_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_ARRAY_HOF_RESULT_STORAGE;
    return true;
}

/* Which carrier a tagged reference occupies at a direct-local boundary. Array
 * value/result rows and ref-boundary class rows share the physical carrier but
 * not the semantic admission rule. The role a slot plays does not decide this:
 * by-value and ref parameters are both XR_TARGET_SLOT_PARAMETER yet arrive in
 * different carriers, while call results and shared reads are both temporaries
 * with different ownership. */
typedef enum XrTargetTaggedBoundaryCarrier {
    /* The tagged outer value, borrowed: one allocation shared for the extent
     * of the borrow, with nothing to release. */
    XR_TARGET_TAGGED_CARRIER_BORROWED_VALUE = 0,
    /* What a ref callee reads back out of its place. It is the same borrowed
     * tagged value, but it belongs to the ref boundary, so it admits whatever
     * that boundary admits rather than only the container shapes. */
    XR_TARGET_TAGGED_CARRIER_REF_PLACE_LOAD,
    /* A pointer to the caller's cell, so a ref callee can rebind it. */
    XR_TARGET_TAGGED_CARRIER_REF_PLACE,
    /* The tagged outer value, owned: a transfer the caller must release. */
    XR_TARGET_TAGGED_CARRIER_OWNED_VALUE,
} XrTargetTaggedBoundaryCarrier;

static bool note_direct_local_tagged_boundary_storage(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis, uint32_t semantic_value,
    uint32_t semantic_type, uint32_t semantic_function, uint32_t semantic_operation, uint8_t role,
    uint8_t carrier, XrStableId source_identity, char *error, size_t error_size) {
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    bool parameter = role == XR_TARGET_SLOT_PARAMETER;
    /* Only the ref carrier reaches elements, so only it has to know what they
     * are; the two value carriers state the element storage the layout records
     * and are content when the type has none. */
    bool ref_boundary = carrier == XR_TARGET_TAGGED_CARRIER_REF_PLACE ||
                        carrier == XR_TARGET_TAGGED_CARRIER_REF_PLACE_LOAD;
    bool exact_type = semantic_direct_local_tagged_boundary_type_is_exact(
        builder->semantic_plan, semantic_type, carrier == XR_TARGET_TAGGED_CARRIER_REF_PLACE,
        ref_boundary, &storage);
    bool value_in_range = semantic_value < analysis->total_values;
    bool type_in_range = semantic_type < analysis->type_count;
    bool function_in_range =
        semantic_function < xr_semantic_plan_function_count(builder->semantic_plan);
    bool operation_in_range =
        parameter || semantic_operation < xr_semantic_plan_operation_count(builder->semantic_plan);
    bool already_bound = value_in_range && analysis->defined_values[semantic_value];
    if (!exact_type || !value_in_range || !type_in_range || !function_in_range ||
        !operation_in_range || already_bound) {
        if (target_trace_enabled()) {
            fprintf(stderr,
                    "[target] refused while binding direct-local ref boundary storage: "
                    "value=%u type=%u function=%u operation=%u role=%u carrier=%u storage=%u\n",
                    semantic_value, semantic_type, semantic_function, semantic_operation, role,
                    carrier, storage);
            target_trace_type(builder->semantic_plan, "boundary value type", semantic_type);
            target_trace_judgement("boundary type is exact", exact_type);
            target_trace_judgement("semantic value is in range", value_in_range);
            target_trace_judgement("semantic type is in range", type_in_range);
            target_trace_judgement("semantic function is in range", function_in_range);
            target_trace_judgement("semantic operation is in range", operation_in_range);
            target_trace_judgement("semantic value is not already bound", !already_bound);
        }
        return fail(error, error_size, "XR_TARGET_1001",
                    "direct-local tagged boundary storage authority is incomplete");
    }
    XrTargetMachineRepRecord rep;
    XrTargetMachineRepRecord layout_rep;
    const XrTargetMachineFacts *facts = xr_target_profile_machine_facts(builder->profile);
    bool rep_exact;
    switch (carrier) {
        case XR_TARGET_TAGGED_CARRIER_REF_PLACE:
            rep_exact = make_machine_rep(facts, XR_MACHINE_REP_RAW_PTR, &rep);
            if (rep_exact)
                rep.ownership = XR_TARGET_OWNERSHIP_BORROWED;
            break;
        case XR_TARGET_TAGGED_CARRIER_OWNED_VALUE:
            rep_exact = make_dynamic_value_rep(facts, &rep);
            break;
        default:
            rep_exact = make_borrowed_dynamic_value_rep(facts, &rep);
            break;
    }
    if (!rep_exact || !make_borrowed_dynamic_value_rep(facts, &layout_rep) ||
        !append_rep_intent(builder, &rep, error, error_size) ||
        !append_rep_intent(builder, &layout_rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize tagged boundary storage");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, semantic_function, role, source_identity,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "direct-local tagged ref parameter slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = semantic_function,
        .semantic_value = semantic_value,
        .semantic_operation = parameter ? XR_SEMANTIC_INDEX_NONE : semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = role,
        .root_kind = rep.root_kind,
        .ownership = rep.ownership,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = semantic_value,
        .semantic_function = semantic_function,
        .semantic_type = semantic_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        !append_tagged_boundary_layout_intent(builder, semantic_type, storage, &layout_rep,
                                              ref_boundary, error, error_size) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[semantic_value] = 1;
    analysis->used_types[semantic_type] = 1;
    analysis->value_types[semantic_value] = semantic_type;
    analysis->value_functions[semantic_value] = semantic_function;
    analysis->type_rep_kinds[semantic_type] = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* A ref call points at storage another family already proved. The source may be
 * a fresh Array allocation, a typed reference join, a shared read or a class
 * instance; the call boundary must not reconstruct that producer from an
 * opcode. It consumes the existing value binding and checks only the facts it
 * owns: exact boundary type, function identity and tagged storage. */
static bool builder_ref_caller_storage_is_exact(const XrTargetPlanBuilder *builder,
                                                uint32_t semantic_value, uint32_t semantic_type,
                                                uint32_t semantic_function) {
    uint8_t array_element_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!builder || !semantic_direct_local_tagged_boundary_type_is_exact(
                        builder->semantic_plan, semantic_type, false, true, &array_element_storage))
        return false;
    const XrTargetValueIntent *match = NULL;
    for (uint32_t i = 0; i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *intent = &builder->value_intents[i];
        if (intent->semantic_value != semantic_value)
            continue;
        if (match)
            return false;
        match = intent;
    }
    if (!match || match->semantic_type != semantic_type ||
        match->semantic_function != semantic_function || !match->has_slot)
        return false;

    XrTargetMachineRepRecord owned_rep;
    XrTargetMachineRepRecord borrowed_rep;
    const XrTargetMachineFacts *facts = xr_target_profile_machine_facts(builder->profile);
    if (!make_dynamic_value_rep(facts, &owned_rep) ||
        !make_borrowed_dynamic_value_rep(facts, &borrowed_rep))
        return false;
    bool owned = compare_rep_record(&match->register_rep, &owned_rep) == 0 &&
                 compare_rep_record(&match->memory_rep, &owned_rep) == 0;
    bool borrowed = compare_rep_record(&match->register_rep, &borrowed_rep) == 0 &&
                    compare_rep_record(&match->memory_rep, &borrowed_rep) == 0;
    if (!owned && !borrowed)
        return false;

    const XrTargetSlotIntent *slot_match = NULL;
    for (uint32_t i = 0; i < builder->slot_intent_count; i++) {
        const XrTargetSlotIntent *slot = &builder->slot_intents[i];
        if (!xr_stable_id_equal(slot->identity, match->slot_identity))
            continue;
        if (slot_match)
            return false;
        slot_match = slot;
    }
    uint8_t ownership = owned ? XR_TARGET_OWNERSHIP_OWNED : XR_TARGET_OWNERSHIP_BORROWED;
    if (!slot_match || slot_match->function != semantic_function ||
        slot_match->semantic_value != semantic_value ||
        compare_rep_record(&slot_match->register_rep, &match->register_rep) != 0 ||
        compare_rep_record(&slot_match->memory_rep, &match->memory_rep) != 0 ||
        slot_match->root_kind != XR_TARGET_ROOT_DYNAMIC || slot_match->ownership != ownership ||
        slot_match->logical_slot != XR_SEMANTIC_INDEX_NONE ||
        slot_match->debug_variable != XR_SEMANTIC_INDEX_NONE)
        return false;

    const XrTargetLayoutIntent *layout_match = NULL;
    for (uint32_t i = 0; i < builder->layout_intent_count; i++) {
        const XrTargetLayoutIntent *layout = &builder->layout_intents[i];
        if (layout->semantic_type != semantic_type)
            continue;
        if (layout_match)
            return false;
        layout_match = layout;
    }
    if (!layout_match || layout_match->kind != XR_TARGET_LAYOUT_DYNAMIC ||
        layout_match->element_count != 0 ||
        layout_match->array_element_storage != array_element_storage ||
        layout_match->memory_rep.kind != XR_MACHINE_REP_DYN_VALUE ||
        layout_match->memory_rep.register_bits != owned_rep.register_bits ||
        layout_match->memory_rep.memory_size != owned_rep.memory_size ||
        layout_match->memory_rep.memory_align != owned_rep.memory_align ||
        layout_match->memory_rep.root_kind != XR_TARGET_ROOT_DYNAMIC ||
        layout_match->memory_rep.null_encoding != XR_TARGET_NULL_TAGGED ||
        (layout_match->memory_rep.ownership != XR_TARGET_OWNERSHIP_OWNED &&
         layout_match->memory_rep.ownership != XR_TARGET_OWNERSHIP_BORROWED))
        return false;
    return true;
}

static bool builder_add_direct_local_tagged_ref_argument_storage(XrTargetPlanBuilder *builder,
                                                                 char *error, size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_DIRECT_LOCAL_TAGGED_REF_ARGUMENT_STORAGE,
                              error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value >= analysis.total_values ||
            value->semantic_type >= analysis.type_count)
            continue;
        analysis.defined_values[value->semantic_value] = 1;
        analysis.used_types[value->semantic_type] = 1;
        analysis.value_types[value->semantic_value] = value->semantic_type;
        analysis.value_functions[value->semantic_value] = value->semantic_function;
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    /* An Array a direct-local call hands back. The result is a transfer, so it
     * is the one Array carrier in this family that owns what it holds. */
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *call =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!call || call->result_value == XR_SEMANTIC_INDEX_NONE ||
            call->result_value >= analysis.total_values ||
            analysis.defined_values[call->result_value] ||
            !semantic_direct_local_array_result_is_exact(
                builder->semantic_plan, call,
                semantic_direct_local_callee_for_operation(builder->semantic_plan, i)))
            continue;
        valid = note_direct_local_tagged_boundary_storage(
            builder, &analysis, call->result_value, call->result_type, call->function, i,
            XR_TARGET_SLOT_TEMPORARY, XR_TARGET_TAGGED_CARRIER_OWNED_VALUE, call->id, error,
            error_size);
    }
    /* Every exact shared read of an Array is a borrowed outer tagged value,
     * whether or not a direct-local call later observes it. A named mutable
     * local lowers through a shared cell, so omitting this carrier left the
     * ordinary receiver of Array members without target storage authority. */
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *read =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!read || read->result_value >= analysis.total_values ||
            analysis.defined_values[read->result_value] ||
            !xr_semantic_tagged_array_shared_read_is_exact(builder->semantic_plan, read))
            continue;
        valid = note_direct_local_tagged_boundary_storage(
            builder, &analysis, read->result_value, read->result_type, read->function, i,
            XR_TARGET_SLOT_TEMPORARY, XR_TARGET_TAGGED_CARRIER_BORROWED_VALUE, read->id, error,
            error_size);
    }
    uint32_t parameter_count = (uint32_t) xr_semantic_plan_parameter_count(builder->semantic_plan);
    /* An Array parameter passed by value. It shares the caller's allocation for
     * the extent of the call, so it borrows the tagged value it is handed and
     * needs no place of its own. */
    for (uint32_t i = 0; valid && i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(builder->semantic_plan, i);
        uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
        if (!semantic_direct_local_array_value_parameter_is_exact(builder->semantic_plan, parameter,
                                                                  &storage) ||
            parameter->value >= analysis.total_values || analysis.defined_values[parameter->value])
            continue;
        valid = note_direct_local_tagged_boundary_storage(
            builder, &analysis, parameter->value, parameter->type, parameter->function,
            XR_SEMANTIC_INDEX_NONE, XR_TARGET_SLOT_PARAMETER,
            XR_TARGET_TAGGED_CARRIER_BORROWED_VALUE, parameter->id, error, error_size);
    }
    for (uint32_t i = 0; valid && i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(builder->semantic_plan, i);
        uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
        if (!semantic_direct_local_tagged_ref_parameter_is_exact(builder->semantic_plan, parameter,
                                                                 &storage))
            continue;
        valid = note_direct_local_tagged_boundary_storage(
            builder, &analysis, parameter->value, parameter->type, parameter->function,
            XR_SEMANTIC_INDEX_NONE, XR_TARGET_SLOT_PARAMETER, XR_TARGET_TAGGED_CARRIER_REF_PLACE,
            parameter->id, error, error_size);
        uint32_t operation_count =
            (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
        for (uint32_t operation_index = 0; valid && operation_index < operation_count;
             operation_index++) {
            const XrSemanticOperationRecord *load =
                xr_semantic_plan_operation(builder->semantic_plan, operation_index);
            if (!semantic_direct_local_tagged_ref_place_load_is_exact(
                    builder->semantic_plan, load, operation_index, parameter->value,
                    parameter->type) ||
                load->function != parameter->function)
                continue;
            if (load->result_value >= analysis.total_values) {
                valid = fail(error, error_size, "XR_TARGET_1001",
                             "direct-local tagged ref parameter load is invalid");
                break;
            }
            if (!analysis.defined_values[load->result_value])
                valid = note_direct_local_tagged_boundary_storage(
                    builder, &analysis, load->result_value, load->result_type, load->function,
                    operation_index, XR_TARGET_SLOT_TEMPORARY,
                    XR_TARGET_TAGGED_CARRIER_REF_PLACE_LOAD, load->id, error, error_size);
        }
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(builder->semantic_plan, &operand_count);
    uint32_t target_count = (uint32_t) xr_semantic_plan_call_target_count(builder->semantic_plan);
    for (uint32_t target_index = 0; valid && operands && target_index < target_count;
         target_index++) {
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(builder->semantic_plan, target_index);
        const XrSemanticOperationRecord *call =
            target ? xr_semantic_plan_operation(builder->semantic_plan, target->operation) : NULL;
        const XrSemanticFunctionRecord *callee =
            target ? xr_semantic_plan_function(builder->semantic_plan, target->function) : NULL;
        if (!target || target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL || !call || !callee ||
            (call->opcode != XI_CALL && call->opcode != XI_TAIL_CALL) ||
            call->operand_count != (uint32_t) callee->parameter_count + 1u ||
            call->operand_begin > operand_count ||
            call->operand_count > operand_count - call->operand_begin)
            continue;
        for (uint16_t ordinal = 0; valid && ordinal < callee->parameter_count; ordinal++) {
            const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(
                builder->semantic_plan, callee->parameter_begin + ordinal);
            const XrSemanticOperandRecord *operand = &operands[call->operand_begin + ordinal + 1u];
            uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
            uint32_t caller_storage_value = XR_SEMANTIC_INDEX_NONE;
            if (!semantic_direct_local_tagged_ref_parameter_is_exact(builder->semantic_plan,
                                                                     parameter, &storage) ||
                parameter->type != operand->type || operand->role != XR_SEM_OPERAND_ARGUMENT ||
                operand->parameter != (int16_t) ordinal ||
                operand->parameter_mode != XR_PARAM_REF || operand->access != XR_CALL_ARG_REF ||
                operand->origin == XI_PLACE_ORIGIN_NONE ||
                operand->lifetime != XI_PLACE_LIFETIME_CALL_BOUND ||
                operand->escape != XI_PLACE_ESCAPE_NONE ||
                operand->ownership_action != XR_SEM_OPERAND_BORROW ||
                operand->transfer_mode != XR_TRANSFER_SHARE ||
                operand->flags != (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE) ||
                !semantic_direct_local_tagged_ref_place_is_exact(builder->semantic_plan, operand,
                                                                 &caller_storage_value))
                continue;
            if (caller_storage_value >= analysis.total_values) {
                valid = fail(error, error_size, "XR_TARGET_1001",
                             "direct-local tagged ref caller storage value is invalid");
                break;
            }
            /* The place points at a value another exact producer family has
             * already bound. Consume that authority instead of guessing the
             * producer from one opcode: allocations, joins and shared reads all
             * legitimately back a local address. */
            bool exact_storage = builder_ref_caller_storage_is_exact(builder, caller_storage_value,
                                                                     operand->type, call->function);
            if (!exact_storage || !analysis.defined_values[caller_storage_value]) {
                if (target_trace_enabled()) {
                    fprintf(stderr,
                            "[target] refused in direct-local Array ref caller storage: "
                            "target=%u argument=%u caller_storage_value=%u bound=%u\n",
                            target_index, ordinal, caller_storage_value,
                            analysis.defined_values[caller_storage_value]);
                    target_trace_operation(builder->semantic_plan, target->operation, call);
                    target_trace_type(builder->semantic_plan, "Array ref argument type",
                                      operand->type);
                    target_trace_judgement("caller storage has an exact tagged value binding",
                                           exact_storage);
                    for (uint32_t definition_index = 0; definition_index < operation_count;
                         definition_index++) {
                        const XrSemanticOperationRecord *definition =
                            xr_semantic_plan_operation(builder->semantic_plan, definition_index);
                        if (definition && definition->result_value == caller_storage_value) {
                            fprintf(stderr, "[target]   candidate storage producer:\n");
                            target_trace_operation(builder->semantic_plan, definition_index,
                                                   definition);
                        }
                    }
                }
                valid = fail(error, error_size, "XR_TARGET_1001",
                             "direct-local tagged ref caller has no exact storage producer");
                break;
            }
            for (uint32_t operation_index = 0; valid && operation_index < operation_count;
                 operation_index++) {
                const XrSemanticOperationRecord *load =
                    xr_semantic_plan_operation(builder->semantic_plan, operation_index);
                if (!semantic_direct_local_tagged_ref_place_load_is_exact(
                        builder->semantic_plan, load, operation_index, operand->value,
                        operand->type))
                    continue;
                if (load->result_value >= analysis.total_values) {
                    valid = fail(error, error_size, "XR_TARGET_1001",
                                 "direct-local tagged ref writeback value is invalid");
                    break;
                }
                if (!analysis.defined_values[load->result_value])
                    valid = note_direct_local_tagged_boundary_storage(
                        builder, &analysis, load->result_value, load->result_type, load->function,
                        operation_index, XR_TARGET_SLOT_TEMPORARY,
                        XR_TARGET_TAGGED_CARRIER_REF_PLACE_LOAD, load->id, error, error_size);
            }
        }
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_DIRECT_LOCAL_TAGGED_REF_ARGUMENT_STORAGE;
    return true;
}

/* Binds one nullable scalar to its own borrowed dynamic slot. BORROWED is the
 * storage fact that this row claims no allocation, no root map, and no cleanup:
 * the carrier holds either the null tag or a plain machine scalar, so nothing
 * in it is ever released. Every nullable scalar of one semantic type shares the
 * single tagged geometry, so the type carries one dynamic layout. */
static bool note_nullable_scalar_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis, uint32_t semantic_value,
    uint32_t semantic_type, uint32_t semantic_function, uint32_t semantic_operation, uint8_t role,
    XrStableId source_identity, char *error, size_t error_size) {
    if (semantic_value >= analysis->total_values || semantic_type >= analysis->type_count ||
        semantic_function >= xr_semantic_plan_function_count(builder->semantic_plan) ||
        analysis->defined_values[semantic_value])
        return fail(error, error_size, "XR_TARGET_1001",
                    "nullable scalar storage identity is incomplete");
    if (analysis->type_rep_kinds[semantic_type] != XR_MACHINE_REP_COUNT &&
        analysis->type_rep_kinds[semantic_type] != XR_MACHINE_REP_DYN_VALUE)
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic nullable type has conflicting storage representations");
    XrTargetMachineRepRecord rep;
    if (!make_borrowed_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize nullable scalar storage");
    XrStableId slot_identity;
    const bool parameter_slot = role == XR_TARGET_SLOT_PARAMETER;
    if ((!parameter_slot &&
         semantic_operation >= xr_semantic_plan_operation_count(builder->semantic_plan)) ||
        !make_slot_identity(builder->semantic_plan, semantic_function, role, source_identity,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "nullable scalar slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = semantic_function,
        .semantic_value = semantic_value,
        .semantic_operation = parameter_slot ? XR_SEMANTIC_INDEX_NONE : semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = role,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_BORROWED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = semantic_value,
        .semantic_function = semantic_function,
        .semantic_type = semantic_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[semantic_type] &&
         !append_layout_intent(builder, semantic_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep, error,
                               error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[semantic_value] = 1;
    analysis->used_types[semantic_type] = 1;
    analysis->value_types[semantic_value] = semantic_type;
    analysis->value_functions[semantic_value] = semantic_function;
    analysis->type_rep_kinds[semantic_type] = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* This family is driven by the semantic type rather than by one opcode: every
 * value spelled `T?` over an exact machine scalar has the same single storage
 * fact whatever produced it, so parameters and operation results are walked in
 * the same order the scalar family walks them and are claimed on the same
 * ground. A value another family already bound is not this family's to claim. */
static bool builder_add_nullable_scalar_storage(XrTargetPlanBuilder *builder, char *error,
                                                size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_NULLABLE_SCALAR_STORAGE, error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values)
            analysis.defined_values[value->semantic_value] = 1;
    }
    uint32_t parameter_count = (uint32_t) xr_semantic_plan_parameter_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(builder->semantic_plan, i);
        if (!parameter || parameter->value >= analysis.total_values) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic parameter is invalid");
            break;
        }
        if (!semantic_nullable_scalar_type_is_exact(
                xr_semantic_plan_type(builder->semantic_plan, parameter->type)) ||
            analysis.defined_values[parameter->value])
            continue;
        valid = note_nullable_scalar_storage_value(
            builder, &analysis, parameter->value, parameter->type, parameter->function,
            XR_SEMANTIC_INDEX_NONE, XR_TARGET_SLOT_PARAMETER, parameter->id, error, error_size);
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        if (operation->result_value == XR_SEMANTIC_INDEX_NONE ||
            operation->result_value >= analysis.total_values ||
            analysis.defined_values[operation->result_value] ||
            !semantic_nullable_scalar_type_is_exact(
                xr_semantic_plan_type(builder->semantic_plan, operation->result_type)))
            continue;
        uint8_t role = operation->opcode == XI_PHI ? XR_TARGET_SLOT_PHI : XR_TARGET_SLOT_TEMPORARY;
        valid = note_nullable_scalar_storage_value(builder, &analysis, operation->result_value,
                                                   operation->result_type, operation->function, i,
                                                   role, operation->id, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_NULLABLE_SCALAR_STORAGE;
    return true;
}

/* An array member that hands back its receiver defines a second name for the
 * container the receiver already owns. The only storage fact this plan can
 * state for that name is the same owned tagged outer value the receiver
 * carries. The family also states the type's one dynamic layout: the receiver
 * can be an array literal whose defining operation has no separate allocation
 * authority, and append_layout_intent is idempotent when another exact family
 * already froze the same geometry. */
static bool builder_add_array_member_result_storage(XrTargetPlanBuilder *builder, char *error,
                                                    size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_ARRAY_MEMBER_RESULT_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values)
            analysis.defined_values[value->semantic_value] = 1;
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        bool receiver_result = false;
        bool member_exact =
            operation && semantic_array_member_scalar_is_exact(builder->semantic_plan, operation,
                                                               NULL, &receiver_result);
        /* A member that builds a string needs the same dynamic owned slot a
         * receiver-returning one gets: both hand back a value held in the
         * carrier rather than a scalar the row states outright. */
        uint32_t member_metadata_count = 0;
        const char *const *member_metadata =
            xr_semantic_plan_metadata(builder->semantic_plan, &member_metadata_count);
        const XrArrayMemberShape *member_shape =
            member_exact && member_metadata && operation->metadata_begin < member_metadata_count
                ? xr_array_member_shape(member_metadata[operation->metadata_begin],
                                        operation->operand_count)
                : NULL;
        bool string_member =
            member_shape && member_shape->result_shape == XR_ARRAY_MEMBER_RESULT_STRING;
        bool scalar_member = member_exact && (receiver_result || string_member);
        bool scalar_fill = operation && semantic_array_fill_scalar_is_exact(
                                            builder->semantic_plan, operation, NULL, NULL, NULL);
        if (!scalar_member && !scalar_fill)
            continue;
        if (operation->result_value >= analysis.total_values ||
            analysis.defined_values[operation->result_value]) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "array member result storage identity is ambiguous");
            break;
        }
        XrTargetMachineRepRecord rep;
        XrStableId slot_identity;
        valid = make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) &&
                append_rep_intent(builder, &rep, error, error_size) &&
                make_slot_identity(builder->semantic_plan, operation->function,
                                   XR_TARGET_SLOT_TEMPORARY, operation->id, XR_SEMANTIC_INDEX_NONE,
                                   &slot_identity);
        if (!valid)
            break;
        XrTargetSlotIntent slot = {
            .identity = slot_identity,
            .function = operation->function,
            .semantic_value = operation->result_value,
            .semantic_operation = i,
            .logical_slot = XR_SEMANTIC_INDEX_NONE,
            .register_rep = rep,
            .memory_rep = rep,
            .role = XR_TARGET_SLOT_TEMPORARY,
            .root_kind = XR_TARGET_ROOT_DYNAMIC,
            .ownership = XR_TARGET_OWNERSHIP_OWNED,
            .debug_variable = XR_SEMANTIC_INDEX_NONE,
        };
        XrTargetValueIntent value = {
            .semantic_value = operation->result_value,
            .semantic_function = operation->function,
            .semantic_type = operation->result_type,
            .register_rep = rep,
            .memory_rep = rep,
            .slot_identity = slot_identity,
            .has_slot = true,
        };
        valid = append_slot_intent(builder, &slot, error, error_size) &&
                append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0,
                                     &rep, error, error_size) &&
                append_value_intent(builder, &value, error, error_size);
        if (valid)
            analysis.defined_values[operation->result_value] = 1;
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_ARRAY_MEMBER_RESULT_STORAGE;
    return true;
}

/* Binds the class object to its own owned dynamic slot. The allocation is a
 * module-level ownership root: Semantic ownership and the existing AOT class
 * lifetime path still own the allocation, the roots and the cleanup, so this
 * family adds no root or cleanup row. The representation is the outer tagged
 * value because that is what Xi selects for an erased reference; freezing a
 * bare object pointer here would state a machine fact the IR does not carry. */
static bool note_source_class_object_storage_value(XrTargetPlanBuilder *builder,
                                                   XrTargetValueStorageAnalysis *analysis,
                                                   uint32_t semantic_operation, char *error,
                                                   size_t error_size) {
    const XrSemanticPlan *plan = builder->semantic_plan;
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(plan, semantic_operation);
    if (!operation || operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >= xr_semantic_plan_function_count(plan) ||
        analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1001",
                    "source class object storage authority is incomplete");
    if (analysis->type_rep_kinds[operation->result_type] != XR_MACHINE_REP_COUNT &&
        analysis->type_rep_kinds[operation->result_type] != XR_MACHINE_REP_DYN_VALUE)
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic class object type has conflicting storage representations");
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize exact class object storage");
    XrStableId slot_identity;
    if (!make_slot_identity(plan, operation->function, XR_TARGET_SLOT_TEMPORARY, operation->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "class object slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_OWNED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[operation->result_type] &&
         !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                               error, error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->used_types[operation->result_type] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool builder_add_source_class_object_storage(XrTargetPlanBuilder *builder, char *error,
                                                    size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_SOURCE_CLASS_OBJECT_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    /* A value another family already bound is not this family's to claim. */
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values)
            analysis.defined_values[value->semantic_value] = 1;
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    uint32_t class_count = (uint32_t) xr_semantic_plan_source_class_count(builder->semantic_plan);
    uint8_t *claimed =
        class_count ? (uint8_t *) allocate_records(class_count, sizeof(*claimed)) : NULL;
    if (valid && class_count && !claimed) {
        value_storage_analysis_dispose(&analysis);
        builder->poisoned = true;
        return fail(error, error_size, "XR_EXEC_5003",
                    "class object storage collector allocation failed");
    }
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        if (operation->opcode != XI_CLASS_CREATE)
            continue;
        uint32_t source_class =
            xr_semantic_class_object_source_class(builder->semantic_plan, operation);
        /* A generic template's class object is not this family's to bind:
         * nothing is ever an instance of the template, and each specialisation
         * lowers its own class object which this family does bind. Declining it
         * is not the same as failing to name it -- the check below still
         * refuses a class object that should have landed here. */
        if (xr_semantic_class_object_is_generic_template(builder->semantic_plan, operation))
            continue;
        /* Every class allocation in the module must land in this family: an
         * allocation the shared judgement cannot name would otherwise leave a
         * value with no representation row at all. */
        if (source_class >= class_count || claimed[source_class]) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "class allocation has no exact source class authority");
            break;
        }
        claimed[source_class] = 1;
        valid = note_source_class_object_storage_value(builder, &analysis, i, error, error_size);
    }
    xr_free(claimed);
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_SOURCE_CLASS_OBJECT_STORAGE;
    return true;
}

/* Binds the three values a source-class construction produces: the borrowed
 * read of the class object that the call dispatches on, the owned instance the
 * call returns, and every borrowed read of that instance out of its module
 * slot. All three are outer tagged values for the same reason the class object
 * is: the IR types the class object `any` and gives the instance no machine
 * geometry, so a bare object pointer would state a fact the IR does not carry.
 * The ownership is the operation's own result ownership rather than a property
 * of the family, so a read can never be frozen as an owning root. Semantic
 * ownership and the existing AOT class lifetime path still own the allocation,
 * its roots and its cleanup, so this family adds no root or cleanup row. */
static bool note_source_class_instance_storage_value(XrTargetPlanBuilder *builder,
                                                     XrTargetValueStorageAnalysis *analysis,
                                                     uint32_t semantic_operation, char *error,
                                                     size_t error_size) {
    const XrSemanticPlan *plan = builder->semantic_plan;
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(plan, semantic_operation);
    if (!operation || operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >= xr_semantic_plan_function_count(plan) ||
        analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1001",
                    "source class instance storage authority is incomplete");
    bool owned = operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED;
    if (!owned && operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED)
        return fail(error, error_size, "XR_TARGET_1001",
                    "source class instance value has no exact ownership authority");
    if (analysis->type_rep_kinds[operation->result_type] != XR_MACHINE_REP_COUNT &&
        analysis->type_rep_kinds[operation->result_type] != XR_MACHINE_REP_DYN_VALUE)
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic class instance type has conflicting storage representations");
    XrTargetMachineRepRecord rep;
    const XrTargetMachineFacts *facts = xr_target_profile_machine_facts(builder->profile);
    if (!(owned ? make_dynamic_value_rep(facts, &rep)
                : make_borrowed_dynamic_value_rep(facts, &rep)) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize exact class instance storage");
    XrStableId slot_identity;
    if (!make_slot_identity(plan, operation->function, XR_TARGET_SLOT_TEMPORARY, operation->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "class instance slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = owned ? XR_TARGET_OWNERSHIP_OWNED : XR_TARGET_OWNERSHIP_BORROWED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[operation->result_type] &&
         !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                               error, error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->used_types[operation->result_type] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* Imported instances have no caller-local source-class index. Consume the one
 * explicit constructor target and its frozen dependency class export instead;
 * this is mechanical plan authority and never walks the caller's import/store
 * graph again. */
static bool
imported_source_class_instance_storage_is_exact(const XrTargetPlanBuilder *builder,
                                                uint32_t semantic_operation,
                                                const XrSemanticOperationRecord *operation) {
    const XrSemanticPlan *plan = builder ? builder->semantic_plan : NULL;
    const XrSemanticCallTargetRecord *target = NULL;
    uint32_t target_count = (uint32_t) xr_semantic_plan_call_target_count(plan);
    for (uint32_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *candidate = xr_semantic_plan_call_target(plan, i);
        if (!candidate || candidate->operation != semantic_operation ||
            candidate->kind != XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR ||
            candidate->dependency == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (target)
            return false;
        target = candidate;
    }
    const XrSemanticPlan *dependency =
        target && target->dependency < builder->semantic_dependency_count
            ? builder->semantic_dependencies[target->dependency]
            : NULL;
    const XrSemanticDependencyRecord *dependency_record =
        target && target->dependency < xr_semantic_plan_dependency_count(plan)
            ? xr_semantic_plan_dependency(plan, target->dependency)
            : NULL;
    const XrSemanticSourceExportRecord *source_export =
        dependency && target->source_export < xr_semantic_plan_source_export_count(dependency)
            ? xr_semantic_plan_source_export(dependency, target->source_export)
            : NULL;
    uint32_t constructor = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_class =
        target ? xr_semantic_imported_class_construction_authority_source_class(
                     plan, dependency, dependency_record, source_export, operation, &constructor)
               : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticFunctionRecord *callee =
        constructor != XR_SEMANTIC_INDEX_NONE ? xr_semantic_plan_function(dependency, constructor)
                                              : NULL;
    return target && source_export && source_class != XR_SEMANTIC_INDEX_NONE &&
           target->function == XR_SEMANTIC_INDEX_NONE &&
           target->callable_type == operation->result_type &&
           xr_stable_id_equal(target->export_identity, source_export->id) &&
           ((callee && xr_stable_id_equal(target->callee_function, callee->id)) ||
            (!callee && stable_id_is_zero(target->callee_function)));
}

static bool builder_add_source_class_instance_storage(XrTargetPlanBuilder *builder, char *error,
                                                      size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_SOURCE_CLASS_INSTANCE_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    /* A value another family already bound is not this family's to claim. */
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values)
            analysis.defined_values[value->semantic_value] = 1;
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        bool local =
            xr_semantic_class_instance_value_is_exact(builder->semantic_plan, operation, NULL);
        bool imported = !local && operation->opcode == XI_CALL &&
                        imported_source_class_instance_storage_is_exact(builder, i, operation);
        if (!local && !imported)
            continue;
        valid = note_source_class_instance_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_SOURCE_CLASS_INSTANCE_STORAGE;
    return true;
}

/* Binds a class instance that crosses a parameter boundary: the receiver a
 * constructor builds, the receiver an instance method borrows, and an ordinary
 * parameter declared with the class as its type. A receiver is the one value in
 * the construction family whose declaration its own type row cannot name, so it
 * is bound from the function's identity instead; everything else about all
 * three is the same outer tagged value the construction returns, because the IR
 * gives an instance no machine geometry a bare object pointer could state. The
 * ownership is the parameter's own recorded ownership rather than a property of
 * the family, and the slot is a parameter slot rather than a temporary, because
 * the value is bound on entry rather than computed. The allocation, its roots
 * and its cleanup stay with semantic ownership and the existing AOT class
 * lifetime path, so this family adds no root or cleanup row. */
static bool note_source_class_parameter_storage_value(XrTargetPlanBuilder *builder,
                                                      XrTargetValueStorageAnalysis *analysis,
                                                      uint32_t parameter_index, char *error,
                                                      size_t error_size) {
    const XrSemanticPlan *plan = builder->semantic_plan;
    const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(plan, parameter_index);
    uint32_t source_class =
        xr_semantic_class_instance_parameter_source_class(plan, parameter_index);
    if (source_class == XR_SEMANTIC_INDEX_NONE || !parameter ||
        parameter->value >= analysis->total_values || parameter->type >= analysis->type_count ||
        parameter->function >= xr_semantic_plan_function_count(plan) ||
        analysis->defined_values[parameter->value]) {
        if (target_trace_enabled())
            fprintf(stderr,
                    "[target] refused in source-class parameter storage: parameter=%u "
                    "source_class=%u value=%u/%u type=%u/%u function=%u/%zu already_bound=%u\n",
                    parameter_index, source_class,
                    parameter ? parameter->value : XR_SEMANTIC_INDEX_NONE, analysis->total_values,
                    parameter ? parameter->type : XR_SEMANTIC_INDEX_NONE, analysis->type_count,
                    parameter ? parameter->function : XR_SEMANTIC_INDEX_NONE,
                    xr_semantic_plan_function_count(plan),
                    parameter && parameter->value < analysis->total_values
                        ? analysis->defined_values[parameter->value]
                        : 0);
        return fail(error, error_size, "XR_TARGET_1001",
                    "source class parameter storage authority is incomplete");
    }
    bool owned = parameter->ownership == XI_OWN_OWNED;
    if (!owned && parameter->ownership != XI_OWN_BORROWED)
        return fail(error, error_size, "XR_TARGET_1001",
                    "source class receiver has no exact ownership authority");
    if (analysis->type_rep_kinds[parameter->type] != XR_MACHINE_REP_COUNT &&
        analysis->type_rep_kinds[parameter->type] != XR_MACHINE_REP_DYN_VALUE)
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic class receiver type has conflicting storage representations");
    XrTargetMachineRepRecord rep;
    const XrTargetMachineFacts *facts = xr_target_profile_machine_facts(builder->profile);
    if (!(owned ? make_dynamic_value_rep(facts, &rep)
                : make_borrowed_dynamic_value_rep(facts, &rep)) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize exact class receiver storage");
    XrStableId slot_identity;
    if (!make_slot_identity(plan, parameter->function, XR_TARGET_SLOT_PARAMETER, parameter->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "class receiver slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = parameter->function,
        .semantic_value = parameter->value,
        .semantic_operation = XR_SEMANTIC_INDEX_NONE,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_PARAMETER,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = owned ? XR_TARGET_OWNERSHIP_OWNED : XR_TARGET_OWNERSHIP_BORROWED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = parameter->value,
        .semantic_function = parameter->function,
        .semantic_type = parameter->type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[parameter->type] &&
         !append_layout_intent(builder, parameter->type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep, error,
                               error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[parameter->value] = 1;
    analysis->used_types[parameter->type] = 1;
    analysis->value_types[parameter->value] = parameter->type;
    analysis->value_functions[parameter->value] = parameter->function;
    analysis->type_rep_kinds[parameter->type] = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* The three parameter shapes are collected by three families rather than one so
 * that a plan states which of them it actually contains; they share one
 * collector because the storage row they bind is the same. */
static bool builder_add_source_class_parameter_family(XrTargetPlanBuilder *builder, uint64_t family,
                                                      uint32_t (*judge)(const XrSemanticPlan *,
                                                                        uint32_t),
                                                      char *error, size_t error_size) {
    if (!builder_begin_family(builder, family, error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    /* A value another family already bound is not this family's to claim. */
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values)
            analysis.defined_values[value->semantic_value] = 1;
    }
    uint32_t parameter_count = (uint32_t) xr_semantic_plan_parameter_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < parameter_count; i++) {
        if (judge(builder->semantic_plan, i) == XR_SEMANTIC_INDEX_NONE)
            continue;
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(builder->semantic_plan, i);
        uint8_t ref_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        /* A class passed by ref is not a tagged parameter value. It is the raw
         * pointer to the caller's tagged cell, so the ref-boundary family owns
         * its binding after all value producers have been collected. */
        if (semantic_direct_local_tagged_ref_parameter_is_exact(builder->semantic_plan, parameter,
                                                                &ref_storage))
            continue;
        valid = note_source_class_parameter_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= family;
    return true;
}

static bool builder_add_source_class_receiver_storage(XrTargetPlanBuilder *builder, char *error,
                                                      size_t error_size) {
    return builder_add_source_class_parameter_family(
        builder, XR_TARGET_FAMILY_SOURCE_CLASS_RECEIVER_STORAGE,
        xr_semantic_class_constructor_receiver_source_class, error, error_size);
}

static bool builder_add_source_class_method_receiver_storage(XrTargetPlanBuilder *builder,
                                                             char *error, size_t error_size) {
    return builder_add_source_class_parameter_family(
        builder, XR_TARGET_FAMILY_SOURCE_CLASS_METHOD_RECEIVER_STORAGE,
        xr_semantic_class_method_receiver_source_class, error, error_size);
}

static bool builder_add_source_class_argument_storage(XrTargetPlanBuilder *builder, char *error,
                                                      size_t error_size) {
    return builder_add_source_class_parameter_family(
        builder, XR_TARGET_FAMILY_SOURCE_CLASS_ARGUMENT_STORAGE,
        xr_semantic_class_argument_source_class, error, error_size);
}

/* Binds the String a concatenation allocates to its own owned dynamic slot.
 * String is immutable and shared, so the outer tagged value is the whole
 * storage fact, exactly as it is for a String literal and for the owned String
 * a direct-local call returns. The join consumes every operand, so this row
 * states no borrow of its own. */
static bool builder_add_string_concat_result_storage(XrTargetPlanBuilder *builder, char *error,
                                                     size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_STRING_CONCAT_RESULT_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    /* A value another family already bound is not this family's to claim. */
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values)
            analysis.defined_values[value->semantic_value] = 1;
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        if (!xr_semantic_string_concat_is_exact(builder->semantic_plan, operation))
            continue;
        if (operation->result_value >= analysis.total_values ||
            operation->result_type >= analysis.type_count ||
            operation->function >= xr_semantic_plan_function_count(builder->semantic_plan) ||
            analysis.defined_values[operation->result_value]) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "string concatenation storage identity is ambiguous");
            break;
        }
        if (analysis.type_rep_kinds[operation->result_type] != XR_MACHINE_REP_COUNT &&
            analysis.type_rep_kinds[operation->result_type] != XR_MACHINE_REP_DYN_VALUE) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "semantic string type has conflicting storage representations");
            break;
        }
        XrTargetMachineRepRecord rep;
        XrStableId slot_identity;
        valid = make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) &&
                append_rep_intent(builder, &rep, error, error_size) &&
                make_slot_identity(builder->semantic_plan, operation->function,
                                   XR_TARGET_SLOT_TEMPORARY, operation->id, XR_SEMANTIC_INDEX_NONE,
                                   &slot_identity);
        if (!valid)
            break;
        XrTargetSlotIntent slot = {
            .identity = slot_identity,
            .function = operation->function,
            .semantic_value = operation->result_value,
            .semantic_operation = i,
            .logical_slot = XR_SEMANTIC_INDEX_NONE,
            .register_rep = rep,
            .memory_rep = rep,
            .role = XR_TARGET_SLOT_TEMPORARY,
            .root_kind = XR_TARGET_ROOT_DYNAMIC,
            .ownership = XR_TARGET_OWNERSHIP_OWNED,
            .debug_variable = XR_SEMANTIC_INDEX_NONE,
        };
        XrTargetValueIntent value = {
            .semantic_value = operation->result_value,
            .semantic_function = operation->function,
            .semantic_type = operation->result_type,
            .register_rep = rep,
            .memory_rep = rep,
            .slot_identity = slot_identity,
            .has_slot = true,
        };
        valid = append_slot_intent(builder, &slot, error, error_size) &&
                (analysis.used_types[operation->result_type] ||
                 append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0,
                                      &rep, error, error_size)) &&
                append_value_intent(builder, &value, error, error_size);
        if (valid) {
            analysis.defined_values[operation->result_value] = 1;
            analysis.used_types[operation->result_type] = 1;
            analysis.value_types[operation->result_value] = operation->result_type;
            analysis.value_functions[operation->result_value] = operation->function;
            analysis.type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
        }
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_STRING_CONCAT_RESULT_STORAGE;
    return true;
}

/* Binds the String a `string(x)` conversion allocates to its own owned dynamic
 * slot, which is the same storage fact a concatenation result, a String literal
 * and the owned String a direct-local call returns already carry: String is
 * immutable and shared, so the outer tagged value is the whole fact. The
 * conversion borrows its scalar source, whose own representation the scalar
 * family froze, so this row states no borrow of its own. */
static bool builder_add_string_convert_result_storage(XrTargetPlanBuilder *builder, char *error,
                                                      size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_STRING_CONVERT_RESULT_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    /* A value another family already bound is not this family's to claim. */
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values)
            analysis.defined_values[value->semantic_value] = 1;
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        if (!xr_semantic_string_convert_is_exact(builder->semantic_plan, operation))
            continue;
        if (operation->result_value >= analysis.total_values ||
            operation->result_type >= analysis.type_count ||
            operation->function >= xr_semantic_plan_function_count(builder->semantic_plan) ||
            analysis.defined_values[operation->result_value]) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "string conversion storage identity is ambiguous");
            break;
        }
        if (analysis.type_rep_kinds[operation->result_type] != XR_MACHINE_REP_COUNT &&
            analysis.type_rep_kinds[operation->result_type] != XR_MACHINE_REP_DYN_VALUE) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "semantic string type has conflicting storage representations");
            break;
        }
        XrTargetMachineRepRecord rep;
        XrStableId slot_identity;
        valid = make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) &&
                append_rep_intent(builder, &rep, error, error_size) &&
                make_slot_identity(builder->semantic_plan, operation->function,
                                   XR_TARGET_SLOT_TEMPORARY, operation->id, XR_SEMANTIC_INDEX_NONE,
                                   &slot_identity);
        if (!valid)
            break;
        XrTargetSlotIntent slot = {
            .identity = slot_identity,
            .function = operation->function,
            .semantic_value = operation->result_value,
            .semantic_operation = i,
            .logical_slot = XR_SEMANTIC_INDEX_NONE,
            .register_rep = rep,
            .memory_rep = rep,
            .role = XR_TARGET_SLOT_TEMPORARY,
            .root_kind = XR_TARGET_ROOT_DYNAMIC,
            .ownership = XR_TARGET_OWNERSHIP_OWNED,
            .debug_variable = XR_SEMANTIC_INDEX_NONE,
        };
        XrTargetValueIntent value = {
            .semantic_value = operation->result_value,
            .semantic_function = operation->function,
            .semantic_type = operation->result_type,
            .register_rep = rep,
            .memory_rep = rep,
            .slot_identity = slot_identity,
            .has_slot = true,
        };
        valid = append_slot_intent(builder, &slot, error, error_size) &&
                (analysis.used_types[operation->result_type] ||
                 append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0,
                                      &rep, error, error_size)) &&
                append_value_intent(builder, &value, error, error_size);
        if (valid) {
            analysis.defined_values[operation->result_value] = 1;
            analysis.used_types[operation->result_type] = 1;
            analysis.value_types[operation->result_value] = operation->result_type;
            analysis.value_functions[operation->result_value] = operation->function;
            analysis.type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
        }
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_STRING_CONVERT_RESULT_STORAGE;
    return true;
}

static bool builder_add_panic_catch_storage(XrTargetPlanBuilder *builder, char *error,
                                            size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_PANIC_CATCH_STORAGE, error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values)
            analysis.defined_values[value->semantic_value] = 1;
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        if (!xr_semantic_panic_catch_is_exact(builder->semantic_plan, operation))
            continue;
        if (operation->result_value >= analysis.total_values ||
            operation->result_type >= analysis.type_count ||
            analysis.defined_values[operation->result_value] ||
            (analysis.type_rep_kinds[operation->result_type] != XR_MACHINE_REP_COUNT &&
             analysis.type_rep_kinds[operation->result_type] != XR_MACHINE_REP_DYN_VALUE)) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "panic-catch storage identity is ambiguous");
            break;
        }
        XrTargetMachineRepRecord rep;
        XrStableId slot_identity;
        valid = make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) &&
                append_rep_intent(builder, &rep, error, error_size) &&
                make_slot_identity(builder->semantic_plan, operation->function,
                                   XR_TARGET_SLOT_TEMPORARY, operation->id, XR_SEMANTIC_INDEX_NONE,
                                   &slot_identity);
        if (!valid)
            break;
        XrTargetSlotIntent slot = {
            .identity = slot_identity,
            .function = operation->function,
            .semantic_value = operation->result_value,
            .semantic_operation = i,
            .logical_slot = XR_SEMANTIC_INDEX_NONE,
            .register_rep = rep,
            .memory_rep = rep,
            .role = XR_TARGET_SLOT_TEMPORARY,
            .root_kind = XR_TARGET_ROOT_DYNAMIC,
            .ownership = XR_TARGET_OWNERSHIP_OWNED,
            .debug_variable = XR_SEMANTIC_INDEX_NONE,
        };
        XrTargetValueIntent value = {
            .semantic_value = operation->result_value,
            .semantic_function = operation->function,
            .semantic_type = operation->result_type,
            .register_rep = rep,
            .memory_rep = rep,
            .slot_identity = slot_identity,
            .has_slot = true,
        };
        valid = append_slot_intent(builder, &slot, error, error_size) &&
                (analysis.used_types[operation->result_type] ||
                 append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0,
                                      &rep, error, error_size)) &&
                append_value_intent(builder, &value, error, error_size);
        if (valid) {
            analysis.defined_values[operation->result_value] = 1;
            analysis.used_types[operation->result_type] = 1;
            analysis.value_types[operation->result_value] = operation->result_type;
            analysis.value_functions[operation->result_value] = operation->function;
            analysis.type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
        }
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_PANIC_CATCH_STORAGE;
    return true;
}

static bool builder_add_string_literal_storage(XrTargetPlanBuilder *builder, char *error,
                                               size_t error_size) {
    if (!builder_begin_family(builder,
                              XR_TARGET_FAMILY_STRING_LITERAL_STORAGE |
                                  XR_TARGET_FAMILY_BIGINT_VALUE_STORAGE,
                              error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        if (xr_semantic_string_literal_is_exact(builder->semantic_plan, operation))
            valid = note_string_literal_storage_value(builder, &analysis, i, error, error_size);
        else if (xr_semantic_bigint_value_is_exact(builder->semantic_plan, operation))
            valid = note_bigint_value_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |=
        XR_TARGET_FAMILY_STRING_LITERAL_STORAGE | XR_TARGET_FAMILY_BIGINT_VALUE_STORAGE;
    return true;
}

static bool builder_add_string_byte_slice_view_storage(XrTargetPlanBuilder *builder, char *error,
                                                       size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_STRING_BYTE_SLICE_VIEW_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    size_t parameter_count = xr_semantic_plan_parameter_count(builder->semantic_plan);
    for (uint32_t i = 0; i < (uint32_t) parameter_count && valid; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(builder->semantic_plan, i);
        if (!semantic_u8_slice_parameter_is_exact(builder->semantic_plan, parameter))
            continue;
        valid = note_u8_slice_view_parameter_storage_value(builder, &analysis, parameter, error,
                                                           error_size);
    }
    size_t operation_count = xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; i < (uint32_t) operation_count && valid; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_STRING_BYTE_SLICE_VIEW)
            continue;
        valid = note_string_byte_slice_view_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_STRING_BYTE_SLICE_VIEW_STORAGE;
    return true;
}

/* The view `container[start:end]` produces. It borrows part of a container it
 * did not allocate, so it takes the same two-word VIEW pair a borrowed Slice
 * parameter carries and never a tagged carrier: there is no allocation behind
 * it to hold. The container operand keeps whatever carrier its own family bound,
 * which is what leaves this row adapter-free. */
static bool note_range_slice_view_storage_value(XrTargetPlanBuilder *builder,
                                                XrTargetValueStorageAnalysis *analysis,
                                                uint32_t semantic_operation, char *error,
                                                size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    if (!xr_semantic_range_slice_is_exact(builder->semantic_plan, operation, NULL) ||
        operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >= xr_semantic_plan_function_count(builder->semantic_plan))
        return fail(error, error_size, "XR_TARGET_1001",
                    "range slice view storage requires exact authority");
    if (analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1001", "range slice view storage is duplicated");
    XrTargetMachineRepRecord rep;
    if (!make_string_byte_slice_view_rep(xr_target_profile_machine_facts(builder->profile), &rep))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize range slice view ABI");
    rep.detail = operation->result_type;
    if (!append_rep_intent(builder, &rep, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "range slice view slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = rep.root_kind,
        .ownership = rep.ownership,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    return append_slot_intent(builder, &slot, error, error_size) &&
           append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_VIEW, 0, &rep,
                                error, error_size) &&
           append_value_intent(builder, &value, error, error_size);
}

static bool builder_add_range_slice_view_storage(XrTargetPlanBuilder *builder, char *error,
                                                 size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_RANGE_SLICE_VIEW_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    size_t operation_count = xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; i < (uint32_t) operation_count && valid; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation ||
            !xr_semantic_range_slice_is_exact(builder->semantic_plan, operation, NULL))
            continue;
        valid = note_range_slice_view_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_RANGE_SLICE_VIEW_STORAGE;
    return true;
}

static bool builder_add_stringbuilder_append_rune_storage(XrTargetPlanBuilder *builder, char *error,
                                                          size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_STRINGBUILDER_APPEND_RUNE_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values) {
            analysis.defined_values[value->semantic_value] = 1;
            analysis.value_types[value->semantic_value] = value->semantic_type;
            analysis.value_functions[value->semantic_value] = value->semantic_function;
        }
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_RUNE)
            valid = note_stringbuilder_append_rune_storage_value(builder, &analysis, i, error,
                                                                 error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_STRINGBUILDER_APPEND_RUNE_STORAGE;
    return true;
}

static bool builder_add_string_runes_storage(XrTargetPlanBuilder *builder, char *error,
                                             size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_STRING_RUNES_RESULT_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values) {
            analysis.defined_values[value->semantic_value] = 1;
            analysis.value_types[value->semantic_value] = value->semantic_type;
            analysis.value_functions[value->semantic_value] = value->semantic_function;
            analysis.used_types[value->semantic_type] = 1;
        }
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_STRING_RUNES)
            valid = note_string_runes_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_STRING_RUNES_RESULT_STORAGE;
    return true;
}

static bool builder_add_string_slice_range_storage(XrTargetPlanBuilder *builder, char *error,
                                                   size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_STRING_SLICE_RANGE_RESULT_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values) {
            analysis.defined_values[value->semantic_value] = 1;
            analysis.value_types[value->semantic_value] = value->semantic_type;
            analysis.value_functions[value->semantic_value] = value->semantic_function;
            analysis.used_types[value->semantic_type] = 1;
        }
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_STRING_SLICE_RANGE)
            valid = note_string_slice_range_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_STRING_SLICE_RANGE_RESULT_STORAGE;
    return true;
}

static bool builder_add_rune_to_string_storage(XrTargetPlanBuilder *builder, char *error,
                                               size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_RUNE_TO_STRING_RESULT_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values) {
            analysis.defined_values[value->semantic_value] = 1;
            analysis.value_types[value->semantic_value] = value->semantic_type;
            analysis.value_functions[value->semantic_value] = value->semantic_function;
            analysis.used_types[value->semantic_type] = 1;
        }
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_RUNE_TO_STRING)
            valid = note_rune_to_string_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_RUNE_TO_STRING_RESULT_STORAGE;
    return true;
}

static bool builder_add_stringbuilder_to_string_storage(XrTargetPlanBuilder *builder, char *error,
                                                        size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_STRINGBUILDER_TO_STRING_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values) {
            analysis.defined_values[value->semantic_value] = 1;
            analysis.used_types[value->semantic_type] = 1;
        }
    }
    uint32_t count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_STRINGBUILDER_TO_STRING)
            valid = note_stringbuilder_to_string_storage_value(builder, &analysis, i, error,
                                                               error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_STRINGBUILDER_TO_STRING_STORAGE;
    return true;
}

static bool builder_add_json_namespace_value_storage(XrTargetPlanBuilder *builder, char *error,
                                                     size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_JSON_NAMESPACE_VALUE_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values) {
            analysis.defined_values[value->semantic_value] = 1;
            analysis.used_types[value->semantic_type] = 1;
        }
    }
    uint32_t count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_JSON_NAMESPACE_VALUE)
            valid =
                note_json_namespace_value_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_JSON_NAMESPACE_VALUE_STORAGE;
    return true;
}

static bool note_dynamic_value_storage_value(XrTargetPlanBuilder *builder,
                                             XrTargetValueStorageAnalysis *analysis,
                                             uint32_t semantic_operation, char *error,
                                             size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    bool exact_dynamic = xr_semantic_dynamic_value_is_exact(builder->semantic_plan, operation);
    bool exact_map_iterator_result =
        xr_semantic_map_entries_iterator_is_exact(builder->semantic_plan, operation, NULL, NULL) ||
        xr_semantic_map_entry_iterator_next_is_exact(builder->semantic_plan, operation, NULL);
    if ((!exact_dynamic && !exact_map_iterator_result) ||
        operation->result_value >= analysis->total_values)
        return fail(error, error_size, "XR_TARGET_1001", "dynamic value authority is incomplete");
    /* A value an earlier family already bound is not this family's to claim:
     * a producer some other judgement could narrow belongs to that judgement. */
    if (analysis->defined_values[operation->result_value])
        return true;
    bool borrowed = exact_dynamic && xr_semantic_dynamic_value_is_borrowed(operation);
    XrTargetMachineRepRecord rep;
    if (!(borrowed
              ? make_borrowed_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile),
                                                &rep)
              : make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep)) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize a dynamic value");
    XrTargetSlotRole role = exact_dynamic && xr_semantic_dynamic_value_is_join(operation)
                                ? XR_TARGET_SLOT_PHI
                                : XR_TARGET_SLOT_TEMPORARY;
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, role, operation->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "dynamic value slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = role,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = borrowed ? XR_TARGET_OWNERSHIP_BORROWED : XR_TARGET_OWNERSHIP_OWNED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                              error, error_size) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

/* `copy(c)` over a container materialises a fresh owned Array, so its result is
 * an allocation like any other this family binds -- the note function above
 * states the whole row, and this sweep only names which operations to ask it
 * about. */
/* An owner transfer takes the representation its source already proved, the
 * same way an identity copy does: the two differ in who owns the result, not
 * in what is held. Propagates rather than decides, so it runs after the
 * families that determine storage and declines a transfer whose source nothing
 * claimed. */
static bool builder_add_owner_forward_storage(XrTargetPlanBuilder *builder, char *error,
                                              size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_OWNER_FORWARD_STORAGE, error, error_size))
        return false;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    bool valid = true;
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        uint32_t source_value = XR_SEMANTIC_INDEX_NONE;
        if (!operation ||
            !xr_semantic_owner_forward_is_exact(builder->semantic_plan, operation, &source_value))
            continue;
        const XrTargetValueIntent *source_intent = NULL;
        bool result_claimed = false;
        for (uint32_t v = 0; v < builder->value_intent_count; v++) {
            uint32_t claimed = builder->value_intents[v].semantic_value;
            if (claimed == operation->result_value)
                result_claimed = true;
            if (claimed == source_value)
                source_intent = &builder->value_intents[v];
        }
        if (result_claimed || !source_intent)
            continue;
        /* The result names its own semantic type, which need not be the one the
         * source named even though both hold the same thing. A binding without
         * a layout for its type is refused downstream, so the transfer states
         * one, carrying the source's memory representation. */
        if (!append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0,
                                  &source_intent->memory_rep, error, error_size)) {
            valid = false;
            break;
        }
        XrStableId slot_identity;
        if (!make_slot_identity(builder->semantic_plan, operation->function,
                                XR_TARGET_SLOT_TEMPORARY, operation->id, XR_SEMANTIC_INDEX_NONE,
                                &slot_identity)) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "owner forward slot identity is incomplete");
            break;
        }
        XrTargetSlotIntent slot = {
            .identity = slot_identity,
            .function = operation->function,
            .semantic_value = operation->result_value,
            .semantic_operation = i,
            .logical_slot = XR_SEMANTIC_INDEX_NONE,
            .register_rep = source_intent->register_rep,
            .memory_rep = source_intent->memory_rep,
            .role = XR_TARGET_SLOT_TEMPORARY,
            .root_kind = source_intent->memory_rep.root_kind,
            .ownership = source_intent->memory_rep.ownership,
            .debug_variable = XR_SEMANTIC_INDEX_NONE,
        };
        if (!append_slot_intent(builder, &slot, error, error_size)) {
            valid = false;
            break;
        }
        XrTargetValueIntent intent = {
            .semantic_value = operation->result_value,
            .semantic_function = operation->function,
            .semantic_type = operation->result_type,
            .register_rep = source_intent->register_rep,
            .memory_rep = source_intent->memory_rep,
            .slot_identity = slot_identity,
            .has_slot = true,
            .resolve_type_rep = false,
        };
        valid = append_value_intent(builder, &intent, error, error_size);
    }
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_OWNER_FORWARD_STORAGE;
    return true;
}

static bool builder_add_identity_copy_storage(XrTargetPlanBuilder *builder, char *error,
                                              size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_IDENTITY_COPY_STORAGE, error, error_size))
        return false;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    bool valid = true;
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        uint32_t source_value = XR_SEMANTIC_INDEX_NONE;
        if (!operation ||
            !xr_semantic_identity_copy_is_exact(builder->semantic_plan, operation, &source_value))
            continue;
        const XrTargetValueIntent *source_intent = NULL;
        bool result_claimed = false;
        for (uint32_t v = 0; v < builder->value_intent_count; v++) {
            const XrTargetValueIntent *candidate = &builder->value_intents[v];
            if (candidate->semantic_value == operation->result_value) {
                result_claimed = true;
                break;
            }
            if (candidate->semantic_value == source_value)
                source_intent = candidate;
        }
        if (result_claimed || !source_intent)
            continue;
        /* A slot belongs to exactly one value and its identity is rebuilt from
         * that value, so the result cannot name the source's slot even though
         * it holds the same thing. It gets its own, carrying the source's
         * representation. */
        XrStableId slot_identity;
        if (!make_slot_identity(builder->semantic_plan, operation->function,
                                XR_TARGET_SLOT_TEMPORARY, operation->id, XR_SEMANTIC_INDEX_NONE,
                                &slot_identity)) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "identity copy slot identity is incomplete");
            break;
        }
        XrTargetSlotIntent slot = {
            .identity = slot_identity,
            .function = operation->function,
            .semantic_value = operation->result_value,
            .semantic_operation = i,
            .logical_slot = XR_SEMANTIC_INDEX_NONE,
            .register_rep = source_intent->register_rep,
            .memory_rep = source_intent->memory_rep,
            .role = XR_TARGET_SLOT_TEMPORARY,
            .root_kind = source_intent->memory_rep.root_kind,
            .ownership = source_intent->memory_rep.ownership,
            .debug_variable = XR_SEMANTIC_INDEX_NONE,
        };
        if (!append_slot_intent(builder, &slot, error, error_size)) {
            valid = false;
            break;
        }
        XrTargetValueIntent intent = {
            .semantic_value = operation->result_value,
            .semantic_function = operation->function,
            .semantic_type = operation->result_type,
            .register_rep = source_intent->register_rep,
            .memory_rep = source_intent->memory_rep,
            .slot_identity = slot_identity,
            .has_slot = true,
            .resolve_type_rep = false,
        };
        valid = append_value_intent(builder, &intent, error, error_size);
    }
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_IDENTITY_COPY_STORAGE;
    return true;
}

static bool builder_add_container_copy_result_storage(XrTargetPlanBuilder *builder, char *error,
                                                      size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_CONTAINER_COPY_RESULT_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values) {
            analysis.defined_values[value->semantic_value] = 1;
            analysis.used_types[value->semantic_type] = 1;
        }
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation ||
            !xr_semantic_container_copy_is_exact(builder->semantic_plan, operation, NULL, NULL))
            continue;
        valid = note_array_allocation_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_CONTAINER_COPY_RESULT_STORAGE;
    return true;
}

/* The address of a local is a borrowed raw pointer to the subject's storage.
 * It is bound here rather than by the scalar family because its semantic type
 * describes the subject, not the address -- see the shape header. The layout it
 * records is the pointer's own, not the subject's: what the address points at
 * is a fact the emitter reads from the source value, which keeps its own row. */
static bool note_local_address_storage_value(XrTargetPlanBuilder *builder,
                                             XrTargetValueStorageAnalysis *analysis,
                                             uint32_t semantic_operation, char *error,
                                             size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    if (!xr_semantic_local_addr_is_exact(builder->semantic_plan, operation, NULL) ||
        operation->result_value >= analysis->total_values)
        return fail(error, error_size, "XR_TARGET_1001", "local address authority is incomplete");
    if (analysis->defined_values[operation->result_value])
        return true;
    XrTargetMachineRepRecord rep;
    if (!make_machine_rep(xr_target_profile_machine_facts(builder->profile), XR_MACHINE_REP_RAW_PTR,
                          &rep))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize a local address");
    /* The address is a bare pointer into a frame the function already owns: it
     * roots nothing, frees nothing, and outlives nothing. The borrowed spelling
     * is reserved for a ref parameter, whose lifetime the caller proves. */
    rep.ownership = XR_TARGET_OWNERSHIP_TRIVIAL;
    rep.root_kind = XR_TARGET_ROOT_NONE;
    if (!append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize a local address");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "local address slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_NONE,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool builder_add_local_address_storage(XrTargetPlanBuilder *builder, char *error,
                                              size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_LOCAL_ADDRESS_STORAGE, error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values) {
            analysis.defined_values[value->semantic_value] = 1;
            analysis.used_types[value->semantic_type] = 1;
        }
    }
    uint32_t count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (xr_semantic_local_addr_is_exact(builder->semantic_plan, operation, NULL))
            valid = note_local_address_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_LOCAL_ADDRESS_STORAGE;
    return true;
}

static bool builder_add_dynamic_value_storage(XrTargetPlanBuilder *builder, char *error,
                                              size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_DYNAMIC_VALUE_STORAGE, error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values) {
            analysis.defined_values[value->semantic_value] = 1;
            analysis.used_types[value->semantic_type] = 1;
        }
    }
    uint32_t count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (semantic_stringbuilder_constructor_is_exact(builder->semantic_plan, operation))
            valid = note_stringbuilder_constructor_storage_value(builder, &analysis, i, error,
                                                                 error_size);
        else if (xr_semantic_dynamic_value_is_exact(builder->semantic_plan, operation) ||
                 xr_semantic_map_entries_iterator_is_exact(builder->semantic_plan, operation, NULL,
                                                           NULL) ||
                 xr_semantic_map_entry_iterator_next_is_exact(builder->semantic_plan, operation,
                                                              NULL))
            valid = note_dynamic_value_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_DYNAMIC_VALUE_STORAGE;
    return true;
}

static bool builder_add_panic_info_constructor_storage(XrTargetPlanBuilder *builder, char *error,
                                                       size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_PANIC_INFO_CONSTRUCTOR_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values) {
            analysis.defined_values[value->semantic_value] = 1;
            analysis.used_types[value->semantic_type] = 1;
        }
    }
    uint32_t count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_PANIC_INFO_CONSTRUCTOR)
            valid =
                note_panic_info_constructor_storage_value(builder, &analysis, i, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_PANIC_INFO_CONSTRUCTOR_STORAGE;
    return true;
}

static bool builder_add_stringbuilder_append_string_storage(XrTargetPlanBuilder *builder,
                                                            char *error, size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_STRINGBUILDER_APPEND_STRING_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++)
        if (builder->value_intents[i].semantic_value < analysis.total_values)
            analysis.defined_values[builder->value_intents[i].semantic_value] = 1;
    uint32_t count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!semantic_stringbuilder_append_string_is_exact(builder->semantic_plan, operation, NULL,
                                                           NULL))
            continue;
        if (operation->result_value >= analysis.total_values ||
            analysis.defined_values[operation->result_value]) {
            valid = false;
            break;
        }
        XrTargetMachineRepRecord rep;
        XrStableId slot_identity;
        bool borrowed = operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED;
        valid = make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep);
        if (valid)
            rep.ownership = borrowed ? XR_TARGET_OWNERSHIP_BORROWED : XR_TARGET_OWNERSHIP_OWNED;
        valid = valid && append_rep_intent(builder, &rep, error, error_size) &&
                make_slot_identity(builder->semantic_plan, operation->function,
                                   XR_TARGET_SLOT_TEMPORARY, operation->id, XR_SEMANTIC_INDEX_NONE,
                                   &slot_identity);
        XrTargetSlotIntent slot = {.identity = slot_identity,
                                   .function = operation->function,
                                   .semantic_value = operation->result_value,
                                   .semantic_operation = i,
                                   .logical_slot = XR_SEMANTIC_INDEX_NONE,
                                   .register_rep = rep,
                                   .memory_rep = rep,
                                   .role = XR_TARGET_SLOT_TEMPORARY,
                                   .root_kind = XR_TARGET_ROOT_DYNAMIC,
                                   .ownership = borrowed ? XR_TARGET_OWNERSHIP_BORROWED
                                                         : XR_TARGET_OWNERSHIP_OWNED,
                                   .debug_variable = XR_SEMANTIC_INDEX_NONE};
        XrTargetValueIntent value = {.semantic_value = operation->result_value,
                                     .semantic_function = operation->function,
                                     .semantic_type = operation->result_type,
                                     .register_rep = rep,
                                     .memory_rep = rep,
                                     .slot_identity = slot_identity,
                                     .has_slot = true};
        valid = valid && append_slot_intent(builder, &slot, error, error_size) &&
                append_value_intent(builder, &value, error, error_size);
        if (valid)
            analysis.defined_values[operation->result_value] = 1;
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.append(string) storage authority is incomplete");
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_STRINGBUILDER_APPEND_STRING_STORAGE;
    return true;
}

/* Which carrier a String occupies at a direct-local call boundary. Both sides
 * of the boundary hold the same outer XrValue -- a String has no other storage
 * fact -- so the carrier records only who is answerable for the allocation. The
 * slot role cannot answer that on its own, which is why it is named here. */
typedef enum XrTargetStringBoundaryCarrier {
    /* The tagged value, owned: the callee transferred it and the caller must
     * release it. */
    XR_TARGET_STRING_CARRIER_OWNED_VALUE = 0,
    /* The tagged value, borrowed: the callee reads the caller's allocation for
     * the extent of the call and releases nothing. */
    XR_TARGET_STRING_CARRIER_BORROWED_VALUE,
} XrTargetStringBoundaryCarrier;

/* Bind a String crossing a direct-local call boundary to its outer XrValue
 * slot. This states storage and ownership only; allocation, roots, and cleanup
 * stay with Semantic ownership and the existing AOT lifetime path, exactly as
 * the String literal and StringBuilder result rows already do. */
static bool note_direct_local_string_boundary_storage(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis, uint32_t semantic_value,
    uint32_t semantic_type, uint32_t semantic_function, uint32_t semantic_operation, uint8_t role,
    uint8_t carrier, XrStableId source_identity, char *error, size_t error_size) {
    const XrSemanticPlan *plan = builder->semantic_plan;
    bool parameter = role == XR_TARGET_SLOT_PARAMETER;
    if (semantic_value >= analysis->total_values || semantic_type >= analysis->type_count ||
        semantic_function >= xr_semantic_plan_function_count(plan) ||
        (!parameter && semantic_operation >= xr_semantic_plan_operation_count(plan)) ||
        analysis->defined_values[semantic_value])
        return fail(error, error_size, "XR_TARGET_1001",
                    "direct-local String boundary storage authority is incomplete");
    XrTargetMachineRepRecord rep;
    const XrTargetMachineFacts *facts = xr_target_profile_machine_facts(builder->profile);
    bool rep_exact = carrier == XR_TARGET_STRING_CARRIER_BORROWED_VALUE
                         ? make_borrowed_dynamic_value_rep(facts, &rep)
                         : make_dynamic_value_rep(facts, &rep);
    if (!rep_exact || !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize direct-local String boundary storage");
    XrStableId slot_identity;
    if (!make_slot_identity(plan, semantic_function, role, source_identity, XR_SEMANTIC_INDEX_NONE,
                            &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "direct-local String boundary slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = semantic_function,
        .semantic_value = semantic_value,
        .semantic_operation = parameter ? XR_SEMANTIC_INDEX_NONE : semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = role,
        .root_kind = rep.root_kind,
        .ownership = rep.ownership,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = semantic_value,
        .semantic_function = semantic_function,
        .semantic_type = semantic_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[semantic_type] &&
         !append_layout_intent(builder, semantic_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep, error,
                               error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[semantic_value] = 1;
    analysis->used_types[semantic_type] = 1;
    analysis->value_types[semantic_value] = semantic_type;
    analysis->value_functions[semantic_value] = semantic_function;
    analysis->type_rep_kinds[semantic_type] = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static bool builder_add_direct_local_string_boundary_storage(XrTargetPlanBuilder *builder,
                                                             char *error, size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_DIRECT_LOCAL_STRING_BOUNDARY_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values) {
            analysis.defined_values[value->semantic_value] = 1;
            analysis.used_types[value->semantic_type] = 1;
        }
    }
    uint32_t count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation)
            continue;
        const XrSemanticFunctionRecord *callee =
            semantic_direct_local_callee_for_operation(builder->semantic_plan, i);
        if (!callee || !semantic_direct_local_string_result_is_exact(builder->semantic_plan,
                                                                     operation, callee))
            continue;
        valid = note_direct_local_string_boundary_storage(
            builder, &analysis, operation->result_value, operation->result_type,
            operation->function, i, XR_TARGET_SLOT_TEMPORARY, XR_TARGET_STRING_CARRIER_OWNED_VALUE,
            operation->id, error, error_size);
    }
    /* Every exact shared read of a String, whether or not a call boundary ever
     * looks at it. Binding only the reads an argument happens to reach would
     * leave a String bound to a plain local unstated, which is the shape every
     * program that names a string after binding it has. The read borrows the
     * cell's allocation, so it holds the tagged carrier and owes nothing back. */
    for (uint32_t i = 0; valid && i < count; i++) {
        const XrSemanticOperationRecord *read =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!read || read->result_value >= analysis.total_values ||
            analysis.defined_values[read->result_value] ||
            !xr_semantic_tagged_string_shared_read_is_exact(builder->semantic_plan, read))
            continue;
        valid = note_direct_local_string_boundary_storage(
            builder, &analysis, read->result_value, read->result_type, read->function, i,
            XR_TARGET_SLOT_TEMPORARY, XR_TARGET_STRING_CARRIER_BORROWED_VALUE, read->id, error,
            error_size);
    }
    /* A String parameter passed by value. It holds the same tagged carrier the
     * caller handed over; the declaration says whether the callee borrows that
     * allocation for the extent of the call or holds an owning reference it
     * releases itself, and the carrier records which. */
    uint32_t parameter_count = (uint32_t) xr_semantic_plan_parameter_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(builder->semantic_plan, i);
        bool callee_owns = false;
        if (!xr_semantic_direct_local_string_value_parameter_is_exact(builder->semantic_plan,
                                                                      parameter, &callee_owns) ||
            parameter->value >= analysis.total_values || analysis.defined_values[parameter->value])
            continue;
        valid = note_direct_local_string_boundary_storage(
            builder, &analysis, parameter->value, parameter->type, parameter->function,
            XR_SEMANTIC_INDEX_NONE, XR_TARGET_SLOT_PARAMETER,
            callee_owns ? XR_TARGET_STRING_CARRIER_OWNED_VALUE
                        : XR_TARGET_STRING_CARRIER_BORROWED_VALUE,
            parameter->id, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_DIRECT_LOCAL_STRING_BOUNDARY_STORAGE;
    return true;
}

/* State that every direct-local call returning a value aggregate has caller
 * storage.
 *
 * The slot itself was placed by the aggregate family, which is where a
 * caller-owned aggregate slot belongs -- writing a second placement here would
 * bind the value twice. What this family adds is the authority: a backend
 * reading a call row learns the result is written in place, in a slot the
 * caller already owns, rather than transferred from the callee. Without the
 * family the row would carry an aggregate representation no one had proved.
 *
 * A result the aggregate family declined -- a field that is not itself exact,
 * a recursive layout -- has no binding, and that is refused here rather than
 * left for a backend to discover. */
static bool builder_add_direct_local_aggregate_result_storage(XrTargetPlanBuilder *builder,
                                                              char *error, size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_DIRECT_LOCAL_AGGREGATE_RESULT_STORAGE,
                              error, error_size))
        return false;
    const XrSemanticProgramProvenance *provenance =
        xr_semantic_plan_program_provenance(builder->semantic_plan);
    if (provenance &&
        provenance->program_family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL &&
        !semantic_leaf_program_provenance(builder->semantic_plan)) {
        builder->poisoned = true;
        return fail(error, error_size, "XR_TARGET_1003",
                    "leaf aggregate program provenance is incomplete");
    }
    if (provenance &&
        provenance->program_family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL &&
        !semantic_product_program_provenance(builder->semantic_plan)) {
        builder->poisoned = true;
        return fail(error, error_size, "XR_TARGET_1003",
                    "leaf product program provenance is incomplete");
    }
    uint32_t count =
        semantic_leaf_program_provenance(builder->semantic_plan)
            ? (uint32_t) xr_semantic_plan_program_call_binding_count(builder->semantic_plan)
        : semantic_product_program_provenance(builder->semantic_plan)
            ? (uint32_t) xr_semantic_plan_program_call_binding_count(builder->semantic_plan)
            : 0u;
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticProgramCallBinding *binding =
            xr_semantic_plan_program_call_binding(builder->semantic_plan, i);
        const XrSemanticOperationRecord *operation =
            binding ? xr_semantic_plan_operation(builder->semantic_plan, binding->operation) : NULL;
        const XrSemanticFunctionRecord *callee =
            binding ? xr_semantic_plan_function(builder->semantic_plan, binding->target_function)
                    : NULL;
        if (!binding ||
            (!semantic_leaf_program_direct_call_is_exact(
                 builder->semantic_plan, binding->operation, operation, callee, NULL) &&
             !semantic_product_direct_call_is_exact(
                 builder->semantic_plan, binding->operation, operation, callee, NULL))) {
            builder->poisoned = true;
            return fail(error, error_size, "XR_TARGET_1003",
                        "leaf aggregate program call binding is incomplete");
        }
        bool bound = false;
        for (uint32_t v = 0; !bound && v < builder->value_intent_count; v++)
            bound = builder->value_intents[v].semantic_value == operation->result_value &&
                    builder->value_intents[v].semantic_type == operation->result_type &&
                    builder->value_intents[v].has_slot;
        if (!bound) {
            builder->poisoned = true;
            return fail(error, error_size, "XR_TARGET_1003",
                        "direct-local aggregate call result has no caller storage");
        }
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_DIRECT_LOCAL_AGGREGATE_RESULT_STORAGE;
    return true;
}

static bool note_adt_enum_storage_value(XrTargetPlanBuilder *builder,
                                        XrTargetValueStorageAnalysis *analysis,
                                        uint32_t semantic_value, uint32_t semantic_type,
                                        uint32_t semantic_function, uint32_t semantic_operation,
                                        uint8_t role, XrStableId source_identity, uint8_t ownership,
                                        char *error, size_t error_size) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(builder->semantic_plan, semantic_type);
    if (!xr_semantic_adt_enum_type_is_exact(type) || semantic_value >= analysis->total_values ||
        semantic_type >= analysis->type_count ||
        semantic_function >= xr_semantic_plan_function_count(builder->semantic_plan))
        return fail(error, error_size, "XR_TARGET_1001",
                    "ADT enum storage authority is incomplete");
    if (analysis->defined_values[semantic_value])
        return analysis->value_types[semantic_value] == semantic_type &&
               analysis->value_functions[semantic_value] == semantic_function;
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        (ownership != XR_TARGET_OWNERSHIP_OWNED && ownership != XR_TARGET_OWNERSHIP_BORROWED))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize ADT enum storage");
    rep.ownership = ownership;
    if (!append_rep_intent(builder, &rep, error, error_size))
        return false;
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, semantic_function, role, source_identity,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001", "ADT enum slot identity is incomplete");
    bool parameter = role == XR_TARGET_SLOT_PARAMETER;
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = semantic_function,
        .semantic_value = semantic_value,
        .semantic_operation = parameter ? XR_SEMANTIC_INDEX_NONE : semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = role,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = ownership,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = semantic_value,
        .semantic_function = semantic_function,
        .semantic_type = semantic_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[semantic_type] &&
         !append_layout_intent(builder, semantic_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep, error,
                               error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[semantic_value] = 1;
    analysis->used_types[semantic_type] = 1;
    analysis->value_types[semantic_value] = semantic_type;
    analysis->value_functions[semantic_value] = semantic_function;
    analysis->type_rep_kinds[semantic_type] = XR_MACHINE_REP_DYN_VALUE;
    return true;
}

/* Bind only the exact ADT enum values needed at the constructor and
 * direct-local ABI boundary. Other enum-producing operations remain outside
 * the family until their own immutable producer contract exists. */
static bool builder_add_adt_enum_storage(XrTargetPlanBuilder *builder, char *error,
                                         size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_ADT_ENUM_STORAGE, error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values) {
            analysis.defined_values[value->semantic_value] = 1;
            analysis.used_types[value->semantic_type] = 1;
            analysis.value_types[value->semantic_value] = value->semantic_type;
            analysis.value_functions[value->semantic_value] = value->semantic_function;
        }
    }
    uint32_t parameter_count = (uint32_t) xr_semantic_plan_parameter_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(builder->semantic_plan, i);
        if (!parameter || !xr_semantic_adt_enum_type_is_exact(
                              xr_semantic_plan_type(builder->semantic_plan, parameter->type)))
            continue;
        if ((parameter->ownership != XI_OWN_NONE && parameter->ownership != XI_OWN_OWNED &&
             parameter->ownership != XI_OWN_BORROWED) ||
            parameter->mode != XR_PARAM_READ || parameter->transfer_mode != XR_TRANSFER_SHARE) {
            if (error && error_size)
                snprintf(error, error_size,
                         "XR_TARGET_1001: ADT enum parameter ownership is unsupported "
                         "(ownership=%u mode=%u transfer=%u)",
                         parameter->ownership, parameter->mode, parameter->transfer_mode);
            valid = false;
        } else
            valid = note_adt_enum_storage_value(
                builder, &analysis, parameter->value, parameter->type, parameter->function,
                XR_SEMANTIC_INDEX_NONE, XR_TARGET_SLOT_PARAMETER, parameter->id,
                parameter->ownership == XI_OWN_BORROWED ? XR_TARGET_OWNERSHIP_BORROWED
                                                        : XR_TARGET_OWNERSHIP_OWNED,
                error, error_size);
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        const XrSemanticFunctionRecord *callee =
            semantic_direct_local_callee_for_operation(builder->semantic_plan, i);
        bool constructor =
            xr_semantic_adt_enum_constructor_is_exact(builder->semantic_plan, operation, NULL);
        bool direct_result = xr_semantic_direct_local_adt_enum_result_is_exact(
            builder->semantic_plan, operation, callee);
        if (!constructor && !direct_result)
            continue;
        valid = note_adt_enum_storage_value(builder, &analysis, operation->result_value,
                                            operation->result_type, operation->function, i,
                                            XR_TARGET_SLOT_TEMPORARY, operation->id,
                                            XR_TARGET_OWNERSHIP_OWNED, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_ADT_ENUM_STORAGE;
    return true;
}

static bool builder_add_direct_local_callee_storage(XrTargetPlanBuilder *builder, char *error,
                                                    size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_DIRECT_LOCAL_CALLEE_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis values = {0};
    XrDirectLocalCalleeStorageAnalysis callees = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &values, error, error_size) &&
                 index_value_operations(builder->semantic_plan, &values, error, error_size) &&
                 direct_local_callee_storage_analysis_init(builder->semantic_plan, &values,
                                                           &callees, error, error_size);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        bool candidate = operation->opcode == XI_GET_SHARED &&
                         operation->result_value < callees.value_count &&
                         callees.target_by_value[operation->result_value] != XR_SEMANTIC_INDEX_NONE;
        if (!candidate)
            continue;
        if (!direct_local_callee_storage_value_is_exact(builder->semantic_plan, &callees,
                                                        operation)) {
            const XrSemanticTypeRecord *failed_type =
                xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
            const XrSemanticFunctionRecord *failed_target = xr_semantic_plan_function(
                builder->semantic_plan, callees.target_by_value[operation->result_value]);
            if (error && error_size)
                snprintf(
                    error, error_size,
                    "XR_TARGET_1001: direct-local shared callee authority is incomplete "
                    "(value=%u invalid=%u uses=%u target=%u own=%u provenance=%u:%d:%u "
                    "type=%u:%u:%u:%u:%u flags=%u target-parent=%u params=%u:%u/%zu "
                    "shape=%lld:%u:%u:%u effects=%u/%u allocation=%u)",
                    operation->result_value, callees.invalid_value[operation->result_value],
                    callees.use_count_by_value[operation->result_value],
                    callees.target_by_value[operation->result_value], operation->result_ownership,
                    operation->return_provenance, operation->return_parameter,
                    operation->return_complete, failed_type ? failed_type->kind : UINT16_MAX,
                    failed_type ? failed_type->child_count : UINT32_MAX,
                    failed_type ? failed_type->scalar_rep : UINT16_MAX,
                    failed_type ? failed_type->aggregate_extent : UINT32_MAX,
                    failed_type ? failed_type->aggregate_align : UINT32_MAX,
                    failed_type ? failed_type->flags : UINT16_MAX,
                    failed_target ? failed_target->parent : UINT32_MAX,
                    failed_target ? failed_target->parameter_count : UINT16_MAX,
                    failed_target ? failed_target->parameter_begin : UINT32_MAX,
                    xr_semantic_plan_parameter_count(builder->semantic_plan),
                    (long long) operation->semantic_immediate, operation->operand_count,
                    operation->constant, operation->callable_function, operation->effects,
                    xi_generated_op_effects(XI_GET_SHARED), operation->allocation_key ? 1u : 0u);
            valid = false;
            break;
        }
        valid = note_direct_local_callee_storage_value(builder, &values, &callees, i, error,
                                                       error_size);
    }
    direct_local_callee_storage_analysis_dispose(&callees);
    value_storage_analysis_dispose(&values);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_DIRECT_LOCAL_CALLEE_STORAGE;
    return true;
}

static bool builder_add_direct_local_go_callee_storage(XrTargetPlanBuilder *builder, char *error,
                                                       size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_DIRECT_LOCAL_GO_CALLEE_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis values = {0};
    XrDirectLocalGoCalleeStorageAnalysis callees = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &values, error, error_size) &&
                 index_value_operations(builder->semantic_plan, &values, error, error_size) &&
                 direct_local_go_callee_storage_analysis_init(builder->semantic_plan, &values,
                                                              &callees, error, error_size);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        bool candidate = operation->opcode == XI_GET_SHARED &&
                         operation->result_value < callees.value_count &&
                         callees.candidate_value[operation->result_value];
        if (!candidate)
            continue;
        if (!direct_local_go_callee_storage_value_is_exact(builder->semantic_plan, &callees,
                                                           operation)) {
            if (error && error_size)
                snprintf(
                    error, error_size,
                    "XR_TARGET_1001: direct-local go shared callee authority is incomplete "
                    "(op=%u value=%u invalid=%u uses=%u target=%u shape=%lld:%u own=%u prov=%u)",
                    i, operation->result_value, callees.invalid_value[operation->result_value],
                    callees.use_count_by_value[operation->result_value],
                    callees.target_by_value[operation->result_value],
                    (long long) operation->semantic_immediate, operation->result_type,
                    operation->result_ownership, operation->return_provenance);
            valid = false;
            break;
        }
        valid = note_direct_local_go_callee_storage_value(builder, &values, &callees, i, error,
                                                          error_size);
    }
    direct_local_go_callee_storage_analysis_dispose(&callees);
    value_storage_analysis_dispose(&values);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_DIRECT_LOCAL_GO_CALLEE_STORAGE;
    return true;
}

/* A Task<T> returned by a proved direct-local GO is a borrowed handle owned by
 * the runtime executor. The target plan binds only its tagged carrier and its
 * temporary slot; it must never manufacture an ARC cleanup for the executor's
 * task object. */
static bool builder_add_direct_local_go_task_result_storage(XrTargetPlanBuilder *builder,
                                                            char *error, size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_DIRECT_LOCAL_GO_TASK_RESULT_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis values = {0};
    XrDirectLocalGoCalleeStorageAnalysis callees = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &values, error, error_size) &&
                 index_value_operations(builder->semantic_plan, &values, error, error_size) &&
                 direct_local_go_callee_storage_analysis_init(builder->semantic_plan, &values,
                                                              &callees, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < values.total_values)
            values.defined_values[value->semantic_value] = 1;
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(builder->semantic_plan, &operand_count);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation || operation->operand_begin > operand_count ||
            operation->operand_count > operand_count - operation->operand_begin) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "direct-local GO task storage input is invalid");
            break;
        }
        if (operation->opcode != XI_GO || operation->operand_count == 0)
            continue;
        uint32_t callee_value = operands[operation->operand_begin].value;
        uint32_t callee_operation = callee_value < values.total_values
                                        ? values.value_operations[callee_value]
                                        : XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *callee =
            callee_operation == XR_SEMANTIC_INDEX_NONE
                ? NULL
                : xr_semantic_plan_operation(builder->semantic_plan, callee_operation);
        bool callee_exact = callee && direct_local_go_callee_storage_value_is_exact(
                                          builder->semantic_plan, &callees, callee);
        uint32_t proved_callee = XR_SEMANTIC_INDEX_NONE;
        if (!callee_exact)
            continue;
        if (!xr_semantic_direct_local_go_task_result_is_exact(builder->semantic_plan, operation,
                                                              true, &proved_callee) ||
            proved_callee != callee_value || operation->result_value >= values.total_values ||
            operation->result_type >= values.type_count ||
            operation->function >= xr_semantic_plan_function_count(builder->semantic_plan) ||
            values.defined_values[operation->result_value] ||
            (values.type_rep_kinds[operation->result_type] != XR_MACHINE_REP_COUNT &&
             values.type_rep_kinds[operation->result_type] != XR_MACHINE_REP_DYN_VALUE)) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "direct-local GO task result authority is incomplete");
            break;
        }
        XrTargetMachineRepRecord rep;
        XrStableId slot_identity;
        valid = make_borrowed_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile),
                                                &rep) &&
                append_rep_intent(builder, &rep, error, error_size) &&
                make_slot_identity(builder->semantic_plan, operation->function,
                                   XR_TARGET_SLOT_TEMPORARY, operation->id, XR_SEMANTIC_INDEX_NONE,
                                   &slot_identity);
        if (!valid)
            break;
        XrTargetSlotIntent slot = {
            .identity = slot_identity,
            .function = operation->function,
            .semantic_value = operation->result_value,
            .semantic_operation = i,
            .logical_slot = XR_SEMANTIC_INDEX_NONE,
            .register_rep = rep,
            .memory_rep = rep,
            .role = XR_TARGET_SLOT_TEMPORARY,
            .root_kind = XR_TARGET_ROOT_DYNAMIC,
            .ownership = XR_TARGET_OWNERSHIP_BORROWED,
            .debug_variable = XR_SEMANTIC_INDEX_NONE,
        };
        XrTargetValueIntent value = {
            .semantic_value = operation->result_value,
            .semantic_function = operation->function,
            .semantic_type = operation->result_type,
            .register_rep = rep,
            .memory_rep = rep,
            .slot_identity = slot_identity,
            .has_slot = true,
        };
        valid = append_slot_intent(builder, &slot, error, error_size) &&
                (values.used_types[operation->result_type] ||
                 append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0,
                                      &rep, error, error_size)) &&
                append_value_intent(builder, &value, error, error_size);
        if (valid) {
            values.defined_values[operation->result_value] = 1;
            values.used_types[operation->result_type] = 1;
            values.value_types[operation->result_value] = operation->result_type;
            values.value_functions[operation->result_value] = operation->function;
            values.type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
        }
    }
    direct_local_go_callee_storage_analysis_dispose(&callees);
    value_storage_analysis_dispose(&values);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_DIRECT_LOCAL_GO_TASK_RESULT_STORAGE;
    return true;
}

static bool builder_add_channel_allocation_storage(XrTargetPlanBuilder *builder, char *error,
                                                   size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_CHANNEL_ALLOCATION_STORAGE, error,
                              error_size))
        return false;
    XrTargetValueStorageAnalysis values = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &values, error, error_size) &&
                 index_value_operations(builder->semantic_plan, &values, error, error_size);
    uint8_t *exact =
        valid ? (uint8_t *) allocate_records(values.total_values, sizeof(*exact)) : NULL;
    if (valid && values.total_values && !exact)
        valid = fail(error, error_size, "XR_EXEC_5003",
                     "channel outer-storage analysis allocation failed");
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        bool candidate = operation->opcode == XI_CHAN_NEW || operation->opcode == XI_COPY;
        bool is_allocation =
            semantic_channel_allocation_is_exact(builder->semantic_plan, operation);
        bool is_alias = semantic_channel_identity_copy_is_exact(builder->semantic_plan, operation,
                                                                exact, values.total_values);
        if (operation->opcode == XI_CHAN_NEW && !is_allocation) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "channel allocation authority is incomplete");
            break;
        }
        if (!candidate || (!is_allocation && !is_alias))
            continue;
        exact[operation->result_value] = 1;
        valid =
            note_channel_allocation_storage_value(builder, &values, exact, i, error, error_size);
    }
    xr_free(exact);
    value_storage_analysis_dispose(&values);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_CHANNEL_ALLOCATION_STORAGE;
    return true;
}

static bool builder_add_channel_receive_storage(XrTargetPlanBuilder *builder, char *error,
                                                size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_CHANNEL_RECEIVE_STORAGE, error, error_size))
        return false;
    XrTargetValueStorageAnalysis values = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &values, error, error_size) &&
                 index_value_operations(builder->semantic_plan, &values, error, error_size);
    uint8_t *exact_channels =
        valid ? (uint8_t *) allocate_records(values.total_values, sizeof(*exact_channels)) : NULL;
    if (valid && values.total_values && !exact_channels)
        valid =
            fail(error, error_size, "XR_EXEC_5003", "channel receive analysis allocation failed");
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
            break;
        }
        bool allocation = semantic_channel_allocation_is_exact(builder->semantic_plan, operation);
        bool alias = semantic_channel_identity_copy_is_exact(builder->semantic_plan, operation,
                                                             exact_channels, values.total_values);
        if (allocation || alias)
            exact_channels[operation->result_value] = 1;
        if (operation->opcode != XI_CHAN_TRY_RECV)
            continue;
        uint16_t receive_kind = XR_MACHINE_REP_COUNT;
        if (classify_scalar_type(
                xr_semantic_plan_type(builder->semantic_plan, operation->result_type),
                &receive_kind) != XR_TARGET_SCALAR_VALUE ||
            receive_kind == XR_MACHINE_REP_VOID)
            continue;
        valid = note_channel_receive_storage_value(builder, &values, exact_channels, i, error,
                                                   error_size);
    }
    xr_free(exact_channels);
    value_storage_analysis_dispose(&values);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_CHANNEL_RECEIVE_STORAGE;
    return true;
}

static bool note_source_namespace_storage_value(XrTargetPlanBuilder *builder,
                                                XrTargetValueStorageAnalysis *values,
                                                const XrSourceNamespaceStorageAnalysis *namespaces,
                                                uint32_t semantic_operation, char *error,
                                                size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    if (!operation || operation->result_value >= namespaces->value_count ||
        !namespaces->exact_value[operation->result_value] ||
        (operation->opcode != XI_IMPORT_REF && operation->opcode != XI_GET_SHARED &&
         operation->opcode != XI_COPY) ||
        operation->result_value >= values->total_values ||
        operation->result_type >= values->type_count ||
        operation->function >= xr_semantic_plan_function_count(builder->semantic_plan) ||
        values->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1001",
                    "source namespace storage identity is incomplete");
    XrTargetMachineRepRecord rep;
    if (!make_borrowed_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize source namespace storage");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function, XR_TARGET_SLOT_TEMPORARY,
                            operation->id, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "source namespace slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = operation->function,
        .semantic_value = operation->result_value,
        .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY,
        .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_BORROWED,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value,
        .semantic_function = operation->function,
        .semantic_type = operation->result_type,
        .register_rep = rep,
        .memory_rep = rep,
        .slot_identity = slot_identity,
        .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!values->used_types[operation->result_type] &&
         !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC, 0, &rep,
                               error, error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    values->defined_values[operation->result_value] = 1;
    values->value_types[operation->result_value] = operation->result_type;
    values->value_functions[operation->result_value] = operation->function;
    values->type_rep_kinds[operation->result_type] = XR_MACHINE_REP_DYN_VALUE;
    values->used_types[operation->result_type] = 1;
    return true;
}

static bool source_namespace_type_is_exact(const XrSemanticPlan *plan,
                                           const XrSemanticOperationRecord *operation,
                                           uint16_t opcode) {
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!plan || !operation || !type || operation->opcode != opcode || operation->allocation_key ||
        !stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE || operation->auxiliary_kind != 0 ||
        operation->effects != xi_generated_op_effects(opcode) ||
        operation->flags != xi_generated_op_default_flags(opcode) ||
        operation->ownership_use != xi_generated_op_own_use(opcode) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_parameter != -1 || operation->return_complete != 1 ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    return opcode == XI_IMPORT_REF
               ? operation->operand_count == 0 && operation->semantic_immediate >= -1 &&
                     operation->semantic_immediate <= UINT16_MAX && operation->metadata_count == 2
               : opcode == XI_GET_SHARED && operation->operand_count == 0 &&
                     operation->semantic_immediate >= 0 &&
                     operation->semantic_immediate <= UINT16_MAX && operation->metadata_count == 0;
}

static bool source_import_dependency_is_exact(const XrSemanticPlan *plan,
                                              const XrSemanticOperationRecord *operation,
                                              const char *const *metadata, uint32_t metadata_count,
                                              bool named_export, uint32_t *out_dependency) {
    if (!plan || !operation || !metadata || !out_dependency ||
        !source_namespace_type_is_exact(plan, operation, XI_IMPORT_REF) ||
        operation->function != 0 ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE ||
        operation->metadata_begin + 1u >= metadata_count)
        return false;
    const char *module_path = metadata[operation->metadata_begin];
    const char *member = metadata[operation->metadata_begin + 1u];
    if (!module_path || !module_path[0] || !member || ((member[0] != '\0') != named_export))
        return false;
    uint32_t match = XR_SEMANTIC_INDEX_NONE;
    uint32_t dependency_count = (uint32_t) xr_semantic_plan_dependency_count(plan);
    for (uint32_t i = 0; i < dependency_count; i++) {
        const XrSemanticDependencyRecord *dependency = xr_semantic_plan_dependency(plan, i);
        if (!dependency || !dependency->module_path ||
            strcmp(dependency->module_path, module_path) != 0)
            continue;
        if (match != XR_SEMANTIC_INDEX_NONE)
            return false;
        match = i;
    }
    if (match == XR_SEMANTIC_INDEX_NONE)
        return false;
    *out_dependency = match;
    return true;
}

static bool source_namespace_identity_copy_is_exact(const XrSemanticPlan *plan,
                                                    const XrSemanticOperationRecord *operation,
                                                    const XrSemanticOperandRecord *operands,
                                                    uint32_t operand_count) {
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!plan || !operation || !operands || !type || operation->opcode != XI_COPY ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->semantic_immediate != XI_COPY_KIND_IDENTITY || operation->allocation_key ||
        !stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE || operation->auxiliary_kind != 0 ||
        operation->metadata_count != 0 || operation->effects != xi_generated_op_effects(XI_COPY) ||
        operation->flags != xi_generated_op_default_flags(XI_COPY) ||
        operation->ownership_use != xi_generated_op_own_use(XI_COPY) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_alias_operand != 0 ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_parameter != -1 || operation->return_complete != 1 ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    return source->role == XR_SEM_OPERAND_VALUE && source->parameter == -1 &&
           source->type == operation->result_type && source->flags == 0;
}

static void source_namespace_storage_analysis_dispose(XrSourceNamespaceStorageAnalysis *analysis) {
    if (!analysis)
        return;
    xr_free(analysis->exact_value);
    xr_free(analysis->dependency_by_value);
    memset(analysis, 0, sizeof(*analysis));
}

static bool source_namespace_storage_analysis_init(const XrSemanticPlan *plan,
                                                   const XrTargetValueStorageAnalysis *values,
                                                   XrSourceNamespaceStorageAnalysis *analysis,
                                                   char *error, size_t error_size) {
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    uint32_t target_count = (uint32_t) xr_semantic_plan_call_target_count(plan);
    analysis->value_count = values->total_values;
    analysis->exact_value =
        (uint8_t *) allocate_records(analysis->value_count, sizeof(*analysis->exact_value));
    analysis->dependency_by_value = (uint32_t *) allocate_records(
        analysis->value_count, sizeof(*analysis->dependency_by_value));
    uint32_t *source_target_by_operation =
        (uint32_t *) allocate_records(operation_count, sizeof(*source_target_by_operation));
    uint32_t *expected_uses =
        (uint32_t *) allocate_records(analysis->value_count, sizeof(*expected_uses));
    uint32_t *retain_uses =
        (uint32_t *) allocate_records(analysis->value_count, sizeof(*retain_uses));
    uint32_t *consumer_by_value =
        (uint32_t *) allocate_records(analysis->value_count, sizeof(*consumer_by_value));
    uint32_t *visit_epoch =
        (uint32_t *) allocate_records(analysis->value_count, sizeof(*visit_epoch));
    uint8_t *candidate = (uint8_t *) allocate_records(analysis->value_count, sizeof(*candidate));
    uint8_t *standalone_import =
        (uint8_t *) allocate_records(analysis->value_count, sizeof(*standalone_import));
    if ((analysis->value_count && (!analysis->exact_value || !analysis->dependency_by_value ||
                                   !expected_uses || !retain_uses || !consumer_by_value ||
                                   !visit_epoch || !candidate || !standalone_import)) ||
        (operation_count && !source_target_by_operation)) {
        xr_free(source_target_by_operation);
        xr_free(expected_uses);
        xr_free(retain_uses);
        xr_free(consumer_by_value);
        xr_free(visit_epoch);
        xr_free(candidate);
        xr_free(standalone_import);
        source_namespace_storage_analysis_dispose(analysis);
        return fail(error, error_size, "XR_EXEC_5003",
                    "source namespace storage analysis allocation failed");
    }
    for (uint32_t i = 0; i < analysis->value_count; i++) {
        analysis->dependency_by_value[i] = XR_SEMANTIC_INDEX_NONE;
        consumer_by_value[i] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < operation_count; i++)
        source_target_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(plan, i);
        if (!target || target->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT)
            continue;
        const XrSemanticOperationRecord *call =
            target->operation < operation_count
                ? xr_semantic_plan_operation(plan, target->operation)
                : NULL;
        if (target->operation >= operation_count ||
            target->dependency >= xr_semantic_plan_dependency_count(plan) ||
            source_target_by_operation[target->operation] != XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        source_target_by_operation[target->operation] = i;
    }
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    uint32_t next_epoch = 1;
    for (uint32_t i = 0; i < operation_count; i++) {
        uint32_t target_index = source_target_by_operation[i];
        if (target_index == XR_SEMANTIC_INDEX_NONE)
            continue;
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(plan, target_index);
        const XrSemanticOperationRecord *call = xr_semantic_plan_operation(plan, i);
        bool named_export = call && call->opcode == XI_CALL;
        if (!target || !call || (call->opcode != XI_CALL && call->opcode != XI_CALL_METHOD) ||
            call->operand_count == 0 || call->operand_begin >= operand_count)
            goto invalid;
        const XrSemanticOperandRecord *receiver = &operands[call->operand_begin];
        if (receiver->role != (named_export ? XR_SEM_OPERAND_CALLEE : XR_SEM_OPERAND_RECEIVER) ||
            receiver->parameter != -1 || receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
            receiver->parameter_mode != XR_PARAM_READ || receiver->access != XR_CALL_ARG_PLAIN ||
            receiver->flags != (named_export ? 0u : XR_SEM_OPERAND_CALL_CONTRACT))
            goto invalid;
        const XrSemanticOperationRecord *load = NULL;
        uint32_t current_value = receiver->value;
        uint32_t consumer_index = i;
        uint32_t namespace_type = receiver->type;
        uint32_t epoch = next_epoch++;
        if (epoch == 0) {
            memset(visit_epoch, 0, analysis->value_count * sizeof(*visit_epoch));
            epoch = next_epoch++;
        }
        for (uint32_t depth = 0;; depth++) {
            if (depth >= operation_count || current_value >= analysis->value_count ||
                visit_epoch[current_value] == epoch)
                goto invalid;
            visit_epoch[current_value] = epoch;
            uint32_t definition_index = values->value_operations[current_value];
            const XrSemanticOperationRecord *definition =
                definition_index != XR_SEMANTIC_INDEX_NONE
                    ? xr_semantic_plan_operation(plan, definition_index)
                    : NULL;
            if (!definition || definition->result_value != current_value ||
                definition->result_type != namespace_type ||
                definition->function != call->function ||
                (candidate[current_value] &&
                 (analysis->dependency_by_value[current_value] != target->dependency ||
                  consumer_by_value[current_value] != consumer_index)))
                goto invalid;
            candidate[current_value] = 1;
            analysis->dependency_by_value[current_value] = target->dependency;
            consumer_by_value[current_value] = consumer_index;
            if (source_namespace_type_is_exact(plan, definition, XI_GET_SHARED)) {
                load = definition;
                break;
            }
            if (!source_namespace_identity_copy_is_exact(plan, definition, operands, operand_count))
                goto invalid;
            const XrSemanticOperandRecord *source = &operands[definition->operand_begin];
            consumer_index = definition_index;
            current_value = source->value;
        }
        uint32_t store_index = XR_SEMANTIC_INDEX_NONE;
        for (uint32_t j = 0; j < operation_count; j++) {
            const XrSemanticOperationRecord *candidate_store = xr_semantic_plan_operation(plan, j);
            if (!candidate_store || candidate_store->function != 0 ||
                candidate_store->opcode != XI_SET_SHARED ||
                candidate_store->semantic_immediate != load->semantic_immediate)
                continue;
            if (store_index != XR_SEMANTIC_INDEX_NONE)
                goto invalid;
            store_index = j;
        }
        const XrSemanticOperationRecord *store = store_index != XR_SEMANTIC_INDEX_NONE
                                                     ? xr_semantic_plan_operation(plan, store_index)
                                                     : NULL;
        if (!store || store->operand_count != 1 || store->operand_begin >= operand_count)
            goto invalid;
        const XrSemanticOperandRecord *stored = &operands[store->operand_begin];
        const XrSemanticOperationRecord *import = NULL;
        current_value = stored->value;
        consumer_index = store_index;
        namespace_type = stored->type;
        epoch = next_epoch++;
        if (epoch == 0) {
            memset(visit_epoch, 0, analysis->value_count * sizeof(*visit_epoch));
            epoch = next_epoch++;
        }
        for (uint32_t depth = 0;; depth++) {
            if (depth >= operation_count || current_value >= analysis->value_count ||
                visit_epoch[current_value] == epoch)
                goto invalid;
            visit_epoch[current_value] = epoch;
            uint32_t definition_index = values->value_operations[current_value];
            const XrSemanticOperationRecord *definition =
                definition_index != XR_SEMANTIC_INDEX_NONE
                    ? xr_semantic_plan_operation(plan, definition_index)
                    : NULL;
            if (!definition || definition->result_value != current_value ||
                definition->result_type != namespace_type || definition->function != 0 ||
                (candidate[current_value] &&
                 (analysis->dependency_by_value[current_value] != target->dependency ||
                  consumer_by_value[current_value] != consumer_index)))
                goto invalid;
            candidate[current_value] = 1;
            analysis->dependency_by_value[current_value] = target->dependency;
            consumer_by_value[current_value] = consumer_index;
            if (source_namespace_type_is_exact(plan, definition, XI_IMPORT_REF)) {
                import = definition;
                break;
            }
            if (!source_namespace_identity_copy_is_exact(plan, definition, operands, operand_count))
                goto invalid;
            const XrSemanticOperandRecord *source = &operands[definition->operand_begin];
            consumer_index = definition_index;
            current_value = source->value;
        }
        uint32_t import_dependency = XR_SEMANTIC_INDEX_NONE;
        if (!import || !load || receiver->type != load->result_type || import->function != 0 ||
            load->function != call->function || store->function != 0 ||
            stored->type != import->result_type ||
            store->semantic_immediate != load->semantic_immediate ||
            stored->role != XR_SEM_OPERAND_VALUE || stored->parameter != -1 ||
            stored->ownership_action != XR_SEM_OPERAND_CONSUME ||
            stored->parameter_mode != XR_PARAM_READ || stored->access != XR_CALL_ARG_PLAIN ||
            stored->flags != 0 || load->result_type != import->result_type ||
            !source_import_dependency_is_exact(plan, import, metadata, metadata_count, named_export,
                                               &import_dependency) ||
            import_dependency != target->dependency)
            goto invalid;
    }
    /* A namespace alias that is published but never loaded still needs an
     * exact dynamic representation for ARC.  Its identity is the frozen
     * source dependency plus one unique module-init store; any load would make
     * this a non-standalone lifecycle and is therefore refused here. */
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *import = xr_semantic_plan_operation(plan, i);
        if (!import || import->opcode != XI_IMPORT_REF ||
            import->import_resolution != XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE ||
            import->metadata_count != 2 || import->metadata_begin + 1u >= metadata_count ||
            !metadata || !metadata[import->metadata_begin + 1u] ||
            metadata[import->metadata_begin + 1u][0] != '\0')
            continue;
        if (import->result_value >= analysis->value_count)
            goto invalid;
        if (candidate[import->result_value])
            continue;
        uint32_t dependency = XR_SEMANTIC_INDEX_NONE;
        if (!source_import_dependency_is_exact(plan, import, metadata, metadata_count, false,
                                               &dependency))
            goto invalid;
        uint32_t store_index = XR_SEMANTIC_INDEX_NONE;
        int64_t shared_slot = -1;
        for (uint32_t j = 0; j < operation_count; j++) {
            const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(plan, j);
            if (!operation || operation->opcode != XI_SET_SHARED || operation->function != 0 ||
                operation->operand_count != 1 || operation->operand_begin >= operand_count)
                continue;
            const XrSemanticOperandRecord *stored = &operands[operation->operand_begin];
            if (stored->value != import->result_value)
                continue;
            if (store_index != XR_SEMANTIC_INDEX_NONE || operation->semantic_immediate < 0 ||
                operation->semantic_immediate > UINT16_MAX || stored->type != import->result_type ||
                stored->role != XR_SEM_OPERAND_VALUE || stored->parameter != -1 ||
                stored->ownership_action != XR_SEM_OPERAND_CONSUME ||
                stored->parameter_mode != XR_PARAM_READ || stored->access != XR_CALL_ARG_PLAIN ||
                stored->flags != 0)
                goto invalid;
            store_index = j;
            shared_slot = operation->semantic_immediate;
        }
        if (store_index == XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        for (uint32_t j = 0; j < operation_count; j++) {
            const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(plan, j);
            if (!operation)
                goto invalid;
            if ((operation->opcode == XI_GET_SHARED || operation->opcode == XI_SET_SHARED) &&
                operation->semantic_immediate == shared_slot && j != store_index)
                goto invalid;
        }
        candidate[import->result_value] = 1;
        standalone_import[import->result_value] = 1;
        analysis->dependency_by_value[import->result_value] = dependency;
        consumer_by_value[import->result_value] = store_index;
    }
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(plan, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            goto invalid;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand = &operands[use->operand_begin + a];
            if (operand->value >= analysis->value_count || !candidate[operand->value])
                continue;
            uint32_t definition_index = values->value_operations[operand->value];
            const XrSemanticOperationRecord *definition =
                xr_semantic_plan_operation(plan, definition_index);
            bool expected = i == consumer_by_value[operand->value] && a == 0;
            if (expected && (use->opcode == XI_CALL || use->opcode == XI_CALL_METHOD)) {
                uint32_t target_index = source_target_by_operation[i];
                const XrSemanticCallTargetRecord *target =
                    target_index != XR_SEMANTIC_INDEX_NONE
                        ? xr_semantic_plan_call_target(plan, target_index)
                        : NULL;
                expected = target && target->operation == i &&
                           target->dependency == analysis->dependency_by_value[operand->value] &&
                           operand->role == (use->opcode == XI_CALL ? XR_SEM_OPERAND_CALLEE
                                                                    : XR_SEM_OPERAND_RECEIVER);
            } else if (expected) {
                expected = (use->opcode == XI_COPY || use->opcode == XI_SET_SHARED) &&
                           operand->role == XR_SEM_OPERAND_VALUE;
            }
            if (expected) {
                if (expected_uses[operand->value] != 0)
                    goto invalid;
                expected_uses[operand->value] = 1;
                continue;
            }
            bool retain =
                definition && definition->opcode == XI_IMPORT_REF && use->opcode == XI_RETAIN &&
                a == 0 && use->function == definition->function &&
                operand->role == XR_SEM_OPERAND_VALUE && operand->type == definition->result_type &&
                operand->parameter == -1 && operand->flags == 0;
            if (!retain || retain_uses[operand->value] != 0)
                goto invalid;
            retain_uses[operand->value] = 1;
        }
    }
    uint32_t block_count = (uint32_t) xr_semantic_plan_block_count(plan);
    for (uint32_t i = 0; i < block_count; i++) {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(plan, i);
        if (!block ||
            (block->control_value != XR_SEMANTIC_INDEX_NONE &&
             (block->control_value >= analysis->value_count || candidate[block->control_value])))
            goto invalid;
    }
    for (uint32_t i = 0; i < analysis->value_count; i++) {
        if (!candidate[i])
            continue;
        const XrSemanticOperationRecord *definition =
            xr_semantic_plan_operation(plan, values->value_operations[i]);
        if (!definition || expected_uses[i] != 1 || (standalone_import[i] && retain_uses[i] != 1))
            goto invalid;
        analysis->exact_value[i] = 1;
    }
    xr_free(source_target_by_operation);
    xr_free(expected_uses);
    xr_free(retain_uses);
    xr_free(consumer_by_value);
    xr_free(visit_epoch);
    xr_free(candidate);
    xr_free(standalone_import);
    return true;

invalid:
    xr_free(source_target_by_operation);
    xr_free(expected_uses);
    xr_free(retain_uses);
    xr_free(consumer_by_value);
    xr_free(visit_epoch);
    xr_free(candidate);
    xr_free(standalone_import);
    source_namespace_storage_analysis_dispose(analysis);
    return fail(error, error_size, "XR_TARGET_1001",
                "source namespace storage authority is not exact");
}

static bool builder_add_source_namespace_storage(XrTargetPlanBuilder *builder, char *error,
                                                 size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_SOURCE_IMPORT_STORAGE, error, error_size))
        return false;
    XrTargetValueStorageAnalysis values = {0};
    XrSourceNamespaceStorageAnalysis namespaces = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &values, error, error_size) &&
                 index_value_operations(builder->semantic_plan, &values, error, error_size) &&
                 source_namespace_storage_analysis_init(builder->semantic_plan, &values,
                                                        &namespaces, error, error_size);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation || operation->result_value >= namespaces.value_count)
            continue;
        if (!namespaces.exact_value[operation->result_value])
            continue;
        valid = note_source_namespace_storage_value(builder, &values, &namespaces, i, error,
                                                    error_size);
    }
    source_namespace_storage_analysis_dispose(&namespaces);
    value_storage_analysis_dispose(&values);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_SOURCE_IMPORT_STORAGE;
    return true;
}

static bool fixed_array_element_count(const XrSemanticPlan *plan, uint32_t semantic_type,
                                      uint32_t *out, char *error, size_t error_size) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, semantic_type);
    if (!type || type->kind != XR_KIND_FIXED_ARRAY || type->child_count != 1 ||
        type->aggregate_extent == 0 || type->aggregate_extent > UINT16_MAX)
        return fail(error, error_size, "XR_TARGET_1002",
                    "fixed-array extent is outside the exact semantic field budget");
    *out = type->aggregate_extent;
    return true;
}

static int find_layout_intent(const XrTargetPlanBuilder *builder, uint32_t semantic_type) {
    for (uint32_t i = 0; i < builder->layout_intent_count; i++)
        if (builder->layout_intents[i].semantic_type == semantic_type)
            return (int) i;
    return -1;
}

static int aggregate_layout_eligibility(const XrSemanticPlan *plan, uint32_t semantic_type,
                                        int8_t *states) {
    if (semantic_type >= xr_semantic_plan_type_count(plan))
        return -1;
    if (states[semantic_type] != 0)
        return states[semantic_type] == -2 ? -1 : states[semantic_type];
    states[semantic_type] = -2;
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, semantic_type);
    uint16_t scalar_kind = XR_MACHINE_REP_COUNT;
    XrTargetScalarEligibility scalar = classify_scalar_type(type, &scalar_kind);
    if (scalar == XR_TARGET_SCALAR_INVALID)
        return states[semantic_type] = -1;
    if (scalar == XR_TARGET_SCALAR_VALUE)
        return states[semantic_type] = scalar_kind == XR_MACHINE_REP_VOID ? 2 : 1;
    const XrSemanticProgramTypeBinding *leaf_binding = NULL;
    XrTargetLeafProgramTypeKind leaf_kind =
        semantic_leaf_program_type_kind(plan, semantic_type, &leaf_binding);
    if (leaf_kind == XR_TARGET_LEAF_PROGRAM_TYPE_INVALID)
        return states[semantic_type] = -1;
    if (leaf_kind == XR_TARGET_LEAF_PROGRAM_TYPE_AGGREGATE) {
        for (uint32_t i = 0; i < leaf_binding->field_count; i++) {
            uint32_t child_type = XR_SEMANTIC_INDEX_NONE;
            if (!semantic_leaf_program_field_type(plan, leaf_binding, i, &child_type))
                return states[semantic_type] = -1;
            int child = aggregate_layout_eligibility(plan, child_type, states);
            if (child < 0)
                return states[semantic_type] = -1;
            if (child == 2)
                return states[semantic_type] = 2;
        }
        return states[semantic_type] = 1;
    }
    if (semantic_leaf_program_provenance(plan) && type && type->kind == XR_KIND_INSTANCE &&
        (type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0)
        return states[semantic_type] = -1;
    int aggregate = xr_semantic_aggregate_type_kind(type);
    if (aggregate < 0)
        return states[semantic_type] = -1;
    if (aggregate == 0)
        return states[semantic_type] = 2;
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    if (!type || type->child_begin > child_count ||
        type->child_count > child_count - type->child_begin ||
        (type->kind == XR_KIND_FIXED_ARRAY
             ? (type->child_count != 1 || type->aggregate_extent == 0 ||
                type->aggregate_extent > UINT16_MAX)
             : type->aggregate_extent != type->child_count))
        return states[semantic_type] = -1;
    uint32_t dependencies = type->kind == XR_KIND_FIXED_ARRAY ? 1u : type->child_count;
    for (uint32_t i = 0; i < dependencies; i++) {
        int child = aggregate_layout_eligibility(plan, children[type->child_begin + i], states);
        if (child < 0)
            return states[semantic_type] = -1;
        if (child == 2)
            return states[semantic_type] = 2;
    }
    return states[semantic_type] = 1;
}

static bool collect_layout_dependency(XrTargetPlanBuilder *builder, uint32_t semantic_type,
                                      uint8_t *states, char *error, size_t error_size) {
    if (semantic_type >= xr_semantic_plan_type_count(builder->semantic_plan))
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate field type is outside the semantic type table");
    if (find_layout_intent(builder, semantic_type) >= 0)
        return true;
    if (states[semantic_type] == 1)
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate layout contains an inline recursive cycle");
    if (states[semantic_type] == 2)
        return true;
    states[semantic_type] = 1;
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(builder->semantic_plan, semantic_type);
    uint16_t scalar_kind = XR_MACHINE_REP_COUNT;
    XrTargetScalarEligibility scalar = classify_scalar_type(type, &scalar_kind);
    if (scalar == XR_TARGET_SCALAR_INVALID)
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate field has an invalid scalar type fact");
    if (scalar == XR_TARGET_SCALAR_VALUE && scalar_kind != XR_MACHINE_REP_VOID) {
        XrTargetMachineRepRecord rep;
        if (!make_scalar_type_rep(builder, semantic_type, scalar_kind, &rep) ||
            !append_rep_intent(builder, &rep, error, error_size) ||
            !append_layout_intent(builder, semantic_type, XR_TARGET_LAYOUT_SCALAR, 0, &rep, error,
                                  error_size))
            return false;
        states[semantic_type] = 2;
        return true;
    }
    const XrSemanticProgramTypeBinding *leaf_binding = NULL;
    XrTargetLeafProgramTypeKind leaf_kind =
        semantic_leaf_program_type_kind(builder->semantic_plan, semantic_type, &leaf_binding);
    if (leaf_kind == XR_TARGET_LEAF_PROGRAM_TYPE_INVALID)
        return fail(error, error_size, "XR_TARGET_1002", "leaf aggregate type binding is invalid");
    if (leaf_kind == XR_TARGET_LEAF_PROGRAM_TYPE_AGGREGATE) {
        for (uint32_t i = 0; i < leaf_binding->field_count; i++) {
            uint32_t child_type = XR_SEMANTIC_INDEX_NONE;
            if (!semantic_leaf_program_field_type(builder->semantic_plan, leaf_binding, i,
                                                  &child_type) ||
                !collect_layout_dependency(builder, child_type, states, error, error_size))
                return fail(error, error_size, "XR_TARGET_1002",
                            "leaf aggregate field binding is incomplete");
        }
        XrTargetMachineRepRecord unresolved = {0};
        if (!append_layout_intent(builder, semantic_type, XR_TARGET_LAYOUT_AGGREGATE,
                                  leaf_binding->field_count, &unresolved, error, error_size))
            return false;
        states[semantic_type] = 2;
        return true;
    }
    if (semantic_leaf_program_provenance(builder->semantic_plan) && type &&
        type->kind == XR_KIND_INSTANCE && (type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0)
        return fail(error, error_size, "XR_TARGET_1002",
                    "leaf aggregate lacks a typed program binding");
    int aggregate = xr_semantic_aggregate_type_kind(type);
    if (aggregate <= 0)
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate field lacks an exact supported value layout");
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(builder->semantic_plan, &child_count);
    uint32_t element_count = type->aggregate_extent;
    if (type->kind == XR_KIND_FIXED_ARRAY) {
        if (type->child_count != 1 ||
            !fixed_array_element_count(builder->semantic_plan, semantic_type, &element_count, error,
                                       error_size))
            return false;
    } else if (element_count != type->child_count)
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate extent disagrees with exact semantic fields");
    if (type->child_begin > child_count || type->child_count > child_count - type->child_begin)
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate semantic child range is invalid");
    uint32_t dependencies = type->kind == XR_KIND_FIXED_ARRAY ? 1u : type->child_count;
    for (uint32_t i = 0; i < dependencies; i++)
        if (!collect_layout_dependency(builder, children[type->child_begin + i], states, error,
                                       error_size))
            return false;
    XrTargetMachineRepRecord unresolved = {0};
    if (!append_layout_intent(builder, semantic_type, XR_TARGET_LAYOUT_AGGREGATE, element_count,
                              &unresolved, error, error_size))
        return false;
    states[semantic_type] = 2;
    return true;
}

static bool note_aggregate_value(XrTargetPlanBuilder *builder,
                                 XrTargetValueStorageAnalysis *analysis, uint32_t semantic_value,
                                 uint32_t semantic_type, uint32_t semantic_function,
                                 uint32_t semantic_operation, uint8_t role,
                                 XrStableId source_identity, uint8_t *states, int8_t *eligibility,
                                 char *error, size_t error_size) {
    if (semantic_value >= analysis->total_values || semantic_type >= analysis->type_count ||
        semantic_function >= xr_semantic_plan_function_count(builder->semantic_plan))
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic aggregate value identity is out of range");
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(builder->semantic_plan, semantic_type);
    if (semantic_operation < xr_semantic_plan_operation_count(builder->semantic_plan)) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
        if (operation && operation->opcode < XI_OP_COUNT &&
            xi_generated_op_result_kind(operation->opcode) == XI_GEN_RESULT_VOID)
            return true;
    }
    const XrSemanticProgramTypeBinding *leaf_binding = NULL;
    XrTargetLeafProgramTypeKind leaf_kind =
        semantic_leaf_program_type_kind(builder->semantic_plan, semantic_type, &leaf_binding);
    if (leaf_kind == XR_TARGET_LEAF_PROGRAM_TYPE_INVALID)
        return fail(error, error_size, "XR_TARGET_1002", "leaf aggregate value binding is invalid");
    int aggregate = leaf_kind == XR_TARGET_LEAF_PROGRAM_TYPE_AGGREGATE
                        ? 1
                        : xr_semantic_aggregate_type_kind(type);
    if (aggregate < 0) {
        if (error && error_size)
            snprintf(error, error_size,
                     "XR_TARGET_1002: value aggregate lacks exact semantic field facts "
                     "(type=%s)",
                     type && type->canonical_key ? type->canonical_key : "<unknown>");
        return false;
    }
    if (leaf_kind == XR_TARGET_LEAF_PROGRAM_TYPE_NOT_APPLICABLE &&
        semantic_leaf_program_provenance(builder->semantic_plan) && type &&
        type->kind == XR_KIND_INSTANCE && (type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0)
        return fail(error, error_size, "XR_TARGET_1002",
                    "leaf aggregate value lacks a typed program binding");
    if (aggregate == 0)
        return true;
    int eligible = aggregate_layout_eligibility(builder->semantic_plan, semantic_type, eligibility);
    if (eligible < 0)
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate layout has invalid or recursive exact type facts");
    if (eligible == 2)
        return true;
    if (analysis->defined_values[semantic_value]) {
        if (analysis->value_types[semantic_value] != semantic_type ||
            analysis->value_functions[semantic_value] != semantic_function)
            return fail(error, error_size, "XR_TARGET_1001",
                        "semantic aggregate value identity is ambiguous");
        return true;
    }
    analysis->defined_values[semantic_value] = 1;
    analysis->value_types[semantic_value] = semantic_type;
    analysis->value_functions[semantic_value] = semantic_function;
    if (!collect_layout_dependency(builder, semantic_type, states, error, error_size))
        return false;
    XrStableId slot_identity;
    bool parameter_slot = role == XR_TARGET_SLOT_PARAMETER;
    if ((!parameter_slot &&
         semantic_operation >= xr_semantic_plan_operation_count(builder->semantic_plan)) ||
        !make_slot_identity(builder->semantic_plan, semantic_function, role, source_identity,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001", "aggregate slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = semantic_function,
        .semantic_value = semantic_value,
        .semantic_operation = parameter_slot ? XR_SEMANTIC_INDEX_NONE : semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .role = role,
        .root_kind = XR_TARGET_ROOT_NONE,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
        .debug_variable = XR_SEMANTIC_INDEX_NONE,
        .semantic_type = semantic_type,
        .resolve_type_rep = true,
    };
    XrTargetValueIntent value = {
        .semantic_value = semantic_value,
        .semantic_function = semantic_function,
        .semantic_type = semantic_type,
        .slot_identity = slot_identity,
        .has_slot = true,
        .resolve_type_rep = true,
    };
    return append_slot_intent(builder, &slot, error, error_size) &&
           append_value_intent(builder, &value, error, error_size);
}

static bool mark_coroutine_functions(const XrSemanticPlan *plan, uint8_t *deferred,
                                     uint32_t function_count, char *error, size_t error_size) {
    size_t entity_count = xr_semantic_plan_entity_count(plan);
    size_t operation_count = xr_semantic_plan_operation_count(plan);
    for (size_t i = 0; i < entity_count; i++) {
        const XrSemanticEntityRecord *entity = xr_semantic_plan_entity(plan, i);
        if (!entity || entity->kind != XR_SEM_ENTITY_COROUTINE_STATE ||
            entity->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION)
            continue;
        if (entity->subject >= operation_count)
            return fail(error, error_size, "XR_TARGET_1001",
                        "coroutine state operation identity is out of range");
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan, entity->subject);
        if (!operation || operation->function >= function_count)
            return fail(error, error_size, "XR_TARGET_1001",
                        "coroutine state function identity is out of range");
        deferred[operation->function] = 1;
    }
    return true;
}

static bool collect_aggregate_intents(XrTargetPlanBuilder *builder,
                                      XrTargetValueStorageAnalysis *analysis, uint8_t *states,
                                      int8_t *eligibility, const uint8_t *deferred_functions,
                                      char *error, size_t error_size) {
    if (!index_value_operations(builder->semantic_plan, analysis, error, error_size))
        return false;
    uint32_t function_count = (uint32_t) xr_semantic_plan_function_count(builder->semantic_plan);
    uint32_t parameters = (uint32_t) xr_semantic_plan_parameter_count(builder->semantic_plan);
    for (uint32_t i = 0; i < parameters; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(builder->semantic_plan, i);
        if (!parameter)
            return false;
        if (parameter->function >= function_count)
            return fail(error, error_size, "XR_TARGET_1001",
                        "aggregate parameter function identity is out of range");
        if (deferred_functions[parameter->function])
            continue;
        if (!note_aggregate_value(builder, analysis, parameter->value, parameter->type,
                                  parameter->function, XR_SEMANTIC_INDEX_NONE,
                                  XR_TARGET_SLOT_PARAMETER, parameter->id, states, eligibility,
                                  error, error_size))
            return false;
    }
    uint32_t operations = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; i < operations; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation)
            return fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
        if (operation->function >= function_count)
            return fail(error, error_size, "XR_TARGET_1001",
                        "aggregate operation function identity is out of range");
        if (operation->result_value == XR_SEMANTIC_INDEX_NONE || operation->opcode == XI_PARAM)
            continue;
        /* Later CALL/COROUTINE families own caller-storage and suspend-frame
         * placement.
         *
         * A direct-local call returning a value aggregate is the exception it
         * does not need: the callee writes the result into the caller's own
         * slot, so that slot is ordinary aggregate storage and the aggregate
         * family can state it here. Leaving it deferred would strand the value
         * with no binding at all, because no call family places a slot the
         * caller already owns. A coroutine function stays deferred either way
         * -- its frame placement is a separate authority. */
        const XrSemanticFunctionRecord *direct_callee =
            semantic_direct_local_callee_for_operation(builder->semantic_plan, i);
        bool direct_local_aggregate_result =
            semantic_leaf_program_direct_call_is_exact(builder->semantic_plan, i, operation,
                                                       direct_callee, NULL) ||
            semantic_product_direct_call_is_exact(builder->semantic_plan, i, operation,
                                                  direct_callee, NULL);
        if (deferred_functions[operation->function] ||
            (!direct_local_aggregate_result && operation->opcode < XI_OP_COUNT &&
             (xi_generated_op_class(operation->opcode) == XI_GEN_CLASS_CALL ||
              xi_generated_op_class(operation->opcode) == XI_GEN_CLASS_COROUTINE)))
            continue;
        uint8_t role = operation->opcode == XI_PHI ? XR_TARGET_SLOT_PHI : XR_TARGET_SLOT_TEMPORARY;
        if (!note_aggregate_value(builder, analysis, operation->result_value,
                                  operation->result_type, operation->function, i, role,
                                  operation->id, states, eligibility, error, error_size))
            return false;
    }
    return true;
}

static bool builder_add_aggregates(XrTargetPlanBuilder *builder, char *error, size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_AGGREGATE, error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    uint32_t type_count = (uint32_t) xr_semantic_plan_type_count(builder->semantic_plan);
    uint32_t function_count = (uint32_t) xr_semantic_plan_function_count(builder->semantic_plan);
    uint8_t *states = (uint8_t *) allocate_records(type_count, sizeof(*states));
    int8_t *eligibility = (int8_t *) allocate_records(type_count, sizeof(*eligibility));
    uint8_t *deferred_functions =
        (uint8_t *) allocate_records(function_count, sizeof(*deferred_functions));
    bool valid =
        (!type_count || (states && eligibility)) && (!function_count || deferred_functions) &&
        mark_coroutine_functions(builder->semantic_plan, deferred_functions, function_count, error,
                                 error_size) &&
        value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size) &&
        collect_aggregate_intents(builder, &analysis, states, eligibility, deferred_functions,
                                  error, error_size);
    xr_free(states);
    xr_free(eligibility);
    xr_free(deferred_functions);
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        if (!error || !error_size || !error[0])
            fail(error, error_size, "XR_EXEC_5003",
                 "aggregate intent collection allocation failed");
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_AGGREGATE;
    return true;
}

static bool semantic_operation_is_call_shaped(const XrSemanticPlan *plan,
                                              const XrSemanticOperationRecord *operation) {
    if (operation) {
        /* Numeric SemanticPlan operation identity is explicit authority here;
         * do not reclassify from effects, names, or a generated backend class. */
        switch (operation->opcode) {
            case XI_CALL:
            case XI_CALL_METHOD:
            case XI_CALL_METHOD_DIRECT:
            case XI_TAIL_CALL:
            case XI_CALL_BUILTIN:
            case XI_ATOMIC_TO_STRING:
            case XI_EXTRACT:
            case XI_GEN_CALL:
            case XI_MULTI_RET:
                return true;
            default:
                break;
        }
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!operation || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin)
        return false;
    for (uint32_t i = 0; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *operand = &operands[operation->operand_begin + i];
        if (operand->role == XR_SEM_OPERAND_CALLEE || operand->role == XR_SEM_OPERAND_RECEIVER ||
            (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) != 0)
            return true;
    }
    return false;
}

static bool call_type_is_exact_scalar(const XrSemanticPlan *plan, uint32_t type_index) {
    uint16_t kind = XR_MACHINE_REP_COUNT;
    return type_index < xr_semantic_plan_type_count(plan) &&
           classify_scalar_type(xr_semantic_plan_type(plan, type_index), &kind) ==
               XR_TARGET_SCALAR_VALUE;
}

/* The frozen TargetPlan contract already owns every fact needed to name
 * Channel.close without a
 * backend registry lookup: receiver type, source selector, non-super symbol
 * encoding, arity, result type, and instance suspension flags. The receiver
 * is the dispatch target, not a source argument; its machine slot remains a
 * future channel-storage family concern. */
static bool semantic_operation_is_exact_channel_close(const XrSemanticPlan *plan,
                                                      const XrSemanticOperationRecord *operation,
                                                      uint32_t *receiver_type) {
    if (receiver_type)
        *receiver_type = XR_SEMANTIC_INDEX_NONE;
    if (!plan || !operation)
        return false;
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & INT64_C(1)) != 0 ||
        (uint64_t) operation->semantic_immediate > UINT32_MAX || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count || !operands || !metadata ||
        !metadata[operation->metadata_begin] ||
        strcmp(metadata[operation->metadata_begin], "close") != 0 ||
        (operation->flags & XI_FLAG_MAY_SUSPEND) != 0)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *receiver_record = xr_semantic_plan_type(plan, receiver->type);
    const XrSemanticTypeRecord *result = xr_semantic_plan_type(plan, operation->result_type);
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(plan, operation->function);
    if (!receiver_record || !result || !function || receiver_record->kind != XR_KIND_CHANNEL ||
        result->kind != XR_KIND_UNIT || result->scalar_rep != XR_SCALAR_REP_NONE ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        (receiver->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0 ||
        receiver->value < function->value_begin ||
        receiver->value >= function->value_begin + function->value_count ||
        operation->result_value < function->value_begin ||
        operation->result_value >= function->value_begin + function->value_count)
        return false;
    if (receiver_type)
        *receiver_type = receiver->type;
    return true;
}

static bool collect_channel_close_call_intent(XrTargetPlanBuilder *builder,
                                              uint32_t operation_index,
                                              const XrSemanticOperationRecord *operation,
                                              char *error, size_t error_size) {
    uint32_t receiver_type = XR_SEMANTIC_INDEX_NONE;
    if (!semantic_operation_is_exact_channel_close(builder->semantic_plan, operation,
                                                   &receiver_type) ||
        !call_type_is_exact_scalar(builder->semantic_plan, operation->result_type))
        return fail(error, error_size, "XR_TARGET_1003",
                    "channel-close dispatch authority is incomplete");
    const XrSemanticTypeRecord *receiver =
        xr_semantic_plan_type(builder->semantic_plan, receiver_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_CHANNEL_CLOSE,
        .target_kind = XR_TARGET_CALL_TARGET_CHANNEL_CLOSE,
    };
    if (!receiver ||
        !stable_identity_from_pair("xray-target-call-v5", operation->id, receiver->id,
                                   (uint32_t) operation->semantic_immediate, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "channel-close call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

/* The construction of a declared class. The SemanticPlan target names the
 * declaration and nothing else, so the intent carries no callee function and no
 * suspension: the shared class-shape judgement admits only a call whose effects
 * are the generated call effects, which excludes suspension. The result is the
 * owned instance the construction returns, and its storage belongs to the
 * source-class instance family. The arguments are the declaration's own
 * constructor parameters after its receiver, which the same shared judgement
 * has already matched one for one; the receiver itself is never an argument,
 * because the construction supplies it rather than passing it. */
static bool collect_source_class_constructor_call_intent(XrTargetPlanBuilder *builder,
                                                         uint32_t target_index,
                                                         const XrSemanticCallTargetRecord *target,
                                                         char *error, size_t error_size) {
    const XrSemanticPlan *plan = builder->semantic_plan;
    const XrSemanticOperationRecord *operation =
        target ? xr_semantic_plan_operation(plan, target->operation) : NULL;
    bool imported = target && target->dependency != XR_SEMANTIC_INDEX_NONE;
    const XrSemanticPlan *callee_plan =
        imported && target->dependency < builder->semantic_dependency_count
            ? builder->semantic_dependencies[target->dependency]
            : plan;
    const XrSemanticSourceExportRecord *source_export =
        imported && callee_plan &&
                target->source_export < xr_semantic_plan_source_export_count(callee_plan)
            ? xr_semantic_plan_source_export(callee_plan, target->source_export)
            : NULL;
    uint32_t constructor = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_class = imported
                                ? xr_semantic_imported_class_construction_authority_source_class(
                                      plan, callee_plan,
                                      target->dependency < xr_semantic_plan_dependency_count(plan)
                                          ? xr_semantic_plan_dependency(plan, target->dependency)
                                          : NULL,
                                      source_export, operation, &constructor)
                                : xr_semantic_class_construction_source_class(plan, operation);
    if (!imported)
        constructor = xr_semantic_class_constructor_function(plan, source_class);
    const XrSemanticFunctionRecord *callee =
        constructor != XR_SEMANTIC_INDEX_NONE ? xr_semantic_plan_function(callee_plan, constructor)
                                              : NULL;
    bool target_identity =
        !imported ||
        (source_export && xr_stable_id_equal(target->export_identity, source_export->id) &&
         ((callee && xr_stable_id_equal(target->callee_function, callee->id)) ||
          (!callee && stable_id_is_zero(target->callee_function))));
    if (!target || !operation || target->kind != XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR ||
        target->function != XR_SEMANTIC_INDEX_NONE || !callee_plan || !target_identity ||
        (!imported && (target->dependency != XR_SEMANTIC_INDEX_NONE ||
                       target->source_export != XR_SEMANTIC_INDEX_NONE ||
                       !stable_id_is_zero(target->export_identity) ||
                       !stable_id_is_zero(target->callee_function))) ||
        target->callable_type != operation->result_type || source_class == XR_SEMANTIC_INDEX_NONE)
        return fail(error, error_size, "XR_TARGET_1003",
                    "source class construction dispatch authority is incomplete");
    uint16_t argument_count = (uint16_t) (operation->operand_count - 1u);
    if (argument_count != 0 && !callee)
        return fail(error, error_size, "XR_TARGET_1003",
                    "source class construction argument authority is incomplete");
    XrTargetCallIntent call = {
        .semantic_call_target = target_index,
        .semantic_operation = target->operation,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = imported ? target->dependency : XR_SEMANTIC_INDEX_NONE,
        .source_export = imported ? target->source_export : XR_SEMANTIC_INDEX_NONE,
        .source_export_identity = imported ? target->export_identity : (XrStableId) {{0}},
        .source_callee_identity = imported ? target->callee_function : (XrStableId) {{0}},
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = argument_count,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_SOURCE_CLASS_CONSTRUCTOR,
        .target_kind = XR_TARGET_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR,
    };
    if (!stable_identity_from_pair("xray-target-source-class-constructor-v1", target->id,
                                   operation->id, 0, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "source class construction call identity is incomplete");
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    uint32_t call_intent = builder->call_intent_count;
    for (uint16_t ordinal = 0; ordinal < argument_count; ordinal++) {
        uint32_t parameter_index = callee->parameter_begin + 1u + ordinal;
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(callee_plan, parameter_index);
        uint32_t semantic_operand = operation->operand_begin + 1u + ordinal;
        if (!operands || !parameter || semantic_operand >= operand_count)
            return fail(error, error_size, "XR_TARGET_1003",
                        "source class construction argument authority is incomplete");
        const XrSemanticOperandRecord *operand = &operands[semantic_operand];
        /* The shared judgement already proved the parameter contract. Storage
         * authority is resolved once, during call materialization, from the
         * caller value and (for a local constructor) the callee parameter. A
         * scalar-only prefilter here used to reject valid String, container and
         * class arguments locally while imported constructors bypassed it; the
         * canonical storage rows below now decide both routes identically. */
        XrTargetCallArgumentIntent argument = {
            .call_intent = call_intent,
            .semantic_operand = semantic_operand,
            .semantic_value = operand->value,
            .caller_storage_value = operand->value,
            .callee_parameter = parameter_index,
            .ordinal = ordinal,
            .mode = XR_TARGET_CALL_VALUE,
            .ownership = operand->ownership_action == XR_SEM_OPERAND_CONSUME
                             ? XR_TARGET_CALL_CONSUME
                             : XR_TARGET_CALL_READ,
            .transfer_mode = operand->transfer_mode,
        };
        if (!stable_identity_from_pair("xray-target-call-argument-v1", target->id, parameter->id,
                                       ordinal, &argument.identity) ||
            !append_call_argument_intent(builder, &argument, error, error_size))
            return false;
    }
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_stringbuilder_constructor_call_intent(
    XrTargetPlanBuilder *builder, uint32_t operation_index,
    const XrSemanticOperationRecord *operation, char *error, size_t error_size) {
    if (!semantic_stringbuilder_constructor_is_exact(builder->semantic_plan, operation))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder constructor dispatch authority is incomplete");
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_STRINGBUILDER_CONSTRUCTOR,
        .target_kind = XR_TARGET_CALL_TARGET_STRINGBUILDER_CONSTRUCTOR,
    };
    if (!stable_identity_from_pair("xray-target-stringbuilder-constructor-v1", operation->id,
                                   operation->allocation_id, 0, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder constructor call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

/* The scalar result owns nothing, so the row states no returned ownership and
 * carries no argument entry: this builtin has no callee function whose
 * parameter records an argument row could be paired with, and the emission
 * recipe reads the semantic operand directly. */
/* The container spelling differs from the scalar one in exactly one respect the
 * row has to carry: the result is a fresh allocation the caller owns, so the
 * call returns ownership rather than nothing. */
static bool collect_container_copy_call_intent(XrTargetPlanBuilder *builder,
                                               uint32_t operation_index,
                                               const XrSemanticOperationRecord *operation,
                                               char *error, size_t error_size) {
    uint32_t argument = XR_SEMANTIC_INDEX_NONE;
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!xr_semantic_container_copy_is_exact(builder->semantic_plan, operation, &argument, NULL) ||
        !xr_target_container_copy_storage(builder->semantic_plan, operation, &storage))
        return fail(error, error_size, "XR_TARGET_1003",
                    "container copy dispatch authority is incomplete");
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_CONTAINER_COPY,
        .target_kind = XR_TARGET_CALL_TARGET_CONTAINER_COPY,
        .array_element_storage = storage,
    };
    if (!result_type || !stable_identity_from_pair("xray-target-container-copy-v1", operation->id,
                                                   result_type->id, argument, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "container copy call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_scalar_copy_call_intent(XrTargetPlanBuilder *builder, uint32_t operation_index,
                                            const XrSemanticOperationRecord *operation, char *error,
                                            size_t error_size) {
    uint32_t argument = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_scalar_copy_is_exact(builder->semantic_plan, operation, &argument))
        return fail(error, error_size, "XR_TARGET_1003",
                    "scalar copy dispatch authority is incomplete");
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_SCALAR_COPY,
        .target_kind = XR_TARGET_CALL_TARGET_SCALAR_COPY,
    };
    if (!result_type || !stable_identity_from_pair("xray-target-scalar-copy-v1", operation->id,
                                                   result_type->id, argument, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003", "scalar copy call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_array_intrinsic_call_intent(XrTargetPlanBuilder *builder,
                                                uint32_t operation_index,
                                                const XrSemanticOperationRecord *operation,
                                                char *error, size_t error_size) {
    uint8_t kind = XR_TARGET_ARRAY_INTRINSIC_NONE;
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!semantic_array_intrinsic_is_exact(builder->semantic_plan, operation, &kind, &storage))
        return fail(error, error_size, "XR_TARGET_1003",
                    "Array intrinsic dispatch authority is incomplete");
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = operation->operand_count,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_ARRAY_INTRINSIC,
        .target_kind = XR_TARGET_CALL_TARGET_ARRAY_INTRINSIC,
        .array_intrinsic_kind = kind,
        .array_element_storage = storage,
    };
    uint32_t discriminator = ((uint32_t) kind << 8) | storage;
    if (!stable_identity_from_pair("xray-target-array-intrinsic-v1", operation->id,
                                   operation->allocation_id, discriminator, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "Array intrinsic call identity is incomplete");
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(builder->semantic_plan, &operand_count);
    uint32_t call_intent = builder->call_intent_count;
    for (uint16_t ordinal = 0; ordinal < operation->operand_count; ordinal++) {
        uint32_t semantic_operand = operation->operand_begin + ordinal;
        if (!operands || semantic_operand >= operand_count)
            return fail(error, error_size, "XR_TARGET_1003",
                        "Array intrinsic operand authority is incomplete");
        const XrSemanticOperandRecord *operand = &operands[semantic_operand];
        const XrSemanticTypeRecord *type =
            xr_semantic_plan_type(builder->semantic_plan, operand->type);
        XrTargetCallArgumentIntent argument = {
            .call_intent = call_intent,
            .semantic_operand = semantic_operand,
            .semantic_value = operand->value,
            .caller_storage_value = operand->value,
            .callee_parameter = XR_SEMANTIC_INDEX_NONE,
            .ordinal = ordinal,
            .mode = XR_TARGET_CALL_VALUE,
            .ownership = XR_TARGET_CALL_CONSUME,
            .transfer_mode = operand->transfer_mode,
        };
        if (!type ||
            !stable_identity_from_pair("xray-target-array-intrinsic-argument-v1", operation->id,
                                       type->id, ordinal, &argument.identity) ||
            !append_call_argument_intent(builder, &argument, error, error_size))
            return false;
    }
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_array_fill_scalar_call_intent(XrTargetPlanBuilder *builder,
                                                  uint32_t operation_index,
                                                  const XrSemanticOperationRecord *operation,
                                                  char *error, size_t error_size) {
    uint32_t receiver_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t fill_value = XR_SEMANTIC_INDEX_NONE;
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!semantic_array_fill_scalar_is_exact(builder->semantic_plan, operation, &receiver_value,
                                             &fill_value, &storage))
        return fail(error, error_size, "XR_TARGET_1003",
                    "Array.fill scalar dispatch authority is incomplete");
    const XrSemanticTypeRecord *receiver_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 2,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_ARRAY_FILL_SCALAR,
        .target_kind = XR_TARGET_CALL_TARGET_ARRAY_FILL_SCALAR,
        .array_intrinsic_kind = XR_TARGET_ARRAY_INTRINSIC_NONE,
        .array_element_storage = storage,
    };
    if (!receiver_type ||
        !stable_identity_from_pair("xray-target-array-fill-scalar-v1", operation->id,
                                   receiver_type->id, storage, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "Array.fill scalar call identity is incomplete");
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(builder->semantic_plan, &operand_count);
    uint32_t call_intent = builder->call_intent_count;
    for (uint16_t ordinal = 0; ordinal < 2; ordinal++) {
        uint32_t semantic_operand = operation->operand_begin + ordinal;
        if (!operands || semantic_operand >= operand_count)
            return fail(error, error_size, "XR_TARGET_1003",
                        "Array.fill scalar operand authority is incomplete");
        const XrSemanticOperandRecord *operand = &operands[semantic_operand];
        const XrSemanticTypeRecord *type =
            xr_semantic_plan_type(builder->semantic_plan, operand->type);
        XrTargetCallArgumentIntent argument = {
            .call_intent = call_intent,
            .semantic_operand = semantic_operand,
            .semantic_value = ordinal == 0 ? receiver_value : fill_value,
            .caller_storage_value = ordinal == 0 ? receiver_value : fill_value,
            .callee_parameter = XR_SEMANTIC_INDEX_NONE,
            .ordinal = ordinal,
            .mode = XR_TARGET_CALL_VALUE,
            .ownership = ordinal == 0 ? XR_TARGET_CALL_BORROW : XR_TARGET_CALL_CONSUME,
            .transfer_mode = operand->transfer_mode,
        };
        if (!type ||
            !stable_identity_from_pair("xray-target-array-fill-scalar-argument-v1", operation->id,
                                       type->id, ordinal, &argument.identity) ||
            !append_call_argument_intent(builder, &argument, error, error_size))
            return false;
    }
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_array_hof_call_intent(XrTargetPlanBuilder *builder, uint32_t operation_index,
                                          const XrSemanticOperationRecord *operation, char *error,
                                          size_t error_size) {
    uint8_t kind = XR_TARGET_ARRAY_HOF_NONE;
    uint8_t source_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t result_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    uint32_t callback = XR_SEMANTIC_INDEX_NONE;
    uint32_t initial = XR_SEMANTIC_INDEX_NONE;
    if (!semantic_array_hof_is_exact(builder->semantic_plan, operation, &kind, &source_storage,
                                     &result_storage, &receiver, &callback, &initial))
        return fail(error, error_size, "XR_TARGET_1003",
                    "Array higher-order dispatch authority is incomplete");
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(builder->semantic_plan, operation->callable_function);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = operation->callable_function,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = operation->operand_count,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership =
            kind == XR_TARGET_ARRAY_HOF_REDUCE ? XR_TARGET_CALL_NONE : XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_ARRAY_HOF,
        .target_kind = XR_TARGET_CALL_TARGET_ARRAY_HOF,
        .array_intrinsic_kind = XR_TARGET_ARRAY_INTRINSIC_NONE,
        .array_element_storage = source_storage,
        .array_hof_kind = kind,
        .array_result_element_storage = result_storage,
    };
    uint32_t discriminator =
        ((uint32_t) kind << 16) | ((uint32_t) source_storage << 8) | result_storage;
    if (!callee || !stable_identity_from_pair("xray-target-array-hof-v1", operation->id, callee->id,
                                              discriminator, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "Array higher-order call identity is incomplete");
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(builder->semantic_plan, &operand_count);
    uint32_t call_intent = builder->call_intent_count;
    for (uint16_t ordinal = 0; ordinal < operation->operand_count; ordinal++) {
        uint32_t semantic_operand = operation->operand_begin + ordinal;
        if (!operands || semantic_operand >= operand_count)
            return fail(error, error_size, "XR_TARGET_1003",
                        "Array higher-order operand authority is incomplete");
        const XrSemanticOperandRecord *operand = &operands[semantic_operand];
        const XrSemanticTypeRecord *type =
            xr_semantic_plan_type(builder->semantic_plan, operand->type);
        uint32_t expected_value = ordinal == 0 ? receiver : ordinal == 1 ? callback : initial;
        XrTargetCallArgumentIntent argument = {
            .call_intent = call_intent,
            .semantic_operand = semantic_operand,
            .semantic_value = expected_value,
            .caller_storage_value = expected_value,
            .callee_parameter = XR_SEMANTIC_INDEX_NONE,
            .ordinal = ordinal,
            .mode = XR_TARGET_CALL_VALUE,
            .ownership = ordinal == 0 ? XR_TARGET_CALL_BORROW : XR_TARGET_CALL_CONSUME,
            .transfer_mode = operand->transfer_mode,
        };
        if (!type || operand->value != expected_value ||
            !stable_identity_from_pair("xray-target-array-hof-argument-v1", operation->id, type->id,
                                       ordinal, &argument.identity) ||
            !append_call_argument_intent(builder, &argument, error, error_size))
            return false;
    }
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_string_byte_slice_view_call_intent(XrTargetPlanBuilder *builder,
                                                       uint32_t operation_index,
                                                       const XrSemanticOperationRecord *operation,
                                                       char *error, size_t error_size) {
    if (!semantic_string_byte_slice_view_is_exact(builder->semantic_plan, operation))
        return fail(error, error_size, "XR_TARGET_1003",
                    "string byte-slice view target authority is incomplete");
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_BORROW,
        .calling_convention = XR_TARGET_CALL_CONVENTION_STRING_BYTE_SLICE_VIEW,
        .target_kind = XR_TARGET_CALL_TARGET_STRING_BYTE_SLICE_VIEW,
    };
    if (!stable_identity_from_pair(
            "xray-target-string-byte-slice-view-v1", operation->id,
            xr_semantic_plan_type(builder->semantic_plan, operation->result_type)->id, 0,
            &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "string byte-slice view call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_stringbuilder_append_rune_call_intent(
    XrTargetPlanBuilder *builder, uint32_t operation_index,
    const XrSemanticOperationRecord *operation, char *error, size_t error_size) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    uint32_t argument = XR_SEMANTIC_INDEX_NONE;
    if (!semantic_stringbuilder_append_rune_is_exact(builder->semantic_plan, operation, &receiver,
                                                     &argument))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.append(rune) dispatch authority is incomplete");
    const XrSemanticTypeRecord *receiver_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED
                                ? XR_TARGET_CALL_BORROW
                                : XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_RUNE,
        .target_kind = XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_RUNE,
    };
    if (!receiver_type ||
        !stable_identity_from_pair("xray-target-stringbuilder-append-rune-v1", operation->id,
                                   receiver_type->id, argument, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.append(rune) call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_string_runes_call_intent(XrTargetPlanBuilder *builder, uint32_t operation_index,
                                             const XrSemanticOperationRecord *operation,
                                             char *error, size_t error_size) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_string_runes_is_exact(builder->semantic_plan, operation, &receiver))
        return fail(error, error_size, "XR_TARGET_1003",
                    "String.runes dispatch authority is incomplete");
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_STRING_RUNES,
        .target_kind = XR_TARGET_CALL_TARGET_STRING_RUNES,
    };
    if (!result_type || !stable_identity_from_pair("xray-target-string-runes-v1", operation->id,
                                                   result_type->id, receiver, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "String.runes call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_iterator_rune_has_next_call_intent(XrTargetPlanBuilder *builder,
                                                       uint32_t operation_index,
                                                       const XrSemanticOperationRecord *operation,
                                                       char *error, size_t error_size) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_iterator_rune_has_next_is_exact(builder->semantic_plan, operation, &receiver))
        return fail(error, error_size, "XR_TARGET_1003",
                    "Iterator<rune>.hasNext dispatch authority is incomplete");
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_HAS_NEXT,
        .target_kind = XR_TARGET_CALL_TARGET_ITERATOR_RUNE_HAS_NEXT,
    };
    if (!result_type ||
        !stable_identity_from_pair("xray-target-iterator-rune-has-next-v1", operation->id,
                                   result_type->id, receiver, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "Iterator<rune>.hasNext call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_iterator_rune_next_call_intent(XrTargetPlanBuilder *builder,
                                                   uint32_t operation_index,
                                                   const XrSemanticOperationRecord *operation,
                                                   char *error, size_t error_size) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_iterator_rune_next_is_exact(builder->semantic_plan, operation, &receiver))
        return fail(error, error_size, "XR_TARGET_1003",
                    "Iterator<rune>.next dispatch authority is incomplete");
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_NEXT,
        .target_kind = XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NEXT,
    };
    if (!result_type ||
        !stable_identity_from_pair("xray-target-iterator-rune-next-v1", operation->id,
                                   result_type->id, receiver, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "Iterator<rune>.next call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_map_entry_iterator_call_intent(XrTargetPlanBuilder *builder,
                                                   uint32_t operation_index,
                                                   const XrSemanticOperationRecord *operation,
                                                   char *error, size_t error_size) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    const char *domain = NULL;
    uint8_t convention = XR_TARGET_CALL_CONVENTION_INVALID;
    uint8_t target = XR_TARGET_CALL_TARGET_INVALID;
    uint8_t ownership = XR_TARGET_CALL_NONE;
    if (xr_semantic_map_entries_iterator_is_exact(builder->semantic_plan, operation, &receiver,
                                                  NULL)) {
        domain = "xray-target-map-entries-iterator-v1";
        convention = XR_TARGET_CALL_CONVENTION_MAP_ENTRIES_ITERATOR;
        target = XR_TARGET_CALL_TARGET_MAP_ENTRIES_ITERATOR;
        ownership = XR_TARGET_CALL_RETURN_OWNED;
    } else if (xr_semantic_map_entry_iterator_has_next_is_exact(builder->semantic_plan, operation,
                                                                &receiver)) {
        domain = "xray-target-map-entry-iterator-has-next-v1";
        convention = XR_TARGET_CALL_CONVENTION_MAP_ENTRY_ITERATOR_HAS_NEXT;
        target = XR_TARGET_CALL_TARGET_MAP_ENTRY_ITERATOR_HAS_NEXT;
    } else if (xr_semantic_map_entry_iterator_next_is_exact(builder->semantic_plan, operation,
                                                            &receiver)) {
        domain = "xray-target-map-entry-iterator-next-v1";
        convention = XR_TARGET_CALL_CONVENTION_MAP_ENTRY_ITERATOR_NEXT;
        target = XR_TARGET_CALL_TARGET_MAP_ENTRY_ITERATOR_NEXT;
        ownership = XR_TARGET_CALL_RETURN_OWNED;
    } else {
        return fail(error, error_size, "XR_TARGET_1003",
                    "Map entry iterator dispatch authority is incomplete");
    }
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = ownership,
        .calling_convention = convention,
        .target_kind = target,
    };
    if (!result_type || !stable_identity_from_pair(domain, operation->id, result_type->id, receiver,
                                                   &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "Map entry iterator call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

/* Same shape as the next call above, plus the index it projects by. The
 * argument is carried as an ordinary call argument so the storage families
 * decide how it is held, exactly as they would for any other scalar operand. */
static bool collect_iterator_rune_nth_call_intent(XrTargetPlanBuilder *builder,
                                                  uint32_t operation_index,
                                                  const XrSemanticOperationRecord *operation,
                                                  char *error, size_t error_size) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    uint32_t index_value = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_iterator_rune_nth_is_exact(builder->semantic_plan, operation, &receiver,
                                                &index_value))
        return fail(error, error_size, "XR_TARGET_1003",
                    "Iterator<rune>.nth dispatch authority is incomplete");
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(builder->semantic_plan, &operand_count);
    uint32_t semantic_operand = operation->operand_begin + 1u;
    const XrSemanticOperandRecord *operand =
        operands && semantic_operand < operand_count ? &operands[semantic_operand] : NULL;
    const XrSemanticTypeRecord *operand_type =
        operand ? xr_semantic_plan_type(builder->semantic_plan, operand->type) : NULL;
    if (!operand || operand->value != index_value || !operand_type)
        return fail(error, error_size, "XR_TARGET_1003",
                    "Iterator<rune>.nth index authority is incomplete");
    uint32_t argument_begin = builder->call_argument_intent_count;
    uint32_t call_intent = builder->call_intent_count;
    XrTargetCallArgumentIntent argument = {
        .call_intent = call_intent,
        .semantic_operand = semantic_operand,
        .semantic_value = index_value,
        .caller_storage_value = index_value,
        .callee_parameter = XR_SEMANTIC_INDEX_NONE,
        .ordinal = 0,
        .mode = XR_TARGET_CALL_VALUE,
        .ownership = XR_TARGET_CALL_CONSUME,
        .transfer_mode = operand->transfer_mode,
    };
    if (!stable_identity_from_pair("xray-target-iterator-rune-nth-argument-v1", operation->id,
                                   operand_type->id, 0, &argument.identity) ||
        !append_call_argument_intent(builder, &argument, error, error_size))
        return false;
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = argument_begin,
        .argument_count = 1,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_NTH,
        .target_kind = XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NTH,
    };
    if (!result_type ||
        !stable_identity_from_pair("xray-target-iterator-rune-nth-v1", operation->id,
                                   result_type->id, receiver, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "Iterator<rune>.nth call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_rune_to_uint32_call_intent(XrTargetPlanBuilder *builder,
                                               uint32_t operation_index,
                                               const XrSemanticOperationRecord *operation,
                                               char *error, size_t error_size) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_rune_to_uint32_is_exact(builder->semantic_plan, operation, &receiver))
        return fail(error, error_size, "XR_TARGET_1003",
                    "rune.toUInt32 dispatch authority is incomplete");
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_RUNE_TO_UINT32,
        .target_kind = XR_TARGET_CALL_TARGET_RUNE_TO_UINT32,
    };
    if (!result_type || !stable_identity_from_pair("xray-target-rune-to-uint32-v1", operation->id,
                                                   result_type->id, receiver, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "rune.toUInt32 call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

/* The one-rune string. Same shape as the toUInt32 intent beside it; only the
 * result type and the convention differ. */
static bool collect_rune_to_string_call_intent(XrTargetPlanBuilder *builder,
                                               uint32_t operation_index,
                                               const XrSemanticOperationRecord *operation,
                                               char *error, size_t error_size) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_rune_to_string_is_exact(builder->semantic_plan, operation, &receiver))
        return fail(error, error_size, "XR_TARGET_1003",
                    "rune.toString dispatch authority is incomplete");
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_RUNE_TO_STRING,
        .target_kind = XR_TARGET_CALL_TARGET_RUNE_TO_STRING,
    };
    if (!result_type || !stable_identity_from_pair("xray-target-rune-to-string-v1", operation->id,
                                                   result_type->id, receiver, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "rune.toString call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_rune_is_whitespace_call_intent(XrTargetPlanBuilder *builder,
                                                   uint32_t operation_index,
                                                   const XrSemanticOperationRecord *operation,
                                                   char *error, size_t error_size) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_rune_is_whitespace_is_exact(builder->semantic_plan, operation, &receiver))
        return fail(error, error_size, "XR_TARGET_1003",
                    "rune.isWhitespace dispatch authority is incomplete");
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_RUNE_IS_WHITESPACE,
        .target_kind = XR_TARGET_CALL_TARGET_RUNE_IS_WHITESPACE,
    };
    if (!result_type ||
        !stable_identity_from_pair("xray-target-rune-is-whitespace-v1", operation->id,
                                   result_type->id, receiver, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "rune.isWhitespace call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_string_slice_range_call_intent(XrTargetPlanBuilder *builder,
                                                   uint32_t operation_index,
                                                   const XrSemanticOperationRecord *operation,
                                                   char *error, size_t error_size) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    uint32_t start = XR_SEMANTIC_INDEX_NONE;
    uint32_t end = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_string_slice_range_is_exact(builder->semantic_plan, operation, &receiver,
                                                 &start, &end))
        return fail(error, error_size, "XR_TARGET_1003",
                    "String.slice(start, end) dispatch authority is incomplete");
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_STRING_SLICE_RANGE,
        .target_kind = XR_TARGET_CALL_TARGET_STRING_SLICE_RANGE,
        .tail = true,
    };
    if (!result_type ||
        !stable_identity_from_pair("xray-target-string-slice-range-v1", operation->id,
                                   result_type->id, receiver, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "String.slice(start, end) call identity is incomplete");
    (void) start;
    (void) end;
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_stringbuilder_to_string_call_intent(XrTargetPlanBuilder *builder,
                                                        uint32_t operation_index,
                                                        const XrSemanticOperationRecord *operation,
                                                        char *error, size_t error_size) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!semantic_stringbuilder_to_string_is_exact(builder->semantic_plan, operation, &receiver))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.toString dispatch authority is incomplete");
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_STRINGBUILDER_TO_STRING,
        .target_kind = XR_TARGET_CALL_TARGET_STRINGBUILDER_TO_STRING,
    };
    if (!result_type ||
        !stable_identity_from_pair("xray-target-stringbuilder-to-string-v1", operation->id,
                                   result_type->id, receiver, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.toString call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_stringbuilder_append_string_call_intent(
    XrTargetPlanBuilder *builder, uint32_t operation_index,
    const XrSemanticOperationRecord *operation, char *error, size_t error_size) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE, argument = XR_SEMANTIC_INDEX_NONE;
    if (!semantic_stringbuilder_append_string_is_exact(builder->semantic_plan, operation, &receiver,
                                                       &argument))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.append(string) authority is incomplete");
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED
                                ? XR_TARGET_CALL_BORROW
                                : XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_STRING,
        .target_kind = XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_STRING};
    if (!type || !stable_identity_from_pair("xray-target-stringbuilder-append-string-v1",
                                            operation->id, type->id, argument, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.append(string) call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_json_namespace_value_call_intent(XrTargetPlanBuilder *builder,
                                                     uint32_t operation_index,
                                                     const XrSemanticOperationRecord *operation,
                                                     char *error, size_t error_size) {
    uint32_t argument = XR_SEMANTIC_INDEX_NONE;
    if (!semantic_json_namespace_value_is_exact(builder->semantic_plan, operation, &argument))
        return fail(error, error_size, "XR_TARGET_1003",
                    "JSON.value dispatch authority is incomplete");
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_JSON_NAMESPACE_VALUE,
        .target_kind = XR_TARGET_CALL_TARGET_JSON_NAMESPACE_VALUE,
    };
    if (!result_type ||
        !stable_identity_from_pair("xray-target-json-namespace-value-v1", operation->id,
                                   result_type->id, argument, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003", "JSON.value call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_panic_info_constructor_call_intent(XrTargetPlanBuilder *builder,
                                                       uint32_t operation_index,
                                                       const XrSemanticOperationRecord *operation,
                                                       char *error, size_t error_size) {
    uint32_t argument = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_panic_info_constructor_is_exact(builder->semantic_plan, operation, &argument))
        return fail(error, error_size, "XR_TARGET_1003",
                    "PanicInfo constructor dispatch authority is incomplete");
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    /* The message argument is not a callee parameter: this class has no callee
     * function whose parameter records could be paired with an argument row, so
     * the emission recipe reads the semantic operand directly and the call row
     * carries no argument entries. */
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_PANIC_INFO_CONSTRUCTOR,
        .target_kind = XR_TARGET_CALL_TARGET_PANIC_INFO_CONSTRUCTOR,
    };
    if (!result_type ||
        !stable_identity_from_pair("xray-target-panic-info-constructor-v1", operation->id,
                                   result_type->id, argument, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "PanicInfo constructor call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

/* A member that hands back its receiver has no result storage to claim: the
 * result is the receiver's own reference, which the receiver's own storage row
 * already states, so the call row binds no scalar there and every use of that
 * result stays without authority.  Every other member here returns a fresh
 * scalar the row states outright. */
static bool collect_array_member_scalar_call_intent(XrTargetPlanBuilder *builder,
                                                    uint32_t operation_index,
                                                    const XrSemanticOperationRecord *operation,
                                                    char *error, size_t error_size) {
    uint32_t element = XR_SEMANTIC_INDEX_NONE;
    uint32_t operands_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(builder->semantic_plan, &operands_count);
    if (!semantic_array_member_scalar_is_exact(builder->semantic_plan, operation, &element, NULL) ||
        !operands)
        return fail(error, error_size, "XR_TARGET_1003",
                    "Array member dispatch authority is incomplete");
    uint32_t receiver_type_index = operands[operation->operand_begin].type;
    uint32_t tagged_store_operand = XR_SEMANTIC_INDEX_NONE;
    bool tagged_store = semantic_array_member_tagged_store_is_exact(
        builder->semantic_plan, operation, &tagged_store_operand, &element, NULL);
    bool receiver_result = operation->result_type == receiver_type_index;
    /* A member that builds a string hands back a fresh heap value rather than
     * a scalar the row states outright, so it claims the return and the caller
     * releases it. The shape table says which members those are. */
    uint32_t member_metadata_count = 0;
    const char *const *member_metadata =
        xr_semantic_plan_metadata(builder->semantic_plan, &member_metadata_count);
    const XrArrayMemberShape *result_shape =
        member_metadata && operation->metadata_begin < member_metadata_count
            ? xr_array_member_shape(member_metadata[operation->metadata_begin],
                                    operation->operand_count)
            : NULL;
    bool string_result =
        result_shape && result_shape->result_shape == XR_ARRAY_MEMBER_RESULT_STRING;
    if (!receiver_result && !string_result &&
        !call_type_is_exact_scalar(builder->semantic_plan, operation->result_type))
        return fail(error, error_size, "XR_TARGET_1003",
                    "Array member result storage is incomplete");
    const XrSemanticTypeRecord *receiver_type =
        xr_semantic_plan_type(builder->semantic_plan, receiver_type_index);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = tagged_store ? operation->operand_count : 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = string_result ? XR_TARGET_CALL_RETURN_OWNED : XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_ARRAY_MEMBER_SCALAR,
        .target_kind = XR_TARGET_CALL_TARGET_ARRAY_MEMBER_SCALAR,
        .array_element_storage =
            tagged_store ? XR_TARGET_ARRAY_STORAGE_TAGGED : XR_TARGET_ARRAY_STORAGE_NONE,
    };
    if (!receiver_type ||
        !stable_identity_from_pair("xray-target-array-member-scalar-v1", operation->id,
                                   receiver_type->id, element, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "Array member call identity is incomplete");
    if (tagged_store) {
        uint32_t call_intent = builder->call_intent_count;
        for (uint16_t ordinal = 0; ordinal < operation->operand_count; ordinal++) {
            uint32_t semantic_operand = operation->operand_begin + ordinal;
            const XrSemanticOperandRecord *operand = &operands[semantic_operand];
            const XrSemanticTypeRecord *type =
                xr_semantic_plan_type(builder->semantic_plan, operand->type);
            XrTargetCallArgumentIntent argument = {
                .call_intent = call_intent,
                .semantic_operand = semantic_operand,
                .semantic_value = operand->value,
                .caller_storage_value = operand->value,
                .callee_parameter = XR_SEMANTIC_INDEX_NONE,
                .ordinal = ordinal,
                .mode = XR_TARGET_CALL_VALUE,
                .ownership = ordinal == 0 ? XR_TARGET_CALL_BORROW : XR_TARGET_CALL_CONSUME,
                .transfer_mode = operand->transfer_mode,
                .flags = 0,
                .array_element_storage = semantic_operand == tagged_store_operand
                                             ? XR_TARGET_ARRAY_STORAGE_TAGGED
                                             : XR_TARGET_ARRAY_STORAGE_NONE,
            };
            if (!type ||
                !stable_identity_from_pair("xray-target-array-member-tagged-store-argument-v1",
                                           operation->id, type->id, ordinal, &argument.identity) ||
                !append_call_argument_intent(builder, &argument, error, error_size))
                return false;
        }
    }
    return append_call_intent(builder, &call, error, error_size);
}

/* The native stdlib namespace member owns no callee function index: the frozen
 * definition registry names its implementation, and the receiver is a namespace
 * handle rather than a source argument, so the row carries no argument intent
 * and folds the proven arity into its identity. The result claims no ownership
 * because a plain scalar leaves nothing to release. */
static bool collect_native_module_scalar_call_intent(XrTargetPlanBuilder *builder,
                                                     uint32_t operation_index,
                                                     const XrSemanticOperationRecord *operation,
                                                     char *error, size_t error_size) {
    uint32_t arity = 0;
    if (!semantic_native_module_scalar_call_is_exact(builder->semantic_plan, operation, &arity) ||
        !call_type_is_exact_scalar(builder->semantic_plan, operation->result_type))
        return fail(error, error_size, "XR_TARGET_1003",
                    "native module scalar dispatch authority is incomplete");
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_NATIVE_MODULE_SCALAR,
        .target_kind = XR_TARGET_CALL_TARGET_NATIVE_MODULE_SCALAR,
    };
    if (!result_type ||
        !stable_identity_from_pair("xray-target-native-module-scalar-v1", operation->id,
                                   result_type->id, arity, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "native module scalar call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

/* A payload-bearing source enum constructor has no callee ABI: its namespace
 * receiver and payloads are frozen semantic inputs to the materialization
 * recipe. The call row owns the exact constructor dispatch and the fresh
 * tagged result, while CEmissionPlan projects the ordered payload recipe. */
static bool collect_adt_enum_constructor_call_intent(XrTargetPlanBuilder *builder,
                                                     uint32_t operation_index,
                                                     const XrSemanticOperationRecord *operation,
                                                     char *error, size_t error_size) {
    XrSemanticAdtEnumConstructorShape shape = {0};
    const XrSemanticTypeRecord *result_type =
        operation ? xr_semantic_plan_type(builder->semantic_plan, operation->result_type) : NULL;
    if (!xr_semantic_adt_enum_constructor_is_exact(builder->semantic_plan, operation, &shape) ||
        !result_type)
        return fail(error, error_size, "XR_TARGET_1003",
                    "ADT enum constructor dispatch authority is incomplete");
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_ADT_ENUM_CONSTRUCTOR,
        .target_kind = XR_TARGET_CALL_TARGET_ADT_ENUM_CONSTRUCTOR,
    };
    if (!stable_identity_from_pair("xray-target-adt-enum-constructor-v1", operation->id,
                                   result_type->id, shape.member_ordinal, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "ADT enum constructor call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_direct_local_call_intent(XrTargetPlanBuilder *builder, uint32_t target_index,
                                             const XrSemanticCallTargetRecord *target,
                                             bool suspends, bool callee_suspendable, char *error,
                                             size_t error_size) {
    const XrSemanticPlan *plan = builder->semantic_plan;
    uint32_t operand_table_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_table_count);
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(plan, target->operation);
    const XrSemanticFunctionRecord *callee = xr_semantic_plan_function(plan, target->function);
    /* Two spellings reach this family. A direct local call puts the callee in
     * operand 0 and the declared parameters in the rest. A source instance
     * method resolved to one body puts the receiver in operand 0, and that
     * receiver IS the callee's first parameter -- `this` is declared like any
     * other, carrying the same type, mode and transfer as the operand filling
     * it. The two therefore differ by exactly one position: the method's
     * operands line up with the parameters one-to-one, while a direct call's
     * are shifted by the callee operand, which fills no parameter. */
    bool method = xr_semantic_local_call_operand_shift(target) == 0u;
    uint32_t operand_shift = xr_semantic_local_call_operand_shift(target);
    if (!operation || !callee ||
        !xr_semantic_call_target_names_local_function(target, operation,
                                                      xr_semantic_plan_function_count(plan)) ||
        operation->function >= xr_semantic_plan_function_count(plan) ||
        operation->operand_count == 0 || operation->operand_begin > operand_table_count ||
        operation->operand_count > operand_table_count - operation->operand_begin)
        return fail(error, error_size, "XR_TARGET_1003",
                    "only exact local call operations are supported");
    bool expected_suspend =
        (operation->effects & XI_EFFECT_MAY_SUSPEND) != 0 || operation->opcode == XI_GO ||
        ((operation->opcode == XI_CALL || operation->opcode == XI_CALL_METHOD) &&
         callee_suspendable);
    if (expected_suspend != suspends)
        return fail(error, error_size, "XR_TARGET_1003",
                    "coroutine call lacks exact suspension-state authority");
    const XrSemanticProgramTypeBinding *leaf_aggregate = NULL;
    bool exact_leaf_aggregate =
        !method && !suspends &&
        semantic_leaf_program_direct_call_is_exact(plan, target->operation, operation, callee,
                                                   &leaf_aggregate) &&
        target->function ==
            xr_semantic_plan_program_call_for_operation(plan, target->operation)->target_function;
    const XrSemanticProgramTypeBinding *leaf_product = NULL;
    bool exact_leaf_product =
        !method && !suspends &&
        semantic_product_direct_call_is_exact(plan, target->operation, operation, callee,
                                              &leaf_product) &&
        target->function ==
            xr_semantic_plan_program_call_for_operation(plan, target->operation)->target_function;
    if (callee->parameter_count == UINT16_MAX ||
        operation->operand_count != (uint32_t) callee->parameter_count + operand_shift ||
        callee->parameter_begin > xr_semantic_plan_parameter_count(plan) ||
        callee->parameter_count >
            xr_semantic_plan_parameter_count(plan) - callee->parameter_begin ||
        operation->result_type != callee->return_type ||
        (!call_type_is_exact_scalar(plan, operation->result_type) &&
         !semantic_direct_local_nullable_scalar_result_is_exact(plan, operation, callee) &&
         !semantic_direct_local_string_result_is_exact(plan, operation, callee) &&
         !xr_semantic_direct_local_adt_enum_result_is_exact(plan, operation, callee) &&
         !xr_semantic_direct_local_unit_enum_result_is_exact(plan, operation, callee) &&
         !semantic_direct_local_array_result_is_exact(plan, operation, callee) &&
         xr_semantic_class_instance_result_source_class(plan, operation) ==
             XR_SEMANTIC_INDEX_NONE &&
         !exact_leaf_aggregate && !exact_leaf_product)) {
        if (target_trace_enabled()) {
            fprintf(stderr,
                    "[target] refused in direct-local signature: the call's arity, its result type "
                    "or the storage of that result is not one this family binds\n");
            target_trace_operation(plan, target->operation, operation);
            fprintf(stderr,
                    "[target]   callee function=%u, declares %u parameters from parameter %u\n",
                    target->function, callee->parameter_count, callee->parameter_begin);
            target_trace_equality("operand count vs declared arity plus callee",
                                  (unsigned long long) callee->parameter_count + operand_shift,
                                  operation->operand_count);
            target_trace_equality("call result type vs callee return type", callee->return_type,
                                  operation->result_type);
            fprintf(stderr,
                    "[target]   the result type must land in one storage family, and each was "
                    "asked in turn:\n");
            target_trace_type(plan, "the type every family below was asked about",
                              operation->result_type);
            target_trace_judgement("result is an exact scalar",
                                   call_type_is_exact_scalar(plan, operation->result_type));
            target_trace_judgement(
                "result is an exact nullable scalar",
                semantic_direct_local_nullable_scalar_result_is_exact(plan, operation, callee));
            target_trace_judgement(
                "result is an exact String",
                semantic_direct_local_string_result_is_exact(plan, operation, callee));
            target_trace_judgement(
                "result is an exact ADT enum",
                xr_semantic_direct_local_adt_enum_result_is_exact(plan, operation, callee));
            target_trace_judgement(
                "result is an exact unit enum",
                xr_semantic_direct_local_unit_enum_result_is_exact(plan, operation, callee));
            target_trace_judgement(
                "result is an exact Array",
                semantic_direct_local_array_result_is_exact(plan, operation, callee));
            target_trace_judgement("result is an exact class instance",
                                   xr_semantic_class_instance_result_source_class(
                                       plan, operation) != XR_SEMANTIC_INDEX_NONE);
            target_trace_judgement("result is an exact typed leaf aggregate", exact_leaf_aggregate);
            target_trace_judgement("result is an exact typed leaf product", exact_leaf_product);
            fprintf(stderr,
                    "[target]   read it as: an arity or type line marked \"differs\" is the whole "
                    "refusal; if all of them match, the result type reached no storage family and "
                    "the last five lines say which families declined it.\n");
        }
        uint32_t result_storage_mask =
            (call_type_is_exact_scalar(plan, operation->result_type)
                 ? XR_TARGET_SURVEY_STORAGE_SCALAR
                 : 0u) |
            (semantic_direct_local_nullable_scalar_result_is_exact(plan, operation, callee)
                 ? XR_TARGET_SURVEY_STORAGE_NULLABLE_SCALAR
                 : 0u) |
            (semantic_direct_local_string_result_is_exact(plan, operation, callee)
                 ? XR_TARGET_SURVEY_STORAGE_STRING
                 : 0u) |
            (xr_semantic_direct_local_adt_enum_result_is_exact(plan, operation, callee)
                 ? XR_TARGET_SURVEY_STORAGE_ADT_ENUM
                 : 0u) |
            (xr_semantic_direct_local_unit_enum_result_is_exact(plan, operation, callee)
                 ? XR_TARGET_SURVEY_STORAGE_UNIT_ENUM
                 : 0u) |
            (semantic_direct_local_array_result_is_exact(plan, operation, callee)
                 ? XR_TARGET_SURVEY_STORAGE_ARRAY
                 : 0u) |
            (xr_semantic_class_instance_result_source_class(plan, operation) !=
                     XR_SEMANTIC_INDEX_NONE
                 ? XR_TARGET_SURVEY_STORAGE_CLASS_INSTANCE
                 : 0u) |
            (exact_leaf_aggregate ? XR_TARGET_SURVEY_STORAGE_LEAF_AGGREGATE : 0u) |
            (exact_leaf_product ? XR_TARGET_SURVEY_STORAGE_LEAF_PRODUCT : 0u);
        char detail[384];
        snprintf(detail, sizeof(detail),
                 "direct-local signature or result storage is incomplete opcode=%u "
                 "operand-count=%u parameter-count=%u result-type-match=%u storage-mask=%u "
                 "method=%u",
                 operation->opcode, operation->operand_count, callee->parameter_count,
                 operation->result_type == callee->return_type, result_storage_mask, method);
        return fail(error, error_size, "XR_TARGET_1003", detail);
    }
    /* Only a direct call has a head operand outside the parameter list. The
     * method's receiver fills parameter 0, so the loop below checks it under
     * the same contract every other argument gets. */
    const XrSemanticOperandRecord *callee_operand = &operands[operation->operand_begin];
    if (!method &&
        (callee_operand->role != XR_SEM_OPERAND_CALLEE || callee_operand->parameter != -1 ||
         (callee_operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) != 0))
        return fail(error, error_size, "XR_TARGET_1003",
                    "direct-local callee operand is not exact");

    XrTargetCallIntent call = {
        .semantic_call_target = target_index,
        .semantic_operation = target->operation,
        .caller_function = operation->function,
        .callee_function = target->function,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = callee->parameter_count,
        .result_mode = (exact_leaf_aggregate || exact_leaf_product)
                           ? XR_TARGET_CALL_CALLER_STORAGE
                           : XR_TARGET_CALL_VALUE,
        .result_ownership =
            (semantic_direct_local_string_result_is_exact(plan, operation, callee) ||
             xr_semantic_direct_local_adt_enum_result_is_exact(plan, operation, callee) ||
             semantic_direct_local_array_result_is_exact(plan, operation, callee) ||
             xr_semantic_class_instance_result_source_class(plan, operation) !=
                 XR_SEMANTIC_INDEX_NONE)
                ? XR_TARGET_CALL_RETURN_OWNED
                : XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL,
        .target_kind = XR_TARGET_CALL_TARGET_DIRECT_LOCAL,
        .suspends = suspends,
        .tail = operation->opcode == XI_TAIL_CALL,
    };
    if (!stable_identity_from_pair("xray-target-call-v5", target->id, operation->id, 0,
                                   &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "direct-local call identity is incomplete");
    uint32_t call_intent = builder->call_intent_count;
    for (uint32_t ordinal = 0; ordinal < callee->parameter_count; ordinal++) {
        uint32_t parameter_index = callee->parameter_begin + ordinal;
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan, parameter_index);
        uint32_t semantic_operand = operation->operand_begin + ordinal + operand_shift;
        const XrSemanticOperandRecord *operand = &operands[semantic_operand];
        bool exact_scalar = call_type_is_exact_scalar(plan, operand->type);
        /* A raw pointer argument may state a mutability the parameter does not
         * ask for. The two records are otherwise one representation, so this is
         * the one place a direct-local argument type is allowed to differ from
         * the parameter it fills. */
        bool pointer_weakens = parameter && operand->type != parameter->type &&
                               xr_semantic_raw_pointer_argument_satisfies_parameter(
                                   xr_semantic_plan_type(plan, operand->type),
                                   xr_semantic_plan_type(plan, parameter->type));
        bool exact_u8_slice = semantic_u8_slice_parameter_is_exact(plan, parameter) &&
                              semantic_u8_slice_type_is_exact(plan, operand->type);
        bool exact_unit_enum =
            parameter && parameter->type == operand->type &&
            xr_semantic_unit_enum_type_is_exact(xr_semantic_plan_type(plan, operand->type));
        bool exact_adt_enum =
            parameter && parameter->type == operand->type &&
            xr_semantic_adt_enum_type_is_exact(xr_semantic_plan_type(plan, operand->type));
        uint8_t array_element_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        uint32_t caller_storage_value = operand->value;
        bool exact_tagged_ref =
            parameter && parameter->type == operand->type &&
            semantic_direct_local_tagged_ref_parameter_is_exact(plan, parameter,
                                                                &array_element_storage) &&
            operand->parameter_mode == XR_PARAM_REF && operand->access == XR_CALL_ARG_REF &&
            operand->origin != XI_PLACE_ORIGIN_NONE &&
            operand->lifetime == XI_PLACE_LIFETIME_CALL_BOUND &&
            operand->escape == XI_PLACE_ESCAPE_NONE &&
            operand->ownership_action == XR_SEM_OPERAND_BORROW &&
            operand->transfer_mode == XR_TRANSFER_SHARE &&
            operand->flags == (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE) &&
            semantic_direct_local_tagged_ref_place_is_exact(plan, operand, &caller_storage_value);
        /* A class instance is admitted as an argument through the same shared
         * judgement that binds its storage on the callee side, so a parameter
         * this call passes can never be one the callee's own family refused. */
        /* A receiver is a class instance crossing a parameter boundary just as
         * a declared class parameter is; the shared judgement covers both, plus
         * the constructor receiver. Asking only about the declared-parameter
         * form would refuse every `this`, whose type row is the anonymous
         * instance that names no declaration. */
        bool exact_class_instance =
            parameter && parameter->type == operand->type &&
            xr_semantic_class_instance_parameter_source_class(plan, parameter_index) !=
                XR_SEMANTIC_INDEX_NONE &&
            xr_semantic_class_parameter_call_transfer_is_exact(plan, parameter_index, operand);
        /* An Array handed over by value. It travels the plain argument path a
         * scalar takes -- the tagged value is copied, the allocation is shared
         * -- so it states no place and no element storage of its own. Whatever
         * produced the value the caller names, a construction or a shared read,
         * was bound by the family that owns that shape. */
        uint8_t array_value_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        bool exact_array_value = parameter && parameter->type == operand->type &&
                                 semantic_direct_local_array_value_parameter_is_exact(
                                     plan, parameter, &array_value_storage) &&
                                 operand->ownership_action == XR_SEM_OPERAND_BORROW &&
                                 operand->transfer_mode == XR_TRANSFER_SHARE;
        /* A String handed over by value. It reaches the callee the same way an
         * Array by value does -- the tagged value is copied, the allocation is
         * shared -- because both are reference-capable containers whose one
         * storage fact is that tagged outer value. Whatever produced the value
         * the caller names, a literal, a concatenation or a call result, was
         * bound by the family that owns that shape.
         *
         * The two sides must agree about the allocation, not merely about the
         * carrier: a callee that holds an owning reference is one the caller
         * hands its own over to, and a callee that borrows is one the caller
         * keeps answering for. The declaration proves which, and the call site
         * has to say the same thing. */
        bool string_callee_owns = false;
        bool exact_string_value =
            parameter && parameter->type == operand->type &&
            xr_semantic_direct_local_string_value_parameter_is_exact(plan, parameter,
                                                                     &string_callee_owns) &&
            operand->ownership_action ==
                (string_callee_owns ? XR_SEM_OPERAND_CONSUME : XR_SEM_OPERAND_BORROW) &&
            operand->transfer_mode == XR_TRANSFER_SHARE;
        XrSemanticManagedAggregateArgumentShape managed_aggregate_shape;
        bool exact_managed_aggregate =
            !method && xr_semantic_direct_local_managed_aggregate_argument_is_exact(
                           plan, operation, callee, ordinal, &managed_aggregate_shape);
        bool exact_leaf_aggregate_argument = exact_leaf_aggregate && ordinal == 0 &&
                                             leaf_aggregate && parameter &&
                                             parameter->type == leaf_aggregate->semantic_type &&
                                             operand->type == leaf_aggregate->semantic_type;
        bool exact_leaf_product_argument = exact_leaf_product && ordinal == 0 && leaf_product &&
                                           parameter &&
                                           parameter->type == leaf_product->semantic_type &&
                                           operand->type == leaf_product->semantic_type;
        /* The receiver sits at parameter 0 and is spelled as a receiver, not
         * an argument, and it names no argument ordinal of its own. Every
         * later operand states the argument ordinal it fills, counted without
         * the receiver. */
        bool receiver_slot = method && ordinal == 0;
        uint8_t expected_role = receiver_slot ? XR_SEM_OPERAND_RECEIVER : XR_SEM_OPERAND_ARGUMENT;
        int16_t expected_parameter = method ? (int16_t) ((int32_t) ordinal - 1) : (int16_t) ordinal;
        if (!parameter || operand->role != expected_role ||
            operand->parameter != expected_parameter ||
            (operand->type != parameter->type && !pointer_weakens) ||
            operand->parameter_mode != parameter->mode ||
            operand->transfer_mode != parameter->transfer_mode ||
            (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0 ||
            (!exact_scalar && !exact_u8_slice && !exact_unit_enum && !exact_adt_enum &&
             !exact_class_instance && !exact_tagged_ref && !exact_array_value &&
             !exact_string_value && !exact_leaf_aggregate_argument &&
             !exact_leaf_product_argument) ||
            (!exact_tagged_ref &&
             (parameter->mode != XR_PARAM_READ || operand->access != XR_CALL_ARG_PLAIN ||
              (operand->flags & XR_SEM_OPERAND_ADDRESSABLE) != 0)) ||
            /* A String by value is absent from the table below because it does
             * not need one: its own judgement already admitted exactly the two
             * ownerships a String parameter may declare and required the call
             * site to state the matching one, so a second table restating half
             * of that would be the narrower of two spellings of one rule. */
            (parameter->ownership != XI_OWN_NONE && !exact_string_value && !exact_class_instance &&
             !(exact_adt_enum && parameter->ownership == XI_OWN_OWNED) &&
             !((exact_u8_slice || exact_unit_enum || exact_adt_enum || exact_tagged_ref ||
                exact_array_value) &&
               parameter->ownership == XI_OWN_BORROWED))) {
            if (target_trace_enabled()) {
                fprintf(stderr,
                        "[target] refused in direct-local argument contract: argument %u of this "
                        "call reached no storage family, or reached one whose ownership the "
                        "parameter does not state\n",
                        ordinal);
                target_trace_operation(plan, target->operation, operation);
                if (!parameter) {
                    fprintf(stderr, "[target]   parameter=%u      <no record at this index>\n",
                            parameter_index);
                } else {
                    fprintf(stderr,
                            "[target]   parameter=%u      ordinal %u of callee function %u, "
                            "semantic type %u, mode %u, transfer %u, ownership %u\n",
                            parameter_index, parameter->ordinal, parameter->function,
                            parameter->type, parameter->mode, parameter->transfer_mode,
                            parameter->ownership);
                    fprintf(stderr, "[target]   the argument operand and the parameter must agree "
                                    "exactly:\n");
                    target_trace_equality("operand type vs parameter type", parameter->type,
                                          operand->type);
                    target_trace_equality("operand mode vs parameter mode", parameter->mode,
                                          operand->parameter_mode);
                    target_trace_equality("operand transfer vs parameter transfer",
                                          parameter->transfer_mode, operand->transfer_mode);
                }
                target_trace_equality("operand parameter ordinal",
                                      (unsigned long long) (int64_t) expected_parameter,
                                      (unsigned long long) (int64_t) operand->parameter);
                target_trace_judgement(receiver_slot ? "operand role is receiver"
                                                     : "operand role is argument",
                                       operand->role == expected_role);
                target_trace_judgement("operand carries the call contract flag",
                                       (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) != 0);
                fprintf(stderr,
                        "[target]   the argument must land in one storage family, and each was "
                        "asked in turn:\n");
                target_trace_type(plan, "the type every family below was asked about",
                                  operand->type);
                target_trace_judgement("argument is an exact scalar", exact_scalar);
                target_trace_judgement("argument is an exact u8 slice", exact_u8_slice);
                target_trace_judgement("argument is an exact unit enum", exact_unit_enum);
                target_trace_judgement("argument is an exact ADT enum", exact_adt_enum);
                target_trace_judgement(receiver_slot ? "receiver is an exact class instance"
                                                     : "argument is an exact class instance",
                                       exact_class_instance);
                target_trace_judgement("argument is an exact tagged reference", exact_tagged_ref);
                target_trace_judgement("argument is an exact Array by value", exact_array_value);
                target_trace_judgement("argument is an exact String by value", exact_string_value);
                target_trace_judgement("argument is an exact typed leaf aggregate by value",
                                       exact_leaf_aggregate_argument);
                target_trace_judgement("argument is an exact managed aggregate precursor",
                                       exact_managed_aggregate);
                if (parameter && !exact_tagged_ref) {
                    fprintf(stderr,
                            "[target]   everything but an Array by reference travels the plain "
                            "read path:\n");
                    target_trace_judgement("parameter mode is READ",
                                           parameter->mode == XR_PARAM_READ);
                    target_trace_judgement("operand access is PLAIN",
                                           operand->access == XR_CALL_ARG_PLAIN);
                    target_trace_judgement("operand is not addressable",
                                           (operand->flags & XR_SEM_OPERAND_ADDRESSABLE) == 0);
                }
                if (parameter && parameter->ownership != XI_OWN_NONE)
                    fprintf(stderr,
                            "[target]   the parameter states ownership %u; an owned class, String "
                            "or ADT enum requires a consuming operand, while a borrowed "
                            "reference-capable value requires a borrowing operand; a plain "
                            "scalar must state none\n",
                            parameter->ownership);
                if (method)
                    fprintf(stderr,
                            "[target]   read it as: ordinal %u is a parameter position and "
                            "operand[%u] fills it -- the receiver fills parameter 0, so operands "
                            "and parameters line up one to one here.\n",
                            ordinal, ordinal);
                else
                    fprintf(stderr,
                            "[target]   read it as: ordinal %u is the argument position, not an "
                            "operand index -- the operand it names is operand[%u] above, because "
                            "operand[0] is the callee.\n",
                            ordinal, ordinal + 1u);
            }
            if (exact_managed_aggregate)
                return fail(error, error_size, "XR_TARGET_1003",
                            "direct-local managed aggregate needs frozen clone, drop, root, and "
                            "generation authority");
            uint32_t argument_storage_mask =
                (exact_scalar ? XR_TARGET_SURVEY_STORAGE_SCALAR : 0u) |
                (exact_u8_slice ? XR_TARGET_SURVEY_STORAGE_U8_SLICE : 0u) |
                (exact_unit_enum ? XR_TARGET_SURVEY_STORAGE_UNIT_ENUM : 0u) |
                (exact_adt_enum ? XR_TARGET_SURVEY_STORAGE_ADT_ENUM : 0u) |
                (exact_class_instance ? XR_TARGET_SURVEY_STORAGE_CLASS_INSTANCE : 0u) |
                (exact_tagged_ref ? XR_TARGET_SURVEY_STORAGE_TAGGED_REFERENCE : 0u) |
                (exact_array_value ? XR_TARGET_SURVEY_STORAGE_ARRAY : 0u) |
                (exact_string_value ? XR_TARGET_SURVEY_STORAGE_STRING : 0u) |
                (exact_leaf_aggregate_argument ? XR_TARGET_SURVEY_STORAGE_LEAF_AGGREGATE : 0u) |
                (exact_leaf_product_argument ? XR_TARGET_SURVEY_STORAGE_LEAF_PRODUCT : 0u);
            char detail[512];
            snprintf(
                detail, sizeof(detail),
                "direct-local argument contract needs unsupported storage or ownership "
                "opcode=%u parameter-ordinal=%u storage-mask=%u operand-mode=%u "
                "parameter-mode=%u operand-transfer=%u parameter-transfer=%u "
                "operand-ownership=%u parameter-ownership=%u operand-access=%u "
                "operand-role=%u expected-role=%u type-match=%u ordinal-match=%u "
                "contract-flag=%u addressable=%u",
                operation->opcode, ordinal, argument_storage_mask, operand->parameter_mode,
                parameter ? parameter->mode : UINT32_MAX, operand->transfer_mode,
                parameter ? parameter->transfer_mode : UINT32_MAX, operand->ownership_action,
                parameter ? parameter->ownership : UINT32_MAX, operand->access, operand->role,
                expected_role, parameter && operand->type == parameter->type,
                operand->parameter == expected_parameter,
                (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) != 0,
                (operand->flags & XR_SEM_OPERAND_ADDRESSABLE) != 0);
            return fail(error, error_size, "XR_TARGET_1003", detail);
        }
        XrTargetCallArgumentIntent argument = {
            .call_intent = call_intent,
            .semantic_operand = semantic_operand,
            .semantic_value = operand->value,
            .caller_storage_value = caller_storage_value,
            .callee_parameter = parameter_index,
            .ordinal = (uint16_t) ordinal,
            .mode = exact_tagged_ref ? XR_TARGET_CALL_REFERENCE : XR_TARGET_CALL_VALUE,
            .ownership = exact_tagged_ref ? XR_TARGET_CALL_BORROW
                         : operand->ownership_action == XR_SEM_OPERAND_CONSUME
                             ? XR_TARGET_CALL_CONSUME
                             : XR_TARGET_CALL_READ,
            .transfer_mode = operand->transfer_mode,
            .flags = exact_tagged_ref ? XR_TARGET_CALL_ARGUMENT_ADDRESSABLE : 0,
            .array_element_storage = array_element_storage,
        };
        const char *argument_identity_domain = exact_tagged_ref
                                                   ? "xray-target-direct-tagged-ref-argument-v2"
                                                   : "xray-target-call-argument-v1";
        if (!stable_identity_from_pair(argument_identity_domain, target->id, parameter->id, ordinal,
                                       &argument.identity) ||
            !append_call_argument_intent(builder, &argument, error, error_size))
            return false;
    }
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_source_export_call_intent(XrTargetPlanBuilder *builder, uint32_t target_index,
                                              const XrSemanticCallTargetRecord *target,
                                              bool suspends, char *error, size_t error_size) {
    const XrSemanticPlan *plan = builder->semantic_plan;
    const XrSemanticPlan *dependency =
        target && target->dependency < builder->semantic_dependency_count
            ? builder->semantic_dependencies[target->dependency]
            : NULL;
    const XrSemanticSourceExportRecord *source_export =
        dependency && target->source_export < xr_semantic_plan_source_export_count(dependency)
            ? xr_semantic_plan_source_export(dependency, target->source_export)
            : NULL;
    const XrSemanticFunctionRecord *callee =
        source_export && source_export->kind == XR_SEM_SOURCE_EXPORT_FUNCTION
            ? xr_semantic_plan_function(dependency, source_export->function)
            : NULL;
    const XrSemanticOperationRecord *operation =
        target ? xr_semantic_plan_operation(plan, target->operation) : NULL;
    const XrSemanticTypeRecord *result_type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    const XrSemanticTypeRecord *callee_result_type =
        callee ? xr_semantic_plan_type(dependency, callee->return_type) : NULL;
    if (!target || !dependency || !source_export ||
        source_export->kind != XR_SEM_SOURCE_EXPORT_FUNCTION || !callee ||
        !xr_stable_id_equal(source_export->exported_entity, callee->id) || !operation ||
        target->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT ||
        target->function != XR_SEMANTIC_INDEX_NONE ||
        (operation->opcode != XI_CALL_METHOD && operation->opcode != XI_CALL) ||
        operation->operand_count != (uint32_t) callee->parameter_count + 1u ||
        !xr_stable_id_equal(target->export_identity, source_export->id) ||
        !xr_stable_id_equal(target->callee_function, callee->id) || !result_type ||
        !callee_result_type || !xr_stable_id_equal(result_type->id, callee_result_type->id) ||
        !call_type_is_exact_scalar(plan, operation->result_type))
        return fail(error, error_size, "XR_TARGET_1003",
                    "source-export call authority is incomplete");

    XrTargetCallIntent call = {
        .semantic_call_target = target_index,
        .semantic_operation = target->operation,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = target->dependency,
        .source_export = target->source_export,
        .source_export_identity = target->export_identity,
        .source_callee_identity = target->callee_function,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = callee->parameter_count,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_SOURCE_EXPORT,
        .target_kind = XR_TARGET_CALL_TARGET_SOURCE_EXPORT,
        .suspends = suspends,
    };
    if (!stable_identity_from_pair("xray-target-call-v5", target->id, operation->id, 0,
                                   &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "source-export call identity is incomplete");
    uint32_t call_intent = builder->call_intent_count;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!operands ||
        (uint64_t) operation->operand_begin + operation->operand_count > (uint64_t) operand_count)
        return fail(error, error_size, "XR_TARGET_1003", "source-export operands are incomplete");
    for (uint32_t ordinal = 0; ordinal < callee->parameter_count; ordinal++) {
        uint32_t parameter_index = callee->parameter_begin + ordinal;
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(dependency, parameter_index);
        uint32_t semantic_operand = operation->operand_begin + ordinal + 1u;
        const XrSemanticOperandRecord *operand = &operands[semantic_operand];
        const XrSemanticTypeRecord *operand_type = xr_semantic_plan_type(plan, operand->type);
        const XrSemanticTypeRecord *parameter_type =
            parameter ? xr_semantic_plan_type(dependency, parameter->type) : NULL;
        bool reference = parameter && parameter->mode == XR_PARAM_REF;
        bool read = parameter && parameter->mode == XR_PARAM_READ;
        bool addressable = (operand->flags & XR_SEM_OPERAND_ADDRESSABLE) != 0;
        if (!parameter || parameter->function != source_export->function ||
            parameter->ordinal != ordinal || !operand_type || !parameter_type ||
            !xr_semantic_parameter_type_admits_argument(dependency, parameter_type, operand_type) ||
            operand->role != XR_SEM_OPERAND_ARGUMENT || operand->parameter != (int16_t) ordinal ||
            operand->parameter_mode != parameter->mode ||
            operand->transfer_mode != parameter->transfer_mode ||
            (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0 || (!read && !reference) ||
            (read && (operand->access != XR_CALL_ARG_PLAIN || addressable)) ||
            (reference && (operand->access != XR_CALL_ARG_REF || !addressable ||
                           operand->ownership_action != XR_SEM_OPERAND_BORROW)))
            return fail(error, error_size, "XR_TARGET_1003",
                        "source-export argument authority is incomplete");
        XrTargetCallArgumentIntent argument = {
            .call_intent = call_intent,
            .semantic_operand = semantic_operand,
            .semantic_value = operand->value,
            .caller_storage_value = operand->value,
            .callee_parameter = parameter_index,
            .ordinal = (uint16_t) ordinal,
            .mode = reference ? XR_TARGET_CALL_REFERENCE : XR_TARGET_CALL_VALUE,
            .ownership = reference ? XR_TARGET_CALL_WRITEBACK
                         : operand->ownership_action == XR_SEM_OPERAND_CONSUME
                             ? XR_TARGET_CALL_CONSUME
                             : XR_TARGET_CALL_READ,
            .transfer_mode = operand->transfer_mode,
            .flags = reference ? XR_TARGET_CALL_ARGUMENT_ADDRESSABLE : 0,
        };
        if (!stable_identity_from_pair("xray-target-source-call-argument-v1", target->id,
                                       parameter->id, ordinal, &argument.identity) ||
            !append_call_argument_intent(builder, &argument, error, error_size))
            return false;
    }
    return append_call_intent(builder, &call, error, error_size);
}

/* A verified native-namespace yieldable target already names the exact
 * registry member and call operation in SemanticPlan. TargetPlan owns only
 * the target-neutral suspension ABI here: no source dependency, callee index,
 * or backend symbol is invented. Scalar/unit results are the first closed
 * storage domain; reference-returning yieldables remain fail-closed. */
static bool collect_native_namespace_yieldable_call_intent(XrTargetPlanBuilder *builder,
                                                           uint32_t target_index,
                                                           const XrSemanticCallTargetRecord *target,
                                                           bool suspends, char *error,
                                                           size_t error_size) {
    const XrSemanticPlan *plan = builder ? builder->semantic_plan : NULL;
    const XrSemanticOperationRecord *operation =
        target ? xr_semantic_plan_operation(plan, target->operation) : NULL;
    if (!target || !operation || target->kind != XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE ||
        target->function != XR_SEMANTIC_INDEX_NONE ||
        target->dependency != XR_SEMANTIC_INDEX_NONE ||
        target->source_export != XR_SEMANTIC_INDEX_NONE ||
        target->callable_type != XR_SEMANTIC_INDEX_NONE || operation->opcode != XI_CALL_METHOD ||
        operation->operand_count < 1 || !suspends ||
        !call_type_is_exact_scalar(plan, operation->result_type))
        return fail(error, error_size, "XR_TARGET_1003",
                    "native namespace yieldable call authority is incomplete");
    XrTargetCallIntent call = {
        .semantic_call_target = target_index,
        .semantic_operation = target->operation,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_NATIVE_NAMESPACE_YIELDABLE,
        .target_kind = XR_TARGET_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE,
        .suspends = true,
    };
    if (!stable_identity_from_pair("xray-target-native-namespace-yieldable-v1", target->id,
                                   operation->id, operation->operand_count - 1u, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "native namespace yieldable call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

/* The suspending method of a frozen builtin instance. The SemanticPlan target
 * names the receiver type and the roster entry its builtin id and arity select;
 * it names no callee function, so the intent carries none either. The call
 * always parks its caller -- the roster admits nothing that does not -- so the
 * suspension flag is the judgement's own consequence rather than a separate
 * fact rebuilt here. The receiver is the dispatch target, not an argument, and
 * the arguments the roster allows after it are plain scalars the generic
 * argument walk already binds. */
static bool collect_builtin_instance_yieldable_call_intent(XrTargetPlanBuilder *builder,
                                                           uint32_t target_index,
                                                           const XrSemanticCallTargetRecord *target,
                                                           bool suspends, char *error,
                                                           size_t error_size) {
    const XrSemanticPlan *plan = builder ? builder->semantic_plan : NULL;
    const XrSemanticOperationRecord *operation =
        target ? xr_semantic_plan_operation(plan, target->operation) : NULL;
    uint32_t receiver_type = XR_SEMANTIC_INDEX_NONE;
    if (!suspends || !xr_semantic_builtin_instance_yieldable_call_is_exact(plan, target, operation,
                                                                           &receiver_type))
        return fail(error, error_size, "XR_TARGET_1003",
                    "builtin instance yieldable call authority is incomplete");
    const XrSemanticTypeRecord *receiver = xr_semantic_plan_type(plan, receiver_type);
    XrTargetCallIntent call = {
        .semantic_call_target = target_index,
        .semantic_operation = target->operation,
        .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE,
        .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value,
        .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_BUILTIN_INSTANCE_YIELDABLE,
        .target_kind = XR_TARGET_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE,
        .suspends = true,
    };
    if (!receiver ||
        !stable_identity_from_pair("xray-target-builtin-instance-yieldable-v1", target->id,
                                   receiver->id, operation->operand_count - 1u, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "builtin instance yieldable call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool builder_add_calls_and_adapters(XrTargetPlanBuilder *builder, char *error,
                                           size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_CALL_ADAPTER, error, error_size))
        return false;
    const XrSemanticPlan *plan = builder->semantic_plan;
    size_t operation_count = xr_semantic_plan_operation_count(plan);
    size_t call_target_count = xr_semantic_plan_call_target_count(plan);
    if (operation_count > 10000000u || call_target_count > operation_count) {
        builder->poisoned = true;
        return fail(error, error_size, "XR_EXEC_5003", "call target budget exhausted");
    }
    uint32_t *target_by_operation =
        (uint32_t *) allocate_records((uint32_t) operation_count, sizeof(*target_by_operation));
    uint8_t *state_by_operation =
        (uint8_t *) allocate_records((uint32_t) operation_count, sizeof(*state_by_operation));
    uint32_t function_count = (uint32_t) xr_semantic_plan_function_count(plan);
    uint8_t *suspendable = (uint8_t *) allocate_records(function_count, sizeof(*suspendable));
    uint32_t *reverse_head = (uint32_t *) allocate_records(function_count, sizeof(*reverse_head));
    uint32_t *reverse_next =
        (uint32_t *) allocate_records((uint32_t) call_target_count, sizeof(*reverse_next));
    uint32_t *queue = (uint32_t *) allocate_records(function_count, sizeof(*queue));
    if ((operation_count && (!target_by_operation || !state_by_operation)) ||
        (function_count && (!suspendable || !reverse_head || !queue)) ||
        (call_target_count && !reverse_next)) {
        xr_free(target_by_operation);
        xr_free(state_by_operation);
        xr_free(suspendable);
        xr_free(reverse_head);
        xr_free(reverse_next);
        xr_free(queue);
        builder->poisoned = true;
        return fail(error, error_size, "XR_EXEC_5003", "call coverage allocation failed");
    }
    bool valid = true;
    for (uint32_t operation = 0; operation < (uint32_t) operation_count; operation++)
        target_by_operation[operation] = XR_SEMANTIC_INDEX_NONE;
    size_t entity_count = xr_semantic_plan_entity_count(plan);
    for (uint32_t i = 0; i < (uint32_t) entity_count && valid; i++) {
        const XrSemanticEntityRecord *entity = xr_semantic_plan_entity(plan, i);
        if (!entity || entity->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        const XrSemanticOperationRecord *operation =
            entity->subject < operation_count ? xr_semantic_plan_operation(plan, entity->subject)
                                              : NULL;
        if (entity->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION || !operation ||
            operation->function >= function_count || state_by_operation[entity->subject]) {
            valid = fail(error, error_size, "XR_TARGET_1003",
                         "coroutine state authority is missing or duplicated");
            break;
        }
        state_by_operation[entity->subject] = 1;
    }
    for (uint32_t function = 0; function < function_count; function++)
        reverse_head[function] = XR_SEMANTIC_INDEX_NONE;
    uint32_t queue_begin = 0;
    uint32_t queue_end = 0;
    for (uint32_t operation_index = 0; operation_index < (uint32_t) operation_count;
         operation_index++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan, operation_index);
        if (!operation || operation->function >= function_count) {
            valid = fail(error, error_size, "XR_TARGET_1003",
                         "call coverage operation has no owning function authority");
            break;
        }
        if (((operation->effects & XI_EFFECT_MAY_SUSPEND) != 0 || operation->opcode == XI_GO) &&
            !suspendable[operation->function]) {
            suspendable[operation->function] = 1;
            queue[queue_end++] = operation->function;
        }
    }
    /* Every row this family refuses is one gap, and a module usually has more
     * than one. Under a survey each walk below records its refusal and carries
     * on, so the run reports the whole set rather than the lowest-numbered
     * member of it; the first refusal still reaches the caller's error buffer,
     * which is what a normal build reports.
     *
     * Both walks share this bookkeeping because they refuse the same family for
     * two different reasons -- a frozen call target this family does not
     * consume, and a call-shaped operation no family claimed -- and a survey
     * that stopped at the first of one kind would report the second kind as if
     * the module had only the calls the first walk happened to reach. */
    bool survey = false;
    uint32_t refused_rows = 0;
    char first_row[512] = {0};
    for (uint32_t i = 0; i < (uint32_t) call_target_count && valid; i++) {
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(plan, i);
        const XrSemanticOperationRecord *operation =
            target && target->operation < operation_count
                ? xr_semantic_plan_operation(plan, target->operation)
                : NULL;
        bool direct = target && target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL;
        bool source = target && target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT;
        bool native_namespace =
            target && target->kind == XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE;
        bool class_construction =
            target && target->kind == XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR;
        bool builtin_instance =
            target && target->kind == XR_SEM_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE;
        /* A source instance method resolved to one body in this module names a
         * function exactly as a direct local call does, and the plan verifier
         * already builds its suspendability edge. Consume it here on the same
         * terms, or a target the semantic layer proved is refused as uncovered. */
        bool instance_method_local = xr_semantic_call_target_binds_instance_method(
            target, plan, builder->semantic_dependencies, builder->semantic_dependency_count);
        bool names_local_function =
            xr_semantic_call_target_names_local_function(target, operation, function_count);
        if (!target || !operation ||
            (!direct && !source && !native_namespace && !class_construction && !builtin_instance &&
             !instance_method_local) ||
            ((direct || instance_method_local) && !names_local_function) ||
            (source && operation->opcode != XI_CALL_METHOD && operation->opcode != XI_CALL) ||
            (native_namespace && operation->opcode != XI_CALL_METHOD) ||
            (class_construction && operation->opcode != XI_CALL) ||
            (builtin_instance && operation->opcode != XI_CALL_METHOD) ||
            target_by_operation[target->operation] != XR_SEMANTIC_INDEX_NONE) {
            /* A SemanticPlan call-target kind this family does not yet consume
             * is a coverage limit, not an internal inconsistency.  Name the
             * kind, the opcode and the selector: an unnamed refusal here is
             * indistinguishable from an allocation failure, and the evidence
             * needed to extend the family is exactly what the refusal saw. */
            uint32_t metadata_count = 0;
            const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
            const char *selector = operation && operation->metadata_count == 1 &&
                                           operation->metadata_begin < metadata_count
                                       ? metadata[operation->metadata_begin]
                                       : "";
            if (target_trace_enabled()) {
                fprintf(stderr,
                        "[target] refused in call target coverage: SemanticPlan proved a call "
                        "target of kind %s, and this family consumes only DIRECT_LOCAL, "
                        "SOURCE_EXPORT, NATIVE_NAMESPACE_YIELDABLE, BUILTIN_INSTANCE_YIELDABLE, "
                        "SOURCE_INSTANCE_METHOD_LOCAL and SOURCE_CLASS_CONSTRUCTOR\n",
                        target_trace_call_target_kind_name(target ? target->kind : 0u));
                fprintf(stderr,
                        "[target]   call target=%u    kind=%s (%u), names function %u, dependency "
                        "%u\n",
                        i, target_trace_call_target_kind_name(target ? target->kind : 0u),
                        target ? target->kind : 0u,
                        target ? target->function : XR_SEMANTIC_INDEX_NONE,
                        target ? target->dependency : XR_SEMANTIC_INDEX_NONE);
                target_trace_operation(plan, target ? target->operation : XR_SEMANTIC_INDEX_NONE,
                                       operation);
                target_trace_judgement("the operation record exists", operation != NULL);
                target_trace_judgement("this family consumes the kind",
                                       direct || source || native_namespace || class_construction ||
                                           builtin_instance);
                if (direct)
                    target_trace_judgement("DIRECT_LOCAL names a function in range",
                                           target->function < function_count);
                if (direct)
                    target_trace_judgement("DIRECT_LOCAL sits on CALL or TAIL_CALL",
                                           operation && (operation->opcode == XI_CALL ||
                                                         operation->opcode == XI_TAIL_CALL));
                if (source)
                    target_trace_judgement("SOURCE_EXPORT sits on CALL_METHOD or CALL",
                                           operation && (operation->opcode == XI_CALL_METHOD ||
                                                         operation->opcode == XI_CALL));
                if (native_namespace)
                    target_trace_judgement("NATIVE_NAMESPACE_YIELDABLE sits on CALL_METHOD",
                                           operation && operation->opcode == XI_CALL_METHOD);
                if (class_construction)
                    target_trace_judgement("SOURCE_CLASS_CONSTRUCTOR sits on CALL",
                                           operation && operation->opcode == XI_CALL);
                if (builtin_instance)
                    target_trace_judgement("BUILTIN_INSTANCE_YIELDABLE sits on CALL_METHOD",
                                           operation && operation->opcode == XI_CALL_METHOD);
                target_trace_judgement("no earlier target already claimed the operation",
                                       !operation || target_by_operation[target->operation] ==
                                                         XR_SEMANTIC_INDEX_NONE);
                fprintf(
                    stderr,
                    "[target]   read it as: the semantic layer did prove an exact target here. "
                    "The gap is this family's adapter coverage, not a missing proof upstream.\n");
            }
            if (error && error_size)
                snprintf(error, error_size,
                         "XR_TARGET_1003: call target has no consumable adapter authority "
                         "target=%u kind=%u operation=%u opcode=%u selector=%s function=%u",
                         i, target ? target->kind : 0u,
                         target ? target->operation : XR_SEMANTIC_INDEX_NONE,
                         operation ? operation->opcode : 0u, selector,
                         target ? target->function : XR_SEMANTIC_INDEX_NONE);
            if (refused_rows == 0) {
                survey = target_survey_enabled();
                snprintf(first_row, sizeof(first_row), "%s", error && error_size ? error : "");
            }
            refused_rows++;
            if (!survey) {
                valid = false;
                break;
            }
            /* The row stated no authority, so nothing binds this operation and
             * the operation walk below will reach it as an unclaimed call. That
             * second refusal is a consequence of this one rather than an
             * independent gap, which is the bias the survey carries by design. */
            target_survey_row("calls_and_adapters", error);
            continue;
        }
        target_by_operation[target->operation] = i;
        reverse_next[i] = XR_SEMANTIC_INDEX_NONE;
        if (names_local_function) {
            reverse_next[i] = reverse_head[target->function];
            reverse_head[target->function] = i;
        } else if (!class_construction && state_by_operation[target->operation] != 0 &&
                   !suspendable[operation->function]) {
            /* A construction enters no body this row names, and the shared
             * judgement already refused any call that may suspend, so it never
             * makes its caller suspendable. */
            suspendable[operation->function] = 1;
            queue[queue_end++] = operation->function;
        }
    }
    while (valid && queue_begin < queue_end) {
        uint32_t callee = queue[queue_begin++];
        for (uint32_t target_index = reverse_head[callee]; target_index != XR_SEMANTIC_INDEX_NONE;
             target_index = reverse_next[target_index]) {
            const XrSemanticCallTargetRecord *target =
                xr_semantic_plan_call_target(plan, target_index);
            const XrSemanticOperationRecord *operation =
                target ? xr_semantic_plan_operation(plan, target->operation) : NULL;
            if (!operation || operation->function >= function_count) {
                valid = false;
                break;
            }
            if (!suspendable[operation->function]) {
                suspendable[operation->function] = 1;
                queue[queue_end++] = operation->function;
            }
        }
    }
    for (uint32_t i = 0; i < (uint32_t) operation_count && valid; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(plan, i);
        uint32_t target_index = target_by_operation[i];
        if (target_index != XR_SEMANTIC_INDEX_NONE) {
            const XrSemanticCallTargetRecord *target =
                xr_semantic_plan_call_target(plan, target_index);
            if (target && (target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
                           xr_semantic_call_target_binds_instance_method(
                               target, plan, builder->semantic_dependencies,
                               builder->semantic_dependency_count))) {
                valid = collect_direct_local_call_intent(
                    builder, target_index, target, state_by_operation[i] != 0,
                    target->function < function_count && suspendable[target->function] != 0, error,
                    error_size);
            } else if (target && target->kind == XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR) {
                valid = collect_source_class_constructor_call_intent(builder, target_index, target,
                                                                     error, error_size);
            } else if (target && target->kind == XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE) {
                valid = collect_native_namespace_yieldable_call_intent(
                    builder, target_index, target, state_by_operation[i] != 0, error, error_size);
            } else if (target && target->kind == XR_SEM_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE) {
                valid = collect_builtin_instance_yieldable_call_intent(
                    builder, target_index, target, state_by_operation[i] != 0, error, error_size);
            } else if (target && target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT) {
                valid = collect_source_export_call_intent(
                    builder, target_index, target, state_by_operation[i] != 0, error, error_size);
            } else {
                /* Every kind this dispatch covers is named above. Falling
                 * through to one of those collectors would make it refuse on
                 * its own kind check and report a gap in the wrong family; name
                 * the kind that has no collector instead. */
                valid = fail(error, error_size, "XR_TARGET_1003",
                             "call target kind has no intent collector");
                if (target_trace_enabled())
                    fprintf(stderr,
                            "[target] refused in call intent: the adapter family consumed a "
                            "target of kind %s, but no collector builds an intent for that "
                            "kind\n",
                            target_trace_call_target_kind_name(target ? target->kind : 0u));
            }
        } else if (semantic_operation_is_exact_channel_close(plan, operation, NULL)) {
            valid = collect_channel_close_call_intent(builder, i, operation, error, error_size);
        } else if (xr_semantic_container_copy_is_exact(plan, operation, NULL, NULL)) {
            valid = collect_container_copy_call_intent(builder, i, operation, error, error_size);
        } else if (xr_semantic_scalar_copy_is_exact(plan, operation, NULL)) {
            valid = collect_scalar_copy_call_intent(builder, i, operation, error, error_size);
        } else if (semantic_stringbuilder_constructor_is_exact(plan, operation)) {
            valid = collect_stringbuilder_constructor_call_intent(builder, i, operation, error,
                                                                  error_size);
        } else if (semantic_array_intrinsic_is_exact(plan, operation, NULL, NULL)) {
            valid = collect_array_intrinsic_call_intent(builder, i, operation, error, error_size);
        } else if (semantic_array_hof_is_exact(plan, operation, NULL, NULL, NULL, NULL, NULL,
                                               NULL)) {
            valid = collect_array_hof_call_intent(builder, i, operation, error, error_size);
        } else if (semantic_array_fill_scalar_is_exact(plan, operation, NULL, NULL, NULL)) {
            valid = collect_array_fill_scalar_call_intent(builder, i, operation, error, error_size);
        } else if (semantic_string_byte_slice_view_is_exact(plan, operation)) {
            valid = collect_string_byte_slice_view_call_intent(builder, i, operation, error,
                                                               error_size);
        } else if (semantic_stringbuilder_append_rune_is_exact(plan, operation, NULL, NULL)) {
            valid = collect_stringbuilder_append_rune_call_intent(builder, i, operation, error,
                                                                  error_size);
        } else if (xr_semantic_string_runes_is_exact(plan, operation, NULL)) {
            valid = collect_string_runes_call_intent(builder, i, operation, error, error_size);
        } else if (xr_semantic_iterator_rune_has_next_is_exact(plan, operation, NULL)) {
            valid = collect_iterator_rune_has_next_call_intent(builder, i, operation, error,
                                                               error_size);
        } else if (xr_semantic_rune_to_string_is_exact(plan, operation, NULL)) {
            valid = collect_rune_to_string_call_intent(builder, i, operation, error, error_size);
        } else if (xr_semantic_iterator_rune_nth_is_exact(plan, operation, NULL, NULL)) {
            valid = collect_iterator_rune_nth_call_intent(builder, i, operation, error, error_size);
        } else if (xr_semantic_iterator_rune_next_is_exact(plan, operation, NULL)) {
            valid =
                collect_iterator_rune_next_call_intent(builder, i, operation, error, error_size);
        } else if (xr_semantic_map_entries_iterator_is_exact(plan, operation, NULL, NULL) ||
                   xr_semantic_map_entry_iterator_has_next_is_exact(plan, operation, NULL) ||
                   xr_semantic_map_entry_iterator_next_is_exact(plan, operation, NULL)) {
            valid =
                collect_map_entry_iterator_call_intent(builder, i, operation, error, error_size);
        } else if (xr_semantic_rune_to_uint32_is_exact(plan, operation, NULL)) {
            valid = collect_rune_to_uint32_call_intent(builder, i, operation, error, error_size);
        } else if (xr_semantic_rune_is_whitespace_is_exact(plan, operation, NULL)) {
            valid =
                collect_rune_is_whitespace_call_intent(builder, i, operation, error, error_size);
        } else if (xr_semantic_string_slice_range_is_exact(plan, operation, NULL, NULL, NULL)) {
            valid =
                collect_string_slice_range_call_intent(builder, i, operation, error, error_size);
        } else if (semantic_stringbuilder_to_string_is_exact(plan, operation, NULL)) {
            valid = collect_stringbuilder_to_string_call_intent(builder, i, operation, error,
                                                                error_size);
        } else if (semantic_stringbuilder_append_string_is_exact(plan, operation, NULL, NULL)) {
            valid = collect_stringbuilder_append_string_call_intent(builder, i, operation, error,
                                                                    error_size);
        } else if (semantic_json_namespace_value_is_exact(plan, operation, NULL)) {
            valid =
                collect_json_namespace_value_call_intent(builder, i, operation, error, error_size);
        } else if (xr_semantic_panic_info_constructor_is_exact(plan, operation, NULL)) {
            valid = collect_panic_info_constructor_call_intent(builder, i, operation, error,
                                                               error_size);
        } else if (semantic_array_member_scalar_is_exact(plan, operation, NULL, NULL)) {
            valid =
                collect_array_member_scalar_call_intent(builder, i, operation, error, error_size);
        } else if (semantic_native_module_scalar_call_is_exact(plan, operation, NULL)) {
            valid =
                collect_native_module_scalar_call_intent(builder, i, operation, error, error_size);
        } else if (xr_semantic_adt_enum_constructor_is_exact(plan, operation, NULL)) {
            valid =
                collect_adt_enum_constructor_call_intent(builder, i, operation, error, error_size);
        } else if (semantic_operation_is_call_shaped(plan, operation)) {
            uint32_t metadata_count = 0;
            uint32_t operand_count = 0;
            const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
            const XrSemanticOperandRecord *operands =
                xr_semantic_plan_operands(plan, &operand_count);
            const char *selector =
                operation->metadata_count == 1 && operation->metadata_begin < metadata_count
                    ? metadata[operation->metadata_begin]
                    : "";
            const XrSemanticOperandRecord *receiver =
                operation->operand_count > 0 && operation->operand_begin < operand_count
                    ? &operands[operation->operand_begin]
                    : NULL;
            const XrSemanticOperandRecord *argument =
                operation->operand_count > 1 && operation->operand_begin + 1u < operand_count
                    ? &operands[operation->operand_begin + 1u]
                    : NULL;
            if (target_trace_enabled()) {
                fprintf(stderr,
                        "[target] refused in call coverage: this operation is call-shaped, "
                        "SemanticPlan proved no call target for it, and no builtin family here "
                        "claimed it either\n");
                target_trace_operation(plan, i, operation);
                target_trace_judgement("SemanticPlan bound a call target to this operation", false);
                fprintf(stderr,
                        "[target]   every builtin family was asked about this operation and each "
                        "declined; the families are selected by selector and receiver shape, so "
                        "the selector and the operand list above are what decide which one should "
                        "have claimed it.\n");
                target_trace_type(plan, "result type", operation->result_type);
                if (receiver)
                    target_trace_type(plan, "type of operand[0]", receiver->type);
                if (argument)
                    target_trace_type(plan, "type of operand[1]", argument->type);
                uint32_t result_source_class = xr_semantic_class_instance_type_source_class(
                    plan, xr_semantic_plan_type(plan, operation->result_type));
                if (operation->opcode == XI_CALL && result_source_class != XR_SEMANTIC_INDEX_NONE &&
                    receiver) {
                    const XrSemanticOperationRecord *class_load =
                        xr_semantic_class_value_definition(plan, receiver->value);
                    uint32_t loaded_source_class =
                        xr_semantic_class_object_read_source_class(plan, class_load);
                    fprintf(stderr,
                            "[target]   class construction candidate       result-class=%u "
                            "loaded-class=%u exact-operation=%s\n",
                            result_source_class, loaded_source_class,
                            xr_semantic_class_construction_operation_is_exact(operation) ? "yes"
                                                                                         : "NO");
                    target_trace_judgement(
                        "callee operand has the exact class-call contract",
                        receiver->role == XR_SEM_OPERAND_CALLEE && receiver->parameter == -1 &&
                            receiver->transfer_mode == 0 &&
                            receiver->ownership_action == XR_SEM_OPERAND_BORROW &&
                            receiver->parameter_mode == 0 && receiver->access == 0 &&
                            receiver->origin == 0 && receiver->lifetime == 0 &&
                            receiver->escape == 0 && receiver->flags == 0);
                    target_trace_judgement("callee value has one defining operation",
                                           class_load != NULL);
                    target_trace_judgement("callee class matches result class",
                                           loaded_source_class == result_source_class);
                    if (class_load) {
                        target_trace_operation(plan, XR_SEMANTIC_INDEX_NONE, class_load);
                        const XrSemanticFunctionRecord *root = xr_semantic_plan_function(plan, 0);
                        fprintf(stderr,
                                "[target]   class shared authority           slot=%lld "
                                "root-initializer=%s root-entry=%u\n",
                                (long long) class_load->semantic_immediate,
                                root && root->is_module_initializer ? "yes" : "NO",
                                root ? root->block_begin : XR_SEMANTIC_INDEX_NONE);
                        uint32_t diagnostic_operation_count =
                            (uint32_t) xr_semantic_plan_operation_count(plan);
                        for (uint32_t diagnostic_operation = 0;
                             diagnostic_operation < diagnostic_operation_count;
                             diagnostic_operation++) {
                            const XrSemanticOperationRecord *candidate =
                                xr_semantic_plan_operation(plan, diagnostic_operation);
                            if (!candidate || candidate->opcode != XI_SET_SHARED ||
                                candidate->semantic_immediate != class_load->semantic_immediate)
                                continue;
                            fprintf(stderr,
                                    "[target]     matching shared store       operation=%u "
                                    "owner=%u block=%u exact=%s\n",
                                    diagnostic_operation, candidate->function, candidate->block,
                                    xr_semantic_class_shared_store_shape_is_exact(plan, candidate)
                                        ? "yes"
                                        : "NO");
                            target_trace_operation(plan, diagnostic_operation, candidate);
                        }
                    }
                }
                fprintf(stderr,
                        "[target]   read it as: a CALL or TAIL_CALL here means the semantic layer "
                        "could not name a callee at all, while a CALL_METHOD means the selector "
                        "belongs to no family this pass describes.\n");
            }
            if (error && error_size) {
                /* Type indexes are plan-local, so a refusal that names only
                 * them says nothing about what the receiver actually was. The
                 * canonical key is the cross-module identity and is what tells
                 * a reader whether the uncovered call was on an Array, a Map or
                 * a source class. */
                const XrSemanticTypeRecord *receiver_type =
                    receiver ? xr_semantic_plan_type(plan, receiver->type) : NULL;
                snprintf(
                    error, error_size,
                    "XR_TARGET_1003: call-shaped operation has no exact target authority "
                    "operation=%u function=%u opcode=%u selector=%s intrinsic=%u "
                    "immediate=%lld result=value:%u,type:%u,ownership:%u,alias:%d "
                    "operands=%u receiver=value:%u,type:%u,role:%u,flags:%u "
                    "argument=value:%u,type:%u,role:%u,flags:%u receiver-type=%s",
                    i, operation->function, operation->opcode, selector, operation->intrinsic_kind,
                    (long long) operation->semantic_immediate, operation->result_value,
                    operation->result_type, operation->result_ownership,
                    operation->result_alias_operand, operation->operand_count,
                    receiver ? receiver->value : XR_SEMANTIC_INDEX_NONE,
                    receiver ? receiver->type : XR_SEMANTIC_INDEX_NONE,
                    receiver ? receiver->role : 0, receiver ? receiver->flags : 0,
                    argument ? argument->value : XR_SEMANTIC_INDEX_NONE,
                    argument ? argument->type : XR_SEMANTIC_INDEX_NONE,
                    argument ? argument->role : 0, argument ? argument->flags : 0,
                    receiver_type && receiver_type->canonical_key ? receiver_type->canonical_key
                                                                  : "<none>");
            }
            valid = false;
        }
        if (valid)
            continue;
        if (refused_rows == 0) {
            survey = target_survey_enabled();
            snprintf(first_row, sizeof(first_row), "%s", error && error_size ? error : "");
        }
        refused_rows++;
        if (!survey)
            break;
        target_survey_row("calls_and_adapters", error);
        valid = true;
    }
    if (refused_rows) {
        valid = false;
        if (error && error_size)
            snprintf(error, error_size, "%s", first_row);
    }
    xr_free(target_by_operation);
    xr_free(state_by_operation);
    xr_free(suspendable);
    xr_free(reverse_head);
    xr_free(reverse_next);
    xr_free(queue);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_CALL_ADAPTER;
    return true;
}

static bool builder_add_coroutine_state_calls(XrTargetPlanBuilder *builder, char *error,
                                              size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_COROUTINE_STATE_CALL, error, error_size))
        return false;
    size_t state_count = 0;
    size_t entity_count = xr_semantic_plan_entity_count(builder->semantic_plan);
    for (size_t i = 0; i < entity_count; i++) {
        const XrSemanticEntityRecord *entity =
            xr_semantic_plan_entity(builder->semantic_plan, (uint32_t) i);
        state_count += entity && entity->kind == XR_SEM_ENTITY_COROUTINE_STATE;
    }
    if (state_count > 10000000u) {
        builder->poisoned = true;
        return fail(error, error_size, "XR_EXEC_5003", "coroutine state-call budget exhausted");
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_COROUTINE_STATE_CALL;
    return true;
}

static bool builder_add_dynamic_entry_expectations(XrTargetPlanBuilder *builder, char *error,
                                                   size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_DYNAMIC_ENTRY_EXPECTATION, error,
                              error_size))
        return false;
    builder->completed_family_mask |= XR_TARGET_FAMILY_DYNAMIC_ENTRY_EXPECTATION;
    return true;
}

static int find_rep_id(const XrTargetMaterializedPlan *materialized,
                       const XrTargetMachineRepRecord *record) {
    uint32_t low = 0;
    uint32_t high = materialized->machine_rep_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        int order = compare_rep_record(&materialized->machine_reps[middle], record);
        if (order < 0)
            low = middle + 1u;
        else
            high = middle;
    }
    if (low < materialized->machine_rep_count &&
        compare_rep_record(&materialized->machine_reps[low], record) == 0)
        return (int) low;
    return -1;
}

static int find_slot_id(const XrTargetMaterializedPlan *materialized, uint32_t function,
                        XrStableId identity) {
    uint32_t low = 0;
    uint32_t high = materialized->slot_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        const XrTargetSlotRecord *slot = &materialized->slots[middle];
        int order = slot->function < function   ? -1
                    : slot->function > function ? 1
                                                : xr_stable_id_compare(slot->identity, identity);
        if (order < 0)
            low = middle + 1u;
        else
            high = middle;
    }
    if (low < materialized->slot_count && materialized->slots[low].function == function &&
        xr_stable_id_equal(materialized->slots[low].identity, identity))
        return (int) low;
    return -1;
}

static bool materialize_machine_reps(XrTargetPlanBuilder *builder,
                                     XrTargetMaterializedPlan *materialized, char *error,
                                     size_t error_size) {
    if (builder->rep_intent_count > 1u)
        qsort(builder->rep_intents, builder->rep_intent_count, sizeof(*builder->rep_intents),
              compare_rep_intent);
    materialized->machine_rep_count = builder->rep_intent_count;
    materialized->machine_reps = (XrTargetMachineRepRecord *) allocate_records(
        materialized->machine_rep_count, sizeof(*materialized->machine_reps));
    if (materialized->machine_rep_count && !materialized->machine_reps)
        return fail(error, error_size, "XR_EXEC_5003",
                    "machine representation materialization failed");
    for (uint32_t i = 0; i < materialized->machine_rep_count; i++) {
        if (i && compare_rep_record(&builder->rep_intents[i - 1u].record,
                                    &builder->rep_intents[i].record) == 0)
            return fail(error, error_size, "XR_TARGET_1001",
                        "duplicate machine representation intent survived collection");
        materialized->machine_reps[i] = builder->rep_intents[i].record;
        materialized->machine_reps[i].id = i;
    }
    return true;
}

static int find_sorted_layout_intent(const XrTargetPlanBuilder *builder, uint32_t semantic_type) {
    uint32_t low = 0;
    uint32_t high = builder->layout_intent_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (builder->layout_intents[middle].semantic_type < semantic_type)
            low = middle + 1u;
        else
            high = middle;
    }
    return low < builder->layout_intent_count &&
                   builder->layout_intents[low].semantic_type == semantic_type
               ? (int) low
               : -1;
}

static bool semantic_layout_field_type(const XrTargetPlanBuilder *builder,
                                       const XrTargetLayoutIntent *intent,
                                       const XrSemanticTypeRecord *type, uint32_t field_index,
                                       uint32_t *out_child_type, uint32_t *out_semantic_name,
                                       char *error, size_t error_size) {
    if (out_child_type)
        *out_child_type = XR_SEMANTIC_INDEX_NONE;
    if (out_semantic_name)
        *out_semantic_name = XR_SEMANTIC_INDEX_NONE;
    if (!builder || !intent || !type || !out_child_type || !out_semantic_name)
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate field materialization input is incomplete");
    const XrSemanticProgramTypeBinding *leaf_binding = NULL;
    XrTargetLeafProgramTypeKind leaf_kind = semantic_leaf_program_type_kind(
        builder->semantic_plan, intent->semantic_type, &leaf_binding);
    if (leaf_kind == XR_TARGET_LEAF_PROGRAM_TYPE_INVALID)
        return fail(error, error_size, "XR_TARGET_1002", "leaf aggregate field binding is invalid");
    if (leaf_kind == XR_TARGET_LEAF_PROGRAM_TYPE_AGGREGATE) {
        if (intent->element_count != leaf_binding->field_count ||
            !semantic_leaf_program_field_type(builder->semantic_plan, leaf_binding, field_index,
                                              out_child_type))
            return fail(error, error_size, "XR_TARGET_1002",
                        "leaf aggregate field binding is incomplete");
        return true;
    }
    if (semantic_leaf_program_provenance(builder->semantic_plan) &&
        type->kind == XR_KIND_INSTANCE && (type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0)
        return fail(error, error_size, "XR_TARGET_1002",
                    "leaf aggregate field authority is missing");
    XrSemanticValueAggregateShape aggregate_shape = {0};
    bool named_value_aggregate =
        type->kind == XR_KIND_INSTANCE && (type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0;
    if (named_value_aggregate &&
        !xr_semantic_value_aggregate_shape_for_type(builder->semantic_plan,
                                                    intent->semantic_type, &aggregate_shape))
        return fail(error, error_size, "XR_TARGET_1002",
                    "value aggregate field identity is incomplete");
    uint32_t child_table_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(builder->semantic_plan, &child_table_count);
    uint32_t child_ordinal = type->kind == XR_KIND_FIXED_ARRAY ? 0u : field_index;
    if (!children || type->child_begin > child_table_count ||
        type->child_count > child_table_count - type->child_begin ||
        child_ordinal >= type->child_count)
        return fail(error, error_size, "XR_TARGET_1002", "aggregate field type range is invalid");
    *out_child_type = children[type->child_begin + child_ordinal];
    if (named_value_aggregate)
        *out_semantic_name = aggregate_shape.field_metadata_begin + field_index;
    return true;
}

static bool materialize_layout_geometry(XrTargetPlanBuilder *builder,
                                        XrTargetMaterializedPlan *materialized, uint32_t index,
                                        uint8_t *states, char *error, size_t error_size) {
    if (states[index] == 2)
        return true;
    if (states[index] == 1)
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate layout dependency is recursive");
    states[index] = 1;
    XrTargetLayoutIntent *intent = &builder->layout_intents[index];
    XrTargetLayoutRecord *layout = &materialized->layouts[index];
    if (intent->kind == XR_TARGET_LAYOUT_SCALAR || intent->kind == XR_TARGET_LAYOUT_DYNAMIC ||
        intent->kind == XR_TARGET_LAYOUT_VIEW) {
        layout->align = intent->memory_rep.memory_align;
        layout->fixed_prefix_size = intent->memory_rep.memory_size;
        states[index] = 2;
        return true;
    }
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(builder->semantic_plan, intent->semantic_type);
    if (!type)
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate layout has no exact semantic field range");
    uint32_t offset = 0;
    uint32_t aggregate_align = 1;
    for (uint32_t field_index = 0; field_index < intent->element_count; field_index++) {
        uint32_t child_type = XR_SEMANTIC_INDEX_NONE;
        uint32_t semantic_name = XR_SEMANTIC_INDEX_NONE;
        if (!semantic_layout_field_type(builder, intent, type, field_index, &child_type,
                                        &semantic_name, error, error_size))
            return false;
        int child_layout = find_sorted_layout_intent(builder, child_type);
        if (child_layout < 0 ||
            !materialize_layout_geometry(builder, materialized, (uint32_t) child_layout, states,
                                         error, error_size))
            return false;
        const XrTargetLayoutRecord *child = &materialized->layouts[child_layout];
        uint32_t aligned = 0;
        if (!xr_checked_align_u32(offset, child->align, &aligned) ||
            child->fixed_prefix_size > UINT32_MAX - aligned)
            return fail(error, error_size, "XR_EXEC_5003",
                        "aggregate field offset or size overflows");
        XrTargetFieldRecord *field = &materialized->fields[layout->field_begin + field_index];
        *field = (XrTargetFieldRecord) {
            .layout = index,
            .semantic_field = field_index,
            .semantic_name = semantic_name,
            .offset = aligned,
            .size = child->fixed_prefix_size,
            .align = child->align,
        };
        offset = aligned + child->fixed_prefix_size;
        if (child->align > aggregate_align)
            aggregate_align = child->align;
    }
    if (type->aggregate_align != 0) {
        if (type->aggregate_align > UINT16_MAX ||
            (type->aggregate_align & (type->aggregate_align - 1u)) != 0)
            return fail(error, error_size, "XR_TARGET_1002",
                        "aggregate explicit alignment is invalid");
        if (type->aggregate_align > aggregate_align)
            aggregate_align = type->aggregate_align;
    }
    if (!offset)
        offset = 1;
    if (!xr_checked_align_u32(offset, aggregate_align, &layout->fixed_prefix_size) ||
        layout->fixed_prefix_size > UINT16_MAX / 8u)
        return fail(error, error_size, "XR_EXEC_5003",
                    "aggregate representation exceeds its checked width budget");
    layout->align = (uint16_t) aggregate_align;
    states[index] = 2;
    return true;
}

static bool materialize_layouts(XrTargetPlanBuilder *builder,
                                XrTargetMaterializedPlan *materialized, char *error,
                                size_t error_size) {
    if (builder->layout_intent_count > 1u)
        qsort(builder->layout_intents, builder->layout_intent_count,
              sizeof(*builder->layout_intents), compare_layout_intent);
    materialized->layout_count = builder->layout_intent_count;
    materialized->extent_count = materialized->layout_count;
    uint32_t field_count = 0;
    for (uint32_t i = 0; i < materialized->layout_count; i++) {
        const XrTargetLayoutIntent *intent = &builder->layout_intents[i];
        if ((i && builder->layout_intents[i - 1u].semantic_type == intent->semantic_type) ||
            intent->element_count > UINT16_MAX || intent->element_count > UINT32_MAX - field_count)
            return fail(error, error_size, "XR_TARGET_1002",
                        "layout intents are duplicated or exceed field budgets");
        field_count += intent->element_count;
    }
    materialized->field_count = field_count;
    materialized->layouts = (XrTargetLayoutRecord *) allocate_records(
        materialized->layout_count, sizeof(*materialized->layouts));
    materialized->extents = (XrTargetExtentRecord *) allocate_records(
        materialized->extent_count, sizeof(*materialized->extents));
    materialized->fields = (XrTargetFieldRecord *) allocate_records(materialized->field_count,
                                                                    sizeof(*materialized->fields));
    if ((materialized->layout_count && !materialized->layouts) ||
        (materialized->extent_count && !materialized->extents) ||
        (materialized->field_count && !materialized->fields))
        return fail(error, error_size, "XR_EXEC_5003", "layout materialization failed");
    uint32_t field_begin = 0;
    for (uint32_t i = 0; i < materialized->layout_count; i++) {
        const XrTargetLayoutIntent *intent = &builder->layout_intents[i];
        materialized->layouts[i] = (XrTargetLayoutRecord) {
            .id = i,
            .semantic_type = intent->semantic_type,
            .kind = intent->kind,
            .array_element_storage = intent->array_element_storage,
            .extent = i,
            .field_begin = field_begin,
            .field_count = (uint16_t) intent->element_count,
        };
        materialized->extents[i] = (XrTargetExtentRecord) {
            .id = i,
            .kind = XR_TARGET_EXTENT_FIXED,
            .element_layout = XR_SEMANTIC_INDEX_NONE,
        };
        field_begin += intent->element_count;
    }
    uint8_t *states = (uint8_t *) allocate_records(materialized->layout_count, sizeof(*states));
    if (materialized->layout_count && !states)
        return fail(error, error_size, "XR_EXEC_5003",
                    "aggregate layout worklist allocation failed");
    for (uint32_t i = 0; i < materialized->layout_count; i++) {
        if (!materialize_layout_geometry(builder, materialized, i, states, error, error_size)) {
            xr_free(states);
            return false;
        }
    }
    xr_free(states);
    for (uint32_t i = 0; i < materialized->layout_count; i++) {
        XrTargetLayoutIntent *intent = &builder->layout_intents[i];
        if (intent->kind != XR_TARGET_LAYOUT_AGGREGATE)
            continue;
        XrTargetMachineRepRecord rep = {
            .kind = XR_MACHINE_REP_AGGREGATE,
            .register_bits = (uint16_t) (materialized->layouts[i].fixed_prefix_size * 8u),
            .memory_size = materialized->layouts[i].fixed_prefix_size,
            .memory_align = materialized->layouts[i].align,
            .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
            .detail = i,
        };
        intent->memory_rep = rep;
        if (!append_rep_intent(builder, &rep, error, error_size))
            return false;
    }
    return true;
}

static bool materialize_field_representations(const XrTargetPlanBuilder *builder,
                                              XrTargetMaterializedPlan *materialized, char *error,
                                              size_t error_size) {
    for (uint32_t layout_index = 0; layout_index < materialized->layout_count; layout_index++) {
        const XrTargetLayoutIntent *intent = &builder->layout_intents[layout_index];
        if (intent->kind != XR_TARGET_LAYOUT_AGGREGATE)
            continue;
        const XrSemanticTypeRecord *type =
            xr_semantic_plan_type(builder->semantic_plan, intent->semantic_type);
        if (!type)
            return fail(error, error_size, "XR_TARGET_1002",
                        "aggregate field type range is invalid");
        for (uint32_t field_index = 0; field_index < intent->element_count; field_index++) {
            uint32_t child_type = XR_SEMANTIC_INDEX_NONE;
            uint32_t semantic_name = XR_SEMANTIC_INDEX_NONE;
            if (!semantic_layout_field_type(builder, intent, type, field_index, &child_type,
                                            &semantic_name, error, error_size) ||
                semantic_name !=
                    materialized
                        ->fields[materialized->layouts[layout_index].field_begin + field_index]
                        .semantic_name)
                return false;
            int child_layout = find_sorted_layout_intent(builder, child_type);
            int rep =
                child_layout < 0
                    ? -1
                    : find_rep_id(materialized, &builder->layout_intents[child_layout].memory_rep);
            if (rep < 0)
                return fail(error, error_size, "XR_TARGET_1002",
                            "aggregate field representation is missing");
            XrTargetFieldRecord *field =
                &materialized
                     ->fields[materialized->layouts[layout_index].field_begin + field_index];
            field->memory_rep = (uint16_t) rep;
            field->root_kind = materialized->machine_reps[rep].root_kind;
            materialized->layouts[layout_index].root_field_count +=
                field->root_kind != XR_TARGET_ROOT_NONE;
        }
    }
    return true;
}

static bool resolve_intent_reps(const XrTargetPlanBuilder *builder,
                                const XrTargetMaterializedPlan *materialized,
                                uint32_t semantic_type, bool resolve_type_rep,
                                const XrTargetMachineRepRecord *register_intent,
                                const XrTargetMachineRepRecord *memory_intent, int *register_rep,
                                int *memory_rep) {
    const XrTargetMachineRepRecord *register_record = register_intent;
    const XrTargetMachineRepRecord *memory_record = memory_intent;
    if (resolve_type_rep) {
        int layout = find_sorted_layout_intent(builder, semantic_type);
        if (layout < 0)
            return false;
        register_record = memory_record = &builder->layout_intents[layout].memory_rep;
    }
    *register_rep = find_rep_id(materialized, register_record);
    *memory_rep = find_rep_id(materialized, memory_record);
    return *register_rep >= 0 && *memory_rep >= 0;
}

static bool materialize_functions_and_slots(XrTargetPlanBuilder *builder,
                                            XrTargetMaterializedPlan *materialized, char *error,
                                            size_t error_size) {
    if (builder->slot_intent_count > 1u)
        qsort(builder->slot_intents, builder->slot_intent_count, sizeof(*builder->slot_intents),
              compare_slot_intent);
    size_t function_count = xr_semantic_plan_function_count(builder->semantic_plan);
    if (function_count > 100000u)
        return fail(error, error_size, "XR_EXEC_5003", "target function budget exhausted");
    materialized->function_count = (uint32_t) function_count;
    materialized->slot_count = builder->slot_intent_count;
    materialized->functions = (XrTargetFunctionRecord *) allocate_records(
        materialized->function_count, sizeof(*materialized->functions));
    materialized->slots = (XrTargetSlotRecord *) allocate_records(materialized->slot_count,
                                                                  sizeof(*materialized->slots));
    if ((materialized->function_count && !materialized->functions) ||
        (materialized->slot_count && !materialized->slots))
        return fail(error, error_size, "XR_EXEC_5003", "slot materialization failed");
    uint32_t next_slot = 0;
    for (uint32_t function = 0; function < materialized->function_count; function++) {
        XrTargetFunctionRecord *function_record = &materialized->functions[function];
        function_record->id = function;
        function_record->semantic_function = function;
        function_record->slot_begin = next_slot;
        function_record->frame_align = 1;
        uint32_t offset = 0;
        while (next_slot < materialized->slot_count &&
               builder->slot_intents[next_slot].function == function) {
            const XrTargetSlotIntent *intent = &builder->slot_intents[next_slot];
            if (next_slot > function_record->slot_begin &&
                xr_stable_id_equal(builder->slot_intents[next_slot - 1u].identity,
                                   intent->identity))
                return fail(error, error_size, "XR_TARGET_1002",
                            "slot identity intent is duplicated");
            int register_rep = -1;
            int memory_rep = -1;
            uint32_t aligned = 0;
            if (!resolve_intent_reps(builder, materialized, intent->semantic_type,
                                     intent->resolve_type_rep, &intent->register_rep,
                                     &intent->memory_rep, &register_rep, &memory_rep))
                return fail(error, error_size, "XR_TARGET_1001",
                            "slot intent has no exact representation");
            const XrTargetMachineRepRecord *resolved_memory =
                &materialized->machine_reps[memory_rep];
            if (!xr_checked_align_u32(offset, resolved_memory->memory_align, &aligned) ||
                resolved_memory->memory_size > UINT32_MAX - aligned)
                return fail(error, error_size, "XR_EXEC_5003", "packed slot frame overflows");
            XrTargetSlotRecord *slot = &materialized->slots[next_slot];
            *slot = (XrTargetSlotRecord) {
                .identity = intent->identity,
                .id = next_slot,
                .function = function,
                .semantic_value = intent->semantic_value,
                .semantic_operation = intent->semantic_operation,
                .logical_slot = intent->logical_slot,
                .offset = aligned,
                .size = resolved_memory->memory_size,
                .align = resolved_memory->memory_align,
                .register_rep = (uint16_t) register_rep,
                .memory_rep = (uint16_t) memory_rep,
                .role = intent->role,
                .root_kind = intent->root_kind,
                .ownership = intent->ownership,
                .debug_variable = intent->debug_variable,
            };
            if (slot->align > function_record->frame_align)
                function_record->frame_align = slot->align;
            offset = aligned + slot->size;
            next_slot++;
        }
        function_record->slot_count = next_slot - function_record->slot_begin;
        if (!xr_checked_align_u32(offset, function_record->frame_align,
                                  &function_record->frame_size))
            return fail(error, error_size, "XR_EXEC_5003", "packed function frame overflows");
    }
    if (next_slot != materialized->slot_count)
        return fail(error, error_size, "XR_TARGET_1002",
                    "slot intent references an unknown function");
    return true;
}

static bool materialize_values(XrTargetPlanBuilder *builder, XrTargetMaterializedPlan *materialized,
                               char *error, size_t error_size) {
    if (builder->value_intent_count > 1u)
        qsort(builder->value_intents, builder->value_intent_count, sizeof(*builder->value_intents),
              compare_value_intent);
    materialized->value_rep_count = builder->value_intent_count;
    materialized->value_reps = (XrTargetValueRepRecord *) allocate_records(
        materialized->value_rep_count, sizeof(*materialized->value_reps));
    if (materialized->value_rep_count && !materialized->value_reps)
        return fail(error, error_size, "XR_EXEC_5003",
                    "value representation materialization failed");
    for (uint32_t i = 0; i < materialized->value_rep_count; i++) {
        const XrTargetValueIntent *intent = &builder->value_intents[i];
        if (i && builder->value_intents[i - 1u].semantic_value == intent->semantic_value) {
            if (error && error_size)
                snprintf(error, error_size,
                         "XR_TARGET_1001: value representation intent is duplicated "
                         "(value=%u types=%u/%u aggregate=%u/%u families=0x%llx/0x%llx "
                         "reg=%u:%u/%u:%u mem=%u:%u/%u:%u slot=%u/%u equal=%u)",
                         intent->semantic_value, builder->value_intents[i - 1u].semantic_type,
                         intent->semantic_type,
                         builder->value_intents[i - 1u].resolve_type_rep ? 1u : 0u,
                         intent->resolve_type_rep ? 1u : 0u,
                         (unsigned long long) builder->value_intents[i - 1u].family,
                         (unsigned long long) intent->family,
                         builder->value_intents[i - 1u].register_rep.kind,
                         builder->value_intents[i - 1u].register_rep.register_bits,
                         intent->register_rep.kind, intent->register_rep.register_bits,
                         builder->value_intents[i - 1u].memory_rep.kind,
                         builder->value_intents[i - 1u].memory_rep.memory_size,
                         intent->memory_rep.kind, intent->memory_rep.memory_size,
                         builder->value_intents[i - 1u].has_slot ? 1u : 0u,
                         intent->has_slot ? 1u : 0u,
                         memcmp(&builder->value_intents[i - 1u].register_rep, &intent->register_rep,
                                sizeof(intent->register_rep)) == 0 &&
                                 memcmp(&builder->value_intents[i - 1u].memory_rep,
                                        &intent->memory_rep, sizeof(intent->memory_rep)) == 0
                             ? 1u
                             : 0u);
            return false;
        }
        int register_rep = -1;
        int memory_rep = -1;
        int slot = intent->has_slot ? find_slot_id(materialized, intent->semantic_function,
                                                   intent->slot_identity)
                                    : -1;
        if ((intent->has_slot && slot < 0) ||
            !resolve_intent_reps(builder, materialized, intent->semantic_type,
                                 intent->resolve_type_rep, &intent->register_rep,
                                 &intent->memory_rep, &register_rep, &memory_rep))
            return fail(error, error_size, "XR_TARGET_1001",
                        "value intent cannot bind its canonical records");
        materialized->value_reps[i] = (XrTargetValueRepRecord) {
            .semantic_value = intent->semantic_value,
            .register_rep = (uint16_t) register_rep,
            .memory_rep = (uint16_t) memory_rep,
            .slot = intent->has_slot ? (uint32_t) slot : XR_SEMANTIC_INDEX_NONE,
        };
    }
    return true;
}

static const XrTargetValueRepRecord *
find_materialized_value(const XrTargetMaterializedPlan *materialized, uint32_t semantic_value) {
    uint32_t low = 0;
    uint32_t high = materialized->value_rep_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (materialized->value_reps[middle].semantic_value < semantic_value)
            low = middle + 1u;
        else
            high = middle;
    }
    return low < materialized->value_rep_count &&
                   materialized->value_reps[low].semantic_value == semantic_value
               ? &materialized->value_reps[low]
               : NULL;
}

static int compare_u32_value(const void *left, const void *right) {
    uint32_t a = *(const uint32_t *) left;
    uint32_t b = *(const uint32_t *) right;
    return a < b ? -1 : a != b;
}

typedef struct XrTargetLifecycleProjection {
    uint32_t function;
    uint32_t state_operation;
    uint32_t producer_operation;
    uint32_t producer_value;
    uint16_t kind;
} XrTargetLifecycleProjection;

static int compare_target_lifecycle_projection(const void *left, const void *right) {
    const XrTargetLifecycleProjection *a = (const XrTargetLifecycleProjection *) left;
    const XrTargetLifecycleProjection *b = (const XrTargetLifecycleProjection *) right;
    if (a->function != b->function)
        return a->function < b->function ? -1 : 1;
    if (a->state_operation != b->state_operation)
        return a->state_operation < b->state_operation ? -1 : 1;
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : 1;
    if (a->producer_operation != b->producer_operation)
        return a->producer_operation < b->producer_operation ? -1 : 1;
    return 0;
}

static bool exact_owned_string_lifecycle_slot(const XrTargetMaterializedPlan *materialized,
                                              const XrSemanticCoroutineLifecycleShape *shape,
                                              uint32_t function, uint32_t *slot_out) {
    const XrTargetValueRepRecord *binding =
        shape ? find_materialized_value(materialized, shape->producer_value) : NULL;
    const XrTargetSlotRecord *slot = binding && binding->slot < materialized->slot_count
                                         ? &materialized->slots[binding->slot]
                                         : NULL;
    const XrTargetMachineRepRecord *register_rep =
        binding && binding->register_rep < materialized->machine_rep_count
            ? &materialized->machine_reps[binding->register_rep]
            : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding && binding->memory_rep < materialized->machine_rep_count
            ? &materialized->machine_reps[binding->memory_rep]
            : NULL;
    if (!slot || !register_rep || !memory_rep || register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE || slot->function != function ||
        slot->semantic_value != shape->producer_value ||
        slot->semantic_operation != shape->producer_operation ||
        slot->register_rep != binding->register_rep || slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC || slot->ownership != XR_TARGET_OWNERSHIP_OWNED)
        return false;
    if (slot_out)
        *slot_out = binding->slot;
    return true;
}

static bool materialize_coroutine_roots_and_string_cleanups(XrTargetPlanBuilder *builder,
                                                            XrTargetMaterializedPlan *materialized,
                                                            char *error, size_t error_size) {
    const XrSemanticPlan *semantic = builder->semantic_plan;
    uint32_t entity_count = (uint32_t) xr_semantic_plan_entity_count(semantic);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    uint32_t root_entity_count = 0;
    uint32_t drop_entity_count = 0;
    for (uint32_t entity = 0; entity < entity_count; entity++) {
        const XrSemanticEntityRecord *record = xr_semantic_plan_entity(semantic, entity);
        root_entity_count += record && record->kind == XR_SEM_ENTITY_COROUTINE_ROOT;
        drop_entity_count += record && record->kind == XR_SEM_ENTITY_COROUTINE_DROP;
    }
    XrSemanticStringConcatReleaseIndex release_index = {0};
    XrSemanticStringConcatReleaseIndexStatus release_status =
        xr_semantic_string_concat_release_index_build(semantic, &release_index);
    if (release_status != XR_SEMANTIC_RELEASE_INDEX_OK)
        return fail(error, error_size,
                    release_status == XR_SEMANTIC_RELEASE_INDEX_INVALID ? "XR_TARGET_1001"
                                                                        : "XR_EXEC_5003",
                    "cleanup release projection is unavailable");
    if (release_index.count > UINT32_MAX - drop_entity_count) {
        xr_semantic_string_concat_release_index_dispose(&release_index);
        return fail(error, error_size, "XR_EXEC_5003", "cleanup materialization budget exhausted");
    }
    materialized->cleanup_count = release_index.count + drop_entity_count;
    uint32_t projection_count = root_entity_count + drop_entity_count;
    if (!xr_semantic_lifecycle_work_charge_product(&release_index.linear_work, entity_count, 2u) ||
        !xr_semantic_lifecycle_work_charge(&release_index.linear_work, operation_count) ||
        !xr_semantic_lifecycle_work_charge(&release_index.linear_work,
                                           materialized->function_count) ||
        !xr_semantic_lifecycle_work_charge(&release_index.linear_work,
                                           xr_semantic_plan_block_count(semantic)) ||
        !xr_semantic_lifecycle_work_charge_product(
            &release_index.linear_work, projection_count,
            (uint64_t) xr_semantic_lifecycle_sort_height(projection_count) * 2u)) {
        xr_semantic_string_concat_release_index_dispose(&release_index);
        return fail(error, error_size, "XR_EXEC_5003",
                    "coroutine lifecycle projection budget exhausted");
    }
    XrTargetLifecycleProjection *projection =
        projection_count ? (XrTargetLifecycleProjection *) allocate_records(projection_count,
                                                                            sizeof(*projection))
                         : NULL;
    uint32_t projection_cursor = 0;
    for (uint32_t entity = 0; entity < entity_count; entity++) {
        const XrSemanticEntityRecord *record = xr_semantic_plan_entity(semantic, entity);
        if (!record || (record->kind != XR_SEM_ENTITY_COROUTINE_ROOT &&
                        record->kind != XR_SEM_ENTITY_COROUTINE_DROP))
            continue;
        const XrSemanticEntityRecord *state = xr_semantic_plan_entity(semantic, record->parent);
        const XrSemanticOperationRecord *state_operation =
            state ? xr_semantic_plan_operation(semantic, state->subject) : NULL;
        const XrSemanticOperationRecord *producer =
            xr_semantic_plan_operation(semantic, record->subject);
        if (!projection || projection_cursor >= projection_count || !state || !state_operation ||
            !producer || state->kind != XR_SEM_ENTITY_COROUTINE_STATE ||
            state->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION ||
            producer->function != state_operation->function ||
            producer->result_value == XR_SEMANTIC_INDEX_NONE) {
            xr_free(projection);
            xr_semantic_string_concat_release_index_dispose(&release_index);
            return fail(error, error_size, "XR_TARGET_1001",
                        "coroutine lifecycle projection is invalid");
        }
        projection[projection_cursor++] = (XrTargetLifecycleProjection) {
            .function = producer->function,
            .state_operation = state->subject,
            .producer_operation = record->subject,
            .producer_value = producer->result_value,
            .kind = record->kind,
        };
    }
    if (projection_cursor != projection_count) {
        xr_free(projection);
        xr_semantic_string_concat_release_index_dispose(&release_index);
        return fail(error, error_size, "XR_EXEC_5003",
                    "coroutine lifecycle projection budget exhausted");
    }
    qsort(projection, projection_count, sizeof(*projection), compare_target_lifecycle_projection);
    materialized->root_maps = (XrTargetRootMapRecord *) allocate_records(
        root_entity_count, sizeof(*materialized->root_maps));
    materialized->root_slots =
        (uint32_t *) allocate_records(root_entity_count, sizeof(*materialized->root_slots));
    materialized->root_slot_count = root_entity_count;
    materialized->cleanups = (XrTargetCleanupRecord *) allocate_records(
        materialized->cleanup_count, sizeof(*materialized->cleanups));
    uint32_t candidate_capacity =
        root_entity_count > drop_entity_count ? root_entity_count : drop_entity_count;
    uint32_t *candidate_slots =
        (uint32_t *) allocate_records(candidate_capacity, sizeof(*candidate_slots));
    if ((root_entity_count && (!materialized->root_maps || !materialized->root_slots)) ||
        (materialized->cleanup_count && !materialized->cleanups) ||
        (candidate_capacity && !candidate_slots)) {
        xr_free(candidate_slots);
        xr_free(projection);
        xr_semantic_string_concat_release_index_dispose(&release_index);
        return fail(error, error_size, "XR_EXEC_5003",
                    "root and cleanup materialization allocation failed");
    }

    uint32_t next_root = 0;
    uint32_t next_root_slot = 0;
    uint32_t next_cleanup = 0;
    uint32_t next_projection = 0;
    uint32_t next_release = 0;
    for (uint32_t function = 0; function < materialized->function_count; function++) {
        const XrSemanticFunctionRecord *semantic_function =
            xr_semantic_plan_function(semantic, function);
        XrTargetFunctionRecord *target_function = &materialized->functions[function];
        target_function->root_begin = next_root;
        target_function->cleanup_begin = next_cleanup;
        if (!semantic_function ||
            semantic_function->block_begin > xr_semantic_plan_block_count(semantic) ||
            semantic_function->block_count >
                xr_semantic_plan_block_count(semantic) - semantic_function->block_begin) {
            xr_free(candidate_slots);
            xr_free(projection);
            xr_semantic_string_concat_release_index_dispose(&release_index);
            return fail(error, error_size, "XR_TARGET_1001",
                        "cleanup function partition is invalid");
        }
        for (uint32_t block_offset = 0; block_offset < semantic_function->block_count;
             block_offset++) {
            const XrSemanticBlockRecord *block =
                xr_semantic_plan_block(semantic, semantic_function->block_begin + block_offset);
            if (!block || block->function != function || block->operation_begin > operation_count ||
                block->operation_count > operation_count - block->operation_begin) {
                xr_free(candidate_slots);
                xr_free(projection);
                xr_semantic_string_concat_release_index_dispose(&release_index);
                return fail(error, error_size, "XR_TARGET_1001",
                            "cleanup block partition is invalid");
            }
            for (uint32_t operation = block->operation_begin;
                 operation < block->operation_begin + block->operation_count; operation++) {
                uint32_t root_count = 0;
                uint32_t drop_count = 0;
                while (next_projection < projection_count &&
                       projection[next_projection].function == function &&
                       projection[next_projection].state_operation == operation) {
                    const XrTargetLifecycleProjection *record = &projection[next_projection++];
                    XrSemanticCoroutineLifecycleShape lifecycle = {
                        .function = record->function,
                        .state_operation = record->state_operation,
                        .producer_operation = record->producer_operation,
                        .producer_value = record->producer_value,
                    };
                    uint32_t slot = XR_SEMANTIC_INDEX_NONE;
                    if (!exact_owned_string_lifecycle_slot(materialized, &lifecycle, function,
                                                           &slot)) {
                        xr_free(candidate_slots);
                        xr_free(projection);
                        xr_semantic_string_concat_release_index_dispose(&release_index);
                        return fail(error, error_size, "XR_TARGET_1001",
                                    "coroutine lifecycle has no exact owned String slot");
                    }
                    if (record->kind == XR_SEM_ENTITY_COROUTINE_ROOT)
                        materialized->root_slots[next_root_slot + root_count++] = slot;
                    else
                        candidate_slots[drop_count++] = slot;
                }
                if (root_count) {
                    if (root_count > UINT16_MAX) {
                        xr_free(candidate_slots);
                        xr_free(projection);
                        xr_semantic_string_concat_release_index_dispose(&release_index);
                        return fail(error, error_size, "XR_EXEC_5003",
                                    "coroutine root map exceeds exact row width");
                    }
                    qsort(materialized->root_slots + next_root_slot, root_count,
                          sizeof(*materialized->root_slots), compare_u32_value);
                    for (uint32_t i = 1; i < root_count; i++)
                        if (materialized->root_slots[next_root_slot + i - 1u] ==
                            materialized->root_slots[next_root_slot + i]) {
                            xr_free(candidate_slots);
                            xr_free(projection);
                            xr_semantic_string_concat_release_index_dispose(&release_index);
                            return fail(error, error_size, "XR_TARGET_1002",
                                        "coroutine root slot is duplicated");
                        }
                    materialized->root_maps[next_root] = (XrTargetRootMapRecord) {
                        .id = next_root,
                        .function = function,
                        .semantic_operation = operation,
                        .slot_begin = next_root_slot,
                        .slot_count = (uint16_t) root_count,
                        .flags =
                            XR_TARGET_ROOT_SUSPEND | XR_TARGET_ROOT_CANCEL | XR_TARGET_ROOT_EXIT,
                    };
                    next_root++;
                    next_root_slot += root_count;
                }
                if (drop_count) {
                    qsort(candidate_slots, drop_count, sizeof(*candidate_slots), compare_u32_value);
                    for (uint32_t i = 0; i < drop_count; i++) {
                        if (i && candidate_slots[i - 1u] == candidate_slots[i]) {
                            xr_free(candidate_slots);
                            xr_free(projection);
                            xr_semantic_string_concat_release_index_dispose(&release_index);
                            return fail(error, error_size, "XR_TARGET_1002",
                                        "coroutine cleanup slot is duplicated");
                        }
                        materialized->cleanups[next_cleanup] = (XrTargetCleanupRecord) {
                            .id = next_cleanup,
                            .function = function,
                            .semantic_operation = operation,
                            .slot = candidate_slots[i],
                            .action = XR_TARGET_CLEANUP_RELEASE,
                            .flags = XR_TARGET_CLEANUP_CANCEL | XR_TARGET_CLEANUP_EXIT,
                        };
                        next_cleanup++;
                    }
                }
                if (next_release >= release_index.count ||
                    release_index.rows[next_release].operation != operation)
                    continue;
                const XrSemanticStringConcatReleaseShape shape = release_index.rows[next_release++];
                const XrTargetValueRepRecord *binding =
                    find_materialized_value(materialized, shape.released_value);
                const XrTargetSlotRecord *slot = binding && binding->slot < materialized->slot_count
                                                     ? &materialized->slots[binding->slot]
                                                     : NULL;
                const XrTargetMachineRepRecord *register_rep =
                    binding && binding->register_rep < materialized->machine_rep_count
                        ? &materialized->machine_reps[binding->register_rep]
                        : NULL;
                const XrTargetMachineRepRecord *memory_rep =
                    binding && binding->memory_rep < materialized->machine_rep_count
                        ? &materialized->machine_reps[binding->memory_rep]
                        : NULL;
                if (next_cleanup >= materialized->cleanup_count || !slot || !register_rep ||
                    !memory_rep || register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
                    memory_rep->kind != XR_MACHINE_REP_DYN_VALUE || slot->function != function ||
                    slot->semantic_value != shape.released_value ||
                    slot->semantic_operation != shape.producer_operation ||
                    slot->register_rep != binding->register_rep ||
                    slot->memory_rep != binding->memory_rep ||
                    slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
                    slot->ownership != XR_TARGET_OWNERSHIP_OWNED) {
                    xr_free(candidate_slots);
                    xr_free(projection);
                    xr_semantic_string_concat_release_index_dispose(&release_index);
                    return fail(error, error_size, "XR_TARGET_1001",
                                "cleanup owner has no exact owned String slot");
                }
                materialized->cleanups[next_cleanup] = (XrTargetCleanupRecord) {
                    .id = next_cleanup,
                    .function = function,
                    .semantic_operation = operation,
                    .slot = binding->slot,
                    .action = XR_TARGET_CLEANUP_RELEASE,
                };
                next_cleanup++;
            }
        }
        target_function->root_count = next_root - target_function->root_begin;
        target_function->cleanup_count = next_cleanup - target_function->cleanup_begin;
    }
    bool complete = next_root_slot == materialized->root_slot_count &&
                    next_cleanup == materialized->cleanup_count &&
                    next_projection == projection_count && next_release == release_index.count;
    xr_free(candidate_slots);
    xr_free(projection);
    xr_semantic_string_concat_release_index_dispose(&release_index);
    materialized->root_map_count = next_root;
    if (!complete)
        return fail(error, error_size, "XR_TARGET_1001",
                    "root and cleanup materialization is incomplete");
    return true;
}

static bool semantic_type_is_exact_i64(const XrSemanticPlan *plan, uint32_t type_index) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    return type && type->kind == XR_KIND_INT && type->scalar_rep == XR_NATIVE_I64 &&
           (type->flags & XR_SEM_TYPE_NULLABLE) == 0;
}

/* The comparison relations answer a truth value, which the language types
 * `bool` and the plan lays out as a one-byte I1 slot, so the executable family
 * admits exactly that second slot shape beside the signed i64 one. Both shapes
 * are stated once here and selected by the row, and neither is ever coerced
 * into the other. */
typedef enum XrTargetScalarSlotFamily {
    XR_TARGET_SCALAR_SLOT_I64 = 0,
    XR_TARGET_SCALAR_SLOT_BOOL,
} XrTargetScalarSlotFamily;

static bool semantic_type_is_exact_bool(const XrSemanticPlan *plan, uint32_t type_index) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    return type && type->kind == XR_KIND_BOOL && type->scalar_rep == XR_SCALAR_REP_NONE &&
           (type->flags & XR_SEM_TYPE_NULLABLE) == 0;
}

static bool materialized_rep_is_exact(const XrTargetMachineRepRecord *rep,
                                      XrTargetScalarSlotFamily family) {
    if (!rep || rep->root_kind != XR_TARGET_ROOT_NONE ||
        rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
        return false;
    if (family == XR_TARGET_SCALAR_SLOT_BOOL)
        return rep->kind == XR_MACHINE_REP_I1 && rep->register_bits == 1 && rep->memory_size == 1 &&
               rep->memory_align == 1 && rep->signedness == XR_TARGET_SIGN_NONE;
    return rep->kind == XR_MACHINE_REP_I64 && rep->register_bits == 64 && rep->memory_size == 8 &&
           rep->memory_align == 8 && rep->signedness == XR_TARGET_SIGN_SIGNED;
}

static bool materialized_scalar_slot(const XrTargetMaterializedPlan *materialized,
                                     uint32_t function, uint32_t semantic_value,
                                     XrTargetScalarSlotFamily family, uint32_t *out_slot) {
    const XrTargetValueRepRecord *value = find_materialized_value(materialized, semantic_value);
    if (!value || value->slot == XR_SEMANTIC_INDEX_NONE ||
        value->slot >= materialized->slot_count ||
        value->register_rep >= materialized->machine_rep_count ||
        value->memory_rep >= materialized->machine_rep_count)
        return false;
    const XrTargetSlotRecord *slot = &materialized->slots[value->slot];
    uint32_t width = family == XR_TARGET_SCALAR_SLOT_BOOL ? 1u : 8u;
    if (slot->id != value->slot || slot->function != function ||
        slot->semantic_value != semantic_value || slot->register_rep != value->register_rep ||
        slot->memory_rep != value->memory_rep || slot->size != width || slot->align != width ||
        slot->root_kind != XR_TARGET_ROOT_NONE || slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        !materialized_rep_is_exact(&materialized->machine_reps[value->register_rep], family) ||
        !materialized_rep_is_exact(&materialized->machine_reps[value->memory_rep], family))
        return false;
    if (out_slot)
        *out_slot = value->slot;
    return true;
}

static bool materialized_i64_slot(const XrTargetMaterializedPlan *materialized, uint32_t function,
                                  uint32_t semantic_value, uint32_t *out_slot) {
    return materialized_scalar_slot(materialized, function, semantic_value,
                                    XR_TARGET_SCALAR_SLOT_I64, out_slot);
}

static bool source_entry_call_is_exact(const XrTargetPlanBuilder *builder,
                                       const XrTargetMaterializedPlan *materialized,
                                       uint32_t call_index) {
    if (!builder || !materialized || call_index >= materialized->call_count)
        return false;
    const XrTargetCallRecord *call = &materialized->calls[call_index];
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, call->semantic_operation);
    const XrSemanticPlan *dependency = call->source_dependency < builder->semantic_dependency_count
                                           ? builder->semantic_dependencies[call->source_dependency]
                                           : NULL;
    const XrSemanticSourceExportRecord *export_record =
        dependency && call->source_export < xr_semantic_plan_source_export_count(dependency)
            ? xr_semantic_plan_source_export(dependency, call->source_export)
            : NULL;
    const XrSemanticFunctionRecord *callee =
        export_record && export_record->kind == XR_SEM_SOURCE_EXPORT_FUNCTION
            ? xr_semantic_plan_function(dependency, export_record->function)
            : NULL;
    if (!operation || !dependency || !export_record ||
        export_record->kind != XR_SEM_SOURCE_EXPORT_FUNCTION || !callee ||
        !xr_stable_id_equal(export_record->exported_entity, callee->id) || call->id != call_index ||
        (operation->opcode != XI_CALL && operation->opcode != XI_CALL_METHOD) ||
        operation->function != call->caller_function || call->flags != 0 ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_SOURCE_EXPORT ||
        call->target_kind != XR_TARGET_CALL_TARGET_SOURCE_EXPORT ||
        call->callee_function != XR_SEMANTIC_INDEX_NONE || call->adapter_count != 0 ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE ||
        call->result_value != operation->result_value ||
        operation->operand_count != (uint32_t) call->argument_count + 1u ||
        call->argument_count != callee->parameter_count || callee->capture_count != 0 ||
        !semantic_type_is_exact_i64(builder->semantic_plan, operation->result_type) ||
        !semantic_type_is_exact_i64(dependency, callee->return_type) ||
        call->result_register_rep >= materialized->machine_rep_count ||
        call->result_memory_rep >= materialized->machine_rep_count ||
        !materialized_rep_is_exact(&materialized->machine_reps[call->result_register_rep],
                                   XR_TARGET_SCALAR_SLOT_I64) ||
        !materialized_rep_is_exact(&materialized->machine_reps[call->result_memory_rep],
                                   XR_TARGET_SCALAR_SLOT_I64) ||
        !materialized_i64_slot(materialized, call->caller_function, operation->result_value,
                               NULL) ||
        call->argument_begin > materialized->call_argument_count ||
        call->argument_count > materialized->call_argument_count - call->argument_begin)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(builder->semantic_plan, &operand_count);
    if (!operands || operation->operand_begin >= operand_count)
        return false;
    for (uint16_t ordinal = 0; ordinal < call->argument_count; ordinal++) {
        const XrTargetCallArgumentRecord *argument =
            &materialized->call_arguments[call->argument_begin + ordinal];
        const XrSemanticOperandRecord *operand = &operands[operation->operand_begin + 1u + ordinal];
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(dependency, callee->parameter_begin + ordinal);
        if (!parameter || parameter->function != export_record->function ||
            parameter->ordinal != ordinal || parameter->mode != XR_PARAM_READ ||
            parameter->transfer_mode != XR_TRANSFER_SHARE ||
            !semantic_type_is_exact_i64(dependency, parameter->type) ||
            argument->call != call_index || argument->ordinal != ordinal ||
            argument->semantic_value != operand->value || argument->mode != XR_TARGET_CALL_VALUE ||
            (argument->ownership != XR_TARGET_CALL_READ &&
             argument->ownership != XR_TARGET_CALL_CONSUME) ||
            argument->transfer_mode != XR_TRANSFER_SHARE || argument->flags != 0 ||
            argument->callee_slot != XR_SEMANTIC_INDEX_NONE ||
            argument->register_rep >= materialized->machine_rep_count ||
            argument->memory_rep >= materialized->machine_rep_count ||
            argument->callee_register_rep >= materialized->machine_rep_count ||
            argument->callee_memory_rep >= materialized->machine_rep_count ||
            !materialized_rep_is_exact(&materialized->machine_reps[argument->register_rep],
                                       XR_TARGET_SCALAR_SLOT_I64) ||
            !materialized_rep_is_exact(&materialized->machine_reps[argument->memory_rep],
                                       XR_TARGET_SCALAR_SLOT_I64) ||
            !materialized_rep_is_exact(&materialized->machine_reps[argument->callee_register_rep],
                                       XR_TARGET_SCALAR_SLOT_I64) ||
            !materialized_rep_is_exact(&materialized->machine_reps[argument->callee_memory_rep],
                                       XR_TARGET_SCALAR_SLOT_I64) ||
            !materialized_i64_slot(materialized, call->caller_function, operand->value, NULL))
            return false;
    }
    return true;
}

static bool materialize_entry_expectations(const XrTargetPlanBuilder *builder,
                                           XrTargetMaterializedPlan *materialized, char *error,
                                           size_t error_size) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < materialized->call_count; i++)
        count += source_entry_call_is_exact(builder, materialized, i);
    materialized->entry_expectation_count = count;
    materialized->entry_expectations = (XrTargetEntryExpectationRecord *) allocate_records(
        count, sizeof(*materialized->entry_expectations));
    if (count && !materialized->entry_expectations)
        return fail(error, error_size, "XR_EXEC_5003",
                    "dynamic entry expectation allocation failed");
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(builder->profile);
    if (!machine)
        return fail(error, error_size, "XR_TARGET_1000", "dynamic entry target profile is missing");
    uint32_t next = 0;
    for (uint32_t call_index = 0; call_index < materialized->call_count; call_index++) {
        if (!source_entry_call_is_exact(builder, materialized, call_index))
            continue;
        const XrTargetCallRecord *call = &materialized->calls[call_index];
        XrTargetEntryExpectationRecord *record = &materialized->entry_expectations[next];
        XrTargetEntryAbiFacts facts = {0};
        facts.schema_version = XR_TARGET_ENTRY_ABI_SCHEMA_VERSION;
        facts.parameter_count = call->argument_count;
        facts.native_abi = machine->native_abi;
        facts.value_kind = XR_TARGET_ENTRY_VALUE_EXACT_I64;
        facts.target_data_layout = machine->data_layout.stable_hash;
        facts.target_profile_fingerprint = xr_target_profile_fingerprint(builder->profile);
        *record = (XrTargetEntryExpectationRecord) {
            .id = next,
            .call = call_index,
            .abi_schema_version = facts.schema_version,
            .parameter_count = facts.parameter_count,
            .native_abi = facts.native_abi,
            .value_kind = facts.value_kind,
            .adapter_kind = XR_TARGET_ENTRY_ADAPTER_IDENTITY,
            .target_data_layout = facts.target_data_layout,
            .target_profile_fingerprint = facts.target_profile_fingerprint,
        };
        if (!stable_identity_from_pair("xray-target-entry-expectation-v1", call->identity,
                                       call->source_callee_identity, next, &record->identity) ||
            !xr_target_entry_abi_fingerprint(&facts, &record->entry_abi_fingerprint) ||
            !xr_target_entry_identity_adapter_fingerprint(&record->entry_abi_fingerprint,
                                                          &record->adapter_fingerprint))
            return fail(error, error_size, "XR_TARGET_1003",
                        "dynamic entry expectation identity is incomplete");
        next++;
    }
    return next == count;
}

static uint16_t scalar_instruction_opcode(uint16_t semantic_opcode) {
    typedef struct ScalarInstructionBinding {
        uint16_t semantic_opcode;
        uint16_t target_opcode;
    } ScalarInstructionBinding;
    static const ScalarInstructionBinding bindings[] = {
#define XR_TARGET_SEMANTIC_BINDING(semantic, target) {semantic, XR_TARGET_INSTRUCTION_##target},
        XR_TARGET_INSTRUCTION_SEMANTIC_BINDINGS(XR_TARGET_SEMANTIC_BINDING)
#undef XR_TARGET_SEMANTIC_BINDING
    };
    uint16_t target_opcode = XR_TARGET_INSTRUCTION_INVALID;
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        if (bindings[i].semantic_opcode != semantic_opcode)
            continue;
        if (target_opcode != XR_TARGET_INSTRUCTION_INVALID)
            return XR_TARGET_INSTRUCTION_INVALID;
        target_opcode = bindings[i].target_opcode;
    }
    return target_opcode;
}

/*
 * A parameter operation computes nothing; it names the argument ordinal that
 * fills the function's parameter slot. The whole function is rejected unless
 * the operation and the frozen parameter record agree on ordinal, function,
 * exact signed-i64 type, and SSA value, so no row may bind an argument the
 * signature does not declare.
 */
static bool scalar_parameter_row_is_exact(const XrSemanticPlan *semantic,
                                          const XrTargetMaterializedPlan *materialized,
                                          uint32_t function_index,
                                          const XrSemanticFunctionRecord *function,
                                          const XrSemanticOperationRecord *operation,
                                          uint64_t *out_ordinal, uint32_t *out_slot) {
    if (operation->semantic_immediate < 0 ||
        (uint64_t) operation->semantic_immediate >= function->parameter_count)
        return false;
    uint32_t ordinal = (uint32_t) operation->semantic_immediate;
    const XrSemanticParameterRecord *parameter =
        xr_semantic_plan_parameter(semantic, function->parameter_begin + ordinal);
    if (!parameter || parameter->function != function_index || parameter->ordinal != ordinal ||
        parameter->value != operation->result_value || parameter->type != operation->result_type ||
        !semantic_type_is_exact_i64(semantic, parameter->type) ||
        !materialized_i64_slot(materialized, function_index, parameter->value, out_slot))
        return false;
    if (out_ordinal)
        *out_ordinal = ordinal;
    return true;
}

/* A branch may only leave for a block of the same function, so a successor is
 * admitted as a local ordinal or not at all. */
static bool scalar_block_successor_local(const XrSemanticFunctionRecord *function,
                                         uint32_t successor, uint32_t *out_local) {
    if (successor == XR_SEMANTIC_INDEX_NONE || successor < function->block_begin ||
        successor - function->block_begin >= function->block_count)
        return false;
    if (out_local)
        *out_local = successor - function->block_begin;
    return true;
}

/*
 * Each admitted block kind has exactly one terminator row, so a block's row
 * count is its operation count plus one and is known before any row is
 * written. XI_BLOCK_UNREACHABLE has no terminator this family can express and
 * leaves the function unavailable rather than falling out of its last block.
 */
static bool scalar_block_is_admissible(const XrSemanticPlan *semantic,
                                       const XrSemanticFunctionRecord *function,
                                       uint32_t function_index, uint32_t block_index,
                                       const XrSemanticBlockRecord **out_block) {
    const XrSemanticBlockRecord *block = xr_semantic_plan_block(semantic, block_index);
    uint32_t operation_total = (uint32_t) xr_semantic_plan_operation_count(semantic);
    if (!block || block->function != function_index || block->operation_begin > operation_total ||
        block->operation_count > operation_total - block->operation_begin ||
        block->operation_count >= 40000000u)
        return false;
    switch (block->kind) {
        case XI_BLOCK_RETURN:
            if (block->control_value == XR_SEMANTIC_INDEX_NONE ||
                block->successors[0] != XR_SEMANTIC_INDEX_NONE ||
                block->successors[1] != XR_SEMANTIC_INDEX_NONE)
                return false;
            break;
        case XI_BLOCK_PLAIN:
            if (block->control_value != XR_SEMANTIC_INDEX_NONE ||
                !scalar_block_successor_local(function, block->successors[0], NULL) ||
                block->successors[1] != XR_SEMANTIC_INDEX_NONE)
                return false;
            break;
        case XI_BLOCK_IF:
            /* The condition becomes a nonzero test on the slot the plan already
             * gave its value, so a block whose condition is neither an exact
             * signed i64 nor an exact truth slot is refused when that slot is
             * resolved rather than coerced into either. */
            if (block->control_value == XR_SEMANTIC_INDEX_NONE ||
                !scalar_block_successor_local(function, block->successors[0], NULL) ||
                !scalar_block_successor_local(function, block->successors[1], NULL))
                return false;
            break;
        default:
            return false;
    }
    if (out_block)
        *out_block = block;
    return true;
}

/* Writes the one row that ends a block. Targets are row indexes relative to
 * the group's first row, taken from the frozen block layout. */
static bool scalar_block_terminator_row(const XrTargetMaterializedPlan *materialized,
                                        uint32_t function_index,
                                        const XrSemanticFunctionRecord *function,
                                        const XrSemanticBlockRecord *block,
                                        const uint32_t *block_row, XrTargetInstructionRecord *row) {
    uint32_t control_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
    uint32_t taken = 0;
    uint32_t untaken = 0;
    *row = (XrTargetInstructionRecord) {
        .function = function_index,
        .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
        .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE, XR_TARGET_INSTRUCTION_SLOT_NONE},
    };
    switch (block->kind) {
        case XI_BLOCK_RETURN:
            if (!materialized_i64_slot(materialized, function_index, block->control_value,
                                       &control_slot))
                return false;
            row->opcode = XR_TARGET_INSTRUCTION_RETURN_I64;
            row->operand_count = 1;
            row->operand_slots[0] = control_slot;
            return true;
        case XI_BLOCK_PLAIN:
            if (!scalar_block_successor_local(function, block->successors[0], &taken))
                return false;
            row->opcode = XR_TARGET_INSTRUCTION_JUMP;
            row->immediate_bits = XR_TARGET_INSTRUCTION_TARGET_PACK(block_row[taken], 0);
            return true;
        case XI_BLOCK_IF:
            /* The condition's own slot decides which branch row this is: a
             * signed i64 condition keeps the nonzero test it already had, and
             * the truth slot a comparison writes gets the branch that reads one
             * byte. The choice is frozen into the row here, so nothing at
             * execution has to work out how wide the condition is. */
            if (materialized_i64_slot(materialized, function_index, block->control_value,
                                      &control_slot))
                row->opcode = XR_TARGET_INSTRUCTION_BRANCH_IF_NONZERO_I64;
            else if (materialized_scalar_slot(materialized, function_index, block->control_value,
                                              XR_TARGET_SCALAR_SLOT_BOOL, &control_slot))
                row->opcode = XR_TARGET_INSTRUCTION_BRANCH_IF_TRUE_BOOL;
            else
                return false;
            if (!scalar_block_successor_local(function, block->successors[0], &taken) ||
                !scalar_block_successor_local(function, block->successors[1], &untaken))
                return false;
            row->operand_count = 1;
            row->operand_slots[0] = control_slot;
            row->immediate_bits =
                XR_TARGET_INSTRUCTION_TARGET_PACK(block_row[taken], block_row[untaken]);
            return true;
        default:
            return false;
    }
}

typedef struct XrScalarInstructionAnalysis {
    uint32_t *call_by_operation;
    uint32_t *entry_by_operation;
    uint32_t *coroutine_by_operation;
    uint8_t *elided_operations;
    uint32_t operation_count;
} XrScalarInstructionAnalysis;

static void scalar_instruction_analysis_dispose(XrScalarInstructionAnalysis *analysis) {
    if (!analysis)
        return;
    xr_free(analysis->call_by_operation);
    xr_free(analysis->entry_by_operation);
    xr_free(analysis->coroutine_by_operation);
    xr_free(analysis->elided_operations);
    memset(analysis, 0, sizeof(*analysis));
}

static bool scalar_call_rep_is_i64(const XrTargetMaterializedPlan *materialized, uint16_t rep) {
    return materialized && rep < materialized->machine_rep_count &&
           materialized->machine_reps[rep].kind == XR_MACHINE_REP_I64;
}

/* This is the builder-side admission boundary for the one executable call
 * specialization. The independent instruction verifier repeats the judgement
 * from frozen TargetPlan rows; this copy sees only materialized builder state
 * and decides whether a semantic XI_CALL may receive a row at all. */
static bool scalar_direct_i64_call_is_exact(const XrTargetPlanBuilder *builder,
                                            const XrTargetMaterializedPlan *materialized,
                                            uint32_t call_index) {
    if (!builder || !materialized || call_index >= materialized->call_count)
        return false;
    const XrTargetCallRecord *call = &materialized->calls[call_index];
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, call->semantic_operation);
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(builder->semantic_plan, call->callee_function);
    if (!operation || !callee || call->id != call_index || operation->opcode != XI_CALL ||
        operation->function != call->caller_function || call->flags != 0 ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
        call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL || call->adapter_count != 0 ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_mode != XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL ||
        call->array_intrinsic_kind != XR_TARGET_ARRAY_INTRINSIC_NONE ||
        call->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        call->result_value != operation->result_value ||
        operation->operand_count != (uint32_t) call->argument_count + 1u ||
        call->argument_count != callee->parameter_count ||
        !semantic_type_is_exact_i64(builder->semantic_plan, operation->result_type) ||
        !scalar_call_rep_is_i64(materialized, call->result_register_rep) ||
        !scalar_call_rep_is_i64(materialized, call->result_memory_rep) ||
        !materialized_i64_slot(materialized, call->caller_function, operation->result_value,
                               NULL) ||
        call->argument_begin > materialized->call_argument_count ||
        call->argument_count > materialized->call_argument_count - call->argument_begin)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(builder->semantic_plan, &operand_count);
    if (!operands || operation->operand_begin >= operand_count ||
        operands[operation->operand_begin].role != XR_SEM_OPERAND_CALLEE)
        return false;
    for (uint16_t ordinal = 0; ordinal < call->argument_count; ordinal++) {
        const XrTargetCallArgumentRecord *argument =
            &materialized->call_arguments[call->argument_begin + ordinal];
        const XrSemanticOperandRecord *operand = &operands[operation->operand_begin + 1u + ordinal];
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(builder->semantic_plan, callee->parameter_begin + ordinal);
        if (!parameter || argument->call != call_index || argument->ordinal != ordinal ||
            argument->semantic_operand != operation->operand_begin + 1u + ordinal ||
            argument->semantic_value != operand->value ||
            argument->callee_parameter != callee->parameter_begin + ordinal ||
            argument->mode != XR_TARGET_CALL_VALUE ||
            (argument->ownership != XR_TARGET_CALL_READ &&
             argument->ownership != XR_TARGET_CALL_CONSUME) ||
            argument->transfer_mode != XR_TRANSFER_SHARE || argument->flags != 0 ||
            argument->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
            !scalar_call_rep_is_i64(materialized, argument->register_rep) ||
            !scalar_call_rep_is_i64(materialized, argument->memory_rep) ||
            !scalar_call_rep_is_i64(materialized, argument->callee_register_rep) ||
            !scalar_call_rep_is_i64(materialized, argument->callee_memory_rep) ||
            !materialized_i64_slot(materialized, call->caller_function, operand->value, NULL) ||
            !materialized_i64_slot(materialized, call->callee_function, parameter->value, NULL))
            return false;
    }
    return true;
}

static bool scalar_instruction_analysis_init(const XrTargetPlanBuilder *builder,
                                             const XrTargetMaterializedPlan *materialized,
                                             XrScalarInstructionAnalysis *analysis) {
    if (!builder || !materialized || !analysis ||
        xr_semantic_plan_operation_count(builder->semantic_plan) > UINT32_MAX)
        return false;
    memset(analysis, 0, sizeof(*analysis));
    analysis->operation_count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    analysis->call_by_operation = (uint32_t *) allocate_records(
        analysis->operation_count, sizeof(*analysis->call_by_operation));
    analysis->entry_by_operation = (uint32_t *) allocate_records(
        analysis->operation_count, sizeof(*analysis->entry_by_operation));
    analysis->coroutine_by_operation = (uint32_t *) allocate_records(
        analysis->operation_count, sizeof(*analysis->coroutine_by_operation));
    analysis->elided_operations = (uint8_t *) allocate_records(
        analysis->operation_count, sizeof(*analysis->elided_operations));
    if ((analysis->operation_count &&
         (!analysis->call_by_operation || !analysis->entry_by_operation ||
          !analysis->coroutine_by_operation)) ||
        (analysis->operation_count && !analysis->elided_operations)) {
        scalar_instruction_analysis_dispose(analysis);
        return false;
    }
    for (uint32_t i = 0; i < analysis->operation_count; i++) {
        analysis->call_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
        analysis->entry_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
        analysis->coroutine_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < materialized->coroutine_count; i++) {
        uint32_t operation = materialized->coroutines[i].semantic_operation;
        if (operation >= analysis->operation_count ||
            analysis->coroutine_by_operation[operation] != XR_SEMANTIC_INDEX_NONE) {
            scalar_instruction_analysis_dispose(analysis);
            return false;
        }
        analysis->coroutine_by_operation[operation] = i;
    }

    XrTargetValueStorageAnalysis values = {0};
    XrDirectLocalCalleeStorageAnalysis shared = {0};
    XrSourceNamespaceStorageAnalysis namespaces = {0};
    char ignored[1] = {0};
    if (!value_storage_analysis_init(builder->semantic_plan, &values, ignored, sizeof(ignored)) ||
        !index_value_operations(builder->semantic_plan, &values, ignored, sizeof(ignored)) ||
        !direct_local_callee_storage_analysis_init(builder->semantic_plan, &values, &shared,
                                                   ignored, sizeof(ignored)) ||
        !source_namespace_storage_analysis_init(builder->semantic_plan, &values, &namespaces,
                                                ignored, sizeof(ignored))) {
        source_namespace_storage_analysis_dispose(&namespaces);
        direct_local_callee_storage_analysis_dispose(&shared);
        value_storage_analysis_dispose(&values);
        scalar_instruction_analysis_dispose(analysis);
        return false;
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(builder->semantic_plan, &operand_count);
    uint32_t *entry_by_call =
        (uint32_t *) allocate_records(materialized->call_count, sizeof(*entry_by_call));
    if (materialized->call_count && !entry_by_call) {
        source_namespace_storage_analysis_dispose(&namespaces);
        direct_local_callee_storage_analysis_dispose(&shared);
        value_storage_analysis_dispose(&values);
        scalar_instruction_analysis_dispose(analysis);
        return false;
    }
    for (uint32_t i = 0; i < materialized->call_count; i++)
        entry_by_call[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < materialized->entry_expectation_count; i++) {
        uint32_t call = materialized->entry_expectations[i].call;
        if (call >= materialized->call_count || entry_by_call[call] != XR_SEMANTIC_INDEX_NONE) {
            xr_free(entry_by_call);
            source_namespace_storage_analysis_dispose(&namespaces);
            direct_local_callee_storage_analysis_dispose(&shared);
            value_storage_analysis_dispose(&values);
            scalar_instruction_analysis_dispose(analysis);
            return false;
        }
        entry_by_call[call] = i;
    }
    for (uint32_t call_index = 0; call_index < materialized->call_count; call_index++) {
        const XrTargetCallRecord *call = &materialized->calls[call_index];
        bool direct = scalar_direct_i64_call_is_exact(builder, materialized, call_index);
        bool dynamic = entry_by_call[call_index] != XR_SEMANTIC_INDEX_NONE &&
                       source_entry_call_is_exact(builder, materialized, call_index);
        if ((!direct && !dynamic) || call->semantic_operation >= analysis->operation_count)
            continue;
        analysis->call_by_operation[call->semantic_operation] = call_index;
        if (dynamic)
            analysis->entry_by_operation[call->semantic_operation] = entry_by_call[call_index];
        const XrSemanticOperationRecord *call_operation =
            xr_semantic_plan_operation(builder->semantic_plan, call->semantic_operation);
        uint32_t value = operands[call_operation->operand_begin].value;
        uint32_t *path = (uint32_t *) allocate_records(analysis->operation_count, sizeof(*path));
        if (!path)
            continue;
        uint32_t path_count = 0;
        bool exact = false;
        for (uint32_t depth = 0; depth < analysis->operation_count; depth++) {
            uint32_t producer_index = value < values.total_values ? values.value_operations[value]
                                                                  : XR_SEMANTIC_INDEX_NONE;
            const XrSemanticOperationRecord *producer =
                producer_index == XR_SEMANTIC_INDEX_NONE
                    ? NULL
                    : xr_semantic_plan_operation(builder->semantic_plan, producer_index);
            if (!producer || producer->function != call->caller_function)
                break;
            path[path_count++] = producer_index;
            if (producer->opcode == XI_COPY &&
                producer->semantic_immediate == XI_COPY_KIND_IDENTITY &&
                producer->operand_count == 1 && producer->operand_begin < operand_count) {
                value = operands[producer->operand_begin].value;
                continue;
            }
            if (dynamic && value < namespaces.value_count && namespaces.exact_value[value] &&
                namespaces.dependency_by_value[value] == call->source_dependency) {
                exact = true;
                break;
            }
            bool closure = (producer->opcode == XI_CLOSURE_NEW ||
                            (producer->opcode == XI_STACK_ALLOC &&
                             producer->semantic_immediate == XI_CLOSURE_NEW)) &&
                           producer->callable_function == call->callee_function;
            bool shared_callee =
                producer->opcode == XI_GET_SHARED &&
                direct_local_callee_storage_value_is_exact(builder->semantic_plan, &shared,
                                                           producer) &&
                shared.target_by_value[producer->result_value] == call->callee_function;
            exact = direct && (closure || shared_callee);
            break;
        }
        if (exact)
            for (uint32_t i = 0; i < path_count; i++)
                analysis->elided_operations[path[i]] = 1;
        xr_free(path);
    }
    /* A skipped callable chain is compile-time-only only when every use stays
     * inside another skipped identity edge or is the callee edge of an exact
     * admitted call. Any ordinary value use leaves its producer in the block,
     * which makes the whole scalar function unavailable instead of dropping
     * observable closure or shared-storage behaviour. */
    for (uint32_t use_index = 0; use_index < analysis->operation_count; use_index++) {
        const XrSemanticOperationRecord *use =
            xr_semantic_plan_operation(builder->semantic_plan, use_index);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            continue;
        for (uint16_t ordinal = 0; ordinal < use->operand_count; ordinal++) {
            uint32_t value = operands[use->operand_begin + ordinal].value;
            uint32_t producer = value < values.total_values ? values.value_operations[value]
                                                            : XR_SEMANTIC_INDEX_NONE;
            if (producer >= analysis->operation_count || !analysis->elided_operations[producer])
                continue;
            bool forwarding = ordinal == 0 && analysis->elided_operations[use_index] &&
                              use->opcode == XI_COPY &&
                              use->semantic_immediate == XI_COPY_KIND_IDENTITY;
            bool dynamic_receiver =
                analysis->entry_by_operation[use_index] != XR_SEMANTIC_INDEX_NONE &&
                use->opcode == XI_CALL_METHOD;
            bool callee = ordinal == 0 &&
                          analysis->call_by_operation[use_index] != XR_SEMANTIC_INDEX_NONE &&
                          operands[use->operand_begin].role ==
                              (dynamic_receiver ? XR_SEM_OPERAND_RECEIVER : XR_SEM_OPERAND_CALLEE);
            if (!forwarding && !callee)
                analysis->elided_operations[producer] = 0;
        }
    }
    uint32_t block_count = (uint32_t) xr_semantic_plan_block_count(builder->semantic_plan);
    for (uint32_t i = 0; i < block_count; i++) {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(builder->semantic_plan, i);
        uint32_t producer = block && block->control_value < values.total_values
                                ? values.value_operations[block->control_value]
                                : XR_SEMANTIC_INDEX_NONE;
        if (producer < analysis->operation_count)
            analysis->elided_operations[producer] = 0;
    }
    direct_local_callee_storage_analysis_dispose(&shared);
    source_namespace_storage_analysis_dispose(&namespaces);
    xr_free(entry_by_call);
    value_storage_analysis_dispose(&values);
    return true;
}

/* A suspend row is admitted only when the frozen coroutine record names the
 * block's sole XI_YIELD operation and the continuation stays in this exact
 * function. The row carries the state record ID; its resume block remains a
 * TargetPlan fact and is never reconstructed from a runtime tag. */
static bool scalar_suspend_block_is_exact(const XrTargetPlanBuilder *builder,
                                          const XrTargetMaterializedPlan *materialized,
                                          uint32_t function_index,
                                          const XrSemanticFunctionRecord *function,
                                          const XrSemanticBlockRecord *block,
                                          const XrScalarInstructionAnalysis *analysis,
                                          uint32_t *state_out) {
    if (!builder || !materialized || !function || !block || !analysis ||
        block->function != function_index || block->operation_count != 1 ||
        block->operation_begin >= analysis->operation_count)
        return false;
    uint32_t operation_index = block->operation_begin;
    uint32_t state_index = analysis->coroutine_by_operation[operation_index];
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, operation_index);
    const XrTargetCoroutineStateRecord *state =
        state_index < materialized->coroutine_count ? &materialized->coroutines[state_index] : NULL;
    if (!operation || !state || state->id != state_index || state->function != function_index ||
        state->semantic_operation != operation_index || state->suspend_block != operation->block ||
        state->resume_predecessor != state->suspend_block ||
        state->resume_predecessor_ordinal != 0 || state->resume_block < function->block_begin ||
        state->resume_block - function->block_begin >= function->block_count ||
        state->direct_call != XR_SEMANTIC_INDEX_NONE ||
        state->result_slot != XR_SEMANTIC_INDEX_NONE || state->flags != 0 ||
        operation->function != function_index || operation->opcode != XI_YIELD ||
        operation->operand_count != 0 || operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->semantic_immediate != 0 ||
        operation->effects != xi_generated_op_effects(XI_YIELD))
        return false;
    if (state_out)
        *state_out = state_index;
    return true;
}

/*
 * A function is committed only as one complete instruction group. Structural
 * or semantic facts outside this deliberately small closed family make the
 * function unavailable; they never produce a partial executable program.
 */
static bool materialize_scalar_instruction_function(const XrTargetPlanBuilder *builder,
                                                    const XrTargetMaterializedPlan *materialized,
                                                    uint32_t function_index,
                                                    const XrScalarInstructionAnalysis *analysis,
                                                    const uint8_t *executable_functions,
                                                    XrTargetInstructionRecord *rows,
                                                    uint32_t row_begin, uint32_t *out_row_count) {
    if (out_row_count)
        *out_row_count = 0;
    const XrSemanticPlan *semantic = builder->semantic_plan;
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(semantic, function_index);
    if (!function || function_index >= materialized->function_count ||
        materialized->functions[function_index].id != function_index ||
        materialized->functions[function_index].semantic_function != function_index ||
        function->parameter_count > XR_TARGET_INSTRUCTION_MAX_PARAMETERS ||
        function->capture_count != 0 || function->block_count == 0 ||
        function->block_count > XR_TARGET_INSTRUCTION_MAX_BLOCKS ||
        !semantic_type_is_exact_i64(semantic, function->return_type))
        return false;
    /* Every declared parameter must be exact signed i64 before any row is
     * emitted; a signature the executor could not fill stays unavailable
     * instead of producing a group that silently drops an argument. */
    for (uint16_t parameter_ordinal = 0; parameter_ordinal < function->parameter_count;
         parameter_ordinal++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(semantic, function->parameter_begin + parameter_ordinal);
        if (!parameter || parameter->function != function_index ||
            parameter->ordinal != parameter_ordinal ||
            !semantic_type_is_exact_i64(semantic, parameter->type) ||
            !materialized_i64_slot(materialized, function_index, parameter->value, NULL))
            return false;
    }

    uint32_t operand_total = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_total);
    /* The block layout is frozen before any row is written, because a jump can
     * name a block that has not been emitted yet. */
    uint32_t *block_row = (uint32_t *) xr_calloc(function->block_count, sizeof(uint32_t));
    if (!block_row)
        return false;
    uint32_t group_rows = 0;
    bool admissible = true;
    for (uint32_t ordinal = 0; ordinal < function->block_count && admissible; ordinal++) {
        const XrSemanticBlockRecord *block = NULL;
        block_row[ordinal] = group_rows;
        if (!scalar_block_is_admissible(semantic, function, function_index,
                                        function->block_begin + ordinal, &block) ||
            block->operation_count > 40000000u - 1u - group_rows)
            admissible = false;
        else if (scalar_suspend_block_is_exact(builder, materialized, function_index, function,
                                               block, analysis, NULL))
            group_rows++;
        else {
            uint32_t emitted = 0;
            for (uint32_t i = 0; i < block->operation_count; i++)
                emitted += !analysis->elided_operations[block->operation_begin + i];
            group_rows += emitted + 1u;
        }
    }
    XrTargetInstructionRecord *group =
        admissible ? (XrTargetInstructionRecord *) xr_calloc(group_rows, sizeof(*group)) : NULL;
    if (!group) {
        xr_free(block_row);
        return false;
    }

    uint64_t bound_parameters = 0;
    uint32_t next_row = 0;
    for (uint32_t block_ordinal = 0; block_ordinal < function->block_count && admissible;
         block_ordinal++) {
        uint32_t block_index = function->block_begin + block_ordinal;
        const XrSemanticBlockRecord *block = NULL;
        if (!scalar_block_is_admissible(semantic, function, function_index, block_index, &block) ||
            next_row != block_row[block_ordinal]) {
            admissible = false;
            break;
        }
        uint32_t coroutine_state = XR_SEMANTIC_INDEX_NONE;
        if (scalar_suspend_block_is_exact(builder, materialized, function_index, function, block,
                                          analysis, &coroutine_state)) {
            XrTargetCoroutineStateRecord *state = &materialized->coroutines[coroutine_state];
            uint32_t resume_ordinal = state->resume_block - function->block_begin;
            state->resume_instruction = block_row[resume_ordinal];
            group[next_row++] = (XrTargetInstructionRecord) {
                .function = function_index,
                .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
                .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE, XR_TARGET_INSTRUCTION_SLOT_NONE},
                .immediate_bits =
                    XR_TARGET_INSTRUCTION_SUSPEND_PACK(coroutine_state, state->resume_instruction),
                .opcode = XR_TARGET_INSTRUCTION_SUSPEND,
                .operand_count = 0,
            };
            continue;
        }
        for (uint32_t ordinal = 0; ordinal < block->operation_count; ordinal++) {
            uint32_t operation_index = block->operation_begin + ordinal;
            if (analysis->elided_operations[operation_index])
                continue;
            const XrSemanticOperationRecord *operation =
                xr_semantic_plan_operation(semantic, operation_index);
            uint16_t opcode = operation ? scalar_instruction_opcode(operation->opcode)
                                        : XR_TARGET_INSTRUCTION_INVALID;
            uint32_t entry_index = operation_index < analysis->operation_count
                                       ? analysis->entry_by_operation[operation_index]
                                       : XR_SEMANTIC_INDEX_NONE;
            if (entry_index != XR_SEMANTIC_INDEX_NONE)
                opcode = XR_TARGET_INSTRUCTION_CALL_ENTRY_I64;
            const XrTargetInstructionContract *contract = xr_target_instruction_contract(opcode);
            uint32_t result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
            bool direct_call_dispatch =
                contract && contract->dispatch_kind == XR_TARGET_INSTRUCTION_DISPATCH_CALL;
            bool entry_call_dispatch =
                contract && contract->dispatch_kind == XR_TARGET_INSTRUCTION_DISPATCH_ENTRY_CALL;
            bool call_dispatch = direct_call_dispatch || entry_call_dispatch;
            uint32_t call_index = operation_index < analysis->operation_count
                                      ? analysis->call_by_operation[operation_index]
                                      : XR_SEMANTIC_INDEX_NONE;
            const XrTargetCallRecord *call =
                call_index < materialized->call_count ? &materialized->calls[call_index] : NULL;
            if (!operation || !contract || operation->function != function_index ||
                operation->block != block_index || (!call_dispatch && operation->effects != 0) ||
                (direct_call_dispatch &&
                 (!call || !scalar_direct_i64_call_is_exact(builder, materialized, call_index) ||
                  (executable_functions && !executable_functions[call->callee_function]))) ||
                (entry_call_dispatch &&
                 (!call || entry_index >= materialized->entry_expectation_count ||
                  materialized->entry_expectations[entry_index].call != call_index ||
                  !source_entry_call_is_exact(builder, materialized, call_index))) ||
                (!call_dispatch && operation->operand_count != contract->arity) ||
                operation->operand_begin > operand_total ||
                operation->operand_count > operand_total - operation->operand_begin) {
                admissible = false;
                break;
            }
            /* A relation answers a truth value and every other admitted
             * operation answers a signed i64, so the opcode decides which
             * result the operation must have declared. Both are proved exactly;
             * neither is inferred from what the operation happens to carry. */
            bool comparison = contract->result_rep == XR_TARGET_INSTRUCTION_REP_BOOL;
            if (comparison ? !semantic_type_is_exact_bool(semantic, operation->result_type) ||
                                 !materialized_scalar_slot(materialized, function_index,
                                                           operation->result_value,
                                                           XR_TARGET_SCALAR_SLOT_BOOL, &result_slot)
                           : !semantic_type_is_exact_i64(semantic, operation->result_type) ||
                                 !materialized_i64_slot(materialized, function_index,
                                                        operation->result_value, &result_slot)) {
                admissible = false;
                break;
            }
            if ((!call_dispatch && operation->opcode == XI_COPY &&
                 operation->semantic_immediate != XI_COPY_KIND_IDENTITY) ||
                (!call_dispatch && operation->opcode != XI_CONST && operation->opcode != XI_COPY &&
                 operation->opcode != XI_PARAM && operation->semantic_immediate != 0)) {
                admissible = false;
                break;
            }

            uint64_t immediate_bits = 0;
            if (call_dispatch) {
                if (operation->constant != XR_SEMANTIC_INDEX_NONE ||
                    call->result_slot != result_slot) {
                    admissible = false;
                    break;
                }
                immediate_bits = entry_call_dispatch ? entry_index : call_index;
            } else if (operation->opcode == XI_CONST) {
                const XrSemanticConstantRecord *constant =
                    xr_semantic_plan_constant(semantic, operation->constant);
                if (!constant || constant->kind != XR_SEM_CONST_INT ||
                    constant->type != operation->result_type) {
                    admissible = false;
                    break;
                }
                immediate_bits = (uint64_t) constant->integer;
            } else if (operation->constant != XR_SEMANTIC_INDEX_NONE) {
                admissible = false;
                break;
            }
            if (operation->opcode == XI_PARAM) {
                uint32_t parameter_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
                if (!scalar_parameter_row_is_exact(semantic, materialized, function_index, function,
                                                   operation, &immediate_bits, &parameter_slot) ||
                    parameter_slot != result_slot ||
                    (bound_parameters & (UINT64_C(1) << immediate_bits)) != 0) {
                    admissible = false;
                    break;
                }
                bound_parameters |= UINT64_C(1) << immediate_bits;
            }

            uint32_t operand_slots[2] = {XR_TARGET_INSTRUCTION_SLOT_NONE,
                                         XR_TARGET_INSTRUCTION_SLOT_NONE};
            for (uint16_t operand = 0; operand < contract->arity; operand++) {
                const XrSemanticOperandRecord *semantic_operand =
                    &operands[operation->operand_begin + operand];
                if (!semantic_type_is_exact_i64(semantic, semantic_operand->type) ||
                    !materialized_i64_slot(materialized, function_index, semantic_operand->value,
                                           &operand_slots[operand])) {
                    admissible = false;
                    break;
                }
            }
            if (!admissible)
                break;
            group[next_row++] = (XrTargetInstructionRecord) {
                .function = function_index,
                .result_slot = result_slot,
                .operand_slots = {operand_slots[0], operand_slots[1]},
                .immediate_bits = immediate_bits,
                .opcode = opcode,
                .operand_count = contract->arity,
            };
        }
        if (!admissible || !scalar_block_terminator_row(materialized, function_index, function,
                                                        block, block_row, &group[next_row]))
            admissible = false;
        else
            next_row++;
    }

    /* Dense coverage of the declared ordinals: a signature whose parameter is
     * never bound by a row would let the executor read an unfilled slot. */
    uint64_t declared_parameters = function->parameter_count == XR_TARGET_INSTRUCTION_MAX_PARAMETERS
                                       ? UINT64_MAX
                                       : (UINT64_C(1) << function->parameter_count) - 1u;
    /* Admission is decided by the very judgement the independent verifier will
     * apply, so a group can never be emitted that verification would then
     * refuse. Where a value is defined and where it is read are now separated
     * by control flow, and this is what proves the two agree on every path. */
    if (!admissible || next_row != group_rows || bound_parameters != declared_parameters ||
        !xr_target_instruction_rows_control_flow_is_exact(
            group, group_rows, materialized->functions[function_index].slot_begin,
            materialized->functions[function_index].slot_count, materialized->calls,
            materialized->call_count, materialized->call_arguments,
            materialized->call_argument_count, materialized->entry_expectations,
            materialized->entry_expectation_count, materialized->coroutines,
            materialized->coroutine_count)) {
        xr_free(group);
        xr_free(block_row);
        return false;
    }
    if (rows)
        for (uint32_t i = 0; i < group_rows; i++) {
            group[i].id = row_begin + i;
            rows[row_begin + i] = group[i];
        }
    if (out_row_count)
        *out_row_count = group_rows;
    xr_free(group);
    xr_free(block_row);
    return true;
}

typedef struct XrLeafAggregateInstructionShape {
    const XrSemanticProgramTypeBinding *aggregate;
    const XrSemanticProgramFunctionBinding *caller_binding;
    const XrSemanticProgramFunctionBinding *callee_binding;
    const XrSemanticFunctionRecord *caller;
    const XrSemanticFunctionRecord *callee;
    const XrSemanticParameterRecord *parameter;
    const XrSemanticOperationRecord *caller_ops[9];
    const XrSemanticOperationRecord *callee_ops[10];
    const XrTargetCallRecord *call;
    uint32_t call_index;
    uint32_t caller_operation_begin;
    uint32_t callee_operation_begin;
    uint32_t layout;
    uint32_t field[2];
    uint16_t aggregate_rep;
} XrLeafAggregateInstructionShape;

static bool leaf_operation_operands_are(const XrSemanticPlan *semantic,
                                        const XrSemanticOperationRecord *operation,
                                        uint16_t count, uint32_t first, uint32_t second) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    if (!operation || operation->operand_count != count || !operands ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin)
        return false;
    if (count > 0 && operands[operation->operand_begin].value != first)
        return false;
    return count < 2 || operands[operation->operand_begin + 1u].value == second;
}

static bool leaf_materialized_slot_is_exact(const XrTargetMaterializedPlan *materialized,
                                            uint32_t semantic_value, uint32_t function,
                                            uint32_t semantic_operation, uint8_t role,
                                            uint16_t rep, uint32_t size,
                                            uint32_t *out_slot) {
    const XrTargetValueRepRecord *value =
        find_materialized_value(materialized, semantic_value);
    const XrTargetSlotRecord *slot =
        value && value->slot < materialized->slot_count
            ? &materialized->slots[value->slot]
            : NULL;
    const XrTargetMachineRepRecord *machine =
        rep < materialized->machine_rep_count ? &materialized->machine_reps[rep] : NULL;
    if (!value || !slot || !machine || value->register_rep != rep || value->memory_rep != rep ||
        slot->id != value->slot || slot->function != function ||
        slot->semantic_value != semantic_value || slot->semantic_operation != semantic_operation ||
        slot->role != role || slot->register_rep != rep || slot->memory_rep != rep ||
        slot->size != size || slot->align != 8 || slot->root_kind != XR_TARGET_ROOT_NONE ||
        slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL || slot->reserved != 0 ||
        machine->memory_size != size || machine->memory_align != 8 ||
        machine->root_kind != XR_TARGET_ROOT_NONE ||
        machine->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
        return false;
    if (out_slot)
        *out_slot = value->slot;
    return true;
}

/* Rebuild the bounded PSC4 leaf program from pointer-free SemanticPlan rows.
 * The source class and field spellings are intentionally absent: the program
 * bindings select the aggregate, declaration ordinals select its two fields,
 * and the frozen SSA rows prove the constructor/get/set/call ordering. */
static bool leaf_aggregate_instruction_shape_is_exact(
    const XrTargetPlanBuilder *builder, const XrTargetMaterializedPlan *materialized,
    XrLeafAggregateInstructionShape *shape) {
    if (shape)
        memset(shape, 0, sizeof(*shape));
    const XrSemanticPlan *semantic = builder ? builder->semantic_plan : NULL;
    const XrSemanticProgramProvenance *provenance =
        semantic ? semantic_leaf_program_provenance(semantic) : NULL;
    if (!semantic || !materialized || !shape || !provenance)
        return false;

    for (uint32_t i = 0; i < provenance->function_count; i++) {
        const XrSemanticProgramFunctionBinding *binding =
            xr_semantic_plan_program_function_binding(semantic, i);
        if (!binding || binding->program_row >= provenance->function_count ||
            memcmp(binding->reserved, (uint8_t[3]) {0}, 3) != 0 ||
            binding->semantic_function >= materialized->function_count)
            return false;
        if ((binding->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) != 0) {
            if (shape->caller_binding)
                return false;
            shape->caller_binding = binding;
        } else if (binding->flags == 0) {
            if (shape->callee_binding)
                return false;
            shape->callee_binding = binding;
        } else
            return false;
    }
    if (!shape->caller_binding || !shape->callee_binding)
        return false;
    shape->caller = xr_semantic_plan_function(
        semantic, shape->caller_binding->semantic_function);
    shape->callee = xr_semantic_plan_function(
        semantic, shape->callee_binding->semantic_function);
    const XrSemanticProgramCallBinding *call_binding =
        xr_semantic_plan_program_call_binding(semantic, 0);
    const XrSemanticOperationRecord *call_operation =
        call_binding ? xr_semantic_plan_operation(semantic, call_binding->operation) : NULL;
    if (!shape->caller || !shape->callee || !call_binding || !call_operation ||
        !semantic_leaf_program_direct_call_is_exact(
            semantic, call_binding->operation, call_operation, shape->callee, &shape->aggregate) ||
        call_operation->function != shape->caller_binding->semantic_function ||
        call_binding->target_function != shape->callee_binding->semantic_function ||
        shape->caller->block_count != 1 || shape->callee->block_count != 1 ||
        shape->caller->parameter_count != 0 || shape->callee->parameter_count != 1 ||
        shape->caller->capture_count != 0 || shape->callee->capture_count != 0)
        return false;

    const XrSemanticBlockRecord *caller_block =
        xr_semantic_plan_block(semantic, shape->caller->block_begin);
    const XrSemanticBlockRecord *callee_block =
        xr_semantic_plan_block(semantic, shape->callee->block_begin);
    if (!caller_block || !callee_block || caller_block->function !=
            shape->caller_binding->semantic_function || callee_block->function !=
            shape->callee_binding->semantic_function || caller_block->kind != XI_BLOCK_RETURN ||
        callee_block->kind != XI_BLOCK_RETURN || caller_block->operation_count != 9 ||
        callee_block->operation_count != 10 ||
        caller_block->successors[0] != XR_SEMANTIC_INDEX_NONE ||
        caller_block->successors[1] != XR_SEMANTIC_INDEX_NONE ||
        callee_block->successors[0] != XR_SEMANTIC_INDEX_NONE ||
        callee_block->successors[1] != XR_SEMANTIC_INDEX_NONE)
        return false;
    shape->caller_operation_begin = caller_block->operation_begin;
    shape->callee_operation_begin = callee_block->operation_begin;
    for (uint32_t i = 0; i < 9; i++) {
        shape->caller_ops[i] =
            xr_semantic_plan_operation(semantic, caller_block->operation_begin + i);
        if (!shape->caller_ops[i] || shape->caller_ops[i]->function != caller_block->function ||
            shape->caller_ops[i]->block != shape->caller->block_begin)
            return false;
    }
    for (uint32_t i = 0; i < 10; i++) {
        shape->callee_ops[i] =
            xr_semantic_plan_operation(semantic, callee_block->operation_begin + i);
        if (!shape->callee_ops[i] || shape->callee_ops[i]->function != callee_block->function ||
            shape->callee_ops[i]->block != shape->callee->block_begin)
            return false;
    }
    shape->parameter = xr_semantic_plan_parameter(semantic, shape->callee->parameter_begin);
    const XrSemanticOperationRecord *const0 = shape->caller_ops[1];
    const XrSemanticOperationRecord *const1 = shape->caller_ops[2];
    const XrSemanticConstantRecord *constant0 =
        xr_semantic_plan_constant(semantic, const0->constant);
    const XrSemanticConstantRecord *constant1 =
        xr_semantic_plan_constant(semantic, const1->constant);
    if (!shape->parameter || shape->parameter->function !=
            shape->callee_binding->semantic_function || shape->parameter->ordinal != 0 ||
        shape->parameter->type != shape->aggregate->semantic_type ||
        shape->callee_ops[0]->opcode != XI_PARAM ||
        shape->callee_ops[0]->result_value != shape->parameter->value ||
        shape->callee_ops[0]->result_type != shape->aggregate->semantic_type ||
        shape->callee_ops[0]->semantic_immediate != 0 ||
        !leaf_operation_operands_are(semantic, shape->callee_ops[0], 0, 0, 0) ||
        shape->callee_ops[1]->opcode != XI_PLACE_LOAD ||
        shape->callee_ops[1]->result_type != shape->aggregate->semantic_type ||
        !leaf_operation_operands_are(semantic, shape->callee_ops[1], 1,
                                     shape->parameter->value, 0) ||
        shape->callee_ops[2]->opcode != XI_AGG_GET ||
        shape->callee_ops[2]->semantic_immediate != 1 ||
        !leaf_operation_operands_are(semantic, shape->callee_ops[2], 1,
                                     shape->callee_ops[1]->result_value, 0) ||
        shape->callee_ops[3]->opcode != XI_PLACE_LOAD ||
        !leaf_operation_operands_are(semantic, shape->callee_ops[3], 1,
                                     shape->parameter->value, 0) ||
        shape->callee_ops[4]->opcode != XI_AGG_GET ||
        shape->callee_ops[4]->semantic_immediate != 0 ||
        !leaf_operation_operands_are(semantic, shape->callee_ops[4], 1,
                                     shape->callee_ops[3]->result_value, 0) ||
        shape->callee_ops[5]->opcode != XI_GET_SHARED ||
        shape->callee_ops[6]->opcode != XI_RETAIN ||
        shape->callee_ops[6]->result_type != shape->callee_ops[5]->result_type ||
        !leaf_operation_operands_are(semantic, shape->callee_ops[6], 1,
                                     shape->callee_ops[5]->result_value, 0) ||
        shape->callee_ops[7]->opcode != XI_AGG_NEW ||
        shape->callee_ops[7]->result_type != shape->aggregate->semantic_type ||
        !leaf_operation_operands_are(semantic, shape->callee_ops[7], 1,
                                     shape->callee_ops[5]->result_value, 0) ||
        shape->callee_ops[8]->opcode != XI_AGG_SET ||
        shape->callee_ops[8]->semantic_immediate != 0 ||
        !leaf_operation_operands_are(semantic, shape->callee_ops[8], 2,
                                     shape->callee_ops[7]->result_value,
                                     shape->callee_ops[2]->result_value) ||
        shape->callee_ops[9]->opcode != XI_AGG_SET ||
        shape->callee_ops[9]->semantic_immediate != 1 ||
        !leaf_operation_operands_are(semantic, shape->callee_ops[9], 2,
                                     shape->callee_ops[7]->result_value,
                                     shape->callee_ops[4]->result_value) ||
        callee_block->control_value != shape->callee_ops[7]->result_value ||
        shape->caller_ops[0]->opcode != XI_GET_SHARED || const0->opcode != XI_CONST ||
        const1->opcode != XI_CONST || !constant0 || !constant1 ||
        constant0->kind != XR_SEM_CONST_INT || constant1->kind != XR_SEM_CONST_INT ||
        constant0->type != const0->result_type || constant1->type != const1->result_type ||
        semantic_leaf_program_type_kind(semantic, const0->result_type, NULL) !=
            XR_TARGET_LEAF_PROGRAM_TYPE_SCALAR ||
        const1->result_type != const0->result_type ||
        shape->caller_ops[3]->opcode != XI_GET_SHARED ||
        shape->caller_ops[4]->opcode != XI_RETAIN ||
        shape->caller_ops[4]->result_type != shape->caller_ops[3]->result_type ||
        !leaf_operation_operands_are(semantic, shape->caller_ops[4], 1,
                                     shape->caller_ops[3]->result_value, 0) ||
        shape->caller_ops[5]->opcode != XI_AGG_NEW ||
        shape->caller_ops[5]->result_type != shape->aggregate->semantic_type ||
        !leaf_operation_operands_are(semantic, shape->caller_ops[5], 1,
                                     shape->caller_ops[3]->result_value, 0) ||
        shape->caller_ops[6]->opcode != XI_AGG_SET ||
        shape->caller_ops[6]->semantic_immediate != 0 ||
        !leaf_operation_operands_are(semantic, shape->caller_ops[6], 2,
                                     shape->caller_ops[5]->result_value,
                                     const0->result_value) ||
        shape->caller_ops[7]->opcode != XI_AGG_SET ||
        shape->caller_ops[7]->semantic_immediate != 1 ||
        !leaf_operation_operands_are(semantic, shape->caller_ops[7], 2,
                                     shape->caller_ops[5]->result_value,
                                     const1->result_value) ||
        shape->caller_ops[8] != call_operation ||
        caller_block->control_value != call_operation->result_value)
        return false;

    uint32_t layout_matches = 0;
    for (uint32_t i = 0; i < materialized->layout_count; i++) {
        const XrTargetLayoutRecord *layout = &materialized->layouts[i];
        if (layout->semantic_type != shape->aggregate->semantic_type)
            continue;
        shape->layout = i;
        layout_matches++;
    }
    const XrTargetLayoutRecord *layout =
        layout_matches == 1 ? &materialized->layouts[shape->layout] : NULL;
    if (!layout || layout->id != shape->layout || layout->kind != XR_TARGET_LAYOUT_AGGREGATE ||
        layout->fixed_prefix_size != 16 || layout->align != 8 || layout->field_count != 2 ||
        layout->field_begin > materialized->field_count ||
        layout->field_count > materialized->field_count - layout->field_begin)
        return false;
    for (uint32_t ordinal = 0; ordinal < 2; ordinal++) {
        shape->field[ordinal] = layout->field_begin + ordinal;
        const XrTargetFieldRecord *field = &materialized->fields[shape->field[ordinal]];
        if (field->layout != shape->layout || field->semantic_field != ordinal ||
            field->semantic_name != XR_SEMANTIC_INDEX_NONE || field->offset != ordinal * 8u ||
            field->size != 8 || field->align != 8 || field->root_kind != XR_TARGET_ROOT_NONE ||
            field->flags != 0 || field->reserved != 0 ||
            field->memory_rep >= materialized->machine_rep_count ||
            materialized->machine_reps[field->memory_rep].kind != XR_MACHINE_REP_I64)
            return false;
    }
    uint32_t rep_matches = 0;
    for (uint32_t i = 0; i < materialized->machine_rep_count; i++) {
        const XrTargetMachineRepRecord *rep = &materialized->machine_reps[i];
        if (rep->kind == XR_MACHINE_REP_AGGREGATE && rep->detail == shape->layout) {
            shape->aggregate_rep = (uint16_t) i;
            rep_matches++;
        }
    }
    if (rep_matches != 1)
        return false;
    for (uint32_t i = 0; i < materialized->call_count; i++) {
        if (materialized->calls[i].semantic_operation != call_binding->operation)
            continue;
        if (shape->call)
            return false;
        shape->call = &materialized->calls[i];
        shape->call_index = i;
    }
    if (!shape->call || shape->call->id != shape->call_index ||
        shape->call->caller_function != shape->caller_binding->semantic_function ||
        shape->call->callee_function != shape->callee_binding->semantic_function ||
        shape->call->result_mode != XR_TARGET_CALL_CALLER_STORAGE ||
        shape->call->result_slot != shape->call->caller_storage_slot ||
        shape->call->argument_count != 1 || shape->call->adapter_count != 0 ||
        shape->call->result_register_rep != shape->aggregate_rep ||
        shape->call->result_memory_rep != shape->aggregate_rep)
        return false;
    return true;
}

static bool materialize_leaf_aggregate_instruction_function(
    const XrTargetPlanBuilder *builder, const XrTargetMaterializedPlan *materialized,
    uint32_t function_index, XrTargetInstructionRecord *rows, uint32_t row_begin,
    uint32_t *out_row_count) {
    if (out_row_count)
        *out_row_count = 0;
    XrLeafAggregateInstructionShape shape = {0};
    if (!leaf_aggregate_instruction_shape_is_exact(builder, materialized, &shape))
        return false;
    bool callee = function_index == shape.callee_binding->semantic_function;
    bool caller = function_index == shape.caller_binding->semantic_function;
    if (!callee && !caller)
        return false;

    uint32_t slot[5] = {0};
    XrTargetInstructionRecord group[5] = {0};
    if (callee) {
        if (!leaf_materialized_slot_is_exact(
                materialized, shape.parameter->value, function_index,
                XR_SEMANTIC_INDEX_NONE, XR_TARGET_SLOT_PARAMETER,
                shape.aggregate_rep, 16, &slot[0]) ||
            !leaf_materialized_slot_is_exact(
                materialized, shape.callee_ops[2]->result_value, function_index,
                shape.callee_operation_begin + 2u, XR_TARGET_SLOT_TEMPORARY,
                materialized->fields[shape.field[1]].memory_rep, 8, &slot[1]) ||
            !leaf_materialized_slot_is_exact(
                materialized, shape.callee_ops[4]->result_value, function_index,
                shape.callee_operation_begin + 4u, XR_TARGET_SLOT_TEMPORARY,
                materialized->fields[shape.field[0]].memory_rep, 8, &slot[2]) ||
            !leaf_materialized_slot_is_exact(
                materialized, shape.callee_ops[7]->result_value, function_index,
                shape.callee_operation_begin + 7u, XR_TARGET_SLOT_TEMPORARY,
                shape.aggregate_rep, 16, &slot[3]))
            return false;
        group[0] = (XrTargetInstructionRecord) {
            .function = function_index, .result_slot = slot[0],
            .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE,
                              XR_TARGET_INSTRUCTION_SLOT_NONE},
            .opcode = XR_TARGET_INSTRUCTION_PARAM_AGGREGATE, .immediate_bits = 0,
        };
        group[1] = (XrTargetInstructionRecord) {
            .function = function_index, .result_slot = slot[1],
            .operand_slots = {slot[0], XR_TARGET_INSTRUCTION_SLOT_NONE},
            .opcode = XR_TARGET_INSTRUCTION_AGGREGATE_GET_I64, .operand_count = 1,
            .immediate_bits = shape.field[1],
        };
        group[2] = (XrTargetInstructionRecord) {
            .function = function_index, .result_slot = slot[2],
            .operand_slots = {slot[0], XR_TARGET_INSTRUCTION_SLOT_NONE},
            .opcode = XR_TARGET_INSTRUCTION_AGGREGATE_GET_I64, .operand_count = 1,
            .immediate_bits = shape.field[0],
        };
        group[3] = (XrTargetInstructionRecord) {
            .function = function_index, .result_slot = slot[3],
            .operand_slots = {slot[1], slot[2]},
            .opcode = XR_TARGET_INSTRUCTION_AGGREGATE_MAKE_I64X2, .operand_count = 2,
            .immediate_bits = shape.layout,
        };
        group[4] = (XrTargetInstructionRecord) {
            .function = function_index,
            .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
            .operand_slots = {slot[3], XR_TARGET_INSTRUCTION_SLOT_NONE},
            .opcode = XR_TARGET_INSTRUCTION_RETURN_AGGREGATE, .operand_count = 1,
        };
    } else {
        const XrSemanticConstantRecord *constant0 = xr_semantic_plan_constant(
            builder->semantic_plan, shape.caller_ops[1]->constant);
        const XrSemanticConstantRecord *constant1 = xr_semantic_plan_constant(
            builder->semantic_plan, shape.caller_ops[2]->constant);
        if (!constant0 || !constant1 ||
            !leaf_materialized_slot_is_exact(
                materialized, shape.caller_ops[1]->result_value, function_index,
                shape.caller_operation_begin + 1u, XR_TARGET_SLOT_TEMPORARY,
                materialized->fields[shape.field[0]].memory_rep, 8, &slot[0]) ||
            !leaf_materialized_slot_is_exact(
                materialized, shape.caller_ops[2]->result_value, function_index,
                shape.caller_operation_begin + 2u, XR_TARGET_SLOT_TEMPORARY,
                materialized->fields[shape.field[1]].memory_rep, 8, &slot[1]) ||
            !leaf_materialized_slot_is_exact(
                materialized, shape.caller_ops[5]->result_value, function_index,
                shape.caller_operation_begin + 5u, XR_TARGET_SLOT_TEMPORARY,
                shape.aggregate_rep, 16, &slot[2]) ||
            !leaf_materialized_slot_is_exact(
                materialized, shape.caller_ops[8]->result_value, function_index,
                shape.caller_operation_begin + 8u, XR_TARGET_SLOT_TEMPORARY,
                shape.aggregate_rep, 16, &slot[3]))
            return false;
        group[0] = (XrTargetInstructionRecord) {
            .function = function_index, .result_slot = slot[0],
            .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE,
                              XR_TARGET_INSTRUCTION_SLOT_NONE},
            .opcode = XR_TARGET_INSTRUCTION_CONST_I64,
            .immediate_bits = (uint64_t) constant0->integer,
        };
        group[1] = (XrTargetInstructionRecord) {
            .function = function_index, .result_slot = slot[1],
            .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE,
                              XR_TARGET_INSTRUCTION_SLOT_NONE},
            .opcode = XR_TARGET_INSTRUCTION_CONST_I64,
            .immediate_bits = (uint64_t) constant1->integer,
        };
        group[2] = (XrTargetInstructionRecord) {
            .function = function_index, .result_slot = slot[2],
            .operand_slots = {slot[0], slot[1]},
            .opcode = XR_TARGET_INSTRUCTION_AGGREGATE_MAKE_I64X2, .operand_count = 2,
            .immediate_bits = shape.layout,
        };
        group[3] = (XrTargetInstructionRecord) {
            .function = function_index, .result_slot = slot[3],
            .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE,
                              XR_TARGET_INSTRUCTION_SLOT_NONE},
            .opcode = XR_TARGET_INSTRUCTION_CALL_DIRECT_AGGREGATE,
            .immediate_bits = shape.call_index,
        };
        group[4] = (XrTargetInstructionRecord) {
            .function = function_index,
            .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
            .operand_slots = {slot[3], XR_TARGET_INSTRUCTION_SLOT_NONE},
            .opcode = XR_TARGET_INSTRUCTION_RETURN_AGGREGATE, .operand_count = 1,
        };
    }
    if (!xr_target_instruction_rows_control_flow_is_exact(
            group, 5, materialized->functions[function_index].slot_begin,
            materialized->functions[function_index].slot_count, materialized->calls,
            materialized->call_count, materialized->call_arguments,
            materialized->call_argument_count, materialized->entry_expectations,
            materialized->entry_expectation_count, materialized->coroutines,
            materialized->coroutine_count))
        return false;
    if (rows)
        for (uint32_t i = 0; i < 5; i++) {
            group[i].id = row_begin + i;
            rows[row_begin + i] = group[i];
        }
    if (out_row_count)
        *out_row_count = 5;
    return true;
}

typedef struct XrLeafProductInstructionShape {
    const XrSemanticProgramTypeBinding *product;
    const XrSemanticProgramFunctionBinding *callers[2];
    const XrSemanticProgramFunctionBinding *callee;
    const XrSemanticOperationRecord *constructs[3];
    const XrSemanticOperationRecord *projects[2][6];
    const XrSemanticOperationRecord *callables[2];
    const XrSemanticOperationRecord *values[6];
    const XrSemanticOperationRecord *literal_ops[6];
    const XrTargetCallRecord *calls[2];
    uint32_t call_indices[2];
    uint32_t layout;
    uint32_t fields[6];
    uint16_t aggregate_rep;
} XrLeafProductInstructionShape;

static const XrSemanticOperationRecord *product_operation_for_value(
    const XrSemanticPlan *semantic, const XrSemanticBlockRecord *block, uint32_t value,
    uint32_t *out_index) {
    const XrSemanticOperationRecord *match = NULL;
    for (uint32_t i = 0; block && i < block->operation_count; i++) {
        uint32_t index = block->operation_begin + i;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, index);
        if (!operation || operation->result_value != value)
            continue;
        if (match)
            return NULL;
        match = operation;
        if (out_index)
            *out_index = index;
    }
    return match;
}

static bool leaf_product_instruction_shape_is_exact(
    const XrTargetPlanBuilder *builder, const XrTargetMaterializedPlan *materialized,
    XrLeafProductInstructionShape *shape) {
    if (shape)
        memset(shape, 0, sizeof(*shape));
    const XrSemanticPlan *semantic = builder ? builder->semantic_plan : NULL;
    const XrSemanticProgramProvenance *provenance =
        semantic ? semantic_product_program_provenance(semantic) : NULL;
    if (!semantic || !materialized || !shape || !provenance)
        return false;
    uint32_t caller_count = 0;
    for (uint32_t i = 0; i < provenance->function_count; i++) {
        const XrSemanticProgramFunctionBinding *binding =
            xr_semantic_plan_program_function_binding(semantic, i);
        if (!binding || binding->program_row >= provenance->function_count ||
            binding->semantic_function >= materialized->function_count ||
            memcmp(binding->reserved, (uint8_t[3]) {0}, 3) != 0)
            return false;
        if (binding->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) {
            if (caller_count >= 2)
                return false;
            shape->callers[caller_count++] = binding;
        } else if (binding->flags == 0) {
            if (shape->callee)
                return false;
            shape->callee = binding;
        } else {
            return false;
        }
    }
    if (caller_count != 2 || !shape->callee)
        return false;
    for (uint32_t i = 0; i < provenance->type_count; i++) {
        const XrSemanticProgramTypeBinding *binding =
            xr_semantic_plan_program_type_binding(semantic, i);
        if (binding && binding->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT) {
            if (shape->product)
                return false;
            shape->product = binding;
        }
    }
    if (!shape->product || shape->product->field_count != 6)
        return false;

    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    for (uint32_t role = 0; role < 3; role++) {
        const XrSemanticProgramFunctionBinding *binding =
            role < 2 ? shape->callers[role] : shape->callee;
        const XrSemanticFunctionRecord *function =
            xr_semantic_plan_function(semantic, binding->semantic_function);
        const XrSemanticBlockRecord *block =
            function && function->block_count == 1
                ? xr_semantic_plan_block(semantic, function->block_begin)
                : NULL;
        if (!function || !block || function->parameter_count != 0 ||
            function->capture_count != 0 || function->return_type != shape->product->semantic_type ||
            block->function != binding->semantic_function || block->kind != XI_BLOCK_RETURN ||
            block->successors[0] != XR_SEMANTIC_INDEX_NONE ||
            block->successors[1] != XR_SEMANTIC_INDEX_NONE)
            return false;
        uint32_t constructs = 0, projects = 0, calls = 0, callables = 0;
        for (uint32_t i = 0; i < block->operation_count; i++) {
            uint32_t operation_index = block->operation_begin + i;
            const XrSemanticOperationRecord *operation =
                xr_semantic_plan_operation(semantic, operation_index);
            if (!operation || operation->function != binding->semantic_function ||
                operation->block != function->block_begin)
                return false;
            if (operation->opcode == XI_VALUE_PRODUCT_CONSTRUCT) {
                if (++constructs != 1 || operation->result_type != shape->product->semantic_type ||
                    operation->operand_count != 6 || operation->semantic_immediate != 6 ||
                    operation->operand_begin > operand_count ||
                    operation->operand_count > operand_count - operation->operand_begin)
                    return false;
                shape->constructs[role] = operation;
            } else if (operation->opcode == XI_VALUE_PRODUCT_PROJECT) {
                if (role >= 2 || operation->semantic_immediate < 0 ||
                    operation->semantic_immediate >= 6 || operation->operand_count != 1 ||
                    operation->operand_begin >= operand_count ||
                    shape->projects[role][operation->semantic_immediate])
                    return false;
                shape->projects[role][operation->semantic_immediate] = operation;
                projects++;
            } else if (operation->opcode == XI_CALL) {
                const XrSemanticFunctionRecord *callee =
                    xr_semantic_plan_function(semantic, shape->callee->semantic_function);
                if (role >= 2 || ++calls != 1 ||
                    !semantic_product_direct_call_is_exact(
                        semantic, operation_index, operation, callee, NULL))
                    return false;
                const XrTargetCallRecord *target_call = NULL;
                uint32_t target_index = XR_SEMANTIC_INDEX_NONE;
                for (uint32_t call = 0; call < materialized->call_count; call++) {
                    if (materialized->calls[call].semantic_operation != operation_index)
                        continue;
                    if (target_call)
                        return false;
                    target_call = &materialized->calls[call];
                    target_index = call;
                }
                if (!target_call || target_call->id != target_index ||
                    target_call->caller_function != binding->semantic_function ||
                    target_call->callee_function != shape->callee->semantic_function ||
                    target_call->result_mode != XR_TARGET_CALL_CALLER_STORAGE ||
                    target_call->result_slot != target_call->caller_storage_slot ||
                    target_call->argument_count != 0 || target_call->adapter_count != 0 ||
                    target_call->result_ownership != XR_TARGET_CALL_NONE)
                    return false;
                shape->calls[role] = target_call;
                shape->call_indices[role] = target_index;
            } else if (operation->opcode == XI_GET_SHARED) {
                if (role >= 2 || ++callables != 1)
                    return false;
                shape->callables[role] = operation;
            } else if (operation->opcode != XI_CONST && operation->opcode != XI_NARROW_U8) {
                return false;
            }
        }
        if (constructs != 1 || (role < 2 ? projects != 6 || calls != 1
                                         : projects != 0 || calls != 0) ||
            (role < 2 ? callables != 1 : callables != 0) ||
            block->control_value != shape->constructs[role]->result_value)
            return false;
        if (role < 2) {
            if (!shape->calls[role] ||
                shape->constructs[role]->operand_begin > operand_count)
                return false;
            const XrSemanticOperationRecord *call_operation =
                xr_semantic_plan_operation(semantic,
                                           shape->calls[role]->semantic_operation);
            if (!call_operation || call_operation->operand_count != 1 ||
                call_operation->operand_begin >= operand_count ||
                operands[call_operation->operand_begin].value !=
                    shape->callables[role]->result_value)
                return false;
            for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
                const XrSemanticOperationRecord *project = shape->projects[role][ordinal];
                if (!project || project->semantic_immediate != (int64_t) ordinal ||
                    project->operand_begin >= operand_count ||
                    operands[project->operand_begin].value != shape->calls[role]->result_value ||
                    operands[shape->constructs[role]->operand_begin + ordinal].value !=
                        project->result_value)
                    return false;
            }
        } else {
            for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
                uint32_t value = operands[shape->constructs[role]->operand_begin + ordinal].value;
                const XrSemanticOperationRecord *source =
                    product_operation_for_value(semantic, block, value, NULL);
                const XrSemanticProgramTypeFieldBinding *field =
                    xr_semantic_plan_program_type_field_binding(
                        semantic, shape->product->field_begin + ordinal);
                const XrSemanticOperationRecord *constant = source;
                if (ordinal == 2) {
                    if (!source || source->opcode != XI_NARROW_U8 || source->operand_count != 1 ||
                        source->operand_begin >= operand_count || !field ||
                        source->result_type != field->semantic_field_type)
                        return false;
                    constant = product_operation_for_value(
                        semantic, block, operands[source->operand_begin].value, NULL);
                }
                const XrSemanticConstantRecord *literal =
                    constant ? xr_semantic_plan_constant(semantic, constant->constant) : NULL;
                if (!constant || !literal || constant->opcode != XI_CONST ||
                    literal->kind != XR_SEM_CONST_INT || literal->type != constant->result_type ||
                    !field || field->declaration_ordinal != ordinal ||
                    (ordinal != 2 && constant->result_type != field->semantic_field_type) ||
                    (ordinal == 2 &&
                     (literal->integer < 0 || literal->integer > UINT8_MAX)))
                    return false;
                shape->values[ordinal] = source;
                shape->literal_ops[ordinal] = constant;
            }
        }
    }

    uint32_t layout_matches = 0;
    for (uint32_t i = 0; i < materialized->layout_count; i++) {
        if (materialized->layouts[i].semantic_type != shape->product->semantic_type)
            continue;
        shape->layout = i;
        layout_matches++;
    }
    const XrTargetLayoutRecord *layout =
        layout_matches == 1 ? &materialized->layouts[shape->layout] : NULL;
    if (!layout || layout->kind != XR_TARGET_LAYOUT_AGGREGATE ||
        layout->fixed_prefix_size != 48 || layout->align != 8 || layout->field_count != 6 ||
        layout->field_begin > materialized->field_count ||
        layout->field_count > materialized->field_count - layout->field_begin)
        return false;
    for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
        shape->fields[ordinal] = layout->field_begin + ordinal;
        const XrTargetFieldRecord *field = &materialized->fields[shape->fields[ordinal]];
        uint16_t expected_kind = ordinal == 2 ? XR_MACHINE_REP_U8 : XR_MACHINE_REP_I64;
        uint32_t expected_size = ordinal == 2 ? 1u : 8u;
        uint16_t expected_align = ordinal == 2 ? 1u : 8u;
        if (field->layout != shape->layout || field->semantic_field != ordinal ||
            field->semantic_name != XR_SEMANTIC_INDEX_NONE || field->offset != ordinal * 8u ||
            field->size != expected_size || field->align != expected_align ||
            field->root_kind != XR_TARGET_ROOT_NONE || field->flags != 0 ||
            field->reserved != 0 || field->memory_rep >= materialized->machine_rep_count ||
            materialized->machine_reps[field->memory_rep].kind != expected_kind)
            return false;
    }
    uint32_t rep_matches = 0;
    for (uint32_t i = 0; i < materialized->machine_rep_count; i++) {
        const XrTargetMachineRepRecord *rep = &materialized->machine_reps[i];
        if (rep->kind == XR_MACHINE_REP_AGGREGATE && rep->detail == shape->layout) {
            shape->aggregate_rep = (uint16_t) i;
            rep_matches++;
            if (rep->register_bits != 384 || rep->memory_size != 48 ||
                rep->memory_align != 8 || rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
                return false;
        }
    }
    return rep_matches == 1;
}

static bool product_materialized_slot_is_exact(
    const XrTargetMaterializedPlan *materialized, uint32_t semantic_value, uint32_t function,
    uint32_t semantic_operation, uint8_t role, uint16_t rep, uint32_t size, uint16_t align,
    uint32_t *out_slot) {
    const XrTargetValueRepRecord *value = find_materialized_value(materialized, semantic_value);
    const XrTargetSlotRecord *slot =
        value && value->slot < materialized->slot_count ? &materialized->slots[value->slot] : NULL;
    const XrTargetMachineRepRecord *machine =
        rep < materialized->machine_rep_count ? &materialized->machine_reps[rep] : NULL;
    if (!value || !slot || !machine || value->register_rep != rep || value->memory_rep != rep ||
        slot->function != function || slot->semantic_value != semantic_value ||
        slot->semantic_operation != semantic_operation || slot->role != role ||
        slot->register_rep != rep || slot->memory_rep != rep || slot->size != size ||
        slot->align != align || slot->root_kind != XR_TARGET_ROOT_NONE ||
        slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL || machine->memory_size != size ||
        machine->memory_align != align || machine->root_kind != XR_TARGET_ROOT_NONE ||
        machine->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
        return false;
    if (out_slot)
        *out_slot = value->slot;
    return true;
}

static bool materialize_leaf_product_instruction_function(
    const XrTargetPlanBuilder *builder, const XrTargetMaterializedPlan *materialized,
    uint32_t function_index, XrTargetInstructionRecord *rows, uint32_t row_begin,
    uint32_t *out_row_count) {
    if (out_row_count)
        *out_row_count = 0;
    XrLeafProductInstructionShape shape = {0};
    if (!leaf_product_instruction_shape_is_exact(builder, materialized, &shape))
        return false;
    uint32_t role = function_index == shape.callers[0]->semantic_function
                        ? 0
                    : function_index == shape.callers[1]->semantic_function
                        ? 1
                    : function_index == shape.callee->semantic_function ? 2 : UINT32_MAX;
    if (role == UINT32_MAX)
        return false;
    XrTargetInstructionRecord group[15] = {0};
    uint32_t scalar_slots[6] = {0};
    uint32_t product_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
    uint32_t next = 0;
    const XrSemanticFunctionRecord *semantic_function =
        xr_semantic_plan_function(builder->semantic_plan, function_index);
    const XrSemanticBlockRecord *block =
        semantic_function ? xr_semantic_plan_block(builder->semantic_plan,
                                                   semantic_function->block_begin)
                          : NULL;
    if (!block)
        return false;
    if (role == 2) {
        for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
            const XrSemanticOperationRecord *value = shape.values[ordinal];
            const XrSemanticOperationRecord *constant = shape.literal_ops[ordinal];
            uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
            product_operation_for_value(builder->semantic_plan, block, value->result_value,
                                        &operation_index);
            const XrSemanticConstantRecord *literal =
                xr_semantic_plan_constant(builder->semantic_plan, constant->constant);
            const XrTargetFieldRecord *field = &materialized->fields[shape.fields[ordinal]];
            if (!literal || !product_materialized_slot_is_exact(
                                materialized, value->result_value, function_index,
                                operation_index, XR_TARGET_SLOT_TEMPORARY, field->memory_rep,
                                field->size, field->align, &scalar_slots[ordinal]))
                return false;
            group[next++] = (XrTargetInstructionRecord) {
                .function = function_index,
                .result_slot = scalar_slots[ordinal],
                .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE,
                                  XR_TARGET_INSTRUCTION_SLOT_NONE},
                .opcode = ordinal == 2 ? XR_TARGET_INSTRUCTION_CONST_U8
                                       : XR_TARGET_INSTRUCTION_CONST_I64,
                .immediate_bits = (uint64_t) literal->integer,
            };
        }
    } else {
        if (!product_materialized_slot_is_exact(
                materialized, shape.calls[role]->result_value, function_index,
                shape.calls[role]->semantic_operation, XR_TARGET_SLOT_TEMPORARY,
                shape.aggregate_rep, 48, 8, &product_slot))
            return false;
        group[next++] = (XrTargetInstructionRecord) {
            .function = function_index,
            .result_slot = product_slot,
            .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE,
                              XR_TARGET_INSTRUCTION_SLOT_NONE},
            .opcode = XR_TARGET_INSTRUCTION_CALL_DIRECT_AGGREGATE,
            .immediate_bits = shape.call_indices[role],
        };
        for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
            const XrSemanticOperationRecord *project = shape.projects[role][ordinal];
            const XrTargetFieldRecord *field = &materialized->fields[shape.fields[ordinal]];
            uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
            if (product_operation_for_value(builder->semantic_plan, block,
                                            project->result_value,
                                            &operation_index) != project)
                return false;
            if (!product_materialized_slot_is_exact(
                    materialized, project->result_value, function_index, operation_index,
                    XR_TARGET_SLOT_TEMPORARY, field->memory_rep, field->size, field->align,
                    &scalar_slots[ordinal]))
                return false;
            group[next++] = (XrTargetInstructionRecord) {
                .function = function_index,
                .result_slot = scalar_slots[ordinal],
                .operand_slots = {product_slot, XR_TARGET_INSTRUCTION_SLOT_NONE},
                .opcode = ordinal == 2 ? XR_TARGET_INSTRUCTION_VALUE_PRODUCT_GET_U8
                                       : XR_TARGET_INSTRUCTION_AGGREGATE_GET_I64,
                .operand_count = 1,
                .immediate_bits = shape.fields[ordinal],
            };
        }
    }
    const XrSemanticOperationRecord *construct = shape.constructs[role];
    uint32_t construct_index = XR_SEMANTIC_INDEX_NONE;
    if (product_operation_for_value(builder->semantic_plan, block,
                                    construct->result_value,
                                    &construct_index) != construct)
        return false;
    uint32_t construct_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
    if (!product_materialized_slot_is_exact(
            materialized, construct->result_value, function_index, construct_index,
            XR_TARGET_SLOT_TEMPORARY, shape.aggregate_rep, 48, 8, &construct_slot))
        return false;
    group[next++] = (XrTargetInstructionRecord) {
        .function = function_index,
        .result_slot = construct_slot,
        .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE,
                          XR_TARGET_INSTRUCTION_SLOT_NONE},
        .opcode = XR_TARGET_INSTRUCTION_VALUE_PRODUCT_INIT,
        .immediate_bits = shape.layout,
    };
    for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
        group[next++] = (XrTargetInstructionRecord) {
            .function = function_index,
            .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
            .operand_slots = {construct_slot, scalar_slots[ordinal]},
            .opcode = ordinal == 2 ? XR_TARGET_INSTRUCTION_VALUE_PRODUCT_SET_U8
                                   : XR_TARGET_INSTRUCTION_VALUE_PRODUCT_SET_I64,
            .operand_count = 2,
            .immediate_bits = shape.fields[ordinal],
        };
    }
    group[next++] = (XrTargetInstructionRecord) {
        .function = function_index,
        .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
        .operand_slots = {construct_slot, XR_TARGET_INSTRUCTION_SLOT_NONE},
        .opcode = XR_TARGET_INSTRUCTION_RETURN_AGGREGATE,
        .operand_count = 1,
    };
    uint32_t expected = role == 2 ? 14u : 15u;
    if (next != expected ||
        !xr_target_instruction_rows_control_flow_is_exact(
            group, next, materialized->functions[function_index].slot_begin,
            materialized->functions[function_index].slot_count, materialized->calls,
            materialized->call_count, materialized->call_arguments,
            materialized->call_argument_count, materialized->entry_expectations,
            materialized->entry_expectation_count, materialized->coroutines,
            materialized->coroutine_count))
        return false;
    if (rows)
        for (uint32_t i = 0; i < next; i++) {
            group[i].id = row_begin + i;
            rows[row_begin + i] = group[i];
        }
    if (out_row_count)
        *out_row_count = next;
    return true;
}

/* The managed tagged Array.push family is deliberately separate from the
 * scalar family.  Its two parameter slots carry XrValue, the element slot is
 * consumed by the side-effect row, and the unit return has no result slot at
 * all.  CEmissionPlan is not consulted: the executable row binds directly to
 * the already materialized Target call authority. */
static bool materialized_dyn_parameter_slot_is_exact(const XrTargetMaterializedPlan *materialized,
                                                     uint32_t function, uint32_t semantic_value,
                                                     uint8_t ownership, uint32_t *out_slot) {
    const XrTargetValueRepRecord *value = find_materialized_value(materialized, semantic_value);
    if (!value || value->slot >= materialized->slot_count ||
        value->register_rep >= materialized->machine_rep_count ||
        value->memory_rep >= materialized->machine_rep_count)
        return false;
    const XrTargetSlotRecord *slot = &materialized->slots[value->slot];
    const XrTargetMachineRepRecord *register_rep = &materialized->machine_reps[value->register_rep];
    const XrTargetMachineRepRecord *memory_rep = &materialized->machine_reps[value->memory_rep];
    if (slot->id != value->slot || slot->function != function ||
        slot->semantic_value != semantic_value ||
        slot->semantic_operation != XR_SEMANTIC_INDEX_NONE ||
        slot->role != XR_TARGET_SLOT_PARAMETER || slot->register_rep != value->register_rep ||
        slot->memory_rep != value->memory_rep || slot->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        slot->ownership != ownership || register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        register_rep->root_kind != XR_TARGET_ROOT_DYNAMIC ||
        memory_rep->root_kind != XR_TARGET_ROOT_DYNAMIC || register_rep->ownership != ownership ||
        memory_rep->ownership != ownership ||
        register_rep->memory_size != memory_rep->memory_size ||
        register_rep->memory_align != memory_rep->memory_align ||
        slot->size != memory_rep->memory_size || slot->align != memory_rep->memory_align)
        return false;
    if (out_slot)
        *out_slot = value->slot;
    return true;
}

static bool tagged_array_push_parameter_operation_is_exact(
    const XrSemanticPlan *semantic, const XrSemanticFunctionRecord *function,
    const XrSemanticBlockRecord *block, uint32_t function_index,
    const XrSemanticParameterRecord *parameter, uint16_t ordinal) {
    if (!semantic || !function || !block || !parameter || parameter->function != function_index ||
        parameter->ordinal != ordinal || parameter->mode != XR_PARAM_READ ||
        parameter->transfer_mode != XR_TRANSFER_SHARE ||
        parameter->flags != XR_SEM_PARAMETER_REQUIRED || parameter->reserved != 0)
        return false;
    uint32_t matches = 0;
    for (uint32_t i = 0; i < block->operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, block->operation_begin + i);
        if (!operation || operation->opcode != XI_PARAM ||
            operation->result_value != parameter->value)
            continue;
        matches++;
        if (operation->function != function_index || operation->block != function->block_begin ||
            operation->result_type != parameter->type || operation->operand_count != 0 ||
            operation->semantic_immediate != ordinal ||
            operation->constant != XR_SEMANTIC_INDEX_NONE ||
            operation->effects != xi_generated_op_effects(XI_PARAM))
            return false;
    }
    return matches == 1;
}

static bool materialize_tagged_array_push_instruction_function(
    const XrTargetPlanBuilder *builder, const XrTargetMaterializedPlan *materialized,
    uint32_t function_index, XrTargetInstructionRecord *rows, uint32_t row_begin,
    uint32_t *out_row_count) {
    if (out_row_count)
        *out_row_count = 0;
    const XrSemanticPlan *semantic = builder ? builder->semantic_plan : NULL;
    const XrSemanticFunctionRecord *function =
        semantic ? xr_semantic_plan_function(semantic, function_index) : NULL;
    const XrSemanticBlockRecord *block =
        function && function->block_count == 1
            ? xr_semantic_plan_block(semantic, function->block_begin)
            : NULL;
    if (!semantic || !materialized || !function || !block || function->parameter_count != 2 ||
        function->capture_count != 0 || block->function != function_index ||
        block->kind != XI_BLOCK_RETURN || block->operation_count != 3 ||
        block->successors[0] != XR_SEMANTIC_INDEX_NONE ||
        block->successors[1] != XR_SEMANTIC_INDEX_NONE)
        return false;

    uint32_t call_index = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *push = NULL;
    for (uint32_t i = 0; i < materialized->call_count; i++) {
        const XrTargetCallRecord *candidate = &materialized->calls[i];
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, candidate->semantic_operation);
        if (candidate->caller_function != function_index ||
            !semantic_array_member_tagged_store_is_exact(semantic, operation, NULL, NULL, NULL))
            continue;
        if (push)
            return false;
        push = operation;
        call_index = i;
    }
    if (!push || block->control_value != push->result_value ||
        !xr_semantic_array_member_unit_type_is_exact(
            xr_semantic_plan_type(semantic, function->return_type)) ||
        call_index >= materialized->call_count)
        return false;
    const XrTargetCallRecord *call = &materialized->calls[call_index];
    if (call->id != call_index ||
        call->semantic_operation >= xr_semantic_plan_operation_count(semantic) ||
        call->argument_count != 2 || call->argument_begin > materialized->call_argument_count ||
        call->argument_count > materialized->call_argument_count - call->argument_begin ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_ARRAY_MEMBER_SCALAR ||
        call->target_kind != XR_TARGET_CALL_TARGET_ARRAY_MEMBER_SCALAR ||
        call->array_element_storage != XR_TARGET_ARRAY_STORAGE_TAGGED ||
        call->array_intrinsic_kind != XR_TARGET_ARRAY_INTRINSIC_NONE ||
        call->array_hof_kind != XR_TARGET_ARRAY_HOF_NONE ||
        call->array_result_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->result_slot != XR_SEMANTIC_INDEX_NONE || call->flags != 0)
        return false;

    const XrSemanticParameterRecord *receiver_parameter =
        xr_semantic_plan_parameter(semantic, function->parameter_begin);
    const XrSemanticParameterRecord *element_parameter =
        xr_semantic_plan_parameter(semantic, function->parameter_begin + 1u);
    const XrTargetCallArgumentRecord *receiver =
        &materialized->call_arguments[call->argument_begin];
    const XrTargetCallArgumentRecord *element = receiver + 1;
    uint32_t receiver_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
    uint32_t element_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
    if (!receiver_parameter || !element_parameter ||
        receiver_parameter->ownership != XI_OWN_BORROWED ||
        element_parameter->ownership != XI_OWN_OWNED ||
        !tagged_array_push_parameter_operation_is_exact(semantic, function, block, function_index,
                                                        receiver_parameter, 0) ||
        !tagged_array_push_parameter_operation_is_exact(semantic, function, block, function_index,
                                                        element_parameter, 1) ||
        !materialized_dyn_parameter_slot_is_exact(materialized, function_index,
                                                  receiver_parameter->value,
                                                  XR_TARGET_OWNERSHIP_BORROWED, &receiver_slot) ||
        !materialized_dyn_parameter_slot_is_exact(materialized, function_index,
                                                  element_parameter->value,
                                                  XR_TARGET_OWNERSHIP_OWNED, &element_slot) ||
        receiver->call != call_index || receiver->ordinal != 0 ||
        receiver->semantic_value != receiver_parameter->value ||
        receiver->caller_slot != receiver_slot || receiver->mode != XR_TARGET_CALL_VALUE ||
        receiver->ownership != XR_TARGET_CALL_BORROW || receiver->flags != 0 ||
        receiver->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        element->call != call_index || element->ordinal != 1 ||
        element->semantic_value != element_parameter->value ||
        element->caller_slot != element_slot || element->mode != XR_TARGET_CALL_VALUE ||
        element->ownership != XR_TARGET_CALL_CONSUME || element->flags != 0 ||
        element->array_element_storage != XR_TARGET_ARRAY_STORAGE_TAGGED)
        return false;

    XrTargetInstructionRecord group[4] = {
        {.function = function_index,
         .result_slot = receiver_slot,
         .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE, XR_TARGET_INSTRUCTION_SLOT_NONE},
         .opcode = XR_TARGET_INSTRUCTION_PARAM_DYN_BORROW,
         .immediate_bits = 0},
        {.function = function_index,
         .result_slot = element_slot,
         .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE, XR_TARGET_INSTRUCTION_SLOT_NONE},
         .opcode = XR_TARGET_INSTRUCTION_PARAM_DYN_OWNED,
         .immediate_bits = 1},
        {.function = function_index,
         .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
         .operand_slots = {receiver_slot, element_slot},
         .opcode = XR_TARGET_INSTRUCTION_ARRAY_PUSH_TAGGED,
         .operand_count = 2,
         .immediate_bits = call_index},
        {.function = function_index,
         .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
         .operand_slots = {XR_TARGET_INSTRUCTION_SLOT_NONE, XR_TARGET_INSTRUCTION_SLOT_NONE},
         .opcode = XR_TARGET_INSTRUCTION_RETURN_UNIT},
    };
    if (!xr_target_instruction_rows_control_flow_is_exact(
            group, 4, materialized->functions[function_index].slot_begin,
            materialized->functions[function_index].slot_count, materialized->calls,
            materialized->call_count, materialized->call_arguments,
            materialized->call_argument_count, materialized->entry_expectations,
            materialized->entry_expectation_count, materialized->coroutines,
            materialized->coroutine_count))
        return false;
    if (rows)
        for (uint32_t i = 0; i < 4; i++) {
            group[i].id = row_begin + i;
            rows[row_begin + i] = group[i];
        }
    if (out_row_count)
        *out_row_count = 4;
    return true;
}

static bool materialize_typed_instructions(const XrTargetPlanBuilder *builder,
                                            XrTargetMaterializedPlan *materialized, char *error,
                                            size_t error_size) {
    const XrSemanticProgramProvenance *published =
        builder && builder->semantic_plan
            ? xr_semantic_plan_program_provenance(builder->semantic_plan)
            : NULL;
    bool requires_leaf_aggregate =
        published && published->program_family ==
                         XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL;
    bool requires_leaf_product =
        published && published->program_family ==
                         XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL;
    XrLeafAggregateInstructionShape required_leaf_shape = {0};
    if (requires_leaf_aggregate &&
        !leaf_aggregate_instruction_shape_is_exact(builder, materialized,
                                                   &required_leaf_shape))
        return fail(error, error_size, "XR_TARGET_1005",
                    "leaf aggregate program requires one exact caller and callee instruction group");
    XrLeafProductInstructionShape required_product_shape = {0};
    if (requires_leaf_product &&
        !leaf_product_instruction_shape_is_exact(builder, materialized,
                                                 &required_product_shape))
        return fail(error, error_size, "XR_TARGET_1005",
                    "leaf product program requires two exact callers and one callee instruction group");
    XrScalarInstructionAnalysis analysis = {0};
    if (!scalar_instruction_analysis_init(builder, materialized, &analysis))
        return fail(error, error_size, "XR_EXEC_5003", "scalar instruction analysis failed");
    uint32_t *function_rows =
        (uint32_t *) allocate_records(materialized->function_count, sizeof(*function_rows));
    uint8_t *executable =
        (uint8_t *) allocate_records(materialized->function_count, sizeof(*executable));
    uint8_t *managed_push =
        (uint8_t *) allocate_records(materialized->function_count, sizeof(*managed_push));
    uint8_t *leaf_aggregate =
        (uint8_t *) allocate_records(materialized->function_count, sizeof(*leaf_aggregate));
    uint8_t *leaf_product =
        (uint8_t *) allocate_records(materialized->function_count, sizeof(*leaf_product));
    if (materialized->function_count &&
        (!function_rows || !executable || !managed_push || !leaf_aggregate || !leaf_product)) {
        xr_free(function_rows);
        xr_free(executable);
        xr_free(managed_push);
        xr_free(leaf_aggregate);
        xr_free(leaf_product);
        scalar_instruction_analysis_dispose(&analysis);
        return fail(error, error_size, "XR_EXEC_5003",
                    "typed instruction eligibility allocation failed");
    }
    uint32_t instruction_count = 0;
    for (uint32_t function = 0; function < materialized->function_count; function++) {
        bool required_product_function =
            requires_leaf_product &&
            (function == required_product_shape.callers[0]->semantic_function ||
             function == required_product_shape.callers[1]->semantic_function ||
             function == required_product_shape.callee->semantic_function);
        bool emitted_product = materialize_leaf_product_instruction_function(
            builder, materialized, function, NULL, 0, &function_rows[function]);
        uint32_t expected_product_rows =
            requires_leaf_product &&
                    function == required_product_shape.callee->semantic_function
                ? 14u
                : 15u;
        if (required_product_function &&
            (!emitted_product || function_rows[function] != expected_product_rows)) {
            xr_free(function_rows);
            xr_free(executable);
            xr_free(managed_push);
            xr_free(leaf_aggregate);
            xr_free(leaf_product);
            scalar_instruction_analysis_dispose(&analysis);
            return fail(error, error_size, "XR_TARGET_1005",
                        "leaf product program instruction coverage is incomplete");
        }
        if (emitted_product) {
            leaf_product[function] = 1;
            executable[function] = 1;
            continue;
        }
        bool required_leaf_function =
            requires_leaf_aggregate &&
            (function == required_leaf_shape.caller_binding->semantic_function ||
             function == required_leaf_shape.callee_binding->semantic_function);
        bool emitted_leaf = materialize_leaf_aggregate_instruction_function(
            builder, materialized, function, NULL, 0, &function_rows[function]);
        if (required_leaf_function && (!emitted_leaf || function_rows[function] != 5)) {
            xr_free(function_rows);
            xr_free(executable);
            xr_free(managed_push);
            xr_free(leaf_aggregate);
            xr_free(leaf_product);
            scalar_instruction_analysis_dispose(&analysis);
            return fail(error, error_size, "XR_TARGET_1005",
                        "leaf aggregate program instruction coverage is incomplete");
        }
        if (emitted_leaf) {
            leaf_aggregate[function] = 1;
        } else if (!materialize_scalar_instruction_function(
                       builder, materialized, function, &analysis, NULL, NULL, 0,
                       &function_rows[function])) {
            if (!materialize_tagged_array_push_instruction_function(
                    builder, materialized, function, NULL, 0, &function_rows[function]))
                continue;
            managed_push[function] = 1;
        }
        executable[function] = 1;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < materialized->call_count; i++) {
            const XrTargetCallRecord *call = &materialized->calls[i];
            if (call->semantic_operation < analysis.operation_count &&
                call->caller_function < materialized->function_count &&
                call->callee_function < materialized->function_count &&
                executable[call->caller_function] &&
                analysis.call_by_operation[call->semantic_operation] == i &&
                !executable[call->callee_function]) {
                executable[call->caller_function] = 0;
                function_rows[call->caller_function] = 0;
                changed = true;
            }
        }
    }
    if (requires_leaf_aggregate) {
        uint32_t caller = required_leaf_shape.caller_binding->semantic_function;
        uint32_t callee = required_leaf_shape.callee_binding->semantic_function;
        if (caller >= materialized->function_count || callee >= materialized->function_count ||
            !executable[caller] || !executable[callee] || !leaf_aggregate[caller] ||
            !leaf_aggregate[callee] || function_rows[caller] != 5 || function_rows[callee] != 5) {
            xr_free(function_rows);
            xr_free(executable);
            xr_free(managed_push);
            xr_free(leaf_aggregate);
            xr_free(leaf_product);
            scalar_instruction_analysis_dispose(&analysis);
            return fail(error, error_size, "XR_TARGET_1005",
                        "leaf aggregate caller and callee must survive instruction closure");
        }
    }
    if (requires_leaf_product) {
        uint32_t caller0 = required_product_shape.callers[0]->semantic_function;
        uint32_t caller1 = required_product_shape.callers[1]->semantic_function;
        uint32_t callee = required_product_shape.callee->semantic_function;
        if (caller0 >= materialized->function_count ||
            caller1 >= materialized->function_count ||
            callee >= materialized->function_count || !executable[caller0] ||
            !executable[caller1] || !executable[callee] || !leaf_product[caller0] ||
            !leaf_product[caller1] || !leaf_product[callee] ||
            function_rows[caller0] != 15 || function_rows[caller1] != 15 ||
            function_rows[callee] != 14) {
            xr_free(function_rows);
            xr_free(executable);
            xr_free(managed_push);
            xr_free(leaf_aggregate);
            xr_free(leaf_product);
            scalar_instruction_analysis_dispose(&analysis);
            return fail(error, error_size, "XR_TARGET_1005",
                        "leaf product callers and callee must survive instruction closure");
        }
    }
    for (uint32_t function = 0; function < materialized->function_count; function++) {
        if (function_rows[function] > 40000000u - instruction_count) {
            xr_free(function_rows);
            xr_free(executable);
            xr_free(managed_push);
            xr_free(leaf_aggregate);
            xr_free(leaf_product);
            scalar_instruction_analysis_dispose(&analysis);
            return fail(error, error_size, "XR_EXEC_5003", "typed instruction budget exhausted");
        }
        instruction_count += function_rows[function];
    }
    /* Eligibility probes above derive the local continuation in order to run
     * the same independent row verifier as the final build. Clear those probe
     * results before publication; only functions that survive call-closure
     * pruning may publish an executable continuation. */
    for (uint32_t state = 0; state < materialized->coroutine_count; state++)
        materialized->coroutines[state].resume_instruction = XR_SEMANTIC_INDEX_NONE;
    materialized->instruction_count = instruction_count;
    materialized->instructions = (XrTargetInstructionRecord *) allocate_records(
        instruction_count, sizeof(*materialized->instructions));
    if (instruction_count && !materialized->instructions)
        goto allocation_failed;

    uint32_t next_instruction = 0;
    for (uint32_t function = 0; function < materialized->function_count; function++) {
        if (!executable[function])
            continue;
        uint32_t emitted_rows = 0;
        bool emitted = leaf_product[function]
                           ? materialize_leaf_product_instruction_function(
                                 builder, materialized, function, materialized->instructions,
                                 next_instruction, &emitted_rows)
                       : leaf_aggregate[function]
                           ? materialize_leaf_aggregate_instruction_function(
                                 builder, materialized, function, materialized->instructions,
                                 next_instruction, &emitted_rows)
                       : managed_push[function]
                           ? materialize_tagged_array_push_instruction_function(
                                 builder, materialized, function, materialized->instructions,
                                 next_instruction, &emitted_rows)
                           : materialize_scalar_instruction_function(
                                 builder, materialized, function, &analysis, executable,
                                 materialized->instructions, next_instruction, &emitted_rows);
        if (!emitted || emitted_rows != function_rows[function]) {
            xr_free(function_rows);
            xr_free(executable);
            xr_free(managed_push);
            xr_free(leaf_aggregate);
            xr_free(leaf_product);
            scalar_instruction_analysis_dispose(&analysis);
            return fail(error, error_size, "XR_TARGET_1005",
                        "typed instruction eligibility changed during materialization");
        }
        next_instruction += emitted_rows;
    }
    xr_free(function_rows);
    xr_free(executable);
    xr_free(managed_push);
    xr_free(leaf_aggregate);
    xr_free(leaf_product);
    scalar_instruction_analysis_dispose(&analysis);
    if (next_instruction != materialized->instruction_count)
        return fail(error, error_size, "XR_TARGET_1005",
                    "typed instruction row count changed during materialization");
    return true;

allocation_failed:
    xr_free(function_rows);
    xr_free(executable);
    xr_free(managed_push);
    xr_free(leaf_aggregate);
    xr_free(leaf_product);
    scalar_instruction_analysis_dispose(&analysis);
    return fail(error, error_size, "XR_EXEC_5003", "typed instruction materialization failed");
}

static int find_rep_kind(const XrTargetMaterializedPlan *materialized, uint16_t kind) {
    for (uint32_t i = 0; i < materialized->machine_rep_count; i++)
        if (materialized->machine_reps[i].kind == kind)
            return (int) i;
    return -1;
}

static bool machine_reps_have_same_call_abi(const XrTargetMachineRepRecord *caller,
                                            const XrTargetMachineRepRecord *callee) {
    return caller && callee && caller->kind == callee->kind &&
           caller->register_bits == callee->register_bits &&
           caller->memory_size == callee->memory_size &&
           caller->memory_align == callee->memory_align &&
           caller->signedness == callee->signedness && caller->root_kind == callee->root_kind &&
           caller->null_encoding == callee->null_encoding && caller->detail == callee->detail &&
           caller->lane_count == callee->lane_count && caller->reserved == callee->reserved &&
           memcmp(caller->legal_conversion_mask, callee->legal_conversion_mask,
                  sizeof(caller->legal_conversion_mask)) == 0;
}

/* A reference-capable container handed over by value.
 *
 * The callee always borrows it: the allocation stays the caller's for the
 * extent of the call and the callee releases nothing. What the caller holds is
 * its own business -- a freshly built container is owned, a shared read of a
 * local is borrowed -- so the two sides agree on representation and are allowed
 * to differ in ownership alone. An Array and a String reach this boundary in
 * the same tagged carrier, so they ask this one question instead of stating the
 * same rep agreement twice in spellings that could drift. */
/* Both sides of a by-value container boundary hold the same tagged carrier, so
 * they must agree on representation and on call ABI. They need not agree on
 * ownership: what the caller holds is its own business -- a freshly built array
 * or a fresh concatenation is owned, a shared read of a local is borrowed -- and
 * `callee_ownership` states the one side the boundary does fix. */
static bool tagged_container_value_boundary(const XrTargetMaterializedPlan *materialized,
                                            const XrTargetValueRepRecord *caller,
                                            const XrTargetValueRepRecord *callee,
                                            uint8_t callee_ownership) {
    return materialized && caller && callee &&
           caller->register_rep < materialized->machine_rep_count &&
           caller->memory_rep < materialized->machine_rep_count &&
           callee->register_rep < materialized->machine_rep_count &&
           callee->memory_rep < materialized->machine_rep_count &&
           machine_reps_have_same_call_abi(&materialized->machine_reps[caller->register_rep],
                                           &materialized->machine_reps[callee->register_rep]) &&
           machine_reps_have_same_call_abi(&materialized->machine_reps[caller->memory_rep],
                                           &materialized->machine_reps[callee->memory_rep]) &&
           materialized->machine_reps[caller->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
           materialized->machine_reps[caller->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
           materialized->machine_reps[callee->register_rep].ownership == callee_ownership &&
           materialized->machine_reps[callee->memory_rep].ownership == callee_ownership;
}

static bool materialize_calls_and_adapters(const XrTargetPlanBuilder *builder,
                                           XrTargetMaterializedPlan *materialized, char *error,
                                           size_t error_size) {
    materialized->call_count = builder->call_intent_count;
    materialized->call_argument_count = builder->call_argument_intent_count;
    materialized->adapter_count = 0;
    materialized->calls = (XrTargetCallRecord *) allocate_records(materialized->call_count,
                                                                  sizeof(*materialized->calls));
    materialized->call_arguments = (XrTargetCallArgumentRecord *) allocate_records(
        materialized->call_argument_count, sizeof(*materialized->call_arguments));
    if ((materialized->call_count && !materialized->calls) ||
        (materialized->call_argument_count && !materialized->call_arguments))
        return fail(error, error_size, "XR_EXEC_5003", "call materialization failed");
    int void_rep = find_rep_kind(materialized, XR_MACHINE_REP_VOID);
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(builder->profile);
    if (void_rep < 0 || !machine)
        return fail(error, error_size, "XR_TARGET_1003",
                    "call error-channel representation is missing");
    uint32_t next_argument = 0;
    for (uint32_t i = 0; i < materialized->call_count; i++) {
        const XrTargetCallIntent *intent = &builder->call_intents[i];
        const XrTargetValueRepRecord *result =
            find_materialized_value(materialized, intent->result_value);
        if (!result || intent->argument_begin != next_argument ||
            intent->argument_count > materialized->call_argument_count - next_argument)
            return fail(error, error_size, "XR_TARGET_1003",
                        "call result or argument partition cannot bind canonical storage");
        XrTargetCallRecord *call = &materialized->calls[i];
        bool result_is_void =
            result && result->memory_rep < materialized->machine_rep_count &&
            materialized->machine_reps[result->memory_rep].kind == XR_MACHINE_REP_VOID;
        bool caller_storage_result = intent->result_mode == XR_TARGET_CALL_CALLER_STORAGE;
        if (caller_storage_result &&
            (result_is_void || result->slot == XR_SEMANTIC_INDEX_NONE || intent->suspends ||
             intent->result_ownership != XR_TARGET_CALL_NONE))
            return fail(error, error_size, "XR_TARGET_1003",
                        "caller-storage call result is not exact");
        if (intent->suspends && !result_is_void && result->slot == XR_SEMANTIC_INDEX_NONE)
            return fail(error, error_size, "XR_TARGET_1003",
                        "suspending call result lacks exact caller storage");
        *call = (XrTargetCallRecord) {
            .identity = intent->identity,
            .id = i,
            .semantic_call_target = intent->semantic_call_target,
            .semantic_operation = intent->semantic_operation,
            .caller_function = intent->caller_function,
            .callee_function = intent->callee_function,
            .source_dependency = intent->source_dependency,
            .source_export = intent->source_export,
            .source_export_identity = intent->source_export_identity,
            .source_callee_identity = intent->source_callee_identity,
            .result_value = intent->result_value,
            .result_slot = result->slot,
            .caller_storage_slot = caller_storage_result ? result->slot : XR_SEMANTIC_INDEX_NONE,
            .error_slot = XR_SEMANTIC_INDEX_NONE,
            .argument_begin = next_argument,
            .adapter_begin = 0,
            .result_register_rep = result->register_rep,
            .result_memory_rep = result->memory_rep,
            .error_register_rep = (uint16_t) void_rep,
            .error_memory_rep = (uint16_t) void_rep,
            .argument_count = intent->argument_count,
            .adapter_count = 0,
            .native_abi = machine->native_abi,
            .flags = (intent->suspends ? XR_TARGET_CALL_SUSPEND : 0) |
                     (intent->tail ? XR_TARGET_CALL_TAIL : 0),
            .calling_convention = intent->calling_convention,
            .target_kind = intent->target_kind,
            .result_mode = intent->result_mode,
            .result_ownership = intent->result_ownership,
            .error_mode = XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL,
            .array_intrinsic_kind = intent->array_intrinsic_kind,
            .array_element_storage = intent->array_element_storage,
            .array_hof_kind = intent->array_hof_kind,
            .array_result_element_storage = intent->array_result_element_storage,
        };
        for (uint32_t ordinal = 0; ordinal < intent->argument_count; ordinal++) {
            const XrTargetCallArgumentIntent *argument_intent =
                &builder->call_argument_intents[next_argument];
            bool array_intrinsic =
                intent->calling_convention == XR_TARGET_CALL_CONVENTION_ARRAY_INTRINSIC &&
                intent->target_kind == XR_TARGET_CALL_TARGET_ARRAY_INTRINSIC;
            bool external_source_callee =
                intent->target_kind == XR_TARGET_CALL_TARGET_SOURCE_EXPORT ||
                (intent->target_kind == XR_TARGET_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR &&
                 intent->source_dependency != XR_SEMANTIC_INDEX_NONE);
            const XrSemanticPlan *callee_semantic =
                external_source_callee &&
                        intent->source_dependency < builder->semantic_dependency_count
                    ? builder->semantic_dependencies[intent->source_dependency]
                    : builder->semantic_plan;
            bool array_fill =
                intent->calling_convention == XR_TARGET_CALL_CONVENTION_ARRAY_FILL_SCALAR &&
                intent->target_kind == XR_TARGET_CALL_TARGET_ARRAY_FILL_SCALAR;
            bool array_hof = intent->calling_convention == XR_TARGET_CALL_CONVENTION_ARRAY_HOF &&
                             intent->target_kind == XR_TARGET_CALL_TARGET_ARRAY_HOF;
            bool iterator_rune_nth =
                intent->calling_convention == XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_NTH &&
                intent->target_kind == XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NTH;
            bool array_member_tagged_store =
                intent->calling_convention == XR_TARGET_CALL_CONVENTION_ARRAY_MEMBER_SCALAR &&
                intent->target_kind == XR_TARGET_CALL_TARGET_ARRAY_MEMBER_SCALAR &&
                intent->array_element_storage == XR_TARGET_ARRAY_STORAGE_TAGGED &&
                (intent->argument_count == 2 || intent->argument_count == 4);
            const XrSemanticParameterRecord *parameter =
                array_intrinsic || array_fill || array_hof || iterator_rune_nth ||
                        array_member_tagged_store
                    ? NULL
                    : xr_semantic_plan_parameter(callee_semantic,
                                                 argument_intent->callee_parameter);
            const XrTargetValueRepRecord *caller =
                find_materialized_value(materialized, argument_intent->caller_storage_value);
            const XrTargetValueRepRecord *callee =
                !external_source_callee && parameter
                    ? find_materialized_value(materialized, parameter->value)
                    : NULL;
            const XrSemanticTypeRecord *parameter_type =
                parameter ? xr_semantic_plan_type(callee_semantic, parameter->type) : NULL;
            bool adt_enum_borrow_boundary =
                xr_semantic_adt_enum_type_is_exact(parameter_type) && caller && callee &&
                caller->register_rep < materialized->machine_rep_count &&
                caller->memory_rep < materialized->machine_rep_count &&
                callee->register_rep < materialized->machine_rep_count &&
                callee->memory_rep < materialized->machine_rep_count &&
                machine_reps_have_same_call_abi(
                    &materialized->machine_reps[caller->register_rep],
                    &materialized->machine_reps[callee->register_rep]) &&
                machine_reps_have_same_call_abi(&materialized->machine_reps[caller->memory_rep],
                                                &materialized->machine_reps[callee->memory_rep]) &&
                materialized->machine_reps[caller->register_rep].ownership ==
                    XR_TARGET_OWNERSHIP_OWNED &&
                materialized->machine_reps[callee->register_rep].ownership ==
                    XR_TARGET_OWNERSHIP_BORROWED &&
                materialized->machine_reps[caller->memory_rep].ownership ==
                    XR_TARGET_OWNERSHIP_OWNED &&
                materialized->machine_reps[callee->memory_rep].ownership ==
                    XR_TARGET_OWNERSHIP_BORROWED;
            uint8_t exact_tagged_storage = XR_TARGET_ARRAY_STORAGE_NONE;
            bool tagged_ref_borrow_boundary =
                parameter &&
                semantic_direct_local_tagged_ref_parameter_is_exact(
                    builder->semantic_plan, parameter, &exact_tagged_storage) &&
                argument_intent->array_element_storage == exact_tagged_storage &&
                argument_intent->mode == XR_TARGET_CALL_REFERENCE &&
                argument_intent->ownership == XR_TARGET_CALL_BORROW &&
                argument_intent->transfer_mode == XR_TRANSFER_SHARE &&
                argument_intent->flags == XR_TARGET_CALL_ARGUMENT_ADDRESSABLE && caller && callee &&
                caller->register_rep < materialized->machine_rep_count &&
                caller->memory_rep < materialized->machine_rep_count &&
                callee->register_rep < materialized->machine_rep_count &&
                callee->memory_rep < materialized->machine_rep_count &&
                materialized->machine_reps[caller->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
                materialized->machine_reps[caller->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
                materialized->machine_reps[callee->register_rep].kind == XR_MACHINE_REP_RAW_PTR &&
                materialized->machine_reps[callee->memory_rep].kind == XR_MACHINE_REP_RAW_PTR &&
                materialized->machine_reps[caller->register_rep].ownership ==
                    materialized->machine_reps[caller->memory_rep].ownership &&
                (materialized->machine_reps[caller->register_rep].ownership ==
                     XR_TARGET_OWNERSHIP_OWNED ||
                 materialized->machine_reps[caller->register_rep].ownership ==
                     XR_TARGET_OWNERSHIP_BORROWED) &&
                materialized->machine_reps[callee->register_rep].ownership ==
                    XR_TARGET_OWNERSHIP_BORROWED &&
                materialized->machine_reps[callee->memory_rep].ownership ==
                    XR_TARGET_OWNERSHIP_BORROWED;
            /* An Array or a String handed over by value. Both are
             * reference-capable containers whose one storage fact is the tagged
             * outer value, so both cross this boundary as a plain argument the
             * callee borrows, and the rep agreement they need is the same one. */
            uint8_t exact_array_value_storage = XR_TARGET_ARRAY_STORAGE_NONE;
            bool string_callee_owns = false;
            bool container_value_argument =
                parameter && argument_intent->mode == XR_TARGET_CALL_VALUE &&
                argument_intent->transfer_mode == XR_TRANSFER_SHARE &&
                argument_intent->flags == 0 &&
                argument_intent->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
                (semantic_direct_local_array_value_parameter_is_exact(
                     builder->semantic_plan, parameter, &exact_array_value_storage) ||
                 xr_semantic_direct_local_string_value_parameter_is_exact(
                     builder->semantic_plan, parameter, &string_callee_owns));
            bool container_value_borrow_boundary =
                container_value_argument &&
                tagged_container_value_boundary(materialized, caller, callee,
                                                string_callee_owns ? XR_TARGET_OWNERSHIP_OWNED
                                                                   : XR_TARGET_OWNERSHIP_BORROWED);
            /* A class instance crosses in the ownership its parameter states.
             * Borrowing and consuming calls use the same tagged carrier; only
             * the callee-side ownership changes. */
            bool class_instance_boundary =
                parameter &&
                xr_semantic_class_instance_parameter_source_class(
                    callee_semantic, argument_intent->callee_parameter) != XR_SEMANTIC_INDEX_NONE &&
                tagged_container_value_boundary(materialized, caller, callee,
                                                parameter->ownership == XI_OWN_OWNED
                                                    ? XR_TARGET_OWNERSHIP_OWNED
                                                    : XR_TARGET_OWNERSHIP_BORROWED);
            if (array_intrinsic || array_fill || array_hof || iterator_rune_nth ||
                array_member_tagged_store) {
                if (argument_intent->call_intent != i || argument_intent->ordinal != ordinal ||
                    !caller || argument_intent->callee_parameter != XR_SEMANTIC_INDEX_NONE)
                    return fail(error, error_size, "XR_TARGET_1003",
                                iterator_rune_nth
                                    ? "Iterator<rune>.nth index lacks exact caller storage"
                                : array_member_tagged_store
                                    ? "Array member tagged store lacks exact caller storage"
                                    : "Array intrinsic argument lacks exact caller storage");
                materialized->call_arguments[next_argument] = (XrTargetCallArgumentRecord) {
                    .identity = argument_intent->identity,
                    .call = i,
                    .semantic_operand = argument_intent->semantic_operand,
                    .semantic_value = argument_intent->semantic_value,
                    .callee_parameter = XR_SEMANTIC_INDEX_NONE,
                    .caller_slot = caller->slot,
                    .callee_slot = XR_SEMANTIC_INDEX_NONE,
                    .register_rep = caller->register_rep,
                    .memory_rep = caller->memory_rep,
                    .callee_register_rep = caller->register_rep,
                    .callee_memory_rep = caller->memory_rep,
                    .ordinal = argument_intent->ordinal,
                    .mode = argument_intent->mode,
                    .ownership = argument_intent->ownership,
                    .transfer_mode = argument_intent->transfer_mode,
                    .flags = argument_intent->flags,
                    .array_element_storage = argument_intent->array_element_storage,
                };
                next_argument++;
                continue;
            }
            if (argument_intent->call_intent != i || argument_intent->ordinal != ordinal ||
                !parameter || !caller || caller->slot == XR_SEMANTIC_INDEX_NONE ||
                (!external_source_callee &&
                 (!callee || callee->slot == XR_SEMANTIC_INDEX_NONE ||
                  ((caller->register_rep != callee->register_rep ||
                    caller->memory_rep != callee->memory_rep) &&
                   !adt_enum_borrow_boundary && !tagged_ref_borrow_boundary &&
                   !container_value_borrow_boundary && !class_instance_boundary)))) {
                if (target_trace_enabled()) {
                    fprintf(stderr,
                            "[target] refused in call argument materialization: argument %u of "
                            "call intent %u has no storage both sides agree on\n",
                            ordinal, i);
                    target_trace_judgement("the callee parameter record exists", parameter != NULL);
                    target_trace_judgement("the caller value has a bound slot",
                                           caller && caller->slot != XR_SEMANTIC_INDEX_NONE);
                    target_trace_judgement("the callee value has a bound slot",
                                           external_source_callee ||
                                               (callee && callee->slot != XR_SEMANTIC_INDEX_NONE));
                    if (caller && callee) {
                        target_trace_equality("caller register rep vs callee register rep",
                                              callee->register_rep, caller->register_rep);
                        target_trace_equality("caller memory rep vs callee memory rep",
                                              callee->memory_rep, caller->memory_rep);
                    }
                    fprintf(stderr,
                            "[target]   reps that differ are admitted only by a named boundary, "
                            "and each was asked in turn:\n");
                    target_trace_judgement("ADT enum borrow boundary", adt_enum_borrow_boundary);
                    target_trace_judgement("tagged reference borrow boundary",
                                           tagged_ref_borrow_boundary);
                    target_trace_judgement("tagged container by value boundary",
                                           container_value_borrow_boundary);
                    target_trace_judgement("class instance ownership boundary",
                                           class_instance_boundary);
                    fprintf(stderr,
                            "[target]   read it as: matching reps need no boundary at all. A "
                            "boundary is what lets the two sides hold the same carrier under "
                            "different ownership, so a \"NO\" on all three with differing reps "
                            "means no family described this hand-over.\n");
                }
                return fail(error, error_size, "XR_TARGET_1003",
                            "call argument lacks exact caller/callee storage");
            }
            materialized->call_arguments[next_argument] = (XrTargetCallArgumentRecord) {
                .identity = argument_intent->identity,
                .call = i,
                .semantic_operand = argument_intent->semantic_operand,
                .semantic_value = argument_intent->semantic_value,
                .callee_parameter = argument_intent->callee_parameter,
                .caller_slot = caller->slot,
                .callee_slot = external_source_callee ? XR_SEMANTIC_INDEX_NONE : callee->slot,
                .register_rep = caller->register_rep,
                .memory_rep = caller->memory_rep,
                .callee_register_rep =
                    external_source_callee ? caller->register_rep : callee->register_rep,
                .callee_memory_rep =
                    external_source_callee ? caller->memory_rep : callee->memory_rep,
                .ordinal = argument_intent->ordinal,
                .mode = argument_intent->mode,
                .ownership = argument_intent->ownership,
                .transfer_mode = argument_intent->transfer_mode,
                .flags = argument_intent->flags,
                .array_element_storage = argument_intent->array_element_storage,
            };
            next_argument++;
        }
    }
    if (next_argument != materialized->call_argument_count)
        return fail(error, error_size, "XR_TARGET_1003",
                    "call arguments do not exactly partition their table");
    return true;
}

static bool reconstruct_coroutine_resume(const XrSemanticPlan *semantic, uint32_t operation_index,
                                         const uint32_t *edge_by_block, const uint8_t *edge_counts,
                                         uint32_t block_count, uint32_t *suspend_block,
                                         uint32_t *resume_block, uint16_t *predecessor_ordinal) {
    uint32_t predecessor_count = 0;
    const uint32_t *predecessors = xr_semantic_plan_predecessors(semantic, &predecessor_count);
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(semantic, operation_index);
    const XrSemanticBlockRecord *suspend =
        operation ? xr_semantic_plan_block(semantic, operation->block) : NULL;
    if (!operation || operation->block >= block_count || !suspend ||
        suspend->function != operation->function || suspend->operation_begin != operation_index ||
        suspend->operation_count != 1 || suspend->predecessor_count != 1 ||
        suspend->predecessor_begin >= predecessor_count ||
        suspend->successors[0] == XR_SEMANTIC_INDEX_NONE ||
        (suspend->successors[1] != XR_SEMANTIC_INDEX_NONE &&
         suspend->successors[1] != suspend->successors[0]))
        return false;
    const XrSemanticBlockRecord *before =
        xr_semantic_plan_block(semantic, predecessors[suspend->predecessor_begin]);
    const XrSemanticBlockRecord *resume = xr_semantic_plan_block(semantic, suspend->successors[0]);
    if (!before || !resume || before->function != operation->function ||
        resume->function != operation->function ||
        (before->successors[0] != operation->block && before->successors[1] != operation->block) ||
        resume->predecessor_count != 1 || resume->predecessor_begin >= predecessor_count ||
        predecessors[resume->predecessor_begin] != operation->block ||
        edge_counts[operation->block] != 1)
        return false;
    const XrSemanticEdgeRecord *edge =
        xr_semantic_plan_edge(semantic, edge_by_block[operation->block]);
    if (!edge || edge->function != operation->function || edge->from_block != operation->block ||
        edge->to_block != suspend->successors[0] || edge->operation != XR_SEMANTIC_INDEX_NONE ||
        edge->kind != XR_SEM_EDGE_NORMAL || edge->flags != 0)
        return false;
    *suspend_block = operation->block;
    *resume_block = suspend->successors[0];
    *predecessor_ordinal = 0;
    return true;
}

static bool materialize_coroutine_state_calls(const XrTargetPlanBuilder *builder,
                                              XrTargetMaterializedPlan *materialized, char *error,
                                              size_t error_size) {
    const XrSemanticPlan *semantic = builder->semantic_plan;
    uint32_t function_count = materialized->function_count;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    uint32_t entity_count = (uint32_t) xr_semantic_plan_entity_count(semantic);
    uint32_t block_count = (uint32_t) xr_semantic_plan_block_count(semantic);
    uint32_t *call_by_operation =
        operation_count ? (uint32_t *) allocate_records(operation_count, sizeof(*call_by_operation))
                        : NULL;
    uint32_t *edge_by_block =
        block_count ? (uint32_t *) allocate_records(block_count, sizeof(*edge_by_block)) : NULL;
    uint8_t *edge_counts =
        block_count ? (uint8_t *) allocate_records(block_count, sizeof(*edge_counts)) : NULL;
    if ((operation_count && !call_by_operation) ||
        (block_count && (!edge_by_block || !edge_counts))) {
        xr_free(call_by_operation);
        xr_free(edge_by_block);
        xr_free(edge_counts);
        return fail(error, error_size, "XR_EXEC_5003", "coroutine call index allocation failed");
    }
    for (uint32_t operation = 0; operation < operation_count; operation++)
        call_by_operation[operation] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t block = 0; block < block_count; block++)
        edge_by_block[block] = XR_SEMANTIC_INDEX_NONE;
    bool valid = true;
    uint32_t edge_count = (uint32_t) xr_semantic_plan_edge_count(semantic);
    for (uint32_t edge_index = 0; edge_index < edge_count; edge_index++) {
        const XrSemanticEdgeRecord *edge = xr_semantic_plan_edge(semantic, edge_index);
        if (!edge || edge->from_block >= block_count) {
            valid = false;
            break;
        }
        if (edge_counts[edge->from_block] == 0)
            edge_by_block[edge->from_block] = edge_index;
        if (edge_counts[edge->from_block] < 2)
            edge_counts[edge->from_block]++;
    }
    for (uint32_t call = 0; call < materialized->call_count; call++) {
        uint32_t operation = materialized->calls[call].semantic_operation;
        if (operation >= operation_count ||
            call_by_operation[operation] != XR_SEMANTIC_INDEX_NONE) {
            valid = false;
            break;
        }
        call_by_operation[operation] = call;
    }
    uint32_t state_count = 0;
    for (uint32_t entity_index = 0; valid && entity_index < entity_count; entity_index++) {
        const XrSemanticEntityRecord *entity = xr_semantic_plan_entity(semantic, entity_index);
        if (!entity || entity->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, entity->subject);
        if (entity->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION || !operation ||
            operation->function >= function_count ||
            materialized->functions[operation->function].coroutine_count == UINT32_MAX) {
            valid = false;
            break;
        }
        materialized->functions[operation->function].coroutine_count++;
        state_count++;
    }
    if (!valid || state_count > 10000000u) {
        xr_free(call_by_operation);
        xr_free(edge_by_block);
        xr_free(edge_counts);
        return fail(error, error_size, "XR_CORO_4000", "coroutine state-call coverage is invalid");
    }
    materialized->coroutine_count = state_count;
    materialized->coroutines = (XrTargetCoroutineStateRecord *) allocate_records(
        state_count, sizeof(*materialized->coroutines));
    if (state_count && !materialized->coroutines) {
        xr_free(call_by_operation);
        xr_free(edge_by_block);
        xr_free(edge_counts);
        return fail(error, error_size, "XR_EXEC_5003",
                    "coroutine state-call materialization failed");
    }
    uint32_t cursor = 0;
    for (uint32_t function = 0; function < function_count; function++) {
        XrTargetFunctionRecord *record = &materialized->functions[function];
        record->coroutine_begin = cursor;
        if (record->coroutine_count > state_count - cursor) {
            valid = false;
            break;
        }
        cursor += record->coroutine_count;
    }
    if (cursor != state_count)
        valid = false;
    for (uint32_t i = 0; i < state_count; i++)
        materialized->coroutines[i].semantic_entity = XR_SEMANTIC_INDEX_NONE;

    for (uint32_t entity_index = 0; valid && entity_index < entity_count; entity_index++) {
        const XrSemanticEntityRecord *entity = xr_semantic_plan_entity(semantic, entity_index);
        if (!entity || entity->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, entity->subject);
        XrTargetFunctionRecord *function = operation && operation->function < function_count
                                               ? &materialized->functions[operation->function]
                                               : NULL;
        if (!function || entity->ordinal == 0 || entity->ordinal > function->coroutine_count) {
            valid = false;
            break;
        }
        uint32_t state_index = function->coroutine_begin + entity->ordinal - 1u;
        XrTargetCoroutineStateRecord *state = &materialized->coroutines[state_index];
        uint32_t suspend_block = XR_SEMANTIC_INDEX_NONE;
        uint32_t resume_block = XR_SEMANTIC_INDEX_NONE;
        uint16_t predecessor_ordinal = UINT16_MAX;
        if (state->semantic_entity != XR_SEMANTIC_INDEX_NONE ||
            !reconstruct_coroutine_resume(semantic, entity->subject, edge_by_block, edge_counts,
                                          block_count, &suspend_block, &resume_block,
                                          &predecessor_ordinal)) {
            valid = false;
            break;
        }
        uint32_t direct_call = call_by_operation[entity->subject];
        uint32_t result_slot = XR_SEMANTIC_INDEX_NONE;
        uint16_t flags = 0;
        const XrTargetValueRepRecord *result =
            find_materialized_value(materialized, operation->result_value);
        if (result) {
            if (result->memory_rep >= materialized->machine_rep_count) {
                valid = false;
                break;
            }
            if (materialized->machine_reps[result->memory_rep].kind != XR_MACHINE_REP_VOID) {
                if (result->slot >= materialized->slot_count ||
                    materialized->slots[result->slot].function != operation->function ||
                    materialized->slots[result->slot].semantic_value != operation->result_value) {
                    valid = false;
                    break;
                }
                result_slot = result->slot;
                flags |= XR_TARGET_COROUTINE_RESULT_SLOT_BOUND;
            }
        }
        if (direct_call != XR_SEMANTIC_INDEX_NONE) {
            bool source_call =
                direct_call < materialized->call_count &&
                materialized->calls[direct_call].target_kind == XR_TARGET_CALL_TARGET_SOURCE_EXPORT;
            bool native_namespace_call = direct_call < materialized->call_count &&
                                         materialized->calls[direct_call].target_kind ==
                                             XR_TARGET_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE;
            if (direct_call >= materialized->call_count ||
                ((source_call || native_namespace_call) ? operation->opcode != XI_CALL_METHOD
                                                        : operation->opcode != XI_CALL) ||
                (materialized->calls[direct_call].flags & XR_TARGET_CALL_SUSPEND) == 0 ||
                materialized->calls[direct_call].result_slot != result_slot ||
                materialized->calls[direct_call].caller_storage_slot != XR_SEMANTIC_INDEX_NONE) {
                valid = false;
                break;
            }
            flags |= XR_TARGET_COROUTINE_DIRECT_CHILD;
            if (source_call)
                flags |= XR_TARGET_COROUTINE_SOURCE_CHILD;
        }
        *state = (XrTargetCoroutineStateRecord) {
            .id = state_index,
            .function = operation->function,
            .semantic_entity = entity_index,
            .semantic_operation = entity->subject,
            .logical_state = entity->ordinal,
            .suspend_block = suspend_block,
            .resume_block = resume_block,
            .resume_predecessor = suspend_block,
            .resume_instruction = XR_SEMANTIC_INDEX_NONE,
            .direct_call = direct_call,
            .result_slot = result_slot,
            .resume_predecessor_ordinal = predecessor_ordinal,
            .flags = flags,
        };
    }
    for (uint32_t i = 0; valid && i < state_count; i++)
        valid = materialized->coroutines[i].semantic_entity != XR_SEMANTIC_INDEX_NONE;
    xr_free(call_by_operation);
    xr_free(edge_by_block);
    xr_free(edge_counts);
    return valid ||
           fail(error, error_size, "XR_CORO_4000", "coroutine state-call facts are not canonical");
}

static const XrSemanticEntityRecord *find_semantic_entity(const XrSemanticPlan *semantic,
                                                          uint16_t kind, uint8_t subject_kind,
                                                          uint32_t subject) {
    const XrSemanticEntityRecord *match = NULL;
    uint32_t count = (uint32_t) xr_semantic_plan_entity_count(semantic);
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticEntityRecord *candidate = xr_semantic_plan_entity(semantic, i);
        if (!candidate || candidate->kind != kind || candidate->subject_kind != subject_kind ||
            candidate->subject != subject)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

static uint32_t debug_fact_operation_for_instruction(const XrTargetMaterializedPlan *materialized,
                                                     const XrTargetInstructionRecord *instruction) {
    if (!materialized || !instruction)
        return XR_SEMANTIC_INDEX_NONE;
    if (instruction->result_slot < materialized->slot_count) {
        const XrTargetSlotRecord *slot = &materialized->slots[instruction->result_slot];
        if (slot->id == instruction->result_slot && slot->function == instruction->function &&
            slot->semantic_operation != XR_SEMANTIC_INDEX_NONE)
            return slot->semantic_operation;
    }
    if ((instruction->opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 ||
         instruction->opcode == XR_TARGET_INSTRUCTION_CALL_ENTRY_I64) &&
        instruction->immediate_bits <= UINT32_MAX) {
        uint32_t call =
            instruction->opcode == XR_TARGET_INSTRUCTION_CALL_ENTRY_I64
                ? (instruction->immediate_bits < materialized->entry_expectation_count
                       ? materialized->entry_expectations[(uint32_t) instruction->immediate_bits]
                             .call
                       : XR_SEMANTIC_INDEX_NONE)
                : (uint32_t) instruction->immediate_bits;
        if (call < materialized->call_count && materialized->calls[call].id == call &&
            materialized->calls[call].caller_function == instruction->function)
            return materialized->calls[call].semantic_operation;
    }
    if (instruction->operand_slots[0] < materialized->slot_count) {
        const XrTargetSlotRecord *slot = &materialized->slots[instruction->operand_slots[0]];
        if (slot->id == instruction->operand_slots[0] && slot->function == instruction->function)
            return slot->semantic_operation;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static bool materialize_debug_facts(const XrTargetPlanBuilder *builder,
                                    XrTargetMaterializedPlan *materialized, char *error,
                                    size_t error_size) {
    const XrSemanticPlan *semantic = builder->semantic_plan;
    const XrOwnershipCertificate *ownership = xr_semantic_plan_ownership(semantic);
    const XrFingerprint zero_fingerprint = {{0}};
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    materialized->debug_fact_count = materialized->instruction_count;
    materialized->debug_facts = (XrTargetDebugFactRecord *) allocate_records(
        materialized->debug_fact_count, sizeof(*materialized->debug_facts));
    if (materialized->debug_fact_count && !materialized->debug_facts)
        return fail(error, error_size, "XR_EXEC_5003", "target debug fact materialization failed");
    for (uint32_t i = 0; i < materialized->debug_fact_count; i++) {
        const XrTargetInstructionRecord *instruction = &materialized->instructions[i];
        XrTargetDebugFactRecord *fact = &materialized->debug_facts[i];
        uint32_t operation = debug_fact_operation_for_instruction(materialized, instruction);
        if (instruction->id != i || instruction->function >= materialized->function_count)
            return fail(error, error_size, "XR_TARGET_1005",
                        "target debug fact instruction relation is invalid");
        *fact = (XrTargetDebugFactRecord) {
            .id = i,
            .instruction = i,
            .function = instruction->function,
            .semantic_operation = XR_SEMANTIC_INDEX_NONE,
            .coroutine_state = XR_SEMANTIC_INDEX_NONE,
        };
        if (operation == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (operation >= operation_count)
            return fail(error, error_size, "XR_TARGET_1005",
                        "target debug fact semantic operation is out of range");
        const XrSemanticOperationRecord *semantic_operation =
            xr_semantic_plan_operation(semantic, operation);
        if (!semantic_operation || semantic_operation->function != instruction->function)
            return fail(error, error_size, "XR_TARGET_1005",
                        "target debug fact semantic operation is invalid");
        fact->semantic_operation = operation;
        fact->semantic_operation_identity = semantic_operation->id;
        if (semantic_operation->source_file) {
            const XrSemanticEntityRecord *span = find_semantic_entity(
                semantic, XR_SEM_ENTITY_DEBUG_SPAN, XR_SEM_ENTITY_SUBJECT_OPERATION, operation);
            if (!span)
                return fail(error, error_size, "XR_TARGET_1005",
                            "target debug source span identity is invalid");
            fact->source_start_line = semantic_operation->source_start_line;
            fact->source_start_column = semantic_operation->source_start_column;
            fact->source_end_line = semantic_operation->source_end_line;
            fact->source_end_column = semantic_operation->source_end_column;
            fact->source_span_identity = span->id;
        }
        for (uint32_t state = 0; state < materialized->coroutine_count; state++) {
            const XrTargetCoroutineStateRecord *candidate = &materialized->coroutines[state];
            if (candidate->semantic_operation != operation)
                continue;
            const XrSemanticEntityRecord *entity =
                xr_semantic_plan_entity(semantic, candidate->semantic_entity);
            if (fact->coroutine_state != XR_SEMANTIC_INDEX_NONE || !entity ||
                entity->kind != XR_SEM_ENTITY_COROUTINE_STATE || entity->subject != operation)
                return fail(error, error_size, "XR_TARGET_1005",
                            "target debug coroutine identity is invalid");
            fact->coroutine_state = candidate->id;
            fact->coroutine_state_identity = entity->id;
        }
        for (uint32_t layout = 0; layout < materialized->layout_count; layout++) {
            const XrTargetLayoutRecord *candidate = &materialized->layouts[layout];
            if (candidate->semantic_type != semantic_operation->result_type)
                continue;
            if (!xr_fingerprint_equal(fact->layout_fingerprint, zero_fingerprint))
                return fail(error, error_size, "XR_TARGET_1005",
                            "target debug layout relation is ambiguous");
            fact->layout_fingerprint = candidate->fingerprint;
        }
        if (ownership && semantic_operation->result_value != XR_SEMANTIC_INDEX_NONE) {
            for (uint32_t owner = 0; owner < xr_ownership_certificate_owner_count(ownership);
                 owner++) {
                const XrOwnershipOwnerRecord *record =
                    xr_ownership_certificate_owner(ownership, owner);
                if (!record)
                    return fail(error, error_size, "XR_TARGET_1005",
                                "target debug ownership certificate is invalid");
                if (record->function != instruction->function ||
                    record->origin_value != semantic_operation->result_value)
                    continue;
                const XrSemanticEntityRecord *entity = find_semantic_entity(
                    semantic, XR_SEM_ENTITY_OWNER, XR_SEM_ENTITY_SUBJECT_OWNER, owner);
                if (!stable_id_is_zero(fact->owner_identity) || !entity)
                    return fail(error, error_size, "XR_TARGET_1005",
                                "target debug owner identity is invalid");
                fact->owner_identity = entity->id;
            }
        }
    }
    return true;
}

static bool materialize_capabilities(const XrTargetPlanBuilder *builder,
                                     XrTargetMaterializedPlan *materialized, char *error,
                                     size_t error_size) {
    const XrTargetProfileDraft *facts = xr_target_profile_facts(builder->profile);
    if (!facts || (facts->provider_mask & XR_TARGET_FOUNDATION_CAPABILITY_MASK) !=
                      XR_TARGET_FOUNDATION_CAPABILITY_MASK)
        return fail(error, error_size, "XR_TARGET_1004",
                    "target profile lacks a foundation capability provider");
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(builder->profile);
    if (!machine)
        return fail(error, error_size, "XR_TARGET_1004",
                    "target profile has no exact runtime identity");
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(builder->semantic_plan, &operand_count);
    uint32_t assertion_requirements = XR_ASSERTION_CAPABILITY_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(builder->semantic_plan); i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_ASSERTION)
            continue;
        XrAssertionPlan assertion;
        if (!xr_semantic_operation_assertion_plan(operation, &assertion))
            return fail(error, error_size, "XR_TARGET_1004",
                        "assertion capability requirement is not exact");
        if (machine->runtime_profile == XR_TARGET_RUNTIME_PROFILE_FREESTANDING &&
            assertion.kind == XR_ASSERTION_KIND_EQUAL) {
            if (!operands || operation->operand_count < 2 || operand_count < 2 ||
                operation->operand_begin > operand_count - 2u)
                return fail(error, error_size, "XR_TARGET_1004",
                            "freestanding assertion equality operands are missing");
            const XrSemanticTypeRecord *left = xr_semantic_plan_type(
                builder->semantic_plan, operands[operation->operand_begin].type);
            const XrSemanticTypeRecord *right = xr_semantic_plan_type(
                builder->semantic_plan, operands[operation->operand_begin + 1u].type);
            if (!xr_target_freestanding_assertion_equality_type_supported(left) ||
                !xr_target_freestanding_assertion_equality_type_supported(right) ||
                left->kind != right->kind || left->scalar_rep != right->scalar_rep ||
                left->enum_layout_id != right->enum_layout_id)
                return fail(
                    error, error_size, "XR_TARGET_1004",
                    "freestanding assertion equality type has no exact renderer/equality adapter");
        }
        assertion_requirements |= assertion.required_capabilities;
    }
    uint32_t assertion_count =
        ((machine->runtime_profile == XR_TARGET_RUNTIME_PROFILE_FREESTANDING &&
          (assertion_requirements & XR_ASSERTION_CAPABILITY_FAILURE_REPORT))
             ? 1u
             : 0u) +
        ((assertion_requirements & XR_ASSERTION_CAPABILITY_TYPED_ERROR_BOUNDARY) ? 1u : 0u) +
        ((assertion_requirements & XR_ASSERTION_CAPABILITY_PANIC_BOUNDARY) ? 1u : 0u);
    if (machine->runtime_profile == XR_TARGET_RUNTIME_PROFILE_FREESTANDING &&
        (assertion_requirements & (XR_ASSERTION_CAPABILITY_TYPED_ERROR_BOUNDARY |
                                   XR_ASSERTION_CAPABILITY_PANIC_BOUNDARY)) != 0)
        return fail(error, error_size, "XR_TARGET_1004",
                    "freestanding assertion action requires a capturable failure boundary");
    materialized->capability_count = 2u + assertion_count;
    materialized->capabilities = (XrTargetCapabilityRecord *) allocate_records(
        materialized->capability_count, sizeof(*materialized->capabilities));
    if (!materialized->capabilities)
        return fail(error, error_size, "XR_EXEC_5003", "capability closure materialization failed");
    materialized->capabilities[0] = (XrTargetCapabilityRecord) {
        .id = 0,
        .capability = XR_TARGET_PROVIDER_ALLOCATOR,
        .provider = XR_TARGET_PROVIDER_ALLOCATOR,
        .flags = XR_TARGET_CAPABILITY_REQUIRED,
    };
    materialized->capabilities[1] = (XrTargetCapabilityRecord) {
        .id = 1,
        .capability = XR_TARGET_PROVIDER_PANIC,
        .provider = XR_TARGET_PROVIDER_PANIC,
        .flags = XR_TARGET_CAPABILITY_REQUIRED,
    };
    uint32_t next = 2;
#define XR_APPEND_ASSERTION_CAPABILITY(assertion_bit, target_capability)                           \
    do {                                                                                           \
        if ((assertion_requirements & (assertion_bit)) != 0) {                                     \
            uint64_t capability_bit = xr_target_capability_mask(target_capability);                \
            uint16_t provider = xr_target_capability_provider(target_capability);                  \
            if (capability_bit == 0 ||                                                             \
                (target_capability != XR_TARGET_CAPABILITY_TYPED_ERROR_BOUNDARY &&                 \
                 (facts->provider_mask & capability_bit) == 0))                                    \
                return fail(error, error_size, "XR_TARGET_1004",                                   \
                            "target profile lacks a required assertion capability");               \
            materialized->capabilities[next] = (XrTargetCapabilityRecord) {                        \
                .id = next,                                                                        \
                .capability = target_capability,                                                   \
                .provider = provider,                                                              \
                .flags = XR_TARGET_CAPABILITY_REQUIRED,                                            \
            };                                                                                     \
            next++;                                                                                \
        }                                                                                          \
    } while (0)
    if (machine->runtime_profile == XR_TARGET_RUNTIME_PROFILE_FREESTANDING) {
        XR_APPEND_ASSERTION_CAPABILITY(XR_ASSERTION_CAPABILITY_FAILURE_REPORT,
                                       XR_TARGET_CAPABILITY_ASSERTION_REPORT);
    }
    XR_APPEND_ASSERTION_CAPABILITY(XR_ASSERTION_CAPABILITY_TYPED_ERROR_BOUNDARY,
                                   XR_TARGET_CAPABILITY_TYPED_ERROR_BOUNDARY);
    XR_APPEND_ASSERTION_CAPABILITY(XR_ASSERTION_CAPABILITY_PANIC_BOUNDARY,
                                   XR_TARGET_CAPABILITY_PANIC_BOUNDARY);
#undef XR_APPEND_ASSERTION_CAPABILITY
    if (next != materialized->capability_count)
        return fail(error, error_size, "XR_TARGET_1004",
                    "assertion capability closure is incomplete");
    return true;
}

static bool builder_materialize(XrTargetPlanBuilder *builder,
                                XrTargetMaterializedPlan *materialized, char *error,
                                size_t error_size) {
    if (!builder || !materialized || builder->poisoned || builder->materialized ||
        builder->started_family_mask != XR_TARGET_REQUIRED_FAMILIES ||
        builder->completed_family_mask != XR_TARGET_REQUIRED_FAMILIES)
        return fail(error, error_size, "XR_TARGET_1001",
                    "target builder family coverage is incomplete");
    if (!materialize_layouts(builder, materialized, error, error_size) ||
        !materialize_machine_reps(builder, materialized, error, error_size) ||
        !materialize_field_representations(builder, materialized, error, error_size) ||
        !materialize_functions_and_slots(builder, materialized, error, error_size) ||
        !materialize_values(builder, materialized, error, error_size) ||
        !materialize_coroutine_roots_and_string_cleanups(builder, materialized, error,
                                                         error_size) ||
        !materialize_calls_and_adapters(builder, materialized, error, error_size) ||
        !materialize_entry_expectations(builder, materialized, error, error_size) ||
        !materialize_coroutine_state_calls(builder, materialized, error, error_size) ||
        !materialize_typed_instructions(builder, materialized, error, error_size) ||
        !materialize_debug_facts(builder, materialized, error, error_size) ||
        !materialize_capabilities(builder, materialized, error, error_size)) {
        builder->poisoned = true;
        return false;
    }
    builder->materialized = true;
    return true;
}

static bool builder_new(const XrSemanticPlan *semantic_plan, XrTargetProfile *profile,
                        const XrSemanticPlan *const *dependencies, uint32_t dependency_count,
                        bool allow_program_graph, XrTargetPlanBuilder **out, char *error,
                        size_t error_size) {
    if (out)
        *out = NULL;
    if (!semantic_plan || !profile || !out)
        return fail(error, error_size, "XR_TARGET_1000", "target builder input is missing");
    const XrSemanticProgramProvenance *program =
        xr_semantic_plan_program_provenance(semantic_plan);
    if (!allow_program_graph && program && program->program_family ==
                       XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL)
        return fail(error, error_size, "XR_TARGET_1001",
                    "graph SemanticPlan execution is outside TargetPlan coverage");
    if (dependency_count > XR_TARGET_MAX_SEMANTIC_DEPENDENCIES ||
        dependency_count != xr_semantic_plan_dependency_count(semantic_plan))
        return fail(error, error_size, "XR_TARGET_1000", "semantic plan is not verified");
    char nested_error[512] = {0};
    bool verified =
        dependency_count == 0
            ? xr_semantic_plan_verify(semantic_plan, nested_error, sizeof(nested_error))
            : xr_semantic_plan_verify_module_set(semantic_plan, dependencies, dependency_count,
                                                 nested_error, sizeof(nested_error));
    if (!verified)
        return fail(error, error_size, "XR_TARGET_1000", "semantic plan is not verified");
    if (!xr_target_profile_verify(profile, error, error_size))
        return false;
    XrTargetPlanBuilder *builder = (XrTargetPlanBuilder *) xr_calloc(1, sizeof(*builder));
    if (!builder)
        return fail(error, error_size, "XR_EXEC_5003", "target builder allocation failed");
    builder->semantic_plan = xr_semantic_plan_retain((XrSemanticPlan *) semantic_plan);
    builder->semantic_dependency_count = dependency_count;
    if (dependency_count) {
        builder->semantic_dependencies = (XrSemanticPlan **) xr_calloc(
            dependency_count, sizeof(*builder->semantic_dependencies));
        if (!builder->semantic_dependencies) {
            builder_free(builder);
            return fail(error, error_size, "XR_EXEC_5003",
                        "target dependency retain allocation failed");
        }
        for (uint32_t i = 0; i < dependency_count; i++) {
            builder->semantic_dependencies[i] =
                xr_semantic_plan_retain((XrSemanticPlan *) dependencies[i]);
            if (!builder->semantic_dependencies[i]) {
                builder_free(builder);
                return fail(error, error_size, "XR_EXEC_5003", "target dependency retain failed");
            }
        }
    }
    builder->profile = xr_target_profile_retain(profile);
    if (!builder->semantic_plan || !builder->profile) {
        builder_free(builder);
        return fail(error, error_size, "XR_EXEC_5003", "target builder retain failed");
    }
    *out = builder;
    return true;
}

static bool builder_freeze(XrTargetPlanBuilder *builder, XrTargetPlan **out, char *error,
                           size_t error_size) {
    if (out)
        *out = NULL;
    if (!builder || !out || !builder->semantic_plan || !builder->profile)
        return fail(error, error_size, "XR_TARGET_1000", "target builder freeze input is missing");
    XrTargetMaterializedPlan materialized = {0};
    if (!builder_materialize(builder, &materialized, error, error_size)) {
        materialized_dispose(&materialized);
        return false;
    }
    XrTargetPlanDraft draft = {
        .semantic_plan = builder->semantic_plan,
        .semantic_dependencies = (const XrSemanticPlan *const *) builder->semantic_dependencies,
        .semantic_dependency_count = builder->semantic_dependency_count,
        .profile = builder->profile,
        .completed_family_mask = builder->completed_family_mask,
        .machine_reps = materialized.machine_reps,
        .machine_reps_count = materialized.machine_rep_count,
        .value_reps = materialized.value_reps,
        .value_reps_count = materialized.value_rep_count,
        .extents = materialized.extents,
        .extents_count = materialized.extent_count,
        .layouts = materialized.layouts,
        .layouts_count = materialized.layout_count,
        .fields = materialized.fields,
        .fields_count = materialized.field_count,
        .functions = materialized.functions,
        .functions_count = materialized.function_count,
        .slots = materialized.slots,
        .slots_count = materialized.slot_count,
        .instructions = materialized.instructions,
        .instructions_count = materialized.instruction_count,
        .calls = materialized.calls,
        .calls_count = materialized.call_count,
        .call_arguments = materialized.call_arguments,
        .call_arguments_count = materialized.call_argument_count,
        .root_maps = materialized.root_maps,
        .root_maps_count = materialized.root_map_count,
        .root_slots = materialized.root_slots,
        .root_slots_count = materialized.root_slot_count,
        .cleanups = materialized.cleanups,
        .cleanups_count = materialized.cleanup_count,
        .adapters = materialized.adapters,
        .adapters_count = materialized.adapter_count,
        .capabilities = materialized.capabilities,
        .capabilities_count = materialized.capability_count,
        .coroutines = materialized.coroutines,
        .coroutines_count = materialized.coroutine_count,
        .entry_expectations = materialized.entry_expectations,
        .entry_expectations_count = materialized.entry_expectation_count,
        .debug_facts = materialized.debug_facts,
        .debug_facts_count = materialized.debug_fact_count,
    };
    bool frozen = xr_target_plan_freeze(&draft, out, error, error_size);
    materialized_dispose(&materialized);
    return frozen;
}

static void builder_free(XrTargetPlanBuilder *builder) {
    if (!builder)
        return;
    xr_free(builder->rep_intents);
    xr_free(builder->layout_intents);
    xr_free(builder->value_intents);
    xr_free(builder->slot_intents);
    xr_free(builder->call_intents);
    xr_free(builder->call_argument_intents);
    for (uint32_t i = 0; i < builder->semantic_dependency_count; i++)
        xr_semantic_plan_free(builder->semantic_dependencies[i]);
    xr_free(builder->semantic_dependencies);
    xr_semantic_plan_free(builder->semantic_plan);
    xr_target_profile_free(builder->profile);
    xr_free(builder);
}

/* One family states the authority for one construct. They run in the order
 * their authorities become readable: a family may read what an earlier one
 * stated and never what a later one will, so the order below is a dependency
 * order and not a list. */
typedef bool (*XrTargetFamilyBuild)(XrTargetPlanBuilder *builder, char *error, size_t error_size);

typedef struct {
    const char *name;
    XrTargetFamilyBuild build;
} XrTargetFamily;

static const XrTargetFamily k_target_families[] = {
    {"scalars", builder_add_scalars},
    {"closure_storage", builder_add_closure_storage},
    {"array_allocation_storage", builder_add_array_allocation_storage},
    {"array_intrinsic_storage", builder_add_array_intrinsic_storage},
    {"array_hof_result_storage", builder_add_array_hof_result_storage},
    {"nullable_scalar_storage", builder_add_nullable_scalar_storage},
    {"array_member_result_storage", builder_add_array_member_result_storage},
    {"source_class_object_storage", builder_add_source_class_object_storage},
    {"source_class_instance_storage", builder_add_source_class_instance_storage},
    {"source_class_receiver_storage", builder_add_source_class_receiver_storage},
    {"source_class_method_receiver_storage", builder_add_source_class_method_receiver_storage},
    {"source_class_argument_storage", builder_add_source_class_argument_storage},
    {"string_concat_result_storage", builder_add_string_concat_result_storage},
    {"string_convert_result_storage", builder_add_string_convert_result_storage},
    {"panic_catch_storage", builder_add_panic_catch_storage},
    {"string_literal_storage", builder_add_string_literal_storage},
    {"string_runes_storage", builder_add_string_runes_storage},
    {"string_slice_range_storage", builder_add_string_slice_range_storage},
    {"rune_to_string_storage", builder_add_rune_to_string_storage},
    {"string_byte_slice_view_storage", builder_add_string_byte_slice_view_storage},
    {"range_slice_view_storage", builder_add_range_slice_view_storage},
    {"stringbuilder_append_rune_storage", builder_add_stringbuilder_append_rune_storage},
    {"stringbuilder_to_string_storage", builder_add_stringbuilder_to_string_storage},
    {"stringbuilder_append_string_storage", builder_add_stringbuilder_append_string_storage},
    {"json_namespace_value_storage", builder_add_json_namespace_value_storage},
    {"panic_info_constructor_storage", builder_add_panic_info_constructor_storage},
    {"container_copy_result_storage", builder_add_container_copy_result_storage},
    {"direct_local_string_boundary_storage", builder_add_direct_local_string_boundary_storage},
    {"adt_enum_storage", builder_add_adt_enum_storage},
    {"direct_local_callee_storage", builder_add_direct_local_callee_storage},
    {"direct_local_go_callee_storage", builder_add_direct_local_go_callee_storage},
    {"direct_local_go_task_result_storage", builder_add_direct_local_go_task_result_storage},
    {"channel_allocation_storage", builder_add_channel_allocation_storage},
    {"channel_receive_storage", builder_add_channel_receive_storage},
    {"source_namespace_storage", builder_add_source_namespace_storage},
    {"native_module_namespace_storage", builder_add_native_module_namespace_storage},
    {"dynamic_value_storage", builder_add_dynamic_value_storage},
    /* A ref boundary consumes storage already owned by the exact producer
     * families above, including typed reference joins. It must run before the
     * address family that turns its LOCAL_ADDR result into a raw pointer. */
    {"direct_local_tagged_ref_argument_storage",
     builder_add_direct_local_tagged_ref_argument_storage},
    {"local_address_storage", builder_add_local_address_storage},
    {"aggregates", builder_add_aggregates},
    /* After the aggregate family, whose slots it states the authority for, and
     * before the call family, which reads that authority to give the call row
     * its result representation. */
    {"direct_local_aggregate_result_storage", builder_add_direct_local_aggregate_result_storage},
    /* Propagates, so it runs after every family that decides storage and
     * before the call family that reads the result. */
    {"identity_copy_storage", builder_add_identity_copy_storage},
    {"owner_forward_storage", builder_add_owner_forward_storage},
    {"calls_and_adapters", builder_add_calls_and_adapters},
    {"coroutine_state_calls", builder_add_coroutine_state_calls},
    {"dynamic_entry_expectations", builder_add_dynamic_entry_expectations},
};

static bool builder_collect_families(XrTargetPlanBuilder *builder, char *error,
                                     size_t error_size) {
    bool survey = false;
    uint32_t refused_families = 0;
    for (size_t i = 0; i < sizeof(k_target_families) / sizeof(k_target_families[0]); i++) {
        char family_error[512] = {0};
        bool ok_p = k_target_families[i].build(builder, family_error, sizeof(family_error));
        if (ok_p)
            continue;
        if (refused_families == 0) {
            survey = target_survey_enabled();
            if (error && error_size)
                snprintf(error, error_size, "%s", family_error);
        }
        refused_families++;
        if (!survey)
            return false;
        target_survey_row(k_target_families[i].name, family_error);
    }
    if (!refused_families)
        return true;
    fprintf(stderr, "[refusal-survey] target-plan families refused: %u\n", refused_families);
    return false;
}

typedef struct XrProgramGraphModuleDraft {
    const XrSemanticPlan *semantic;
    XrTargetMaterializedPlan target;
    uint32_t semantic_module;
} XrProgramGraphModuleDraft;

static bool build_program_graph_module(const XrSemanticPlan *semantic,
                                       const XrSemanticPlan *const *dependencies,
                                       uint32_t dependency_count, XrTargetProfile *profile,
                                       XrTargetMaterializedPlan *out, char *error,
                                       size_t error_size) {
    XrTargetPlanBuilder *builder = NULL;
    memset(out, 0, sizeof(*out));
    if (!builder_new(semantic, profile, dependencies, dependency_count, true, &builder, error,
                     error_size))
        return false;
    bool built = builder_collect_families(builder, error, error_size) &&
                 builder_materialize(builder, out, error, error_size);
    builder_free(builder);
    if (!built)
        materialized_dispose(out);
    return built;
}

static bool graph_add_count(uint32_t *total, uint32_t count) {
    if (*total > UINT32_MAX - count)
        return false;
    *total += count;
    return true;
}

static void graph_add_row(uint32_t *row, uint32_t offset) {
    if (*row != XR_SEMANTIC_INDEX_NONE)
        *row += offset;
}

static bool graph_allocate_materialized(XrTargetMaterializedPlan *target, char *error,
                                        size_t error_size) {
#define XR_GRAPH_ALLOCATE(name, count_name)                                                        \
    do {                                                                                           \
        target->name = allocate_records(target->count_name, sizeof(*target->name));                \
        if (target->count_name && !target->name)                                                   \
            return fail(error, error_size, "XR_EXEC_5003",                                       \
                        "program graph target table allocation failed");                          \
    } while (0)
    XR_GRAPH_ALLOCATE(machine_reps, machine_rep_count);
    XR_GRAPH_ALLOCATE(value_reps, value_rep_count);
    XR_GRAPH_ALLOCATE(extents, extent_count);
    XR_GRAPH_ALLOCATE(layouts, layout_count);
    XR_GRAPH_ALLOCATE(fields, field_count);
    XR_GRAPH_ALLOCATE(functions, function_count);
    XR_GRAPH_ALLOCATE(slots, slot_count);
    XR_GRAPH_ALLOCATE(instructions, instruction_count);
    XR_GRAPH_ALLOCATE(calls, call_count);
    XR_GRAPH_ALLOCATE(call_arguments, call_argument_count);
    XR_GRAPH_ALLOCATE(root_maps, root_map_count);
    XR_GRAPH_ALLOCATE(root_slots, root_slot_count);
    XR_GRAPH_ALLOCATE(cleanups, cleanup_count);
    XR_GRAPH_ALLOCATE(adapters, adapter_count);
    XR_GRAPH_ALLOCATE(capabilities, capability_count);
    XR_GRAPH_ALLOCATE(coroutines, coroutine_count);
    XR_GRAPH_ALLOCATE(debug_facts, debug_fact_count);
#undef XR_GRAPH_ALLOCATE
    return true;
}

static bool graph_accumulate_counts(XrTargetMaterializedPlan *target,
                                    const XrTargetMaterializedPlan *source) {
#define XR_GRAPH_ADD_COUNT(name)                                                                  \
    if (!graph_add_count(&target->name, source->name))                                             \
        return false
    XR_GRAPH_ADD_COUNT(value_rep_count);
    XR_GRAPH_ADD_COUNT(extent_count);
    XR_GRAPH_ADD_COUNT(layout_count);
    XR_GRAPH_ADD_COUNT(field_count);
    XR_GRAPH_ADD_COUNT(function_count);
    XR_GRAPH_ADD_COUNT(slot_count);
    XR_GRAPH_ADD_COUNT(instruction_count);
    XR_GRAPH_ADD_COUNT(call_count);
    XR_GRAPH_ADD_COUNT(call_argument_count);
    XR_GRAPH_ADD_COUNT(root_map_count);
    XR_GRAPH_ADD_COUNT(root_slot_count);
    XR_GRAPH_ADD_COUNT(cleanup_count);
    XR_GRAPH_ADD_COUNT(adapter_count);
    XR_GRAPH_ADD_COUNT(coroutine_count);
    XR_GRAPH_ADD_COUNT(debug_fact_count);
#undef XR_GRAPH_ADD_COUNT
    return true;
}

static bool graph_merge_machine_reps(const XrTargetMaterializedPlan *left,
                                     const XrTargetMaterializedPlan *right,
                                     XrTargetMaterializedPlan *target,
                                     uint16_t rep_maps[2][256]) {
    uint32_t indexes[2] = {0, 0};
    uint32_t count = 0;
    while (indexes[0] < left->machine_rep_count || indexes[1] < right->machine_rep_count) {
        bool have_left = indexes[0] < left->machine_rep_count;
        bool have_right = indexes[1] < right->machine_rep_count;
        int order = !have_left ? 1
                    : !have_right
                        ? -1
                        : compare_rep_record(&left->machine_reps[indexes[0]],
                                             &right->machine_reps[indexes[1]]);
        if (count >= target->machine_rep_count || count >= 256u)
            return false;
        const XrTargetMachineRepRecord *chosen =
            order <= 0 ? &left->machine_reps[indexes[0]]
                       : &right->machine_reps[indexes[1]];
        target->machine_reps[count] = *chosen;
        target->machine_reps[count].id = count;
        if (order <= 0)
            rep_maps[0][indexes[0]++] = (uint16_t) count;
        if (order >= 0)
            rep_maps[1][indexes[1]++] = (uint16_t) count;
        count++;
    }
    target->machine_rep_count = count;
    return true;
}

static bool graph_merge_capabilities(const XrTargetMaterializedPlan *left,
                                     const XrTargetMaterializedPlan *right,
                                     XrTargetMaterializedPlan *target) {
    uint32_t indexes[2] = {0u, 0u};
    uint32_t count = 0u;
    while (indexes[0] < left->capability_count || indexes[1] < right->capability_count) {
        const XrTargetCapabilityRecord *left_row =
            indexes[0] < left->capability_count ? &left->capabilities[indexes[0]] : NULL;
        const XrTargetCapabilityRecord *right_row =
            indexes[1] < right->capability_count ? &right->capabilities[indexes[1]] : NULL;
        int order = !right_row ? -1
                    : !left_row ? 1
                    : left_row->capability < right_row->capability ? -1
                    : left_row->capability > right_row->capability ? 1
                                                                    : 0;
        if (order == 0 &&
            (left_row->provider != right_row->provider ||
             left_row->flags != right_row->flags))
            return false;
        const XrTargetCapabilityRecord *chosen = order <= 0 ? left_row : right_row;
        if (!chosen || count >= target->capability_count)
            return false;
        target->capabilities[count] = *chosen;
        target->capabilities[count].id = count++;
        if (order <= 0)
            indexes[0]++;
        if (order >= 0)
            indexes[1]++;
    }
    target->capability_count = count;
    return true;
}

static bool graph_merge_module(XrTargetMaterializedPlan *target,
                               const XrTargetMaterializedPlan *source,
                               XrTargetModulePartitionRecord *partition,
                               const uint16_t rep_map[256]) {
#define XR_GRAPH_COPY(name, count_name, begin_name)                                                \
    do {                                                                                           \
        if (source->count_name)                                                                    \
            memcpy(target->name + partition->begin_name, source->name,                            \
                   (size_t) source->count_name * sizeof(*source->name));                           \
    } while (0)
    XR_GRAPH_COPY(value_reps, value_rep_count, value_reps_begin);
    XR_GRAPH_COPY(extents, extent_count, extents_begin);
    XR_GRAPH_COPY(layouts, layout_count, layouts_begin);
    XR_GRAPH_COPY(fields, field_count, fields_begin);
    XR_GRAPH_COPY(functions, function_count, functions_begin);
    XR_GRAPH_COPY(slots, slot_count, slots_begin);
    XR_GRAPH_COPY(instructions, instruction_count, instructions_begin);
    XR_GRAPH_COPY(calls, call_count, calls_begin);
    XR_GRAPH_COPY(call_arguments, call_argument_count, call_arguments_begin);
    XR_GRAPH_COPY(root_maps, root_map_count, root_maps_begin);
    XR_GRAPH_COPY(root_slots, root_slot_count, root_slots_begin);
    XR_GRAPH_COPY(cleanups, cleanup_count, cleanups_begin);
    XR_GRAPH_COPY(adapters, adapter_count, adapters_begin);
    XR_GRAPH_COPY(coroutines, coroutine_count, coroutines_begin);
    XR_GRAPH_COPY(debug_facts, debug_fact_count, debug_facts_begin);
#undef XR_GRAPH_COPY

    for (uint32_t i = 0; i < partition->value_reps_count; i++) {
        XrTargetValueRepRecord *row =
            &target->value_reps[partition->value_reps_begin + i];
        graph_add_row(&row->slot, partition->slots_begin);
        row->register_rep = rep_map[row->register_rep];
        row->memory_rep = rep_map[row->memory_rep];
    }
    for (uint32_t i = 0; i < partition->extents_count; i++) {
        XrTargetExtentRecord *row = &target->extents[partition->extents_begin + i];
        row->id += partition->extents_begin;
        graph_add_row(&row->element_layout, partition->layouts_begin);
    }
    for (uint32_t i = 0; i < partition->layouts_count; i++) {
        XrTargetLayoutRecord *row = &target->layouts[partition->layouts_begin + i];
        row->id += partition->layouts_begin;
        graph_add_row(&row->extent, partition->extents_begin);
        row->field_begin += partition->fields_begin;
    }
    for (uint32_t i = 0; i < partition->fields_count; i++) {
        XrTargetFieldRecord *row = &target->fields[partition->fields_begin + i];
        row->layout += partition->layouts_begin;
        row->memory_rep = rep_map[row->memory_rep];
    }
    for (uint32_t i = 0; i < partition->functions_count; i++) {
        XrTargetFunctionRecord *row = &target->functions[partition->functions_begin + i];
        row->id += partition->functions_begin;
        row->slot_begin += partition->slots_begin;
        row->root_begin += partition->root_maps_begin;
        row->cleanup_begin += partition->cleanups_begin;
        row->coroutine_begin += partition->coroutines_begin;
    }
    for (uint32_t i = 0; i < partition->slots_count; i++) {
        XrTargetSlotRecord *row = &target->slots[partition->slots_begin + i];
        row->id += partition->slots_begin;
        row->function += partition->functions_begin;
        row->register_rep = rep_map[row->register_rep];
        row->memory_rep = rep_map[row->memory_rep];
    }
    for (uint32_t i = 0; i < partition->instructions_count; i++) {
        XrTargetInstructionRecord *row =
            &target->instructions[partition->instructions_begin + i];
        row->id += partition->instructions_begin;
        row->function += partition->functions_begin;
        graph_add_row(&row->result_slot, partition->slots_begin);
        graph_add_row(&row->operand_slots[0], partition->slots_begin);
        graph_add_row(&row->operand_slots[1], partition->slots_begin);
        if (row->opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_I64)
            row->immediate_bits += partition->calls_begin;
    }
    for (uint32_t i = 0; i < partition->calls_count; i++) {
        XrTargetCallRecord *row = &target->calls[partition->calls_begin + i];
        row->id += partition->calls_begin;
        row->caller_function += partition->functions_begin;
        graph_add_row(&row->callee_function, partition->functions_begin);
        graph_add_row(&row->result_slot, partition->slots_begin);
        graph_add_row(&row->caller_storage_slot, partition->slots_begin);
        graph_add_row(&row->error_slot, partition->slots_begin);
        row->argument_begin += partition->call_arguments_begin;
        row->adapter_begin += partition->adapters_begin;
        row->result_register_rep = rep_map[row->result_register_rep];
        row->result_memory_rep = rep_map[row->result_memory_rep];
        row->error_register_rep = rep_map[row->error_register_rep];
        row->error_memory_rep = rep_map[row->error_memory_rep];
    }
    for (uint32_t i = 0; i < partition->call_arguments_count; i++) {
        XrTargetCallArgumentRecord *row =
            &target->call_arguments[partition->call_arguments_begin + i];
        row->call += partition->calls_begin;
        graph_add_row(&row->caller_slot, partition->slots_begin);
        graph_add_row(&row->callee_slot, partition->slots_begin);
        row->register_rep = rep_map[row->register_rep];
        row->memory_rep = rep_map[row->memory_rep];
        row->callee_register_rep = rep_map[row->callee_register_rep];
        row->callee_memory_rep = rep_map[row->callee_memory_rep];
    }
    for (uint32_t i = 0; i < partition->root_maps_count; i++) {
        XrTargetRootMapRecord *row = &target->root_maps[partition->root_maps_begin + i];
        row->id += partition->root_maps_begin;
        row->function += partition->functions_begin;
        row->slot_begin += partition->root_slots_begin;
    }
    for (uint32_t i = 0; i < partition->root_slots_count; i++)
        target->root_slots[partition->root_slots_begin + i] += partition->slots_begin;
    for (uint32_t i = 0; i < partition->cleanups_count; i++) {
        XrTargetCleanupRecord *row = &target->cleanups[partition->cleanups_begin + i];
        row->id += partition->cleanups_begin;
        row->function += partition->functions_begin;
        row->slot += partition->slots_begin;
    }
    for (uint32_t i = 0; i < partition->adapters_count; i++) {
        XrTargetAdapterRecord *row = &target->adapters[partition->adapters_begin + i];
        row->id += partition->adapters_begin;
        row->call += partition->calls_begin;
        row->input_rep = rep_map[row->input_rep];
        row->output_rep = rep_map[row->output_rep];
        graph_add_row(&row->layout, partition->layouts_begin);
    }
    for (uint32_t i = 0; i < partition->coroutines_count; i++) {
        XrTargetCoroutineStateRecord *row = &target->coroutines[partition->coroutines_begin + i];
        row->id += partition->coroutines_begin;
        row->function += partition->functions_begin;
        graph_add_row(&row->direct_call, partition->calls_begin);
        graph_add_row(&row->result_slot, partition->slots_begin);
    }
    for (uint32_t i = 0; i < partition->debug_facts_count; i++) {
        XrTargetDebugFactRecord *row = &target->debug_facts[partition->debug_facts_begin + i];
        row->id += partition->debug_facts_begin;
        row->instruction += partition->instructions_begin;
        row->function += partition->functions_begin;
        graph_add_row(&row->coroutine_state, partition->coroutines_begin);
    }
    return true;
}

static const XrSemanticProgramFunctionBinding *
graph_function_binding(const XrSemanticPlan *semantic, uint8_t exact_flags) {
    const XrSemanticProgramFunctionBinding *found = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_program_function_binding_count(semantic); i++) {
        const XrSemanticProgramFunctionBinding *row =
            xr_semantic_plan_program_function_binding(semantic, i);
        if (!row || row->flags != exact_flags)
            continue;
        if (found)
            return NULL;
        found = row;
    }
    return found;
}

static bool graph_target_function_for_semantic(const XrTargetMaterializedPlan *target,
                                               const XrTargetModulePartitionRecord *partition,
                                               uint32_t semantic_function,
                                               uint32_t *target_function) {
    for (uint32_t i = 0; i < partition->functions_count; i++) {
        uint32_t row = partition->functions_begin + i;
        if (target->functions[row].semantic_function != semantic_function)
            continue;
        *target_function = row;
        return true;
    }
    return false;
}

static bool graph_target_value_slot(const XrTargetMaterializedPlan *target,
                                    const XrTargetModulePartitionRecord *partition,
                                    uint32_t semantic_value, uint32_t *slot,
                                    uint16_t *register_rep, uint16_t *memory_rep) {
    for (uint32_t i = 0; i < partition->value_reps_count; i++) {
        const XrTargetValueRepRecord *row =
            &target->value_reps[partition->value_reps_begin + i];
        if (row->semantic_value != semantic_value)
            continue;
        if (row->slot == XR_SEMANTIC_INDEX_NONE)
            return false;
        *slot = row->slot;
        *register_rep = row->register_rep;
        *memory_rep = row->memory_rep;
        return true;
    }
    return false;
}

static bool graph_bind_direct_call(
    const XrSemanticPlan *entry, const XrSemanticPlan *producer,
    const XrTargetModulePartitionRecord *entry_partition,
    const XrTargetModulePartitionRecord *producer_partition, XrTargetProfile *profile,
    XrTargetMaterializedPlan *target, XrTargetProgramGraphRecord *graph, char *error,
    size_t error_size) {
    const XrSemanticProgramProvenance *program = xr_semantic_plan_program_provenance(entry);
    const XrSemanticProgramFunctionBinding *entry_function =
        graph_function_binding(entry, XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY);
    const XrSemanticProgramFunctionBinding *producer_function =
        graph_function_binding(producer, XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED);
    const XrSemanticProgramCallBinding *call_binding =
        xr_semantic_plan_program_call_binding_count(entry) == 1u
            ? xr_semantic_plan_program_call_binding(entry, 0)
            : NULL;
    const XrSemanticOperationRecord *operation =
        call_binding ? xr_semantic_plan_operation(entry, call_binding->operation) : NULL;
    const XrSemanticCallTargetRecord *semantic_target = NULL;
    uint32_t semantic_target_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; operation && i < xr_semantic_plan_call_target_count(entry); i++) {
        const XrSemanticCallTargetRecord *candidate = xr_semantic_plan_call_target(entry, i);
        if (!candidate || candidate->operation != call_binding->operation)
            continue;
        if (semantic_target)
            return fail(error, error_size, "XR_TARGET_1001",
                        "program graph call target is ambiguous");
        semantic_target = candidate;
        semantic_target_index = i;
    }
    const XrSemanticSourceExportRecord *source_export =
        semantic_target && semantic_target->source_export <
                               xr_semantic_plan_source_export_count(producer)
            ? xr_semantic_plan_source_export(producer, semantic_target->source_export)
            : NULL;
    const XrSemanticFunctionRecord *producer_semantic_function =
        producer_function
            ? xr_semantic_plan_function(producer, producer_function->semantic_function)
            : NULL;
    const XrSemanticParameterRecord *parameter =
        producer_semantic_function && producer_semantic_function->parameter_count == 1u
            ? xr_semantic_plan_parameter(producer,
                                         producer_semantic_function->parameter_begin)
            : NULL;
    uint32_t entry_target_function = UINT32_MAX, producer_target_function = UINT32_MAX;
    if (!program || !entry_function || !producer_function || !call_binding || !operation ||
        !semantic_target || !source_export || !producer_semantic_function || !parameter ||
        semantic_target->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT ||
        semantic_target->dependency != 0u || source_export->kind != XR_SEM_SOURCE_EXPORT_FUNCTION ||
        source_export->function != producer_function->semantic_function ||
        !graph_target_function_for_semantic(target, entry_partition,
                                            entry_function->semantic_function,
                                            &entry_target_function) ||
        !graph_target_function_for_semantic(target, producer_partition,
                                            producer_function->semantic_function,
                                            &producer_target_function))
        return fail(error, error_size, "XR_TARGET_1001",
                    "program graph semantic function/export joins are not exact");

    XrTargetCallRecord *call = NULL;
    uint32_t target_call = UINT32_MAX;
    for (uint32_t i = 0; i < entry_partition->calls_count; i++) {
        uint32_t row = entry_partition->calls_begin + i;
        XrTargetCallRecord *candidate = &target->calls[row];
        if (candidate->semantic_operation != call_binding->operation ||
            candidate->semantic_call_target != semantic_target_index)
            continue;
        if (call)
            return fail(error, error_size, "XR_TARGET_1001",
                        "program graph target call is ambiguous");
        call = candidate;
        target_call = row;
    }
    if (!call || call->argument_count != 1u ||
        call->argument_begin >= target->call_argument_count)
        return fail(error, error_size, "XR_TARGET_1001",
                    "program graph target call/argument is missing");
    XrTargetCallArgumentRecord *argument = &target->call_arguments[call->argument_begin];
    uint32_t callee_slot = UINT32_MAX;
    uint16_t callee_register_rep = 0, callee_memory_rep = 0;
    if (argument->call != target_call || argument->ordinal != 0u ||
        argument->callee_parameter != producer_semantic_function->parameter_begin ||
        !graph_target_value_slot(target, producer_partition, parameter->value, &callee_slot,
                                 &callee_register_rep, &callee_memory_rep))
        return fail(error, error_size, "XR_TARGET_1001",
                    "program graph argument slot join is not exact");

    uint32_t direct_instruction = UINT32_MAX;
    for (uint32_t i = 0; i < entry_partition->instructions_count; i++) {
        uint32_t row = entry_partition->instructions_begin + i;
        XrTargetInstructionRecord *instruction = &target->instructions[row];
        if (instruction->opcode != XR_TARGET_INSTRUCTION_CALL_ENTRY_I64 ||
            instruction->function != entry_target_function ||
            instruction->result_slot != call->result_slot)
            continue;
        if (direct_instruction != UINT32_MAX)
            return fail(error, error_size, "XR_TARGET_1001",
                        "program graph call instruction is ambiguous");
        direct_instruction = row;
    }
    if (direct_instruction == UINT32_MAX)
        return fail(error, error_size, "XR_TARGET_1001",
                    "program graph call instruction is missing");

    call->callee_function = producer_target_function;
    call->calling_convention = XR_TARGET_CALL_CONVENTION_PROGRAM_DIRECT;
    call->target_kind = XR_TARGET_CALL_TARGET_PROGRAM_DIRECT;
    argument->callee_slot = callee_slot;
    argument->callee_register_rep = callee_register_rep;
    argument->callee_memory_rep = callee_memory_rep;
    target->instructions[direct_instruction].opcode = XR_TARGET_INSTRUCTION_CALL_DIRECT_I64;
    target->instructions[direct_instruction].immediate_bits = target_call;

    const XrSemanticFunctionRecord *entry_semantic_function =
        xr_semantic_plan_function(entry, entry_function->semantic_function);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(entry, &operand_count);
    uint32_t argument_operand = operation->operand_begin + 1u;
    if (!entry_semantic_function || !operands || operation->operand_count != 2u ||
        argument_operand >= operand_count || argument->semantic_operand != argument_operand)
        return fail(error, error_size, "XR_TARGET_1001",
                    "program graph semantic argument identity is missing");

    *graph = (XrTargetProgramGraphRecord) {
        .schema = XR_TARGET_PROGRAM_GRAPH_SCHEMA_VERSION,
        .family = program->program_family,
        .module_count = 2u,
        .function_count = program->function_count,
        .export_count = 1u,
        .entry_count = 1u,
        .call_count = 1u,
        .argument_count = 1u,
        .entry_partition = entry_partition->program_module_row,
        .producer_partition = producer_partition->program_module_row,
        .entry_target_function = entry_target_function,
        .producer_target_function = producer_target_function,
        .entry_semantic_function = entry_function->semantic_function,
        .producer_semantic_function = producer_function->semantic_function,
        .target_call = target_call,
        .target_argument = call->argument_begin,
        .entry_semantic_operation = call_binding->operation,
        .producer_semantic_export = semantic_target->source_export,
        .entry_semantic_dependency = semantic_target->dependency,
        .producer_semantic_parameter = producer_semantic_function->parameter_begin,
        .caller_slot = argument->caller_slot,
        .callee_slot = argument->callee_slot,
        .argument_ordinal = argument->ordinal,
        .flags = XR_TARGET_PROGRAM_GRAPH_SINGLE_PLAN | XR_TARGET_PROGRAM_GRAPH_DIRECT_I64,
        .program_fingerprint = program->program_fingerprint,
        .generation_identity = program->generation_identity,
        .target_profile_fingerprint = xr_target_profile_fingerprint(profile),
        .entry_function_identity = entry_function->program_function,
        .producer_function_identity = producer_function->program_function,
        .entry_function_flags = entry_function->flags,
        .producer_function_flags = producer_function->flags,
        .export_identity = source_export->id,
        .exported_function_identity = source_export->exported_entity,
        .entry_identity = entry_semantic_function->id,
        .call_identity = call_binding->program_call,
        .callsite_identity = call_binding->callsite,
        .resolver_binding = call_binding->resolver_binding,
        .argument_identity = argument->identity,
        .parameter_identity = parameter->id,
    };
    return true;
}

bool xr_target_plan_build_program_graph(const XrSemanticPlan *const *semantic_modules,
                                        uint32_t semantic_module_count,
                                        XrTargetProfile *profile,
                                        XrTargetPlan **out, char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!semantic_modules || semantic_module_count != 2u || !profile || !out)
        return fail(error, error_size, "XR_TARGET_1000",
                    "program graph builder requires the exact bounded semantic module set");
    if (!xr_target_semantic_program_module_set_verify(
            semantic_modules, semantic_module_count, error, error_size))
        return false;
    const XrSemanticPlan *entry = NULL;
    const XrSemanticPlan *producer = NULL;
    for (uint32_t row = 0; row < semantic_module_count; row++) {
        const XrSemanticPlan *semantic = semantic_modules[row];
        if (graph_function_binding(semantic, XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY)) {
            if (entry)
                return fail(error, error_size, "XR_TARGET_1000",
                            "program graph entry module is ambiguous");
            entry = semantic;
        }
        if (graph_function_binding(semantic, XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED)) {
            if (producer)
                return fail(error, error_size, "XR_TARGET_1000",
                            "program graph producer module is ambiguous");
            producer = semantic;
        }
    }
    const XrSemanticProgramProvenance *entry_program =
        xr_semantic_plan_program_provenance(entry);
    const XrSemanticProgramProvenance *producer_program =
        xr_semantic_plan_program_provenance(producer);
    if (!entry || !producer || !entry_program || !producer_program ||
        entry_program->program_family !=
            XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL ||
        producer_program->program_family != entry_program->program_family ||
        entry_program->module_count != 2u || producer_program->module_count != 2u ||
        entry_program->program_module_row >= 2u || producer_program->program_module_row >= 2u ||
        entry_program->program_module_row == producer_program->program_module_row ||
        !xr_fingerprint_equal(entry_program->program_fingerprint,
                              producer_program->program_fingerprint) ||
        !xr_stable_id_equal(entry_program->generation_identity,
                            producer_program->generation_identity))
        return fail(error, error_size, "XR_TARGET_1000",
                    "program graph builder requires the exact bounded semantic module set");

    XrProgramGraphModuleDraft modules[2] = {0};
    modules[entry_program->program_module_row].semantic = entry;
    modules[entry_program->program_module_row].semantic_module =
        entry_program->program_module_row;
    modules[producer_program->program_module_row].semantic = producer;
    modules[producer_program->program_module_row].semantic_module =
        producer_program->program_module_row;
    const XrSemanticPlan **entry_dependencies = NULL;
    const XrSemanticPlan **producer_dependencies = NULL;
    uint32_t entry_dependency_count = 0, producer_dependency_count = 0;
    if (!xr_target_semantic_program_module_direct_dependencies(
            semantic_modules, semantic_module_count, entry_program->program_module_row,
            &entry_dependencies, &entry_dependency_count, error, error_size) ||
        !xr_target_semantic_program_module_direct_dependencies(
            semantic_modules, semantic_module_count, producer_program->program_module_row,
            &producer_dependencies, &producer_dependency_count, error, error_size) ||
        entry_dependency_count != 1u || producer_dependency_count != 0u) {
        xr_free(entry_dependencies);
        xr_free(producer_dependencies);
        return fail(error, error_size, "XR_TARGET_1000",
                    "program graph local dependency vectors are not exact");
    }
    bool built = build_program_graph_module(entry, entry_dependencies, entry_dependency_count,
                                            profile,
                                            &modules[entry_program->program_module_row].target,
                                            error, error_size) &&
                 build_program_graph_module(producer, producer_dependencies,
                                            producer_dependency_count, profile,
                                            &modules[producer_program->program_module_row].target,
                                            error, error_size);
    if (!built)
        goto done;
    XrTargetMaterializedPlan merged = {0};
    if (modules[0].target.machine_rep_count >
        UINT32_MAX - modules[1].target.machine_rep_count) {
        fail(error, error_size, "XR_EXEC_5003",
             "program graph machine representation count overflowed");
        built = false;
        goto done;
    }
    merged.machine_rep_count =
        modules[0].target.machine_rep_count + modules[1].target.machine_rep_count;
    if (modules[0].target.capability_count >
        UINT32_MAX - modules[1].target.capability_count) {
        fail(error, error_size, "XR_EXEC_5003", "program graph capability count overflowed");
        built = false;
        goto done;
    }
    merged.capability_count =
        modules[0].target.capability_count + modules[1].target.capability_count;
    for (uint32_t i = 0; built && i < 2u; i++)
        built = graph_accumulate_counts(&merged, &modules[i].target);
    if (!built || !graph_allocate_materialized(&merged, error, error_size)) {
        if (built)
            built = false;
        else
            fail(error, error_size, "XR_EXEC_5003",
                 "program graph target row count overflowed");
        materialized_dispose(&merged);
        goto done;
    }
    uint16_t rep_maps[2][256] = {{0}};
    if (!graph_merge_machine_reps(&modules[0].target, &modules[1].target, &merged, rep_maps)) {
        fail(error, error_size, "XR_TARGET_1004",
             "program graph machine representation union is not exact");
        built = false;
        materialized_dispose(&merged);
        goto done;
    }
    if (!graph_merge_capabilities(&modules[0].target, &modules[1].target, &merged)) {
        fail(error, error_size, "XR_TARGET_1004",
             "program graph capability authorities are inconsistent");
        built = false;
        materialized_dispose(&merged);
        goto done;
    }

    XrTargetModulePartitionRecord partitions[2] = {0};
    uint32_t value_reps = 0, extents = 0, layouts = 0, fields = 0, functions = 0;
    uint32_t slots = 0, instructions = 0, calls = 0, call_arguments = 0;
    uint32_t root_maps = 0, root_slots = 0, cleanups = 0, adapters = 0;
    uint32_t coroutines = 0, debug_facts = 0;
    for (uint32_t i = 0; i < 2u; i++) {
        const XrSemanticProgramProvenance *program =
            xr_semantic_plan_program_provenance(modules[i].semantic);
        partitions[i] = (XrTargetModulePartitionRecord) {
            .module_identity = program->program_module,
            .semantic_fingerprint = xr_semantic_plan_fingerprint(modules[i].semantic),
            .program_module_row = i,
            .semantic_module = modules[i].semantic_module,
            .value_reps_begin = value_reps,
            .value_reps_count = modules[i].target.value_rep_count,
            .extents_begin = extents,
            .extents_count = modules[i].target.extent_count,
            .layouts_begin = layouts,
            .layouts_count = modules[i].target.layout_count,
            .fields_begin = fields,
            .fields_count = modules[i].target.field_count,
            .functions_begin = functions,
            .functions_count = modules[i].target.function_count,
            .slots_begin = slots,
            .slots_count = modules[i].target.slot_count,
            .instructions_begin = instructions,
            .instructions_count = modules[i].target.instruction_count,
            .calls_begin = calls,
            .calls_count = modules[i].target.call_count,
            .call_arguments_begin = call_arguments,
            .call_arguments_count = modules[i].target.call_argument_count,
            .root_maps_begin = root_maps,
            .root_maps_count = modules[i].target.root_map_count,
            .root_slots_begin = root_slots,
            .root_slots_count = modules[i].target.root_slot_count,
            .cleanups_begin = cleanups,
            .cleanups_count = modules[i].target.cleanup_count,
            .adapters_begin = adapters,
            .adapters_count = modules[i].target.adapter_count,
            .coroutines_begin = coroutines,
            .coroutines_count = modules[i].target.coroutine_count,
            .debug_facts_begin = debug_facts,
            .debug_facts_count = modules[i].target.debug_fact_count,
        };
        value_reps += modules[i].target.value_rep_count;
        extents += modules[i].target.extent_count;
        layouts += modules[i].target.layout_count;
        fields += modules[i].target.field_count;
        functions += modules[i].target.function_count;
        slots += modules[i].target.slot_count;
        instructions += modules[i].target.instruction_count;
        calls += modules[i].target.call_count;
        call_arguments += modules[i].target.call_argument_count;
        root_maps += modules[i].target.root_map_count;
        root_slots += modules[i].target.root_slot_count;
        cleanups += modules[i].target.cleanup_count;
        adapters += modules[i].target.adapter_count;
        coroutines += modules[i].target.coroutine_count;
        debug_facts += modules[i].target.debug_fact_count;
        graph_merge_module(&merged, &modules[i].target, &partitions[i], rep_maps[i]);
    }

    XrTargetProgramGraphRecord graph = {0};
    built = graph_bind_direct_call(
        entry, producer, &partitions[entry_program->program_module_row],
        &partitions[producer_program->program_module_row], profile, &merged, &graph, error,
        error_size);
    if (built) {
        XrTargetPlanDraft draft = {
            .semantic_plan = entry,
            .semantic_dependencies = entry_dependencies,
            .semantic_dependency_count = entry_dependency_count,
            .semantic_modules = semantic_modules,
            .semantic_module_count = semantic_module_count,
            .profile = profile,
            .completed_family_mask = XR_TARGET_REQUIRED_FAMILIES,
            .machine_reps = merged.machine_reps,
            .machine_reps_count = merged.machine_rep_count,
            .value_reps = merged.value_reps,
            .value_reps_count = merged.value_rep_count,
            .extents = merged.extents,
            .extents_count = merged.extent_count,
            .layouts = merged.layouts,
            .layouts_count = merged.layout_count,
            .fields = merged.fields,
            .fields_count = merged.field_count,
            .functions = merged.functions,
            .functions_count = merged.function_count,
            .slots = merged.slots,
            .slots_count = merged.slot_count,
            .instructions = merged.instructions,
            .instructions_count = merged.instruction_count,
            .calls = merged.calls,
            .calls_count = merged.call_count,
            .call_arguments = merged.call_arguments,
            .call_arguments_count = merged.call_argument_count,
            .root_maps = merged.root_maps,
            .root_maps_count = merged.root_map_count,
            .root_slots = merged.root_slots,
            .root_slots_count = merged.root_slot_count,
            .cleanups = merged.cleanups,
            .cleanups_count = merged.cleanup_count,
            .adapters = merged.adapters,
            .adapters_count = merged.adapter_count,
            .capabilities = merged.capabilities,
            .capabilities_count = merged.capability_count,
            .coroutines = merged.coroutines,
            .coroutines_count = merged.coroutine_count,
            .debug_facts = merged.debug_facts,
            .debug_facts_count = merged.debug_fact_count,
            .program_graphs = &graph,
            .program_graphs_count = 1u,
            .module_partitions = partitions,
            .module_partitions_count = 2u,
        };
        built = xr_target_plan_freeze(&draft, out, error, error_size);
    }
    materialized_dispose(&merged);

done:
    xr_free(entry_dependencies);
    xr_free(producer_dependencies);
    materialized_dispose(&modules[0].target);
    materialized_dispose(&modules[1].target);
    return built;
}

bool xr_target_plan_build(const XrSemanticPlan *semantic_plan, XrTargetProfile *profile,
                          XrTargetPlan **out, char *error, size_t error_size) {
    return xr_target_plan_build_module_set(semantic_plan, NULL, 0, profile, out, error, error_size);
}

bool xr_target_plan_build_module_set(const XrSemanticPlan *semantic_plan,
                                     const XrSemanticPlan *const *dependencies,
                                     uint32_t dependency_count, XrTargetProfile *profile,
                                     XrTargetPlan **out, char *error, size_t error_size) {
    XrTargetPlanBuilder *builder = NULL;
    if (out)
        *out = NULL;
    if (!builder_new(semantic_plan, profile, dependencies, dependency_count, false, &builder,
                     error, error_size))
        return false;
    if (!builder_collect_families(builder, error, error_size)) {
        builder_free(builder);
        return false;
    }
    bool frozen = builder_freeze(builder, out, error, error_size);
    builder_free(builder);
    return frozen;
}
