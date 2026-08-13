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

#include "xr_target_builder.h"
#include "xr_target_plan_internal.h"
#include "xr_target_profile_internal.h"
#include "../../base/xmalloc.h"
#include "../../frontend/analyzer/xa_intrinsic_registry.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../semantic/xr_semantic_graph.h"
#include "../semantic/xr_semantic_verify.h"
#include "../../runtime/value/xtype.h"
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
    bool suspends;
    bool tail;
} XrTargetCallIntent;

typedef struct XrTargetCallArgumentIntent {
    XrStableId identity;
    uint32_t call_intent;
    uint32_t semantic_operand;
    uint32_t semantic_value;
    uint32_t callee_parameter;
    uint16_t ordinal;
    uint8_t mode;
    uint8_t ownership;
    uint8_t transfer_mode;
    uint8_t flags;
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

typedef struct XrDirectLocalGoStoreEntry {
    uint32_t function;
    uint32_t operation;
    uint16_t slot;
    uint8_t occupied;
    uint8_t ambiguous;
} XrDirectLocalGoStoreEntry;

typedef struct XrDirectLocalGoCalleeStorageAnalysis {
    XrDirectLocalGoStoreEntry *stores;
    uint32_t store_capacity;
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
    XrTargetAdapterRecord *adapters;
    uint32_t adapter_count;
    XrTargetCapabilityRecord *capabilities;
    uint32_t capability_count;
    XrTargetCoroutineStateRecord *coroutines;
    uint32_t coroutine_count;
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

static void *allocate_records(uint32_t count, size_t size) {
    if (!count)
        return NULL;
    if (size && count > SIZE_MAX / size)
        return NULL;
    return xr_calloc(count, size);
}

static bool reserve_records(void **records, uint32_t *capacity, uint32_t required,
                            uint32_t limit, size_t record_size) {
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
    xr_free(materialized->adapters);
    xr_free(materialized->capabilities);
    xr_free(materialized->coroutines);
    memset(materialized, 0, sizeof(*materialized));
}

static XrTargetScalarEligibility classify_scalar_type(const XrSemanticTypeRecord *type,
                                                      uint16_t *out_kind) {
    if (!type || !out_kind)
        return XR_TARGET_SCALAR_INVALID;
    if ((type->flags & XR_SEM_TYPE_NULLABLE) != 0)
        return XR_TARGET_SCALAR_NOT_APPLICABLE;
    switch (type->kind) {
        case XR_KIND_INT:
            switch (type->scalar_rep) {
                case XR_NATIVE_I8: *out_kind = XR_MACHINE_REP_I8; break;
                case XR_NATIVE_U8: *out_kind = XR_MACHINE_REP_U8; break;
                case XR_NATIVE_I16: *out_kind = XR_MACHINE_REP_I16; break;
                case XR_NATIVE_U16: *out_kind = XR_MACHINE_REP_U16; break;
                case XR_NATIVE_I32: *out_kind = XR_MACHINE_REP_I32; break;
                case XR_NATIVE_U32: *out_kind = XR_MACHINE_REP_U32; break;
                case XR_NATIVE_I64: *out_kind = XR_MACHINE_REP_I64; break;
                case XR_NATIVE_U64: *out_kind = XR_MACHINE_REP_U64; break;
                case XR_NATIVE_ISIZE: *out_kind = XR_MACHINE_REP_ISIZE; break;
                case XR_NATIVE_USIZE: *out_kind = XR_MACHINE_REP_USIZE; break;
                default: return XR_TARGET_SCALAR_INVALID;
            }
            return XR_TARGET_SCALAR_VALUE;
        case XR_KIND_FLOAT:
            if (type->scalar_rep == XR_NATIVE_F32)
                *out_kind = XR_MACHINE_REP_F32;
            else if (type->scalar_rep == XR_NATIVE_F64)
                *out_kind = XR_MACHINE_REP_F64;
            else
                return XR_TARGET_SCALAR_INVALID;
            return XR_TARGET_SCALAR_VALUE;
        case XR_KIND_BOOL:
            if (type->scalar_rep != XR_SCALAR_REP_NONE)
                return XR_TARGET_SCALAR_INVALID;
            *out_kind = XR_MACHINE_REP_I1;
            return XR_TARGET_SCALAR_VALUE;
        case XR_KIND_RUNE:
            if (type->scalar_rep != XR_SCALAR_REP_NONE)
                return XR_TARGET_SCALAR_INVALID;
            *out_kind = XR_MACHINE_REP_RUNE;
            return XR_TARGET_SCALAR_VALUE;
        case XR_KIND_UNIT:
        case XR_KIND_NEVER:
            if (type->scalar_rep != XR_SCALAR_REP_NONE)
                return XR_TARGET_SCALAR_INVALID;
            *out_kind = XR_MACHINE_REP_VOID;
            return XR_TARGET_SCALAR_VALUE;
        default:
            return XR_TARGET_SCALAR_NOT_APPLICABLE;
    }
}

static bool checked_align_u32(uint32_t value, uint32_t alignment, uint32_t *out) {
    if (!alignment || (alignment & (alignment - 1u)) != 0 ||
        value > UINT32_MAX - alignment + 1u)
        return false;
    *out = (value + alignment - 1u) & ~(alignment - 1u);
    return true;
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
    if (!rep_layout_for_kind(profile, kind, &layout, &register_bits, &signedness) ||
        !layout.size || layout.size > UINT32_MAX || !layout.align || layout.align > UINT16_MAX)
        return false;
    out->register_bits = register_bits;
    out->memory_size = (uint32_t) layout.size;
    out->memory_align = (uint16_t) layout.align;
    out->signedness = signedness;
    out->ownership = XR_TARGET_OWNERSHIP_TRIVIAL;
    return true;
}

static bool semantic_unit_enum_type_is_exact(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    return type && type->kind == XR_KIND_ENUM && type->source_enum_key &&
           !xr_stable_id_equal(type->source_enum_identity, zero) &&
           type->enum_layout_id != 0 && type->enum_member_count != 0 &&
           type->enum_flags == (XR_SEM_ENUM_DECLARATION_EXACT | XR_SEM_ENUM_UNIT) &&
           type->reserved_enum == 0 && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           (type->flags & XR_SEM_TYPE_NULLABLE) == 0;
}

static bool make_unit_enum_rep(const XrTargetPlanBuilder *builder,
                               uint32_t semantic_type,
                               XrTargetMachineRepRecord *out) {
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(builder ? builder->semantic_plan : NULL, semantic_type);
    if (!semantic_unit_enum_type_is_exact(type) ||
        !make_machine_rep(xr_target_profile_machine_facts(builder->profile),
                          XR_MACHINE_REP_ENUM_ORDINAL, out))
        return false;
    out->detail = semantic_type;
    return true;
}

static bool make_dynamic_value_rep(const XrTargetMachineFacts *profile,
                                   XrTargetMachineRepRecord *out) {
    if (!profile || !out || !profile->data_layout.xr_value.size ||
        profile->data_layout.xr_value.size > UINT16_MAX / 8u ||
        !profile->data_layout.xr_value.align ||
        profile->data_layout.xr_value.align > UINT16_MAX)
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
#define XR_COMPARE_REP_FIELD(field)                                                               \
    do {                                                                                          \
        if (left->field < right->field) return -1;                                                \
        if (left->field > right->field) return 1;                                                 \
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
        if (left->legal_conversion_mask[i] < right->legal_conversion_mask[i]) return -1;
        if (left->legal_conversion_mask[i] > right->legal_conversion_mask[i]) return 1;
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
    if (a->semantic_type < b->semantic_type) return -1;
    if (a->semantic_type > b->semantic_type) return 1;
    return 0;
}

static int compare_value_intent(const void *left, const void *right) {
    const XrTargetValueIntent *a = (const XrTargetValueIntent *) left;
    const XrTargetValueIntent *b = (const XrTargetValueIntent *) right;
    if (a->semantic_value < b->semantic_value) return -1;
    if (a->semantic_value > b->semantic_value) return 1;
    return 0;
}

static int compare_slot_intent(const void *left, const void *right) {
    const XrTargetSlotIntent *a = (const XrTargetSlotIntent *) left;
    const XrTargetSlotIntent *b = (const XrTargetSlotIntent *) right;
    if (a->function < b->function) return -1;
    if (a->function > b->function) return 1;
    return xr_stable_id_compare(a->identity, b->identity);
}

static bool append_rep_intent(XrTargetPlanBuilder *builder,
                              const XrTargetMachineRepRecord *record, char *error,
                              size_t error_size) {
    for (uint32_t i = 0; i < builder->rep_intent_count; i++)
        if (compare_rep_record(&builder->rep_intents[i].record, record) == 0)
            return true;
    if (!reserve_records((void **) &builder->rep_intents, &builder->rep_intent_capacity,
                         builder->rep_intent_count + 1u, 256u,
                         sizeof(*builder->rep_intents)))
        return fail(error, error_size, "XR_EXEC_5003", "machine representation intent budget exhausted");
    builder->rep_intents[builder->rep_intent_count++].record = *record;
    return true;
}

static bool append_layout_intent(XrTargetPlanBuilder *builder, uint32_t semantic_type,
                                 uint8_t kind, uint32_t element_count,
                                 const XrTargetMachineRepRecord *memory_rep, char *error,
                                 size_t error_size) {
    for (uint32_t i = 0; i < builder->layout_intent_count; i++) {
        XrTargetLayoutIntent *existing = &builder->layout_intents[i];
        if (existing->semantic_type != semantic_type)
            continue;
        bool same_dynamic_geometry =
            kind == XR_TARGET_LAYOUT_DYNAMIC &&
            existing->kind == XR_TARGET_LAYOUT_DYNAMIC &&
            existing->memory_rep.kind == XR_MACHINE_REP_DYN_VALUE &&
            memory_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
            existing->memory_rep.memory_size == memory_rep->memory_size &&
            existing->memory_rep.memory_align == memory_rep->memory_align;
        if (existing->kind == kind && existing->element_count == element_count &&
            (compare_rep_record(&existing->memory_rep, memory_rep) == 0 ||
             same_dynamic_geometry))
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
    return true;
}

static bool append_value_intent(XrTargetPlanBuilder *builder,
                                const XrTargetValueIntent *intent, char *error,
                                size_t error_size) {
    if (!reserve_records((void **) &builder->value_intents, &builder->value_intent_capacity,
                         builder->value_intent_count + 1u, 40000000u,
                         sizeof(*builder->value_intents)))
        return fail(error, error_size, "XR_EXEC_5003", "value representation intent budget exhausted");
    builder->value_intents[builder->value_intent_count++] = *intent;
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

static bool append_call_intent(XrTargetPlanBuilder *builder,
                               const XrTargetCallIntent *intent, char *error,
                               size_t error_size) {
    if (!reserve_records((void **) &builder->call_intents,
                         &builder->call_intent_capacity,
                         builder->call_intent_count + 1u, 10000000u,
                         sizeof(*builder->call_intents)))
        return fail(error, error_size, "XR_EXEC_5003", "call intent budget exhausted");
    builder->call_intents[builder->call_intent_count++] = *intent;
    return true;
}

static bool append_call_argument_intent(
    XrTargetPlanBuilder *builder, const XrTargetCallArgumentIntent *intent,
    char *error, size_t error_size) {
    if (!reserve_records((void **) &builder->call_argument_intents,
                         &builder->call_argument_intent_capacity,
                         builder->call_argument_intent_count + 1u, 40000000u,
                         sizeof(*builder->call_argument_intents)))
        return fail(error, error_size, "XR_EXEC_5003",
                    "call argument intent budget exhausted");
    builder->call_argument_intents[builder->call_argument_intent_count++] = *intent;
    return true;
}

static bool stable_identity_from_pair(const char *domain, XrStableId first,
                                      XrStableId second, uint32_t ordinal,
                                      XrStableId *out) {
    char first_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char second_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    XrFingerprint digest;
    xr_stable_id_hex(first, first_hex);
    xr_stable_id_hex(second, second_hex);
    int written = snprintf(key, sizeof(key), "%s:first=%s:second=%s:ordinal=%u",
                           domain, first_hex, second_hex, ordinal);
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
    int written = snprintf(key, sizeof(key),
                           "xray-target-slot-v2:function=%s:role=%u:source=%s:logical=%u",
                           function_id, (unsigned) role, source_id, logical_slot);
    XrFingerprint digest;
    return written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static bool value_storage_analysis_init(
    const XrSemanticPlan *plan, XrTargetValueStorageAnalysis *analysis,
    char *error, size_t error_size) {
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
            return fail(error, error_size, "XR_EXEC_5003", "semantic value identity budget overflow");
        total_values = last->value_begin + last->value_count;
    }
    if (total_values > 40000000u)
        return fail(error, error_size, "XR_EXEC_5003",
                    "target value-storage budget exhausted");
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
            return fail(error, error_size, "XR_TARGET_1001", "semantic value has ambiguous defining operations");
        analysis->value_operations[operation->result_value] = i;
    }
    return true;
}

static bool semantic_allocation_identity_is_canonical(
    const XrSemanticOperationRecord *operation) {
    static const char suffix[] = "/allocation";
    if (!operation || !operation->canonical_key || !operation->allocation_key)
        return false;
    size_t canonical_length = strlen(operation->canonical_key);
    size_t allocation_length = strlen(operation->allocation_key);
    if (canonical_length > SIZE_MAX - sizeof(suffix) ||
        allocation_length != canonical_length + sizeof(suffix) - 1u ||
        memcmp(operation->allocation_key, operation->canonical_key,
               canonical_length) != 0 ||
        memcmp(operation->allocation_key + canonical_length, suffix,
               sizeof(suffix)) != 0)
        return false;
    XrStableId expected;
    XrFingerprint digest;
    return xr_stable_id_from_key(operation->allocation_key, &expected, &digest) &&
           xr_stable_id_equal(expected, operation->allocation_id);
}

static bool semantic_heap_closure_is_exact(const XrSemanticPlan *plan,
                                           const XrSemanticOperationRecord *operation) {
    if (!plan || !operation || operation->opcode != XI_CLOSURE_NEW ||
        operation->callable_function >= xr_semantic_plan_function_count(plan) ||
        operation->operand_count != 0 ||
        !semantic_allocation_identity_is_canonical(operation) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
        return false;
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(plan, operation->callable_function);
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(plan, operation->result_type);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    bool typed_function = type && type->kind == XR_KIND_FUNCTION;
    bool opaque_closure = type && type->kind == XR_KIND_UNKNOWN &&
                          type->child_count == 0;
    if (!callee || !type || callee->parent != operation->function ||
        callee->capture_count != 0 || (!typed_function && !opaque_closure) ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE |
                        XR_SEM_TYPE_BORROW_VIEW | XR_SEM_TYPE_AGGREGATE_EXACT)) != 0 ||
        (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE |
                        XR_SEM_TYPE_OWNERSHIP_ROOT)) !=
            (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        callee->parameter_count == UINT16_MAX ||
        (typed_function &&
         type->child_count != (uint32_t) callee->parameter_count + 1u) ||
        type->child_begin > child_count ||
        type->child_count > child_count - type->child_begin ||
        callee->parameter_begin > xr_semantic_plan_parameter_count(plan) ||
        callee->parameter_count > xr_semantic_plan_parameter_count(plan) -
                                      callee->parameter_begin)
        return false;
    for (uint32_t i = 0; i < callee->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan, callee->parameter_begin + i);
        if (!parameter || parameter->function != operation->callable_function ||
            parameter->ordinal != i ||
            (typed_function &&
             children[type->child_begin + i] != parameter->type))
            return false;
    }
    return opaque_closure ||
           children[type->child_begin + callee->parameter_count] ==
               callee->return_type;
}

static bool semantic_string_literal_is_exact(
    const XrSemanticPlan *plan,
    const XrSemanticOperationRecord *operation) {
    if (!plan || !operation || operation->opcode != XI_CONST ||
        operation->operand_count != 0 ||
        operation->constant >= xr_semantic_plan_constant_count(plan) ||
        operation->allocation_key != NULL ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_complete != 1)
        return false;
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++) {
        if (operation->allocation_id.bytes[i] != 0)
            return false;
    }
    const XrSemanticConstantRecord *constant =
        xr_semantic_plan_constant(plan, operation->constant);
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(plan, operation->result_type);
    return constant && constant->kind == XR_SEM_CONST_STRING &&
           constant->string && constant->type == operation->result_type &&
           type && type->kind == XR_KIND_STRING && type->child_count == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE |
                           XR_SEM_TYPE_BORROW_VIEW |
                           XR_SEM_TYPE_AGGREGATE_EXACT)) == 0 &&
           (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE |
                           XR_SEM_TYPE_OWNERSHIP_ROOT)) ==
               (XR_SEM_TYPE_REFERENCE_CAPABLE |
                XR_SEM_TYPE_OWNERSHIP_ROOT);
}

static bool semantic_stringbuilder_type_is_exact(const XrSemanticTypeRecord *type) {
    char expected_type_key[160];
    int written = snprintf(
        expected_type_key, sizeof(expected_type_key),
        "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:13:StringBuilder[0]",
        (unsigned) XR_KIND_INSTANCE, (unsigned) XR_TID_STRINGBUILDER,
        (unsigned) XR_SCALAR_REP_NONE);
    return type && written > 0 && (size_t) written < sizeof(expected_type_key) &&
           type->kind == XR_KIND_INSTANCE && type->builtin_type == XR_TID_STRINGBUILDER &&
           type->child_count == 0 && type->aggregate_extent == 0 &&
           type->aggregate_align == 0 && type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->canonical_key && strcmp(type->canonical_key, expected_type_key) == 0;
}

static bool semantic_stringbuilder_constructor_is_exact(
    const XrSemanticPlan *plan,
    const XrSemanticOperationRecord *operation) {
    uint32_t metadata_count = 0;
    const char *const *metadata =
        xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || operation->opcode != XI_CALL_BUILTIN ||
        operation->operand_count != 0 || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count || !metadata ||
        strcmp(metadata[operation->metadata_begin], "StringBuilder") != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->semantic_immediate != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_BUILTIN) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_BUILTIN) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_BUILTIN) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_parameter != -1 || operation->return_complete != 1 ||
        !semantic_allocation_identity_is_canonical(operation))
        return false;
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(plan, operation->result_type);
    return semantic_stringbuilder_type_is_exact(type);
}

static bool semantic_stringbuilder_append_rune_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *receiver_value, uint32_t *argument_value) {
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
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
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

static bool semantic_stringbuilder_to_string_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
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
    if (receiver_value) *receiver_value = receiver->value;
    return true;
}

static bool semantic_stringbuilder_append_string_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *receiver_value, uint32_t *argument_value) {
    uint32_t operands_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operands_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_STRING ||
        operation->operand_count != 2 || operation->operand_begin + 1u >= operands_count ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "append") != 0 ||
        operation->result_alias_operand != 0 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
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
    if (receiver_value) *receiver_value = receiver->value;
    if (argument_value) *argument_value = argument->value;
    return true;
}

static bool semantic_json_namespace_type_is_exact(const XrSemanticTypeRecord *type) {
    char expected_type_key[160];
    int written = snprintf(expected_type_key, sizeof(expected_type_key),
                           "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:4:JSON[0]",
                           (unsigned) XR_KIND_CLASS, (unsigned) XR_TID_NULL,
                           (unsigned) XR_SCALAR_REP_NONE);
    XrStableId zero = {{0}};
    return type && written > 0 && (size_t) written < sizeof(expected_type_key) &&
           type->kind == XR_KIND_CLASS && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
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

static bool semantic_channel_type_is_exact(const XrSemanticPlan *plan,
                                           uint32_t type_index,
                                           uint32_t *element_type) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE |
                       XR_SEM_TYPE_OWNERSHIP_ROOT;
    uint8_t allowed = required | XR_SEM_TYPE_CONST;
    if (!type || type->kind != XR_KIND_CHANNEL ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 1 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        (type->flags & required) != required ||
        (type->flags & ~allowed) != 0 || type->child_begin >= child_count ||
        children[type->child_begin] >= xr_semantic_plan_type_count(plan))
        return false;
    if (element_type)
        *element_type = children[type->child_begin];
    return true;
}

static bool semantic_channel_capacity_type_is_exact(
    const XrSemanticPlan *plan, uint32_t type_index) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    uint16_t kind = XR_MACHINE_REP_COUNT;
    if (!type || type->kind != XR_KIND_INT ||
        classify_scalar_type(type, &kind) != XR_TARGET_SCALAR_VALUE)
        return false;
    return kind >= XR_MACHINE_REP_I8 && kind <= XR_MACHINE_REP_USIZE;
}

