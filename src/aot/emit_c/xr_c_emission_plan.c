/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_c_emission_plan.c - Immutable TargetPlan-backed C emission plan
 */

#include "xr_c_emission_plan.h"
#include "../../plan/target/xr_target_plan.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "../../ir/xi_own.h"
#include "../../frontend/analyzer/xa_intrinsic_registry.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

#define XR_C_EMISSION_PLAN_SCHEMA_VERSION UINT32_C(11)
#define XR_C_CHANNEL_NEW_SYMBOL "xr_aot_channel_new"
#define XR_C_STRINGBUILDER_NEW_SYMBOL "xrt_strbuf_new"
#define XR_C_CHANNEL_RECV_INT_SYMBOL "XR_TO_INT"
#define XR_C_CHANNEL_RECV_FLOAT_SYMBOL "XR_TO_FLOAT"
#define XR_C_CHANNEL_RECV_BOOL_SYMBOL "XR_TO_BOOL"
#define XR_C_CHANNEL_RECV_RUNE_SYMBOL "XR_TO_RUNE"
#define XR_C_STRING_BYTE_SLICE_VIEW_SYMBOL "xrt_span_from_string_bytes"

struct XrCEmissionPlan {
    XrCValueEmissionView *values;
    uint32_t value_count;
    uint32_t schema_version;
    XrFingerprint target_fingerprint;
    XrFingerprint profile_fingerprint;
    XrFingerprint fingerprint;
    bool verified;
};

static bool emission_error(char *error, size_t error_size, const char *code,
                           const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "%s: %s", code, detail);
    return false;
}

static bool machine_kind_to_c_rep(uint16_t kind, XrCValueRep *out, const char **c_type) {
    if (!out || !c_type)
        return false;
    switch (kind) {
        case XR_MACHINE_REP_VOID:
            *out = XR_C_VALUE_REP_VOID;
            *c_type = "void";
            return true;
        case XR_MACHINE_REP_I1:
            *out = XR_C_VALUE_REP_BOOL;
            *c_type = "uint8_t";
            return true;
        case XR_MACHINE_REP_I8:
            *out = XR_C_VALUE_REP_I8;
            *c_type = "int8_t";
            return true;
        case XR_MACHINE_REP_U8:
            *out = XR_C_VALUE_REP_U8;
            *c_type = "uint8_t";
            return true;
        case XR_MACHINE_REP_I16:
            *out = XR_C_VALUE_REP_I16;
            *c_type = "int16_t";
            return true;
        case XR_MACHINE_REP_U16:
            *out = XR_C_VALUE_REP_U16;
            *c_type = "uint16_t";
            return true;
        case XR_MACHINE_REP_I32:
            *out = XR_C_VALUE_REP_I32;
            *c_type = "int32_t";
            return true;
        case XR_MACHINE_REP_U32:
            *out = XR_C_VALUE_REP_U32;
            *c_type = "uint32_t";
            return true;
        case XR_MACHINE_REP_I64:
            *out = XR_C_VALUE_REP_I64;
            *c_type = "int64_t";
            return true;
        case XR_MACHINE_REP_U64:
            *out = XR_C_VALUE_REP_U64;
            *c_type = "uint64_t";
            return true;
        case XR_MACHINE_REP_ISIZE:
            *out = XR_C_VALUE_REP_ISIZE;
            *c_type = "ptrdiff_t";
            return true;
        case XR_MACHINE_REP_USIZE:
            *out = XR_C_VALUE_REP_USIZE;
            *c_type = "size_t";
            return true;
        case XR_MACHINE_REP_F32:
            *out = XR_C_VALUE_REP_F32;
            *c_type = "float";
            return true;
        case XR_MACHINE_REP_F64:
            *out = XR_C_VALUE_REP_F64;
            *c_type = "double";
            return true;
        case XR_MACHINE_REP_RUNE:
            *out = XR_C_VALUE_REP_RUNE;
            *c_type = "uint32_t";
            return true;
        case XR_MACHINE_REP_DYN_VALUE:
            *out = XR_C_VALUE_REP_TAGGED;
            *c_type = "XrValue";
            return true;
        case XR_MACHINE_REP_VIEW:
            *out = XR_C_VALUE_REP_VIEW;
            *c_type = "xr_span_t";
            return true;
        default: return false;
    }
}

static bool emission_stable_id_is_zero(XrStableId id) {
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++) {
        if (id.bytes[i] != 0)
            return false;
    }
    return true;
}

static bool emission_identity_from_pair(const char *domain, XrStableId first,
                                        XrStableId second, uint32_t ordinal,
                                        XrStableId *out) {
    char first_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char second_hex[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    XrFingerprint digest;
    if (!domain || !out)
        return false;
    xr_stable_id_hex(first, first_hex);
    xr_stable_id_hex(second, second_hex);
    int written = snprintf(key, sizeof(key), "%s:first=%s:second=%s:ordinal=%u",
                           domain, first_hex, second_hex, ordinal);
    return written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static const XrSemanticOperationRecord *binding_operation(
    const XrTargetPlan *target_plan,
    const XrTargetValueRepRecord *binding) {
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots =
        xr_target_plan_slots(target_plan, &slot_count);
    if (!binding || binding->slot >= slot_count || !slots)
        return NULL;
    uint32_t operation = slots[binding->slot].semantic_operation;
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_plan(target_plan);
    return operation < xr_semantic_plan_operation_count(semantic)
               ? xr_semantic_plan_operation(semantic, operation)
               : NULL;
}

/* Builder-side collector. The recipe is admitted only by the frozen
 * intrinsic evidence and its exact Target call row; no Xi name survives at
 * this boundary. */
static bool build_exact_string_byte_slice_view_recipe(
    const XrTargetPlan *target_plan, const XrTargetValueRepRecord *binding,
    uint32_t *source_value) {
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation =
        binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls =
        xr_target_plan_calls(target_plan, &call_count);
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots =
        xr_target_plan_slots(target_plan, &slot_count);
    if (!semantic || !operation || !binding || !operands || !slots ||
        binding->slot >= slot_count || operation->opcode != XI_CALL_BUILTIN ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_STRING_BYTE_SLICE_VIEW ||
        operation->evidence[1] != XA_INTRINSIC_STRING_BYTE_SLICE_VIEW ||
        operation->result_value != binding->semantic_value ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->view_source_operand != 0 || operation->view_source_parameter != -1 ||
        operation->view_source_value == XR_SEMANTIC_INDEX_NONE ||
        operation->view_origin != XI_VIEW_ORIGIN_RECEIVER ||
        operation->view_capability != 1 || operation->view_lifetime != 1 ||
        operation->view_complete != 1 || operation->reserved_view[0] != 0 ||
        operation->reserved_view[1] != 0 || operation->reserved_view[2] != 0 ||
        operands[operation->operand_begin].value != operation->view_source_value ||
        operands[operation->operand_begin].parameter != 0 ||
        operands[operation->operand_begin].role != XR_SEM_OPERAND_ARGUMENT ||
        (operands[operation->operand_begin].flags &
         XR_SEM_OPERAND_CALL_CONTRACT) == 0)
        return false;
    const XrSemanticOperandRecord *source =
        &operands[operation->operand_begin];
    const XrSemanticTypeRecord *source_type =
        xr_semantic_plan_type(semantic, source->type);
    const XrSemanticTypeRecord *element_type =
        xr_semantic_plan_type(semantic, operation->view_element_type);
    const XrSemanticTypeRecord *view_type =
        xr_semantic_plan_type(semantic, operation->result_type);
    uint32_t child_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(semantic, &child_count);
    const uint8_t string_required = XR_SEM_TYPE_REFERENCE_CAPABLE |
                                    XR_SEM_TYPE_OWNERSHIP_ROOT;
    const uint8_t string_allowed = string_required | XR_SEM_TYPE_CONST |
                                   XR_SEM_TYPE_NULLABLE;
    if (!source_type || source_type->kind != XR_KIND_STRING ||
        source_type->builtin_type != XR_TID_NULL || source_type->child_count != 0 ||
        source_type->aggregate_extent != 0 || source_type->aggregate_align != 0 ||
        source_type->scalar_rep != XR_SCALAR_REP_NONE ||
        (source_type->flags & string_required) != string_required ||
        (source_type->flags & ~string_allowed) != 0 || !element_type ||
        element_type->kind != XR_KIND_INT || element_type->builtin_type != XR_TID_NULL ||
        element_type->child_count != 0 || element_type->aggregate_extent != 0 ||
        element_type->aggregate_align != 0 || element_type->scalar_rep != XR_NATIVE_U8 ||
        element_type->flags != 0 || !view_type || view_type->kind != XR_KIND_SLICE ||
        view_type->builtin_type != XR_TID_NULL || view_type->child_count != 1 ||
        view_type->child_begin >= child_count ||
        children[view_type->child_begin] != operation->view_element_type ||
        view_type->aggregate_extent != 0 || view_type->aggregate_align != 0 ||
        view_type->scalar_rep != XR_SCALAR_REP_NONE ||
        view_type->flags !=
            (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW))
        return false;
    uint32_t semantic_operation = slots[binding->slot].semantic_operation;
    XrStableId expected_identity;
    if (!emission_identity_from_pair("xray-target-string-byte-slice-view-v1",
                                     operation->id, view_type->id, 0,
                                     &expected_identity))
        return false;
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->semantic_operation != semantic_operation)
            continue;
        if (match || !xr_stable_id_equal(call->identity, expected_identity) ||
            call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
            call->caller_function != operation->function ||
            call->callee_function != XR_SEMANTIC_INDEX_NONE ||
            call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
            call->source_export != XR_SEMANTIC_INDEX_NONE ||
            !emission_stable_id_is_zero(call->source_export_identity) ||
            !emission_stable_id_is_zero(call->source_callee_identity) ||
            call->result_value != binding->semantic_value ||
            call->result_slot != binding->slot ||
            call->result_register_rep != binding->register_rep ||
            call->result_memory_rep != binding->memory_rep ||
            call->argument_count != 0 || call->adapter_count != 0 ||
            call->flags != 0 || call->result_mode != XR_TARGET_CALL_VALUE ||
            call->result_ownership != XR_TARGET_CALL_BORROW ||
            call->calling_convention !=
                XR_TARGET_CALL_CONVENTION_STRING_BYTE_SLICE_VIEW ||
            call->target_kind != XR_TARGET_CALL_TARGET_STRING_BYTE_SLICE_VIEW)
            return false;
        match = call;
    }
    if (!match)
        return false;
    if (source_value)
        *source_value = operation->view_source_value;
    return true;
}

