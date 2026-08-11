/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_builder.c - Composable TargetPlan construction
 */

#include "xr_target_builder.h"
#include "xr_target_plan_internal.h"
#include "../../base/xmalloc.h"
#include "../semantic/xr_semantic_verify.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

typedef enum XrTargetScalarEligibility {
    XR_TARGET_SCALAR_INVALID = -1,
    XR_TARGET_SCALAR_NOT_APPLICABLE = 0,
    XR_TARGET_SCALAR_VALUE = 1,
} XrTargetScalarEligibility;

typedef struct XrTargetScalarBuildStorage {
    uint8_t *defined_values;
    uint8_t *used_types;
    uint8_t *used_rep_kinds;
    uint32_t *value_types;
    uint32_t *value_functions;
    uint16_t *type_rep_kinds;
    uint16_t rep_ids[XR_MACHINE_REP_COUNT];
    XrTargetMachineRepRecord *machine_reps;
    XrTargetValueRepRecord *value_reps;
    XrTargetExtentRecord *extents;
    XrTargetLayoutRecord *layouts;
    XrTargetFunctionRecord *functions;
    XrTargetSlotRecord *slots;
} XrTargetScalarBuildStorage;

typedef struct XrTargetPlanBuilder XrTargetPlanBuilder;

struct XrTargetPlanBuilder {
    XrSemanticPlan *semantic_plan;
    XrTargetProfile *profile;
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
    uint64_t completed_family_mask;
    bool scalars_started;
    bool poisoned;
};

static void builder_free(XrTargetPlanBuilder *builder);

static bool fail(char *error, size_t error_size, const char *code, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "%s: %s", code, detail);
    return false;
}

static void dispose_storage(XrTargetScalarBuildStorage *storage) {
    if (!storage)
        return;
    xr_free(storage->defined_values);
    xr_free(storage->used_types);
    xr_free(storage->used_rep_kinds);
    xr_free(storage->value_types);
    xr_free(storage->value_functions);
    xr_free(storage->type_rep_kinds);
    xr_free(storage->machine_reps);
    xr_free(storage->value_reps);
    xr_free(storage->extents);
    xr_free(storage->layouts);
    xr_free(storage->functions);
    xr_free(storage->slots);
    memset(storage, 0, sizeof(*storage));
}

static void *allocate_records(uint32_t count, size_t size) {
    if (!count)
        return NULL;
    if (size && count > SIZE_MAX / size)
        return NULL;
    return xr_calloc(count, size);
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
            if (type->scalar_rep != 0)
                return XR_TARGET_SCALAR_INVALID;
            *out_kind = XR_MACHINE_REP_I1;
            return XR_TARGET_SCALAR_VALUE;
        case XR_KIND_RUNE:
            if (type->scalar_rep != 0)
                return XR_TARGET_SCALAR_INVALID;
            *out_kind = XR_MACHINE_REP_RUNE;
            return XR_TARGET_SCALAR_VALUE;
        case XR_KIND_UNIT:
        case XR_KIND_NEVER:
            if (type->scalar_rep != 0)
                return XR_TARGET_SCALAR_INVALID;
            *out_kind = XR_MACHINE_REP_VOID;
            return XR_TARGET_SCALAR_VALUE;
        default:
            return XR_TARGET_SCALAR_NOT_APPLICABLE;
    }
}