static bool semantic_channel_allocation_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    if (!plan || !operation)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    uint32_t element_type = XR_SEMANTIC_INDEX_NONE;
    if (operation->opcode != XI_CHAN_NEW ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->operand_count != 1 ||
        operation->operand_begin >= operand_count ||
        !semantic_allocation_identity_is_canonical(operation) ||
        !semantic_channel_type_is_exact(plan, operation->result_type,
                                        &element_type) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != 0 ||
        operation->effects != xi_generated_op_effects(XI_CHAN_NEW) ||
        operation->flags != xi_generated_op_default_flags(XI_CHAN_NEW) ||
        operation->result_ownership !=
            xi_generated_op_result_ownership(XI_CHAN_NEW) ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_parameter != -1 || operation->return_complete != 1)
        return false;
    const XrSemanticOperandRecord *capacity =
        &operands[operation->operand_begin];
    return capacity->value != XR_SEMANTIC_INDEX_NONE &&
           capacity->type < xr_semantic_plan_type_count(plan) &&
           capacity->role == XR_SEM_OPERAND_VALUE && capacity->parameter == -1 &&
           capacity->flags == 0 &&
           semantic_channel_capacity_type_is_exact(plan, capacity->type) &&
           element_type < xr_semantic_plan_type_count(plan);
}

static bool semantic_channel_identity_copy_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const uint8_t *exact_channel_values, uint32_t value_count) {
    if (!plan || !operation || !exact_channel_values)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    if (operation->opcode != XI_COPY || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count ||
        operation->semantic_immediate != XI_COPY_KIND_IDENTITY ||
        operation->allocation_key != NULL ||
        !stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->effects != xi_generated_op_effects(XI_COPY) ||
        operation->flags != xi_generated_op_default_flags(XI_COPY) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_alias_operand != 0 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_parameter != -1 || operation->return_complete != 1)
        return false;
    const XrSemanticOperandRecord *source =
        &operands[operation->operand_begin];
    uint32_t source_element = XR_SEMANTIC_INDEX_NONE;
    uint32_t result_element = XR_SEMANTIC_INDEX_NONE;
    return source->value < value_count && exact_channel_values[source->value] &&
           source->role == XR_SEM_OPERAND_VALUE && source->parameter == -1 &&
           source->flags == 0 &&
           semantic_channel_type_is_exact(plan, source->type,
                                          &source_element) &&
           semantic_channel_type_is_exact(plan, operation->result_type,
                                          &result_element) &&
           source_element == result_element;
}

/* CHANNEL_RECEIVE_STORAGE owns the boundary between the runtime's tagged
 * receive payload and the exact machine slot selected for Channel<T>'s T.
 * This predicate deliberately accepts only channels already proven by the
 * channel-allocation family; a nominal Channel type is not allocation or
 * lifetime authority. */
static bool semantic_channel_receive_storage_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const uint8_t *exact_channel_values, uint32_t value_count) {
    if (!plan || !operation || !exact_channel_values ||
        operation->opcode != XI_CHAN_TRY_RECV || operation->operand_count != 1 ||
        operation->result_value >= value_count || operation->allocation_key ||
        !stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != 0 || operation->semantic_immediate != 0 ||
        operation->effects != xi_generated_op_effects(XI_CHAN_TRY_RECV) ||
        operation->flags != xi_generated_op_default_flags(XI_CHAN_TRY_RECV) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CHAN_TRY_RECV) ||
        operation->result_ownership !=
            xi_generated_op_result_ownership(XI_CHAN_TRY_RECV) ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_parameter != -1 || operation->return_complete != 1)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    if (operation->operand_begin >= operand_count)
        return false;
    const XrSemanticOperandRecord *receiver =
        &operands[operation->operand_begin];
    uint32_t element_type = XR_SEMANTIC_INDEX_NONE;
    uint16_t result_kind = XR_MACHINE_REP_COUNT;
    return receiver->value < value_count &&
           exact_channel_values[receiver->value] != 0 &&
           receiver->role == XR_SEM_OPERAND_VALUE && receiver->parameter == -1 &&
           receiver->transfer_mode == XR_TRANSFER_SHARE &&
           receiver->ownership_action == XR_SEM_OPERAND_BORROW &&
           receiver->parameter_mode == XR_PARAM_READ &&
           receiver->access == XR_CALL_ARG_PLAIN &&
           receiver->origin == XI_PLACE_ORIGIN_NONE &&
           receiver->lifetime == XI_PLACE_LIFETIME_NONE &&
           receiver->escape == XI_PLACE_ESCAPE_NONE && receiver->flags == 0 &&
           semantic_channel_type_is_exact(plan, receiver->type, &element_type) &&
           element_type == operation->result_type &&
           classify_scalar_type(
               xr_semantic_plan_type(plan, operation->result_type),
               &result_kind) == XR_TARGET_SCALAR_VALUE &&
           result_kind != XR_MACHINE_REP_VOID;
}

static void direct_local_callee_storage_analysis_dispose(
    XrDirectLocalCalleeStorageAnalysis *analysis) {
    if (!analysis)
        return;
    xr_free(analysis->target_by_operation);
    xr_free(analysis->target_by_value);
    xr_free(analysis->use_count_by_value);
    xr_free(analysis->invalid_value);
    memset(analysis, 0, sizeof(*analysis));
}

static bool semantic_direct_local_callee_type_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t target_function) {
    if (!plan || !operation || operation->opcode != XI_GET_SHARED ||
        operation->semantic_immediate < 0 ||
        operation->semantic_immediate > UINT16_MAX ||
        operation->operand_count != 0 || operation->allocation_key ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_ownership !=
            xi_generated_op_result_ownership(XI_GET_SHARED) ||
        operation->effects != xi_generated_op_effects(XI_GET_SHARED) ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_complete != 1 || operation->return_parameter != -1 ||
        target_function >= xr_semantic_plan_function_count(plan))
        return false;
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++)
        if (operation->allocation_id.bytes[i] != 0)
            return false;
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(plan, operation->result_type);
    const XrSemanticFunctionRecord *target =
        xr_semantic_plan_function(plan, target_function);
    uint32_t lexical_owner = target ? target->parent : XR_SEMANTIC_INDEX_NONE;
    uint32_t caller_ancestor = operation->function;
    for (uint32_t depth = 0;
         caller_ancestor != XR_SEMANTIC_INDEX_NONE &&
         caller_ancestor != lexical_owner &&
         depth < xr_semantic_plan_function_count(plan);
         depth++) {
        const XrSemanticFunctionRecord *ancestor =
            xr_semantic_plan_function(plan, caller_ancestor);
        caller_ancestor = ancestor ? ancestor->parent : XR_SEMANTIC_INDEX_NONE;
    }
    if (!type || !target || lexical_owner == XR_SEMANTIC_INDEX_NONE ||
        caller_ancestor != lexical_owner ||
        (type->kind != XR_KIND_FUNCTION &&
         type->kind != XR_KIND_UNKNOWN) ||
        type->scalar_rep != XR_SCALAR_REP_NONE ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->child_count != 0 ||
        target->parameter_begin > xr_semantic_plan_parameter_count(plan) ||
        target->parameter_count > xr_semantic_plan_parameter_count(plan) -
                                      target->parameter_begin ||
        (type->flags & (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE |
                        XR_SEM_TYPE_BORROW_VIEW | XR_SEM_TYPE_AGGREGATE_EXACT)) != 0 ||
        (type->flags & (XR_SEM_TYPE_REFERENCE_CAPABLE |
                        XR_SEM_TYPE_OWNERSHIP_ROOT)) !=
            (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    return true;
}

static bool direct_local_callee_storage_analysis_init(
    const XrSemanticPlan *plan, const XrTargetValueStorageAnalysis *values,
    XrDirectLocalCalleeStorageAnalysis *analysis, char *error,
    size_t error_size) {
    if (!plan || !values || !analysis ||
        xr_semantic_plan_operation_count(plan) > UINT32_MAX ||
        xr_semantic_plan_call_target_count(plan) > UINT32_MAX)
        return fail(error, error_size, "XR_EXEC_5003",
                    "direct-local callee-storage budget is invalid");
    analysis->operation_count =
        (uint32_t) xr_semantic_plan_operation_count(plan);
    analysis->value_count = values->total_values;
    analysis->target_by_operation = (uint32_t *) allocate_records(
        analysis->operation_count, sizeof(*analysis->target_by_operation));
    analysis->target_by_value = (uint32_t *) allocate_records(
        analysis->value_count, sizeof(*analysis->target_by_value));
    analysis->use_count_by_value = (uint32_t *) allocate_records(
        analysis->value_count, sizeof(*analysis->use_count_by_value));
    analysis->invalid_value = (uint8_t *) allocate_records(
        analysis->value_count, sizeof(*analysis->invalid_value));
    if ((analysis->operation_count && !analysis->target_by_operation) ||
        (analysis->value_count && (!analysis->target_by_value ||
                                   !analysis->use_count_by_value ||
                                   !analysis->invalid_value))) {
        direct_local_callee_storage_analysis_dispose(analysis);
        return fail(error, error_size, "XR_EXEC_5003",
                    "direct-local callee-storage allocation failed");
    }
    for (uint32_t i = 0; i < analysis->operation_count; i++)
        analysis->target_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < analysis->value_count; i++)
        analysis->target_by_value[i] = XR_SEMANTIC_INDEX_NONE;
    uint32_t target_count =
        (uint32_t) xr_semantic_plan_call_target_count(plan);
    for (uint32_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(plan, i);
        if (target && target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL)
            continue;
        if (!target || target->operation >= analysis->operation_count ||
            analysis->target_by_operation[target->operation] !=
                XR_SEMANTIC_INDEX_NONE) {
            direct_local_callee_storage_analysis_dispose(analysis);
            return fail(error, error_size, "XR_TARGET_1001",
                        "direct-local callee target authority is ambiguous");
        }
        analysis->target_by_operation[target->operation] = i;
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    for (uint32_t i = 0; i < analysis->operation_count; i++) {
        const XrSemanticOperationRecord *use =
            xr_semantic_plan_operation(plan, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin) {
            direct_local_callee_storage_analysis_dispose(analysis);
            return fail(error, error_size, "XR_TARGET_1001",
                        "direct-local callee operand range is invalid");
        }
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand =
                &operands[use->operand_begin + a];
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
            bool exact = a == 0 &&
                         (use->opcode == XI_CALL || use->opcode == XI_TAIL_CALL) &&
                         use->function == source->function && target &&
                         target->operation == i &&
                         target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
                         operand->role == XR_SEM_OPERAND_CALLEE &&
                         operand->parameter == -1 &&
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
            source_index == XR_SEMANTIC_INDEX_NONE
                ? NULL
                : xr_semantic_plan_operation(plan, source_index);
        if (source && source->opcode == XI_GET_SHARED)
            analysis->invalid_value[block->control_value] = 1;
    }
    return true;
}

static bool direct_local_callee_storage_value_is_exact(
    const XrSemanticPlan *plan, const XrDirectLocalCalleeStorageAnalysis *analysis,
    const XrSemanticOperationRecord *operation) {
    if (!plan || !analysis || !operation ||
        operation->result_value >= analysis->value_count ||
        analysis->invalid_value[operation->result_value] != 0 ||
        analysis->use_count_by_value[operation->result_value] == 0 ||
        analysis->target_by_value[operation->result_value] ==
            XR_SEMANTIC_INDEX_NONE)
        return false;
    return semantic_direct_local_callee_type_is_exact(
        plan, operation,
        analysis->target_by_value[operation->result_value]);
}

static void direct_local_go_callee_storage_analysis_dispose(
    XrDirectLocalGoCalleeStorageAnalysis *analysis) {
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

static uint32_t direct_local_go_store_hash(uint32_t function, uint16_t slot) {
    uint32_t mixed = function * UINT32_C(2654435761) ^ (uint32_t) slot;
    mixed ^= mixed >> 16;
    return mixed;
}

static XrDirectLocalGoStoreEntry *direct_local_go_find_store(
    XrDirectLocalGoCalleeStorageAnalysis *analysis, uint32_t function,
    uint16_t slot, bool insert) {
    if (!analysis || !analysis->stores || !analysis->store_capacity)
        return NULL;
    uint32_t mask = analysis->store_capacity - 1u;
    uint32_t cursor = direct_local_go_store_hash(function, slot) & mask;
    for (uint32_t probe = 0; probe < analysis->store_capacity; probe++) {
        XrDirectLocalGoStoreEntry *entry = &analysis->stores[cursor];
        if (!entry->occupied) {
            if (!insert)
                return NULL;
            entry->occupied = 1;
            entry->function = function;
            entry->slot = slot;
            entry->operation = XR_SEMANTIC_INDEX_NONE;
            return entry;
        }
        if (entry->function == function && entry->slot == slot)
            return entry;
        cursor = (cursor + 1u) & mask;
    }
    return NULL;
}

static bool direct_local_go_store_precedes_activation(
    const XrSemanticPlan *plan, uint32_t function_index, uint32_t store_index) {
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(plan, function_index);
    const XrSemanticOperationRecord *store =
        xr_semantic_plan_operation(plan, store_index);
    const XrSemanticBlockRecord *entry =
        function ? xr_semantic_plan_block(plan, function->block_begin) : NULL;
    if (!function || !store || !entry || entry->function != function_index ||
        store->block != function->block_begin ||
        store_index < entry->operation_begin ||
        store_index >= entry->operation_begin + entry->operation_count)
        return false;
    for (uint32_t i = entry->operation_begin; i < store_index; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan, i);
        if (!operation || operation->opcode == XI_CALL ||
            operation->opcode == XI_TAIL_CALL ||
            operation->opcode == XI_CALL_METHOD ||
            operation->opcode == XI_CALL_METHOD_DIRECT ||
            operation->opcode == XI_CALL_BUILTIN ||
            operation->opcode == XI_GO ||
            operation->opcode == XI_THREAD_SPAWN)
            return false;
    }
    return true;
}

static bool semantic_direct_local_go_store_target(
    const XrSemanticPlan *plan, const XrTargetValueStorageAnalysis *values,
    const XrSemanticOperationRecord *load, uint32_t load_index,
    const XrDirectLocalGoStoreEntry *entry, const XrSemanticGraph *graph,
    uint32_t *out_target) {
    if (out_target)
        *out_target = XR_SEMANTIC_INDEX_NONE;
    if (!plan || !values || !load || !entry || !out_target ||
        entry->ambiguous || entry->operation == XR_SEMANTIC_INDEX_NONE ||
        entry->operation >= load_index)
        return false;
    const XrSemanticOperationRecord *store =
        xr_semantic_plan_operation(plan, entry->operation);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    bool initialized = store && load &&
                       (store->block == load->block
                            ? entry->operation < load_index
                            : xr_semantic_graph_dominates(
                                  graph, store->block, load->block));
    if (!store || store->opcode != XI_SET_SHARED ||
        store->function != load->function || !initialized ||
        store->semantic_immediate != load->semantic_immediate ||
        store->operand_count != 1 || store->operand_begin >= operand_count ||
        store->allocation_key || !stable_id_is_zero(store->allocation_id) ||
        store->constant != XR_SEMANTIC_INDEX_NONE ||
        store->callable_function != XR_SEMANTIC_INDEX_NONE ||
        store->effects != xi_generated_op_effects(XI_SET_SHARED) ||
        store->result_ownership !=
            xi_generated_op_result_ownership(XI_SET_SHARED) ||
        !direct_local_go_store_precedes_activation(
            plan, store->function, entry->operation))
        return false;
    const XrSemanticOperandRecord *source = &operands[store->operand_begin];
    if (source->value >= values->total_values ||
        source->role != XR_SEM_OPERAND_VALUE || source->parameter != -1 ||
        source->transfer_mode != XR_TRANSFER_SHARE ||
        source->ownership_action != XR_SEM_OPERAND_CONSUME ||
        source->parameter_mode != XR_PARAM_READ ||
        source->access != XR_CALL_ARG_PLAIN ||
        source->origin != XI_PLACE_ORIGIN_NONE ||
        source->lifetime != XI_PLACE_LIFETIME_NONE ||
        source->escape != XI_PLACE_ESCAPE_NONE || source->flags != 0)
        return false;
    uint32_t producer_index = values->value_operations[source->value];
    const XrSemanticOperationRecord *producer =
        producer_index == XR_SEMANTIC_INDEX_NONE
            ? NULL
            : xr_semantic_plan_operation(plan, producer_index);
    if (!producer || producer_index >= entry->operation ||
        producer->result_value != source->value ||
        producer->result_type != source->type ||
        producer->function != store->function ||
        !semantic_heap_closure_is_exact(plan, producer))
        return false;
    *out_target = producer->callable_function;
    return true;
}

static bool semantic_direct_local_go_use_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *source,
    const XrSemanticOperationRecord *use, const XrSemanticOperandRecord *callee,
    uint16_t operand_index, uint32_t target_function) {
    const XrSemanticFunctionRecord *target =
        xr_semantic_plan_function(plan, target_function);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    if (!source || !use || !callee || !target || operand_index != 0 ||
        use->opcode != XI_GO || use->function != source->function ||
        use->operand_count != (uint16_t) (target->parameter_count + 1u) ||
        use->operand_begin > operand_count ||
        use->operand_count > operand_count - use->operand_begin ||
        use->allocation_key || !stable_id_is_zero(use->allocation_id) ||
        use->constant != XR_SEMANTIC_INDEX_NONE ||
        use->callable_function != XR_SEMANTIC_INDEX_NONE ||
        use->effects != xi_generated_op_effects(XI_GO) ||
        callee->value != source->result_value ||
        callee->type != source->result_type ||
        callee->role != XR_SEM_OPERAND_VALUE || callee->parameter != -1 ||
        callee->transfer_mode != XR_TRANSFER_SHARE ||
        callee->ownership_action != XR_SEM_OPERAND_BORROW ||
        callee->parameter_mode != XR_PARAM_READ ||
        callee->access != XR_CALL_ARG_PLAIN ||
        callee->origin != XI_PLACE_ORIGIN_NONE ||
        callee->lifetime != XI_PLACE_LIFETIME_NONE ||
        callee->escape != XI_PLACE_ESCAPE_NONE || callee->flags != 0)
        return false;
    for (uint16_t i = 1; i < use->operand_count; i++) {
        const XrSemanticOperandRecord *argument =
            &operands[use->operand_begin + i];
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(
            plan, target->parameter_begin + i - 1u);
        if (!parameter || argument->type != parameter->type ||
            argument->role != XR_SEM_OPERAND_VALUE ||
            argument->parameter != -1 ||
            argument->parameter_mode != XR_PARAM_READ ||
            argument->access != XR_CALL_ARG_PLAIN ||
            argument->origin != XI_PLACE_ORIGIN_NONE ||
            argument->lifetime != XI_PLACE_LIFETIME_NONE ||
            argument->escape != XI_PLACE_ESCAPE_NONE || argument->flags != 0)
            return false;
    }
    return semantic_direct_local_callee_type_is_exact(
        plan, source, target_function);
}

