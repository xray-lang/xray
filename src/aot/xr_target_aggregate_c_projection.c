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
#include "../base/xmalloc.h"
#include "../ir/xi_module.h"
#include "../ir/xi_program_semantic_plan.h"
#include "../plan/semantic/xr_program_semantic_closure.h"
#include "../plan/semantic/xr_semantic_value_aggregate_shape.h"
#include "../plan/target/xr_target_instruction_verify.h"
#include "../shared/xr_native_type_core.h"
#include <inttypes.h>
#include <stdarg.h>
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

static const char *scalar_c_type(uint16_t machine_kind) {
    switch ((XrMachineRepKind) machine_kind) {
        case XR_MACHINE_REP_I1: return "uint8_t";
        case XR_MACHINE_REP_I8: return "int8_t";
        case XR_MACHINE_REP_U8: return "uint8_t";
        case XR_MACHINE_REP_I16: return "int16_t";
        case XR_MACHINE_REP_U16: return "uint16_t";
        case XR_MACHINE_REP_I32: return "int32_t";
        case XR_MACHINE_REP_U32: return "uint32_t";
        case XR_MACHINE_REP_I64: return "int64_t";
        case XR_MACHINE_REP_U64: return "uint64_t";
        case XR_MACHINE_REP_ISIZE: return "ptrdiff_t";
        case XR_MACHINE_REP_USIZE: return "size_t";
        case XR_MACHINE_REP_F32: return "float";
        case XR_MACHINE_REP_F64: return "double";
        default: return NULL;
    }
}

static bool leaf_program_binding(const XrSemanticPlan *semantic, uint32_t semantic_type,
                                 const XrSemanticProgramTypeBinding **out) {
    const XrSemanticProgramProvenance *provenance =
        xr_semantic_plan_program_provenance(semantic);
    const XrSemanticProgramTypeBinding *binding =
        xr_semantic_plan_program_type_for_semantic_type(semantic, semantic_type);
    if (!provenance ||
        provenance->program_family != XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL ||
        !binding || binding->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE ||
        binding->field_count != 2 || binding->field_begin >
                                         xr_semantic_plan_program_type_field_binding_count(semantic) ||
        binding->field_count > xr_semantic_plan_program_type_field_binding_count(semantic) -
                                   binding->field_begin)
        return false;
    if (out)
        *out = binding;
    return true;
}

static bool leaf_product_program_family(const XrSemanticPlan *semantic) {
    const XrSemanticProgramProvenance *provenance =
        xr_semantic_plan_program_provenance(semantic);
    return provenance &&
           provenance->program_family ==
               XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL;
}