static bool checked_align_u32(uint32_t value, uint32_t alignment, uint32_t *out) {
    if (!alignment || (alignment & (alignment - 1u)) != 0 || value > UINT32_MAX - alignment + 1u)
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

static bool fill_machine_rep(const XrTargetProfileDraft *profile, uint32_t id, uint16_t kind,
                             XrTargetMachineRepRecord *out) {
    memset(out, 0, sizeof(*out));
    out->id = id;
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

static bool note_value(const XrSemanticPlan *semantic_plan, XrTargetScalarBuildStorage *storage,
                       uint32_t total_values, uint32_t semantic_value, uint32_t semantic_type,
                       uint32_t semantic_function, uint32_t *value_rep_count,
                       uint32_t *slot_count, uint32_t *layout_count, char *error,
                       size_t error_size) {
    size_t type_count = xr_semantic_plan_type_count(semantic_plan);
    size_t function_count = xr_semantic_plan_function_count(semantic_plan);
    if (semantic_value >= total_values || semantic_type >= type_count ||
        semantic_function >= function_count)
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic scalar value identity is out of range");
    if (storage->defined_values[semantic_value]) {
        if (storage->value_types[semantic_value] != semantic_type ||
            storage->value_functions[semantic_value] != semantic_function)
            return fail(error, error_size, "XR_TARGET_1001",
                        "semantic scalar value identity is ambiguous");
        return true;
    }
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic_plan, semantic_type);
    uint16_t kind = XR_MACHINE_REP_COUNT;
    XrTargetScalarEligibility eligibility = classify_scalar_type(type, &kind);
    if (eligibility == XR_TARGET_SCALAR_INVALID)
        return fail(error, error_size, "XR_TARGET_1001",
                    "semantic scalar type has no exact machine representation");
    storage->defined_values[semantic_value] = 1;
    storage->value_types[semantic_value] = semantic_type;
    storage->value_functions[semantic_value] = semantic_function;
    storage->type_rep_kinds[semantic_type] = kind;
    if (eligibility == XR_TARGET_SCALAR_NOT_APPLICABLE)
        return true;
    if (*value_rep_count == UINT32_MAX)
        return fail(error, error_size, "XR_EXEC_5003", "scalar value table budget overflow");
    (*value_rep_count)++;
    storage->used_rep_kinds[kind] = 1;
    if (kind != XR_MACHINE_REP_VOID) {
        if (*slot_count == UINT32_MAX)
            return fail(error, error_size, "XR_EXEC_5003", "scalar slot table budget overflow");
        (*slot_count)++;
        if (!storage->used_types[semantic_type]) {
            storage->used_types[semantic_type] = 1;
            (*layout_count)++;
        }
    }
    return true;
}

static bool collect_scalar_values(const XrSemanticPlan *semantic_plan,
                                  XrTargetScalarBuildStorage *storage, uint32_t total_values,
                                  uint32_t *value_rep_count, uint32_t *slot_count,
                                  uint32_t *layout_count, char *error, size_t error_size) {
    uint32_t parameter_count = (uint32_t) xr_semantic_plan_parameter_count(semantic_plan);
    for (uint32_t i = 0; i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(semantic_plan, i);
        if (!parameter || !note_value(semantic_plan, storage, total_values, parameter->value,
                                      parameter->type, parameter->function, value_rep_count,
                                      slot_count, layout_count, error, error_size))
            return false;
    }
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic_plan);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic_plan, i);
        if (!operation)
            return fail(error, error_size, "XR_TARGET_1001", "semantic operation is missing");
        if (operation->result_value != XR_SEMANTIC_INDEX_NONE &&
            !note_value(semantic_plan, storage, total_values, operation->result_value,
                        operation->result_type, operation->function, value_rep_count, slot_count,
                        layout_count, error, error_size))
            return false;
    }
    return true;
}

static bool allocate_output_tables(XrTargetScalarBuildStorage *storage,
                                   uint32_t machine_rep_count,
                                   uint32_t value_rep_count, uint32_t layout_count,
                                   uint32_t function_count, uint32_t slot_count, char *error,
                                   size_t error_size) {
    storage->machine_reps = (XrTargetMachineRepRecord *) allocate_records(
        machine_rep_count, sizeof(*storage->machine_reps));
    storage->value_reps = (XrTargetValueRepRecord *) allocate_records(
        value_rep_count, sizeof(*storage->value_reps));
    storage->extents = (XrTargetExtentRecord *) allocate_records(
        layout_count ? 1u : 0u, sizeof(*storage->extents));
    storage->layouts = (XrTargetLayoutRecord *) allocate_records(
        layout_count, sizeof(*storage->layouts));
    storage->functions = (XrTargetFunctionRecord *) allocate_records(
        function_count, sizeof(*storage->functions));
    storage->slots = (XrTargetSlotRecord *) allocate_records(slot_count, sizeof(*storage->slots));
    if (!storage->machine_reps || (value_rep_count && !storage->value_reps) ||
        (layout_count && (!storage->extents || !storage->layouts)) ||
        (function_count && !storage->functions) || (slot_count && !storage->slots))
        return fail(error, error_size, "XR_EXEC_5003", "scalar target table allocation failed");
    return true;
}