static bool direct_local_go_callee_storage_analysis_init(
    const XrSemanticPlan *plan, const XrTargetValueStorageAnalysis *values,
    XrDirectLocalGoCalleeStorageAnalysis *analysis, char *error,
    size_t error_size) {
    if (!plan || !values || !analysis ||
        xr_semantic_plan_operation_count(plan) > UINT32_MAX)
        return fail(error, error_size, "XR_EXEC_5003",
                    "direct-local go callee-storage budget is invalid");
    analysis->operation_count =
        (uint32_t) xr_semantic_plan_operation_count(plan);
    analysis->value_count = values->total_values;
    if (analysis->operation_count > (1u << 24))
        return fail(error, error_size, "XR_EXEC_5003",
                    "direct-local go shared-store budget is exhausted");
    uint32_t capacity = 1;
    while (capacity < analysis->operation_count * 2u)
        capacity <<= 1u;
    analysis->store_capacity = capacity;
    analysis->stores = (XrDirectLocalGoStoreEntry *) allocate_records(
        capacity, sizeof(*analysis->stores));
    analysis->target_by_value = (uint32_t *) allocate_records(
        analysis->value_count, sizeof(*analysis->target_by_value));
    analysis->use_count_by_value = (uint32_t *) allocate_records(
        analysis->value_count, sizeof(*analysis->use_count_by_value));
    analysis->candidate_value = (uint8_t *) allocate_records(
        analysis->value_count, sizeof(*analysis->candidate_value));
    analysis->invalid_value = (uint8_t *) allocate_records(
        analysis->value_count, sizeof(*analysis->invalid_value));
    if (!xr_semantic_graph_build(plan, &analysis->graph, error, error_size) ||
        !analysis->stores ||
        (analysis->value_count &&
         (!analysis->target_by_value || !analysis->use_count_by_value ||
          !analysis->candidate_value || !analysis->invalid_value))) {
        direct_local_go_callee_storage_analysis_dispose(analysis);
        return fail(error, error_size, "XR_EXEC_5003",
                    "direct-local go callee-storage allocation failed");
    }
    for (uint32_t i = 0; i < analysis->value_count; i++)
        analysis->target_by_value[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < analysis->operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan, i);
        if (!operation)
            goto invalid_authority;
        if (operation->opcode != XI_SET_SHARED ||
            operation->semantic_immediate < 0 ||
            operation->semantic_immediate > UINT16_MAX)
            continue;
        XrDirectLocalGoStoreEntry *entry = direct_local_go_find_store(
            analysis, operation->function,
            (uint16_t) operation->semantic_immediate, true);
        if (!entry)
            goto invalid_authority;
        if (entry->operation != XR_SEMANTIC_INDEX_NONE)
            entry->ambiguous = 1;
        else
            entry->operation = i;
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    for (uint32_t i = 0; i < analysis->operation_count; i++) {
        const XrSemanticOperationRecord *use =
            xr_semantic_plan_operation(plan, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            goto invalid_authority;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand =
                &operands[use->operand_begin + a];
            if (operand->value >= analysis->value_count)
                goto invalid_authority;
            uint32_t source_index = values->value_operations[operand->value];
            const XrSemanticOperationRecord *source =
                source_index == XR_SEMANTIC_INDEX_NONE
                    ? NULL
                    : xr_semantic_plan_operation(plan, source_index);
            if (!source || source->opcode != XI_GET_SHARED || use->opcode != XI_GO)
                continue;
            uint32_t value = source->result_value;
            analysis->candidate_value[value] = 1;
            uint32_t target = XR_SEMANTIC_INDEX_NONE;
            XrDirectLocalGoStoreEntry *entry =
                source->semantic_immediate >= 0 &&
                        source->semantic_immediate <= UINT16_MAX
                    ? direct_local_go_find_store(
                          analysis, source->function,
                          (uint16_t) source->semantic_immediate, false)
                    : NULL;
            bool exact = entry && semantic_direct_local_go_store_target(
                                      plan, values, source, source_index,
                                      entry, &analysis->graph, &target) &&
                         semantic_direct_local_go_use_is_exact(
                             plan, source, use, operand, a, target);
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
        const XrSemanticOperationRecord *use =
            xr_semantic_plan_operation(plan, i);
        for (uint16_t a = 0; use && a < use->operand_count; a++) {
            uint32_t value = operands[use->operand_begin + a].value;
            uint32_t source_index = value < analysis->value_count
                                        ? values->value_operations[value]
                                        : XR_SEMANTIC_INDEX_NONE;
            const XrSemanticOperationRecord *source =
                source_index == XR_SEMANTIC_INDEX_NONE
                    ? NULL
                    : xr_semantic_plan_operation(plan, source_index);
            if (source && source->opcode == XI_GET_SHARED &&
                analysis->candidate_value[value] &&
                (use->opcode != XI_GO || a != 0))
                analysis->invalid_value[value] = 1;
        }
    }
    for (uint32_t i = 0;
         i < (uint32_t) xr_semantic_plan_block_count(plan); i++) {
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

static bool direct_local_go_callee_storage_value_is_exact(
    const XrSemanticPlan *plan,
    const XrDirectLocalGoCalleeStorageAnalysis *analysis,
    const XrSemanticOperationRecord *operation) {
    return plan && analysis && operation && operation->opcode == XI_GET_SHARED &&
           operation->result_value < analysis->value_count &&
           analysis->candidate_value[operation->result_value] &&
           !analysis->invalid_value[operation->result_value] &&
           analysis->use_count_by_value[operation->result_value] != 0 &&
           analysis->target_by_value[operation->result_value] !=
               XR_SEMANTIC_INDEX_NONE &&
           semantic_direct_local_callee_type_is_exact(
               plan, operation,
               analysis->target_by_value[operation->result_value]);
}

static bool note_scalar_value(XrTargetPlanBuilder *builder,
                              XrTargetValueStorageAnalysis *analysis,
                              uint32_t semantic_value, uint32_t semantic_type,
                              uint32_t semantic_function, uint32_t semantic_operation,
                              uint8_t role, XrStableId source_identity, char *error,
                              size_t error_size) {
    if (semantic_value >= analysis->total_values || semantic_type >= analysis->type_count ||
        semantic_function >= xr_semantic_plan_function_count(builder->semantic_plan))
        return fail(error, error_size, "XR_TARGET_1001", "semantic scalar value identity is out of range");
    if (analysis->defined_values[semantic_value]) {
        if (analysis->value_types[semantic_value] != semantic_type ||
            analysis->value_functions[semantic_value] != semantic_function)
            return fail(error, error_size, "XR_TARGET_1001", "semantic scalar value identity is ambiguous");
        return true;
    }
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(builder->semantic_plan, semantic_type);
    uint16_t kind = XR_MACHINE_REP_COUNT;
    const XrSemanticOperationRecord *operation =
        semantic_operation <
                xr_semantic_plan_operation_count(builder->semantic_plan)
            ? xr_semantic_plan_operation(builder->semantic_plan,
                                         semantic_operation)
            : NULL;
    bool operation_result_void =
        operation && operation->function == semantic_function &&
        operation->result_value == semantic_value &&
        operation->result_type == semantic_type &&
        operation->opcode < XI_OP_COUNT &&
        xi_generated_op_result_kind(operation->opcode) == XI_GEN_RESULT_VOID;
    if (operation_result_void &&
        (operation->effects != xi_generated_op_effects(operation->opcode) ||
         operation->result_ownership !=
             xi_generated_op_result_ownership(operation->opcode)))
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic result-void operation contract is inconsistent");
    if (semantic_heap_closure_is_exact(builder->semantic_plan, operation))
        return fail(error, error_size, "XR_TARGET_1001",
                    "closure storage cannot be claimed by the scalar family");
    XrTargetScalarEligibility eligibility = operation_result_void
                                                ? XR_TARGET_SCALAR_VALUE
                                                : classify_scalar_type(type, &kind);
    if (operation_result_void)
        kind = XR_MACHINE_REP_VOID;
    if (eligibility == XR_TARGET_SCALAR_INVALID)
        return fail(error, error_size, "XR_TARGET_1001", "semantic scalar type has no exact machine representation");
    analysis->defined_values[semantic_value] = 1;
    analysis->value_types[semantic_value] = semantic_type;
    analysis->value_functions[semantic_value] = semantic_function;
    if (eligibility == XR_TARGET_SCALAR_NOT_APPLICABLE)
        return true;
    if (!operation_result_void) {
        if (analysis->type_rep_kinds[semantic_type] != XR_MACHINE_REP_COUNT &&
            analysis->type_rep_kinds[semantic_type] != kind)
            return fail(error, error_size, "XR_TARGET_1001", "semantic type has conflicting scalar representations");
        analysis->type_rep_kinds[semantic_type] = kind;
    }
    XrTargetMachineRepRecord rep;
    if (!make_machine_rep(xr_target_profile_machine_facts(builder->profile),
                          kind, &rep) ||
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
             semantic_operation >=
                 xr_semantic_plan_operation_count(builder->semantic_plan)) ||
            !make_slot_identity(builder->semantic_plan, semantic_function, role, source_identity,
                                XR_SEMANTIC_INDEX_NONE, &slot_identity))
            return fail(error, error_size, "XR_TARGET_1001", "scalar slot identity is incomplete");
        XrTargetSlotIntent slot = {
            .identity = slot_identity,
            .function = semantic_function,
            .semantic_value = semantic_value,
            .semantic_operation = parameter_slot ? XR_SEMANTIC_INDEX_NONE
                                                 : semantic_operation,
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
            if (!append_layout_intent(builder, semantic_type,
                                      XR_TARGET_LAYOUT_SCALAR, 0,
                                      &rep, error, error_size))
                return false;
        }
    }
    return append_value_intent(builder, &value, error, error_size);
}

static bool note_closure_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
    uint32_t semantic_operation, char *error, size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    if (!semantic_heap_closure_is_exact(builder->semantic_plan, operation))
        return fail(error, error_size, "XR_TARGET_1001",
                    "closure-storage family requires exact closure authority");
    if (operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >=
            xr_semantic_plan_function_count(builder->semantic_plan))
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic closure-storage identity is out of range");
    if (analysis->defined_values[operation->result_value]) {
        if (analysis->value_types[operation->result_value] !=
                operation->result_type ||
            analysis->value_functions[operation->result_value] !=
                operation->function)
            return fail(error, error_size, "XR_TARGET_1001",
                        "semantic closure-storage identity is ambiguous");
        return true;
    }
    if (analysis->type_rep_kinds[operation->result_type] !=
            XR_MACHINE_REP_COUNT &&
        analysis->type_rep_kinds[operation->result_type] !=
            XR_MACHINE_REP_DYN_VALUE)
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic closure type has conflicting storage representations");
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(
            xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize exact dynamic closure storage");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function,
                            XR_TARGET_SLOT_TEMPORARY, operation->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
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
    analysis->type_rep_kinds[operation->result_type] =
        XR_MACHINE_REP_DYN_VALUE;
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[operation->result_type] &&
         !append_layout_intent(builder, operation->result_type,
                               XR_TARGET_LAYOUT_DYNAMIC, 0, &rep, error,
                               error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool semantic_string_byte_slice_view_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
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
           result_type && result_type->kind == XR_KIND_SLICE && result_type->child_count == 1 &&
           result_type->child_begin < child_count &&
           children[result_type->child_begin] == operation->view_element_type &&
           result_type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW) &&
           element_type && element_type->kind == XR_KIND_INT &&
           element_type->scalar_rep == XR_NATIVE_U8;
}

static bool semantic_u8_slice_type_is_exact(const XrSemanticPlan *plan,
                                            uint32_t type_index) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    const uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW;
    const uint8_t allowed = required | XR_SEM_TYPE_CONST;
    if (!type || type->kind != XR_KIND_SLICE || type->builtin_type != XR_TID_NULL ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->aggregate_extent != 0 ||
        type->aggregate_align != 0 || type->child_count != 1 ||
        type->child_begin >= child_count || (type->flags & required) != required ||
        (type->flags & ~allowed) != 0)
        return false;
    const XrSemanticTypeRecord *element =
        xr_semantic_plan_type(plan, children[type->child_begin]);
    return element && element->kind == XR_KIND_INT &&
           element->builtin_type == XR_TID_NULL && element->scalar_rep == XR_NATIVE_U8 &&
           element->flags == 0 && element->child_count == 0 &&
           element->aggregate_extent == 0 && element->aggregate_align == 0;
}

static bool semantic_u8_slice_parameter_is_exact(
    const XrSemanticPlan *plan, const XrSemanticParameterRecord *parameter) {
    return parameter && parameter->function < xr_semantic_plan_function_count(plan) &&
           parameter->value != XR_SEMANTIC_INDEX_NONE && parameter->mode == XR_PARAM_READ &&
           parameter->ownership == XI_OWN_BORROWED &&
           parameter->transfer_mode == XR_TRANSFER_SHARE &&
           (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) == 0 &&
           parameter->reserved == 0 && semantic_u8_slice_type_is_exact(plan, parameter->type);
}

static bool note_u8_slice_view_parameter_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
    const XrSemanticParameterRecord *parameter, char *error, size_t error_size) {
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

static bool note_string_byte_slice_view_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
    uint32_t semantic_operation, char *error, size_t error_size) {
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
        .identity = slot_identity, .function = operation->function,
        .semantic_value = operation->result_value, .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE, .register_rep = rep, .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY, .root_kind = rep.root_kind,
        .ownership = rep.ownership, .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value, .semantic_function = operation->function,
        .semantic_type = operation->result_type, .register_rep = rep, .memory_rep = rep,
        .slot_identity = slot_identity, .has_slot = true,
    };
    return append_slot_intent(builder, &slot, error, error_size) &&
           append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_VIEW, 0, &rep,
                                error, error_size) &&
           append_value_intent(builder, &value, error, error_size);
}

static bool note_string_literal_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
    uint32_t semantic_operation, char *error, size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan,
                                   semantic_operation);
    if (!semantic_string_literal_is_exact(builder->semantic_plan, operation))
        return fail(error, error_size, "XR_TARGET_1001",
                    "string-literal-storage family requires exact literal authority");
    if (operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >=
            xr_semantic_plan_function_count(builder->semantic_plan))
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic string-literal identity is out of range");
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(
            xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize exact String literal storage");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function,
                            XR_TARGET_SLOT_TEMPORARY, operation->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "String literal slot identity is incomplete");
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
         !append_layout_intent(builder, operation->result_type,
                               XR_TARGET_LAYOUT_DYNAMIC, 0, &rep, error,
                               error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] =
        XR_MACHINE_REP_DYN_VALUE;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_stringbuilder_constructor_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
    uint32_t semantic_operation, char *error, size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan,
                                   semantic_operation);
    if (!semantic_stringbuilder_constructor_is_exact(builder->semantic_plan,
                                                      operation) ||
        operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >=
            xr_semantic_plan_function_count(builder->semantic_plan) ||
        analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder constructor result authority is incomplete");
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(
            xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1003",
                    "target profile cannot materialize StringBuilder constructor result");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function,
                            XR_TARGET_SLOT_TEMPORARY, operation->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
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
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[operation->result_type] &&
         !append_layout_intent(builder, operation->result_type,
                               XR_TARGET_LAYOUT_DYNAMIC, 0, &rep, error,
                               error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] =
        XR_MACHINE_REP_DYN_VALUE;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_stringbuilder_append_rune_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
    uint32_t semantic_operation, char *error, size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!semantic_stringbuilder_append_rune_is_exact(builder->semantic_plan, operation,
                                                     &receiver, NULL) ||
        operation->result_value >= analysis->total_values ||
        operation->function >= xr_semantic_plan_function_count(builder->semantic_plan) ||
        receiver >= analysis->total_values ||
        analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.append(rune) result authority is incomplete");
    XrTargetMachineRepRecord rep;
    if (!make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1003",
                    "target profile cannot materialize StringBuilder.append(rune) result");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function,
                            XR_TARGET_SLOT_TEMPORARY, operation->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
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
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    return true;
}

