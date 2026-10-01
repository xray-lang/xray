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
#include "../plan/target/xr_target_plain_ref_aggregate_shape.h"
#include "../shared/xr_native_type_core.h"
#include "../shared/xr_align_guard.h"
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

static uint64_t hash_bytes(uint64_t hash, const uint8_t *bytes, size_t size) {
    if (!bytes)
        return hash;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool scalar_native_type(uint16_t machine_kind, uint8_t *out) {
    if (!out)
        return false;
    switch ((XrMachineRepKind) machine_kind) {
        case XR_MACHINE_REP_I1:
            *out = XR_NATIVE_BOOL;
            return true;
        case XR_MACHINE_REP_I8:
            *out = XR_NATIVE_I8;
            return true;
        case XR_MACHINE_REP_U8:
            *out = XR_NATIVE_U8;
            return true;
        case XR_MACHINE_REP_I16:
            *out = XR_NATIVE_I16;
            return true;
        case XR_MACHINE_REP_U16:
            *out = XR_NATIVE_U16;
            return true;
        case XR_MACHINE_REP_I32:
            *out = XR_NATIVE_I32;
            return true;
        case XR_MACHINE_REP_U32:
        case XR_MACHINE_REP_RUNE:
            *out = XR_NATIVE_U32;
            return true;
        case XR_MACHINE_REP_I64:
            *out = XR_NATIVE_I64;
            return true;
        case XR_MACHINE_REP_U64:
            *out = XR_NATIVE_U64;
            return true;
        case XR_MACHINE_REP_ISIZE:
            *out = XR_NATIVE_ISIZE;
            return true;
        case XR_MACHINE_REP_USIZE:
            *out = XR_NATIVE_USIZE;
            return true;
        case XR_MACHINE_REP_F32:
            *out = XR_NATIVE_F32;
            return true;
        case XR_MACHINE_REP_F64:
            *out = XR_NATIVE_F64;
            return true;
        default:
            return false;
    }
}

static const char *scalar_c_type(uint16_t machine_kind) {
    switch ((XrMachineRepKind) machine_kind) {
        case XR_MACHINE_REP_I1:
            return "uint8_t";
        case XR_MACHINE_REP_I8:
            return "int8_t";
        case XR_MACHINE_REP_U8:
            return "uint8_t";
        case XR_MACHINE_REP_I16:
            return "int16_t";
        case XR_MACHINE_REP_U16:
            return "uint16_t";
        case XR_MACHINE_REP_I32:
            return "int32_t";
        case XR_MACHINE_REP_U32:
            return "uint32_t";
        case XR_MACHINE_REP_I64:
            return "int64_t";
        case XR_MACHINE_REP_U64:
            return "uint64_t";
        case XR_MACHINE_REP_ISIZE:
            return "ptrdiff_t";
        case XR_MACHINE_REP_USIZE:
            return "size_t";
        case XR_MACHINE_REP_F32:
            return "float";
        case XR_MACHINE_REP_F64:
            return "double";
        default:
            return NULL;
    }
}

static bool leaf_program_binding(const XrSemanticPlan *semantic, uint32_t semantic_type,
                                 const XrSemanticProgramTypeBinding **out) {
    const XrSemanticProgramProvenance *provenance = xr_semantic_plan_program_provenance(semantic);
    const XrSemanticProgramTypeBinding *binding =
        xr_semantic_plan_program_type_for_semantic_type(semantic, semantic_type);
    if (!provenance ||
        provenance->program_family != XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL ||
        !binding || binding->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE ||
        binding->field_count != 2 ||
        binding->field_begin > xr_semantic_plan_program_type_field_binding_count(semantic) ||
        binding->field_count >
            xr_semantic_plan_program_type_field_binding_count(semantic) - binding->field_begin)
        return false;
    if (out)
        *out = binding;
    return true;
}
static bool leaf_product_program_family(const XrSemanticProgramProvenance *provenance) {
    return provenance &&
           provenance->program_family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL;
}

static bool fixed_array_field_is_exact(
    const XrSemanticPlan *semantic, const XrTargetMachineRepRecord *reps, uint32_t rep_count,
    const XrTargetLayoutRecord *layouts, uint32_t layout_count, const XrTargetFieldRecord *fields,
    uint32_t field_count, uint32_t semantic_type, const XrTargetFieldRecord *parent,
    uint8_t *element_native, uint32_t *element_count) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, semantic_type);
    const XrTargetMachineRepRecord *rep = parent->memory_rep < rep_count ? &reps[parent->memory_rep] : NULL;
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    if (!type || !rep || !children || !xr_target_plain_ref_field_graph(semantic, semantic_type) ||
        type->kind != XR_KIND_FIXED_ARRAY || type->child_begin >= child_count ||
        rep->kind != XR_MACHINE_REP_AGGREGATE || rep->detail >= layout_count ||
        rep->root_kind != XR_TARGET_ROOT_NONE || rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
        return false;
    const XrTargetLayoutRecord *layout = &layouts[rep->detail];
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(semantic, children[type->child_begin]);
    uint16_t kind = XR_MACHINE_REP_COUNT;
    if (!element || xr_target_scalar_rep_for_type(element, false, &kind) != 1 ||
        !scalar_native_type(kind, element_native) || layout->semantic_type != semantic_type ||
        layout->kind != XR_TARGET_LAYOUT_AGGREGATE || layout->root_field_count != 0 ||
        layout->field_count != type->aggregate_extent || layout->field_begin > field_count ||
        layout->field_count > field_count - layout->field_begin ||
        parent->size != layout->fixed_prefix_size || parent->align != layout->align ||
        rep->signedness != XR_TARGET_SIGN_NONE || rep->null_encoding != XR_TARGET_NULL_NOT_NULLABLE ||
        rep->lane_count || rep->reserved || rep->register_bits != parent->size * 8u ||
        rep->memory_size != parent->size || rep->memory_align != parent->align)
        return false;
    uint32_t size = 0;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const XrTargetFieldRecord *lane = &fields[layout->field_begin + i];
        const XrTargetMachineRepRecord *lane_rep = lane->memory_rep < rep_count ? &reps[lane->memory_rep] : NULL;
        if (!lane_rep || lane_rep->kind != kind || lane_rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
            lane_rep->root_kind != XR_TARGET_ROOT_NONE || lane->layout != rep->detail ||
            lane->semantic_field != i || lane->semantic_name != XR_SEMANTIC_INDEX_NONE ||
            lane->offset != size || lane->size != lane_rep->memory_size ||
            lane->align != lane_rep->memory_align || lane->align != layout->align ||
            lane->root_kind != XR_TARGET_ROOT_NONE || lane->flags || lane->reserved ||
            lane->size > UINT32_MAX - size)
            return false;
        size += lane->size;
    }
    if (size != layout->fixed_prefix_size)
        return false;
    *element_count = layout->field_count;
    return true;
}