/* Verifier-side proof is deliberately separate from the collector above. */
static bool verify_exact_string_byte_slice_view_recipe(
    const XrTargetPlan *target_plan, const XrTargetValueRepRecord *binding,
    uint32_t *source_value) {
    const XrSemanticPlan *semantic = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *op = binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operand_rows =
        xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t child_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(semantic, &child_count);
    if (!semantic || !binding || !op || !operand_rows ||
        op->opcode != XI_CALL_BUILTIN ||
        op->intrinsic_kind != XR_SEM_INTRINSIC_STRING_BYTE_SLICE_VIEW ||
        op->evidence[1] != XA_INTRINSIC_STRING_BYTE_SLICE_VIEW ||
        op->operand_count != 1 || op->operand_begin >= operand_count ||
        op->result_value != binding->semantic_value ||
        op->view_source_operand != 0 || op->view_source_parameter != -1 ||
        op->view_origin != XI_VIEW_ORIGIN_RECEIVER || op->view_capability != 1 ||
        op->view_lifetime != 1 || op->view_complete != 1 ||
        op->reserved_view[0] != 0 || op->reserved_view[1] != 0 ||
        op->reserved_view[2] != 0)
        return false;
    const XrSemanticOperandRecord *source = &operand_rows[op->operand_begin];
    const XrSemanticTypeRecord *source_type =
        xr_semantic_plan_type(semantic, source->type);
    const XrSemanticTypeRecord *element =
        xr_semantic_plan_type(semantic, op->view_element_type);
    const XrSemanticTypeRecord *view =
        xr_semantic_plan_type(semantic, op->result_type);
    const uint8_t source_required = XR_SEM_TYPE_REFERENCE_CAPABLE |
                                    XR_SEM_TYPE_OWNERSHIP_ROOT;
    const uint8_t source_allowed = source_required | XR_SEM_TYPE_CONST |
                                   XR_SEM_TYPE_NULLABLE;
    if (source->value != op->view_source_value || source->parameter != 0 ||
        source->role != XR_SEM_OPERAND_ARGUMENT ||
        (source->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0 || !source_type ||
        source_type->kind != XR_KIND_STRING || source_type->builtin_type != XR_TID_NULL ||
        source_type->child_count != 0 || source_type->aggregate_extent != 0 ||
        source_type->aggregate_align != 0 || source_type->scalar_rep != XR_SCALAR_REP_NONE ||
        (source_type->flags & source_required) != source_required ||
        (source_type->flags & ~source_allowed) != 0 || !element ||
        element->kind != XR_KIND_INT || element->builtin_type != XR_TID_NULL ||
        element->child_count != 0 || element->aggregate_extent != 0 ||
        element->aggregate_align != 0 || element->scalar_rep != XR_NATIVE_U8 ||
        element->flags != 0 || !view || view->kind != XR_KIND_SLICE ||
        view->builtin_type != XR_TID_NULL || view->child_count != 1 ||
        view->child_begin >= child_count ||
        children[view->child_begin] != op->view_element_type ||
        view->aggregate_extent != 0 || view->aggregate_align != 0 ||
        view->scalar_rep != XR_SCALAR_REP_NONE ||
        view->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW))
        return false;
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots = xr_target_plan_slots(target_plan, &slot_count);
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls = xr_target_plan_calls(target_plan, &call_count);
    if (!slots || binding->slot >= slot_count)
        return false;
    XrStableId expected;
    if (!emission_identity_from_pair("xray-target-string-byte-slice-view-v1",
                                     op->id, view->id, 0, &expected))
        return false;
    uint32_t semantic_operation = slots[binding->slot].semantic_operation;
    uint32_t matches = 0;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->semantic_operation != semantic_operation)
            continue;
        matches++;
        if (!xr_stable_id_equal(call->identity, expected) ||
            call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
            call->caller_function != op->function ||
            call->callee_function != XR_SEMANTIC_INDEX_NONE ||
            call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
            call->source_export != XR_SEMANTIC_INDEX_NONE ||
            !emission_stable_id_is_zero(call->source_export_identity) ||
            !emission_stable_id_is_zero(call->source_callee_identity) ||
            call->result_value != binding->semantic_value ||
            call->result_slot != binding->slot ||
            call->result_register_rep != binding->register_rep ||
            call->result_memory_rep != binding->memory_rep ||
            call->argument_count != 0 || call->adapter_count != 0 || call->flags != 0 ||
            call->result_mode != XR_TARGET_CALL_VALUE ||
            call->result_ownership != XR_TARGET_CALL_BORROW ||
            call->calling_convention != XR_TARGET_CALL_CONVENTION_STRING_BYTE_SLICE_VIEW ||
            call->target_kind != XR_TARGET_CALL_TARGET_STRING_BYTE_SLICE_VIEW)
            return false;
    }
    if (matches != 1)
        return false;
    if (source_value)
        *source_value = source->value;
    return true;
}

static bool emission_allocation_identity_is_canonical(
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
    return xr_stable_id_from_key(operation->allocation_key, &expected,
                                 &digest) &&
           xr_stable_id_equal(expected, operation->allocation_id);
}

