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
#include "../../base/xmalloc.h"
#include "../../ir/xi.h"
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
} XrTargetLayoutIntent;

typedef struct XrTargetValueIntent {
    uint32_t semantic_value;
    uint32_t semantic_function;
    XrTargetMachineRepRecord register_rep;
    XrTargetMachineRepRecord memory_rep;
    XrStableId slot_identity;
    bool has_slot;
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
} XrTargetSlotIntent;

typedef struct XrTargetScalarAnalysis {
    uint8_t *defined_values;
    uint8_t *used_types;
    uint32_t *value_types;
    uint32_t *value_functions;
    uint32_t *value_operations;
    uint16_t *type_rep_kinds;
    uint32_t total_values;
    uint32_t type_count;
} XrTargetScalarAnalysis;

typedef struct XrTargetMaterializedPlan {
    XrTargetMachineRepRecord *machine_reps;
    uint32_t machine_rep_count;
    XrTargetValueRepRecord *value_reps;
    uint32_t value_rep_count;
    XrTargetExtentRecord *extents;
    uint32_t extent_count;
    XrTargetLayoutRecord *layouts;
    uint32_t layout_count;
    XrTargetFunctionRecord *functions;
    uint32_t function_count;
    XrTargetSlotRecord *slots;
    uint32_t slot_count;
} XrTargetMaterializedPlan;

typedef struct XrTargetPlanBuilder XrTargetPlanBuilder;