static bool fill_machine_reps(const XrTargetProfileDraft *profile,
                              XrTargetScalarBuildStorage *storage, uint32_t machine_rep_count,
                              char *error, size_t error_size) {
    for (uint32_t kind = 0; kind < XR_MACHINE_REP_COUNT; kind++)
        storage->rep_ids[kind] = UINT16_MAX;
    uint32_t next = 0;
    for (uint16_t kind = 0; kind < XR_MACHINE_REP_COUNT; kind++) {
        if (!storage->used_rep_kinds[kind])
            continue;
        if (next >= machine_rep_count ||
            !fill_machine_rep(profile, next, kind, &storage->machine_reps[next]))
            return fail(error, error_size, "XR_TARGET_1001",
                        "target profile cannot materialize a scalar representation");
        storage->rep_ids[kind] = (uint16_t) next++;
    }
    return next == machine_rep_count;
}

static bool fill_layouts(const XrSemanticPlan *semantic_plan,
                         XrTargetScalarBuildStorage *storage, uint32_t layout_count, char *error,
                         size_t error_size) {
    if (layout_count) {
        storage->extents[0].id = 0;
        storage->extents[0].kind = XR_TARGET_EXTENT_FIXED;
        storage->extents[0].element_layout = XR_SEMANTIC_INDEX_NONE;
    }
    uint32_t next = 0;
    uint32_t type_count = (uint32_t) xr_semantic_plan_type_count(semantic_plan);
    for (uint32_t type = 0; type < type_count; type++) {
        if (!storage->used_types[type])
            continue;
        uint16_t kind = storage->type_rep_kinds[type];
        uint16_t rep = kind < XR_MACHINE_REP_COUNT ? storage->rep_ids[kind] : UINT16_MAX;
        if (rep == UINT16_MAX || next >= layout_count)
            return fail(error, error_size, "XR_TARGET_1002",
                        "scalar layout has no machine representation");
        XrTargetLayoutRecord *layout = &storage->layouts[next];
        layout->id = next;
        layout->semantic_type = type;
        layout->kind = XR_TARGET_LAYOUT_SCALAR;
        layout->align = storage->machine_reps[rep].memory_align;
        layout->fixed_prefix_size = storage->machine_reps[rep].memory_size;
        layout->extent = 0;
        next++;
    }
    return next == layout_count;
}