static const XrTargetCallRecord *stringbuilder_constructor_call(
    const XrTargetPlan *target_plan,
    const XrTargetValueRepRecord *binding) {
    uint32_t call_count = 0;
    const XrTargetCallRecord *calls =
        xr_target_plan_calls(target_plan, &call_count);
    const XrSemanticOperationRecord *operation =
        binding_operation(target_plan, binding);
    uint32_t slot_count = 0;
    const XrTargetSlotRecord *slots =
        xr_target_plan_slots(target_plan, &slot_count);
    if (!operation || !binding || !slots || binding->slot >= slot_count)
        return NULL;
    uint32_t semantic_operation = slots[binding->slot].semantic_operation;
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; calls && i < call_count; i++) {
        const XrTargetCallRecord *call = &calls[i];
        if (call->semantic_operation != semantic_operation)
            continue;
        if (match || call->semantic_call_target != XR_SEMANTIC_INDEX_NONE ||
            call->caller_function != operation->function ||
            call->callee_function != XR_SEMANTIC_INDEX_NONE ||
            call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
            call->source_export != XR_SEMANTIC_INDEX_NONE ||
            !emission_stable_id_is_zero(call->source_export_identity) ||
            !emission_stable_id_is_zero(call->source_callee_identity) ||
            call->result_value != binding->semantic_value ||
            call->result_slot != binding->slot ||
            call->result_register_rep != binding->register_rep ||
            call->result_memory_rep != binding->memory_rep ||
            call->argument_count != 0 || call->adapter_count != 0 ||
            call->flags != 0 || call->result_mode != XR_TARGET_CALL_VALUE ||
            call->result_ownership != XR_TARGET_CALL_RETURN_OWNED ||
            call->calling_convention !=
                XR_TARGET_CALL_CONVENTION_STRINGBUILDER_CONSTRUCTOR ||
            call->target_kind !=
                XR_TARGET_CALL_TARGET_STRINGBUILDER_CONSTRUCTOR)
            return NULL;
        match = call;
    }
    return match;
}