struct XrTargetPlanBuilder {
    XrSemanticPlan *semantic_plan;
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

static void scalar_analysis_dispose(XrTargetScalarAnalysis *analysis) {
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
    xr_free(materialized->functions);
    xr_free(materialized->slots);
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

static bool rep_layout_for_kind(const XrTargetProfileDraft *profile, uint16_t kind,
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

static bool make_machine_rep(const XrTargetProfileDraft *profile, uint16_t kind,
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
    return compare_rep_record(&a->memory_rep, &b->memory_rep);
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
                                 const XrTargetMachineRepRecord *memory_rep, char *error,
                                 size_t error_size) {
    if (!reserve_records((void **) &builder->layout_intents, &builder->layout_intent_capacity,
                         builder->layout_intent_count + 1u, 1000000u,
                         sizeof(*builder->layout_intents)))
        return fail(error, error_size, "XR_EXEC_5003", "layout intent budget exhausted");
    XrTargetLayoutIntent *intent = &builder->layout_intents[builder->layout_intent_count++];
    intent->semantic_type = semantic_type;
    intent->memory_rep = *memory_rep;
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

static bool scalar_analysis_init(const XrSemanticPlan *plan, XrTargetScalarAnalysis *analysis,
                                 char *error, size_t error_size) {
    size_t type_count = xr_semantic_plan_type_count(plan);
    size_t function_count = xr_semantic_plan_function_count(plan);
    if (type_count > 1000000u || function_count > 100000u)
        return fail(error, error_size, "XR_EXEC_5003", "scalar target input exceeds hard budgets");
    uint32_t total_values = 0;
    if (function_count) {
        const XrSemanticFunctionRecord *last =
            xr_semantic_plan_function(plan, (uint32_t) function_count - 1u);
        if (!last || last->value_begin > UINT32_MAX - last->value_count)
            return fail(error, error_size, "XR_EXEC_5003", "semantic value identity budget overflow");
        total_values = last->value_begin + last->value_count;
    }
    if (total_values > 40000000u)
        return fail(error, error_size, "XR_EXEC_5003", "scalar target value budget exhausted");
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
        scalar_analysis_dispose(analysis);
        return fail(error, error_size, "XR_EXEC_5003", "scalar target analysis allocation failed");
    }
    for (uint32_t i = 0; i < total_values; i++)
        analysis->value_operations[i] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < (uint32_t) type_count; i++)
        analysis->type_rep_kinds[i] = XR_MACHINE_REP_COUNT;
    return true;
}

static bool index_value_operations(const XrSemanticPlan *plan,
                                   XrTargetScalarAnalysis *analysis, char *error,
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

static bool note_scalar_value(XrTargetPlanBuilder *builder, XrTargetScalarAnalysis *analysis,
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
    XrTargetScalarEligibility eligibility = classify_scalar_type(type, &kind);
    if (eligibility == XR_TARGET_SCALAR_INVALID)
        return fail(error, error_size, "XR_TARGET_1001", "semantic scalar type has no exact machine representation");
    analysis->defined_values[semantic_value] = 1;
    analysis->value_types[semantic_value] = semantic_type;
    analysis->value_functions[semantic_value] = semantic_function;
    if (eligibility == XR_TARGET_SCALAR_NOT_APPLICABLE)
        return true;
    if (analysis->type_rep_kinds[semantic_type] != XR_MACHINE_REP_COUNT &&
        analysis->type_rep_kinds[semantic_type] != kind)
        return fail(error, error_size, "XR_TARGET_1001", "semantic type has conflicting scalar representations");
    analysis->type_rep_kinds[semantic_type] = kind;
    XrTargetMachineRepRecord rep;
    if (!make_machine_rep(xr_target_profile_facts(builder->profile), kind, &rep) ||
        !append_rep_intent(builder, &rep, error, error_size))
        return fail(error, error_size, "XR_TARGET_1001", "target profile cannot materialize a scalar representation");
    XrTargetValueIntent value = {
        .semantic_value = semantic_value,
        .semantic_function = semantic_function,
        .register_rep = rep,
        .memory_rep = rep,
    };
    if (kind != XR_MACHINE_REP_VOID) {
        XrStableId slot_identity;
        if (semantic_operation >= xr_semantic_plan_operation_count(builder->semantic_plan) ||
            !make_slot_identity(builder->semantic_plan, semantic_function, role, source_identity,
                                XR_SEMANTIC_INDEX_NONE, &slot_identity))
            return fail(error, error_size, "XR_TARGET_1001", "scalar slot identity is incomplete");
        XrTargetSlotIntent slot = {
            .identity = slot_identity,
            .function = semantic_function,
            .semantic_value = semantic_value,
            .semantic_operation = semantic_operation,
            .logical_slot = XR_SEMANTIC_INDEX_NONE,
            .register_rep = rep,
            .memory_rep = rep,
            .role = role,
            .root_kind = XR_TARGET_ROOT_NONE,
            .ownership = XR_TARGET_OWNERSHIP_TRIVIAL,
            .debug_variable = XR_SEMANTIC_INDEX_NONE,
        };
        if (!append_slot_intent(builder, &slot, error, error_size))
            return false;
        value.has_slot = true;
        value.slot_identity = slot_identity;
        if (!analysis->used_types[semantic_type]) {
            analysis->used_types[semantic_type] = 1;
            if (!append_layout_intent(builder, semantic_type, &rep, error, error_size))
                return false;
        }
    }
    return append_value_intent(builder, &value, error, error_size);
}

static bool collect_scalar_intents(XrTargetPlanBuilder *builder,
                                   XrTargetScalarAnalysis *analysis, char *error,
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
        if (!operation || operation->opcode != XI_PARAM ||
            !note_scalar_value(builder, analysis, parameter->value, parameter->type,
                               parameter->function, operation_index, XR_TARGET_SLOT_PARAMETER,
                               parameter->id, error, error_size))
            return false;
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
    XrTargetScalarAnalysis analysis = {0};
    XrTargetMachineRepRecord void_rep;
    if (!make_machine_rep(xr_target_profile_facts(builder->profile), XR_MACHINE_REP_VOID,
                          &void_rep) ||
        !append_rep_intent(builder, &void_rep, error, error_size) ||
        !scalar_analysis_init(builder->semantic_plan, &analysis, error, error_size) ||
        !collect_scalar_intents(builder, &analysis, error, error_size)) {
        scalar_analysis_dispose(&analysis);
        builder->poisoned = true;
        return false;
    }
    scalar_analysis_dispose(&analysis);
    builder->completed_family_mask |= XR_TARGET_FAMILY_SCALAR;
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

static bool materialize_layouts(XrTargetPlanBuilder *builder,
                                XrTargetMaterializedPlan *materialized, char *error,
                                size_t error_size) {
    if (builder->layout_intent_count > 1u)
        qsort(builder->layout_intents, builder->layout_intent_count,
              sizeof(*builder->layout_intents), compare_layout_intent);
    materialized->layout_count = builder->layout_intent_count;
    materialized->extent_count = materialized->layout_count ? 1u : 0u;
    materialized->layouts = (XrTargetLayoutRecord *) allocate_records(
        materialized->layout_count, sizeof(*materialized->layouts));
    materialized->extents = (XrTargetExtentRecord *) allocate_records(
        materialized->extent_count, sizeof(*materialized->extents));
    if ((materialized->layout_count && !materialized->layouts) ||
        (materialized->extent_count && !materialized->extents))
        return fail(error, error_size, "XR_EXEC_5003", "layout materialization failed");
    if (materialized->extent_count) {
        materialized->extents[0].id = 0;
        materialized->extents[0].kind = XR_TARGET_EXTENT_FIXED;
        materialized->extents[0].element_layout = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < materialized->layout_count; i++) {
        const XrTargetLayoutIntent *intent = &builder->layout_intents[i];
        if ((i && builder->layout_intents[i - 1u].semantic_type == intent->semantic_type) ||
            find_rep_id(materialized, &intent->memory_rep) < 0)
            return fail(error, error_size, "XR_TARGET_1002", "layout intents are ambiguous or unrepresented");
        materialized->layouts[i] = (XrTargetLayoutRecord) {
            .id = i,
            .semantic_type = intent->semantic_type,
            .kind = XR_TARGET_LAYOUT_SCALAR,
            .align = intent->memory_rep.memory_align,
            .fixed_prefix_size = intent->memory_rep.memory_size,
            .extent = 0,
        };
    }
    return true;
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
            int register_rep = find_rep_id(materialized, &intent->register_rep);
            int memory_rep = find_rep_id(materialized, &intent->memory_rep);
            uint32_t aligned = 0;
            if (register_rep < 0 || memory_rep < 0 ||
                !checked_align_u32(offset, intent->memory_rep.memory_align, &aligned) ||
                intent->memory_rep.memory_size > UINT32_MAX - aligned)
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
                .size = intent->memory_rep.memory_size,
                .align = intent->memory_rep.memory_align,
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
        if (i && builder->value_intents[i - 1u].semantic_value == intent->semantic_value)
            return fail(error, error_size, "XR_TARGET_1001", "value representation intent is duplicated");
        int register_rep = find_rep_id(materialized, &intent->register_rep);
        int memory_rep = find_rep_id(materialized, &intent->memory_rep);
        int slot = intent->has_slot
                       ? find_slot_id(materialized, intent->semantic_function,
                                      intent->slot_identity)
                       : -1;
        if (register_rep < 0 || memory_rep < 0 || (intent->has_slot && slot < 0))
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

static bool builder_materialize(XrTargetPlanBuilder *builder,
                                XrTargetMaterializedPlan *materialized, char *error,
                                size_t error_size) {
    if (!builder || !materialized || builder->poisoned || builder->materialized ||
        builder->started_family_mask != XR_TARGET_REQUIRED_FAMILIES ||
        builder->completed_family_mask != XR_TARGET_REQUIRED_FAMILIES)
        return fail(error, error_size, "XR_TARGET_1001", "target builder family coverage is incomplete");
    if (!materialize_machine_reps(builder, materialized, error, error_size) ||
        !materialize_layouts(builder, materialized, error, error_size) ||
        !materialize_functions_and_slots(builder, materialized, error, error_size) ||
        !materialize_values(builder, materialized, error, error_size)) {
        builder->poisoned = true;
        return false;
    }
    builder->materialized = true;
    return true;
}

static bool builder_new(const XrSemanticPlan *semantic_plan, XrTargetProfile *profile,
                        XrTargetPlanBuilder **out, char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!semantic_plan || !profile || !out)
        return fail(error, error_size, "XR_TARGET_1000", "target builder input is missing");
    char nested_error[512] = {0};
    if (!xr_semantic_plan_verify(semantic_plan, nested_error, sizeof(nested_error)))
        return fail(error, error_size, "XR_TARGET_1000", "semantic plan is not verified");
    if (!xr_target_profile_verify(profile, error, error_size))
        return false;
    XrTargetPlanBuilder *builder =
        (XrTargetPlanBuilder *) xr_calloc(1, sizeof(*builder));
    if (!builder)
        return fail(error, error_size, "XR_EXEC_5003", "target builder allocation failed");
    builder->semantic_plan = xr_semantic_plan_retain((XrSemanticPlan *) semantic_plan);
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
        .functions = materialized.functions,
        .functions_count = materialized.function_count,
        .slots = materialized.slots,
        .slots_count = materialized.slot_count,
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
    xr_semantic_plan_free(builder->semantic_plan);
    xr_target_profile_free(builder->profile);
    xr_free(builder);
}

bool xr_target_plan_build(const XrSemanticPlan *semantic_plan, XrTargetProfile *profile,
                          XrTargetPlan **out, char *error, size_t error_size) {
    XrTargetPlanBuilder *builder = NULL;
    if (out)
        *out = NULL;
    if (!builder_new(semantic_plan, profile, &builder, error, error_size))
        return false;
    if (!builder_add_scalars(builder, error, error_size)) {
        builder_free(builder);
        return false;
    }
    bool frozen = builder_freeze(builder, out, error, error_size);
    builder_free(builder);
    return frozen;
}