static bool plain_ref_scalar_geometry(const XrTargetMachineFacts *machine,
    uint8_t native, XrTargetTypeLayout *out) {
    if (!machine || !out)
        return false;
    switch (native) {
#define XR_PLAIN_SCALAR_LAYOUT(native_kind, field) case native_kind: *out = machine->data_layout.field; return true;
        XR_PLAIN_SCALAR_LAYOUT(XR_NATIVE_I8, i8)
        XR_PLAIN_SCALAR_LAYOUT(XR_NATIVE_U8, u8)
        XR_PLAIN_SCALAR_LAYOUT(XR_NATIVE_I16, i16)
        XR_PLAIN_SCALAR_LAYOUT(XR_NATIVE_U16, u16)
        XR_PLAIN_SCALAR_LAYOUT(XR_NATIVE_I32, i32)
        XR_PLAIN_SCALAR_LAYOUT(XR_NATIVE_U32, u32)
        XR_PLAIN_SCALAR_LAYOUT(XR_NATIVE_I64, i64)
        XR_PLAIN_SCALAR_LAYOUT(XR_NATIVE_U64, u64)
        XR_PLAIN_SCALAR_LAYOUT(XR_NATIVE_ISIZE, isize)
        XR_PLAIN_SCALAR_LAYOUT(XR_NATIVE_USIZE, usize)
        XR_PLAIN_SCALAR_LAYOUT(XR_NATIVE_F32, f32)
        XR_PLAIN_SCALAR_LAYOUT(XR_NATIVE_F64, f64)
        XR_PLAIN_SCALAR_LAYOUT(XR_NATIVE_BOOL, boolean)
#undef XR_PLAIN_SCALAR_LAYOUT
        default: return false;
    }
}

static bool plain_ref_scalar_rep_is_exact(const XrTargetMachineRepRecord *rep,
    uint8_t native, const XrTargetTypeLayout *geometry) {
    if (!rep || !geometry)
        return false;
    uint8_t sign = XR_TARGET_SIGN_NONE;
    switch (native) {
        case XR_NATIVE_I8: case XR_NATIVE_I16: case XR_NATIVE_I32:
        case XR_NATIVE_I64: case XR_NATIVE_ISIZE:
            sign = XR_TARGET_SIGN_SIGNED;
            break;
        case XR_NATIVE_U8: case XR_NATIVE_U16: case XR_NATIVE_U32:
        case XR_NATIVE_U64: case XR_NATIVE_USIZE:
            sign = XR_TARGET_SIGN_UNSIGNED;
            break;
        default:
            break;
    }
    return rep->register_bits == (native == XR_NATIVE_BOOL ? 1u : geometry->size * 8u) &&
        rep->memory_size == geometry->size && rep->memory_align == geometry->align &&
        rep->signedness == sign && rep->detail == 0 && rep->lane_count == 0 && rep->reserved == 0 &&
        rep->null_encoding == XR_TARGET_NULL_NOT_NULLABLE && rep->root_kind == XR_TARGET_ROOT_NONE &&
        rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
}

/* Derive offsets and final padding from the Source field graph and immutable
 * ABI facts. Hashing a self-consistent but oversized Target layout is not proof. */