static bool fill_values_and_slots(const XrSemanticPlan *semantic_plan,
                                  XrTargetScalarBuildStorage *storage, uint32_t value_rep_count,
                                  uint32_t slot_count, char *error, size_t error_size) {
    uint32_t next_value_rep = 0;
    uint32_t next_slot = 0;
    uint32_t function_count = (uint32_t) xr_semantic_plan_function_count(semantic_plan);
    for (uint32_t function = 0; function < function_count; function++) {
        const XrSemanticFunctionRecord *semantic_function =
            xr_semantic_plan_function(semantic_plan, function);
        if (!semantic_function)
            return fail(error, error_size, "XR_TARGET_1002", "semantic function is missing");
        XrTargetFunctionRecord *target_function = &storage->functions[function];
        target_function->id = function;
        target_function->semantic_function = function;
        target_function->slot_begin = next_slot;
        uint32_t offset = 0;
        for (uint32_t local = 0; local < semantic_function->value_count; local++) {
            uint32_t semantic_value = semantic_function->value_begin + local;
            if (!storage->defined_values[semantic_value])
                continue;
            uint32_t type = storage->value_types[semantic_value];
            uint16_t kind = storage->type_rep_kinds[type];
            if (kind >= XR_MACHINE_REP_COUNT || !storage->used_rep_kinds[kind])
                continue;
            uint16_t rep = storage->rep_ids[kind];
            if (rep == UINT16_MAX || next_value_rep >= value_rep_count)
                return fail(error, error_size, "XR_TARGET_1001",
                            "scalar value has no machine representation");
            XrTargetValueRepRecord *binding = &storage->value_reps[next_value_rep++];
            binding->semantic_value = semantic_value;
            binding->register_rep = rep;
            binding->memory_rep = rep;
            binding->slot = XR_SEMANTIC_INDEX_NONE;
            if (kind == XR_MACHINE_REP_VOID)
                continue;
            if (next_slot >= slot_count)
                return fail(error, error_size, "XR_EXEC_5003", "scalar slot table overflow");
            const XrTargetMachineRepRecord *machine = &storage->machine_reps[rep];
            uint32_t aligned = 0;
            if (!checked_align_u32(offset, machine->memory_align, &aligned) ||
                machine->memory_size > UINT32_MAX - aligned)
                return fail(error, error_size, "XR_EXEC_5003",
                            "scalar function slot layout overflows");
            XrTargetSlotRecord *slot = &storage->slots[next_slot];
            slot->id = next_slot;
            slot->function = function;
            slot->offset = aligned;
            slot->size = machine->memory_size;
            slot->align = machine->memory_align;
            slot->register_rep = rep;
            slot->memory_rep = rep;
            slot->ownership = XR_TARGET_OWNERSHIP_TRIVIAL;
            slot->debug_variable = XR_SEMANTIC_INDEX_NONE;
            binding->slot = next_slot++;
            offset = aligned + machine->memory_size;
        }
        target_function->slot_count = next_slot - target_function->slot_begin;
    }
    if (next_value_rep != value_rep_count || next_slot != slot_count)
        return fail(error, error_size, "XR_TARGET_1001",
                    "scalar target tables do not exactly cover their inputs");
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

static bool builder_add_scalars(XrTargetPlanBuilder *builder, char *error,
                                size_t error_size) {
    XrTargetScalarBuildStorage storage = {0};
    if (!builder || !builder->semantic_plan || !builder->profile)
        return fail(error, error_size, "XR_TARGET_1000", "scalar target builder is missing");
    if (builder->scalars_started || builder->poisoned)
        return fail(error, error_size, "XR_TARGET_1001",
                    "scalar target collector is repeated or poisoned");
    builder->scalars_started = true;
    const XrSemanticPlan *semantic_plan = builder->semantic_plan;
    XrTargetProfile *profile = builder->profile;
    size_t type_count_size = xr_semantic_plan_type_count(semantic_plan);
    size_t function_count_size = xr_semantic_plan_function_count(semantic_plan);
    size_t parameter_count_size = xr_semantic_plan_parameter_count(semantic_plan);
    size_t operation_count_size = xr_semantic_plan_operation_count(semantic_plan);
    if (type_count_size > UINT32_MAX || function_count_size > 100000u ||
        parameter_count_size > UINT32_MAX || operation_count_size > UINT32_MAX)
        return fail(error, error_size, "XR_EXEC_5003", "scalar target input exceeds hard budgets");
    uint32_t type_count = (uint32_t) type_count_size;
    uint32_t function_count = (uint32_t) function_count_size;
    uint32_t total_values = 0;
    if (function_count) {
        const XrSemanticFunctionRecord *last =
            xr_semantic_plan_function(semantic_plan, function_count - 1u);
        if (!last || last->value_begin > UINT32_MAX - last->value_count)
            return fail(error, error_size, "XR_EXEC_5003",
                        "semantic value identity budget overflow");
        total_values = last->value_begin + last->value_count;
    }
    if (total_values > 40000000u || type_count > 1000000u)
        return fail(error, error_size, "XR_EXEC_5003", "scalar target tables exceed hard budgets");
    storage.defined_values = (uint8_t *) allocate_records(total_values, sizeof(uint8_t));
    storage.used_types = (uint8_t *) allocate_records(type_count, sizeof(uint8_t));
    storage.used_rep_kinds =
        (uint8_t *) allocate_records(XR_MACHINE_REP_COUNT, sizeof(uint8_t));
    storage.value_types = (uint32_t *) allocate_records(total_values, sizeof(uint32_t));
    storage.value_functions = (uint32_t *) allocate_records(total_values, sizeof(uint32_t));
    storage.type_rep_kinds = (uint16_t *) allocate_records(type_count, sizeof(uint16_t));
    if ((total_values && (!storage.defined_values || !storage.value_types ||
                          !storage.value_functions)) ||
        (type_count && (!storage.used_types || !storage.type_rep_kinds)) ||
        !storage.used_rep_kinds) {
        dispose_storage(&storage);
        return fail(error, error_size, "XR_EXEC_5003", "scalar target analysis allocation failed");
    }
    for (uint32_t type = 0; type < type_count; type++)
        storage.type_rep_kinds[type] = XR_MACHINE_REP_COUNT;
    storage.used_rep_kinds[XR_MACHINE_REP_VOID] = 1;
    uint32_t value_rep_count = 0;
    uint32_t slot_count = 0;
    uint32_t layout_count = 0;
    if (!collect_scalar_values(semantic_plan, &storage, total_values, &value_rep_count, &slot_count,
                               &layout_count, error, error_size)) {
        dispose_storage(&storage);
        return false;
    }
    if (slot_count > 16000000u || layout_count > 1000000u) {
        dispose_storage(&storage);
        return fail(error, error_size, "XR_EXEC_5003", "scalar target tables exceed hard budgets");
    }
    uint32_t machine_rep_count = 0;
    for (uint32_t kind = 0; kind < XR_MACHINE_REP_COUNT; kind++)
        machine_rep_count += storage.used_rep_kinds[kind] != 0;
    if (!allocate_output_tables(&storage, machine_rep_count, value_rep_count, layout_count,
                                function_count, slot_count, error, error_size) ||
        !fill_machine_reps(xr_target_profile_facts(profile), &storage, machine_rep_count, error,
                           error_size) ||
        !fill_layouts(semantic_plan, &storage, layout_count, error, error_size) ||
        !fill_values_and_slots(semantic_plan, &storage, value_rep_count, slot_count, error,
                               error_size)) {
        dispose_storage(&storage);
        return false;
    }
    builder->machine_reps = storage.machine_reps;
    builder->machine_rep_count = machine_rep_count;
    builder->value_reps = storage.value_reps;
    builder->value_rep_count = value_rep_count;
    builder->extents = storage.extents;
    builder->extent_count = layout_count ? 1u : 0u;
    builder->layouts = storage.layouts;
    builder->layout_count = layout_count;
    builder->functions = storage.functions;
    builder->function_count = function_count;
    builder->slots = storage.slots;
    builder->slot_count = slot_count;
    builder->completed_family_mask |= XR_TARGET_FAMILY_SCALAR;
    storage.machine_reps = NULL;
    storage.value_reps = NULL;
    storage.extents = NULL;
    storage.layouts = NULL;
    storage.functions = NULL;
    storage.slots = NULL;
    dispose_storage(&storage);
    return true;
}

static bool builder_freeze(const XrTargetPlanBuilder *builder, XrTargetPlan **out,
                           char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!builder || !out || !builder->semantic_plan || !builder->profile)
        return fail(error, error_size, "XR_TARGET_1000", "target builder freeze input is missing");
    if (builder->poisoned ||
        builder->completed_family_mask != XR_TARGET_REQUIRED_FAMILIES)
        return fail(error, error_size, "XR_TARGET_1001",
                    "target builder family coverage is incomplete");
    XrTargetPlanDraft draft = {
        .semantic_plan = builder->semantic_plan,
        .profile = builder->profile,
        .completed_family_mask = builder->completed_family_mask,
        .machine_reps = builder->machine_reps,
        .machine_reps_count = builder->machine_rep_count,
        .value_reps = builder->value_reps,
        .value_reps_count = builder->value_rep_count,
        .extents = builder->extents,
        .extents_count = builder->extent_count,
        .layouts = builder->layouts,
        .layouts_count = builder->layout_count,
        .functions = builder->functions,
        .functions_count = builder->function_count,
        .slots = builder->slots,
        .slots_count = builder->slot_count,
    };
    return xr_target_plan_freeze(&draft, out, error, error_size);
}

static void builder_free(XrTargetPlanBuilder *builder) {
    if (!builder)
        return;
    xr_free(builder->machine_reps);
    xr_free(builder->value_reps);
    xr_free(builder->extents);
    xr_free(builder->layouts);
    xr_free(builder->functions);
    xr_free(builder->slots);
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
        builder->poisoned = true;
        builder_free(builder);
        return false;
    }
    bool frozen = builder_freeze(builder, out, error, error_size);
    builder_free(builder);
    return frozen;
}