static bool note_stringbuilder_to_string_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
    uint32_t semantic_operation, char *error, size_t error_size) {
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
    if (!make_slot_identity(builder->semantic_plan, operation->function,
                            XR_TARGET_SLOT_TEMPORARY, operation->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.toString result slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity, .function = operation->function,
        .semantic_value = operation->result_value, .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE, .register_rep = rep, .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY, .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_OWNED, .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value, .semantic_function = operation->function,
        .semantic_type = operation->result_type, .register_rep = rep, .memory_rep = rep,
        .slot_identity = slot_identity, .has_slot = true,
    };
    if (!append_slot_intent(builder, &slot, error, error_size) ||
        (!analysis->used_types[operation->result_type] &&
         !append_layout_intent(builder, operation->result_type, XR_TARGET_LAYOUT_DYNAMIC,
                               0, &rep, error, error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_json_namespace_value_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
    uint32_t semantic_operation, char *error, size_t error_size) {
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
    if (!make_slot_identity(builder->semantic_plan, operation->function,
                            XR_TARGET_SLOT_TEMPORARY, operation->id, XR_SEMANTIC_INDEX_NONE,
                            &slot_identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "JSON.value result slot identity is incomplete");
    XrTargetSlotIntent slot = {
        .identity = slot_identity, .function = operation->function,
        .semantic_value = operation->result_value, .semantic_operation = semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE, .register_rep = rep, .memory_rep = rep,
        .role = XR_TARGET_SLOT_TEMPORARY, .root_kind = XR_TARGET_ROOT_DYNAMIC,
        .ownership = XR_TARGET_OWNERSHIP_OWNED, .debug_variable = XR_SEMANTIC_INDEX_NONE,
    };
    XrTargetValueIntent value = {
        .semantic_value = operation->result_value, .semantic_function = operation->function,
        .semantic_type = operation->result_type, .register_rep = rep, .memory_rep = rep,
        .slot_identity = slot_identity, .has_slot = true,
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

static bool note_direct_local_callee_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
    const XrDirectLocalCalleeStorageAnalysis *callee_analysis,
    uint32_t semantic_operation, char *error, size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan,
                                   semantic_operation);
    if (!direct_local_callee_storage_value_is_exact(
            builder->semantic_plan, callee_analysis, operation))
        return fail(error, error_size, "XR_TARGET_1001",
                    "direct-local callee-storage family requires exact shared callable authority");
    if (operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >=
            xr_semantic_plan_function_count(builder->semantic_plan))
        return fail(error, error_size, "XR_TARGET_1001",
                    "direct-local callee-storage identity is out of range");
    XrTargetMachineRepRecord rep;
    if (!make_static_callable_value_rep(
            xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize static callable storage");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function,
                            XR_TARGET_SLOT_TEMPORARY, operation->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "direct-local callee slot identity is incomplete");
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
        !append_layout_intent(builder, operation->result_type,
                              XR_TARGET_LAYOUT_DYNAMIC, 0, &rep, error,
                              error_size) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] =
        XR_MACHINE_REP_DYN_VALUE;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_direct_local_go_callee_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
    const XrDirectLocalGoCalleeStorageAnalysis *callee_analysis,
    uint32_t semantic_operation, char *error, size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan,
                                   semantic_operation);
    if (!direct_local_go_callee_storage_value_is_exact(
            builder->semantic_plan, callee_analysis, operation))
        return fail(error, error_size, "XR_TARGET_1001",
                    "direct-local go callee-storage family requires exact shared callable authority");
    if (operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >=
            xr_semantic_plan_function_count(builder->semantic_plan) ||
        analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1001",
                    "direct-local go callee-storage identity is ambiguous");
    XrTargetMachineRepRecord rep;
    if (!make_static_callable_value_rep(
            xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize static go callable storage");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function,
                            XR_TARGET_SLOT_TEMPORARY, operation->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
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
        !append_layout_intent(builder, operation->result_type,
                              XR_TARGET_LAYOUT_DYNAMIC, 0, &rep, error,
                              error_size) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] =
        XR_MACHINE_REP_DYN_VALUE;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_channel_allocation_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
    const uint8_t *exact_channel_values, uint32_t semantic_operation,
    char *error, size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan,
                                   semantic_operation);
    bool allocation = semantic_channel_allocation_is_exact(
        builder->semantic_plan, operation);
    bool alias = semantic_channel_identity_copy_is_exact(
        builder->semantic_plan, operation, exact_channel_values,
        analysis->total_values);
    if (!allocation && !alias)
        return fail(error, error_size, "XR_TARGET_1001",
                    "channel-allocation-storage family requires exact allocation or identity-copy authority");
    if (operation->result_value >= analysis->total_values ||
        operation->result_type >= analysis->type_count ||
        operation->function >=
            xr_semantic_plan_function_count(builder->semantic_plan) ||
        analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic channel outer-storage identity is ambiguous");
    XrTargetMachineRepRecord rep;
    bool rep_ok = allocation
                      ? make_dynamic_value_rep(
                            xr_target_profile_machine_facts(builder->profile),
                            &rep)
                      : make_borrowed_dynamic_value_rep(
                            xr_target_profile_machine_facts(builder->profile),
                            &rep);
    if (!rep_ok || !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize exact channel outer storage");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function,
                            XR_TARGET_SLOT_TEMPORARY, operation->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
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
         !append_layout_intent(builder, operation->result_type,
                               XR_TARGET_LAYOUT_DYNAMIC, 0, &rep, error,
                               error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] =
        XR_MACHINE_REP_DYN_VALUE;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool note_channel_receive_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
    const uint8_t *exact_channel_values, uint32_t semantic_operation,
    char *error, size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan,
                                   semantic_operation);
    if (!semantic_channel_receive_storage_is_exact(
            builder->semantic_plan, operation, exact_channel_values,
            analysis->total_values))
        return fail(error, error_size, "XR_TARGET_1001",
                    "channel-receive-storage family requires exact Channel<T> payload authority");
    if (operation->result_type >= analysis->type_count ||
        operation->function >=
            xr_semantic_plan_function_count(builder->semantic_plan) ||
        analysis->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic channel receive identity is ambiguous");
    uint16_t kind = XR_MACHINE_REP_COUNT;
    if (classify_scalar_type(
            xr_semantic_plan_type(builder->semantic_plan,
                                  operation->result_type),
            &kind) != XR_TARGET_SCALAR_VALUE ||
        kind == XR_MACHINE_REP_VOID)
        return fail(error, error_size, "XR_TARGET_1001",
                    "channel receive result has no exact scalar representation");
    XrTargetMachineRepRecord rep;
    if (!make_machine_rep(xr_target_profile_machine_facts(builder->profile),
                          kind, &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize channel receive storage");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function,
                            XR_TARGET_SLOT_TEMPORARY, operation->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
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
         !append_layout_intent(builder, operation->result_type,
                               XR_TARGET_LAYOUT_SCALAR, 0, &rep, error,
                               error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[operation->result_value] = 1;
    analysis->value_types[operation->result_value] = operation->result_type;
    analysis->value_functions[operation->result_value] = operation->function;
    analysis->type_rep_kinds[operation->result_type] = kind;
    analysis->used_types[operation->result_type] = 1;
    return true;
}

static bool collect_scalar_intents(XrTargetPlanBuilder *builder,
                                   XrTargetValueStorageAnalysis *analysis, char *error,
                                   size_t error_size) {
    if (!index_value_operations(builder->semantic_plan, analysis, error, error_size))
        return false;
    uint32_t parameter_count =
        (uint32_t) xr_semantic_plan_parameter_count(builder->semantic_plan);
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
            (operation->opcode != XI_PARAM ||
             operation->function != parameter->function ||
             operation->result_value != parameter->value ||
             operation->result_type != parameter->type))
            return fail(error, error_size, "XR_TARGET_1001",
                        "semantic parameter operation is inconsistent");
        if (!note_scalar_value(builder, analysis, parameter->value,
                               parameter->type, parameter->function,
                               XR_SEMANTIC_INDEX_NONE,
                               XR_TARGET_SLOT_PARAMETER, parameter->id,
                               error, error_size))
            return error && error_size && error[0]
                       ? false
                       : fail(error, error_size, "XR_TARGET_1001",
                              "semantic parameter scalar binding failed");
    }
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
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
                return fail(error, error_size, "XR_TARGET_1001", "parameter operation has no semantic parameter");
            continue;
        }
        if (semantic_heap_closure_is_exact(builder->semantic_plan, operation))
            continue;
        if (operation->opcode == XI_CHAN_TRY_RECV) {
            uint16_t receive_kind = XR_MACHINE_REP_COUNT;
            if (classify_scalar_type(
                    xr_semantic_plan_type(builder->semantic_plan,
                                          operation->result_type),
                    &receive_kind) == XR_TARGET_SCALAR_VALUE &&
                receive_kind != XR_MACHINE_REP_VOID)
                continue;
        }
        uint8_t role = operation->opcode == XI_PHI ? XR_TARGET_SLOT_PHI
                                                   : XR_TARGET_SLOT_TEMPORARY;
        if (!note_scalar_value(builder, analysis, operation->result_value,
                               operation->result_type, operation->function, i, role,
                               operation->id, error, error_size))
            return false;
    }
    return true;
}

static bool builder_begin_family(XrTargetPlanBuilder *builder, uint64_t family,
                                 char *error, size_t error_size) {
    if (!builder || builder->poisoned || builder->materialized || !family ||
        (family & ~XR_TARGET_REQUIRED_FAMILIES) != 0 ||
        (builder->started_family_mask & family) != 0)
        return fail(error, error_size, "XR_TARGET_1001", "target family collector cannot start");
    builder->started_family_mask |= family;
    return true;
}

static bool builder_add_scalars(XrTargetPlanBuilder *builder, char *error,
                                size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_SCALAR, error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    XrTargetMachineRepRecord void_rep;
    if (!make_machine_rep(xr_target_profile_machine_facts(builder->profile),
                          XR_MACHINE_REP_VOID,
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

static bool note_direct_local_unit_enum_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *analysis,
    uint32_t semantic_value, uint32_t semantic_type, uint32_t semantic_function,
    uint32_t semantic_operation, uint8_t role, XrStableId source_identity,
    char *error, size_t error_size) {
    if (semantic_value >= analysis->total_values || semantic_type >= analysis->type_count ||
        semantic_function >= xr_semantic_plan_function_count(builder->semantic_plan) ||
        !semantic_unit_enum_type_is_exact(
            xr_semantic_plan_type(builder->semantic_plan, semantic_type)))
        return fail(error, error_size, "XR_TARGET_1001",
                    "unit-enum value identity is not exact");
    if (analysis->defined_values[semantic_value]) {
        if (analysis->value_types[semantic_value] != semantic_type ||
            analysis->value_functions[semantic_value] != semantic_function)
            return fail(error, error_size, "XR_TARGET_1001",
                        "unit-enum value identity is ambiguous");
        return true;
    }
    bool parameter_slot = role == XR_TARGET_SLOT_PARAMETER;
    if ((!parameter_slot &&
         semantic_operation >= xr_semantic_plan_operation_count(builder->semantic_plan)))
        return fail(error, error_size, "XR_TARGET_1001",
                    "unit-enum defining operation is missing");
    XrTargetMachineRepRecord rep;
    XrStableId slot_identity;
    if (!make_unit_enum_rep(builder, semantic_type, &rep) ||
        !append_rep_intent(builder, &rep, error, error_size) ||
        !make_slot_identity(builder->semantic_plan, semantic_function, role,
                            source_identity, XR_SEMANTIC_INDEX_NONE, &slot_identity))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize unit-enum ordinal storage");
    XrTargetSlotIntent slot = {
        .identity = slot_identity,
        .function = semantic_function,
        .semantic_value = semantic_value,
        .semantic_operation = parameter_slot ? XR_SEMANTIC_INDEX_NONE : semantic_operation,
        .logical_slot = XR_SEMANTIC_INDEX_NONE,
        .register_rep = rep,
        .memory_rep = rep,
        .role = role,
        .root_kind = XR_TARGET_ROOT_NONE,
        .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
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
         !append_layout_intent(builder, semantic_type, XR_TARGET_LAYOUT_SCALAR, 0,
                               &rep, error, error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    analysis->defined_values[semantic_value] = 1;
    analysis->used_types[semantic_type] = 1;
    analysis->value_types[semantic_value] = semantic_type;
    analysis->value_functions[semantic_value] = semantic_function;
    analysis->type_rep_kinds[semantic_type] = XR_MACHINE_REP_ENUM_ORDINAL;
    return true;
}

static bool builder_add_direct_local_unit_enum_argument_storage(
    XrTargetPlanBuilder *builder, char *error, size_t error_size) {
    if (!builder_begin_family(
            builder, XR_TARGET_FAMILY_DIRECT_LOCAL_UNIT_ENUM_ARGUMENT_STORAGE,
            error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis,
                                              error, error_size) &&
                 index_value_operations(builder->semantic_plan, &analysis,
                                        error, error_size);
    uint32_t parameter_count =
        (uint32_t) xr_semantic_plan_parameter_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(builder->semantic_plan, i);
        if (!parameter ||
            !semantic_unit_enum_type_is_exact(
                xr_semantic_plan_type(builder->semantic_plan, parameter->type)))
            continue;
        valid = note_direct_local_unit_enum_value(
            builder, &analysis, parameter->value, parameter->type,
            parameter->function, XR_SEMANTIC_INDEX_NONE,
            XR_TARGET_SLOT_PARAMETER, parameter->id, error, error_size);
    }
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation || operation->opcode == XI_PARAM ||
            !semantic_unit_enum_type_is_exact(
                xr_semantic_plan_type(builder->semantic_plan, operation->result_type)))
            continue;
        valid = note_direct_local_unit_enum_value(
            builder, &analysis, operation->result_value, operation->result_type,
            operation->function, i,
            operation->opcode == XI_PHI ? XR_TARGET_SLOT_PHI
                                        : XR_TARGET_SLOT_TEMPORARY,
            operation->id, error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |=
        XR_TARGET_FAMILY_DIRECT_LOCAL_UNIT_ENUM_ARGUMENT_STORAGE;
    return true;
}

static bool builder_add_closure_storage(XrTargetPlanBuilder *builder,
                                        char *error, size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_CLOSURE_STORAGE,
                              error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis,
                                      error, error_size);
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "semantic operation is missing");
            break;
        }
        if (!semantic_heap_closure_is_exact(builder->semantic_plan, operation))
            continue;
        valid = note_closure_storage_value(builder, &analysis, i, error,
                                           error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_CLOSURE_STORAGE;
    return true;
}

static bool builder_add_string_literal_storage(
    XrTargetPlanBuilder *builder, char *error, size_t error_size) {
    if (!builder_begin_family(builder,
                              XR_TARGET_FAMILY_STRING_LITERAL_STORAGE,
                              error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis,
                                              error, error_size);
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "semantic operation is missing");
            break;
        }
        if (!semantic_string_literal_is_exact(builder->semantic_plan,
                                              operation))
            continue;
        valid = note_string_literal_storage_value(builder, &analysis, i,
                                                  error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |=
        XR_TARGET_FAMILY_STRING_LITERAL_STORAGE;
    return true;
}

static bool builder_add_string_byte_slice_view_storage(
    XrTargetPlanBuilder *builder, char *error, size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_STRING_BYTE_SLICE_VIEW_STORAGE,
                              error, error_size))
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

static bool builder_add_stringbuilder_append_rune_storage(
    XrTargetPlanBuilder *builder, char *error, size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_STRINGBUILDER_APPEND_RUNE_STORAGE,
                              error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error,
                                             error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++) {
        const XrTargetValueIntent *value = &builder->value_intents[i];
        if (value->semantic_value < analysis.total_values) {
            analysis.defined_values[value->semantic_value] = 1;
            analysis.value_types[value->semantic_value] = value->semantic_type;
            analysis.value_functions[value->semantic_value] = value->semantic_function;
        }
    }
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (operation && operation->intrinsic_kind ==
                             XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_RUNE)
            valid = note_stringbuilder_append_rune_storage_value(builder, &analysis, i,
                                                                  error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_STRINGBUILDER_APPEND_RUNE_STORAGE;
    return true;
}

static bool builder_add_stringbuilder_to_string_storage(
    XrTargetPlanBuilder *builder, char *error, size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_STRINGBUILDER_TO_STRING_STORAGE,
                              error, error_size))
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
            valid = note_stringbuilder_to_string_storage_value(builder, &analysis, i,
                                                                error, error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) { builder->poisoned = true; return false; }
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
            valid = note_json_namespace_value_storage_value(builder, &analysis, i, error,
                                                             error_size);
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_JSON_NAMESPACE_VALUE_STORAGE;
    return true;
}

static bool builder_add_stringbuilder_append_string_storage(
    XrTargetPlanBuilder *builder, char *error, size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_STRINGBUILDER_APPEND_STRING_STORAGE,
                              error, error_size)) return false;
    XrTargetValueStorageAnalysis analysis = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &analysis, error, error_size);
    for (uint32_t i = 0; valid && i < builder->value_intent_count; i++)
        if (builder->value_intents[i].semantic_value < analysis.total_values)
            analysis.defined_values[builder->value_intents[i].semantic_value] = 1;
    uint32_t count = (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!semantic_stringbuilder_append_string_is_exact(builder->semantic_plan, operation, NULL, NULL))
            continue;
        if (operation->result_value >= analysis.total_values || analysis.defined_values[operation->result_value]) {
            valid = false; break;
        }
        XrTargetMachineRepRecord rep;
        XrStableId slot_identity;
        valid = make_dynamic_value_rep(xr_target_profile_machine_facts(builder->profile), &rep) &&
                append_rep_intent(builder, &rep, error, error_size) &&
                make_slot_identity(builder->semantic_plan, operation->function,
                                   XR_TARGET_SLOT_TEMPORARY, operation->id,
                                   XR_SEMANTIC_INDEX_NONE, &slot_identity);
        XrTargetSlotIntent slot = {.identity=slot_identity,.function=operation->function,
            .semantic_value=operation->result_value,.semantic_operation=i,
            .logical_slot=XR_SEMANTIC_INDEX_NONE,.register_rep=rep,.memory_rep=rep,
            .role=XR_TARGET_SLOT_TEMPORARY,.root_kind=XR_TARGET_ROOT_DYNAMIC,
            .ownership=XR_TARGET_OWNERSHIP_OWNED,.debug_variable=XR_SEMANTIC_INDEX_NONE};
        XrTargetValueIntent value = {.semantic_value=operation->result_value,
            .semantic_function=operation->function,.semantic_type=operation->result_type,
            .register_rep=rep,.memory_rep=rep,.slot_identity=slot_identity,.has_slot=true};
        valid = valid && append_slot_intent(builder,&slot,error,error_size) &&
                append_value_intent(builder,&value,error,error_size);
        if (valid) analysis.defined_values[operation->result_value] = 1;
    }
    value_storage_analysis_dispose(&analysis);
    if (!valid) { builder->poisoned=true; return fail(error,error_size,"XR_TARGET_1003",
        "StringBuilder.append(string) storage authority is incomplete"); }
    builder->completed_family_mask |= XR_TARGET_FAMILY_STRINGBUILDER_APPEND_STRING_STORAGE;
    return true;
}

static bool builder_add_direct_local_callee_storage(
    XrTargetPlanBuilder *builder, char *error, size_t error_size) {
    if (!builder_begin_family(
            builder, XR_TARGET_FAMILY_DIRECT_LOCAL_CALLEE_STORAGE,
            error, error_size))
        return false;
    XrTargetValueStorageAnalysis values = {0};
    XrDirectLocalCalleeStorageAnalysis callees = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &values,
                                              error, error_size) &&
                 index_value_operations(builder->semantic_plan, &values,
                                        error, error_size) &&
                 direct_local_callee_storage_analysis_init(
                     builder->semantic_plan, &values, &callees, error,
                     error_size);
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "semantic operation is missing");
            break;
        }
        bool candidate = operation->opcode == XI_GET_SHARED &&
                         operation->result_value < callees.value_count &&
                         callees.target_by_value[operation->result_value] !=
                             XR_SEMANTIC_INDEX_NONE;
        if (!candidate)
            continue;
        if (!direct_local_callee_storage_value_is_exact(
                builder->semantic_plan, &callees, operation)) {
            const XrSemanticTypeRecord *failed_type =
                xr_semantic_plan_type(builder->semantic_plan,
                                      operation->result_type);
            const XrSemanticFunctionRecord *failed_target =
                xr_semantic_plan_function(
                    builder->semantic_plan,
                    callees.target_by_value[operation->result_value]);
            if (error && error_size)
                snprintf(error, error_size,
                         "XR_TARGET_1001: direct-local shared callee authority is incomplete "
                         "(value=%u invalid=%u uses=%u target=%u own=%u provenance=%u:%d:%u "
                         "type=%u:%u:%u:%u:%u flags=%u target-parent=%u params=%u:%u/%zu "
                         "shape=%lld:%u:%u:%u effects=%u/%u allocation=%u)",
                         operation->result_value,
                         callees.invalid_value[operation->result_value],
                         callees.use_count_by_value[operation->result_value],
                         callees.target_by_value[operation->result_value],
                         operation->result_ownership,
                         operation->return_provenance,
                         operation->return_parameter,
                         operation->return_complete,
                         failed_type ? failed_type->kind : UINT16_MAX,
                         failed_type ? failed_type->child_count : UINT32_MAX,
                         failed_type ? failed_type->scalar_rep : UINT16_MAX,
                         failed_type ? failed_type->aggregate_extent : UINT32_MAX,
                         failed_type ? failed_type->aggregate_align : UINT32_MAX,
                         failed_type ? failed_type->flags : UINT16_MAX,
                         failed_target ? failed_target->parent : UINT32_MAX,
                         failed_target ? failed_target->parameter_count : UINT16_MAX,
                         failed_target ? failed_target->parameter_begin : UINT32_MAX,
                         xr_semantic_plan_parameter_count(builder->semantic_plan),
                         (long long) operation->semantic_immediate,
                         operation->operand_count, operation->constant,
                         operation->callable_function, operation->effects,
                         xi_generated_op_effects(XI_GET_SHARED),
                         operation->allocation_key ? 1u : 0u);
            valid = false;
            break;
        }
        valid = note_direct_local_callee_storage_value(
            builder, &values, &callees, i, error, error_size);
    }
    direct_local_callee_storage_analysis_dispose(&callees);
    value_storage_analysis_dispose(&values);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |=
        XR_TARGET_FAMILY_DIRECT_LOCAL_CALLEE_STORAGE;
    return true;
}

static bool builder_add_direct_local_go_callee_storage(
    XrTargetPlanBuilder *builder, char *error, size_t error_size) {
    if (!builder_begin_family(
            builder, XR_TARGET_FAMILY_DIRECT_LOCAL_GO_CALLEE_STORAGE,
            error, error_size))
        return false;
    XrTargetValueStorageAnalysis values = {0};
    XrDirectLocalGoCalleeStorageAnalysis callees = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &values,
                                              error, error_size) &&
                 index_value_operations(builder->semantic_plan, &values,
                                        error, error_size) &&
                 direct_local_go_callee_storage_analysis_init(
                     builder->semantic_plan, &values, &callees, error,
                     error_size);
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "semantic operation is missing");
            break;
        }
        bool candidate = operation->opcode == XI_GET_SHARED &&
                         operation->result_value < callees.value_count &&
                         callees.candidate_value[operation->result_value];
        if (!candidate)
            continue;
        if (!direct_local_go_callee_storage_value_is_exact(
                builder->semantic_plan, &callees, operation)) {
            if (error && error_size)
                snprintf(error, error_size,
                         "XR_TARGET_1001: direct-local go shared callee authority is incomplete "
                         "(op=%u value=%u invalid=%u uses=%u target=%u shape=%lld:%u own=%u prov=%u)",
                         i, operation->result_value,
                         callees.invalid_value[operation->result_value],
                         callees.use_count_by_value[operation->result_value],
                         callees.target_by_value[operation->result_value],
                         (long long) operation->semantic_immediate,
                         operation->result_type, operation->result_ownership,
                         operation->return_provenance);
            valid = false;
            break;
        }
        valid = note_direct_local_go_callee_storage_value(
            builder, &values, &callees, i, error, error_size);
    }
    direct_local_go_callee_storage_analysis_dispose(&callees);
    value_storage_analysis_dispose(&values);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |=
        XR_TARGET_FAMILY_DIRECT_LOCAL_GO_CALLEE_STORAGE;
    return true;
}

static bool builder_add_channel_allocation_storage(
    XrTargetPlanBuilder *builder, char *error, size_t error_size) {
    if (!builder_begin_family(
            builder, XR_TARGET_FAMILY_CHANNEL_ALLOCATION_STORAGE,
            error, error_size))
        return false;
    XrTargetValueStorageAnalysis values = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &values,
                                              error, error_size) &&
                 index_value_operations(builder->semantic_plan, &values,
                                        error, error_size);
    uint8_t *exact = valid
                         ? (uint8_t *) allocate_records(values.total_values,
                                                        sizeof(*exact))
                         : NULL;
    if (valid && values.total_values && !exact)
        valid = fail(error, error_size, "XR_EXEC_5003",
                     "channel outer-storage analysis allocation failed");
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "semantic operation is missing");
            break;
        }
        bool candidate = operation->opcode == XI_CHAN_NEW ||
                         operation->opcode == XI_COPY;
        bool is_allocation = semantic_channel_allocation_is_exact(
            builder->semantic_plan, operation);
        bool is_alias = semantic_channel_identity_copy_is_exact(
            builder->semantic_plan, operation, exact, values.total_values);
        if (operation->opcode == XI_CHAN_NEW && !is_allocation) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "channel allocation authority is incomplete");
            break;
        }
        if (!candidate || (!is_allocation && !is_alias))
            continue;
        exact[operation->result_value] = 1;
        valid = note_channel_allocation_storage_value(
            builder, &values, exact, i, error, error_size);
    }
    xr_free(exact);
    value_storage_analysis_dispose(&values);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |=
        XR_TARGET_FAMILY_CHANNEL_ALLOCATION_STORAGE;
    return true;
}

static bool builder_add_channel_receive_storage(
    XrTargetPlanBuilder *builder, char *error, size_t error_size) {
    if (!builder_begin_family(
            builder, XR_TARGET_FAMILY_CHANNEL_RECEIVE_STORAGE,
            error, error_size))
        return false;
    XrTargetValueStorageAnalysis values = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &values,
                                              error, error_size) &&
                 index_value_operations(builder->semantic_plan, &values,
                                        error, error_size);
    uint8_t *exact_channels = valid
                                  ? (uint8_t *) allocate_records(
                                        values.total_values,
                                        sizeof(*exact_channels))
                                  : NULL;
    if (valid && values.total_values && !exact_channels)
        valid = fail(error, error_size, "XR_EXEC_5003",
                     "channel receive analysis allocation failed");
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation) {
            valid = fail(error, error_size, "XR_TARGET_1001",
                         "semantic operation is missing");
            break;
        }
        bool allocation = semantic_channel_allocation_is_exact(
            builder->semantic_plan, operation);
        bool alias = semantic_channel_identity_copy_is_exact(
            builder->semantic_plan, operation, exact_channels,
            values.total_values);
        if (allocation || alias)
            exact_channels[operation->result_value] = 1;
        if (operation->opcode != XI_CHAN_TRY_RECV)
            continue;
        uint16_t receive_kind = XR_MACHINE_REP_COUNT;
        if (classify_scalar_type(
                xr_semantic_plan_type(builder->semantic_plan,
                                      operation->result_type),
                &receive_kind) != XR_TARGET_SCALAR_VALUE ||
            receive_kind == XR_MACHINE_REP_VOID)
            continue;
        valid = note_channel_receive_storage_value(
            builder, &values, exact_channels, i, error, error_size);
    }
    xr_free(exact_channels);
    value_storage_analysis_dispose(&values);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |=
        XR_TARGET_FAMILY_CHANNEL_RECEIVE_STORAGE;
    return true;
}