static bool plain_ref_layout_is_exact(const XrTargetPlan *plan, const XrSemanticPlan *semantic,
    const XrTargetMachineFacts *machine, uint32_t layout_index) {
    uint32_t layout_count = 0, field_count = 0, rep_count = 0, child_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(plan, &layout_count);
    const XrTargetFieldRecord *fields = xr_target_plan_fields(plan, &field_count);
    const XrTargetMachineRepRecord *reps = xr_target_plan_machine_reps(plan, &rep_count);
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    const XrTargetLayoutRecord *layout = layouts && layout_index < layout_count ? &layouts[layout_index] : NULL;
    const XrSemanticTypeRecord *type = layout ? xr_semantic_plan_type(semantic, layout->semantic_type) : NULL;
    if (!layout || !type || !fields || !reps || !children || type->kind != XR_KIND_INSTANCE ||
        !xr_target_plain_ref_field_graph(semantic, layout->semantic_type) ||
        layout->kind != XR_TARGET_LAYOUT_AGGREGATE || layout->root_field_count ||
        layout->field_count != type->child_count || layout->field_begin > field_count ||
        layout->field_count > field_count - layout->field_begin)
        return false;
    uint32_t cursor = 0, alignment = 1;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const XrTargetFieldRecord *field = &fields[layout->field_begin + i];
        const XrTargetMachineRepRecord *rep = field->memory_rep < rep_count ? &reps[field->memory_rep] : NULL;
        const XrSemanticTypeRecord *child = xr_semantic_plan_type(semantic, children[type->child_begin + i]);
        uint16_t kind = XR_MACHINE_REP_COUNT;
        uint8_t native = 0;
        XrTargetTypeLayout geometry = {0};
        if (!child || !rep)
            return false;
        if (child->kind == XR_KIND_FIXED_ARRAY) {
            uint32_t count = 0;
            if (!fixed_array_field_is_exact(semantic, reps, rep_count, layouts, layout_count,
                fields, field_count, children[type->child_begin + i], field, &native, &count) ||
                !plain_ref_scalar_geometry(machine, native, &geometry) || !count ||
                geometry.size > UINT32_MAX / count)
                return false;
            const XrTargetLayoutRecord *lanes = &layouts[rep->detail];
            for (uint16_t n = 0; n < lanes->field_count; n++) {
                const XrTargetFieldRecord *lane = &fields[lanes->field_begin + n];
                const XrTargetMachineRepRecord *lane_rep = &reps[lane->memory_rep];
                if (lane->size != geometry.size || lane->align != geometry.align ||
                    !plain_ref_scalar_rep_is_exact(lane_rep, native, &geometry))
                    return false;
            }
            geometry.size *= count;
        } else if (xr_target_scalar_rep_for_type(child, false, &kind) != XR_TARGET_SCALAR_REP_EXACT ||
            rep->kind != kind || !scalar_native_type(kind, &native) ||
            !plain_ref_scalar_geometry(machine, native, &geometry) ||
            !plain_ref_scalar_rep_is_exact(rep, native, &geometry)) {
            return false;
        }
        uint32_t offset = 0;
        if (!xr_checked_align_u32(cursor, geometry.align, &offset) || geometry.size > UINT32_MAX - offset ||
            field->offset != offset || field->size != geometry.size || field->align != geometry.align ||
            rep->memory_size != geometry.size || rep->memory_align != geometry.align ||
            rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL || rep->root_kind != XR_TARGET_ROOT_NONE)
            return false;
        if (child->kind != XR_KIND_FIXED_ARRAY &&
            (rep->detail || rep->lane_count || rep->reserved ||
             rep->null_encoding != XR_TARGET_NULL_NOT_NULLABLE))
            return false;
        cursor = offset + geometry.size;
        if (geometry.align > alignment)
            alignment = geometry.align;
    }
    if (type->aggregate_align > alignment)
        alignment = type->aggregate_align;
    uint32_t size = 0;
    return xr_checked_align_u32(cursor, alignment, &size) &&
        layout->fixed_prefix_size == size && layout->align == alignment;
}