static bool exact_stringbuilder_new_recipe(
    const XrTargetPlan *target_plan,
    const XrTargetValueRepRecord *binding) {
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation =
        binding_operation(target_plan, binding);
    uint32_t metadata_count = 0;
    const char *const *metadata =
        xr_semantic_plan_metadata(semantic, &metadata_count);
    const XrSemanticTypeRecord *type = operation
        ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    char expected_type_key[160];
    int written = snprintf(
        expected_type_key, sizeof(expected_type_key),
        "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:;named:13:StringBuilder[0]",
        (unsigned) XR_KIND_INSTANCE, (unsigned) XR_TID_STRINGBUILDER,
        (unsigned) XR_SCALAR_REP_NONE);
    return operation && type && written > 0 &&
           (size_t) written < sizeof(expected_type_key) &&
           stringbuilder_constructor_call(target_plan, binding) &&
           operation->opcode == XI_CALL_BUILTIN &&
           operation->result_value == binding->semantic_value &&
           operation->operand_count == 0 && operation->metadata_count == 1 &&
           operation->metadata_begin < metadata_count && metadata &&
           strcmp(metadata[operation->metadata_begin], "StringBuilder") == 0 &&
           operation->auxiliary_kind == XI_AUX_KIND_NONE &&
           operation->semantic_immediate == 0 &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
           operation->effects == xi_generated_op_effects(XI_CALL_BUILTIN) &&
           operation->flags == xi_generated_op_default_flags(XI_CALL_BUILTIN) &&
           operation->ownership_use == xi_generated_op_own_use(XI_CALL_BUILTIN) &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
           operation->transfer_mode == XR_TRANSFER_SHARE &&
           operation->parameter_mode == XR_PARAM_READ &&
           operation->parameter_ownership == XI_OWN_NONE &&
           operation->result_alias_operand == -1 &&
           operation->return_provenance == XR_SEM_RETURN_OWNED &&
           operation->return_parameter == -1 &&
           operation->return_complete == 1 &&
           emission_allocation_identity_is_canonical(operation) &&
           type->kind == XR_KIND_INSTANCE &&
           type->builtin_type == XR_TID_STRINGBUILDER &&
           type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags ==
               (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->canonical_key &&
           strcmp(type->canonical_key, expected_type_key) == 0;
}

/* Builder-side recipe collector. It reconstructs the literal authority from
 * frozen SemanticPlan rows; it never reads Xi values or source types. */
static const char *build_exact_string_literal(
    const XrTargetPlan *target_plan,
    const XrTargetValueRepRecord *binding) {
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation =
        binding_operation(target_plan, binding);
    if (!semantic || !operation || operation->opcode != XI_CONST ||
        operation->operand_count != 0 ||
        operation->result_value != binding->semantic_value ||
        operation->constant >= xr_semantic_plan_constant_count(semantic) ||
        operation->allocation_key ||
        !emission_stable_id_is_zero(operation->allocation_id) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        operation->return_complete != 1)
        return NULL;
    const XrSemanticConstantRecord *constant =
        xr_semantic_plan_constant(semantic, operation->constant);
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(semantic, operation->result_type);
    uint8_t forbidden = XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE |
                        XR_SEM_TYPE_BORROW_VIEW |
                        XR_SEM_TYPE_AGGREGATE_EXACT;
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE |
                       XR_SEM_TYPE_OWNERSHIP_ROOT;
    return constant && constant->kind == XR_SEM_CONST_STRING &&
                   constant->type == operation->result_type &&
                   constant->string && type && type->kind == XR_KIND_STRING &&
                   type->child_count == 0 &&
                   type->scalar_rep == XR_SCALAR_REP_NONE &&
                   type->aggregate_extent == 0 &&
                   type->aggregate_align == 0 &&
                   (type->flags & forbidden) == 0 &&
                   (type->flags & required) == required
               ? constant->string
               : NULL;
}

/* Verifier-side predicate is intentionally independent of the collector
 * above. It proves the emitted recipe from the immutable rows again. */
static const char *verify_expected_string_literal(
    const XrTargetPlan *target_plan,
    const XrTargetValueRepRecord *binding) {
    const XrSemanticPlan *plan = xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *op =
        binding_operation(target_plan, binding);
    if (!plan || !op || op->opcode != XI_CONST || op->operand_count != 0 ||
        op->result_value != binding->semantic_value || op->allocation_key != NULL ||
        !emission_stable_id_is_zero(op->allocation_id) ||
        op->return_complete != 1 ||
        op->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        op->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        op->constant >= xr_semantic_plan_constant_count(plan))
        return NULL;
    const XrSemanticConstantRecord *literal =
        xr_semantic_plan_constant(plan, op->constant);
    const XrSemanticTypeRecord *string_type =
        xr_semantic_plan_type(plan, op->result_type);
    if (!literal || literal->kind != XR_SEM_CONST_STRING ||
        literal->type != op->result_type || !literal->string || !string_type ||
        string_type->kind != XR_KIND_STRING || string_type->child_count != 0 ||
        string_type->scalar_rep != XR_SCALAR_REP_NONE ||
        string_type->aggregate_extent != 0 || string_type->aggregate_align != 0)
        return NULL;
    const uint8_t disallowed = XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE |
                               XR_SEM_TYPE_BORROW_VIEW |
                               XR_SEM_TYPE_AGGREGATE_EXACT;
    const uint8_t mandatory = XR_SEM_TYPE_REFERENCE_CAPABLE |
                              XR_SEM_TYPE_OWNERSHIP_ROOT;
    return (string_type->flags & disallowed) == 0 &&
                   (string_type->flags & mandatory) == mandatory
               ? literal->string
               : NULL;
}

static bool emission_channel_type_is_exact(const XrSemanticPlan *semantic,
                                           uint32_t type_index) {
    const XrSemanticTypeRecord *type =
        xr_semantic_plan_type(semantic, type_index);
    uint32_t child_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(semantic, &child_count);
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE |
                       XR_SEM_TYPE_OWNERSHIP_ROOT;
    return type && type->kind == XR_KIND_CHANNEL &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->child_count == 1 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           (type->flags & required) == required &&
           (type->flags & ~(required | XR_SEM_TYPE_CONST)) == 0 &&
           type->child_begin < child_count &&
           children[type->child_begin] < xr_semantic_plan_type_count(semantic);
}

static bool exact_channel_new_recipe(const XrTargetPlan *target_plan,
                                     const XrTargetValueRepRecord *binding,
                                     uint32_t *capacity_value) {
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation =
        binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    static const char suffix[] = "/allocation";
    size_t canonical_length = operation && operation->canonical_key
                                  ? strlen(operation->canonical_key)
                                  : 0;
    size_t allocation_length = operation && operation->allocation_key
                                   ? strlen(operation->allocation_key)
                                   : 0;
    XrStableId expected_allocation;
    XrFingerprint allocation_digest;
    if (!semantic || !operation || operation->opcode != XI_CHAN_NEW ||
        operation->result_value != binding->semantic_value ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        !operation->canonical_key || !operation->allocation_key ||
        canonical_length > SIZE_MAX - sizeof(suffix) ||
        allocation_length != canonical_length + sizeof(suffix) - 1u ||
        memcmp(operation->allocation_key, operation->canonical_key,
               canonical_length) != 0 ||
        memcmp(operation->allocation_key + canonical_length, suffix,
               sizeof(suffix)) != 0 ||
        !xr_stable_id_from_key(operation->allocation_key,
                               &expected_allocation, &allocation_digest) ||
        !xr_stable_id_equal(expected_allocation, operation->allocation_id) ||
        operation->result_ownership !=
            xi_generated_op_result_ownership(XI_CHAN_NEW) ||
        operation->effects != xi_generated_op_effects(XI_CHAN_NEW) ||
        operation->flags != xi_generated_op_default_flags(XI_CHAN_NEW) ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_complete != 1 ||
        !emission_channel_type_is_exact(semantic, operation->result_type))
        return false;
    const XrSemanticOperandRecord *capacity =
        &operands[operation->operand_begin];
    const XrTargetValueRepRecord *capacity_binding =
        xr_target_plan_value_rep(target_plan, capacity->value);
    const XrTargetMachineRepRecord *capacity_rep = capacity_binding
        ? xr_target_plan_machine_rep(target_plan, capacity_binding->register_rep)
        : NULL;
    if (!capacity_binding || !capacity_rep || capacity->parameter != -1 ||
        capacity->role != XR_SEM_OPERAND_VALUE || capacity->flags != 0 ||
        capacity_rep->kind < XR_MACHINE_REP_I8 ||
        capacity_rep->kind > XR_MACHINE_REP_USIZE)
        return false;
    if (capacity_value)
        *capacity_value = capacity->value;
    return true;
}

static const char *channel_receive_symbol(uint16_t machine_kind) {
    switch ((XrMachineRepKind) machine_kind) {
        case XR_MACHINE_REP_F32:
        case XR_MACHINE_REP_F64:
            return XR_C_CHANNEL_RECV_FLOAT_SYMBOL;
        case XR_MACHINE_REP_I1:
            return XR_C_CHANNEL_RECV_BOOL_SYMBOL;
        case XR_MACHINE_REP_RUNE:
            return XR_C_CHANNEL_RECV_RUNE_SYMBOL;
        case XR_MACHINE_REP_I8:
        case XR_MACHINE_REP_U8:
        case XR_MACHINE_REP_I16:
        case XR_MACHINE_REP_U16:
        case XR_MACHINE_REP_I32:
        case XR_MACHINE_REP_U32:
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_U64:
        case XR_MACHINE_REP_ISIZE:
        case XR_MACHINE_REP_USIZE:
            return XR_C_CHANNEL_RECV_INT_SYMBOL;
        default:
            return NULL;
    }
}

static const char *exact_channel_receive_recipe(
    const XrTargetPlan *target_plan,
    const XrTargetValueRepRecord *binding, uint32_t *receiver_value) {
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation =
        binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    if (!semantic || !operation || operation->opcode != XI_CHAN_TRY_RECV ||
        operation->result_value != binding->semantic_value ||
        operation->operand_count != 1 ||
        operation->operand_begin >= operand_count || operation->allocation_key ||
        !emission_stable_id_is_zero(operation->allocation_id) ||
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
        return NULL;
    const XrSemanticOperandRecord *receiver =
        &operands[operation->operand_begin];
    const XrSemanticTypeRecord *channel =
        xr_semantic_plan_type(semantic, receiver->type);
    uint32_t child_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(semantic, &child_count);
    const XrTargetValueRepRecord *receiver_binding =
        xr_target_plan_value_rep(target_plan, receiver->value);
    const XrTargetMachineRepRecord *receiver_rep = receiver_binding
        ? xr_target_plan_machine_rep(target_plan,
                                     receiver_binding->register_rep)
        : NULL;
    const XrTargetMachineRepRecord *result_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE |
                       XR_SEM_TYPE_OWNERSHIP_ROOT;
    if (!channel || channel->kind != XR_KIND_CHANNEL ||
        channel->scalar_rep != XR_SCALAR_REP_NONE || channel->child_count != 1 ||
        channel->aggregate_extent != 0 || channel->aggregate_align != 0 ||
        (channel->flags & required) != required ||
        (channel->flags & ~(required | XR_SEM_TYPE_CONST)) != 0 ||
        channel->child_begin >= child_count ||
        children[channel->child_begin] != operation->result_type ||
        receiver->role != XR_SEM_OPERAND_VALUE || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->parameter_mode != XR_PARAM_READ ||
        receiver->access != XR_CALL_ARG_PLAIN ||
        receiver->origin != XI_PLACE_ORIGIN_NONE ||
        receiver->lifetime != XI_PLACE_LIFETIME_NONE ||
        receiver->escape != XI_PLACE_ESCAPE_NONE || receiver->flags != 0 ||
        !receiver_binding || !receiver_rep ||
        receiver_rep->kind != XR_MACHINE_REP_DYN_VALUE || !result_rep)
        return NULL;
    const char *symbol = channel_receive_symbol(result_rep->kind);
    if (symbol && receiver_value)
        *receiver_value = receiver->value;
    return symbol;
}

static const char *verify_channel_receive_recipe(
    const XrTargetPlan *target_plan,
    const XrTargetValueRepRecord *binding, uint32_t *receiver_value) {
    const XrSemanticPlan *semantic =
        xr_target_plan_semantic_plan(target_plan);
    const XrSemanticOperationRecord *operation =
        binding_operation(target_plan, binding);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    if (!semantic || !operation || operation->opcode != XI_CHAN_TRY_RECV ||
        operation->result_value != binding->semantic_value ||
        operation->operand_count != 1 ||
        operation->operand_begin >= operand_count || operation->allocation_key ||
        !emission_stable_id_is_zero(operation->allocation_id) ||
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
        return NULL;
    const XrSemanticOperandRecord *receiver =
        &operands[operation->operand_begin];
    const XrSemanticTypeRecord *channel =
        xr_semantic_plan_type(semantic, receiver->type);
    uint32_t child_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(semantic, &child_count);
    const XrTargetValueRepRecord *receiver_binding =
        xr_target_plan_value_rep(target_plan, receiver->value);
    const XrTargetMachineRepRecord *receiver_rep = receiver_binding
        ? xr_target_plan_machine_rep(target_plan,
                                     receiver_binding->register_rep)
        : NULL;
    const XrTargetMachineRepRecord *result_rep =
        xr_target_plan_machine_rep(target_plan, binding->register_rep);
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE |
                       XR_SEM_TYPE_OWNERSHIP_ROOT;
    if (!channel || channel->kind != XR_KIND_CHANNEL ||
        channel->scalar_rep != XR_SCALAR_REP_NONE || channel->child_count != 1 ||
        channel->aggregate_extent != 0 || channel->aggregate_align != 0 ||
        (channel->flags & required) != required ||
        (channel->flags & ~(required | XR_SEM_TYPE_CONST)) != 0 ||
        channel->child_begin >= child_count ||
        children[channel->child_begin] != operation->result_type ||
        receiver->role != XR_SEM_OPERAND_VALUE || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->parameter_mode != XR_PARAM_READ ||
        receiver->access != XR_CALL_ARG_PLAIN ||
        receiver->origin != XI_PLACE_ORIGIN_NONE ||
        receiver->lifetime != XI_PLACE_LIFETIME_NONE ||
        receiver->escape != XI_PLACE_ESCAPE_NONE || receiver->flags != 0 ||
        !receiver_binding || !receiver_rep ||
        receiver_rep->kind != XR_MACHINE_REP_DYN_VALUE || !result_rep)
        return NULL;
    const char *symbol = NULL;
    switch ((XrMachineRepKind) result_rep->kind) {
        case XR_MACHINE_REP_I1: symbol = XR_C_CHANNEL_RECV_BOOL_SYMBOL; break;
        case XR_MACHINE_REP_F32:
        case XR_MACHINE_REP_F64:
            symbol = XR_C_CHANNEL_RECV_FLOAT_SYMBOL;
            break;
        case XR_MACHINE_REP_RUNE: symbol = XR_C_CHANNEL_RECV_RUNE_SYMBOL; break;
        case XR_MACHINE_REP_I8:
        case XR_MACHINE_REP_U8:
        case XR_MACHINE_REP_I16:
        case XR_MACHINE_REP_U16:
        case XR_MACHINE_REP_I32:
        case XR_MACHINE_REP_U32:
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_U64:
        case XR_MACHINE_REP_ISIZE:
        case XR_MACHINE_REP_USIZE:
            symbol = XR_C_CHANNEL_RECV_INT_SYMBOL;
            break;
        default: return NULL;
    }
    if (receiver_value)
        *receiver_value = receiver->value;
    return symbol;
}

static void hash_u64(XrSHA256Context *ctx, uint64_t value) {
    uint8_t encoded[8];
    for (uint32_t i = 0; i < sizeof(encoded); i++)
        encoded[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(ctx, encoded, sizeof(encoded));
}

static void compute_fingerprint(const XrCEmissionPlan *plan, XrFingerprint *out) {
    static const uint8_t domain[] = "xray-c-emission-plan-v11\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    hash_u64(&ctx, plan->schema_version);
    xr_sha256_update(&ctx, plan->target_fingerprint.bytes,
                     sizeof(plan->target_fingerprint.bytes));
    xr_sha256_update(&ctx, plan->profile_fingerprint.bytes,
                     sizeof(plan->profile_fingerprint.bytes));
    hash_u64(&ctx, plan->value_count);
    for (uint32_t i = 0; i < plan->value_count; i++) {
        const XrCValueEmissionView *value = &plan->values[i];
        hash_u64(&ctx, value->semantic_value);
        hash_u64(&ctx, value->target_register_rep);
        hash_u64(&ctx, value->target_memory_rep);
        hash_u64(&ctx, value->target_register_kind);
        hash_u64(&ctx, value->target_memory_kind);
        hash_u64(&ctx, value->register_bits);
        hash_u64(&ctx, value->memory_align);
        hash_u64(&ctx, value->memory_size);
        hash_u64(&ctx, value->rep);
        hash_u64(&ctx, value->materialization);
        hash_u64(&ctx, value->reserved);
        hash_u64(&ctx, value->literal_byte_length);
        hash_u64(&ctx, value->recipe_operand_value);
        size_t c_type_length = strlen(value->c_type);
        hash_u64(&ctx, c_type_length);
        xr_sha256_update(&ctx, (const uint8_t *) value->c_type, c_type_length);
        if (value->literal_byte_length)
            xr_sha256_update(&ctx, (const uint8_t *) value->literal_bytes,
                             value->literal_byte_length);
        size_t recipe_symbol_length = value->recipe_symbol
                                          ? strlen(value->recipe_symbol)
                                          : 0;
        hash_u64(&ctx, recipe_symbol_length);
        if (recipe_symbol_length)
            xr_sha256_update(&ctx, (const uint8_t *) value->recipe_symbol,
                             recipe_symbol_length);
    }
    xr_sha256_final(&ctx, out->bytes);
}

static bool verify_value(const XrCValueEmissionView *value) {
    XrCValueRep expected_rep = XR_C_VALUE_REP_COUNT;
    const char *expected_c_type = NULL;
    if (value->target_register_kind != value->target_memory_kind)
        return false;
    switch (value->target_register_kind) {
        case XR_MACHINE_REP_VOID:
            expected_rep = XR_C_VALUE_REP_VOID;
            expected_c_type = "void";
            break;
        case XR_MACHINE_REP_I1:
            expected_rep = XR_C_VALUE_REP_BOOL;
            expected_c_type = "uint8_t";
            break;
        case XR_MACHINE_REP_I8:
            expected_rep = XR_C_VALUE_REP_I8;
            expected_c_type = "int8_t";
            break;
        case XR_MACHINE_REP_U8:
            expected_rep = XR_C_VALUE_REP_U8;
            expected_c_type = "uint8_t";
            break;
        case XR_MACHINE_REP_I16:
            expected_rep = XR_C_VALUE_REP_I16;
            expected_c_type = "int16_t";
            break;
        case XR_MACHINE_REP_U16:
            expected_rep = XR_C_VALUE_REP_U16;
            expected_c_type = "uint16_t";
            break;
        case XR_MACHINE_REP_I32:
            expected_rep = XR_C_VALUE_REP_I32;
            expected_c_type = "int32_t";
            break;
        case XR_MACHINE_REP_U32:
            expected_rep = XR_C_VALUE_REP_U32;
            expected_c_type = "uint32_t";
            break;
        case XR_MACHINE_REP_I64:
            expected_rep = XR_C_VALUE_REP_I64;
            expected_c_type = "int64_t";
            break;
        case XR_MACHINE_REP_U64:
            expected_rep = XR_C_VALUE_REP_U64;
            expected_c_type = "uint64_t";
            break;
        case XR_MACHINE_REP_ISIZE:
            expected_rep = XR_C_VALUE_REP_ISIZE;
            expected_c_type = "ptrdiff_t";
            break;
        case XR_MACHINE_REP_USIZE:
            expected_rep = XR_C_VALUE_REP_USIZE;
            expected_c_type = "size_t";
            break;
        case XR_MACHINE_REP_F32:
            expected_rep = XR_C_VALUE_REP_F32;
            expected_c_type = "float";
            break;
        case XR_MACHINE_REP_F64:
            expected_rep = XR_C_VALUE_REP_F64;
            expected_c_type = "double";
            break;
        case XR_MACHINE_REP_RUNE:
            expected_rep = XR_C_VALUE_REP_RUNE;
            expected_c_type = "uint32_t";
            break;
        case XR_MACHINE_REP_DYN_VALUE:
            expected_rep = XR_C_VALUE_REP_TAGGED;
            expected_c_type = "XrValue";
            break;
        case XR_MACHINE_REP_VIEW:
            expected_rep = XR_C_VALUE_REP_VIEW;
            expected_c_type = "xr_span_t";
            break;
        default: return false;
    }
    bool recipe_valid = value->materialization ==
                                XR_C_VALUE_MATERIALIZATION_NONE
                            ? value->literal_byte_length == 0 &&
                                  value->literal_bytes == NULL &&
                                  value->recipe_operand_value == UINT32_MAX &&
                                  value->recipe_symbol == NULL
                            : value->materialization ==
                                      XR_C_VALUE_MATERIALIZATION_STRING_LITERAL_VIEW &&
                                  value->rep == XR_C_VALUE_REP_TAGGED &&
                                  value->literal_bytes != NULL &&
                                  strlen(value->literal_bytes) ==
                                      value->literal_byte_length &&
                                  value->recipe_operand_value == UINT32_MAX &&
                                  value->recipe_symbol == NULL;
    if (value->materialization == XR_C_VALUE_MATERIALIZATION_CHANNEL_NEW)
        recipe_valid = value->rep == XR_C_VALUE_REP_TAGGED &&
                       value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL &&
                       value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_symbol &&
                       strcmp(value->recipe_symbol,
                              XR_C_CHANNEL_NEW_SYMBOL) == 0;
    if (value->materialization ==
        XR_C_VALUE_MATERIALIZATION_CHANNEL_RECV_PAYLOAD)
        recipe_valid = value->rep != XR_C_VALUE_REP_VOID &&
                       value->rep != XR_C_VALUE_REP_TAGGED &&
                       value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL &&
                       value->recipe_operand_value != UINT32_MAX &&
                       channel_receive_symbol(value->target_register_kind) &&
                       value->recipe_symbol &&
                       strcmp(value->recipe_symbol,
                              channel_receive_symbol(
                                  value->target_register_kind)) == 0;
    if (value->materialization ==
        XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_NEW)
        recipe_valid = value->rep == XR_C_VALUE_REP_TAGGED &&
                       value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL &&
                       value->recipe_operand_value == UINT32_MAX &&
                       value->recipe_symbol &&
                       strcmp(value->recipe_symbol,
                              XR_C_STRINGBUILDER_NEW_SYMBOL) == 0;
    if (value->materialization ==
        XR_C_VALUE_MATERIALIZATION_STRING_BYTE_SLICE_VIEW)
        recipe_valid = value->rep == XR_C_VALUE_REP_VIEW &&
                       value->literal_byte_length == 0 &&
                       value->literal_bytes == NULL &&
                       value->recipe_operand_value != UINT32_MAX &&
                       value->recipe_symbol &&
                       strcmp(value->recipe_symbol,
                              XR_C_STRING_BYTE_SLICE_VIEW_SYMBOL) == 0;
    return expected_rep == (XrCValueRep) value->rep && value->c_type &&
           value->reserved == 0 && recipe_valid &&
           strcmp(value->c_type, expected_c_type) == 0 &&
           (value->rep == XR_C_VALUE_REP_VOID
                ? value->register_bits == 0 && value->memory_size == 0 &&
                      value->memory_align == 0
                : value->register_bits != 0 && value->memory_size != 0 &&
                      value->memory_align != 0);
}

static bool verify_plan(const XrCEmissionPlan *plan) {
    if (!plan || plan->schema_version != XR_C_EMISSION_PLAN_SCHEMA_VERSION ||
        (plan->value_count && !plan->values))
        return false;
    for (uint32_t i = 0; i < plan->value_count; i++) {
        if (!verify_value(&plan->values[i]) ||
            (i && plan->values[i - 1u].semantic_value >= plan->values[i].semantic_value))
            return false;
    }
    XrFingerprint actual = {{0}};
    compute_fingerprint(plan, &actual);
    return xr_fingerprint_equal(actual, plan->fingerprint);
}

static bool verify_target_kind_projection(uint16_t kind,
                                          XrCValueRep *out_rep,
                                          const char **out_c_type) {
    if (!out_rep || !out_c_type)
        return false;
    switch ((XrMachineRepKind) kind) {
        case XR_MACHINE_REP_VOID:
            *out_rep = XR_C_VALUE_REP_VOID;
            *out_c_type = "void";
            return true;
        case XR_MACHINE_REP_I1:
            *out_rep = XR_C_VALUE_REP_BOOL;
            *out_c_type = "uint8_t";
            return true;
        case XR_MACHINE_REP_I8:
            *out_rep = XR_C_VALUE_REP_I8;
            *out_c_type = "int8_t";
            return true;
        case XR_MACHINE_REP_U8:
            *out_rep = XR_C_VALUE_REP_U8;
            *out_c_type = "uint8_t";
            return true;
        case XR_MACHINE_REP_I16:
            *out_rep = XR_C_VALUE_REP_I16;
            *out_c_type = "int16_t";
            return true;
        case XR_MACHINE_REP_U16:
            *out_rep = XR_C_VALUE_REP_U16;
            *out_c_type = "uint16_t";
            return true;
        case XR_MACHINE_REP_I32:
            *out_rep = XR_C_VALUE_REP_I32;
            *out_c_type = "int32_t";
            return true;
        case XR_MACHINE_REP_U32:
            *out_rep = XR_C_VALUE_REP_U32;
            *out_c_type = "uint32_t";
            return true;
        case XR_MACHINE_REP_I64:
            *out_rep = XR_C_VALUE_REP_I64;
            *out_c_type = "int64_t";
            return true;
        case XR_MACHINE_REP_U64:
            *out_rep = XR_C_VALUE_REP_U64;
            *out_c_type = "uint64_t";
            return true;
        case XR_MACHINE_REP_ISIZE:
            *out_rep = XR_C_VALUE_REP_ISIZE;
            *out_c_type = "ptrdiff_t";
            return true;
        case XR_MACHINE_REP_USIZE:
            *out_rep = XR_C_VALUE_REP_USIZE;
            *out_c_type = "size_t";
            return true;
        case XR_MACHINE_REP_F32:
            *out_rep = XR_C_VALUE_REP_F32;
            *out_c_type = "float";
            return true;
        case XR_MACHINE_REP_F64:
            *out_rep = XR_C_VALUE_REP_F64;
            *out_c_type = "double";
            return true;
        case XR_MACHINE_REP_RUNE:
            *out_rep = XR_C_VALUE_REP_RUNE;
            *out_c_type = "uint32_t";
            return true;
        case XR_MACHINE_REP_DYN_VALUE:
            *out_rep = XR_C_VALUE_REP_TAGGED;
            *out_c_type = "XrValue";
            return true;
        case XR_MACHINE_REP_VIEW:
            *out_rep = XR_C_VALUE_REP_VIEW;
            *out_c_type = "xr_span_t";
            return true;
        default: return false;
    }
}

bool xr_c_emission_plan_verify(
    const XrCEmissionPlan *plan, const XrTargetPlan *target_plan,
    XrFingerprint expected_profile_fingerprint, char *error,
    size_t error_size) {
    if (!plan || !target_plan || !xr_target_plan_is_verified(target_plan))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission verification authority is missing");
    XrFingerprint target_fingerprint = xr_target_plan_fingerprint(target_plan);
    const XrTargetProfile *profile = xr_target_plan_profile(target_plan);
    XrFingerprint profile_fingerprint =
        xr_target_profile_fingerprint(profile);
    if (!profile ||
        !xr_fingerprint_equal(profile_fingerprint,
                              expected_profile_fingerprint) ||
        !xr_fingerprint_equal(plan->profile_fingerprint,
                              expected_profile_fingerprint))
        return emission_error(error, error_size, "XR_TARGET_1000",
                              "C emission profile fingerprint is stale");
    if (!xr_fingerprint_equal(plan->target_fingerprint,
                              target_fingerprint))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission TargetPlan fingerprint is stale");
    if (plan->schema_version != XR_C_EMISSION_PLAN_SCHEMA_VERSION ||
        (plan->value_count && !plan->values))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission schema is invalid");

    uint32_t value_count = 0;
    const XrTargetValueRepRecord *values =
        xr_target_plan_value_reps(target_plan, &value_count);
    if (value_count && !values)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "TargetPlan value-representation table is missing");
    uint32_t projected = 0;
    for (uint32_t i = 0; i < value_count; i++) {
        const XrTargetValueRepRecord *binding = &values[i];
        const XrTargetMachineRepRecord *register_rep =
            xr_target_plan_machine_rep(target_plan, binding->register_rep);
        const XrTargetMachineRepRecord *memory_rep =
            xr_target_plan_machine_rep(target_plan, binding->memory_rep);
        XrCValueRep expected_register = XR_C_VALUE_REP_COUNT;
        XrCValueRep expected_memory = XR_C_VALUE_REP_COUNT;
        const char *register_c_type = NULL;
        const char *memory_c_type = NULL;
        bool register_supported = register_rep &&
            verify_target_kind_projection(register_rep->kind,
                                          &expected_register,
                                          &register_c_type);
        bool memory_supported = memory_rep &&
            verify_target_kind_projection(memory_rep->kind, &expected_memory,
                                          &memory_c_type);
        if (!register_supported && !memory_supported)
            continue;
        if (!register_supported || !memory_supported ||
            register_rep->kind != memory_rep->kind ||
            expected_register != expected_memory ||
            strcmp(register_c_type, memory_c_type) != 0)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "TargetPlan C projection is inconsistent");
        if (projected >= plan->value_count)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission projection is missing a TargetPlan row");
        const XrCValueEmissionView *row = &plan->values[projected++];
        if (row->semantic_value > binding->semantic_value)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission projection is missing a TargetPlan row");
        if (row->semantic_value < binding->semantic_value)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission projection has an extra row");
        if (row->semantic_value != binding->semantic_value ||
            row->target_register_rep != binding->register_rep ||
            row->target_memory_rep != binding->memory_rep ||
            row->target_register_kind != register_rep->kind ||
            row->target_memory_kind != memory_rep->kind ||
            row->register_bits != register_rep->register_bits ||
            row->memory_size != memory_rep->memory_size ||
            row->memory_align != memory_rep->memory_align ||
            row->rep != (uint8_t) expected_register || !row->c_type ||
            strcmp(row->c_type, register_c_type) != 0)
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission row disagrees with TargetPlan authority");
        const char *expected_literal =
            verify_expected_string_literal(target_plan, binding);
        uint32_t expected_capacity = UINT32_MAX;
        bool expected_channel = exact_channel_new_recipe(
            target_plan, binding, &expected_capacity);
        uint32_t expected_receiver = UINT32_MAX;
        const char *expected_receive_symbol =
            verify_channel_receive_recipe(target_plan, binding,
                                           &expected_receiver);
        bool expected_stringbuilder =
            exact_stringbuilder_new_recipe(target_plan, binding);
        uint32_t expected_view_source = UINT32_MAX;
        bool expected_string_byte_slice_view =
            verify_exact_string_byte_slice_view_recipe(
                target_plan, binding, &expected_view_source);
        uint8_t expected_recipe = expected_literal
                                      ? XR_C_VALUE_MATERIALIZATION_STRING_LITERAL_VIEW
                                      : expected_channel
                                            ? XR_C_VALUE_MATERIALIZATION_CHANNEL_NEW
                                            : expected_receive_symbol
                                                  ? XR_C_VALUE_MATERIALIZATION_CHANNEL_RECV_PAYLOAD
                                                  : expected_stringbuilder
                                                        ? XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_NEW
                                                        : expected_string_byte_slice_view
                                                              ? XR_C_VALUE_MATERIALIZATION_STRING_BYTE_SLICE_VIEW
                                                              : XR_C_VALUE_MATERIALIZATION_NONE;
        uint32_t expected_operand = expected_channel
                                        ? expected_capacity
                                        : expected_receive_symbol
                                              ? expected_receiver
                                              : expected_string_byte_slice_view
                                                    ? expected_view_source
                                                    : UINT32_MAX;
        const char *expected_symbol = expected_channel
                                          ? XR_C_CHANNEL_NEW_SYMBOL
                                          : expected_receive_symbol
                                                ? expected_receive_symbol
                                                : expected_stringbuilder
                                                      ? XR_C_STRINGBUILDER_NEW_SYMBOL
                                                      : expected_string_byte_slice_view
                                                            ? XR_C_STRING_BYTE_SLICE_VIEW_SYMBOL
                                                            : NULL;
        size_t expected_length = expected_literal ? strlen(expected_literal) : 0;
        if (row->materialization != expected_recipe || row->reserved != 0 ||
            row->literal_byte_length != expected_length ||
            row->recipe_operand_value != expected_operand ||
            (expected_symbol
                 ? (!row->recipe_symbol ||
                    strcmp(row->recipe_symbol,
                           expected_symbol) != 0)
                 : row->recipe_symbol != NULL) ||
            (expected_literal
                 ? (!row->literal_bytes ||
                    memcmp(row->literal_bytes, expected_literal,
                           expected_length + 1u) != 0)
                 : row->literal_bytes != NULL))
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "C emission materialization recipe is not exact");
    }
    if (projected != plan->value_count)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission projection has an extra row");
    if (!verify_plan(plan))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission fingerprint or canonical form is invalid");
    return true;
}

bool xr_c_emission_plan_build(const XrTargetPlan *target_plan,
                              XrFingerprint expected_profile_fingerprint,
                              XrCEmissionPlan **out, char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!target_plan || !out)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission plan input is missing");
    if (!xr_target_plan_is_verified(target_plan))
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission plan requires a verified TargetPlan");
    const uint64_t required_value_families =
        XR_TARGET_FAMILY_SCALAR | XR_TARGET_FAMILY_CLOSURE_STORAGE |
        XR_TARGET_FAMILY_STRING_LITERAL_STORAGE |
        XR_TARGET_FAMILY_DIRECT_LOCAL_CALLEE_STORAGE |
        XR_TARGET_FAMILY_DIRECT_LOCAL_GO_CALLEE_STORAGE |
        XR_TARGET_FAMILY_CHANNEL_ALLOCATION_STORAGE |
        XR_TARGET_FAMILY_CHANNEL_RECEIVE_STORAGE |
        XR_TARGET_FAMILY_SOURCE_NAMESPACE_STORAGE |
        XR_TARGET_FAMILY_STRING_BYTE_SLICE_VIEW_STORAGE;
    if ((xr_target_plan_completed_family_mask(target_plan) &
         required_value_families) != required_value_families)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission plan requires completed value-storage families");
    const XrTargetProfile *profile = xr_target_plan_profile(target_plan);
    XrFingerprint actual_profile_fingerprint = xr_target_profile_fingerprint(profile);
    if (!profile || !xr_fingerprint_equal(actual_profile_fingerprint,
                                           expected_profile_fingerprint))
        return emission_error(error, error_size, "XR_TARGET_1000",
                              "C emission target profile fingerprint does not match");
    uint32_t target_value_count = 0;
    const XrTargetValueRepRecord *values =
        xr_target_plan_value_reps(target_plan, &target_value_count);
    if (target_value_count && !values)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "TargetPlan value-representation table is missing");
    uint32_t emission_value_count = 0;
    for (uint32_t i = 0; i < target_value_count; i++) {
        const XrTargetValueRepRecord *binding = &values[i];
        const XrTargetMachineRepRecord *register_rep =
            xr_target_plan_machine_rep(target_plan, binding->register_rep);
        const XrTargetMachineRepRecord *memory_rep =
            xr_target_plan_machine_rep(target_plan, binding->memory_rep);
        XrCValueRep register_c_rep = XR_C_VALUE_REP_COUNT;
        XrCValueRep memory_c_rep = XR_C_VALUE_REP_COUNT;
        const char *register_c_type = NULL;
        const char *memory_c_type = NULL;
        bool register_is_value =
            register_rep && machine_kind_to_c_rep(register_rep->kind, &register_c_rep,
                                                   &register_c_type);
        bool memory_is_value =
            memory_rep && machine_kind_to_c_rep(memory_rep->kind, &memory_c_rep,
                                                 &memory_c_type);
        if (!register_is_value && !memory_is_value)
            continue;
        if (!register_is_value || !memory_is_value || register_rep->kind != memory_rep->kind ||
            register_c_rep != memory_c_rep || strcmp(register_c_type, memory_c_type) != 0) {
            return emission_error(error, error_size, "XR_TARGET_1001",
                                  "TargetPlan value binding has no exact C projection");
        }
        emission_value_count++;
    }
    if (emission_value_count > SIZE_MAX / sizeof(XrCValueEmissionView))
        return emission_error(error, error_size, "XR_EXEC_5003",
                              "C emission value record budget overflow");
    XrCEmissionPlan *plan = (XrCEmissionPlan *) xr_calloc(1, sizeof(*plan));
    if (!plan)
        return emission_error(error, error_size, "XR_EXEC_5003",
                              "C emission plan allocation failed");
    if (emission_value_count) {
        plan->values =
            (XrCValueEmissionView *) xr_calloc(emission_value_count,
                                               sizeof(*plan->values));
        if (!plan->values) {
            xr_c_emission_plan_free(plan);
            return emission_error(error, error_size, "XR_EXEC_5003",
                                  "C emission value record allocation failed");
        }
    }
    plan->value_count = emission_value_count;
    plan->schema_version = XR_C_EMISSION_PLAN_SCHEMA_VERSION;
    plan->target_fingerprint = xr_target_plan_fingerprint(target_plan);
    plan->profile_fingerprint = actual_profile_fingerprint;
    uint32_t value_index = 0;
    for (uint32_t i = 0; i < target_value_count; i++) {
        const XrTargetValueRepRecord *binding = &values[i];
        const XrTargetMachineRepRecord *register_rep =
            xr_target_plan_machine_rep(target_plan, binding->register_rep);
        const XrTargetMachineRepRecord *memory_rep =
            xr_target_plan_machine_rep(target_plan, binding->memory_rep);
        XrCValueRep c_rep = XR_C_VALUE_REP_COUNT;
        const char *c_type = NULL;
        if (!register_rep || !memory_rep || register_rep->kind != memory_rep->kind ||
            !machine_kind_to_c_rep(register_rep->kind, &c_rep, &c_type))
            continue;
        XrCValueEmissionView *value = &plan->values[value_index++];
        value->semantic_value = binding->semantic_value;
        value->target_register_rep = binding->register_rep;
        value->target_memory_rep = binding->memory_rep;
        value->target_register_kind = register_rep->kind;
        value->target_memory_kind = memory_rep->kind;
        value->register_bits = register_rep->register_bits;
        value->memory_align = memory_rep->memory_align;
        value->memory_size = memory_rep->memory_size;
        value->rep = (uint8_t) c_rep;
        value->recipe_operand_value = UINT32_MAX;
        value->c_type = c_type;
        const char *literal = build_exact_string_literal(target_plan, binding);
        if (literal) {
            size_t length = strlen(literal);
            if (length > UINT32_MAX) {
                xr_c_emission_plan_free(plan);
                return emission_error(error, error_size, "XR_EXEC_5003",
                                      "String literal exceeds C emission budget");
            }
            char *owned = (char *) xr_malloc(length + 1u);
            if (!owned) {
                xr_c_emission_plan_free(plan);
                return emission_error(error, error_size, "XR_EXEC_5003",
                                      "String literal C emission allocation failed");
            }
            memcpy(owned, literal, length + 1u);
            value->materialization =
                XR_C_VALUE_MATERIALIZATION_STRING_LITERAL_VIEW;
            value->literal_byte_length = (uint32_t) length;
            value->literal_bytes = owned;
        } else {
            uint32_t capacity = UINT32_MAX;
            if (exact_channel_new_recipe(target_plan, binding, &capacity)) {
                size_t symbol_length = sizeof(XR_C_CHANNEL_NEW_SYMBOL);
                char *owned = (char *) xr_malloc(symbol_length);
                if (!owned) {
                    xr_c_emission_plan_free(plan);
                    return emission_error(error, error_size, "XR_EXEC_5003",
                                          "channel recipe symbol allocation failed");
                }
                memcpy(owned, XR_C_CHANNEL_NEW_SYMBOL, symbol_length);
                value->materialization =
                    XR_C_VALUE_MATERIALIZATION_CHANNEL_NEW;
                value->recipe_operand_value = capacity;
                value->recipe_symbol = owned;
            } else {
                uint32_t receiver = UINT32_MAX;
                const char *symbol = exact_channel_receive_recipe(
                    target_plan, binding, &receiver);
                if (symbol) {
                    size_t symbol_length = strlen(symbol) + 1u;
                    char *owned = (char *) xr_malloc(symbol_length);
                    if (!owned) {
                        xr_c_emission_plan_free(plan);
                        return emission_error(
                            error, error_size, "XR_EXEC_5003",
                            "channel receive recipe allocation failed");
                    }
                    memcpy(owned, symbol, symbol_length);
                    value->materialization =
                        XR_C_VALUE_MATERIALIZATION_CHANNEL_RECV_PAYLOAD;
                    value->recipe_operand_value = receiver;
                    value->recipe_symbol = owned;
                } else if (exact_stringbuilder_new_recipe(target_plan,
                                                          binding)) {
                    size_t symbol_length =
                        sizeof(XR_C_STRINGBUILDER_NEW_SYMBOL);
                    char *owned = (char *) xr_malloc(symbol_length);
                    if (!owned) {
                        xr_c_emission_plan_free(plan);
                        return emission_error(
                            error, error_size, "XR_EXEC_5003",
                            "StringBuilder recipe symbol allocation failed");
                    }
                    memcpy(owned, XR_C_STRINGBUILDER_NEW_SYMBOL,
                           symbol_length);
                    value->materialization =
                        XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_NEW;
                    value->recipe_symbol = owned;
                } else {
                    uint32_t source_value = UINT32_MAX;
                    if (build_exact_string_byte_slice_view_recipe(
                            target_plan, binding, &source_value)) {
                        size_t symbol_length =
                            sizeof(XR_C_STRING_BYTE_SLICE_VIEW_SYMBOL);
                        char *owned = (char *) xr_malloc(symbol_length);
                        if (!owned) {
                            xr_c_emission_plan_free(plan);
                            return emission_error(
                                error, error_size, "XR_EXEC_5003",
                                "string byte-slice view recipe symbol allocation failed");
                        }
                        memcpy(owned, XR_C_STRING_BYTE_SLICE_VIEW_SYMBOL,
                               symbol_length);
                        value->materialization =
                            XR_C_VALUE_MATERIALIZATION_STRING_BYTE_SLICE_VIEW;
                        value->recipe_operand_value = source_value;
                        value->recipe_symbol = owned;
                    }
                }
            }
        }
    }
    if (value_index != emission_value_count) {
        xr_c_emission_plan_free(plan);
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "C emission value partition is not exact");
    }
    compute_fingerprint(plan, &plan->fingerprint);
    if (!xr_c_emission_plan_verify(plan, target_plan,
                                   expected_profile_fingerprint, error,
                                   error_size)) {
        xr_c_emission_plan_free(plan);
        return false;
    }
    plan->verified = true;
    *out = plan;
    return true;
}