static bool note_source_namespace_storage_value(
    XrTargetPlanBuilder *builder, XrTargetValueStorageAnalysis *values,
    const XrSourceNamespaceStorageAnalysis *namespaces,
    uint32_t semantic_operation, char *error, size_t error_size) {
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(builder->semantic_plan,
                                   semantic_operation);
    if (!operation || operation->result_value >= namespaces->value_count ||
        !namespaces->exact_value[operation->result_value] ||
        (operation->opcode != XI_IMPORT_REF &&
         operation->opcode != XI_GET_SHARED &&
         operation->opcode != XI_COPY) ||
        operation->result_value >= values->total_values ||
        operation->result_type >= values->type_count ||
        operation->function >=
            xr_semantic_plan_function_count(builder->semantic_plan) ||
        values->defined_values[operation->result_value])
        return fail(error, error_size, "XR_TARGET_1001",
                    "source namespace storage identity is incomplete");
    XrTargetMachineRepRecord rep;
    if (!make_borrowed_dynamic_value_rep(
            xr_target_profile_machine_facts(builder->profile), &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001",
                    "target profile cannot materialize source namespace storage");
    XrStableId slot_identity;
    if (!make_slot_identity(builder->semantic_plan, operation->function,
                            XR_TARGET_SLOT_TEMPORARY, operation->id,
                            XR_SEMANTIC_INDEX_NONE, &slot_identity))
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
         !append_layout_intent(builder, operation->result_type,
                               XR_TARGET_LAYOUT_DYNAMIC, 0, &rep, error,
                               error_size)) ||
        !append_value_intent(builder, &value, error, error_size))
        return false;
    values->defined_values[operation->result_value] = 1;
    values->value_types[operation->result_value] = operation->result_type;
    values->value_functions[operation->result_value] = operation->function;
    values->type_rep_kinds[operation->result_type] =
        XR_MACHINE_REP_DYN_VALUE;
    values->used_types[operation->result_type] = 1;
    return true;
}

static bool source_namespace_type_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint16_t opcode) {
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!plan || !operation || !type || operation->opcode != opcode ||
        operation->allocation_key || !stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != 0 ||
        operation->effects != xi_generated_op_effects(opcode) ||
        operation->flags != xi_generated_op_default_flags(opcode) ||
        operation->ownership_use != xi_generated_op_own_use(opcode) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_parameter != -1 || operation->return_complete != 1 ||
        type->scalar_rep != XR_SCALAR_REP_NONE ||
        type->child_count != 0 || type->aggregate_extent != 0 ||
        type->aggregate_align != 0 ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE |
                        XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    return opcode == XI_IMPORT_REF
               ? operation->operand_count == 0 &&
                     operation->semantic_immediate >= -1 &&
                     operation->semantic_immediate <= UINT16_MAX &&
                     operation->metadata_count == 2
               : opcode == XI_GET_SHARED && operation->operand_count == 0 &&
                     operation->semantic_immediate >= 0 &&
                     operation->semantic_immediate <= UINT16_MAX &&
                     operation->metadata_count == 0;
}

static bool source_namespace_identity_copy_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const XrSemanticOperandRecord *operands, uint32_t operand_count) {
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!plan || !operation || !operands || !type ||
        operation->opcode != XI_COPY || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count ||
        operation->semantic_immediate != XI_COPY_KIND_IDENTITY ||
        operation->allocation_key ||
        !stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != 0 || operation->metadata_count != 0 ||
        operation->effects != xi_generated_op_effects(XI_COPY) ||
        operation->flags != xi_generated_op_default_flags(XI_COPY) ||
        operation->ownership_use != xi_generated_op_own_use(XI_COPY) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_alias_operand != 0 ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_parameter != -1 || operation->return_complete != 1 ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE |
                        XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    const XrSemanticOperandRecord *source =
        &operands[operation->operand_begin];
    return source->role == XR_SEM_OPERAND_VALUE && source->parameter == -1 &&
           source->type == operation->result_type && source->flags == 0;
}

static void source_namespace_storage_analysis_dispose(
    XrSourceNamespaceStorageAnalysis *analysis) {
    if (!analysis)
        return;
    xr_free(analysis->exact_value);
    xr_free(analysis->dependency_by_value);
    memset(analysis, 0, sizeof(*analysis));
}

static bool source_namespace_storage_analysis_init(
    const XrSemanticPlan *plan, const XrTargetValueStorageAnalysis *values,
    XrSourceNamespaceStorageAnalysis *analysis, char *error,
    size_t error_size) {
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(plan);
    uint32_t target_count =
        (uint32_t) xr_semantic_plan_call_target_count(plan);
    analysis->value_count = values->total_values;
    analysis->exact_value = (uint8_t *) allocate_records(
        analysis->value_count, sizeof(*analysis->exact_value));
    analysis->dependency_by_value = (uint32_t *) allocate_records(
        analysis->value_count, sizeof(*analysis->dependency_by_value));
    uint32_t *source_target_by_operation = (uint32_t *) allocate_records(
        operation_count, sizeof(*source_target_by_operation));
    uint32_t *expected_uses = (uint32_t *) allocate_records(
        analysis->value_count, sizeof(*expected_uses));
    uint32_t *retain_uses = (uint32_t *) allocate_records(
        analysis->value_count, sizeof(*retain_uses));
    uint32_t *consumer_by_value = (uint32_t *) allocate_records(
        analysis->value_count, sizeof(*consumer_by_value));
    uint32_t *visit_epoch = (uint32_t *) allocate_records(
        analysis->value_count, sizeof(*visit_epoch));
    uint8_t *candidate = (uint8_t *) allocate_records(
        analysis->value_count, sizeof(*candidate));
    if ((analysis->value_count &&
         (!analysis->exact_value || !analysis->dependency_by_value ||
          !expected_uses || !retain_uses || !consumer_by_value ||
          !visit_epoch || !candidate)) ||
        (operation_count && !source_target_by_operation)) {
        xr_free(source_target_by_operation);
        xr_free(expected_uses);
        xr_free(retain_uses);
        xr_free(consumer_by_value);
        xr_free(visit_epoch);
        xr_free(candidate);
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
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(plan, i);
        if (!target || target->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT)
            continue;
        if (target->operation >= operation_count ||
            target->dependency >= xr_semantic_plan_dependency_count(plan) ||
            source_target_by_operation[target->operation] !=
                XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        source_target_by_operation[target->operation] = i;
    }
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata =
        xr_semantic_plan_metadata(plan, &metadata_count);
    uint32_t next_epoch = 1;
    for (uint32_t i = 0; i < operation_count; i++) {
        uint32_t target_index = source_target_by_operation[i];
        if (target_index == XR_SEMANTIC_INDEX_NONE)
            continue;
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(plan, target_index);
        const XrSemanticOperationRecord *call =
            xr_semantic_plan_operation(plan, i);
        if (!target || !call || call->opcode != XI_CALL_METHOD ||
            call->operand_count == 0 || call->operand_begin >= operand_count)
            goto invalid;
        const XrSemanticOperandRecord *receiver =
            &operands[call->operand_begin];
        if (receiver->role != XR_SEM_OPERAND_RECEIVER ||
            receiver->parameter != -1 ||
            receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
            receiver->parameter_mode != XR_PARAM_READ ||
            receiver->access != XR_CALL_ARG_PLAIN ||
            (receiver->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0)
            goto invalid;
        const XrSemanticOperationRecord *load = NULL;
        uint32_t current_value = receiver->value;
        uint32_t consumer_index = i;
        uint32_t namespace_type = receiver->type;
        uint32_t epoch = next_epoch++;
        if (epoch == 0) {
            memset(visit_epoch, 0,
                   analysis->value_count * sizeof(*visit_epoch));
            epoch = next_epoch++;
        }
        for (uint32_t depth = 0;; depth++) {
            if (depth >= operation_count ||
                current_value >= analysis->value_count ||
                visit_epoch[current_value] == epoch)
                goto invalid;
            visit_epoch[current_value] = epoch;
            uint32_t definition_index =
                values->value_operations[current_value];
            const XrSemanticOperationRecord *definition =
                definition_index != XR_SEMANTIC_INDEX_NONE
                    ? xr_semantic_plan_operation(plan, definition_index)
                    : NULL;
            if (!definition || definition->result_value != current_value ||
                definition->result_type != namespace_type ||
                definition->function != call->function ||
                (candidate[current_value] &&
                 (analysis->dependency_by_value[current_value] !=
                      target->dependency ||
                  consumer_by_value[current_value] != consumer_index)))
                goto invalid;
            candidate[current_value] = 1;
            analysis->dependency_by_value[current_value] = target->dependency;
            consumer_by_value[current_value] = consumer_index;
            if (source_namespace_type_is_exact(plan, definition,
                                               XI_GET_SHARED)) {
                load = definition;
                break;
            }
            if (!source_namespace_identity_copy_is_exact(
                    plan, definition, operands, operand_count))
                goto invalid;
            const XrSemanticOperandRecord *source =
                &operands[definition->operand_begin];
            consumer_index = definition_index;
            current_value = source->value;
        }
        uint32_t store_index = XR_SEMANTIC_INDEX_NONE;
        for (uint32_t j = 0; j < operation_count; j++) {
            const XrSemanticOperationRecord *candidate_store =
                xr_semantic_plan_operation(plan, j);
            if (!candidate_store || candidate_store->function != 0 ||
                candidate_store->opcode != XI_SET_SHARED ||
                candidate_store->semantic_immediate !=
                    load->semantic_immediate)
                continue;
            if (store_index != XR_SEMANTIC_INDEX_NONE)
                goto invalid;
            store_index = j;
        }
        const XrSemanticOperationRecord *store =
            store_index != XR_SEMANTIC_INDEX_NONE
                ? xr_semantic_plan_operation(plan, store_index)
                : NULL;
        if (!store || store->operand_count != 1 ||
            store->operand_begin >= operand_count)
            goto invalid;
        const XrSemanticOperandRecord *stored =
            &operands[store->operand_begin];
        const XrSemanticOperationRecord *import = NULL;
        current_value = stored->value;
        consumer_index = store_index;
        namespace_type = stored->type;
        epoch = next_epoch++;
        if (epoch == 0) {
            memset(visit_epoch, 0,
                   analysis->value_count * sizeof(*visit_epoch));
            epoch = next_epoch++;
        }
        for (uint32_t depth = 0;; depth++) {
            if (depth >= operation_count ||
                current_value >= analysis->value_count ||
                visit_epoch[current_value] == epoch)
                goto invalid;
            visit_epoch[current_value] = epoch;
            uint32_t definition_index =
                values->value_operations[current_value];
            const XrSemanticOperationRecord *definition =
                definition_index != XR_SEMANTIC_INDEX_NONE
                    ? xr_semantic_plan_operation(plan, definition_index)
                    : NULL;
            if (!definition || definition->result_value != current_value ||
                definition->result_type != namespace_type ||
                definition->function != 0 ||
                (candidate[current_value] &&
                 (analysis->dependency_by_value[current_value] !=
                      target->dependency ||
                  consumer_by_value[current_value] != consumer_index)))
                goto invalid;
            candidate[current_value] = 1;
            analysis->dependency_by_value[current_value] = target->dependency;
            consumer_by_value[current_value] = consumer_index;
            if (source_namespace_type_is_exact(plan, definition,
                                               XI_IMPORT_REF)) {
                import = definition;
                break;
            }
            if (!source_namespace_identity_copy_is_exact(
                    plan, definition, operands, operand_count))
                goto invalid;
            const XrSemanticOperandRecord *source =
                &operands[definition->operand_begin];
            consumer_index = definition_index;
            current_value = source->value;
        }
        const XrSemanticDependencyRecord *dependency =
            xr_semantic_plan_dependency(plan, target->dependency);
        if (!import || !load || receiver->type != load->result_type ||
            import->function != 0 || load->function != call->function ||
            store->function != 0 || stored->type != import->result_type ||
            store->semantic_immediate != load->semantic_immediate ||
            stored->role != XR_SEM_OPERAND_VALUE || stored->parameter != -1 ||
            stored->ownership_action != XR_SEM_OPERAND_CONSUME ||
            stored->parameter_mode != XR_PARAM_READ ||
            stored->access != XR_CALL_ARG_PLAIN || stored->flags != 0 ||
            load->result_type != import->result_type || !dependency ||
            import->metadata_begin > metadata_count ||
            import->metadata_count > metadata_count - import->metadata_begin ||
            !metadata || !metadata[import->metadata_begin] ||
            !metadata[import->metadata_begin + 1u] ||
            strcmp(metadata[import->metadata_begin],
                   dependency->module_path) != 0 ||
            metadata[import->metadata_begin + 1u][0] != '\0')
            goto invalid;
    }
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *use =
            xr_semantic_plan_operation(plan, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            goto invalid;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand =
                &operands[use->operand_begin + a];
            if (operand->value >= analysis->value_count ||
                !candidate[operand->value])
                continue;
            uint32_t definition_index =
                values->value_operations[operand->value];
            const XrSemanticOperationRecord *definition =
                xr_semantic_plan_operation(plan, definition_index);
            bool expected = i == consumer_by_value[operand->value] && a == 0;
            if (expected && use->opcode == XI_CALL_METHOD) {
                uint32_t target_index = source_target_by_operation[i];
                const XrSemanticCallTargetRecord *target =
                    target_index != XR_SEMANTIC_INDEX_NONE
                        ? xr_semantic_plan_call_target(plan, target_index)
                        : NULL;
                expected = target &&
                           target->dependency ==
                               analysis->dependency_by_value[operand->value] &&
                           operand->role == XR_SEM_OPERAND_RECEIVER;
            } else if (expected) {
                expected = (use->opcode == XI_COPY ||
                            use->opcode == XI_SET_SHARED) &&
                           operand->role == XR_SEM_OPERAND_VALUE;
            }
            if (expected) {
                if (expected_uses[operand->value] != 0)
                    goto invalid;
                expected_uses[operand->value] = 1;
                continue;
            }
            bool retain = definition && definition->opcode == XI_IMPORT_REF &&
                          use->opcode == XI_RETAIN && a == 0 &&
                          use->function == definition->function &&
                          operand->role == XR_SEM_OPERAND_VALUE &&
                          operand->type == definition->result_type &&
                          operand->parameter == -1 && operand->flags == 0;
            if (!retain || retain_uses[operand->value] != 0)
                goto invalid;
            retain_uses[operand->value] = 1;
        }
    }
    uint32_t block_count =
        (uint32_t) xr_semantic_plan_block_count(plan);
    for (uint32_t i = 0; i < block_count; i++) {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(plan, i);
        if (!block || (block->control_value != XR_SEMANTIC_INDEX_NONE &&
                       (block->control_value >= analysis->value_count ||
                        candidate[block->control_value])))
            goto invalid;
    }
    for (uint32_t i = 0; i < analysis->value_count; i++) {
        if (!candidate[i])
            continue;
        const XrSemanticOperationRecord *definition =
            xr_semantic_plan_operation(plan, values->value_operations[i]);
        if (!definition || expected_uses[i] != 1)
            goto invalid;
        analysis->exact_value[i] = 1;
    }
    xr_free(source_target_by_operation);
    xr_free(expected_uses);
    xr_free(retain_uses);
    xr_free(consumer_by_value);
    xr_free(visit_epoch);
    xr_free(candidate);
    return true;

invalid:
    xr_free(source_target_by_operation);
    xr_free(expected_uses);
    xr_free(retain_uses);
    xr_free(consumer_by_value);
    xr_free(visit_epoch);
    xr_free(candidate);
    source_namespace_storage_analysis_dispose(analysis);
    return fail(error, error_size, "XR_TARGET_1001",
                "source namespace storage authority is not exact");
}

static bool builder_add_source_namespace_storage(
    XrTargetPlanBuilder *builder, char *error, size_t error_size) {
    if (!builder_begin_family(builder,
                              XR_TARGET_FAMILY_SOURCE_NAMESPACE_STORAGE,
                              error, error_size))
        return false;
    XrTargetValueStorageAnalysis values = {0};
    XrSourceNamespaceStorageAnalysis namespaces = {0};
    bool valid = value_storage_analysis_init(builder->semantic_plan, &values,
                                              error, error_size) &&
                 index_value_operations(builder->semantic_plan, &values, error,
                                        error_size) &&
                 source_namespace_storage_analysis_init(
                     builder->semantic_plan, &values, &namespaces, error,
                     error_size);
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(builder->semantic_plan);
    for (uint32_t i = 0; valid && i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, i);
        if (!operation || operation->result_value >= namespaces.value_count)
            continue;
        if (!namespaces.exact_value[operation->result_value])
            continue;
        valid = note_source_namespace_storage_value(
            builder, &values, &namespaces, i, error, error_size);
    }
    source_namespace_storage_analysis_dispose(&namespaces);
    value_storage_analysis_dispose(&values);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |=
        XR_TARGET_FAMILY_SOURCE_NAMESPACE_STORAGE;
    return true;
}

static int classify_aggregate_type(const XrSemanticTypeRecord *type) {
    if (!type)
        return -1;
    if ((type->flags & XR_SEM_TYPE_NULLABLE) != 0)
        return 0;
    if (type->kind == XR_KIND_TUPLE || type->kind == XR_KIND_FIXED_ARRAY)
        return type->scalar_rep == XR_SCALAR_REP_NONE ? 1 : -1;
    if (type->kind == XR_KIND_STRUCT_OBJECT)
        return (type->flags & XR_SEM_TYPE_VALUE) == 0
                   ? 0
                   : (type->scalar_rep == XR_SCALAR_REP_NONE ? 1 : -1);
    if (type->kind == XR_KIND_INSTANCE)
        return (type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) == 0
                   ? 0
                   : (type->scalar_rep == XR_SCALAR_REP_NONE ? 1 : -1);
    return 0;
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
        return states[semantic_type] =
                   scalar_kind == XR_MACHINE_REP_VOID ? 2 : 1;
    int aggregate = classify_aggregate_type(type);
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
    uint32_t dependencies =
        type->kind == XR_KIND_FIXED_ARRAY ? 1u : type->child_count;
    for (uint32_t i = 0; i < dependencies; i++) {
        int child = aggregate_layout_eligibility(
            plan, children[type->child_begin + i], states);
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
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(builder->semantic_plan, semantic_type);
    uint16_t scalar_kind = XR_MACHINE_REP_COUNT;
    XrTargetScalarEligibility scalar = classify_scalar_type(type, &scalar_kind);
    if (scalar == XR_TARGET_SCALAR_INVALID)
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate field has an invalid scalar type fact");
    if (scalar == XR_TARGET_SCALAR_VALUE && scalar_kind != XR_MACHINE_REP_VOID) {
        XrTargetMachineRepRecord rep;
        if (!make_machine_rep(xr_target_profile_machine_facts(builder->profile), scalar_kind,
                              &rep) ||
            !append_rep_intent(builder, &rep, error, error_size) ||
            !append_layout_intent(builder, semantic_type, XR_TARGET_LAYOUT_SCALAR, 0,
                                  &rep, error, error_size))
            return false;
        states[semantic_type] = 2;
        return true;
    }
    int aggregate = classify_aggregate_type(type);
    if (aggregate <= 0)
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate field lacks an exact supported value layout");
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(builder->semantic_plan,
                                                               &child_count);
    uint32_t element_count = type->aggregate_extent;
    if (type->kind == XR_KIND_FIXED_ARRAY) {
        if (type->child_count != 1 ||
            !fixed_array_element_count(builder->semantic_plan, semantic_type,
                                       &element_count, error, error_size))
            return false;
    } else if (element_count != type->child_count)
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate extent disagrees with exact semantic fields");
    if (type->child_begin > child_count || type->child_count > child_count - type->child_begin)
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate semantic child range is invalid");
    uint32_t dependencies = type->kind == XR_KIND_FIXED_ARRAY ? 1u : type->child_count;
    for (uint32_t i = 0; i < dependencies; i++)
        if (!collect_layout_dependency(builder, children[type->child_begin + i], states,
                                       error, error_size))
            return false;
    XrTargetMachineRepRecord unresolved = {0};
    if (!append_layout_intent(builder, semantic_type, XR_TARGET_LAYOUT_AGGREGATE,
                              element_count, &unresolved, error, error_size))
        return false;
    states[semantic_type] = 2;
    return true;
}

static bool note_aggregate_value(XrTargetPlanBuilder *builder,
                                 XrTargetValueStorageAnalysis *analysis,
                                 uint32_t semantic_value, uint32_t semantic_type,
                                 uint32_t semantic_function, uint32_t semantic_operation,
                                 uint8_t role, XrStableId source_identity, uint8_t *states,
                                 int8_t *eligibility,
                                 char *error, size_t error_size) {
    if (semantic_value >= analysis->total_values || semantic_type >= analysis->type_count ||
        semantic_function >= xr_semantic_plan_function_count(builder->semantic_plan))
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic aggregate value identity is out of range");
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(builder->semantic_plan, semantic_type);
    if (semantic_operation < xr_semantic_plan_operation_count(builder->semantic_plan)) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(builder->semantic_plan, semantic_operation);
        if (operation && operation->opcode < XI_OP_COUNT &&
            xi_generated_op_result_kind(operation->opcode) == XI_GEN_RESULT_VOID)
            return true;
    }
    int aggregate = classify_aggregate_type(type);
    if (aggregate < 0) {
        if (error && error_size)
            snprintf(error, error_size,
                     "XR_TARGET_1002: value aggregate lacks exact semantic field facts "
                     "(type=%s)",
                     type && type->canonical_key ? type->canonical_key : "<unknown>");
        return false;
    }
    if (aggregate == 0)
        return true;
    int eligible = aggregate_layout_eligibility(builder->semantic_plan, semantic_type,
                                                eligibility);
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
        return fail(error, error_size, "XR_TARGET_1001",
                    "aggregate slot identity is incomplete");
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
                                     uint32_t function_count, char *error,
                                     size_t error_size) {
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
    uint32_t function_count =
        (uint32_t) xr_semantic_plan_function_count(builder->semantic_plan);
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
        if (!note_aggregate_value(builder, analysis, parameter->value,
                                  parameter->type, parameter->function,
                                  XR_SEMANTIC_INDEX_NONE, XR_TARGET_SLOT_PARAMETER,
                                  parameter->id, states, eligibility, error, error_size))
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
        /* Later CALL/COROUTINE families own caller-storage and suspend-frame placement. */
        if (deferred_functions[operation->function] ||
            (operation->opcode < XI_OP_COUNT &&
             (xi_generated_op_class(operation->opcode) == XI_GEN_CLASS_CALL ||
              xi_generated_op_class(operation->opcode) == XI_GEN_CLASS_COROUTINE)))
            continue;
        uint8_t role = operation->opcode == XI_PHI ? XR_TARGET_SLOT_PHI
                                                   : XR_TARGET_SLOT_TEMPORARY;
        if (!note_aggregate_value(builder, analysis, operation->result_value,
                                  operation->result_type, operation->function, i, role,
                                  operation->id, states, eligibility, error, error_size))
            return false;
    }
    return true;
}