static bool named_struct_projection_hash_depth(
    const XrSemanticPlan *semantic, const XrTargetMachineFacts *machine,
    const XrTargetMachineRepRecord *machine_reps, uint32_t machine_rep_count,
    const XrTargetLayoutRecord *layouts, uint32_t layout_count, const XrTargetFieldRecord *fields,
    uint32_t field_count, const char *const *metadata, uint32_t metadata_count,
    uint32_t layout_index, uint32_t *type_stack, uint32_t depth, uint64_t *hash_out) {
    if (!semantic || !machine || !machine_reps || !layouts || !fields || !metadata || !type_stack ||
        !hash_out || layout_index >= layout_count || depth >= 64u)
        return false;
    const XrTargetLayoutRecord *layout = &layouts[layout_index];
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, layout->semantic_type);
    XrSemanticValueAggregateShape shape = {0};
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    if (!type || !children || layout->kind != XR_TARGET_LAYOUT_AGGREGATE ||
        layout->field_count == 0 || layout->field_begin > field_count ||
        layout->field_count > field_count - layout->field_begin ||
        !xr_semantic_value_aggregate_shape_for_type(semantic, layout->semantic_type, &shape) ||
        shape.field_count != layout->field_count || shape.field_metadata_begin > metadata_count ||
        shape.field_count > metadata_count - shape.field_metadata_begin ||
        type->child_begin > child_count || type->child_count > child_count - type->child_begin ||
        type->child_count != layout->field_count)
        return false;
    for (uint32_t i = 0; i < depth; ++i)
        if (type_stack[i] == layout->semantic_type)
            return false;
    type_stack[depth] = layout->semantic_type;
    const XrSemanticSourceClassRecord *declaration =
        xr_semantic_plan_source_class(semantic, shape.source_class);
    if (!declaration || !declaration->name || !declaration->name[0])
        return false;

    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_word(hash, machine->data_layout.stable_hash);
    hash = hash_word(hash, 0); /* XR_AGG_LAYOUT_STRUCT */
    hash = hash_word(hash, type->aggregate_align);
    hash = hash_word(hash, layout->fixed_prefix_size);
    hash = hash_word(hash, layout->align);
    hash = hash_word(hash, layout->field_count);
    hash = hash_string(hash, declaration->name);
    for (uint16_t i = 0; i < layout->field_count; ++i) {
        const XrTargetFieldRecord *field = &fields[layout->field_begin + i];
        const XrTargetMachineRepRecord *field_rep =
            field->memory_rep < machine_rep_count ? &machine_reps[field->memory_rep] : NULL;
        const char *name = metadata[shape.field_metadata_begin + i];
        uint8_t native_type = 0;
        uint64_t nested_hash = 0;
        const XrSemanticTypeRecord *field_type =
            xr_semantic_plan_type(semantic, children[type->child_begin + i]);
        uint8_t element_native = 0;
        uint32_t element_count = 0;
        bool fixed_array = field_type && field_type->kind == XR_KIND_FIXED_ARRAY;
        bool nested = field_rep && field_rep->kind == XR_MACHINE_REP_AGGREGATE && !fixed_array;
        if (fixed_array) {
            native_type = XR_NATIVE_ARRAY;
            if (!fixed_array_field_is_exact(semantic, machine_reps, machine_rep_count, layouts,
                    layout_count, fields, field_count, children[type->child_begin + i], field,
                    &element_native, &element_count))
                return false;
        } else if (nested) {
            native_type = XR_NATIVE_NESTED_AGGREGATE;
            if (field_rep->detail >= layout_count ||
                layouts[field_rep->detail].semantic_type != children[type->child_begin + i] ||
                !named_struct_projection_hash_depth(
                    semantic, machine, machine_reps, machine_rep_count, layouts, layout_count,
                    fields, field_count, metadata, metadata_count, field_rep->detail, type_stack,
                    depth + 1u, &nested_hash))
                return false;
        } else if (!field_rep || !scalar_native_type(field_rep->kind, &native_type)) {
            return false;
        }
        if (field->layout != layout_index || field->semantic_field != i ||
            field->semantic_name != shape.field_metadata_begin + i || !name || !name[0] ||
            field->root_kind != XR_TARGET_ROOT_NONE || field->flags != 0 || field->reserved != 0)
            return false;
        hash = hash_string(hash, name);
        hash = hash_word(hash, field->offset);
        hash = hash_word(hash, native_type);
        hash = hash_word(hash, field->size);
        hash = hash_word(hash, element_native);
        hash = hash_word(hash, element_count);
        hash = hash_word(hash, 0); /* no flexible tail */
        if (nested)
            hash = hash_word(hash, nested_hash);
    }
    *hash_out = hash ? hash : UINT64_C(1);
    return true;
}