void xr_c_emission_plan_free(XrCEmissionPlan *plan) {
    if (!plan)
        return;
    for (uint32_t i = 0; i < plan->value_count; i++)
        xr_free((void *) plan->values[i].literal_bytes);
    for (uint32_t i = 0; i < plan->value_count; i++)
        xr_free((void *) plan->values[i].recipe_symbol);
    xr_free(plan->values);
    xr_free(plan);
}

bool xr_c_emission_plan_is_verified(const XrCEmissionPlan *plan) {
    return plan && plan->verified;
}

uint32_t xr_c_emission_plan_value_count(const XrCEmissionPlan *plan) {
    return plan ? plan->value_count : 0;
}

XrFingerprint xr_c_emission_plan_fingerprint(const XrCEmissionPlan *plan) {
    XrFingerprint zero = {{0}};
    return plan ? plan->fingerprint : zero;
}

XrFingerprint xr_c_emission_plan_target_fingerprint(const XrCEmissionPlan *plan) {
    XrFingerprint zero = {{0}};
    return plan ? plan->target_fingerprint : zero;
}

XrFingerprint xr_c_emission_plan_profile_fingerprint(const XrCEmissionPlan *plan) {
    XrFingerprint zero = {{0}};
    return plan ? plan->profile_fingerprint : zero;
}

bool xr_c_emission_plan_value_view(const XrCEmissionPlan *plan, uint32_t semantic_value,
                                    XrCValueEmissionView *out, char *error,
                                    size_t error_size) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!plan || !plan->verified || !out)
        return emission_error(error, error_size, "XR_TARGET_1001",
                              "verified C emission plan input is missing");
    uint32_t begin = 0;
    uint32_t end = plan->value_count;
    while (begin < end) {
        uint32_t middle = begin + (end - begin) / 2u;
        const XrCValueEmissionView *candidate = &plan->values[middle];
        if (candidate->semantic_value == semantic_value) {
            *out = *candidate;
            return true;
        }
        if (candidate->semantic_value < semantic_value)
            begin = middle + 1u;
        else
            end = middle;
    }
    return emission_error(error, error_size, "XR_TARGET_1001",
                          "semantic C value has no immutable C emission binding");
}