bool xr_c_leaf_aggregate_projection(const XrTargetPlan *target_plan, uint32_t semantic_type,
                                    XrCAggregateProjection *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticProgramTypeBinding *binding = NULL;
    const XrTargetProfile *profile = xr_target_plan_profile(target_plan);
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(profile);
    uint32_t layout_count = 0, field_count = 0;
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(target_plan, &layout_count);
    const XrTargetFieldRecord *fields = xr_target_plan_fields(target_plan, &field_count);
    if (!target_plan || !out || !xr_target_plan_is_verified(target_plan) || !semantic || !machine ||
        !layouts || !fields || !leaf_program_binding(semantic, semantic_type, &binding))
        return false;

    const XrTargetLayoutRecord *layout = NULL;
    uint32_t layout_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < layout_count; i++) {
        if (layouts[i].semantic_type != semantic_type)
            continue;
        if (layout)
            return false;
        layout = &layouts[i];
        layout_index = i;
    }
    if (!layout || layout->kind != XR_TARGET_LAYOUT_AGGREGATE ||
        layout->fixed_prefix_size != 16 || layout->align != 8 || layout->root_field_count != 0 ||
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
            program_field->declaration_ordinal != ordinal ||
            field->layout != layout_index || field->semantic_field != ordinal ||
            field->semantic_name != XR_SEMANTIC_INDEX_NONE ||
            !scalar_native_type(field_rep->kind, &native_type) ||
            field_rep->kind != XR_MACHINE_REP_I64 ||
            field_rep->memory_size != 8 || field_rep->memory_align != 8 ||
            field_rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL || field->offset != ordinal * 8u ||
            field->size != 8 || field->align != 8 ||
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
    int written = snprintf(out->c_type, sizeof(out->c_type),
                           "xrt_struct_abi_%016" PRIx64, hash);
    if (written <= 0 || (size_t) written >= sizeof(out->c_type)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->layout = layout_index;
    out->abi_key = hash;
    out->kind = XR_C_AGGREGATE_PROJECTION_NAMED_STRUCT;
    return true;
}

static bool fixed_array_projection(const XrTargetPlan *target_plan,
                                   const XrTargetValueRepRecord *binding,
                                   const XrTargetMachineRepRecord *aggregate_rep,
                                   const XrTargetLayoutRecord *layout,
                                   const XrTargetFieldRecord *fields,
                                   uint32_t field_count,
                                   const XrSemanticTypeRecord *type,
                                   const XrTargetMachineFacts *machine,
                                   XrCAggregateProjection *out) {
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots =
        xr_target_plan_slots(target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        slots && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    if (!slot || type->kind != XR_KIND_FIXED_ARRAY ||
        type->child_count != 1 || type->aggregate_extent == 0 ||
        type->aggregate_extent > UINT16_MAX ||
        type->aggregate_extent != layout->field_count ||
        (type->flags & XR_SEM_TYPE_NULLABLE) != 0 ||
        type->scalar_rep != XR_SCALAR_REP_NONE ||
        layout->field_begin > field_count ||
        layout->field_count > field_count - layout->field_begin ||
        slot->semantic_value != binding->semantic_value ||
        slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_NONE ||
        slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
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
        const char *field_c_type =
            field_rep ? scalar_c_type(field_rep->kind) : NULL;
        if (!field_rep || !field_c_type ||
            !scalar_native_type(field_rep->kind, &field_native_type) ||
            field->layout != aggregate_rep->detail ||
            field->semantic_field != i ||
            field->semantic_name != XR_SEMANTIC_INDEX_NONE ||
            field->root_kind != XR_TARGET_ROOT_NONE || field->flags != 0 ||
            field->reserved != 0 ||
            (i != 0 && (field_rep->kind != machine_kind ||
                        field_native_type != native_type ||
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
static bool tuple_projection(const XrTargetPlan *target_plan,
                             const XrTargetValueRepRecord *binding,
                             const XrTargetMachineRepRecord *aggregate_rep,
                             const XrTargetLayoutRecord *layout,
                             const XrTargetFieldRecord *fields,
                             uint32_t field_count,
                             const XrSemanticTypeRecord *type,
                             const XrTargetMachineFacts *machine,
                             XrCAggregateProjection *out) {
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots =
        xr_target_plan_slots(target_plan, &slot_count);
    const XrTargetSlotRecord *slot =
        slots && binding->slot < slot_count ? &slots[binding->slot] : NULL;
    if (!slot || type->kind != XR_KIND_TUPLE || type->child_count == 0 ||
        type->aggregate_extent != type->child_count ||
        type->aggregate_extent != layout->field_count ||
        type->aggregate_align != 0 ||
        (type->flags & XR_SEM_TYPE_NULLABLE) != 0 ||
        type->scalar_rep != XR_SCALAR_REP_NONE ||
        layout->field_begin > field_count ||
        layout->field_count > field_count - layout->field_begin ||
        slot->semantic_value != binding->semantic_value ||
        slot->register_rep != binding->register_rep ||
        slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_NONE ||
        slot->ownership != XR_TARGET_OWNERSHIP_TRIVIAL)
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
        if (!field_rep || !scalar_c_type(field_rep->kind) ||
            !scalar_native_type(field_rep->kind, &field_native_type) ||
            field->layout != aggregate_rep->detail ||
            field->semantic_field != i ||
            field->semantic_name != XR_SEMANTIC_INDEX_NONE ||
            field->root_kind != XR_TARGET_ROOT_NONE || field->flags != 0 ||
            field->reserved != 0)
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
    if (!target_plan || !binding || !out || !xr_target_plan_is_verified(target_plan) ||
        !xr_target_plan_fingerprint_is_intact(target_plan) || !register_rep || !memory_rep ||
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
        layout->field_count > field_count - layout->field_begin)
        return false;
    if (leaf_product_program_family(semantic))
        return false;
    if (leaf_program_binding(semantic, layout->semantic_type, NULL))
        return xr_c_leaf_aggregate_projection(target_plan, layout->semantic_type, out);
    if (type->kind == XR_KIND_FIXED_ARRAY)
        return fixed_array_projection(target_plan, binding, register_rep, layout,
                                      fields, field_count, type, machine, out);
    if (type->kind == XR_KIND_TUPLE)
        return tuple_projection(target_plan, binding, register_rep, layout, fields,
                                field_count, type, machine, out);
    if (!xr_semantic_value_aggregate_shape_for_type(
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
    out->kind = XR_C_AGGREGATE_PROJECTION_NAMED_STRUCT;
    return true;
}

static bool leaf_product_c_fail(char *error, size_t error_size, const char *message);

typedef struct XrCLeafValueProductFunctionBinding {
    uint32_t target_function;
    bool entry;
    char c_symbol[48];
} XrCLeafValueProductFunctionBinding;

typedef struct XrCLeafValueProductProgramBinding {
    const XrTargetPlan *target_plan;
    XrFingerprint target_fingerprint;
    XrCLeafValueProductFunctionBinding functions[3];
    bool verified;
} XrCLeafValueProductProgramBinding;

static bool leaf_product_symbol(XrStableId identity, char *out, size_t out_size) {
    static const char hex[] = "0123456789abcdef";
    if (!out || out_size < 39u)
        return false;
    memcpy(out, "xr_lp_", 6u);
    for (size_t i = 0; i < sizeof(identity.bytes); i++) {
        out[6u + i * 2u] = hex[identity.bytes[i] >> 4u];
        out[7u + i * 2u] = hex[identity.bytes[i] & UINT8_C(0x0f)];
    }
    out[38] = '\0';
    return true;
}

static const XrSemanticProgramFunctionBinding *
leaf_product_semantic_function_for_row(const XrSemanticPlan *semantic,
                                       uint32_t program_row) {
    const XrSemanticProgramFunctionBinding *match = NULL;
    size_t count = xr_semantic_plan_program_function_binding_count(semantic);
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticProgramFunctionBinding *candidate =
            xr_semantic_plan_program_function_binding(semantic, i);
        if (!candidate || candidate->program_row != program_row)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

static bool leaf_product_program_bind(
    const XrTargetPlan *target_plan, const XiModule *module,
    XrCLeafValueProductProgramBinding *out, char *error, size_t error_size) {
    if (out)
        memset(out, 0, sizeof(*out));
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrTargetProfile *profile = xr_target_plan_profile(target_plan);
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(profile);
    const XrProgramSemanticClosure *closure =
        module ? module->program_semantic_closure : NULL;
    uint32_t function_count = 0;
    const XrTargetFunctionRecord *target_functions =
        xr_target_plan_functions(target_plan, &function_count);
    if (!target_plan || !module || !module->init || !out || !semantic || !machine || !closure ||
        !target_functions || function_count != 4u || module->nfuncs != 3u ||
        machine->runtime_profile != XR_TARGET_RUNTIME_PROFILE_HOSTED ||
        xr_program_semantic_closure_family(closure) !=
            XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL ||
        !xi_program_semantic_plan_verify_detached_leaf_authority(
            module->init, semantic, error, error_size) ||
        !xr_target_plan_is_verified(target_plan) ||
        !xr_target_plan_fingerprint_is_intact(target_plan))
        return leaf_product_c_fail(error, error_size,
                                   "leaf product C binding authority is incomplete");
    bool seen[3] = {false, false, false};
    uint32_t entry_count = 0;
    for (uint16_t i = 0; i < module->nfuncs; i++) {
        const XiFunc *function = module->functions ? module->functions[i] : NULL;
        const XrProgramSemanticFunctionRecord *program_function =
            function ? xr_program_semantic_closure_function(
                           closure, function->psc_function_index)
                     : NULL;
        const XrSemanticProgramFunctionBinding *semantic_function =
            function ? leaf_product_semantic_function_for_row(
                           semantic, function->psc_function_index)
                     : NULL;
        uint32_t target_function = UINT32_MAX;
        if (!function || !program_function || !semantic_function ||
            !xr_target_plan_find_function(target_plan, semantic,
                                          semantic_function->semantic_function,
                                          &target_function) ||
            function->psc_function_index >= 3u || target_function >= function_count ||
            seen[function->psc_function_index] ||
            xr_target_plan_function_execution_family_mask(target_plan, target_function) !=
                XR_TARGET_EXECUTION_LEAF_VALUE_PRODUCT_TUPLE6 ||
            target_functions[target_function].semantic_function !=
                semantic_function->semantic_function)
            return leaf_product_c_fail(error, error_size,
                                       "leaf product C function join is inexact");
        XrCLeafValueProductFunctionBinding *binding =
            &out->functions[function->psc_function_index];
        binding->target_function = target_function;
        binding->entry =
            program_function->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY;
        if ((!binding->entry && program_function->flags != 0u) ||
            !leaf_product_symbol(program_function->id, binding->c_symbol,
                                 sizeof(binding->c_symbol)))
            return leaf_product_c_fail(error, error_size,
                                       "leaf product C symbol identity is inexact");
        entry_count += binding->entry ? 1u : 0u;
        seen[function->psc_function_index] = true;
    }
    if (!seen[0] || !seen[1] || !seen[2] || entry_count != 2u)
        return leaf_product_c_fail(error, error_size,
                                   "leaf product C entry inventory is inexact");
    out->target_plan = target_plan;
    out->target_fingerprint = xr_target_plan_fingerprint(target_plan);
    out->verified = true;
    return true;
}

typedef struct XrLeafProductCBuffer {
    char *bytes;
    size_t size;
    size_t capacity;
} XrLeafProductCBuffer;

static bool leaf_product_c_append(XrLeafProductCBuffer *buffer, const char *format, ...) {
    if (!buffer || !buffer->bytes || buffer->size >= buffer->capacity)
        return false;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(buffer->bytes + buffer->size, buffer->capacity - buffer->size,
                            format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t) written >= buffer->capacity - buffer->size)
        return false;
    buffer->size += (size_t) written;
    return true;
}

static bool leaf_product_c_fail(char *error, size_t error_size, const char *message) {
    if (error && error_size)
        snprintf(error, error_size, "%s", message);
    return false;
}

static bool leaf_product_rep_is(const XrTargetPlan *plan, uint16_t rep_index,
                                uint16_t kind) {
    const XrTargetMachineRepRecord *rep = xr_target_plan_machine_rep(plan, rep_index);
    if (!rep || rep->kind != kind || rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        rep->root_kind != XR_TARGET_ROOT_NONE)
        return false;
    if (kind == XR_MACHINE_REP_AGGREGATE)
        return rep->register_bits == 384u && rep->memory_size == 48u &&
               rep->memory_align == 8u;
    if (kind == XR_MACHINE_REP_U8)
        return rep->register_bits == 8u && rep->memory_size == 1u &&
               rep->memory_align == 1u;
    return kind == XR_MACHINE_REP_I64 && rep->register_bits == 64u &&
           rep->memory_size == 8u && rep->memory_align == 8u;
}

static bool leaf_product_field_ordinal(const XrTargetPlan *plan, uint64_t field_index,
                                       uint32_t *ordinal) {
    uint32_t field_count = 0;
    uint32_t layout_count = 0;
    const XrTargetFieldRecord *fields = xr_target_plan_fields(plan, &field_count);
    const XrTargetLayoutRecord *layouts = xr_target_plan_layouts(plan, &layout_count);
    if (!fields || !layouts || !ordinal || field_index >= field_count)
        return false;
    const XrTargetFieldRecord *field = &fields[(uint32_t) field_index];
    if (field->layout >= layout_count)
        return false;
    const XrTargetLayoutRecord *layout = &layouts[field->layout];
    if (layout->kind != XR_TARGET_LAYOUT_AGGREGATE || layout->fixed_prefix_size != 48u ||
        layout->align != 8u || layout->field_count != 6u ||
        field_index < layout->field_begin || field_index >= layout->field_begin + 6u)
        return false;
    uint32_t value = (uint32_t) field_index - layout->field_begin;
    if (field->semantic_field != value || field->offset != value * 8u ||
        field->size != (value == 2u ? 1u : 8u) ||
        field->align != (value == 2u ? 1u : 8u) ||
        !leaf_product_rep_is(plan, field->memory_rep,
                             value == 2u ? XR_MACHINE_REP_U8 : XR_MACHINE_REP_I64))
        return false;
    *ordinal = value;
    return true;
}

static bool leaf_product_emit_slot_declarations(XrLeafProductCBuffer *buffer,
                                                const XrTargetPlan *plan,
                                                uint32_t function) {
    uint32_t slot_count = 0;
    uint32_t row_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(plan, &slot_count);
    const XrTargetInstructionRecord *rows =
        xr_target_plan_function_instructions(plan, function, &row_count);
    if (!slots || !rows)
        return false;
    for (uint32_t i = 0; i < slot_count; i++) {
        const XrTargetSlotRecord *slot = &slots[i];
        if (slot->function != function)
            continue;
        bool used = false;
        for (uint32_t row = 0; row < row_count; row++) {
            used |= rows[row].result_slot == slot->id;
            for (uint8_t operand = 0; operand < rows[row].operand_count; operand++)
                used |= rows[row].operand_slots[operand] == slot->id;
        }
        if (!used)
            continue;
        if (slot->register_rep != slot->memory_rep)
            return false;
        if (leaf_product_rep_is(plan, slot->memory_rep, XR_MACHINE_REP_AGGREGATE)) {
            if (!leaf_product_c_append(buffer, "    xr_leaf_product_tuple6 s%u = {0};\n",
                                       slot->id))
                return false;
        } else if (leaf_product_rep_is(plan, slot->memory_rep, XR_MACHINE_REP_U8)) {
            if (!leaf_product_c_append(buffer, "    uint8_t s%u = UINT8_C(0);\n", slot->id))
                return false;
        } else if (leaf_product_rep_is(plan, slot->memory_rep, XR_MACHINE_REP_I64)) {
            if (!leaf_product_c_append(buffer, "    int64_t s%u = INT64_C(0);\n", slot->id))
                return false;
        } else {
            return false;
        }
    }
    return true;
}

static bool leaf_product_emit_function(XrLeafProductCBuffer *buffer,
                                       const XrCLeafValueProductProgramBinding *binding,
                                       uint32_t binding_index) {
    const XrTargetPlan *plan = binding->target_plan;
    const XrCLeafValueProductFunctionBinding *function_binding =
        &binding->functions[binding_index];
    uint32_t function = function_binding->target_function;
    uint32_t row_count = 0;
    uint32_t call_count = 0;
    const XrTargetInstructionRecord *rows =
        xr_target_plan_function_instructions(plan, function, &row_count);
    const XrTargetCallRecord *calls = xr_target_plan_calls(plan, &call_count);
    if (!rows || !calls || !function_binding->c_symbol[0] ||
        !leaf_product_c_append(buffer, "%sxr_leaf_product_tuple6 %s(void) {\n",
                               function_binding->entry ? "" : "static ",
                               function_binding->c_symbol) ||
        !leaf_product_emit_slot_declarations(buffer, plan, function))
        return false;
    for (uint32_t i = 0; i < row_count; i++) {
        const XrTargetInstructionRecord *row = &rows[i];
        uint32_t ordinal = 0;
        switch ((XrTargetInstructionOpcode) row->opcode) {
            case XR_TARGET_INSTRUCTION_CONST_I64:
                if (!leaf_product_c_append(buffer,
                                           "    s%u = (int64_t) UINT64_C(0x%016" PRIx64 ");\n",
                                           row->result_slot, row->immediate_bits))
                    return false;
                break;
            case XR_TARGET_INSTRUCTION_CONST_U8:
                if (!leaf_product_c_append(buffer, "    s%u = UINT8_C(%" PRIu64 ");\n",
                                           row->result_slot, row->immediate_bits))
                    return false;
                break;
            case XR_TARGET_INSTRUCTION_CALL_DIRECT_AGGREGATE: {
                const XrCLeafValueProductFunctionBinding *callee_binding = NULL;
                if (row->immediate_bits < call_count) {
                    uint32_t callee =
                        calls[(uint32_t) row->immediate_bits].callee_function;
                    for (uint32_t candidate = 0; candidate < 3u; candidate++)
                        if (binding->functions[candidate].target_function == callee)
                            callee_binding = &binding->functions[candidate];
                }
                if (row->immediate_bits >= call_count ||
                    calls[(uint32_t) row->immediate_bits].caller_function != function ||
                    !callee_binding || !callee_binding->c_symbol[0] ||
                    !leaf_product_c_append(buffer, "    s%u = %s();\n",
                                           row->result_slot,
                                           callee_binding->c_symbol))
                    return false;
                break;
            }
            case XR_TARGET_INSTRUCTION_AGGREGATE_GET_I64:
            case XR_TARGET_INSTRUCTION_VALUE_PRODUCT_GET_U8:
                if (!leaf_product_field_ordinal(plan, row->immediate_bits, &ordinal) ||
                    !leaf_product_c_append(buffer, "    s%u = s%u.field%u;\n", row->result_slot,
                                           row->operand_slots[0], ordinal))
                    return false;
                break;
            case XR_TARGET_INSTRUCTION_VALUE_PRODUCT_INIT:
                if (!leaf_product_c_append(buffer,
                                           "    s%u = (xr_leaf_product_tuple6){0};\n",
                                           row->result_slot))
                    return false;
                break;
            case XR_TARGET_INSTRUCTION_VALUE_PRODUCT_SET_I64:
            case XR_TARGET_INSTRUCTION_VALUE_PRODUCT_SET_U8:
                if (!leaf_product_field_ordinal(plan, row->immediate_bits, &ordinal) ||
                    !leaf_product_c_append(buffer, "    s%u.field%u = s%u;\n",
                                           row->operand_slots[0], ordinal,
                                           row->operand_slots[1]))
                    return false;
                break;
            case XR_TARGET_INSTRUCTION_RETURN_AGGREGATE:
                if (!leaf_product_c_append(buffer, "    return s%u;\n",
                                           row->operand_slots[0]))
                    return false;
                break;
            default: return false;
        }
    }
    return leaf_product_c_append(buffer, "}\n\n");
}

static bool leaf_product_program_emit_binding(
    const XrCLeafValueProductProgramBinding *binding, char **out_source,
    size_t *out_size, char *error, size_t error_size) {
    if (out_source)
        *out_source = NULL;
    if (out_size)
        *out_size = 0;
    const XrTargetPlan *target_plan = binding ? binding->target_plan : NULL;
    if (!binding || !binding->verified || !target_plan || !out_source || !out_size ||
        xr_target_plan_schema_version(target_plan) != XR_TARGET_PLAN_SCHEMA_VERSION ||
        !xr_target_plan_is_verified(target_plan) ||
        !xr_target_plan_fingerprint_is_intact(target_plan) ||
        !xr_fingerprint_equal(binding->target_fingerprint,
                              xr_target_plan_fingerprint(target_plan)) ||
        !xr_target_instruction_program_verify(target_plan, error, error_size))
        return leaf_product_c_fail(error, error_size,
                                   "leaf product C emission requires an intact verified plan");
    uint32_t function_count = 0;
    uint32_t call_count = 0;
    uint32_t argument_count = 0;
    uint32_t slot_count = 0;
    uint32_t instruction_count = 0;
    const XrTargetFunctionRecord *functions =
        xr_target_plan_functions(target_plan, &function_count);
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    (void) xr_target_plan_call_arguments(target_plan, &argument_count);
    (void) xr_target_plan_slots(target_plan, &slot_count);
    (void) xr_target_plan_instructions(target_plan, &instruction_count);
    if (!functions || !calls || function_count != 4u || call_count != 2u ||
        argument_count != 0u)
        return leaf_product_c_fail(error, error_size, "leaf product program shape is not exact");
    uint32_t callee = calls[0].callee_function;
    if (callee >= function_count || calls[1].callee_function != callee ||
        calls[0].caller_function >= function_count || calls[1].caller_function >= function_count ||
        calls[0].caller_function == calls[1].caller_function)
        return leaf_product_c_fail(error, error_size, "leaf product call graph is not exact");
    for (uint32_t function = 0; function < 3u; function++)
        if (binding->functions[function].target_function >= function_count ||
            xr_target_plan_function_execution_family_mask(
                target_plan, binding->functions[function].target_function) !=
            XR_TARGET_EXECUTION_LEAF_VALUE_PRODUCT_TUPLE6)
            return leaf_product_c_fail(error, error_size,
                                       "leaf product execution coverage is incomplete");

    if (slot_count > (SIZE_MAX - 4096u) / 80u ||
        instruction_count > (SIZE_MAX - 4096u - (size_t) slot_count * 80u) / 112u)
        return leaf_product_c_fail(error, error_size,
                                   "leaf product C source bound overflowed");
    XrLeafProductCBuffer buffer = {
        .capacity = 4096u + (size_t) slot_count * 80u +
                    (size_t) instruction_count * 112u,
    };
    buffer.bytes = xr_malloc(buffer.capacity);
    if (!buffer.bytes)
        return leaf_product_c_fail(error, error_size, "leaf product C source allocation failed");
    buffer.bytes[0] = '\0';
    bool ok = leaf_product_c_append(
        &buffer,
        "#include <stddef.h>\n#include <stdint.h>\n\n"
        "typedef struct xr_leaf_product_tuple6 {\n"
        "    int64_t field0;\n    int64_t field1;\n    uint8_t field2;\n"
        "    uint8_t reserved2[7];\n    int64_t field3;\n    int64_t field4;\n"
        "    int64_t field5;\n} xr_leaf_product_tuple6;\n"
        "_Static_assert(sizeof(xr_leaf_product_tuple6) == 48, \"tuple6 size\");\n"
        "_Static_assert(_Alignof(xr_leaf_product_tuple6) == 8, \"tuple6 align\");\n"
        "_Static_assert(offsetof(xr_leaf_product_tuple6, field0) == 0, \"field0\");\n"
        "_Static_assert(offsetof(xr_leaf_product_tuple6, field1) == 8, \"field1\");\n"
        "_Static_assert(offsetof(xr_leaf_product_tuple6, field2) == 16, \"field2\");\n"
        "_Static_assert(offsetof(xr_leaf_product_tuple6, field3) == 24, \"field3\");\n"
        "_Static_assert(offsetof(xr_leaf_product_tuple6, field4) == 32, \"field4\");\n"
        "_Static_assert(offsetof(xr_leaf_product_tuple6, field5) == 40, \"field5\");\n\n");
    for (uint32_t function = 0; ok && function < 3u; function++) {
        const XrCLeafValueProductFunctionBinding *function_binding =
            &binding->functions[function];
        ok = function_binding->target_function < function_count &&
             function_binding->c_symbol[0] &&
             leaf_product_c_append(
                 &buffer, "%sxr_leaf_product_tuple6 %s(void);\n",
                 function_binding->entry ? "" : "static ",
                 function_binding->c_symbol);
    }
    ok = ok && leaf_product_c_append(&buffer, "\n");
    for (uint32_t function = 0; ok && function < 3u; function++)
        ok = leaf_product_emit_function(&buffer, binding, function);
    if (!ok) {
        xr_free(buffer.bytes);
        return leaf_product_c_fail(error, error_size, "leaf product C source exceeded its bound");
    }
    *out_source = buffer.bytes;
    *out_size = buffer.size;
    return true;
}

bool xr_c_leaf_value_product_program_emit(
    const XrTargetPlan *target_plan, const XiModule *module, char **out_source,
    size_t *out_size, char *error, size_t error_size) {
    if (out_source)
        *out_source = NULL;
    if (out_size)
        *out_size = 0;
    XrCLeafValueProductProgramBinding binding = {0};
    return leaf_product_program_bind(target_plan, module, &binding, error, error_size) &&
           leaf_product_program_emit_binding(&binding, out_source, out_size, error,
                                             error_size);
}
