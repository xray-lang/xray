/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_aggregate_c_projection.c - Exact TargetPlan aggregate C projection
 */

#include "xr_target_aggregate_c_projection.h"
#include "../plan/semantic/xr_semantic_value_aggregate_shape.h"
#include "../shared/xr_native_type_core.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static uint64_t hash_word(uint64_t hash, uint64_t word) {
    for (uint32_t i = 0; i < 8; i++) {
        hash ^= (uint8_t) (word >> (i * 8u));
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t hash_string(uint64_t hash, const char *value) {
    if (value) {
        for (const unsigned char *p = (const unsigned char *) value; *p; p++) {
            hash ^= *p;
            hash *= UINT64_C(1099511628211);
        }
    }
    hash ^= UINT64_C(0xff);
    return hash * UINT64_C(1099511628211);
}

static bool scalar_native_type(uint16_t machine_kind, uint8_t *out) {
    if (!out)
        return false;
    switch ((XrMachineRepKind) machine_kind) {
        case XR_MACHINE_REP_I1: *out = XR_NATIVE_BOOL; return true;
        case XR_MACHINE_REP_I8: *out = XR_NATIVE_I8; return true;
        case XR_MACHINE_REP_U8: *out = XR_NATIVE_U8; return true;
        case XR_MACHINE_REP_I16: *out = XR_NATIVE_I16; return true;
        case XR_MACHINE_REP_U16: *out = XR_NATIVE_U16; return true;
        case XR_MACHINE_REP_I32: *out = XR_NATIVE_I32; return true;
        case XR_MACHINE_REP_U32: *out = XR_NATIVE_U32; return true;
        case XR_MACHINE_REP_I64: *out = XR_NATIVE_I64; return true;
        case XR_MACHINE_REP_U64: *out = XR_NATIVE_U64; return true;
        case XR_MACHINE_REP_ISIZE: *out = XR_NATIVE_ISIZE; return true;
        case XR_MACHINE_REP_USIZE: *out = XR_NATIVE_USIZE; return true;
        case XR_MACHINE_REP_F32: *out = XR_NATIVE_F32; return true;
        case XR_MACHINE_REP_F64: *out = XR_NATIVE_F64; return true;
        default: return false;
    }
}

bool xr_c_aggregate_projection(const XrTargetPlan *target_plan,
                               const XrTargetValueRepRecord *binding,
                               XrCAggregateProjection *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(target_plan, binding->memory_rep) : NULL;
    uint32_t layout_count = 0;
    uint32_t field_count = 0;
    const XrTargetLayoutRecord *layouts =
        xr_target_plan_layouts(target_plan, &layout_count);
    const XrTargetFieldRecord *fields =
        xr_target_plan_fields(target_plan, &field_count);
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_plan(target_plan);
    const XrTargetProfile *profile = xr_target_plan_profile(target_plan);
    const XrTargetMachineFacts *machine =
        xr_target_profile_machine_facts(profile);
    if (!target_plan || !binding || !out || !register_rep || !memory_rep ||
        !layouts || !fields || !semantic || !machine ||
        register_rep->kind != XR_MACHINE_REP_AGGREGATE ||
        memory_rep->kind != XR_MACHINE_REP_AGGREGATE ||
        register_rep->detail != memory_rep->detail ||
        register_rep->detail >= layout_count)
        return false;
    const XrTargetLayoutRecord *layout = &layouts[register_rep->detail];
    XrSemanticValueAggregateShape shape = {0};
    const XrSemanticSourceClassRecord *declaration = NULL;
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(semantic, layout->semantic_type);
    uint32_t metadata_count = 0;
    const char *const *metadata =
        xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!type || layout->kind != XR_TARGET_LAYOUT_AGGREGATE ||
        layout->field_count == 0 ||
        layout->field_begin > field_count ||
        layout->field_count > field_count - layout->field_begin ||
        !xr_semantic_value_aggregate_shape_for_type(
            semantic, layout->semantic_type, &shape) ||
        shape.field_count != layout->field_count ||
        shape.field_metadata_begin > metadata_count ||
        shape.field_count > metadata_count - shape.field_metadata_begin)
        return false;
    declaration = xr_semantic_plan_source_class(semantic, shape.source_class);
    if (!declaration || !declaration->name || !declaration->name[0] || !metadata)
        return false;

    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_word(hash, machine->data_layout.stable_hash);
    hash = hash_word(hash, 0); /* XR_AGG_LAYOUT_STRUCT */
    hash = hash_word(hash, type->aggregate_align);
    hash = hash_word(hash, layout->fixed_prefix_size);
    hash = hash_word(hash, layout->align);
    hash = hash_word(hash, layout->field_count);
    hash = hash_string(hash, declaration->name);
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const XrTargetFieldRecord *field = &fields[layout->field_begin + i];
        const XrTargetMachineRepRecord *field_rep =
            xr_target_plan_machine_rep(target_plan, field->memory_rep);
        uint8_t native_type = 0;
        const char *name = metadata[shape.field_metadata_begin + i];
        if (!field_rep || !scalar_native_type(field_rep->kind, &native_type) ||
            field->layout != register_rep->detail || field->semantic_field != i ||
            field->semantic_name != shape.field_metadata_begin + i ||
            !name || !name[0] || field->root_kind != XR_TARGET_ROOT_NONE ||
            field->flags != 0 || field->reserved != 0)
            return false;
        hash = hash_string(hash, name);
        hash = hash_word(hash, field->offset);
        hash = hash_word(hash, native_type);
        hash = hash_word(hash, field->size);
        hash = hash_word(hash, 0); /* no fixed-array element kind */
        hash = hash_word(hash, 0); /* no fixed-array element count */
        hash = hash_word(hash, 0); /* no flexible tail */
    }
    if (!hash)
        hash = UINT64_C(1);
    int written = snprintf(out->c_type, sizeof(out->c_type),
                           "xrt_struct_abi_%016" PRIx64, hash);
    if (written <= 0 || (size_t) written >= sizeof(out->c_type)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->layout = register_rep->detail;
    out->abi_key = hash;
    return true;
}