bool xr_c_leaf_aggregate_projection(const XrTargetPlan *target_plan, const XrSemanticPlan *semantic,
                                    uint32_t semantic_type, XrCAggregateProjection *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    const XrSemanticProgramTypeBinding *binding = NULL;
    const XrTargetProfile *profile = xr_target_plan_profile(target_plan);
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(profile);
    uint32_t layout_count = 0, field_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(target_plan, &layout_count);
    const XrTargetFieldRecord *fields = xr_target_plan_fields(target_plan, &field_count);
    if (!target_plan || !out || !xr_target_plan_is_verified(target_plan) || !semantic || !machine ||
        !layouts || !fields || !leaf_program_binding(semantic, semantic_type, &binding))
        return false;

    uint32_t partition_index = 0, partition_count = 0;
    if (!xr_target_plan_partition_for_semantic(target_plan, semantic, &partition_index))
        return false;
    const XrTargetModulePartitionRecord *partitions =
        xr_target_plan_module_partitions(target_plan, &partition_count);
    uint32_t layout_begin = 0, layout_end = layout_count;
    if (partition_count) {
        if (!partitions || partition_index >= partition_count)
            return false;
        const XrTargetModulePartitionRecord *partition = &partitions[partition_index];
        if (partition->layouts_begin > layout_count ||
            partition->layouts_count > layout_count - partition->layouts_begin)
            return false;
        layout_begin = partition->layouts_begin;
        layout_end = layout_begin + partition->layouts_count;
    }
    const XrTargetLayoutRecord *layout = NULL;
    uint32_t layout_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = layout_begin; i < layout_end; i++) {
        if (layouts[i].semantic_type != semantic_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
        layout_index = i;
    }
    if (!layout || layout->kind != XR_TARGET_LAYOUT_AGGREGATE || layout->fixed_prefix_size != 16 ||
        layout->align != 8 || layout->root_field_count != 0 ||
        layout->field_count != binding->field_count || layout->field_begin > field_count ||
        layout->field_count > field_count - layout->field_begin)
        return false;

    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_word(hash, machine->data_layout.stable_hash);
    hash = hash_word(hash, UINT64_C(3)); /* typed leaf-program value aggregate */
    hash = hash_bytes(hash, binding->program_type.bytes, sizeof(binding->program_type.bytes));
    hash = hash_bytes(hash, binding->source_class_identity.bytes,
                      sizeof(binding->source_class_identity.bytes));
    hash = hash_word(hash, layout->fixed_prefix_size);
    hash = hash_word(hash, layout->align);
    hash = hash_word(hash, layout->field_count);
    for (uint32_t ordinal = 0; ordinal < layout->field_count; ordinal++) {
        const XrTargetFieldRecord *field = &fields[layout->field_begin + ordinal];
        const XrSemanticProgramTypeFieldBinding *program_field =
            xr_semantic_plan_program_type_field_binding(semantic, binding->field_begin + ordinal);
        const XrTargetMachineRepRecord *field_rep =
            xr_target_plan_machine_rep(target_plan, field->memory_rep);
        uint8_t native_type = 0;
        if (!program_field || !field_rep ||
            program_field->owner_program_row != binding->program_row ||
            program_field->declaration_ordinal != ordinal || field->layout != layout_index ||
            field->semantic_field != ordinal || field->semantic_name != XR_SEMANTIC_INDEX_NONE ||
            !scalar_native_type(field_rep->kind, &native_type) ||
            field_rep->kind != XR_MACHINE_REP_I64 || field_rep->memory_size != 8 ||
            field_rep->memory_align != 8 || field_rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
            field->offset != ordinal * 8u || field->size != 8 || field->align != 8 ||
            field->root_kind != XR_TARGET_ROOT_NONE || field->flags != 0 || field->reserved != 0)
            return false;
        hash = hash_bytes(hash, program_field->program_field_type.bytes,
                          sizeof(program_field->program_field_type.bytes));
        hash = hash_word(hash, field->offset);
        hash = hash_word(hash, field->size);
        hash = hash_word(hash, field->align);
        hash = hash_word(hash, field_rep->kind);
    }
    if (!hash)
        hash = UINT64_C(1);
    int written = snprintf(out->c_type, sizeof(out->c_type), "xrt_struct_abi_%016" PRIx64, hash);
    if (written <= 0 || (size_t) written >= sizeof(out->c_type)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->layout = layout_index;
    out->abi_key = hash;
    out->kind = XR_C_AGGREGATE_PROJECTION_NAMED_STRUCT;
    return true;
}

static bool
fixed_array_projection(const XrTargetPlan *target_plan, const XrTargetValueRepRecord *binding,
                       const XrTargetMachineRepRecord *aggregate_rep,
                       const XrTargetLayoutRecord *layout, const XrTargetFieldRecord *fields,
                       uint32_t field_count, const XrSemanticTypeRecord *type,
                       const XrTargetMachineFacts *machine, XrCAggregateProjection *out) {
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        slots && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    if (!slot || type->kind != XR_KIND_FIXED_ARRAY || type->child_count != 1 ||
        type->aggregate_extent == 0 || type->aggregate_extent > UINT16_MAX ||
        type->aggregate_extent != layout->field_count ||
        (type->flags & XR_SEM_TYPE_NULLABLE) != 0 || type->scalar_rep != XR_SCALAR_REP_NONE ||
        layout->field_begin > field_count ||
        layout->field_count > field_count - layout->field_begin ||
        slot->semantic_value != binding->semantic_value ||
        slot->register_rep != binding->register_rep || slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_NONE || slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
        return false;

    uint8_t native_type = 0;
    uint16_t machine_kind = XR_MACHINE_REP_COUNT;
    const char *element_c_type = NULL;
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_word(hash, machine->data_layout.stable_hash);
    hash = hash_word(hash, 1); /* fixed-array backing-place projection */
    hash = hash_word(hash, layout->fixed_prefix_size);
    hash = hash_word(hash, layout->align);
    hash = hash_word(hash, layout->field_count);
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const XrTargetFieldRecord *field = &fields[layout->field_begin + i];
        const XrTargetMachineRepRecord *field_rep =
            xr_target_plan_machine_rep(target_plan, field->memory_rep);
        uint8_t field_native_type = 0;
        const char *field_c_type = field_rep ? scalar_c_type(field_rep->kind) : NULL;
        if (!field_rep || !field_c_type ||
            !scalar_native_type(field_rep->kind, &field_native_type) ||
            field->layout != aggregate_rep->detail || field->semantic_field != i ||
            field->semantic_name != XR_SEMANTIC_INDEX_NONE ||
            field->root_kind != XR_TARGET_ROOT_NONE || field->flags != 0 || field->reserved != 0 ||
            (i != 0 && (field_rep->kind != machine_kind || field_native_type != native_type ||
                        strcmp(field_c_type, element_c_type) != 0)))
            return false;
        if (i == 0) {
            machine_kind = field_rep->kind;
            native_type = field_native_type;
            element_c_type = field_c_type;
        }
        hash = hash_word(hash, field->offset);
        hash = hash_word(hash, field->size);
        hash = hash_word(hash, field_rep->kind);
    }
    if (!hash)
        hash = UINT64_C(1);
    out->layout = aggregate_rep->detail;
    out->backing_value = binding->semantic_value;
    out->element_count = layout->field_count;
    out->abi_key = hash;
    out->kind = XR_C_AGGREGATE_PROJECTION_FIXED_ARRAY_BACKING;
    out->element_native_type = native_type;
    memcpy(out->c_type, "XrValue", sizeof("XrValue"));
    size_t c_type_length = strlen(element_c_type);
    if (c_type_length >= sizeof(out->element_c_type))
        return false;
    memcpy(out->element_c_type, element_c_type, c_type_length + 1u);
    return true;
}

/* A tuple keeps its lanes in a runtime allocation the construction owns, so its
 * C identity is the tagged handle to that allocation and not a named struct:
 * the aggregate layout states how many lanes there are and what each lane's own
 * representation is, which is what the emitted construction needs, while the
 * value itself travels as one XrValue. Lanes may each carry their own type,
 * which is the one thing separating this from the fixed-array backing that also
 * projects to a handle. */
static bool tuple_projection(const XrTargetPlan *target_plan, const XrTargetValueRepRecord *binding,
                             const XrTargetMachineRepRecord *aggregate_rep,
                             const XrTargetLayoutRecord *layout, const XrTargetFieldRecord *fields,
                             uint32_t field_count, const XrSemanticTypeRecord *type,
                             const XrTargetMachineFacts *machine, XrCAggregateProjection *out) {
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        slots && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    if (!slot || type->kind != XR_KIND_TUPLE || type->child_count == 0 ||
        type->aggregate_extent != type->child_count ||
        type->aggregate_extent != layout->field_count || type->aggregate_align != 0 ||
        (type->flags & XR_SEM_TYPE_NULLABLE) != 0 || type->scalar_rep != XR_SCALAR_REP_NONE ||
        layout->field_begin > field_count ||
        layout->field_count > field_count - layout->field_begin ||
        slot->semantic_value != binding->semantic_value ||
        slot->register_rep != binding->register_rep || slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_NONE || slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
        return false;

    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_word(hash, machine->data_layout.stable_hash);
    hash = hash_word(hash, 2); /* tuple backing-handle projection */
    hash = hash_word(hash, layout->fixed_prefix_size);
    hash = hash_word(hash, layout->align);
    hash = hash_word(hash, layout->field_count);
    for (uint16_t i = 0; i < layout->field_count; i++) {
        const XrTargetFieldRecord *field = &fields[layout->field_begin + i];
        const XrTargetMachineRepRecord *field_rep =
            xr_target_plan_machine_rep(target_plan, field->memory_rep);
        uint8_t field_native_type = 0;
        bool scalar = field_rep && scalar_c_type(field_rep->kind) &&
                      scalar_native_type(field_rep->kind, &field_native_type);
        bool reference = field_rep && field_rep->kind == XR_MACHINE_REP_DYN_VALUE &&
                         field_rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
                         field_rep->null_encoding == XR_TARGET_NULL_TAGGED &&
                         (field_rep->ownership == XR_TARGET_OWNERSHIP_OWNED ||
                          field_rep->ownership == XR_TARGET_OWNERSHIP_BORROWED);
        if ((!scalar && !reference) || field->layout != aggregate_rep->detail ||
            field->semantic_field != i || field->semantic_name != XR_SEMANTIC_INDEX_NONE ||
            field->root_kind != (reference ? XR_TARGET_ROOT_DYNAMIC : XR_TARGET_ROOT_NONE) ||
            field->flags != 0 || field->reserved != 0)
            return false;
        hash = hash_word(hash, field->offset);
        hash = hash_word(hash, field->size);
        hash = hash_word(hash, field_rep->kind);
    }
    if (!hash)
        hash = UINT64_C(1);
    out->layout = aggregate_rep->detail;
    out->backing_value = binding->semantic_value;
    out->element_count = layout->field_count;
    out->abi_key = hash;
    out->kind = XR_C_AGGREGATE_PROJECTION_TUPLE_BACKING;
    memcpy(out->c_type, "XrValue", sizeof("XrValue"));
    return true;
}

bool xr_c_aggregate_projection(const XrTargetPlan *target_plan,
                               const XrTargetValueRepRecord *binding, XrCAggregateProjection *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(target_plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(target_plan, binding->memory_rep) : NULL;
    uint32_t layout_count = 0;
    uint32_t field_count = 0;
    uint32_t machine_rep_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(target_plan, &layout_count);
    const XrTargetFieldRecord *fields = xr_target_plan_fields(target_plan, &field_count);
    const XrTargetMachineRepRecord *machine_reps =
        xr_target_plan_machine_reps(target_plan, &machine_rep_count);
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        binding && slots && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    const XrSemanticPlan *semantic =
        slot ? xr_target_plan_module_for_function(target_plan, slot->function, NULL) : NULL;
    const XrTargetProfile *profile = xr_target_plan_profile(target_plan);
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(profile);
    if (!target_plan || !binding || !out || !xr_target_plan_is_verified(target_plan) ||
        !xr_target_plan_fingerprint_is_intact(target_plan) || !register_rep || !memory_rep ||
        !layouts || !fields || !machine_reps || !semantic || !machine ||
        slot->semantic_value != binding->semantic_value ||
        slot->register_rep != binding->register_rep || slot->memory_rep != binding->memory_rep ||
        register_rep->kind != XR_MACHINE_REP_AGGREGATE ||
        memory_rep->kind != XR_MACHINE_REP_AGGREGATE ||
        register_rep->detail != memory_rep->detail || register_rep->detail >= layout_count)
        return false;
    const XrTargetLayoutRecord *layout = &layouts[register_rep->detail];
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, layout->semantic_type);
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!type || layout->kind != XR_TARGET_LAYOUT_AGGREGATE || layout->field_count == 0 ||
        layout->field_begin > field_count ||
        layout->field_count > field_count - layout->field_begin)
        return false;
    if (leaf_product_program_family(xr_semantic_plan_program_provenance(semantic)))
        return false;
    if (leaf_program_binding(semantic, layout->semantic_type, NULL))
        return xr_c_leaf_aggregate_projection(target_plan, semantic, layout->semantic_type, out);
    if (type->kind == XR_KIND_FIXED_ARRAY)
        return fixed_array_projection(target_plan, binding, register_rep, layout, fields,
                                      field_count, type, machine, out);
    if (type->kind == XR_KIND_TUPLE)
        return tuple_projection(target_plan, binding, register_rep, layout, fields, field_count,
                                type, machine, out);
    uint32_t type_stack[64] = {0};
    uint64_t hash = 0;
    if (!metadata ||
        !named_struct_projection_hash_depth(
            semantic, machine, machine_reps, machine_rep_count, layouts, layout_count, fields,
            field_count, metadata, metadata_count, register_rep->detail, type_stack, 0u, &hash))
        return false;
    int written = snprintf(out->c_type, sizeof(out->c_type), "xrt_struct_abi_%016" PRIx64, hash);
    if (written <= 0 || (size_t) written >= sizeof(out->c_type)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->layout = register_rep->detail;
    out->abi_key = hash;
    out->kind = XR_C_AGGREGATE_PROJECTION_NAMED_STRUCT;
    return true;
}

/* The source judgement and each physical row are replayed here. A verified
 * raw pointer alone is insufficient to identify either its pointee or its ABI. */
bool xr_c_plain_ref_aggregate_argument_projection(const XrTargetPlan *plan,
    const XrTargetCallArgumentRecord *argument, XrCAggregateProjection *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    uint32_t call_count = 0, slot_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &call_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    const XrTargetCallRecord *call = argument && calls && argument->call < call_count
        ? &calls[argument->call] : NULL;
    const XrTargetSlotRecord *caller_slot = argument && slots && argument->caller_slot < slot_count
        ? &slots[argument->caller_slot] : NULL;
    const XrTargetSlotRecord *callee_slot = argument && slots && argument->callee_slot < slot_count
        ? &slots[argument->callee_slot] : NULL;
    const XrSemanticPlan *semantic = NULL;
    uint32_t callee_function = XR_SEMANTIC_INDEX_NONE, caller_function = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticPlan *caller_semantic = NULL;
    if (!plan || !argument || !out || !call || !caller_slot || !callee_slot ||
        !xr_target_plan_is_verified(plan) || !xr_target_plan_fingerprint_is_intact(plan) ||
        !xr_target_plan_function_semantic_binding(plan, call->callee_function, &semantic, &callee_function) ||
        !xr_target_plan_function_semantic_binding(plan, call->caller_function, &caller_semantic, &caller_function) ||
        semantic != caller_semantic || call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL || call->flags ||
        caller_slot->function != call->caller_function || callee_slot->function != call->callee_function ||
        callee_slot->role != XR_TARGET_SLOT_PARAMETER || caller_slot->root_kind != XR_TARGET_ROOT_NONE ||
        callee_slot->root_kind != XR_TARGET_ROOT_NONE ||
        caller_slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        callee_slot->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        argument->mode != XR_TARGET_CALL_REFERENCE || argument->ownership != XR_TARGET_CALL_BORROW ||
        argument->transfer_mode != XR_TRANSFER_SHARE || argument->flags != XR_TARGET_CALL_ARGUMENT_ADDRESSABLE ||
        argument->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        argument->reserved8[0] || argument->reserved8[1] || argument->reserved8[2] ||
        caller_slot->register_rep != argument->register_rep || caller_slot->memory_rep != argument->memory_rep ||
        callee_slot->register_rep != argument->callee_register_rep || callee_slot->memory_rep != argument->callee_memory_rep)
        return false;
    const XrSemanticCallTargetRecord *source_target = xr_semantic_plan_call_target(semantic, call->semantic_call_target);
    const XrSemanticOperationRecord *source_call = xr_semantic_plan_operation(semantic, call->semantic_operation);
    const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(semantic, argument->callee_parameter);
    const XrSemanticOperandRecord *source = NULL;
    if (!source_target || !source_call || !parameter || source_target->operation != call->semantic_operation ||
        source_target->function != callee_function || source_call->function != caller_function ||
        source_call->result_value != call->result_value || parameter->function != callee_function ||
        parameter->ordinal != argument->ordinal || callee_slot->semantic_value != parameter->value ||
        argument->semantic_operand != source_call->operand_begin + argument->ordinal + 1u ||
        !xr_target_plain_ref_call_source_is_exact(semantic, source_target, argument->ordinal, &source) ||
        !source || caller_slot->semantic_value != source->value)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    const XrTargetMachineRepRecord *caller_rep = xr_target_plan_machine_rep(plan, argument->register_rep);
    const XrTargetMachineRepRecord *caller_mem = xr_target_plan_machine_rep(plan, argument->memory_rep);
    const XrTargetMachineRepRecord *callee_rep = xr_target_plan_machine_rep(plan, argument->callee_register_rep);
    const XrTargetMachineRepRecord *callee_mem = xr_target_plan_machine_rep(plan, argument->callee_memory_rep);
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(xr_target_plan_profile(plan));
    if (!operands || argument->semantic_operand >= operand_count ||
        operands[argument->semantic_operand].value != argument->semantic_value ||
        !caller_rep || !caller_mem || !callee_rep || !callee_mem || !machine ||
        caller_rep->kind != XR_MACHINE_REP_AGGREGATE || caller_mem->kind != XR_MACHINE_REP_AGGREGATE ||
        caller_rep->detail != caller_mem->detail || caller_rep->root_kind != XR_TARGET_ROOT_NONE ||
        caller_mem->root_kind != XR_TARGET_ROOT_NONE ||
        caller_rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL || caller_mem->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        callee_rep->kind != XR_MACHINE_REP_RAW_PTR || callee_mem->kind != XR_MACHINE_REP_RAW_PTR ||
        callee_rep->ownership != XR_TARGET_OWNERSHIP_BORROWED || callee_mem->ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        callee_rep->root_kind != XR_TARGET_ROOT_NONE || callee_mem->root_kind != XR_TARGET_ROOT_NONE ||
        callee_rep->detail || callee_mem->detail || callee_rep->lane_count || callee_mem->lane_count ||
        callee_rep->signedness != XR_TARGET_SIGN_NONE || callee_mem->signedness != XR_TARGET_SIGN_NONE ||
        callee_rep->null_encoding != XR_TARGET_NULL_ZERO || callee_mem->null_encoding != XR_TARGET_NULL_ZERO ||
        callee_slot->size != machine->data_layout.pointer.size ||
        callee_slot->align != machine->data_layout.pointer.align ||
        callee_rep->register_bits != machine->data_layout.pointer.size * 8u ||
        callee_mem->register_bits != machine->data_layout.pointer.size * 8u ||
        callee_rep->memory_size != machine->data_layout.pointer.size ||
        callee_mem->memory_size != machine->data_layout.pointer.size ||
        callee_rep->memory_align != machine->data_layout.pointer.align ||
        callee_mem->memory_align != machine->data_layout.pointer.align)
        return false;
    uint32_t partition = 0;
    const XrTargetValueRepRecord *held = xr_target_plan_partition_for_semantic(plan, semantic, &partition)
        ? xr_target_plan_value_rep_for_module(plan, partition, source->value) : NULL;
    if (!held || held->slot != argument->caller_slot ||
        !xr_c_aggregate_projection(plan, held, out) ||
        out->kind != XR_C_AGGREGATE_PROJECTION_NAMED_STRUCT ||
        !plain_ref_layout_is_exact(plan, semantic, machine, out->layout) ||
        caller_slot->size != caller_rep->memory_size || caller_slot->align != caller_rep->memory_align)
        return false;
    size_t length = strlen(out->c_type);
    if (length + 3u > sizeof(out->c_type))
        return false;
    memcpy(out->c_type + length, " *", 3u);
    return true;
}

bool xr_c_plain_ref_aggregate_projection(const XrTargetPlan *plan,
    const XrTargetValueRepRecord *binding, XrCAggregateProjection *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    uint32_t argument_count = 0, slot_count = 0, call_count = 0;
    const XrTargetCallArgumentRecord *arguments = xr_target_plan_call_arguments(plan, &argument_count);
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &call_count);
    const XrTargetSlotRecord *slot = binding && slots && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    const XrTargetMachineRepRecord *reg = binding ? xr_target_plan_machine_rep(plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *mem = binding ? xr_target_plan_machine_rep(plan, binding->memory_rep) : NULL;
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(xr_target_plan_profile(plan));
    if (!plan || !binding || !out || !slot || !reg || !mem ||
        slot->semantic_value != binding->semantic_value || slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep || reg->kind != XR_MACHINE_REP_RAW_PTR || mem->kind != XR_MACHINE_REP_RAW_PTR ||
        !machine || reg->detail || mem->detail || reg->lane_count || mem->lane_count ||
        reg->root_kind != XR_TARGET_ROOT_NONE || mem->root_kind != XR_TARGET_ROOT_NONE ||
        slot->root_kind != XR_TARGET_ROOT_NONE || slot->reserved ||
        reg->null_encoding != XR_TARGET_NULL_ZERO || mem->null_encoding != XR_TARGET_NULL_ZERO ||
        reg->signedness != XR_TARGET_SIGN_NONE || mem->signedness != XR_TARGET_SIGN_NONE ||
        reg->register_bits != machine->data_layout.pointer.size * 8u ||
        mem->register_bits != machine->data_layout.pointer.size * 8u ||
        reg->memory_size != machine->data_layout.pointer.size || mem->memory_size != machine->data_layout.pointer.size ||
        reg->memory_align != machine->data_layout.pointer.align || mem->memory_align != machine->data_layout.pointer.align ||
        slot->size != machine->data_layout.pointer.size || slot->align != machine->data_layout.pointer.align ||
        slot->ownership != reg->ownership || slot->ownership != mem->ownership ||
        (slot->role == XR_TARGET_SLOT_PARAMETER
            ? slot->ownership != XR_TARGET_OWNERSHIP_BORROWED
            : (slot->role != XR_TARGET_SLOT_TEMPORARY || slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)))
        return false;
    bool matched = false;
    for (uint32_t i = 0; arguments && i < argument_count; i++) {
        const XrTargetCallArgumentRecord *argument = &arguments[i];
        if (!calls || argument->call >= call_count)
            return false;
        bool parameter = slot->role == XR_TARGET_SLOT_PARAMETER;
        if (parameter ? argument->callee_slot != binding->slot
            : (argument->semantic_value != binding->semantic_value ||
               calls[argument->call].caller_function != slot->function))
            continue;
        XrCAggregateProjection candidate = {0};
        if (!xr_c_plain_ref_aggregate_argument_projection(plan, argument, &candidate) ||
            (matched && (out->abi_key != candidate.abi_key || out->layout != candidate.layout ||
                         strcmp(out->c_type, candidate.c_type) != 0)))
            return false;
        *out = candidate;
        matched = true;
    }
    return matched;
}