static bool builder_add_aggregates(XrTargetPlanBuilder *builder, char *error,
                                   size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_AGGREGATE, error, error_size))
        return false;
    XrTargetValueStorageAnalysis analysis = {0};
    uint32_t type_count = (uint32_t) xr_semantic_plan_type_count(builder->semantic_plan);
    uint32_t function_count =
        (uint32_t) xr_semantic_plan_function_count(builder->semantic_plan);
    uint8_t *states = (uint8_t *) allocate_records(type_count, sizeof(*states));
    int8_t *eligibility = (int8_t *) allocate_records(type_count, sizeof(*eligibility));
    uint8_t *deferred_functions =
        (uint8_t *) allocate_records(function_count, sizeof(*deferred_functions));
    bool valid = (!type_count || (states && eligibility)) &&
                 (!function_count || deferred_functions) &&
                 mark_coroutine_functions(builder->semantic_plan, deferred_functions,
                                          function_count, error, error_size) &&
                 value_storage_analysis_init(builder->semantic_plan, &analysis, error,
                                             error_size) &&
                 collect_aggregate_intents(builder, &analysis, states, eligibility,
                                           deferred_functions,
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
            case XI_MULTI_RET: return true;
            default: break;
        }
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!operation || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin)
        return false;
    for (uint32_t i = 0; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *operand = &operands[operation->operand_begin + i];
        if (operand->role == XR_SEM_OPERAND_CALLEE ||
            operand->role == XR_SEM_OPERAND_RECEIVER ||
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
static bool semantic_operation_is_exact_channel_close(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *receiver_type) {
    if (receiver_type)
        *receiver_type = XR_SEMANTIC_INDEX_NONE;
    if (!plan || !operation)
        return false;
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (operation->opcode != XI_CALL_METHOD ||
        operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & INT64_C(1)) != 0 ||
        (uint64_t) operation->semantic_immediate > UINT32_MAX ||
        operation->operand_count != 1 ||
        operation->operand_begin >= operand_count ||
        operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count || !operands || !metadata ||
        !metadata[operation->metadata_begin] ||
        strcmp(metadata[operation->metadata_begin], "close") != 0 ||
        (operation->flags & XI_FLAG_MAY_SUSPEND) != 0)
        return false;
    const XrSemanticOperandRecord *receiver =
        &operands[operation->operand_begin];
    const XrSemanticTypeRecord *receiver_record =
        xr_semantic_plan_type(plan, receiver->type);
    const XrSemanticTypeRecord *result =
        xr_semantic_plan_type(plan, operation->result_type);
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(plan, operation->function);
    if (!receiver_record || !result || !function ||
        receiver_record->kind != XR_KIND_CHANNEL ||
        result->kind != XR_KIND_UNIT ||
        result->scalar_rep != XR_SCALAR_REP_NONE ||
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

static bool collect_channel_close_call_intent(
    XrTargetPlanBuilder *builder, uint32_t operation_index,
    const XrSemanticOperationRecord *operation, char *error, size_t error_size) {
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
        !stable_identity_from_pair(
            "xray-target-call-v5", operation->id, receiver->id,
            (uint32_t) operation->semantic_immediate, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "channel-close call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_stringbuilder_constructor_call_intent(
    XrTargetPlanBuilder *builder, uint32_t operation_index,
    const XrSemanticOperationRecord *operation, char *error,
    size_t error_size) {
    if (!semantic_stringbuilder_constructor_is_exact(builder->semantic_plan,
                                                      operation))
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
        .calling_convention =
            XR_TARGET_CALL_CONVENTION_STRINGBUILDER_CONSTRUCTOR,
        .target_kind = XR_TARGET_CALL_TARGET_STRINGBUILDER_CONSTRUCTOR,
    };
    if (!stable_identity_from_pair("xray-target-stringbuilder-constructor-v1",
                                   operation->id, operation->allocation_id, 0,
                                   &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder constructor call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_string_byte_slice_view_call_intent(
    XrTargetPlanBuilder *builder, uint32_t operation_index,
    const XrSemanticOperationRecord *operation, char *error, size_t error_size) {
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
    if (!stable_identity_from_pair("xray-target-string-byte-slice-view-v1", operation->id,
                                   xr_semantic_plan_type(builder->semantic_plan,
                                                         operation->result_type)->id,
                                   0, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "string byte-slice view call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_stringbuilder_append_rune_call_intent(
    XrTargetPlanBuilder *builder, uint32_t operation_index,
    const XrSemanticOperationRecord *operation, char *error, size_t error_size) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    uint32_t argument = XR_SEMANTIC_INDEX_NONE;
    if (!semantic_stringbuilder_append_rune_is_exact(builder->semantic_plan, operation,
                                                     &receiver, &argument))
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
        .result_ownership = XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_RUNE,
        .target_kind = XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_RUNE,
    };
    if (!receiver_type ||
        !stable_identity_from_pair("xray-target-stringbuilder-append-rune-v1",
                                   operation->id, receiver_type->id, argument,
                                   &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.append(rune) call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_stringbuilder_to_string_call_intent(
    XrTargetPlanBuilder *builder, uint32_t operation_index,
    const XrSemanticOperationRecord *operation, char *error, size_t error_size) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!semantic_stringbuilder_to_string_is_exact(builder->semantic_plan, operation, &receiver))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.toString dispatch authority is incomplete");
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {
        .semantic_call_target = XR_SEMANTIC_INDEX_NONE,
        .semantic_operation = operation_index, .caller_function = operation->function,
        .callee_function = XR_SEMANTIC_INDEX_NONE,
        .source_dependency = XR_SEMANTIC_INDEX_NONE, .source_export = XR_SEMANTIC_INDEX_NONE,
        .result_value = operation->result_value, .argument_begin = builder->call_argument_intent_count,
        .argument_count = 0, .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention = XR_TARGET_CALL_CONVENTION_STRINGBUILDER_TO_STRING,
        .target_kind = XR_TARGET_CALL_TARGET_STRINGBUILDER_TO_STRING,
    };
    if (!result_type || !stable_identity_from_pair("xray-target-stringbuilder-to-string-v1",
                                                    operation->id, result_type->id, receiver,
                                                    &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "StringBuilder.toString call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_stringbuilder_append_string_call_intent(
    XrTargetPlanBuilder *builder, uint32_t operation_index,
    const XrSemanticOperationRecord *operation, char *error, size_t error_size) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE, argument = XR_SEMANTIC_INDEX_NONE;
    if (!semantic_stringbuilder_append_string_is_exact(builder->semantic_plan, operation,
                                                        &receiver, &argument))
        return fail(error,error_size,"XR_TARGET_1003","StringBuilder.append(string) authority is incomplete");
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(builder->semantic_plan, operation->result_type);
    XrTargetCallIntent call = {.semantic_call_target=XR_SEMANTIC_INDEX_NONE,
        .semantic_operation=operation_index,.caller_function=operation->function,
        .callee_function=XR_SEMANTIC_INDEX_NONE,.source_dependency=XR_SEMANTIC_INDEX_NONE,
        .source_export=XR_SEMANTIC_INDEX_NONE,.result_value=operation->result_value,
        .argument_begin=builder->call_argument_intent_count,.argument_count=0,
        .result_mode=XR_TARGET_CALL_VALUE,.result_ownership=XR_TARGET_CALL_RETURN_OWNED,
        .calling_convention=XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_STRING,
        .target_kind=XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_STRING};
    if (!type || !stable_identity_from_pair("xray-target-stringbuilder-append-string-v1",
                                             operation->id,type->id,argument,&call.identity))
        return fail(error,error_size,"XR_TARGET_1003","StringBuilder.append(string) call identity is incomplete");
    return append_call_intent(builder,&call,error,error_size);
}

static bool collect_json_namespace_value_call_intent(
    XrTargetPlanBuilder *builder, uint32_t operation_index,
    const XrSemanticOperationRecord *operation, char *error, size_t error_size) {
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
    if (!result_type || !stable_identity_from_pair("xray-target-json-namespace-value-v1",
                                                   operation->id, result_type->id, argument,
                                                   &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "JSON.value call identity is incomplete");
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_direct_local_call_intent(XrTargetPlanBuilder *builder,
                                             uint32_t target_index,
                                             const XrSemanticCallTargetRecord *target,
                                             bool suspends,
                                             bool callee_suspendable,
                                             char *error, size_t error_size) {
    const XrSemanticPlan *plan = builder->semantic_plan;
    uint32_t operand_table_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_table_count);
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(plan, target->operation);
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(plan, target->function);
    if (!operation || !callee || target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
        (operation->opcode != XI_CALL && operation->opcode != XI_TAIL_CALL) ||
        operation->function >=
            xr_semantic_plan_function_count(plan) ||
        operation->operand_count == 0 || operation->operand_begin > operand_table_count ||
        operation->operand_count > operand_table_count - operation->operand_begin)
        return fail(error, error_size, "XR_TARGET_1003",
                    "only exact DIRECT_LOCAL call operations are supported");
    bool expected_suspend =
        (operation->effects & XI_EFFECT_MAY_SUSPEND) != 0 ||
        operation->opcode == XI_GO ||
        (operation->opcode == XI_CALL && callee_suspendable);
    if (expected_suspend != suspends)
        return fail(error, error_size, "XR_TARGET_1003",
                    "coroutine call lacks exact suspension-state authority");
    if (callee->parameter_count == UINT16_MAX ||
        operation->operand_count != (uint32_t) callee->parameter_count + 1u ||
        callee->parameter_begin > xr_semantic_plan_parameter_count(plan) ||
        callee->parameter_count >
            xr_semantic_plan_parameter_count(plan) - callee->parameter_begin ||
        operation->result_type != callee->return_type ||
        !call_type_is_exact_scalar(plan, operation->result_type))
        return fail(error, error_size, "XR_TARGET_1003",
                    "direct-local signature or scalar result storage is incomplete");
    const XrSemanticOperandRecord *callee_operand = &operands[operation->operand_begin];
    if (callee_operand->role != XR_SEM_OPERAND_CALLEE ||
        callee_operand->parameter != -1 ||
        (callee_operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) != 0)
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
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL,
        .target_kind = XR_TARGET_CALL_TARGET_DIRECT_LOCAL,
        .suspends = suspends,
        .tail = operation->opcode == XI_TAIL_CALL,
    };
    if (!stable_identity_from_pair("xray-target-call-v5", target->id, operation->id,
                                   0, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "direct-local call identity is incomplete");
    uint32_t call_intent = builder->call_intent_count;
    for (uint32_t ordinal = 0; ordinal < callee->parameter_count; ordinal++) {
        uint32_t parameter_index = callee->parameter_begin + ordinal;
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan, parameter_index);
        uint32_t semantic_operand = operation->operand_begin + ordinal + 1u;
        const XrSemanticOperandRecord *operand = &operands[semantic_operand];
        bool exact_scalar = call_type_is_exact_scalar(plan, operand->type);
        bool exact_u8_slice = semantic_u8_slice_parameter_is_exact(plan, parameter) &&
                              semantic_u8_slice_type_is_exact(plan, operand->type);
        bool exact_unit_enum = parameter && parameter->type == operand->type &&
                               semantic_unit_enum_type_is_exact(
                                   xr_semantic_plan_type(plan, operand->type));
        if (!parameter || operand->role != XR_SEM_OPERAND_ARGUMENT ||
            operand->parameter != (int16_t) ordinal || operand->type != parameter->type ||
            operand->parameter_mode != parameter->mode ||
            operand->transfer_mode != parameter->transfer_mode ||
            (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0 ||
            (!exact_scalar && !exact_u8_slice && !exact_unit_enum) ||
            parameter->mode != XR_PARAM_READ || operand->access != XR_CALL_ARG_PLAIN ||
            (operand->flags & XR_SEM_OPERAND_ADDRESSABLE) != 0 ||
            (parameter->ownership != XI_OWN_NONE &&
             !((exact_u8_slice || exact_unit_enum) &&
               parameter->ownership == XI_OWN_BORROWED)))
            return fail(error, error_size, "XR_TARGET_1003",
                        "direct-local argument contract needs unsupported storage or ownership");
        XrTargetCallArgumentIntent argument = {
            .call_intent = call_intent,
            .semantic_operand = semantic_operand,
            .semantic_value = operand->value,
            .callee_parameter = parameter_index,
            .ordinal = (uint16_t) ordinal,
            .mode = XR_TARGET_CALL_VALUE,
            .ownership = operand->ownership_action == XR_SEM_OPERAND_CONSUME
                             ? XR_TARGET_CALL_CONSUME
                             : XR_TARGET_CALL_READ,
            .transfer_mode = operand->transfer_mode,
        };
        if (!stable_identity_from_pair("xray-target-call-argument-v1", target->id,
                                       parameter->id, ordinal, &argument.identity) ||
            !append_call_argument_intent(builder, &argument, error, error_size))
            return false;
    }
    return append_call_intent(builder, &call, error, error_size);
}

static bool collect_source_export_call_intent(
    XrTargetPlanBuilder *builder, uint32_t target_index,
    const XrSemanticCallTargetRecord *target, bool suspends, char *error,
    size_t error_size) {
    const XrSemanticPlan *plan = builder->semantic_plan;
    const XrSemanticPlan *dependency =
        target && target->dependency < builder->semantic_dependency_count
            ? builder->semantic_dependencies[target->dependency]
            : NULL;
    const XrSemanticSourceExportRecord *source_export =
        dependency && target->source_export <
                          xr_semantic_plan_source_export_count(dependency)
            ? xr_semantic_plan_source_export(dependency, target->source_export)
            : NULL;
    const XrSemanticFunctionRecord *callee =
        source_export
            ? xr_semantic_plan_function(dependency, source_export->function)
            : NULL;
    const XrSemanticOperationRecord *operation =
        target ? xr_semantic_plan_operation(plan, target->operation) : NULL;
    if (!target || !dependency || !source_export || !callee || !operation ||
        target->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT ||
        target->function != XR_SEMANTIC_INDEX_NONE ||
        operation->opcode != XI_CALL_METHOD || !suspends ||
        operation->operand_count != (uint32_t) callee->parameter_count + 1u ||
        !xr_stable_id_equal(target->export_identity, source_export->id) ||
        !xr_stable_id_equal(target->callee_function, callee->id) ||
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
        .argument_count = 0,
        .result_mode = XR_TARGET_CALL_VALUE,
        .result_ownership = XR_TARGET_CALL_NONE,
        .calling_convention = XR_TARGET_CALL_CONVENTION_SOURCE_EXPORT,
        .target_kind = XR_TARGET_CALL_TARGET_SOURCE_EXPORT,
        .suspends = true,
    };
    if (!stable_identity_from_pair("xray-target-call-v5", target->id,
                                   operation->id, 0, &call.identity))
        return fail(error, error_size, "XR_TARGET_1003",
                    "source-export call identity is incomplete");
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
    uint32_t *target_by_operation = (uint32_t *) allocate_records(
        (uint32_t) operation_count, sizeof(*target_by_operation));
    uint8_t *state_by_operation = (uint8_t *) allocate_records(
        (uint32_t) operation_count, sizeof(*state_by_operation));
    XrTargetValueStorageAnalysis stringbuilder_values = {0};
    uint32_t function_count =
        (uint32_t) xr_semantic_plan_function_count(plan);
    uint8_t *suspendable = (uint8_t *) allocate_records(
        function_count, sizeof(*suspendable));
    uint32_t *reverse_head = (uint32_t *) allocate_records(
        function_count, sizeof(*reverse_head));
    uint32_t *reverse_next = (uint32_t *) allocate_records(
        (uint32_t) call_target_count, sizeof(*reverse_next));
    uint32_t *queue = (uint32_t *) allocate_records(function_count,
                                                     sizeof(*queue));
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
    bool valid = value_storage_analysis_init(
        builder->semantic_plan, &stringbuilder_values, error, error_size);
    for (uint32_t operation = 0; operation < (uint32_t) operation_count;
         operation++)
        target_by_operation[operation] = XR_SEMANTIC_INDEX_NONE;
    size_t entity_count = xr_semantic_plan_entity_count(plan);
    for (uint32_t i = 0; i < (uint32_t) entity_count && valid; i++) {
        const XrSemanticEntityRecord *entity = xr_semantic_plan_entity(plan, i);
        if (!entity || entity->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        const XrSemanticOperationRecord *operation =
            entity->subject < operation_count
                ? xr_semantic_plan_operation(plan, entity->subject)
                : NULL;
        if (entity->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION ||
            !operation || operation->function >= function_count ||
            state_by_operation[entity->subject]) {
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
    for (uint32_t operation_index = 0;
         operation_index < (uint32_t) operation_count; operation_index++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan, operation_index);
        if (!operation || operation->function >= function_count) {
            valid = false;
            break;
        }
        if (((operation->effects & XI_EFFECT_MAY_SUSPEND) != 0 ||
             operation->opcode == XI_GO) &&
            !suspendable[operation->function]) {
            suspendable[operation->function] = 1;
            queue[queue_end++] = operation->function;
        }
    }
    for (uint32_t i = 0; i < (uint32_t) call_target_count && valid; i++) {
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(plan, i);
        const XrSemanticOperationRecord *operation =
            target && target->operation < operation_count
                ? xr_semantic_plan_operation(plan, target->operation)
                : NULL;
        bool direct = target && target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL;
        bool source = target && target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT;
        if (!target || !operation || (!direct && !source) ||
            (direct && target->function >= function_count) ||
            (direct && operation->opcode != XI_CALL &&
             operation->opcode != XI_TAIL_CALL) ||
            (source && operation->opcode != XI_CALL_METHOD) ||
            target_by_operation[target->operation] != XR_SEMANTIC_INDEX_NONE) {
            valid = false;
            break;
        }
        target_by_operation[target->operation] = i;
        reverse_next[i] = XR_SEMANTIC_INDEX_NONE;
        if (direct) {
            reverse_next[i] = reverse_head[target->function];
            reverse_head[target->function] = i;
        } else if (!suspendable[operation->function]) {
            suspendable[operation->function] = 1;
            queue[queue_end++] = operation->function;
        }
    }
    while (valid && queue_begin < queue_end) {
        uint32_t callee = queue[queue_begin++];
        for (uint32_t target_index = reverse_head[callee];
             target_index != XR_SEMANTIC_INDEX_NONE;
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
            if (target && target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL) {
                valid = collect_direct_local_call_intent(
                    builder, target_index, target, state_by_operation[i] != 0,
                    target->function < function_count &&
                        suspendable[target->function] != 0,
                    error, error_size);
            } else {
                valid = target && collect_source_export_call_intent(
                                      builder, target_index, target,
                                      state_by_operation[i] != 0, error,
                                      error_size);
            }
        } else if (semantic_operation_is_exact_channel_close(plan, operation, NULL)) {
            valid = collect_channel_close_call_intent(builder, i, operation,
                                                       error, error_size);
        } else if (semantic_stringbuilder_constructor_is_exact(plan, operation)) {
            valid = note_stringbuilder_constructor_storage_value(
                        builder, &stringbuilder_values, i, error, error_size) &&
                    collect_stringbuilder_constructor_call_intent(
                        builder, i, operation, error, error_size);
        } else if (semantic_string_byte_slice_view_is_exact(plan, operation)) {
            valid = collect_string_byte_slice_view_call_intent(builder, i, operation,
                                                               error, error_size);
        } else if (semantic_stringbuilder_append_rune_is_exact(plan, operation, NULL, NULL)) {
            valid = collect_stringbuilder_append_rune_call_intent(builder, i, operation,
                                                                  error, error_size);
        } else if (semantic_stringbuilder_to_string_is_exact(plan, operation, NULL)) {
            valid = collect_stringbuilder_to_string_call_intent(builder, i, operation,
                                                                 error, error_size);
        } else if (semantic_stringbuilder_append_string_is_exact(plan, operation, NULL, NULL)) {
            valid = collect_stringbuilder_append_string_call_intent(builder, i, operation,
                                                                     error, error_size);
        } else if (semantic_json_namespace_value_is_exact(plan, operation, NULL)) {
            valid = collect_json_namespace_value_call_intent(builder, i, operation, error,
                                                             error_size);
        } else if (semantic_operation_is_call_shaped(plan, operation)) {
            uint32_t metadata_count = 0;
            uint32_t operand_count = 0;
            const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
            const XrSemanticOperandRecord *operands =
                xr_semantic_plan_operands(plan, &operand_count);
            const char *selector = operation->metadata_count == 1 &&
                                           operation->metadata_begin < metadata_count
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
            if (error && error_size)
                snprintf(error, error_size,
                         "XR_TARGET_1003: call-shaped operation has no exact target authority "
                         "operation=%u function=%u opcode=%u selector=%s intrinsic=%u "
                         "immediate=%lld result=value:%u,type:%u,ownership:%u,alias:%d "
                         "operands=%u receiver=value:%u,type:%u,role:%u,flags:%u "
                         "argument=value:%u,type:%u,role:%u,flags:%u",
                         i, operation->function, operation->opcode, selector,
                         operation->intrinsic_kind, (long long) operation->semantic_immediate,
                         operation->result_value, operation->result_type,
                         operation->result_ownership, operation->result_alias_operand,
                         operation->operand_count,
                         receiver ? receiver->value : XR_SEMANTIC_INDEX_NONE,
                         receiver ? receiver->type : XR_SEMANTIC_INDEX_NONE,
                         receiver ? receiver->role : 0,
                         receiver ? receiver->flags : 0,
                         argument ? argument->value : XR_SEMANTIC_INDEX_NONE,
                         argument ? argument->type : XR_SEMANTIC_INDEX_NONE,
                         argument ? argument->role : 0,
                         argument ? argument->flags : 0);
            valid = false;
        }
    }
    xr_free(target_by_operation);
    xr_free(state_by_operation);
    xr_free(suspendable);
    xr_free(reverse_head);
    xr_free(reverse_next);
    xr_free(queue);
    value_storage_analysis_dispose(&stringbuilder_values);
    if (!valid) {
        builder->poisoned = true;
        return false;
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_CALL_ADAPTER;
    return true;
}

static bool builder_add_coroutine_state_calls(XrTargetPlanBuilder *builder,
                                               char *error, size_t error_size) {
    if (!builder_begin_family(builder, XR_TARGET_FAMILY_COROUTINE_STATE_CALL,
                              error, error_size))
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
        return fail(error, error_size, "XR_EXEC_5003",
                    "coroutine state-call budget exhausted");
    }
    builder->completed_family_mask |= XR_TARGET_FAMILY_COROUTINE_STATE_CALL;
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
        int order = slot->function < function ? -1 : slot->function > function ? 1
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
        qsort(builder->rep_intents, builder->rep_intent_count,
              sizeof(*builder->rep_intents), compare_rep_intent);
    materialized->machine_rep_count = builder->rep_intent_count;
    materialized->machine_reps = (XrTargetMachineRepRecord *) allocate_records(
        materialized->machine_rep_count, sizeof(*materialized->machine_reps));
    if (materialized->machine_rep_count && !materialized->machine_reps)
        return fail(error, error_size, "XR_EXEC_5003", "machine representation materialization failed");
    for (uint32_t i = 0; i < materialized->machine_rep_count; i++) {
        if (i && compare_rep_record(&builder->rep_intents[i - 1u].record,
                                    &builder->rep_intents[i].record) == 0)
            return fail(error, error_size, "XR_TARGET_1001", "duplicate machine representation intent survived collection");
        materialized->machine_reps[i] = builder->rep_intents[i].record;
        materialized->machine_reps[i].id = i;
    }
    return true;
}

static int find_sorted_layout_intent(const XrTargetPlanBuilder *builder,
                                     uint32_t semantic_type) {
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

static bool materialize_layout_geometry(XrTargetPlanBuilder *builder,
                                        XrTargetMaterializedPlan *materialized,
                                        uint32_t index, uint8_t *states, char *error,
                                        size_t error_size) {
    if (states[index] == 2)
        return true;
    if (states[index] == 1)
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate layout dependency is recursive");
    states[index] = 1;
    XrTargetLayoutIntent *intent = &builder->layout_intents[index];
    XrTargetLayoutRecord *layout = &materialized->layouts[index];
    if (intent->kind == XR_TARGET_LAYOUT_SCALAR ||
        intent->kind == XR_TARGET_LAYOUT_DYNAMIC ||
        intent->kind == XR_TARGET_LAYOUT_VIEW) {
        layout->align = intent->memory_rep.memory_align;
        layout->fixed_prefix_size = intent->memory_rep.memory_size;
        states[index] = 2;
        return true;
    }
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(builder->semantic_plan, intent->semantic_type);
    uint32_t child_table_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(builder->semantic_plan, &child_table_count);
    if (!type || type->child_begin > child_table_count ||
        type->child_count > child_table_count - type->child_begin)
        return fail(error, error_size, "XR_TARGET_1002",
                    "aggregate layout has no exact semantic field range");
    uint32_t offset = 0;
    uint32_t aggregate_align = 1;
    for (uint32_t field_index = 0; field_index < intent->element_count; field_index++) {
        uint32_t child_ordinal = type->kind == XR_KIND_FIXED_ARRAY ? 0u : field_index;
        if (child_ordinal >= type->child_count)
            return fail(error, error_size, "XR_TARGET_1002",
                        "aggregate field ordinal exceeds semantic facts");
        uint32_t child_type = children[type->child_begin + child_ordinal];
        int child_layout = find_sorted_layout_intent(builder, child_type);
        if (child_layout < 0 ||
            !materialize_layout_geometry(builder, materialized,
                                         (uint32_t) child_layout, states, error, error_size))
            return false;
        const XrTargetLayoutRecord *child = &materialized->layouts[child_layout];
        uint32_t aligned = 0;
        if (!checked_align_u32(offset, child->align, &aligned) ||
            child->fixed_prefix_size > UINT32_MAX - aligned)
            return fail(error, error_size, "XR_EXEC_5003",
                        "aggregate field offset or size overflows");
        XrTargetFieldRecord *field =
            &materialized->fields[layout->field_begin + field_index];
        *field = (XrTargetFieldRecord) {
            .layout = index,
            .semantic_field = field_index,
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
    if (!checked_align_u32(offset, aggregate_align, &layout->fixed_prefix_size) ||
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
            intent->element_count > UINT16_MAX ||
            intent->element_count > UINT32_MAX - field_count)
            return fail(error, error_size, "XR_TARGET_1002",
                        "layout intents are duplicated or exceed field budgets");
        field_count += intent->element_count;
    }
    materialized->field_count = field_count;
    materialized->layouts = (XrTargetLayoutRecord *) allocate_records(
        materialized->layout_count, sizeof(*materialized->layouts));
    materialized->extents = (XrTargetExtentRecord *) allocate_records(
        materialized->extent_count, sizeof(*materialized->extents));
    materialized->fields = (XrTargetFieldRecord *) allocate_records(
        materialized->field_count, sizeof(*materialized->fields));
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
    uint8_t *states = (uint8_t *) allocate_records(materialized->layout_count,
                                                    sizeof(*states));
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
                                              XrTargetMaterializedPlan *materialized,
                                              char *error, size_t error_size) {
    uint32_t child_table_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(builder->semantic_plan, &child_table_count);
    for (uint32_t layout_index = 0; layout_index < materialized->layout_count; layout_index++) {
        const XrTargetLayoutIntent *intent = &builder->layout_intents[layout_index];
        if (intent->kind != XR_TARGET_LAYOUT_AGGREGATE)
            continue;
        const XrSemanticTypeRecord *type =
            xr_semantic_plan_type(builder->semantic_plan, intent->semantic_type);
        if (!type || type->child_begin > child_table_count ||
            type->child_count > child_table_count - type->child_begin)
            return fail(error, error_size, "XR_TARGET_1002",
                        "aggregate field type range is invalid");
        for (uint32_t field_index = 0; field_index < intent->element_count; field_index++) {
            uint32_t child_ordinal = type->kind == XR_KIND_FIXED_ARRAY ? 0u : field_index;
            uint32_t child_type = children[type->child_begin + child_ordinal];
            int child_layout = find_sorted_layout_intent(builder, child_type);
            int rep = child_layout < 0
                          ? -1
                          : find_rep_id(materialized,
                                        &builder->layout_intents[child_layout].memory_rep);
            if (rep < 0)
                return fail(error, error_size, "XR_TARGET_1002",
                            "aggregate field representation is missing");
            XrTargetFieldRecord *field =
                &materialized->fields[materialized->layouts[layout_index].field_begin +
                                      field_index];
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
                                const XrTargetMachineRepRecord *memory_intent,
                                int *register_rep, int *memory_rep) {
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
                                            XrTargetMaterializedPlan *materialized,
                                            char *error, size_t error_size) {
    if (builder->slot_intent_count > 1u)
        qsort(builder->slot_intents, builder->slot_intent_count,
              sizeof(*builder->slot_intents), compare_slot_intent);
    size_t function_count = xr_semantic_plan_function_count(builder->semantic_plan);
    if (function_count > 100000u)
        return fail(error, error_size, "XR_EXEC_5003", "target function budget exhausted");
    materialized->function_count = (uint32_t) function_count;
    materialized->slot_count = builder->slot_intent_count;
    materialized->functions = (XrTargetFunctionRecord *) allocate_records(
        materialized->function_count, sizeof(*materialized->functions));
    materialized->slots = (XrTargetSlotRecord *) allocate_records(
        materialized->slot_count, sizeof(*materialized->slots));
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
                return fail(error, error_size, "XR_TARGET_1002", "slot identity intent is duplicated");
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
            if (!checked_align_u32(offset, resolved_memory->memory_align, &aligned) ||
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
        if (!checked_align_u32(offset, function_record->frame_align,
                               &function_record->frame_size))
            return fail(error, error_size, "XR_EXEC_5003", "packed function frame overflows");
    }
    if (next_slot != materialized->slot_count)
        return fail(error, error_size, "XR_TARGET_1002", "slot intent references an unknown function");
    return true;
}

static bool materialize_values(XrTargetPlanBuilder *builder,
                               XrTargetMaterializedPlan *materialized, char *error,
                               size_t error_size) {
    if (builder->value_intent_count > 1u)
        qsort(builder->value_intents, builder->value_intent_count,
              sizeof(*builder->value_intents), compare_value_intent);
    materialized->value_rep_count = builder->value_intent_count;
    materialized->value_reps = (XrTargetValueRepRecord *) allocate_records(
        materialized->value_rep_count, sizeof(*materialized->value_reps));
    if (materialized->value_rep_count && !materialized->value_reps)
        return fail(error, error_size, "XR_EXEC_5003", "value representation materialization failed");
    for (uint32_t i = 0; i < materialized->value_rep_count; i++) {
        const XrTargetValueIntent *intent = &builder->value_intents[i];
        if (i && builder->value_intents[i - 1u].semantic_value == intent->semantic_value) {
            if (error && error_size)
                snprintf(error, error_size,
                         "XR_TARGET_1001: value representation intent is duplicated "
                         "(value=%u types=%u/%u aggregate=%u/%u)",
                         intent->semantic_value,
                         builder->value_intents[i - 1u].semantic_type,
                         intent->semantic_type,
                         builder->value_intents[i - 1u].resolve_type_rep ? 1u : 0u,
                         intent->resolve_type_rep ? 1u : 0u);
            return false;
        }
        int register_rep = -1;
        int memory_rep = -1;
        int slot = intent->has_slot
                       ? find_slot_id(materialized, intent->semantic_function,
                                      intent->slot_identity)
                       : -1;
        if ((intent->has_slot && slot < 0) ||
            !resolve_intent_reps(builder, materialized, intent->semantic_type,
                                 intent->resolve_type_rep, &intent->register_rep,
                                 &intent->memory_rep, &register_rep, &memory_rep))
            return fail(error, error_size, "XR_TARGET_1001", "value intent cannot bind its canonical records");
        materialized->value_reps[i] = (XrTargetValueRepRecord) {
            .semantic_value = intent->semantic_value,
            .register_rep = (uint16_t) register_rep,
            .memory_rep = (uint16_t) memory_rep,
            .slot = intent->has_slot ? (uint32_t) slot : XR_SEMANTIC_INDEX_NONE,
        };
    }
    return true;
}

static const XrTargetValueRepRecord *find_materialized_value(
    const XrTargetMaterializedPlan *materialized, uint32_t semantic_value) {
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

static bool semantic_type_is_exact_i64(const XrSemanticPlan *plan,
                                       uint32_t type_index) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    return type && type->kind == XR_KIND_INT &&
           type->scalar_rep == XR_NATIVE_I64 &&
           (type->flags & XR_SEM_TYPE_NULLABLE) == 0;
}

static bool materialized_rep_is_exact_i64(
    const XrTargetMachineRepRecord *rep) {
    return rep && rep->kind == XR_MACHINE_REP_I64 &&
           rep->register_bits == 64 && rep->memory_size == 8 &&
           rep->memory_align == 8 &&
           rep->signedness == XR_TARGET_SIGN_SIGNED &&
           rep->root_kind == XR_TARGET_ROOT_NONE &&
           rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
}

static bool materialized_i64_slot(const XrTargetMaterializedPlan *materialized,
                                  uint32_t function, uint32_t semantic_value,
                                  uint32_t *out_slot) {
    const XrTargetValueRepRecord *value =
        find_materialized_value(materialized, semantic_value);
    if (!value || value->slot == XR_SEMANTIC_INDEX_NONE ||
        value->slot >= materialized->slot_count ||
        value->register_rep >= materialized->machine_rep_count ||
        value->memory_rep >= materialized->machine_rep_count)
        return false;
    const XrTargetSlotRecord *slot = &materialized->slots[value->slot];
    if (slot->id != value->slot || slot->function != function ||
        slot->semantic_value != semantic_value ||
        slot->register_rep != value->register_rep ||
        slot->memory_rep != value->memory_rep || slot->size != 8 ||
        slot->align != 8 || slot->root_kind != XR_TARGET_ROOT_NONE ||
        slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        !materialized_rep_is_exact_i64(
            &materialized->machine_reps[value->register_rep]) ||
        !materialized_rep_is_exact_i64(
            &materialized->machine_reps[value->memory_rep]))
        return false;
    if (out_slot)
        *out_slot = value->slot;
    return true;
}

static uint8_t scalar_instruction_opcode(uint16_t semantic_opcode) {
    switch (semantic_opcode) {
        case XI_CONST: return XR_TARGET_INSTRUCTION_CONST_I64;
        case XI_COPY: return XR_TARGET_INSTRUCTION_COPY_I64;
        case XI_ADD: return XR_TARGET_INSTRUCTION_ADD_WRAP_I64;
        case XI_SUB: return XR_TARGET_INSTRUCTION_SUB_WRAP_I64;
        case XI_MUL: return XR_TARGET_INSTRUCTION_MUL_WRAP_I64;
        case XI_BAND: return XR_TARGET_INSTRUCTION_BAND_I64;
        case XI_BOR: return XR_TARGET_INSTRUCTION_BOR_I64;
        case XI_BXOR: return XR_TARGET_INSTRUCTION_BXOR_I64;
        case XI_NEG: return XR_TARGET_INSTRUCTION_NEG_WRAP_I64;
        case XI_BNOT: return XR_TARGET_INSTRUCTION_BNOT_I64;
        case XI_PARAM: return XR_TARGET_INSTRUCTION_PARAM_I64;
        default: return XR_TARGET_INSTRUCTION_INVALID;
    }
}

/*
 * A parameter operation computes nothing; it names the argument ordinal that
 * fills the function's parameter slot. The whole function is rejected unless
 * the operation and the frozen parameter record agree on ordinal, function,
 * exact signed-i64 type, and SSA value, so no row may bind an argument the
 * signature does not declare.
 */
static bool scalar_parameter_row_is_exact(
    const XrSemanticPlan *semantic,
    const XrTargetMaterializedPlan *materialized, uint32_t function_index,
    const XrSemanticFunctionRecord *function,
    const XrSemanticOperationRecord *operation, uint64_t *out_ordinal,
    uint32_t *out_slot) {
    if (operation->semantic_immediate < 0 ||
        (uint64_t) operation->semantic_immediate >= function->parameter_count)
        return false;
    uint32_t ordinal = (uint32_t) operation->semantic_immediate;
    const XrSemanticParameterRecord *parameter =
        xr_semantic_plan_parameter(semantic, function->parameter_begin + ordinal);
    if (!parameter || parameter->function != function_index ||
        parameter->ordinal != ordinal ||
        parameter->value != operation->result_value ||
        parameter->type != operation->result_type ||
        !semantic_type_is_exact_i64(semantic, parameter->type) ||
        !materialized_i64_slot(materialized, function_index, parameter->value,
                               out_slot))
        return false;
    if (out_ordinal)
        *out_ordinal = ordinal;
    return true;
}

/*
 * A function is committed only as one complete instruction group. Structural
 * or semantic facts outside this deliberately small closed family make the
 * function unavailable; they never produce a partial executable program.
 */
static bool materialize_scalar_instruction_function(
    const XrTargetPlanBuilder *builder,
    const XrTargetMaterializedPlan *materialized, uint32_t function_index,
    XrTargetInstructionRecord *rows, uint32_t row_begin,
    uint32_t *out_row_count) {
    if (out_row_count)
        *out_row_count = 0;
    const XrSemanticPlan *semantic = builder->semantic_plan;
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(semantic, function_index);
    if (!function || function_index >= materialized->function_count ||
        materialized->functions[function_index].id != function_index ||
        materialized->functions[function_index].semantic_function != function_index ||
        function->parameter_count > XR_TARGET_INSTRUCTION_MAX_PARAMETERS ||
        function->capture_count != 0 || function->block_count != 1 ||
        function->semantic_effects != 0 ||
        !semantic_type_is_exact_i64(semantic, function->return_type))
        return false;
    /* Every declared parameter must be exact signed i64 before any row is
     * emitted; a signature the executor could not fill stays unavailable
     * instead of producing a group that silently drops an argument. */
    for (uint16_t parameter_ordinal = 0;
         parameter_ordinal < function->parameter_count; parameter_ordinal++) {
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(
            semantic, function->parameter_begin + parameter_ordinal);
        if (!parameter || parameter->function != function_index ||
            parameter->ordinal != parameter_ordinal ||
            !semantic_type_is_exact_i64(semantic, parameter->type) ||
            !materialized_i64_slot(materialized, function_index,
                                   parameter->value, NULL))
            return false;
    }

    const XrSemanticBlockRecord *block =
        xr_semantic_plan_block(semantic, function->block_begin);
    uint32_t operation_total =
        (uint32_t) xr_semantic_plan_operation_count(semantic);
    uint32_t operand_total = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_total);
    if (!block || block->function != function_index ||
        block->kind != XI_BLOCK_RETURN || block->predecessor_count != 0 ||
        block->successors[0] != XR_SEMANTIC_INDEX_NONE ||
        block->successors[1] != XR_SEMANTIC_INDEX_NONE ||
        block->control_value == XR_SEMANTIC_INDEX_NONE ||
        block->operation_count == 0 ||
        block->operation_begin > operation_total ||
        block->operation_count > operation_total - block->operation_begin ||
        block->operation_count >= 40000000u)
        return false;

    bool return_defined = false;
    uint64_t bound_parameters = 0;
    for (uint32_t ordinal = 0; ordinal < block->operation_count; ordinal++) {
        uint32_t operation_index = block->operation_begin + ordinal;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, operation_index);
        uint8_t opcode = operation
                             ? scalar_instruction_opcode(operation->opcode)
                             : XR_TARGET_INSTRUCTION_INVALID;
        uint16_t expected_operands =
            opcode == XR_TARGET_INSTRUCTION_CONST_I64 ||
                    opcode == XR_TARGET_INSTRUCTION_PARAM_I64
                ? 0
                : opcode == XR_TARGET_INSTRUCTION_COPY_I64 ||
                          opcode == XR_TARGET_INSTRUCTION_NEG_WRAP_I64 ||
                          opcode == XR_TARGET_INSTRUCTION_BNOT_I64
                      ? 1
                      : opcode >= XR_TARGET_INSTRUCTION_ADD_WRAP_I64 &&
                                opcode <= XR_TARGET_INSTRUCTION_BXOR_I64
                            ? 2
                            : UINT16_MAX;
        uint32_t result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
        if (!operation || opcode == XR_TARGET_INSTRUCTION_INVALID ||
            operation->function != function_index ||
            operation->block != function->block_begin ||
            operation->effects != 0 ||
            operation->operand_count != expected_operands ||
            operation->operand_begin > operand_total ||
            operation->operand_count > operand_total - operation->operand_begin ||
            !semantic_type_is_exact_i64(semantic, operation->result_type) ||
            !materialized_i64_slot(materialized, function_index,
                                   operation->result_value, &result_slot))
            return false;
        if ((operation->opcode == XI_COPY &&
             operation->semantic_immediate != XI_COPY_KIND_IDENTITY) ||
            (operation->opcode != XI_CONST && operation->opcode != XI_COPY &&
             operation->opcode != XI_PARAM &&
             operation->semantic_immediate != 0))
            return false;

        uint64_t immediate_bits = 0;
        if (operation->opcode == XI_CONST) {
            const XrSemanticConstantRecord *constant =
                xr_semantic_plan_constant(semantic, operation->constant);
            if (!constant || constant->kind != XR_SEM_CONST_INT ||
                constant->type != operation->result_type)
                return false;
            immediate_bits = (uint64_t) constant->integer;
        } else if (operation->constant != XR_SEMANTIC_INDEX_NONE) {
            return false;
        }
        if (operation->opcode == XI_PARAM) {
            uint32_t parameter_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
            if (!scalar_parameter_row_is_exact(semantic, materialized,
                                               function_index, function,
                                               operation, &immediate_bits,
                                               &parameter_slot) ||
                parameter_slot != result_slot ||
                (bound_parameters & (UINT64_C(1) << immediate_bits)) != 0)
                return false;
            bound_parameters |= UINT64_C(1) << immediate_bits;
        }

        uint32_t operand_slots[2] = {XR_TARGET_INSTRUCTION_SLOT_NONE,
                                     XR_TARGET_INSTRUCTION_SLOT_NONE};
        for (uint16_t operand = 0; operand < operation->operand_count;
             operand++) {
            const XrSemanticOperandRecord *semantic_operand =
                &operands[operation->operand_begin + operand];
            if (!semantic_type_is_exact_i64(semantic, semantic_operand->type) ||
                !materialized_i64_slot(materialized, function_index,
                                       semantic_operand->value,
                                       &operand_slots[operand]))
                return false;
        }
        if (rows) {
            rows[row_begin + ordinal] = (XrTargetInstructionRecord) {
                .id = row_begin + ordinal,
                .function = function_index,
                .result_slot = result_slot,
                .operand_slots = {operand_slots[0], operand_slots[1]},
                .immediate_bits = immediate_bits,
                .opcode = opcode,
                .operand_count = (uint8_t) operation->operand_count,
            };
        }
        return_defined = return_defined ||
                         operation->result_value == block->control_value;
    }

    /* Dense coverage of the declared ordinals: a signature whose parameter is
     * never bound by a row would let the executor read an unfilled slot. */
    uint64_t declared_parameters =
        function->parameter_count == XR_TARGET_INSTRUCTION_MAX_PARAMETERS
            ? UINT64_MAX
            : (UINT64_C(1) << function->parameter_count) - 1u;
    uint32_t return_slot = XR_TARGET_INSTRUCTION_SLOT_NONE;
    if (bound_parameters != declared_parameters || !return_defined ||
        !materialized_i64_slot(materialized, function_index,
                               block->control_value, &return_slot))
        return false;
    if (rows) {
        uint32_t return_row = row_begin + block->operation_count;
        rows[return_row] = (XrTargetInstructionRecord) {
            .id = return_row,
            .function = function_index,
            .result_slot = XR_TARGET_INSTRUCTION_SLOT_NONE,
            .operand_slots = {return_slot, XR_TARGET_INSTRUCTION_SLOT_NONE},
            .opcode = XR_TARGET_INSTRUCTION_RETURN_I64,
            .operand_count = 1,
        };
    }
    if (out_row_count)
        *out_row_count = block->operation_count + 1u;
    return true;
}

static bool materialize_scalar_instructions(
    const XrTargetPlanBuilder *builder, XrTargetMaterializedPlan *materialized,
    char *error, size_t error_size) {
    uint32_t instruction_count = 0;
    for (uint32_t function = 0; function < materialized->function_count;
         function++) {
        uint32_t function_rows = 0;
        if (!materialize_scalar_instruction_function(
                builder, materialized, function, NULL, 0, &function_rows))
            continue;
        if (function_rows > 40000000u - instruction_count)
            return fail(error, error_size, "XR_EXEC_5003",
                        "scalar instruction budget exhausted");
        instruction_count += function_rows;
    }
    materialized->instruction_count = instruction_count;
    materialized->instructions = (XrTargetInstructionRecord *) allocate_records(
        instruction_count, sizeof(*materialized->instructions));
    if (instruction_count && !materialized->instructions)
        return fail(error, error_size, "XR_EXEC_5003",
                    "scalar instruction materialization failed");

    uint32_t next_instruction = 0;
    for (uint32_t function = 0; function < materialized->function_count;
         function++) {
        uint32_t function_rows = 0;
        if (!materialize_scalar_instruction_function(
                builder, materialized, function, materialized->instructions,
                next_instruction, &function_rows))
            continue;
        next_instruction += function_rows;
    }
    if (next_instruction != materialized->instruction_count)
        return fail(error, error_size, "XR_TARGET_1005",
                    "scalar instruction eligibility changed during materialization");
    return true;
}

static int find_rep_kind(const XrTargetMaterializedPlan *materialized, uint16_t kind) {
    for (uint32_t i = 0; i < materialized->machine_rep_count; i++)
        if (materialized->machine_reps[i].kind == kind)
            return (int) i;
    return -1;
}

static bool materialize_calls_and_adapters(
    const XrTargetPlanBuilder *builder, XrTargetMaterializedPlan *materialized,
    char *error, size_t error_size) {
    materialized->call_count = builder->call_intent_count;
    materialized->call_argument_count = builder->call_argument_intent_count;
    materialized->adapter_count = 0;
    materialized->calls = (XrTargetCallRecord *) allocate_records(
        materialized->call_count, sizeof(*materialized->calls));
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
        if (intent->suspends && !result_is_void &&
            result->slot == XR_SEMANTIC_INDEX_NONE)
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
            .caller_storage_slot = XR_SEMANTIC_INDEX_NONE,
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
        };
        for (uint32_t ordinal = 0; ordinal < intent->argument_count; ordinal++) {
            const XrTargetCallArgumentIntent *argument_intent =
                &builder->call_argument_intents[next_argument];
            const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(
                builder->semantic_plan, argument_intent->callee_parameter);
            const XrTargetValueRepRecord *caller = find_materialized_value(
                materialized, argument_intent->semantic_value);
            const XrTargetValueRepRecord *callee = parameter
                                                       ? find_materialized_value(
                                                             materialized, parameter->value)
                                                       : NULL;
            if (argument_intent->call_intent != i ||
                argument_intent->ordinal != ordinal || !caller || !callee ||
                caller->slot == XR_SEMANTIC_INDEX_NONE ||
                callee->slot == XR_SEMANTIC_INDEX_NONE ||
                caller->register_rep != callee->register_rep ||
                caller->memory_rep != callee->memory_rep)
                return fail(error, error_size, "XR_TARGET_1003",
                            "direct-local argument lacks identical caller/callee storage");
            materialized->call_arguments[next_argument] =
                (XrTargetCallArgumentRecord) {
                    .identity = argument_intent->identity,
                    .call = i,
                    .semantic_operand = argument_intent->semantic_operand,
                    .semantic_value = argument_intent->semantic_value,
                    .callee_parameter = argument_intent->callee_parameter,
                    .caller_slot = caller->slot,
                    .callee_slot = callee->slot,
                    .register_rep = caller->register_rep,
                    .memory_rep = caller->memory_rep,
                    .ordinal = argument_intent->ordinal,
                    .mode = argument_intent->mode,
                    .ownership = argument_intent->ownership,
                    .transfer_mode = argument_intent->transfer_mode,
                    .flags = argument_intent->flags,
                };
            next_argument++;
        }
    }
    if (next_argument != materialized->call_argument_count)
        return fail(error, error_size, "XR_TARGET_1003",
                    "call arguments do not exactly partition their table");
    return true;
}

static bool reconstruct_coroutine_resume(const XrSemanticPlan *semantic,
                                         uint32_t operation_index,
                                         const uint32_t *edge_by_block,
                                         const uint8_t *edge_counts,
                                         uint32_t block_count,
                                         uint32_t *suspend_block,
                                         uint32_t *resume_block,
                                         uint16_t *predecessor_ordinal) {
    uint32_t predecessor_count = 0;
    const uint32_t *predecessors =
        xr_semantic_plan_predecessors(semantic, &predecessor_count);
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(semantic, operation_index);
    const XrSemanticBlockRecord *suspend = operation
                                               ? xr_semantic_plan_block(
                                                     semantic, operation->block)
                                               : NULL;
    if (!operation || operation->block >= block_count || !suspend ||
        suspend->function != operation->function ||
        suspend->operation_begin != operation_index || suspend->operation_count != 1 ||
        suspend->predecessor_count != 1 ||
        suspend->predecessor_begin >= predecessor_count ||
        suspend->successors[0] == XR_SEMANTIC_INDEX_NONE ||
        (suspend->successors[1] != XR_SEMANTIC_INDEX_NONE &&
         suspend->successors[1] != suspend->successors[0]))
        return false;
    const XrSemanticBlockRecord *before = xr_semantic_plan_block(
        semantic, predecessors[suspend->predecessor_begin]);
    const XrSemanticBlockRecord *resume =
        xr_semantic_plan_block(semantic, suspend->successors[0]);
    if (!before || !resume || before->function != operation->function ||
        resume->function != operation->function ||
        (before->successors[0] != operation->block &&
         before->successors[1] != operation->block) ||
        resume->predecessor_count != 1 ||
        resume->predecessor_begin >= predecessor_count ||
        predecessors[resume->predecessor_begin] != operation->block ||
        edge_counts[operation->block] != 1)
        return false;
    const XrSemanticEdgeRecord *edge =
        xr_semantic_plan_edge(semantic, edge_by_block[operation->block]);
    if (!edge || edge->function != operation->function ||
        edge->from_block != operation->block ||
        edge->to_block != suspend->successors[0] ||
        edge->operation != XR_SEMANTIC_INDEX_NONE ||
        edge->kind != XR_SEM_EDGE_NORMAL || edge->flags != 0)
        return false;
    *suspend_block = operation->block;
    *resume_block = suspend->successors[0];
    *predecessor_ordinal = 0;
    return true;
}

static bool materialize_coroutine_state_calls(
    const XrTargetPlanBuilder *builder, XrTargetMaterializedPlan *materialized,
    char *error, size_t error_size) {
    const XrSemanticPlan *semantic = builder->semantic_plan;
    uint32_t function_count = materialized->function_count;
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(semantic);
    uint32_t entity_count = (uint32_t) xr_semantic_plan_entity_count(semantic);
    uint32_t block_count = (uint32_t) xr_semantic_plan_block_count(semantic);
    uint32_t *call_by_operation = operation_count
                                      ? (uint32_t *) allocate_records(
                                            operation_count,
                                            sizeof(*call_by_operation))
                                      : NULL;
    uint32_t *edge_by_block = block_count
                                  ? (uint32_t *) allocate_records(
                                        block_count, sizeof(*edge_by_block))
                                  : NULL;
    uint8_t *edge_counts = block_count
                               ? (uint8_t *) allocate_records(
                                     block_count, sizeof(*edge_counts))
                               : NULL;
    if ((operation_count && !call_by_operation) ||
        (block_count && (!edge_by_block || !edge_counts))) {
        xr_free(call_by_operation);
        xr_free(edge_by_block);
        xr_free(edge_counts);
        return fail(error, error_size, "XR_EXEC_5003",
                    "coroutine call index allocation failed");
    }
    for (uint32_t operation = 0; operation < operation_count; operation++)
        call_by_operation[operation] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t block = 0; block < block_count; block++)
        edge_by_block[block] = XR_SEMANTIC_INDEX_NONE;
    bool valid = true;
    uint32_t edge_count = (uint32_t) xr_semantic_plan_edge_count(semantic);
    for (uint32_t edge_index = 0; edge_index < edge_count; edge_index++) {
        const XrSemanticEdgeRecord *edge =
            xr_semantic_plan_edge(semantic, edge_index);
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
    for (uint32_t entity_index = 0; valid && entity_index < entity_count;
         entity_index++) {
        const XrSemanticEntityRecord *entity =
            xr_semantic_plan_entity(semantic, entity_index);
        if (!entity || entity->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, entity->subject);
        if (entity->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION ||
            !operation || operation->function >= function_count ||
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
        return fail(error, error_size, "XR_CORO_4000",
                    "coroutine state-call coverage is invalid");
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

    for (uint32_t entity_index = 0; valid && entity_index < entity_count;
         entity_index++) {
        const XrSemanticEntityRecord *entity =
            xr_semantic_plan_entity(semantic, entity_index);
        if (!entity || entity->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, entity->subject);
        XrTargetFunctionRecord *function =
            operation && operation->function < function_count
                ? &materialized->functions[operation->function]
                : NULL;
        if (!function || entity->ordinal == 0 ||
            entity->ordinal > function->coroutine_count) {
            valid = false;
            break;
        }
        uint32_t state_index = function->coroutine_begin + entity->ordinal - 1u;
        XrTargetCoroutineStateRecord *state =
            &materialized->coroutines[state_index];
        uint32_t suspend_block = XR_SEMANTIC_INDEX_NONE;
        uint32_t resume_block = XR_SEMANTIC_INDEX_NONE;
        uint16_t predecessor_ordinal = UINT16_MAX;
        if (state->semantic_entity != XR_SEMANTIC_INDEX_NONE ||
            !reconstruct_coroutine_resume(semantic, entity->subject,
                                          edge_by_block, edge_counts, block_count,
                                          &suspend_block, &resume_block,
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
            if (materialized->machine_reps[result->memory_rep].kind !=
                XR_MACHINE_REP_VOID) {
                if (result->slot >= materialized->slot_count ||
                    materialized->slots[result->slot].function !=
                        operation->function ||
                    materialized->slots[result->slot].semantic_value !=
                        operation->result_value) {
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
                materialized->calls[direct_call].target_kind ==
                    XR_TARGET_CALL_TARGET_SOURCE_EXPORT;
            if (direct_call >= materialized->call_count ||
                (source_call ? operation->opcode != XI_CALL_METHOD
                             : operation->opcode != XI_CALL) ||
                (materialized->calls[direct_call].flags &
                 XR_TARGET_CALL_SUSPEND) == 0 ||
                materialized->calls[direct_call].result_slot != result_slot ||
                materialized->calls[direct_call].caller_storage_slot !=
                    XR_SEMANTIC_INDEX_NONE) {
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
            .direct_call = direct_call,
            .result_slot = result_slot,
            .resume_predecessor_ordinal = predecessor_ordinal,
            .flags = flags,
        };
    }
    for (uint32_t i = 0; valid && i < state_count; i++)
        valid = materialized->coroutines[i].semantic_entity !=
                XR_SEMANTIC_INDEX_NONE;
    xr_free(call_by_operation);
    xr_free(edge_by_block);
    xr_free(edge_counts);
    return valid || fail(error, error_size, "XR_CORO_4000",
                         "coroutine state-call facts are not canonical");
}

static bool materialize_foundation_capabilities(
    const XrTargetPlanBuilder *builder, XrTargetMaterializedPlan *materialized,
    char *error, size_t error_size) {
    const XrTargetProfileDraft *facts = xr_target_profile_facts(builder->profile);
    if (!facts ||
        (facts->provider_mask & XR_TARGET_FOUNDATION_CAPABILITY_MASK) !=
            XR_TARGET_FOUNDATION_CAPABILITY_MASK)
        return fail(error, error_size, "XR_TARGET_1004",
                    "target profile lacks a foundation capability provider");
    materialized->capability_count = 2;
    materialized->capabilities = (XrTargetCapabilityRecord *) allocate_records(
        materialized->capability_count, sizeof(*materialized->capabilities));
    if (!materialized->capabilities)
        return fail(error, error_size, "XR_EXEC_5003",
                    "capability closure materialization failed");
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
    return true;
}

static bool builder_materialize(XrTargetPlanBuilder *builder,
                                XrTargetMaterializedPlan *materialized, char *error,
                                size_t error_size) {
    if (!builder || !materialized || builder->poisoned || builder->materialized ||
        builder->started_family_mask != XR_TARGET_REQUIRED_FAMILIES ||
        builder->completed_family_mask != XR_TARGET_REQUIRED_FAMILIES)
        return fail(error, error_size, "XR_TARGET_1001", "target builder family coverage is incomplete");
    if (!materialize_layouts(builder, materialized, error, error_size) ||
        !materialize_machine_reps(builder, materialized, error, error_size) ||
        !materialize_field_representations(builder, materialized, error, error_size) ||
        !materialize_functions_and_slots(builder, materialized, error, error_size) ||
        !materialize_values(builder, materialized, error, error_size) ||
        !materialize_scalar_instructions(builder, materialized, error,
                                         error_size) ||
        !materialize_calls_and_adapters(builder, materialized, error, error_size) ||
        !materialize_coroutine_state_calls(builder, materialized, error,
                                           error_size) ||
        !materialize_foundation_capabilities(builder, materialized, error,
                                             error_size)) {
        builder->poisoned = true;
        return false;
    }
    builder->materialized = true;
    return true;
}

static bool builder_new(const XrSemanticPlan *semantic_plan, XrTargetProfile *profile,
                        const XrSemanticPlan *const *dependencies,
                        uint32_t dependency_count,
                        XrTargetPlanBuilder **out, char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!semantic_plan || !profile || !out)
        return fail(error, error_size, "XR_TARGET_1000", "target builder input is missing");
    if (dependency_count > 1024u ||
        dependency_count != xr_semantic_plan_dependency_count(semantic_plan))
        return fail(error, error_size, "XR_TARGET_1000", "semantic plan is not verified");
    char nested_error[512] = {0};
    bool verified = dependency_count == 0
                        ? xr_semantic_plan_verify(semantic_plan, nested_error,
                                                  sizeof(nested_error))
                        : xr_semantic_plan_verify_module_set(
                              semantic_plan, dependencies, dependency_count,
                              nested_error, sizeof(nested_error));
    if (!verified)
        return fail(error, error_size, "XR_TARGET_1000", "semantic plan is not verified");
    if (!xr_target_profile_verify(profile, error, error_size))
        return false;
    XrTargetPlanBuilder *builder =
        (XrTargetPlanBuilder *) xr_calloc(1, sizeof(*builder));
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
            builder->semantic_dependencies[i] = xr_semantic_plan_retain(
                (XrSemanticPlan *) dependencies[i]);
            if (!builder->semantic_dependencies[i]) {
                builder_free(builder);
                return fail(error, error_size, "XR_EXEC_5003",
                            "target dependency retain failed");
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

static bool builder_freeze(XrTargetPlanBuilder *builder, XrTargetPlan **out,
                           char *error, size_t error_size) {
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
        .semantic_dependencies =
            (const XrSemanticPlan *const *) builder->semantic_dependencies,
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
        .adapters = materialized.adapters,
        .adapters_count = materialized.adapter_count,
        .capabilities = materialized.capabilities,
        .capabilities_count = materialized.capability_count,
        .coroutines = materialized.coroutines,
        .coroutines_count = materialized.coroutine_count,
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

bool xr_target_plan_build(const XrSemanticPlan *semantic_plan, XrTargetProfile *profile,
                          XrTargetPlan **out, char *error, size_t error_size) {
    return xr_target_plan_build_module_set(semantic_plan, NULL, 0, profile, out,
                                           error, error_size);
}

bool xr_target_plan_build_module_set(
    const XrSemanticPlan *semantic_plan,
    const XrSemanticPlan *const *dependencies, uint32_t dependency_count,
    XrTargetProfile *profile, XrTargetPlan **out, char *error,
    size_t error_size) {
    XrTargetPlanBuilder *builder = NULL;
    if (out)
        *out = NULL;
    if (!builder_new(semantic_plan, profile, dependencies, dependency_count,
                     &builder, error, error_size))
        return false;
    if (!builder_add_scalars(builder, error, error_size) ||
        !builder_add_direct_local_unit_enum_argument_storage(builder, error, error_size) ||
        !builder_add_closure_storage(builder, error, error_size) ||
        !builder_add_string_literal_storage(builder, error, error_size) ||
        !builder_add_string_byte_slice_view_storage(builder, error, error_size) ||
        !builder_add_stringbuilder_append_rune_storage(builder, error, error_size) ||
        !builder_add_stringbuilder_to_string_storage(builder, error, error_size) ||
        !builder_add_stringbuilder_append_string_storage(builder, error, error_size) ||
        !builder_add_json_namespace_value_storage(builder, error, error_size) ||
        !builder_add_direct_local_callee_storage(builder, error, error_size) ||
        !builder_add_direct_local_go_callee_storage(builder, error, error_size) ||
        !builder_add_channel_allocation_storage(builder, error, error_size) ||
        !builder_add_channel_receive_storage(builder, error, error_size) ||
        !builder_add_source_namespace_storage(builder, error, error_size) ||
        !builder_add_aggregates(builder, error, error_size) ||
        !builder_add_calls_and_adapters(builder, error, error_size) ||
        !builder_add_coroutine_state_calls(builder, error, error_size)) {
        builder_free(builder);
        return false;
    }
    bool frozen = builder_freeze(builder, out, error, error_size);
    builder_free(builder);
    return frozen;
}
