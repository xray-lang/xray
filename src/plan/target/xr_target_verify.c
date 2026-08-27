/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_verify.c - Independent structural TargetPlan verifier
 *
 * KEY CONCEPT:
 *   Verification consumes only the frozen semantic and target schemas. It
 *   does not call an AOT planner, C emitter, or VM dispatcher, so malformed
 *   plans fail before any backend can reinterpret missing facts.
 */

#include "../semantic/xr_semantic_heap_literal_shape.h"
#include "xr_target_verify.h"
#include "xr_target_capability.h"
#include "xr_target_instruction_verify.h"
#include "xr_i64_overflow_target_instruction.h"
#include "xr_target_entry_abi.h"
#include "xr_target_plan_internal.h"
#include "xr_target_profile_internal.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../frontend/analyzer/xa_intrinsic_registry.h"
#include "../semantic/xr_semantic_verify.h"
#include "../../stdlib/xstdlib_metadata.h"
#include "../semantic/xr_semantic_allocation_shape.h"
#include "../semantic/xr_semantic_array_type_shape.h"
#include "../semantic/xr_semantic_class_shape.h"
#include "../semantic/xr_semantic_coroutine_lifecycle_shape.h"
#include "../semantic/xr_semantic_enum_shape.h"
#include "../semantic/xr_semantic_range_slice_shape.h"
#include "../semantic/xr_semantic_string_shape.h"
#include "../semantic/xr_semantic_cleanup_shape.h"
#include "../semantic/xr_semantic_task_shape.h"
#include "../semantic/xr_semantic_string_runes_shape.h"
#include "../semantic/xr_semantic_iterator_rune_has_next_shape.h"
#include "../semantic/xr_semantic_iterator_rune_next_shape.h"
#include "../semantic/xr_semantic_iterator_rune_nth_shape.h"
#include "../semantic/xr_semantic_rune_to_string_shape.h"
#include "../semantic/xr_semantic_rune_is_whitespace_shape.h"
#include "../semantic/xr_semantic_string_slice_shape.h"
#include "../semantic/xr_semantic_native_module_shape.h"
#include "../semantic/xr_semantic_native_leaf_shape.h"
#include "../semantic/xr_semantic_value_aggregate_shape.h"
#include "../ownership/xr_ownership_certificate.h"
#include "../semantic/xr_semantic_graph.h"
#include "../../base/xmalloc.h"
#include "../../runtime/value/xtype.h"
#include "../semantic/xr_semantic_array_member_shape.h"
#include "../semantic/xr_semantic_container_copy_shape.h"
#include "../semantic/xr_semantic_identity_copy_shape.h"
#include "../semantic/xr_semantic_owner_forward_shape.h"
#include "../semantic/xr_semantic_dynamic_value_shape.h"
#include "../semantic/xr_semantic_direct_callee_shape.h"
#include "../semantic/xr_semantic_local_call_target_shape.h"
#include "../semantic/xr_semantic_class_seal_shape.h"
#include "xr_target_scalar_rep_shape.h"
#include "../semantic/xr_semantic_local_addr_shape.h"
#include "../semantic/xr_semantic_panic_catch_shape.h"
#include "../semantic/xr_semantic_type_admission_shape.h"
#include "../semantic/xr_semantic_panic_info_shape.h"
#include "../semantic/xr_semantic_scalar_copy_shape.h"
#include "xr_target_array_storage_shape.h"
#include "../semantic/xr_program_semantic_closure.h"
#include "../../base/xsha256.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include "../../shared/xr_align_guard.h"
#include <stdio.h>
#include <string.h>

static int target_plan_layout_for_type(const XrTargetPlan *plan, uint32_t semantic_type);

static bool report(char *error, size_t size, const char *code, const char *detail) {
    if (error && size)
        snprintf(error, size, "%s: %s", code, detail);
    return false;
}

static bool is_power_of_two(uint32_t value) {
    return value && (value & (value - 1u)) == 0;
}

static bool range_valid(uint32_t begin, uint32_t count, uint32_t limit) {
    return begin <= limit && count <= limit - begin;
}

static bool checked_u32_add(uint32_t left, uint32_t right, uint32_t *out) {
    if (left > UINT32_MAX - right)
        return false;
    *out = left + right;
    return true;
}

static bool fingerprint_is_zero(XrFingerprint fingerprint) {
    uint8_t combined = 0;
    for (uint32_t i = 0; i < sizeof(fingerprint.bytes); i++)
        combined |= fingerprint.bytes[i];
    return combined == 0;
}

static bool stable_id_is_zero(XrStableId id) {
    uint8_t combined = 0;
    for (uint32_t i = 0; i < sizeof(id.bytes); i++)
        combined |= id.bytes[i];
    return combined == 0;
}

typedef struct XrVerifyLeafProgramShape {
    const XrSemanticProgramTypeBinding *aggregate_binding;
    const XrSemanticProgramTypeBinding *scalar_binding;
    const XrSemanticProgramFunctionBinding *caller_binding;
    const XrSemanticProgramFunctionBinding *callee_binding;
    const XrSemanticProgramCallBinding *call_binding;
    const XrSemanticFunctionRecord *caller;
    const XrSemanticFunctionRecord *callee;
    const XrSemanticParameterRecord *parameter;
    const XrSemanticOperationRecord *operation;
    const XrSemanticCallTargetRecord *target;
    const XrSemanticOperandRecord *argument;
    uint32_t parameter_index;
    uint32_t target_index;
    uint32_t argument_index;
} XrVerifyLeafProgramShape;

typedef struct XrVerifyProductProgramShape {
    const XrSemanticProgramTypeBinding *product;
    const XrSemanticProgramTypeBinding *i64;
    const XrSemanticProgramTypeBinding *u8;
    const XrSemanticProgramFunctionBinding *caller_bindings[2];
    const XrSemanticProgramFunctionBinding *callee_binding;
    const XrSemanticFunctionRecord *callers[2];
    const XrSemanticFunctionRecord *callee;
    const XrSemanticProgramCallBinding *call_bindings[2];
    const XrSemanticOperationRecord *calls[2];
    uint32_t target_indices[2];
} XrVerifyProductProgramShape;

static XrStableId verify_program_generation(uint32_t schema, XrFingerprint fingerprint) {
    static const uint8_t domain[] = "xray-generation-closure-id-v1\0";
    uint8_t schema_bytes[4], digest[XR_FINGERPRINT_BYTES];
    XrSHA256Context context;
    XrStableId result = {{0}};
    for (uint32_t i = 0; i < sizeof(schema_bytes); i++)
        schema_bytes[i] = (uint8_t) (schema >> (i * 8u));
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain) - 1u);
    xr_sha256_update(&context, schema_bytes, sizeof(schema_bytes));
    xr_sha256_update(&context, fingerprint.bytes, sizeof(fingerprint.bytes));
    xr_sha256_final(&context, digest);
    memcpy(result.bytes, digest, sizeof(result.bytes));
    return result;
}

static bool semantic_is_leaf_program_family(const XrSemanticPlan *semantic) {
    const XrSemanticProgramProvenance *row = xr_semantic_plan_program_provenance(semantic);
    return row &&
           row->program_family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL;
}

static bool semantic_is_product_program_family(const XrSemanticPlan *semantic) {
    const XrSemanticProgramProvenance *row =
        xr_semantic_plan_program_provenance(semantic);
    return row && row->program_family ==
                      XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL;
}

/* Reconstruct the frozen product authority from serialized SemanticPlan rows.
 * This verifier deliberately does not call any TargetPlan builder predicate. */
static bool verify_product_program_shape(const XrSemanticPlan *semantic,
                                         XrVerifyProductProgramShape *shape) {
    if (shape)
        memset(shape, 0, sizeof(*shape));
    const XrSemanticProgramProvenance *provenance =
        semantic ? xr_semantic_plan_program_provenance(semantic) : NULL;
    if (!semantic || !shape || !semantic_is_product_program_family(semantic) ||
        !provenance || provenance->type_count != 3 ||
        provenance->type_field_count != 6 || provenance->function_count != 3 ||
        provenance->call_count != 2 || provenance->module_count != 1 ||
        provenance->dependency_count != 0 ||
        xr_semantic_plan_program_type_binding_count(semantic) != 3 ||
        xr_semantic_plan_program_type_field_binding_count(semantic) != 6 ||
        xr_semantic_plan_program_function_binding_count(semantic) != 3 ||
        xr_semantic_plan_program_call_binding_count(semantic) != 2 ||
        xr_semantic_plan_call_target_count(semantic) != 2)
        return false;
    for (uint32_t row = 0; row < 3; row++) {
        const XrSemanticProgramTypeBinding *binding =
            xr_semantic_plan_program_type_for_row(semantic, row);
        const XrSemanticTypeRecord *type =
            binding ? xr_semantic_plan_type(semantic, binding->semantic_type) : NULL;
        if (!binding || !type || binding->program_row != row)
            return false;
        if (binding->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT) {
            if (shape->product || binding->field_count != 6 ||
                type->kind != XR_KIND_TUPLE || type->child_count != 6 ||
                type->aggregate_extent != 6 || type->flags != XR_SEM_TYPE_VALUE ||
                !stable_id_is_zero(binding->source_class_identity))
                return false;
            shape->product = binding;
        } else if (binding->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR &&
                   binding->exact_scalar == XR_EXACT_SCALAR_I64) {
            if (shape->i64 || type->kind != XR_KIND_INT ||
                type->scalar_rep != XR_NATIVE_I64 || type->child_count != 0)
                return false;
            shape->i64 = binding;
        } else if (binding->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR &&
                   binding->exact_scalar == XR_EXACT_SCALAR_U8) {
            if (shape->u8 || type->kind != XR_KIND_INT ||
                type->scalar_rep != XR_NATIVE_U8 || type->child_count != 0)
                return false;
            shape->u8 = binding;
        } else {
            return false;
        }
    }
    if (!shape->product || !shape->i64 || !shape->u8)
        return false;
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    const XrSemanticTypeRecord *product_type =
        xr_semantic_plan_type(semantic, shape->product->semantic_type);
    if (!children || !product_type ||
        !range_valid(product_type->child_begin, product_type->child_count, child_count))
        return false;
    for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
        const XrSemanticProgramTypeBinding *member = ordinal == 2 ? shape->u8 : shape->i64;
        const XrSemanticProgramTypeFieldBinding *field =
            xr_semantic_plan_program_type_field_binding(
                semantic, shape->product->field_begin + ordinal);
        if (!field || field->owner_program_row != shape->product->program_row ||
            field->field_program_row != member->program_row ||
            field->semantic_field_type != member->semantic_type ||
            field->declaration_ordinal != ordinal ||
            children[product_type->child_begin + ordinal] != member->semantic_type)
            return false;
    }
    uint32_t caller_count = 0;
    for (uint32_t row = 0; row < 3; row++) {
        const XrSemanticProgramFunctionBinding *binding =
            xr_semantic_plan_program_function_binding(semantic, row);
        const XrSemanticFunctionRecord *function =
            binding ? xr_semantic_plan_function(semantic, binding->semantic_function) : NULL;
        if (!binding || !function || binding->program_row >= 3 ||
            function->parameter_count != 0 ||
            function->return_type != shape->product->semantic_type ||
            function->return_parameter != -1 ||
            function->return_provenance != XR_SEM_RETURN_NONE || function->block_count != 1)
            return false;
        if (binding->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY && caller_count < 2) {
            shape->caller_bindings[caller_count] = binding;
            shape->callers[caller_count++] = function;
        } else if (binding->flags == 0 && !shape->callee) {
            shape->callee_binding = binding;
            shape->callee = function;
        } else {
            return false;
        }
    }
    if (caller_count != 2 || !shape->callee)
        return false;
    bool caller_seen[2] = {false, false};
    for (uint32_t row = 0; row < 2; row++) {
        const XrSemanticProgramCallBinding *binding =
            xr_semantic_plan_program_call_binding(semantic, row);
        const XrSemanticOperationRecord *operation =
            binding ? xr_semantic_plan_operation(semantic, binding->operation) : NULL;
        if (!binding || !operation || binding->program_row >= 2 ||
            binding->target_function != shape->callee_binding->semantic_function ||
            operation->opcode != XI_CALL || operation->operand_count != 1 ||
            operation->result_type != shape->product->semantic_type ||
            operation->result_value == XR_SEMANTIC_INDEX_NONE ||
            operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT)
            return false;
        uint32_t caller = operation->function == shape->caller_bindings[0]->semantic_function
                              ? 0u
                          : operation->function == shape->caller_bindings[1]->semantic_function
                              ? 1u
                              : UINT32_MAX;
        if (caller >= 2 || caller_seen[caller])
            return false;
        const XrSemanticCallTargetRecord *target = NULL;
        uint32_t target_index = XR_SEMANTIC_INDEX_NONE;
        for (uint32_t i = 0; i < 2; i++) {
            const XrSemanticCallTargetRecord *candidate =
                xr_semantic_plan_call_target(semantic, i);
            if (candidate && candidate->operation == binding->operation) {
                if (target)
                    return false;
                target = candidate;
                target_index = i;
            }
        }
        if (!target || target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
            target->function != shape->callee_binding->semantic_function ||
            target->dependency != XR_SEMANTIC_INDEX_NONE ||
            target->source_export != XR_SEMANTIC_INDEX_NONE)
            return false;
        caller_seen[caller] = true;
        shape->call_bindings[caller] = binding;
        shape->calls[caller] = operation;
        shape->target_indices[caller] = target_index;
    }
    return caller_seen[0] && caller_seen[1];
}

static bool verify_leaf_program_provenance(const XrSemanticPlan *semantic) {
    const XrSemanticProgramProvenance *row = xr_semantic_plan_program_provenance(semantic);
    XrStableId generation =
        row ? verify_program_generation(row->program_schema, row->program_fingerprint)
            : (XrStableId) {{0}};
    return row && semantic_is_leaf_program_family(semantic) &&
           row->schema == XR_SEMANTIC_PROGRAM_PROVENANCE_SCHEMA_VERSION &&
           row->program_schema == XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION &&
           row->type_count == 2 && row->type_field_count == 2 && row->function_count == 2 &&
           row->call_count == 1 && row->module_count == 1 && row->dependency_count == 0 &&
           row->program_module_row == 0 && row->program_dependency_binding_count == 0 &&
           row->reserved == 0 &&
           !fingerprint_is_zero(row->program_fingerprint) &&
           xr_stable_id_equal(row->generation_identity, generation) &&
           row->type_count == xr_semantic_plan_program_type_binding_count(semantic) &&
           row->type_field_count == xr_semantic_plan_program_type_field_binding_count(semantic) &&
           row->function_count == xr_semantic_plan_program_function_binding_count(semantic) &&
           row->call_count == xr_semantic_plan_program_call_binding_count(semantic);
}

static bool verify_leaf_program_types(const XrSemanticPlan *semantic,
                                      XrVerifyLeafProgramShape *shape) {
    const uint8_t required = XR_PROGRAM_SEMANTIC_TYPE_NONNULLABLE |
                             XR_PROGRAM_SEMANTIC_TYPE_NONGENERIC | XR_PROGRAM_SEMANTIC_TYPE_VALUE |
                             XR_PROGRAM_SEMANTIC_TYPE_POINTER_FREE;
    uint32_t aggregates = 0, scalars = 0;
    for (uint32_t i = 0; i < 2; i++) {
        const XrSemanticProgramTypeBinding *binding =
            xr_semantic_plan_program_type_binding(semantic, i);
        const XrSemanticTypeRecord *type =
            binding ? xr_semantic_plan_type(semantic, binding->semantic_type) : NULL;
        if (!binding || !type || binding->program_row >= 2 || binding->flags != required ||
            binding->reserved != 0 || stable_id_is_zero(binding->program_type) ||
            xr_semantic_plan_program_type_for_row(semantic, binding->program_row) != binding)
            return false;
        if (binding->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR) {
            if (++scalars != 1 || binding->exact_scalar != XR_EXACT_SCALAR_I64 ||
                binding->field_count != 0 || !stable_id_is_zero(binding->source_class_identity) ||
                type->kind != XR_KIND_INT || type->scalar_rep != XR_NATIVE_I64 ||
                type->builtin_type != XR_TID_NULL || type->source_class != XR_SEMANTIC_INDEX_NONE ||
                !stable_id_is_zero(type->source_class_identity) || type->child_count != 0 ||
                type->aggregate_extent != 0 || type->aggregate_align != 0 || type->flags != 0)
                return false;
            shape->scalar_binding = binding;
        } else if (binding->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE) {
            const XrSemanticSourceClassRecord *source =
                type->source_class < xr_semantic_plan_source_class_count(semantic)
                    ? xr_semantic_plan_source_class(semantic, type->source_class)
                    : NULL;
            if (++aggregates != 1 || binding->exact_scalar != XR_EXACT_SCALAR_NONE ||
                binding->field_begin != 0 || binding->field_count != 2 || !source ||
                type->kind != XR_KIND_INSTANCE || type->scalar_rep != XR_SCALAR_REP_NONE ||
                type->builtin_type != XR_TID_NULL || type->child_count != 2 ||
                type->aggregate_extent != 2 || type->aggregate_align != 0 ||
                type->flags != (XR_SEM_TYPE_VALUE | XR_SEM_TYPE_AGGREGATE_EXACT) ||
                !xr_stable_id_equal(binding->source_class_identity, type->source_class_identity) ||
                !xr_stable_id_equal(binding->source_class_identity, source->id) ||
                source->super_name || source->method_count != 0 || source->reserved != 0)
                return false;
            shape->aggregate_binding = binding;
        } else {
            return false;
        }
    }
    return aggregates == 1 && scalars == 1;
}

static bool verify_leaf_program_fields(const XrSemanticPlan *semantic,
                                       const XrVerifyLeafProgramShape *shape) {
    const XrSemanticTypeRecord *aggregate =
        xr_semantic_plan_type(semantic, shape->aggregate_binding->semantic_type);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    if (!aggregate || !children || aggregate->child_begin > child_count ||
        aggregate->child_count > child_count - aggregate->child_begin)
        return false;
    for (uint32_t ordinal = 0; ordinal < 2; ordinal++) {
        const XrSemanticProgramTypeFieldBinding *field =
            xr_semantic_plan_program_type_field_binding(semantic, ordinal);
        if (!field || field->owner_program_row != shape->aggregate_binding->program_row ||
            field->field_program_row != shape->scalar_binding->program_row ||
            field->semantic_field_type != shape->scalar_binding->semantic_type ||
            field->declaration_ordinal != ordinal ||
            !xr_stable_id_equal(field->program_owner_type,
                                shape->aggregate_binding->program_type) ||
            !xr_stable_id_equal(field->program_field_type, shape->scalar_binding->program_type) ||
            children[aggregate->child_begin + ordinal] != shape->scalar_binding->semantic_type)
            return false;
    }
    return true;
}

static bool verify_leaf_program_functions(const XrSemanticPlan *semantic,
                                          XrVerifyLeafProgramShape *shape) {
    bool seen[2] = {false, false};
    for (uint32_t i = 0; i < 2; i++) {
        const XrSemanticProgramFunctionBinding *binding =
            xr_semantic_plan_program_function_binding(semantic, i);
        const XrSemanticFunctionRecord *function =
            binding ? xr_semantic_plan_function(semantic, binding->semantic_function) : NULL;
        if (!binding || !function || binding->program_row >= 2 || seen[binding->program_row] ||
            stable_id_is_zero(binding->program_function) ||
            memcmp(binding->reserved, (uint8_t[3]) {0}, 3) != 0 ||
            function->return_type != shape->aggregate_binding->semantic_type ||
            function->return_parameter != -1 || function->return_provenance != XR_SEM_RETURN_NONE)
            return false;
        seen[binding->program_row] = true;
        if ((binding->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) != 0 && !shape->caller &&
            function->parameter_count == 0) {
            shape->caller_binding = binding;
            shape->caller = function;
        } else if (binding->flags == 0 && !shape->callee && function->parameter_count == 1) {
            shape->callee_binding = binding;
            shape->callee = function;
        } else {
            return false;
        }
    }
    if (!shape->caller || !shape->callee)
        return false;
    shape->parameter_index = shape->callee->parameter_begin;
    shape->parameter = xr_semantic_plan_parameter(semantic, shape->parameter_index);
    return shape->parameter &&
           shape->parameter->function == shape->callee_binding->semantic_function &&
           shape->parameter->type == shape->aggregate_binding->semantic_type &&
           shape->parameter->ordinal == 0 && shape->parameter->mode == XR_PARAM_READ &&
           shape->parameter->ownership == XI_OWN_NONE &&
           shape->parameter->transfer_mode == XR_TRANSFER_SHARE &&
           shape->parameter->flags == XR_SEM_PARAMETER_REQUIRED && shape->parameter->reserved == 0;
}

static bool verify_leaf_program_call(const XrSemanticPlan *semantic,
                                     XrVerifyLeafProgramShape *shape) {
    shape->call_binding = xr_semantic_plan_program_call_binding(semantic, 0);
    shape->operation = shape->call_binding
                           ? xr_semantic_plan_operation(semantic, shape->call_binding->operation)
                           : NULL;
    if (!shape->call_binding || !shape->operation || shape->call_binding->program_row != 0 ||
        shape->call_binding->reserved != 0 ||
        stable_id_is_zero(shape->call_binding->program_call) ||
        stable_id_is_zero(shape->call_binding->callsite) ||
        shape->call_binding->target_function != shape->callee_binding->semantic_function ||
        shape->operation->function != shape->caller_binding->semantic_function ||
        shape->operation->opcode != XI_CALL || shape->operation->operand_count != 2 ||
        shape->operation->result_type != shape->aggregate_binding->semantic_type ||
        shape->operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        shape->operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        shape->operation->result_alias_operand != -1 || shape->operation->return_parameter != -1 ||
        shape->operation->return_provenance != XR_SEM_RETURN_NONE ||
        shape->operation->return_complete != 0 ||
        !xr_stable_id_equal(shape->call_binding->caller_program_function,
                            shape->caller_binding->program_function) ||
        !xr_stable_id_equal(shape->call_binding->callee_program_function,
                            shape->callee_binding->program_function))
        return false;
    uint32_t count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &count);
    if (!operands || shape->operation->operand_begin > count ||
        shape->operation->operand_count > count - shape->operation->operand_begin)
        return false;
    const XrSemanticOperandRecord *callee = &operands[shape->operation->operand_begin];
    shape->argument_index = shape->operation->operand_begin + 1u;
    shape->argument = &operands[shape->argument_index];
    if (callee->role != XR_SEM_OPERAND_CALLEE || callee->parameter != -1 || callee->flags != 0 ||
        shape->argument->role != XR_SEM_OPERAND_ARGUMENT || shape->argument->parameter != 0 ||
        shape->argument->type != shape->parameter->type ||
        shape->argument->parameter_mode != XR_PARAM_READ ||
        shape->argument->ownership_action != XR_SEM_OPERAND_BORROW ||
        shape->argument->transfer_mode != XR_TRANSFER_SHARE ||
        shape->argument->access != XR_CALL_ARG_PLAIN ||
        shape->argument->origin != XI_PLACE_ORIGIN_NONE ||
        shape->argument->lifetime != XI_PLACE_LIFETIME_NONE ||
        shape->argument->escape != XI_PLACE_ESCAPE_NONE ||
        shape->argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        xr_semantic_plan_call_target_count(semantic) != 1)
        return false;
    shape->target_index = 0;
    shape->target = xr_semantic_plan_call_target(semantic, 0);
    return shape->target && shape->target->operation == shape->call_binding->operation &&
           shape->target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
           shape->target->function == shape->callee_binding->semantic_function &&
           shape->target->dependency == XR_SEMANTIC_INDEX_NONE &&
           shape->target->source_export == XR_SEMANTIC_INDEX_NONE &&
           stable_id_is_zero(shape->target->export_identity) &&
           stable_id_is_zero(shape->target->callee_function) &&
           shape->target->callable_type == XR_SEMANTIC_INDEX_NONE &&
           memcmp(shape->target->reserved, (uint8_t[3]) {0}, 3) == 0;
}

static bool verify_leaf_program_shape(const XrSemanticPlan *semantic,
                                      XrVerifyLeafProgramShape *shape) {
    if (shape)
        memset(shape, 0, sizeof(*shape));
    return semantic && shape && verify_leaf_program_provenance(semantic) &&
           verify_leaf_program_types(semantic, shape) &&
           verify_leaf_program_fields(semantic, shape) &&
           verify_leaf_program_functions(semantic, shape) &&
           verify_leaf_program_call(semantic, shape);
}

static bool profile_identity_is_consistent(const XrTargetMachineFacts *facts) {
    switch (facts->operating_system) {
        case XR_TARGET_OS_WINDOWS:
            return facts->environment == XR_TARGET_ENV_MSVC &&
                   ((facts->architecture == XR_TARGET_ARCH_X86_64 &&
                     facts->native_abi == XR_TARGET_ABI_WIN64_X86_64) ||
                    (facts->architecture == XR_TARGET_ARCH_AARCH64 &&
                     facts->native_abi == XR_TARGET_ABI_WIN64_AARCH64));
        case XR_TARGET_OS_LINUX:
            if (facts->environment != XR_TARGET_ENV_GNU && facts->environment != XR_TARGET_ENV_MUSL)
                return false;
            if (facts->architecture == XR_TARGET_ARCH_X86_64)
                return facts->native_abi == XR_TARGET_ABI_SYSV_X86_64;
            if (facts->architecture == XR_TARGET_ARCH_AARCH64)
                return facts->native_abi == XR_TARGET_ABI_AAPCS64;
            if (facts->architecture == XR_TARGET_ARCH_POWERPC64)
                return facts->native_abi == XR_TARGET_ABI_PPC64_ELFV2;
            if (facts->architecture == XR_TARGET_ARCH_LOONGARCH64)
                return facts->native_abi == XR_TARGET_ABI_LOONGARCH_LP64D;
            return false;
        case XR_TARGET_OS_MACOS:
            return facts->environment == XR_TARGET_ENV_DARWIN &&
                   ((facts->architecture == XR_TARGET_ARCH_X86_64 &&
                     facts->native_abi == XR_TARGET_ABI_DARWIN_X86_64) ||
                    (facts->architecture == XR_TARGET_ARCH_AARCH64 &&
                     facts->native_abi == XR_TARGET_ABI_DARWIN_AARCH64));
        case XR_TARGET_OS_WASI:
            return facts->architecture == XR_TARGET_ARCH_WASM32 &&
                   facts->environment == XR_TARGET_ENV_WASI &&
                   facts->native_abi == XR_TARGET_ABI_WASM;
        case XR_TARGET_OS_FREESTANDING:
            if (facts->environment != XR_TARGET_ENV_FREESTANDING)
                return false;
            switch (facts->architecture) {
                case XR_TARGET_ARCH_X86_64:
                    return facts->native_abi == XR_TARGET_ABI_SYSV_X86_64;
                case XR_TARGET_ARCH_AARCH64:
                    return facts->native_abi == XR_TARGET_ABI_AAPCS64;
                case XR_TARGET_ARCH_POWERPC64:
                    return facts->native_abi == XR_TARGET_ABI_PPC64_ELFV2;
                case XR_TARGET_ARCH_LOONGARCH64:
                    return facts->native_abi == XR_TARGET_ABI_LOONGARCH_LP64D;
                case XR_TARGET_ARCH_WASM32:
                    return facts->native_abi == XR_TARGET_ABI_WASM;
                default:
                    return false;
            }
        default:
            return false;
    }
}

static bool profile_machine_features_are_consistent(const XrTargetMachineFacts *facts) {
    uint64_t vectors = facts->vector_feature_mask;
    uint64_t allowed = 0;
    switch (facts->architecture) {
        case XR_TARGET_ARCH_X86_64:
            allowed = XR_TARGET_VECTOR_SSE2 | XR_TARGET_VECTOR_AVX2 | XR_TARGET_VECTOR_AVX512;
            break;
        case XR_TARGET_ARCH_AARCH64:
            allowed = XR_TARGET_VECTOR_NEON | XR_TARGET_VECTOR_SVE;
            break;
        case XR_TARGET_ARCH_POWERPC64:
            allowed = XR_TARGET_VECTOR_VSX;
            break;
        case XR_TARGET_ARCH_LOONGARCH64:
            allowed = XR_TARGET_VECTOR_LSX;
            break;
        case XR_TARGET_ARCH_WASM32:
            allowed = XR_TARGET_VECTOR_WASM128;
            break;
        default:
            return false;
    }
    uint32_t expected_pointer_size = facts->architecture == XR_TARGET_ARCH_WASM32 ? 4u : 8u;
    bool requires_little_endian = facts->architecture == XR_TARGET_ARCH_X86_64 ||
                                  facts->architecture == XR_TARGET_ARCH_LOONGARCH64 ||
                                  facts->architecture == XR_TARGET_ARCH_WASM32 ||
                                  facts->operating_system == XR_TARGET_OS_WINDOWS ||
                                  facts->operating_system == XR_TARGET_OS_MACOS;
    uint16_t exact_vector_bits = 0;
    switch (facts->architecture) {
        case XR_TARGET_ARCH_X86_64:
            if (vectors == XR_TARGET_VECTOR_SSE2)
                exact_vector_bits = 128;
            else if (vectors == (XR_TARGET_VECTOR_SSE2 | XR_TARGET_VECTOR_AVX2))
                exact_vector_bits = 256;
            else if (vectors ==
                     (XR_TARGET_VECTOR_SSE2 | XR_TARGET_VECTOR_AVX2 | XR_TARGET_VECTOR_AVX512))
                exact_vector_bits = 512;
            else if (vectors != 0)
                return false;
            break;
        case XR_TARGET_ARCH_AARCH64:
            if (vectors == XR_TARGET_VECTOR_NEON)
                exact_vector_bits = 128;
            else if (vectors == (XR_TARGET_VECTOR_NEON | XR_TARGET_VECTOR_SVE)) {
                if (facts->maximum_vector_bits < 128 || facts->maximum_vector_bits > 2048 ||
                    !is_power_of_two(facts->maximum_vector_bits))
                    return false;
                exact_vector_bits = facts->maximum_vector_bits;
            } else if (vectors != 0) {
                return false;
            }
            break;
        case XR_TARGET_ARCH_POWERPC64:
            if (vectors == XR_TARGET_VECTOR_VSX)
                exact_vector_bits = 128;
            else if (vectors != 0)
                return false;
            break;
        case XR_TARGET_ARCH_LOONGARCH64:
            if (vectors == XR_TARGET_VECTOR_LSX)
                exact_vector_bits = 128;
            else if (vectors != 0)
                return false;
            break;
        case XR_TARGET_ARCH_WASM32:
            if (vectors == XR_TARGET_VECTOR_WASM128)
                exact_vector_bits = 128;
            else if (vectors != 0)
                return false;
            break;
        default:
            return false;
    }
    return (vectors & ~allowed) == 0 && facts->maximum_vector_bits == exact_vector_bits &&
           facts->data_layout.pointer.size == expected_pointer_size &&
           (!requires_little_endian || facts->data_layout.endian == XR_TARGET_ENDIAN_LITTLE);
}

bool xr_target_profile_verify(const XrTargetProfile *profile, char *error, size_t error_size) {
    if (!profile || !profile->frozen)
        return report(error, error_size, "XR_TARGET_1000", "target profile is not frozen");
    const XrTargetProfileDraft *facts = &profile->facts;
    const XrTargetMachineFacts *machine = &facts->machine;
    if (facts->schema_version != XR_TARGET_PROFILE_SCHEMA_VERSION ||
        machine->architecture <= XR_TARGET_ARCH_NONE ||
        machine->architecture >= XR_TARGET_ARCH_COUNT ||
        machine->operating_system <= XR_TARGET_OS_NONE ||
        machine->operating_system >= XR_TARGET_OS_COUNT ||
        machine->environment <= XR_TARGET_ENV_NONE || machine->environment >= XR_TARGET_ENV_COUNT ||
        machine->native_abi <= XR_TARGET_ABI_NONE || machine->native_abi >= XR_TARGET_ABI_COUNT ||
        machine->runtime_profile < XR_TARGET_RUNTIME_PROFILE_HOSTED ||
        machine->runtime_profile > XR_TARGET_RUNTIME_PROFILE_FREESTANDING ||
        machine->reserved8[0] != 0 || machine->reserved8[1] != 0 || machine->reserved8[2] != 0 ||
        machine->reserved16 != 0)
        return report(error, error_size, "XR_TARGET_1000",
                      "target profile contains an unsupported exact identity");
    if (!xr_target_data_layout_validate(&machine->data_layout))
        return report(error, error_size, "XR_TARGET_1000", "target profile data layout is invalid");
    const uint64_t atomic_width_mask = XR_TARGET_ATOMIC_WIDTH_8 | XR_TARGET_ATOMIC_WIDTH_16 |
                                       XR_TARGET_ATOMIC_WIDTH_32 | XR_TARGET_ATOMIC_WIDTH_64 |
                                       XR_TARGET_ATOMIC_WIDTH_128;
    const uint64_t atomic_order_mask = XR_TARGET_ATOMIC_RELAXED | XR_TARGET_ATOMIC_ACQUIRE |
                                       XR_TARGET_ATOMIC_RELEASE | XR_TARGET_ATOMIC_ACQ_REL |
                                       XR_TARGET_ATOMIC_SEQ_CST;
    const uint64_t float_mask = XR_TARGET_FLOAT_IEEE754 | XR_TARGET_FLOAT_STRICT |
                                XR_TARGET_FLOAT_FAST | XR_TARGET_FLOAT_FMA;
    const uint64_t vector_mask = XR_TARGET_VECTOR_SSE2 | XR_TARGET_VECTOR_AVX2 |
                                 XR_TARGET_VECTOR_AVX512 | XR_TARGET_VECTOR_NEON |
                                 XR_TARGET_VECTOR_SVE | XR_TARGET_VECTOR_VSX |
                                 XR_TARGET_VECTOR_LSX | XR_TARGET_VECTOR_WASM128;
    const uint64_t provider_mask =
        XR_TARGET_PROVIDER_MASK_ALL | XR_TARGET_PROVIDER_DERIVED_CAPABILITY_MASK;
    if ((machine->atomic_width_mask & ~atomic_width_mask) != 0 ||
        (machine->atomic_order_mask & ~atomic_order_mask) != 0 ||
        (machine->float_feature_mask & ~float_mask) != 0 ||
        (machine->vector_feature_mask & ~vector_mask) != 0 ||
        (facts->provider_mask & ~provider_mask) != 0 ||
        (facts->provider_mask & XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_ALLOCATOR)) == 0 ||
        (facts->provider_mask & XR_TARGET_PROVIDER_MASK(XR_TARGET_PROVIDER_PANIC)) == 0 ||
        (machine->float_feature_mask & XR_TARGET_FLOAT_IEEE754) == 0 ||
        ((machine->float_feature_mask & XR_TARGET_FLOAT_STRICT) != 0 &&
         (machine->float_feature_mask & XR_TARGET_FLOAT_FAST) != 0) ||
        (machine->vector_feature_mask == 0 && machine->maximum_vector_bits != 0) ||
        (machine->vector_feature_mask != 0 &&
         (!is_power_of_two(machine->maximum_vector_bits) || machine->maximum_vector_bits < 128u ||
          machine->maximum_vector_bits > 2048u)) ||
        fingerprint_is_zero(facts->provider_set_fingerprint) ||
        fingerprint_is_zero(facts->object_header_fingerprint) ||
        fingerprint_is_zero(facts->runtime_abi_fingerprint) ||
        xr_runtime_string_literal_materialization_contract_verify(&facts->string_literal) !=
            XR_RUNTIME_ABI_OK)
        return report(error, error_size, "XR_TARGET_1000",
                      "target profile runtime facts are incomplete");
    if (!profile_identity_is_consistent(machine) ||
        !profile_machine_features_are_consistent(machine))
        return report(error, error_size, "XR_TARGET_1000",
                      "target machine identity or feature facts are inconsistent");
    XrFingerprint actual;
    xr_target_profile_compute_fingerprint(facts, &actual);
    if (!xr_fingerprint_equal(actual, profile->fingerprint))
        return report(error, error_size, "XR_TARGET_1000",
                      "target profile fingerprint changed after freeze");
    return true;
}

static bool verify_resource_budgets(const XrTargetPlan *plan, char *error, size_t error_size) {
    if (plan->semantic_dependency_count > XR_TARGET_MAX_SEMANTIC_DEPENDENCIES ||
        plan->semantic_module_count > XR_TARGET_MAX_PROGRAM_MODULES ||
        plan->program_graphs_count > 1u ||
        plan->module_partitions_count > XR_TARGET_MAX_PROGRAM_MODULES ||
        plan->machine_reps_count > 256u || plan->extents_count > 1000000u ||
        plan->value_reps_count > 40000000u || plan->layouts_count > 1000000u ||
        plan->fields_count > 16000000u || plan->storage_count > 4000000u ||
        plan->allocations_count > 10000000u || plan->extent_operands_count > 40000000u ||
        plan->functions_count > 100000u || plan->slots_count > 16000000u ||
        plan->i64_overflow_predicates_count > XR_PROGRAM_SEMANTIC_CLOSURE_MAX_CALLS ||
        plan->instructions_count > 40000000u || plan->calls_count > 10000000u ||
        plan->call_arguments_count > 40000000u || plan->root_maps_count > 10000000u ||
        plan->root_slots_count > 40000000u || plan->cleanups_count > 40000000u ||
        plan->adapters_count > 1000000u || plan->capabilities_count > 65536u ||
        plan->coroutines_count > 10000000u || plan->entry_expectations_count > 10000000u ||
        plan->debug_facts_count > 40000000u)
        return report(error, error_size, "XR_EXEC_5003", "TargetPlan exceeds hard budgets");
    size_t total = sizeof(*plan);
    if (plan->semantic_dependency_count >
        (SIZE_MAX - total) / sizeof(*plan->semantic_dependencies))
        return report(error, error_size, "XR_EXEC_5003", "TargetPlan byte budget overflow");
    total += (size_t) plan->semantic_dependency_count * sizeof(*plan->semantic_dependencies);
    if (plan->semantic_module_count >
        (SIZE_MAX - total) / sizeof(*plan->semantic_modules))
        return report(error, error_size, "XR_EXEC_5003", "TargetPlan byte budget overflow");
    total += (size_t) plan->semantic_module_count * sizeof(*plan->semantic_modules);
#define XR_ADD_TARGET_BYTES(name)                                                                  \
    do {                                                                                           \
        if (plan->name##_count > (SIZE_MAX - total) / sizeof(*plan->name))                         \
            return report(error, error_size, "XR_EXEC_5003", "TargetPlan byte budget overflow");   \
        total += (size_t) plan->name##_count * sizeof(*plan->name);                                \
    } while (0)
    XR_ADD_TARGET_BYTES(machine_reps);
    XR_ADD_TARGET_BYTES(value_reps);
    XR_ADD_TARGET_BYTES(extents);
    XR_ADD_TARGET_BYTES(layouts);
    XR_ADD_TARGET_BYTES(fields);
    XR_ADD_TARGET_BYTES(storage);
    XR_ADD_TARGET_BYTES(allocations);
    XR_ADD_TARGET_BYTES(extent_operands);
    XR_ADD_TARGET_BYTES(functions);
    XR_ADD_TARGET_BYTES(slots);
    XR_ADD_TARGET_BYTES(i64_overflow_predicates);
    XR_ADD_TARGET_BYTES(instructions);
    XR_ADD_TARGET_BYTES(calls);
    XR_ADD_TARGET_BYTES(call_arguments);
    XR_ADD_TARGET_BYTES(root_maps);
    XR_ADD_TARGET_BYTES(root_slots);
    XR_ADD_TARGET_BYTES(cleanups);
    XR_ADD_TARGET_BYTES(adapters);
    XR_ADD_TARGET_BYTES(capabilities);
    XR_ADD_TARGET_BYTES(coroutines);
    XR_ADD_TARGET_BYTES(entry_expectations);
    XR_ADD_TARGET_BYTES(debug_facts);
    XR_ADD_TARGET_BYTES(program_graphs);
    XR_ADD_TARGET_BYTES(module_partitions);
#undef XR_ADD_TARGET_BYTES
    if (total > (size_t) UINT32_MAX)
        return report(error, error_size, "XR_EXEC_5003", "TargetPlan exceeds total byte budget");
#define XR_REQUIRE_TARGET_TABLE(name)                                                              \
    if (plan->name##_count && !plan->name)                                                         \
    return report(error, error_size, "XR_EXEC_5003", "TargetPlan table storage is missing")
    XR_REQUIRE_TARGET_TABLE(machine_reps);
    XR_REQUIRE_TARGET_TABLE(value_reps);
    XR_REQUIRE_TARGET_TABLE(extents);
    XR_REQUIRE_TARGET_TABLE(layouts);
    XR_REQUIRE_TARGET_TABLE(fields);
    XR_REQUIRE_TARGET_TABLE(storage);
    XR_REQUIRE_TARGET_TABLE(allocations);
    XR_REQUIRE_TARGET_TABLE(extent_operands);
    XR_REQUIRE_TARGET_TABLE(functions);
    XR_REQUIRE_TARGET_TABLE(slots);
    XR_REQUIRE_TARGET_TABLE(i64_overflow_predicates);
    XR_REQUIRE_TARGET_TABLE(instructions);
    XR_REQUIRE_TARGET_TABLE(calls);
    XR_REQUIRE_TARGET_TABLE(call_arguments);
    XR_REQUIRE_TARGET_TABLE(root_maps);
    XR_REQUIRE_TARGET_TABLE(root_slots);
    XR_REQUIRE_TARGET_TABLE(cleanups);
    XR_REQUIRE_TARGET_TABLE(adapters);
    XR_REQUIRE_TARGET_TABLE(capabilities);
    XR_REQUIRE_TARGET_TABLE(coroutines);
    XR_REQUIRE_TARGET_TABLE(entry_expectations);
    XR_REQUIRE_TARGET_TABLE(debug_facts);
    XR_REQUIRE_TARGET_TABLE(program_graphs);
    XR_REQUIRE_TARGET_TABLE(module_partitions);
#undef XR_REQUIRE_TARGET_TABLE
    return true;
}

static bool conversion_mask_in_range(const XrTargetMachineRepRecord *rep, uint32_t count) {
    for (uint32_t word = 0; word < 4; word++) {
        uint32_t begin = word * 64u;
        uint64_t allowed = UINT64_MAX;
        if (begin >= count)
            allowed = 0;
        else if (count - begin < 64u)
            allowed = (UINT64_C(1) << (count - begin)) - 1u;
        if ((rep->legal_conversion_mask[word] & ~allowed) != 0)
            return false;
    }
    return true;
}

static bool machine_reps_are_storage_compatible(const XrTargetMachineRepRecord *from,
                                                const XrTargetMachineRepRecord *to) {
    return from->kind == to->kind && from->signedness == to->signedness &&
           from->root_kind == to->root_kind && from->ownership == to->ownership &&
           from->null_encoding == to->null_encoding && from->register_bits == to->register_bits &&
           from->memory_size == to->memory_size && from->memory_align == to->memory_align &&
           from->detail == to->detail && from->lane_count == to->lane_count;
}

static bool machine_reps_have_same_call_abi(const XrTargetMachineRepRecord *caller,
                                            const XrTargetMachineRepRecord *callee) {
    return caller && callee && caller->kind == callee->kind &&
           caller->signedness == callee->signedness && caller->root_kind == callee->root_kind &&
           caller->null_encoding == callee->null_encoding &&
           caller->register_bits == callee->register_bits &&
           caller->memory_size == callee->memory_size &&
           caller->memory_align == callee->memory_align && caller->detail == callee->detail &&
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
 * the same tagged carrier, so they are checked by this one judgement rather
 * than by two copies of the same rep agreement. */
static bool verify_tagged_container_value_boundary(const XrTargetPlan *plan,
                                                   const XrTargetValueRepRecord *caller,
                                                   const XrTargetValueRepRecord *callee,
                                                   uint8_t callee_ownership) {
    return plan && caller && callee && caller->register_rep < plan->machine_reps_count &&
           caller->memory_rep < plan->machine_reps_count &&
           callee->register_rep < plan->machine_reps_count &&
           callee->memory_rep < plan->machine_reps_count &&
           machine_reps_have_same_call_abi(&plan->machine_reps[caller->register_rep],
                                           &plan->machine_reps[callee->register_rep]) &&
           machine_reps_have_same_call_abi(&plan->machine_reps[caller->memory_rep],
                                           &plan->machine_reps[callee->memory_rep]) &&
           plan->machine_reps[caller->register_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
           plan->machine_reps[caller->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
           plan->machine_reps[callee->register_rep].ownership == callee_ownership &&
           plan->machine_reps[callee->memory_rep].ownership == callee_ownership;
}

static bool conversion_mask_is_independently_derived(const XrTargetPlan *plan, uint32_t index) {
    const XrTargetMachineRepRecord *from = &plan->machine_reps[index];
    for (uint32_t to = 0; to < plan->machine_reps_count; to++) {
        bool encoded = (from->legal_conversion_mask[to / 64u] & (UINT64_C(1) << (to % 64u))) != 0;
        bool expected =
            to != index && machine_reps_are_storage_compatible(from, &plan->machine_reps[to]);
        if (encoded != expected)
            return false;
    }
    return true;
}

static bool rep_matches_layout(const XrTargetMachineRepRecord *rep,
                               const XrTargetTypeLayout *layout, uint8_t signedness) {
    return rep->register_bits == layout->size * 8u && rep->memory_size == layout->size &&
           rep->memory_align == layout->align && rep->signedness == signedness;
}

static bool scalar_rep_matches_profile(const XrTargetMachineRepRecord *rep,
                                       const XrTargetMachineFacts *profile) {
    switch (rep->kind) {
        case XR_MACHINE_REP_I1:
            return rep->register_bits == 1 &&
                   rep->memory_size == profile->data_layout.boolean.size &&
                   rep->memory_align == profile->data_layout.boolean.align &&
                   rep->signedness == XR_TARGET_SIGN_NONE;
        case XR_MACHINE_REP_I8:
            return rep_matches_layout(rep, &profile->data_layout.i8, XR_TARGET_SIGN_SIGNED);
        case XR_MACHINE_REP_U8:
            return rep_matches_layout(rep, &profile->data_layout.u8, XR_TARGET_SIGN_UNSIGNED);
        case XR_MACHINE_REP_I16:
            return rep_matches_layout(rep, &profile->data_layout.i16, XR_TARGET_SIGN_SIGNED);
        case XR_MACHINE_REP_U16:
            return rep_matches_layout(rep, &profile->data_layout.u16, XR_TARGET_SIGN_UNSIGNED);
        case XR_MACHINE_REP_I32:
            return rep_matches_layout(rep, &profile->data_layout.i32, XR_TARGET_SIGN_SIGNED);
        case XR_MACHINE_REP_U32:
        case XR_MACHINE_REP_RUNE:
            return rep_matches_layout(rep, &profile->data_layout.u32, XR_TARGET_SIGN_UNSIGNED);
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_ENUM_ORDINAL:
            return rep_matches_layout(rep, &profile->data_layout.i64, XR_TARGET_SIGN_SIGNED);
        case XR_MACHINE_REP_U64:
            return rep_matches_layout(rep, &profile->data_layout.u64, XR_TARGET_SIGN_UNSIGNED);
        case XR_MACHINE_REP_ISIZE:
            return rep_matches_layout(rep, &profile->data_layout.isize, XR_TARGET_SIGN_SIGNED);
        case XR_MACHINE_REP_USIZE:
            return rep_matches_layout(rep, &profile->data_layout.usize, XR_TARGET_SIGN_UNSIGNED);
        case XR_MACHINE_REP_F32:
            return rep_matches_layout(rep, &profile->data_layout.f32, XR_TARGET_SIGN_NONE);
        case XR_MACHINE_REP_F64:
            return rep_matches_layout(rep, &profile->data_layout.f64, XR_TARGET_SIGN_NONE);
        default:
            return true;
    }
}

static bool semantic_direct_local_tagged_ref_parameter_is_exact_verify(
    const XrSemanticPlan *semantic, const XrSemanticParameterRecord *parameter, uint8_t *storage);
static bool semantic_stringbuilder_type_is_exact(const XrSemanticTypeRecord *type);

/* A borrowed RAW_PTR is not a general pointer lifecycle. It exists only as
 * the physical callee-side carrier of an exact direct-local tagged ref
 * parameter, and every value binding that names the representation must
 * independently identify such a parameter. */
static bool borrowed_raw_pointer_rep_is_exact(const XrTargetPlan *plan,
                                              const XrTargetMachineRepRecord *rep) {
    bool found = false;
    for (uint32_t i = 0; plan && i < plan->value_reps_count; i++) {
        const XrTargetValueRepRecord *binding = &plan->value_reps[i];
        if (binding->register_rep != rep->id && binding->memory_rep != rep->id)
            continue;
        const XrSemanticParameterRecord *parameter = NULL;
        for (uint32_t p = 0; p < (uint32_t) xr_semantic_plan_parameter_count(plan->semantic_plan);
             p++) {
            const XrSemanticParameterRecord *candidate =
                xr_semantic_plan_parameter(plan->semantic_plan, p);
            if (!candidate || candidate->value != binding->semantic_value)
                continue;
            if (parameter)
                return false;
            parameter = candidate;
        }
        uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
        if (!semantic_direct_local_tagged_ref_parameter_is_exact_verify(plan->semantic_plan,
                                                                        parameter, &storage))
            return false;
        found = true;
    }
    return found;
}

static bool rep_kind_contract_is_exact(const XrTargetPlan *plan,
                                       const XrTargetMachineRepRecord *rep) {
    const XrTargetMachineFacts *profile = xr_target_profile_machine_facts(plan->profile);
    bool scalar = rep->kind >= XR_MACHINE_REP_I1 && rep->kind <= XR_MACHINE_REP_RUNE;
    if (scalar)
        return rep->detail == 0 && rep->lane_count == 0 && rep->root_kind == XR_TARGET_ROOT_NONE &&
               rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
               rep->null_encoding == XR_TARGET_NULL_NOT_NULLABLE;
    switch (rep->kind) {
        case XR_MACHINE_REP_ENUM_ORDINAL: {
            const XrSemanticTypeRecord *type =
                xr_semantic_plan_type(plan->semantic_plan, rep->detail);
            return type && type->kind == XR_KIND_ENUM && type->source_enum_key &&
                   type->enum_layout_id != 0 && type->enum_member_count != 0 &&
                   type->enum_flags == (XR_SEM_ENUM_DECLARATION_EXACT | XR_SEM_ENUM_UNIT) &&
                   type->reserved_enum == 0 && type->builtin_type == XR_TID_NULL &&
                   type->child_count == 0 && type->aggregate_extent == 0 &&
                   type->aggregate_align == 0 && type->scalar_rep == XR_SCALAR_REP_NONE &&
                   (type->flags & XR_SEM_TYPE_NULLABLE) == 0 && rep->lane_count == 0 &&
                   rep->root_kind == XR_TARGET_ROOT_NONE &&
                   rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
                   rep->null_encoding == XR_TARGET_NULL_NOT_NULLABLE;
        }
        case XR_MACHINE_REP_OBJECT_REF:
            return rep->root_kind == XR_TARGET_ROOT_OBJECT && rep->lane_count == 0 &&
                   rep->signedness == XR_TARGET_SIGN_NONE;
        case XR_MACHINE_REP_RAW_PTR:
            return rep->detail == 0 && rep->lane_count == 0 &&
                   rep->root_kind == XR_TARGET_ROOT_NONE &&
                   (rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL ||
                    (rep->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
                     borrowed_raw_pointer_rep_is_exact(plan, rep))) &&
                   rep->null_encoding == XR_TARGET_NULL_ZERO &&
                   rep->signedness == XR_TARGET_SIGN_NONE;
        case XR_MACHINE_REP_CODE_REF:
            return rep->detail == 0 && rep->lane_count == 0 &&
                   rep->root_kind == XR_TARGET_ROOT_NONE && rep->signedness == XR_TARGET_SIGN_NONE;
        case XR_MACHINE_REP_DYN_VALUE:
            return rep->detail == 0 && rep->lane_count == 0 &&
                   rep->root_kind == XR_TARGET_ROOT_DYNAMIC &&
                   (rep->ownership == XR_TARGET_OWNERSHIP_OWNED ||
                    rep->ownership == XR_TARGET_OWNERSHIP_BORROWED) &&
                   rep->null_encoding == XR_TARGET_NULL_TAGGED &&
                   rep->signedness == XR_TARGET_SIGN_NONE;
        case XR_MACHINE_REP_AGGREGATE:
        case XR_MACHINE_REP_VIEW: {
            int layout_index = rep->kind == XR_MACHINE_REP_VIEW
                                   ? target_plan_layout_for_type(plan, rep->detail)
                                   : (rep->detail < plan->layouts_count ? (int) rep->detail : -1);
            if (layout_index < 0 || rep->lane_count != 0 ||
                rep->signedness != XR_TARGET_SIGN_NONE ||
                (rep->kind == XR_MACHINE_REP_AGGREGATE &&
                 (rep->root_kind != XR_TARGET_ROOT_NONE ||
                  rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
                  rep->null_encoding != XR_TARGET_NULL_NOT_NULLABLE)))
                return false;
            const XrTargetLayoutRecord *layout = &plan->layouts[layout_index];
            uint8_t expected_kind = rep->kind == XR_MACHINE_REP_VIEW ? XR_TARGET_LAYOUT_VIEW
                                                                     : XR_TARGET_LAYOUT_AGGREGATE;
            return layout->kind == expected_kind &&
                   (rep->kind != XR_MACHINE_REP_VIEW ||
                    (rep->detail == layout->semantic_type &&
                     rep->root_kind == XR_TARGET_ROOT_VIEW_OWNER &&
                     rep->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
                     rep->null_encoding == XR_TARGET_NULL_NOT_NULLABLE)) &&
                   rep->memory_size == layout->fixed_prefix_size &&
                   rep->memory_align == layout->align &&
                   rep->register_bits == layout->fixed_prefix_size * 8u;
        }
        case XR_MACHINE_REP_VECTOR: {
            if (rep->detail >= plan->machine_reps_count || rep->detail == rep->id ||
                rep->lane_count < 2 || rep->signedness != XR_TARGET_SIGN_NONE ||
                rep->root_kind != XR_TARGET_ROOT_NONE ||
                rep->ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
                rep->null_encoding != XR_TARGET_NULL_NOT_NULLABLE)
                return false;
            const XrTargetMachineRepRecord *lane = &plan->machine_reps[rep->detail];
            uint64_t size = (uint64_t) lane->memory_size * rep->lane_count;
            bool scalar_lane = lane->kind >= XR_MACHINE_REP_I8 && lane->kind <= XR_MACHINE_REP_F64;
            return scalar_lane && lane->root_kind == XR_TARGET_ROOT_NONE && size <= UINT32_MAX &&
                   size <= UINT32_MAX / 8u && rep->memory_size == size &&
                   rep->register_bits == size * 8u &&
                   rep->register_bits <= profile->maximum_vector_bits &&
                   rep->memory_align == size && rep->memory_align >= lane->memory_align;
        }
        default:
            return false;
    }
}

static bool verify_machine_reps(const XrTargetPlan *plan, char *error, size_t error_size) {
    const XrTargetMachineFacts *profile = xr_target_profile_machine_facts(plan->profile);
    if (!plan->machine_reps_count)
        return report(error, error_size, "XR_TARGET_1001", "machine representation table is empty");
    for (uint32_t i = 0; i < plan->machine_reps_count; i++) {
        const XrTargetMachineRepRecord *rep = &plan->machine_reps[i];
        if (rep->id != i || rep->kind >= XR_MACHINE_REP_COUNT ||
            rep->signedness > XR_TARGET_SIGN_UNSIGNED ||
            rep->root_kind > XR_TARGET_ROOT_VIEW_OWNER ||
            rep->ownership > XR_TARGET_OWNERSHIP_SHARED ||
            rep->null_encoding > XR_TARGET_NULL_TAGGED || rep->reserved != 0 ||
            !conversion_mask_in_range(rep, plan->machine_reps_count))
            return report(error, error_size, "XR_TARGET_1001",
                          "machine representation identity or enum is invalid");
        if (rep->kind == XR_MACHINE_REP_VOID) {
            if (rep->register_bits || rep->memory_size || rep->memory_align || rep->signedness ||
                rep->root_kind || rep->ownership || rep->null_encoding || rep->detail ||
                rep->lane_count || rep->legal_conversion_mask[0] || rep->legal_conversion_mask[1] ||
                rep->legal_conversion_mask[2] || rep->legal_conversion_mask[3])
                return report(error, error_size, "XR_TARGET_1001",
                              "void representation carries storage facts");
            continue;
        }
        if (!rep->register_bits || !rep->memory_size || !is_power_of_two(rep->memory_align) ||
            rep->memory_align > rep->memory_size)
            return report(error, error_size, "XR_TARGET_1001",
                          "machine representation width or alignment is invalid");
        if (!scalar_rep_matches_profile(rep, profile))
            return report(error, error_size, "XR_TARGET_1001",
                          "scalar representation disagrees with the target profile");
        if (!rep_kind_contract_is_exact(plan, rep))
            return report(error, error_size, "XR_TARGET_1001",
                          "representation kind carries an invalid detail or lifecycle contract");
        if (rep->kind == XR_MACHINE_REP_OBJECT_REF || rep->kind == XR_MACHINE_REP_RAW_PTR ||
            rep->kind == XR_MACHINE_REP_CODE_REF) {
            if (!rep_matches_layout(rep, &profile->data_layout.pointer, XR_TARGET_SIGN_NONE))
                return report(error, error_size, "XR_TARGET_1001",
                              "pointer representation disagrees with the target profile");
        } else if (rep->kind == XR_MACHINE_REP_DYN_VALUE &&
                   !rep_matches_layout(rep, &profile->data_layout.xr_value, XR_TARGET_SIGN_NONE)) {
            return report(error, error_size, "XR_TARGET_1001",
                          "dynamic representation disagrees with the target profile");
        }
        if (rep->kind == XR_MACHINE_REP_VECTOR) {
            if (!rep->lane_count || rep->detail >= plan->machine_reps_count || rep->detail == i)
                return report(error, error_size, "XR_TARGET_1001",
                              "vector representation has no valid lane representation");
        } else if (rep->lane_count) {
            return report(error, error_size, "XR_TARGET_1001",
                          "non-vector representation carries a lane count");
        }
        if (rep->kind == XR_MACHINE_REP_OBJECT_REF &&
            (rep->detail >= plan->layouts_count ||
             plan->layouts[rep->detail].kind != XR_TARGET_LAYOUT_OBJECT))
            return report(error, error_size, "XR_TARGET_1001",
                          "representation references an invalid layout");
    }
    for (uint32_t i = 0; i < plan->machine_reps_count; i++)
        if (!conversion_mask_is_independently_derived(plan, i))
            return report(error, error_size, "XR_TARGET_1001",
                          "conversion mask disagrees with the independently derived legal domain");
    return true;
}

static bool machine_rep_allows_conversion(const XrTargetPlan *plan, uint16_t from, uint16_t to) {
    if (from >= plan->machine_reps_count || to >= plan->machine_reps_count)
        return false;
    return machine_reps_are_storage_compatible(&plan->machine_reps[from], &plan->machine_reps[to]);
}

/* Independent reconstruction of the raw-pointer SemanticPlan identity.  Keep
 * this separate from the builder so a malformed key or lifecycle row cannot
 * pass because construction and verification shared one classifier. */
static bool verifier_raw_pointer_type_is_exact(const XrSemanticTypeRecord *type) {
    unsigned kind = 0, semantic_type = 0, builtin_type = 0;
    unsigned nullable = 0, is_const = 0, is_value = 0, is_literal = 0;
    unsigned cycle_candidate = 0, pointer_mutable = 0, scalar_rep = 0;
    size_t alias_length = 0;
    int consumed = 0;
    XrStableId zero = {{0}};
    return type && type->canonical_key &&
           sscanf(type->canonical_key, "type-v3:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%zu:%n", &kind,
                  &semantic_type, &builtin_type, &nullable, &is_const, &is_value, &is_literal,
                  &cycle_candidate, &pointer_mutable, &scalar_rep, &alias_length,
                  &consumed) == 11 &&
           consumed > 0 && (size_t) consumed == strlen(type->canonical_key) &&
           kind == XR_KIND_POINTER && semantic_type == 0 && builtin_type == XR_TID_NULL &&
           nullable == 0 && is_const == 0 && is_value == 0 && is_literal == 0 &&
           cycle_candidate == 0 && pointer_mutable <= 1 && scalar_rep == XR_SCALAR_REP_NONE &&
           alias_length == 0 && type->kind == XR_KIND_POINTER &&
           type->builtin_type == XR_TID_NULL && type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 && type->enum_layout_id == 0 &&
           type->enum_member_count == 0 && type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags == 0 && type->enum_flags == 0 && type->reserved_enum == 0 &&
           !type->source_enum_key && xr_stable_id_equal(type->source_enum_identity, zero);
}

/* The mapping is shared with the builder so the two cannot name different
 * representations for one type. The raw-pointer test stays local and derives
 * its answer by re-parsing the frozen canonical key, which is an independent
 * route to the same fact and the reason this verifier catches a record whose
 * fields and key disagree. */
static int semantic_type_expected_rep(const XrSemanticTypeRecord *type, uint16_t *out_kind) {
    return (int) xr_target_scalar_rep_for_type(
        type, type && verifier_raw_pointer_type_is_exact(type), out_kind);
}

/* Rebuilt here from the frozen semantic rows, not read back from the builder.
 * `Array<T>` is a compiler-owned container that no declaration produces, so the
 * whole shape is the outer row: one element child, no aggregate or scalar
 * geometry, and the reference-capable ownership root a container carries. What
 * the element is stays a separate question, asked by whichever carrier needs
 * the answer. */

static bool semantic_direct_local_array_type_is_exact_verify(const XrSemanticPlan *plan,
                                                             uint32_t type_index,
                                                             bool indexes_elements,
                                                             uint8_t *storage);
static const XrSemanticFunctionRecord *
semantic_direct_local_callee_for_operation(const XrSemanticPlan *semantic,
                                           uint32_t operation_index);

static bool semantic_array_allocation_is_exact(const XrSemanticPlan *semantic,
                                               const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands =
        semantic ? xr_semantic_plan_operands(semantic, &operand_count) : NULL;
    const uint32_t *children =
        semantic ? xr_semantic_plan_type_children(semantic, &child_count) : NULL;
    if (!semantic || !operation || !operands || !children || operation->opcode != XI_ARRAY_NEW ||
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
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, operation->result_type);
    if (!xr_semantic_array_type_row_is_exact(type) || type->child_begin >= child_count)
        return false;
    const XrSemanticTypeRecord *element =
        xr_semantic_plan_type(semantic, children[type->child_begin]);
    const XrSemanticOperandRecord *capacity = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *capacity_type = xr_semantic_plan_type(semantic, capacity->type);
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(semantic, operation->function);
    uint16_t capacity_kind = XR_MACHINE_REP_COUNT;
    uint8_t semantic_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t type_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    bool source_class_element =
        xr_semantic_class_instance_type_source_class(semantic, element) != XR_SEMANTIC_INDEX_NONE;
    bool element_storage_exact = false;
    bool semantic_storage_exact = false;
    if (element && (element->flags & XR_SEM_TYPE_REFERENCE_CAPABLE) == 0) {
        element_storage_exact = semantic_direct_local_array_type_is_exact_verify(
            semantic, operation->result_type, true, &type_storage);
        semantic_storage_exact = xr_target_array_storage_from_semantic(
                                     operation->array_element_storage, &semantic_storage) &&
                                 semantic_storage == type_storage;
    } else if (source_class_element) {
        /* Re-derive the SemanticPlan-to-TargetPlan vocabulary bridge rather
         * than
         * trusting the builder's target storage word. */
        type_storage = XR_TARGET_ARRAY_STORAGE_TAGGED;
        element_storage_exact = true;
        semantic_storage_exact = operation->array_element_storage == XR_ELEM_ANY;
    }
    return element && capacity_type && function && element_storage_exact &&
           semantic_storage_exact &&
           semantic_type_expected_rep(capacity_type, &capacity_kind) == 1 &&
           capacity_kind == XR_MACHINE_REP_I64 && capacity->role == XR_SEM_OPERAND_VALUE &&
           capacity->parameter == -1 && capacity->flags == 0 &&
           capacity->ownership_action == XR_SEM_OPERAND_CONSUME &&
           operation->result_value >= function->value_begin &&
           operation->result_value < function->value_begin + function->value_count;
}

/* Independently rebuilt: an exact tagged value at a direct-local boundary.
 * Array carriers additionally state the element storage the boundary may
 * reach; compiler/source-owned class instances admitted only at ref boundaries
 * have no element storage. `indexes_elements` and `admits_instance` keep those
 * two questions explicit instead of inferring them from a slot role. */
static bool semantic_direct_local_tagged_boundary_type_is_exact_verify(
    const XrSemanticPlan *semantic, uint32_t type_index, bool indexes_elements,
    bool admits_instance, uint8_t *storage) {
    uint32_t child_count = 0;
    const uint32_t *children =
        semantic ? xr_semantic_plan_type_children(semantic, &child_count) : NULL;
    const XrSemanticTypeRecord *type =
        semantic ? xr_semantic_plan_type(semantic, type_index) : NULL;
    uint8_t element = XR_TARGET_ARRAY_STORAGE_NONE;
    /* Mirrors the builder, including which call sites opt in: a class instance
     * is admitted
     * at a ref boundary and nowhere else.  The two sides must admit
     * the same set, or the
     * plan the builder froze is one this pass refuses. */
    if (admits_instance && semantic &&
        (xr_semantic_class_instance_type_source_class(semantic, type) != XR_SEMANTIC_INDEX_NONE ||
         semantic_stringbuilder_type_is_exact(type))) {
        if (storage)
            *storage = XR_TARGET_ARRAY_STORAGE_NONE;
        return true;
    }
    if (!semantic || !children || !xr_semantic_array_type_row_is_exact(type) ||
        type->child_begin >= child_count)
        return false;
    const XrSemanticTypeRecord *element_type =
        xr_semantic_plan_type(semantic, children[type->child_begin]);
    if (!xr_target_array_storage_from_type(element_type, &element)) {
        bool source_class_element = xr_semantic_class_instance_type_source_class(
                                        semantic, element_type) != XR_SEMANTIC_INDEX_NONE;
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

static bool semantic_direct_local_array_type_is_exact_verify(const XrSemanticPlan *semantic,
                                                             uint32_t type_index,
                                                             bool indexes_elements,
                                                             uint8_t *storage) {
    return semantic_direct_local_tagged_boundary_type_is_exact_verify(
        semantic, type_index, indexes_elements, false, storage);
}

/* A borrowed `Array<T>` parameter in either passing mode. Both modes borrow the
 * caller's allocation and release nothing; they differ only in whether the
 * callee receives a pointer to the caller's cell (ref, so it may rebind) or the
 * tagged value itself (by value). One judgement with the mode as its parameter,
 * so the two cannot drift apart. */
static bool
semantic_direct_local_array_parameter_is_exact_verify(const XrSemanticPlan *semantic,
                                                      const XrSemanticParameterRecord *parameter,
                                                      uint8_t mode, uint8_t *storage) {
    if (!semantic || !parameter ||
        parameter->function >= xr_semantic_plan_function_count(semantic) ||
        parameter->value == XR_SEMANTIC_INDEX_NONE || parameter->mode != mode ||
        parameter->ownership != XI_OWN_BORROWED || parameter->transfer_mode != XR_TRANSFER_SHARE ||
        (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) != 0 || parameter->reserved != 0)
        return false;
    return semantic_direct_local_array_type_is_exact_verify(semantic, parameter->type,
                                                            mode == XR_PARAM_REF, storage);
}

static bool semantic_direct_local_tagged_ref_parameter_is_exact_verify(
    const XrSemanticPlan *semantic, const XrSemanticParameterRecord *parameter, uint8_t *storage) {
    return parameter && parameter->function < xr_semantic_plan_function_count(semantic) &&
           parameter->value != XR_SEMANTIC_INDEX_NONE && parameter->mode == XR_PARAM_REF &&
           parameter->ownership == XI_OWN_BORROWED &&
           parameter->transfer_mode == XR_TRANSFER_SHARE &&
           (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) == 0 && parameter->reserved == 0 &&
           semantic_direct_local_tagged_boundary_type_is_exact_verify(semantic, parameter->type,
                                                                      true, true, storage);
}

static bool semantic_direct_local_scalar_ref_parameter_is_exact_verify(
    const XrSemanticPlan *semantic, const XrSemanticParameterRecord *parameter,
    uint16_t *machine_kind) {
    uint16_t kind = XR_MACHINE_REP_COUNT;
    if (!semantic || !parameter ||
        parameter->function >= xr_semantic_plan_function_count(semantic) ||
        parameter->value == XR_SEMANTIC_INDEX_NONE || parameter->mode != XR_PARAM_REF ||
        parameter->ownership != XI_OWN_NONE || parameter->transfer_mode != XR_TRANSFER_SHARE ||
        (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) != 0 || parameter->reserved != 0 ||
        semantic_type_expected_rep(xr_semantic_plan_type(semantic, parameter->type), &kind) != 1)
        return false;
    if (machine_kind)
        *machine_kind = kind;
    return true;
}

static bool semantic_direct_local_array_value_parameter_is_exact_verify(
    const XrSemanticPlan *semantic, const XrSemanticParameterRecord *parameter, uint8_t *storage) {
    return semantic_direct_local_array_parameter_is_exact_verify(semantic, parameter, XR_PARAM_READ,
                                                                 storage);
}

/* A direct-local call that hands back a freshly owned `Array<T>`. The container
 * is a dynamic value rather than an aggregate slot the caller owns, so the
 * result is a transfer of the outer tagged value, exactly as an owned String
 * is. An aliased or parameter-forwarded return is refused: it would hand back a
 * borrow whose extent this plan cannot state. */
static bool semantic_direct_local_array_result_is_exact_verify(const XrSemanticPlan *semantic,
                                                               uint32_t operation_index) {
    const XrSemanticOperationRecord *operation =
        operation_index == XR_SEMANTIC_INDEX_NONE
            ? NULL
            : xr_semantic_plan_operation(semantic, operation_index);
    if (!semantic || !operation ||
        (operation->opcode != XI_CALL && operation->opcode != XI_TAIL_CALL) ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->return_complete != 1 || operation->return_provenance != XR_SEM_RETURN_OWNED ||
        !semantic_direct_local_array_type_is_exact_verify(semantic, operation->result_type, false,
                                                          NULL))
        return false;
    const XrSemanticFunctionRecord *callee =
        semantic_direct_local_callee_for_operation(semantic, operation_index);
    return callee && callee->return_type == operation->result_type &&
           callee->return_parameter == -1 && callee->return_provenance == XR_SEM_RETURN_OWNED;
}

static bool
semantic_direct_local_ref_place_is_exact_verify(const XrSemanticPlan *semantic,
                                                const XrSemanticOperandRecord *call_operand,
                                                uint32_t semantic_function,
                                                uint32_t *storage_value) {
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    const XrSemanticOperationRecord *definition = NULL;
    for (uint32_t i = 0; semantic && call_operand && i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->function != semantic_function ||
            candidate->result_value != call_operand->value)
            continue;
        if (definition)
            return false;
        definition = candidate;
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    if (!definition || !operands || definition->opcode != XI_LOCAL_ADDR ||
        definition->result_type != call_operand->type || definition->operand_count != 1 ||
        definition->operand_begin >= operand_count ||
        definition->effects != xi_generated_op_effects(XI_LOCAL_ADDR) ||
        definition->flags != xi_generated_op_default_flags(XI_LOCAL_ADDR) ||
        definition->ownership_use != xi_generated_op_own_use(XI_LOCAL_ADDR) ||
        definition->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        definition->result_alias_operand != -1 || definition->return_parameter != -1 ||
        definition->intrinsic_kind != XR_SEM_INTRINSIC_NONE || definition->metadata_count != 0 ||
        definition->auxiliary_kind != XI_AUX_KIND_NONE || definition->semantic_immediate != 0 ||
        definition->constant != XR_SEMANTIC_INDEX_NONE ||
        definition->callable_function != XR_SEMANTIC_INDEX_NONE ||
        definition->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        definition->allocation_key != NULL || !stable_id_is_zero(definition->allocation_id))
        return false;
    const XrSemanticOperandRecord *source = &operands[definition->operand_begin];
    if (source->type != call_operand->type || source->role != XR_SEM_OPERAND_VALUE ||
        source->parameter != -1 || source->parameter_mode != XR_PARAM_READ ||
        source->transfer_mode != XR_TRANSFER_SHARE || source->access != XR_CALL_ARG_PLAIN ||
        source->origin != XI_PLACE_ORIGIN_NONE || source->lifetime != XI_PLACE_LIFETIME_NONE ||
        source->escape != XI_PLACE_ESCAPE_NONE || source->flags != 0 ||
        source->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    if (storage_value)
        *storage_value = source->value;
    return true;
}

/* Rebuild the ref-i64 address join without consulting the builder's family
 * predicate. A plain LOCAL_ADDR that no exact call boundary consumes is not a
 * RAW_PTR value merely because it has the same opcode. */
static bool semantic_direct_local_scalar_ref_address_is_exact_verify(
    const XrSemanticPlan *semantic, const XrSemanticOperationRecord *address) {
    if (!xr_semantic_ref_argument_local_addr_is_exact(
            semantic, address, address ? address->result_type : XR_SEMANTIC_INDEX_NONE, NULL))
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t target_count = (uint32_t) xr_semantic_plan_call_target_count(semantic);
    for (uint32_t target_index = 0; operands && target_index < target_count; target_index++) {
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(semantic, target_index);
        const XrSemanticOperationRecord *call =
            target ? xr_semantic_plan_operation(semantic, target->operation) : NULL;
        const XrSemanticFunctionRecord *callee =
            target ? xr_semantic_plan_function(semantic, target->function) : NULL;
        if (!target || target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL || !call || !callee ||
            call->function != address->function ||
            !xr_semantic_call_target_names_local_function(
                target, call, (uint32_t) xr_semantic_plan_function_count(semantic)) ||
            call->operand_count != (uint32_t) callee->parameter_count + 1u ||
            call->operand_begin > operand_count ||
            call->operand_count > operand_count - call->operand_begin)
            continue;
        for (uint16_t ordinal = 0; ordinal < callee->parameter_count; ordinal++) {
            const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(
                semantic, callee->parameter_begin + ordinal);
            const XrSemanticOperandRecord *operand =
                &operands[call->operand_begin + ordinal + 1u];
            uint16_t machine_kind = XR_MACHINE_REP_COUNT;
            uint32_t storage_value = XR_SEMANTIC_INDEX_NONE;
            if (!parameter || parameter->function != target->function ||
                parameter->ordinal != ordinal || operand->role != XR_SEM_OPERAND_ARGUMENT ||
                operand->parameter != (int16_t) ordinal || operand->value != address->result_value ||
                operand->type != address->result_type || operand->parameter_mode != XR_PARAM_REF ||
                operand->access != XR_CALL_ARG_REF || operand->origin == XI_PLACE_ORIGIN_NONE ||
                operand->lifetime != XI_PLACE_LIFETIME_CALL_BOUND ||
                operand->escape != XI_PLACE_ESCAPE_NONE ||
                operand->ownership_action != XR_SEM_OPERAND_BORROW ||
                operand->transfer_mode != XR_TRANSFER_SHARE ||
                operand->flags !=
                    (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE) ||
                !semantic_direct_local_scalar_ref_parameter_is_exact_verify(
                    semantic, parameter, &machine_kind) ||
                machine_kind != XR_MACHINE_REP_I64 ||
                !semantic_direct_local_ref_place_is_exact_verify(
                    semantic, operand, call->function, &storage_value))
                continue;
            return true;
        }
    }
    return false;
}

/* The borrowed read of a String held in a shared cell, re-derived through the
 * shared judgement rather than read back from the builder. The builder binds
 * every such read, so a value it left unbound has to look like a missing
 * binding here rather than pass unexamined. */
static bool semantic_string_shared_read_is_exact_verify(const XrSemanticPlan *semantic,
                                                        const XrSemanticOperationRecord *operation,
                                                        uint32_t semantic_value,
                                                        uint32_t semantic_type,
                                                        uint32_t semantic_function) {
    return xr_semantic_tagged_string_shared_read_is_exact(semantic, operation) &&
           operation->result_value == semantic_value && operation->result_type == semantic_type &&
           operation->function == semantic_function;
}

/* Independently re-derive the borrowed Array shared-read carrier that the
 * builder binds. The shared judgement proves operation exactness and unique
 * definition; this wrapper also ties the row to the value binding under
 * verification. */
static bool semantic_array_shared_read_is_exact_verify(const XrSemanticPlan *semantic,
                                                       const XrSemanticOperationRecord *operation,
                                                       uint32_t semantic_value,
                                                       uint32_t semantic_type,
                                                       uint32_t semantic_function) {
    return xr_semantic_tagged_array_shared_read_is_exact(semantic, operation) &&
           operation->result_value == semantic_value && operation->result_type == semantic_type &&
           operation->function == semantic_function;
}

/* Reconstruct the writeback half of an exact direct-local ref boundary from
 * frozen SemanticPlan and TargetPlan rows.  A PLACE_LOAD result is not admitted
 * merely because it has an Array type: its sole place must be the LOCAL_ADDR
 * carried by exactly one direct-local tagged-ref call argument. */
static bool semantic_direct_local_tagged_ref_place_load_is_exact_verify(
    const XrTargetPlan *plan, const XrSemanticOperationRecord *load, uint32_t semantic_value,
    uint32_t semantic_type, uint32_t semantic_function) {
    const XrSemanticPlan *semantic = plan ? plan->semantic_plan : NULL;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        semantic ? xr_semantic_plan_operands(semantic, &operand_count) : NULL;
    if (!semantic || !load || !operands || load->opcode != XI_PLACE_LOAD ||
        load->result_value != semantic_value || load->result_type != semantic_type ||
        load->function != semantic_function || load->operand_count != 1 ||
        load->operand_begin >= operand_count ||
        load->effects != xi_generated_op_effects(XI_PLACE_LOAD) ||
        load->flags != xi_generated_op_default_flags(XI_PLACE_LOAD) ||
        load->ownership_use != xi_generated_op_own_use(XI_PLACE_LOAD) ||
        load->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        load->result_alias_operand != -1 || load->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        load->metadata_count != 0 || load->auxiliary_kind != XI_AUX_KIND_NONE ||
        load->semantic_immediate != 0 || load->constant != XR_SEMANTIC_INDEX_NONE ||
        load->callable_function != XR_SEMANTIC_INDEX_NONE ||
        load->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE)
        return false;
    const XrSemanticOperandRecord *place = &operands[load->operand_begin];
    if (place->type != semantic_type || place->role != XR_SEM_OPERAND_VALUE ||
        place->parameter != -1 || place->parameter_mode != XR_PARAM_READ ||
        place->access != XR_CALL_ARG_PLAIN || place->origin != XI_PLACE_ORIGIN_NONE ||
        place->lifetime != XI_PLACE_LIFETIME_NONE || place->escape != XI_PLACE_ESCAPE_NONE ||
        place->flags != 0 || place->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;

    const XrSemanticParameterRecord *place_parameter = NULL;
    for (uint32_t i = 0; i < (uint32_t) xr_semantic_plan_parameter_count(semantic); i++) {
        const XrSemanticParameterRecord *candidate = xr_semantic_plan_parameter(semantic, i);
        if (!candidate || candidate->value != place->value ||
            candidate->function != semantic_function)
            continue;
        if (place_parameter)
            return false;
        place_parameter = candidate;
    }
    uint8_t parameter_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (semantic_direct_local_tagged_ref_parameter_is_exact_verify(semantic, place_parameter,
                                                                   &parameter_storage) &&
        place_parameter->type == semantic_type) {
        const XrTargetValueRepRecord *binding = xr_target_plan_value_rep(plan, place->value);
        const XrTargetMachineRepRecord *register_rep =
            binding && binding->register_rep < plan->machine_reps_count
                ? &plan->machine_reps[binding->register_rep]
                : NULL;
        const XrTargetMachineRepRecord *memory_rep =
            binding && binding->memory_rep < plan->machine_reps_count
                ? &plan->machine_reps[binding->memory_rep]
                : NULL;
        const XrTargetSlotRecord *slot =
            binding && binding->slot < plan->slots_count ? &plan->slots[binding->slot] : NULL;
        return binding && register_rep && memory_rep && slot &&
               register_rep->kind == XR_MACHINE_REP_RAW_PTR &&
               memory_rep->kind == XR_MACHINE_REP_RAW_PTR &&
               register_rep->root_kind == XR_TARGET_ROOT_NONE &&
               memory_rep->root_kind == XR_TARGET_ROOT_NONE &&
               register_rep->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
               memory_rep->ownership == XR_TARGET_OWNERSHIP_BORROWED &&
               slot->semantic_value == place->value &&
               slot->semantic_operation == XR_SEMANTIC_INDEX_NONE &&
               slot->function == semantic_function && slot->role == XR_TARGET_SLOT_PARAMETER &&
               slot->register_rep == binding->register_rep &&
               slot->memory_rep == binding->memory_rep && slot->root_kind == XR_TARGET_ROOT_NONE &&
               slot->ownership == XR_TARGET_OWNERSHIP_BORROWED;
    }

    const XrTargetCallArgumentRecord *argument = NULL;
    for (uint32_t i = 0; i < plan->call_arguments_count; i++) {
        if (plan->call_arguments[i].semantic_value != place->value)
            continue;
        if (argument)
            return false;
        argument = &plan->call_arguments[i];
    }
    const XrTargetCallRecord *call =
        argument && argument->call < plan->calls_count ? &plan->calls[argument->call] : NULL;
    const XrSemanticCallTargetRecord *target =
        call && call->semantic_call_target != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_call_target(semantic, call->semantic_call_target)
            : NULL;
    const XrSemanticParameterRecord *parameter =
        argument && argument->callee_parameter != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_parameter(semantic, argument->callee_parameter)
            : NULL;
    const XrSemanticOperationRecord *call_operation =
        call ? xr_semantic_plan_operation(semantic, call->semantic_operation) : NULL;
    const XrSemanticOperandRecord *call_operand =
        argument && argument->semantic_operand < operand_count
            ? &operands[argument->semantic_operand]
            : NULL;
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    return argument && call && target && parameter && call_operation && call_operand &&
           semantic_direct_local_tagged_ref_parameter_is_exact_verify(semantic, parameter,
                                                                      &storage) &&
           semantic_direct_local_ref_place_is_exact_verify(semantic, call_operand,
                                                            call_operation->function, NULL) &&
           target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
           target->operation == call->semantic_operation &&
           target->function == call->callee_function &&
           call_operation->function == semantic_function && call_operand->value == place->value &&
           call_operand->type == semantic_type && call_operand->role == XR_SEM_OPERAND_ARGUMENT &&
           call_operand->parameter == (int16_t) argument->ordinal &&
           call_operand->parameter_mode == XR_PARAM_REF &&
           call_operand->access == XR_CALL_ARG_REF &&
           call_operand->origin != XI_PLACE_ORIGIN_NONE &&
           call_operand->lifetime == XI_PLACE_LIFETIME_CALL_BOUND &&
           call_operand->escape == XI_PLACE_ESCAPE_NONE &&
           call_operand->ownership_action == XR_SEM_OPERAND_BORROW &&
           call_operand->transfer_mode == XR_TRANSFER_SHARE &&
           call_operand->flags == (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE) &&
           argument->mode == XR_TARGET_CALL_REFERENCE &&
           argument->ownership == XR_TARGET_CALL_BORROW &&
           argument->transfer_mode == XR_TRANSFER_SHARE &&
           argument->flags == XR_TARGET_CALL_ARGUMENT_ADDRESSABLE &&
           argument->array_element_storage == storage && argument->reserved8[0] == 0 &&
           argument->reserved8[1] == 0 && argument->reserved8[2] == 0;
}

static bool verify_array_intrinsic_fill_type(const XrSemanticTypeRecord *type,
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

/* Verifier-side reconstruction from frozen SemanticPlan rows only. */
static bool semantic_array_intrinsic_is_exact_verify(const XrSemanticPlan *semantic,
                                                     const XrSemanticOperationRecord *operation,
                                                     uint8_t *target_kind,
                                                     uint8_t *target_storage) {
    uint32_t operand_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    bool with_capacity =
        operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_WITH_CAPACITY;
    bool filled = operation && operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_FILLED_NEW;
    uint16_t expected = with_capacity ? 1u : 2u;
    if (!semantic || !operation || (!with_capacity && !filled) || !operands || !children ||
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
    const XrSemanticTypeRecord *array = xr_semantic_plan_type(semantic, operation->result_type);
    if (!xr_semantic_array_type_row_is_exact(array) || array->child_begin >= child_count)
        return false;
    uint32_t element = children[array->child_begin];
    const XrSemanticTypeRecord *element_type = xr_semantic_plan_type(semantic, element);
    const XrSemanticOperandRecord *count = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *count_type = xr_semantic_plan_type(semantic, count->type);
    uint16_t count_rep = XR_MACHINE_REP_COUNT;
    uint8_t storage = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t semantic_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!count_type || semantic_type_expected_rep(count_type, &count_rep) != 1 ||
        count_rep != XR_MACHINE_REP_I64 || count->role != XR_SEM_OPERAND_ARGUMENT ||
        count->parameter != 0 || count->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        count->ownership_action != XR_SEM_OPERAND_CONSUME ||
        !xr_target_array_storage_from_semantic(operation->array_element_storage,
                                               &semantic_storage) ||
        !xr_target_array_storage_from_type(element_type, &storage) || storage != semantic_storage)
        return false;
    if (filled) {
        const XrSemanticOperandRecord *fill = count + 1;
        const XrSemanticTypeRecord *fill_type = xr_semantic_plan_type(semantic, fill->type);
        if (!verify_array_intrinsic_fill_type(fill_type, storage) ||
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

/* Verifier-side reconstruction of the dedicated Array.fill authority. The
 * metadata row is debug-only: its spelling is never read, and the fixed two
 * operands are validated as ordered receiver/fill rows rather than used to
 * infer the family. */
static bool semantic_array_fill_scalar_is_exact_verify(const XrSemanticPlan *semantic,
                                                       const XrSemanticOperationRecord *operation,
                                                       uint32_t *receiver_value,
                                                       uint32_t *fill_value,
                                                       uint8_t *target_storage) {
    uint32_t operand_count = 0, child_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    (void) xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!semantic || !operation || !operands || !children ||
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
    const XrSemanticTypeRecord *array = xr_semantic_plan_type(semantic, receiver->type);
    if (!xr_semantic_array_type_row_is_exact(array) || array->child_begin >= child_count)
        return false;
    uint32_t element_index = children[array->child_begin];
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(semantic, element_index);
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

/* Independent frozen-row reconstruction for Array.map/filter/reduce. No
 * selector spelling, live Xi type, or argument-count inference is consulted. */
static bool semantic_array_hof_is_exact_verify(const XrSemanticPlan *semantic,
                                               const XrSemanticOperationRecord *operation,
                                               uint8_t *target_kind, uint8_t *receiver_storage,
                                               uint8_t *result_storage, uint32_t *receiver_value,
                                               uint32_t *callback_value, uint32_t *initial_value) {
    uint32_t operand_count = 0, child_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    (void) xr_semantic_plan_metadata(semantic, &metadata_count);
    uint8_t kind = XR_TARGET_ARRAY_HOF_NONE;
    if (operation) {
        if (operation->array_hof_kind == XR_SEM_ARRAY_HOF_MAP)
            kind = XR_TARGET_ARRAY_HOF_MAP;
        else if (operation->array_hof_kind == XR_SEM_ARRAY_HOF_FILTER)
            kind = XR_TARGET_ARRAY_HOF_FILTER;
        else if (operation->array_hof_kind == XR_SEM_ARRAY_HOF_REDUCE)
            kind = XR_TARGET_ARRAY_HOF_REDUCE;
    }
    uint16_t expected_operands = kind == XR_TARGET_ARRAY_HOF_REDUCE ? 3u : 2u;
    if (!semantic || !operation || !operands || !children ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_HOF ||
        kind == XR_TARGET_ARRAY_HOF_NONE || operation->opcode != XI_CALL_METHOD ||
        operation->operand_count != expected_operands || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        operation->semantic_immediate != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function >= xr_semantic_plan_function_count(semantic) ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1)
        return false;
    const XrSemanticOperandRecord *rows = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *array = xr_semantic_plan_type(semantic, rows[0].type);
    if (!xr_semantic_array_type_row_is_exact(array) || array->child_begin >= child_count)
        return false;
    uint32_t source_element = children[array->child_begin];
    const XrSemanticTypeRecord *source_type = xr_semantic_plan_type(semantic, source_element);
    uint8_t source = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t frozen_source = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!xr_target_array_storage_from_type(source_type, &source) ||
        !xr_target_array_storage_from_semantic(operation->array_element_storage, &frozen_source) ||
        source != frozen_source)
        return false;
    uint32_t result_element = operation->result_type;
    if (kind != XR_TARGET_ARRAY_HOF_REDUCE) {
        const XrSemanticTypeRecord *result_array =
            xr_semantic_plan_type(semantic, operation->result_type);
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
    const XrSemanticTypeRecord *result_type = xr_semantic_plan_type(semantic, result_element);
    uint8_t result = XR_TARGET_ARRAY_STORAGE_NONE;
    uint8_t frozen_result = XR_TARGET_ARRAY_STORAGE_NONE;
    if (!xr_target_array_storage_from_type(result_type, &result) ||
        !xr_target_array_storage_from_semantic(operation->array_result_element_storage,
                                               &frozen_result) ||
        result != frozen_result)
        return false;
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(semantic, operation->callable_function);
    uint16_t expected_parameters = kind == XR_TARGET_ARRAY_HOF_REDUCE ? 2u : 1u;
    if (!callee || callee->parent != operation->function || callee->capture_count != 0 ||
        callee->parameter_count != expected_parameters ||
        (callee->semantic_effects & (XI_EFFECT_SIDE_EFFECT | XI_EFFECT_MEMORY_WRITE |
                                     XI_EFFECT_MAY_THROW | XI_EFFECT_MAY_SUSPEND)) != 0 ||
        callee->parameter_begin > xr_semantic_plan_parameter_count(semantic) ||
        callee->parameter_count >
            xr_semantic_plan_parameter_count(semantic) - callee->parameter_begin)
        return false;
    const XrSemanticParameterRecord *first =
        xr_semantic_plan_parameter(semantic, callee->parameter_begin);
    const XrSemanticParameterRecord *second =
        expected_parameters == 2u
            ? xr_semantic_plan_parameter(semantic, callee->parameter_begin + 1u)
            : NULL;
    if (!first || first->function != operation->callable_function || first->ordinal != 0 ||
        first->type != (kind == XR_TARGET_ARRAY_HOF_REDUCE ? result_element : source_element) ||
        (second && (second->function != operation->callable_function || second->ordinal != 1 ||
                    second->type != source_element)))
        return false;
    if (kind == XR_TARGET_ARRAY_HOF_FILTER) {
        uint16_t rep = XR_MACHINE_REP_COUNT;
        const XrSemanticTypeRecord *return_type =
            xr_semantic_plan_type(semantic, callee->return_type);
        if (!return_type || semantic_type_expected_rep(return_type, &rep) != 1 ||
            rep != XR_MACHINE_REP_I1)
            return false;
    } else if (callee->return_type != result_element) {
        return false;
    }
    const XrSemanticTypeRecord *callback_type = xr_semantic_plan_type(semantic, rows[1].type);
    if (!callback_type || callback_type->kind != XR_KIND_FUNCTION ||
        callback_type->child_count != (uint32_t) expected_parameters + 1u ||
        callback_type->child_begin > child_count ||
        callback_type->child_count > child_count - callback_type->child_begin)
        return false;
    for (uint16_t i = 0; i < expected_parameters; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(semantic, callee->parameter_begin + i);
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
    for (uint32_t i = 0; result_exact && i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
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

/* Independent reconstruction of the frozen Map<K, V> entry-iterator family.
 * This verifier
 * deliberately does not call the builder's shared shape helper:
 * it rechecks the stable selector
 * IDs, exact instantiated types, ownership and
 * unique producer chain from serialized
 * SemanticPlan rows. */
static bool semantic_map_entry_plain_builtin_type_verify(const XrSemanticTypeRecord *type,
                                                         uint32_t kind, uint16_t children,
                                                         uint32_t extent, uint8_t flags) {
    XrStableId zero = {{0}};
    return type && type->kind == kind && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) &&
           xr_stable_id_equal(type->source_enum_identity, zero) && !type->source_enum_key &&
           type->child_count == children && type->aggregate_extent == extent &&
           type->aggregate_align == 0 && type->enum_layout_id == 0 &&
           type->enum_member_count == 0 && type->enum_flags == 0 && type->reserved_enum == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->flags == flags && type->canonical_key;
}

static bool semantic_map_entry_iterator_types_are_exact_verify(const XrSemanticPlan *semantic,
                                                               const XrSemanticTypeRecord *map,
                                                               const XrSemanticTypeRecord *iterator,
                                                               uint32_t *entry_type_index) {
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    if (!semantic || !children ||
        !semantic_map_entry_plain_builtin_type_verify(
            map, XR_KIND_MAP, 2, 0, XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        !semantic_map_entry_plain_builtin_type_verify(iterator, XR_KIND_INSTANCE, 1, 0,
                                                      XR_SEM_TYPE_REFERENCE_CAPABLE |
                                                          XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        map->child_begin > child_count || map->child_count > child_count - map->child_begin ||
        iterator->child_begin >= child_count)
        return false;
    uint32_t entry_index = children[iterator->child_begin];
    const XrSemanticTypeRecord *entry = xr_semantic_plan_type(semantic, entry_index);
    if (!semantic_map_entry_plain_builtin_type_verify(entry, XR_KIND_TUPLE, 2, 2,
                                                      XR_SEM_TYPE_REFERENCE_CAPABLE |
                                                          XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        entry->child_begin > child_count || entry->child_count > child_count - entry->child_begin ||
        children[entry->child_begin] != children[map->child_begin] ||
        children[entry->child_begin + 1] != children[map->child_begin + 1])
        return false;
    const char prefix[] = "type-v3:11:0:0:0:0:0:0:0:0:255:0:;named:8:Iterator[1;";
    size_t prefix_length = sizeof(prefix) - 1u;
    size_t key_length = strlen(iterator->canonical_key);
    size_t entry_key_length = strlen(entry->canonical_key);
    if (key_length != prefix_length + entry_key_length + 1u ||
        strncmp(iterator->canonical_key, prefix, prefix_length) != 0 ||
        memcmp(iterator->canonical_key + prefix_length, entry->canonical_key, entry_key_length) !=
            0 ||
        iterator->canonical_key[key_length - 1u] != ']')
        return false;
    if (entry_type_index)
        *entry_type_index = entry_index;
    return true;
}

static bool semantic_map_entry_iterator_common_is_exact_verify(
    const XrSemanticPlan *semantic, const XrSemanticOperationRecord *operation,
    XrSemanticIntrinsicKind intrinsic, XiMethodSymbolId symbol, uint8_t result_ownership,
    uint8_t return_provenance, uint8_t return_complete, uint32_t *receiver_value) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!semantic || !operation || !operands || !metadata ||
        operation->intrinsic_kind != intrinsic || operation->opcode != XI_CALL_METHOD ||
        operation->semantic_immediate != ((int64_t) symbol << 1) || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count || !metadata[operation->metadata_begin] ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        (operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) &&
         operation->flags != (xi_generated_op_default_flags(XI_CALL_METHOD) | XI_FLAG_TAIL)) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != result_ownership ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->return_provenance != return_provenance ||
        operation->return_complete != return_complete ||
        operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_NONE || operation->view_capability != 0 ||
        operation->view_lifetime != 0 || operation->view_complete != 0 ||
        operation->reserved_view[0] != 0 || operation->reserved_view[1] != 0)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    if (receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW || receiver->parameter_mode != 0 ||
        receiver->access != 0 || receiver->origin != 0 || receiver->lifetime != 0 ||
        receiver->escape != 0 || receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return false;
    if (receiver_value)
        *receiver_value = receiver->value;
    return true;
}

static const XrSemanticOperationRecord *
semantic_map_entry_unique_value_definition_verify(const XrSemanticPlan *semantic, uint32_t value) {
    const XrSemanticOperationRecord *definition = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->result_value != value)
            continue;
        if (definition)
            return NULL;
        definition = candidate;
    }
    return definition;
}

static bool semantic_map_entries_iterator_is_exact_verify(
    const XrSemanticPlan *semantic, const XrSemanticOperationRecord *operation,
    uint32_t *receiver_value, uint32_t *entry_type_index) {
    uint32_t receiver_value_index = XR_SEMANTIC_INDEX_NONE;
    if (!semantic_map_entry_iterator_common_is_exact_verify(
            semantic, operation, XR_SEM_INTRINSIC_MAP_ENTRIES_ITERATOR,
            XI_METHOD_SYMBOL_ENTRIES_ITERATOR, XI_GEN_RESULT_OWNERSHIP_OWNED, XR_SEM_RETURN_OWNED,
            1, &receiver_value_index))
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *map = xr_semantic_plan_type(semantic, receiver->type);
    const XrSemanticTypeRecord *iterator = xr_semantic_plan_type(semantic, operation->result_type);
    if (!semantic_map_entry_iterator_types_are_exact_verify(semantic, map, iterator,
                                                            entry_type_index))
        return false;
    if (receiver_value)
        *receiver_value = receiver_value_index;
    return true;
}

static bool
semantic_map_entry_iterator_has_next_is_exact_verify(const XrSemanticPlan *semantic,
                                                     const XrSemanticOperationRecord *operation,
                                                     uint32_t *receiver_value) {
    uint32_t receiver_value_index = XR_SEMANTIC_INDEX_NONE;
    if (!semantic_map_entry_iterator_common_is_exact_verify(
            semantic, operation, XR_SEM_INTRINSIC_MAP_ENTRY_ITERATOR_HAS_NEXT,
            XI_METHOD_SYMBOL_HAS_NEXT, XI_GEN_RESULT_OWNERSHIP_CALL_RESULT, XR_SEM_RETURN_NONE, 0,
            &receiver_value_index))
        return false;
    const XrSemanticOperationRecord *factory =
        semantic_map_entry_unique_value_definition_verify(semantic, receiver_value_index);
    const XrSemanticTypeRecord *result = xr_semantic_plan_type(semantic, operation->result_type);
    XrStableId zero = {{0}};
    if (!factory || factory->function != operation->function ||
        !semantic_map_entries_iterator_is_exact_verify(semantic, factory, NULL, NULL) || !result ||
        result->kind != XR_KIND_BOOL || result->builtin_type != XR_TID_NULL ||
        result->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(result->source_class_identity, zero) || result->child_count != 0 ||
        result->aggregate_extent != 0 || result->aggregate_align != 0 ||
        result->scalar_rep != XR_SCALAR_REP_NONE || result->flags != 0)
        return false;
    if (receiver_value)
        *receiver_value = receiver_value_index;
    return true;
}

static bool
semantic_map_entry_iterator_next_is_exact_verify(const XrSemanticPlan *semantic,
                                                 const XrSemanticOperationRecord *operation,
                                                 uint32_t *receiver_value) {
    uint32_t receiver_value_index = XR_SEMANTIC_INDEX_NONE;
    if (!semantic_map_entry_iterator_common_is_exact_verify(
            semantic, operation, XR_SEM_INTRINSIC_MAP_ENTRY_ITERATOR_NEXT, XI_METHOD_SYMBOL_NEXT,
            XI_GEN_RESULT_OWNERSHIP_OWNED, XR_SEM_RETURN_OWNED, 1, &receiver_value_index))
        return false;
    const XrSemanticOperationRecord *factory =
        semantic_map_entry_unique_value_definition_verify(semantic, receiver_value_index);
    uint32_t entry_type = XR_SEMANTIC_INDEX_NONE;
    if (!factory || factory->function != operation->function ||
        !semantic_map_entries_iterator_is_exact_verify(semantic, factory, NULL, &entry_type) ||
        operation->result_type != entry_type)
        return false;
    if (receiver_value)
        *receiver_value = receiver_value_index;
    return true;
}

/* Rebuilt here from the frozen semantic rows, not read back from the builder.
 * `T?` is `T | null`, and the language surface requires the nullable primitives
 * to carry `null` in the tagged representation so a null renders as "null" and
 * not as the payload's zero, with the interpreter and the native backend
 * agreeing. The payload admitted here is exactly one machine scalar that cannot
 * hold a reference, so the tagged carrier owes no reference count. A nullable
 * String, object, or container is reference capable and stays refused. */
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

static int semantic_aggregate_eligibility(const XrSemanticPlan *plan, uint32_t semantic_type,
                                          uint32_t *stack, uint32_t depth) {
    if (semantic_type >= xr_semantic_plan_type_count(plan) || depth >= 64)
        return -1;
    for (uint32_t i = 0; i < depth; i++)
        if (stack[i] == semantic_type)
            return -1;
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, semantic_type);
    uint16_t scalar_kind = XR_MACHINE_REP_COUNT;
    int scalar = type ? semantic_type_expected_rep(type, &scalar_kind) : -1;
    if (scalar < 0)
        return -1;
    if (scalar == 1)
        return scalar_kind == XR_MACHINE_REP_VOID ? 0 : 1;
    int aggregate = xr_semantic_aggregate_type_kind(type);
    if (aggregate <= 0)
        return aggregate;
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    if (type->child_begin > child_count || type->child_count > child_count - type->child_begin ||
        (type->kind == XR_KIND_FIXED_ARRAY
             ? (type->child_count != 1 || type->aggregate_extent == 0 ||
                type->aggregate_extent > UINT16_MAX)
             : type->aggregate_extent != type->child_count))
        return -1;
    stack[depth] = semantic_type;
    uint32_t dependencies = type->kind == XR_KIND_FIXED_ARRAY ? 1u : type->child_count;
    for (uint32_t i = 0; i < dependencies; i++) {
        int child = semantic_aggregate_eligibility(plan, children[type->child_begin + i], stack,
                                                   depth + 1u);
        if (child <= 0)
            return child;
    }
    return 1;
}

static bool semantic_fixed_array_count(const XrSemanticPlan *plan, uint32_t semantic_type,
                                       uint32_t *out) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, semantic_type);
    if (!type || type->kind != XR_KIND_FIXED_ARRAY || type->child_count != 1 ||
        type->aggregate_extent == 0 || type->aggregate_extent > UINT16_MAX)
        return false;
    *out = type->aggregate_extent;
    return true;
}

static bool mark_coroutine_functions(const XrSemanticPlan *plan, uint8_t *deferred,
                                     uint32_t function_count) {
    size_t entity_count = xr_semantic_plan_entity_count(plan);
    size_t operation_count = xr_semantic_plan_operation_count(plan);
    for (size_t i = 0; i < entity_count; i++) {
        const XrSemanticEntityRecord *entity = xr_semantic_plan_entity(plan, i);
        if (!entity || entity->kind != XR_SEM_ENTITY_COROUTINE_STATE ||
            entity->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION)
            continue;
        if (entity->subject >= operation_count)
            return false;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan, entity->subject);
        if (!operation || operation->function >= function_count)
            return false;
        deferred[operation->function] = 1;
    }
    return true;
}

static bool semantic_function_suspendability_is_exact(const XrSemanticPlan *plan, uint32_t function,
                                                      bool *out) {
    if (!plan || !out)
        return false;
    uint32_t function_count = (uint32_t) xr_semantic_plan_function_count(plan);
    uint32_t target_count = (uint32_t) xr_semantic_plan_call_target_count(plan);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    if (function >= function_count)
        return false;
    uint8_t *suspendable = function_count ? (uint8_t *) xr_calloc(function_count, 1) : NULL;
    uint32_t *head =
        function_count ? (uint32_t *) xr_malloc((size_t) function_count * sizeof(*head)) : NULL;
    uint32_t *next =
        target_count ? (uint32_t *) xr_malloc((size_t) target_count * sizeof(*next)) : NULL;
    uint32_t *queue =
        function_count ? (uint32_t *) xr_malloc((size_t) function_count * sizeof(*queue)) : NULL;
    bool valid = (!function_count || (suspendable && head && queue)) && (!target_count || next);
    if (valid) {
        for (uint32_t i = 0; i < function_count; i++)
            head[i] = XR_SEMANTIC_INDEX_NONE;
        for (uint32_t i = 0; i < target_count; i++)
            next[i] = XR_SEMANTIC_INDEX_NONE;
        valid = mark_coroutine_functions(plan, suspendable, function_count);
    }
    for (uint32_t i = 0; valid && i < target_count; i++) {
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(plan, i);
        const XrSemanticOperationRecord *operation =
            target && target->operation < operation_count
                ? xr_semantic_plan_operation(plan, target->operation)
                : NULL;
        if (!target || !operation || operation->function >= function_count) {
            valid = false;
            break;
        }
        if (xr_semantic_call_target_names_local_function(target, operation, function_count)) {
            next[i] = head[target->function];
            head[target->function] = i;
        }
    }
    uint32_t begin = 0;
    uint32_t end = 0;
    for (uint32_t i = 0; valid && i < function_count; i++)
        if (suspendable[i])
            queue[end++] = i;
    while (valid && begin < end) {
        uint32_t callee = queue[begin++];
        for (uint32_t edge = head[callee]; edge != XR_SEMANTIC_INDEX_NONE; edge = next[edge]) {
            const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(plan, edge);
            const XrSemanticOperationRecord *operation =
                target ? xr_semantic_plan_operation(plan, target->operation) : NULL;
            if (!operation || operation->function >= function_count) {
                valid = false;
                break;
            }
            if (!suspendable[operation->function]) {
                suspendable[operation->function] = 1;
                queue[end++] = operation->function;
            }
        }
    }
    if (valid)
        *out = suspendable[function] != 0;
    xr_free(suspendable);
    xr_free(head);
    xr_free(next);
    xr_free(queue);
    return valid;
}

static const XrSemanticParameterRecord *
semantic_parameter_for_value(const XrSemanticPlan *plan, uint32_t function, uint32_t value) {
    uint32_t parameter_count = (uint32_t) xr_semantic_plan_parameter_count(plan);
    for (uint32_t i = 0; i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(plan, i);
        if (parameter && parameter->function == function && parameter->value == value)
            return parameter;
    }
    return NULL;
}

static bool reconstruct_value_slot_identity_for_semantic(
    const XrSemanticPlan *semantic, const XrTargetSlotRecord *slot,
    uint32_t target_function, uint32_t semantic_value,
    uint32_t semantic_function, XrStableId *out) {
    if (!slot || !out || slot->semantic_value != semantic_value ||
        slot->function != target_function || slot->logical_slot != XR_SEMANTIC_INDEX_NONE)
        return false;
    const XrSemanticParameterRecord *parameter =
        semantic_parameter_for_value(semantic, semantic_function, semantic_value);
    XrStableId source;
    uint8_t expected_role;
    if (parameter) {
        if (slot->semantic_operation != XR_SEMANTIC_INDEX_NONE)
            return false;
        expected_role = XR_TARGET_SLOT_PARAMETER;
        source = parameter->id;
    } else {
        if (slot->semantic_operation >= xr_semantic_plan_operation_count(semantic))
            return false;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, slot->semantic_operation);
        if (!operation || operation->function != semantic_function ||
            operation->result_value != semantic_value || operation->opcode == XI_PARAM)
            return false;
        expected_role = xr_semantic_dynamic_value_is_join(operation) ? XR_TARGET_SLOT_PHI
                                                                     : XR_TARGET_SLOT_TEMPORARY;
        source = operation->id;
    }
    if (slot->role != expected_role)
        return false;
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(semantic, semantic_function);
    if (!function)
        return false;
    char function_id[XR_STABLE_ID_BYTES * 2 + 1];
    char source_id[XR_STABLE_ID_BYTES * 2 + 1];
    char key[192];
    xr_stable_id_hex(function->id, function_id);
    xr_stable_id_hex(source, source_id);
    int written =
        snprintf(key, sizeof(key), "xray-target-slot-v2:function=%s:role=%u:source=%s:logical=%u",
                 function_id, (unsigned) expected_role, source_id, XR_SEMANTIC_INDEX_NONE);
    XrFingerprint digest;
    return written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static bool reconstruct_value_slot_identity(const XrTargetPlan *plan,
                                            const XrTargetSlotRecord *slot,
                                            uint32_t semantic_value,
                                            uint32_t semantic_function, XrStableId *out) {
    return reconstruct_value_slot_identity_for_semantic(
        plan ? plan->semantic_plan : NULL, slot, semantic_function,
        semantic_value, semantic_function, out);
}

static int target_plan_layout_for_type(const XrTargetPlan *plan, uint32_t semantic_type) {
    uint32_t low = 0;
    uint32_t high = plan->layouts_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        uint32_t candidate = plan->layouts[middle].semantic_type;
        if (candidate < semantic_type)
            low = middle + 1u;
        else
            high = middle;
    }
    return low < plan->layouts_count && plan->layouts[low].semantic_type == semantic_type
               ? (int) low
               : -1;
}

static bool semantic_heap_closure_is_exact(const XrSemanticPlan *semantic,
                                           const XrSemanticOperationRecord *operation) {
    if (!semantic || !operation || operation->opcode != XI_CLOSURE_NEW ||
        operation->callable_function >= xr_semantic_plan_function_count(semantic) ||
        operation->operand_count != 0 || !xr_semantic_allocation_identity_is_canonical(operation) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
        return false;
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(semantic, operation->callable_function);
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, operation->result_type);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
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
        callee->parameter_begin > xr_semantic_plan_parameter_count(semantic) ||
        callee->parameter_count >
            xr_semantic_plan_parameter_count(semantic) - callee->parameter_begin)
        return false;
    for (uint32_t i = 0; i < callee->parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(semantic, callee->parameter_begin + i);
        if (!parameter || parameter->function != operation->callable_function ||
            parameter->ordinal != i ||
            (typed_function && children[type->child_begin + i] != parameter->type))
            return false;
    }
    return opaque_closure ||
           children[type->child_begin + callee->parameter_count] == callee->return_type;
}

static const XrSemanticFunctionRecord *
semantic_direct_local_callee_for_operation(const XrSemanticPlan *semantic,
                                           uint32_t operation_index) {
    size_t target_count = xr_semantic_plan_call_target_count(semantic);
    const XrSemanticFunctionRecord *callee = NULL;
    for (size_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(semantic, i);
        if (!target || target->operation != operation_index ||
            target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL)
            continue;
        if (callee)
            return NULL;
        callee = xr_semantic_plan_function(semantic, target->function);
        if (!callee)
            return NULL;
    }
    return callee;
}

static bool semantic_direct_local_string_result_is_exact(const XrSemanticPlan *semantic,
                                                         uint32_t operation_index) {
    const XrSemanticOperationRecord *operation =
        operation_index == XR_SEMANTIC_INDEX_NONE
            ? NULL
            : xr_semantic_plan_operation(semantic, operation_index);
    if (!semantic || !operation ||
        (operation->opcode != XI_CALL && operation->opcode != XI_TAIL_CALL) ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->return_complete != 1 || operation->return_provenance != XR_SEM_RETURN_OWNED ||
        !xr_semantic_tagged_string_type_is_exact(
            xr_semantic_plan_type(semantic, operation->result_type)))
        return false;
    const XrSemanticFunctionRecord *callee =
        semantic_direct_local_callee_for_operation(semantic, operation_index);
    return callee && callee->return_type == operation->result_type &&
           callee->return_parameter == -1 && callee->return_provenance == XR_SEM_RETURN_OWNED;
}

static bool semantic_direct_local_adt_enum_result_is_exact(const XrSemanticPlan *semantic,
                                                           uint32_t operation_index) {
    const XrSemanticOperationRecord *operation =
        operation_index == XR_SEMANTIC_INDEX_NONE
            ? NULL
            : xr_semantic_plan_operation(semantic, operation_index);
    const XrSemanticFunctionRecord *callee =
        semantic_direct_local_callee_for_operation(semantic, operation_index);
    return xr_semantic_direct_local_adt_enum_result_is_exact(semantic, operation, callee);
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
semantic_stringbuilder_append_result_is_exact_verify(const XrSemanticPlan *semantic,
                                                     const XrSemanticOperationRecord *operation) {
    const XrSemanticFunctionRecord *function =
        operation ? xr_semantic_plan_function(semantic, operation->function) : NULL;
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

static bool operation_is_exact_stringbuilder_append_rune(const XrSemanticPlan *semantic,
                                                         const XrSemanticOperationRecord *operation,
                                                         uint32_t *receiver_value,
                                                         uint32_t *argument_value);

static bool operation_is_exact_stringbuilder_to_string(const XrSemanticPlan *semantic,
                                                       const XrSemanticOperationRecord *operation,
                                                       uint32_t *receiver_value);

static bool
operation_is_exact_stringbuilder_append_string(const XrSemanticPlan *semantic,
                                               const XrSemanticOperationRecord *operation,
                                               uint32_t *argument_value);

static bool operation_is_exact_json_namespace_value(const XrSemanticPlan *semantic,
                                                    const XrSemanticOperationRecord *operation,
                                                    uint32_t *argument_value);

static bool operation_is_exact_array_member_scalar(const XrSemanticPlan *semantic,
                                                   const XrSemanticOperationRecord *operation,
                                                   uint32_t *receiver_type_index,
                                                   uint32_t *element_value, bool *receiver_result);

static bool operation_is_exact_native_module_scalar_call(const XrSemanticPlan *semantic,
                                                         const XrSemanticOperationRecord *operation,
                                                         uint32_t *argument_count);

static bool
semantic_stringbuilder_constructor_is_exact(const XrSemanticPlan *semantic,
                                            const XrSemanticOperationRecord *operation) {
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!semantic || !operation || operation->opcode != XI_CALL_BUILTIN ||
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
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, operation->result_type);
    return semantic_stringbuilder_type_is_exact(type);
}

/* Independent reconstruction: consume only frozen SemanticPlan rows. */
static bool semantic_u8_slice_type_is_exact(const XrSemanticPlan *semantic, uint32_t type_index);

static bool semantic_string_byte_slice_view_is_exact(const XrSemanticPlan *semantic,
                                                     const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    if (!semantic || !operation || operation->opcode != XI_CALL_BUILTIN ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_STRING_BYTE_SLICE_VIEW ||
        operation->evidence[1] != XA_INTRINSIC_STRING_BYTE_SLICE_VIEW ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->view_source_operand != 0 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_RECEIVER || operation->view_capability != 1 ||
        operation->view_lifetime != 1 || operation->view_complete != 1 ||
        operation->view_element_type >= xr_semantic_plan_type_count(semantic))
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *source_type = xr_semantic_plan_type(semantic, source->type);
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(semantic, operation->result_type);
    const XrSemanticTypeRecord *element =
        xr_semantic_plan_type(semantic, operation->view_element_type);
    return source->value == operation->view_source_value && source->parameter == 0 &&
           source->role == XR_SEM_OPERAND_ARGUMENT &&
           (source->flags & XR_SEM_OPERAND_CALL_CONTRACT) != 0 && source_type &&
           source_type->kind == XR_KIND_STRING && source_type->scalar_rep == XR_SCALAR_REP_NONE &&
           /* The row itself is judged by the one function that answers this
            * question, mirroring the builder. Only the tie between the row's
            * element and the operation's own record is checked separately. */
           semantic_u8_slice_type_is_exact(semantic, operation->result_type) && result_type &&
           result_type->child_begin < child_count &&
           children[result_type->child_begin] == operation->view_element_type && element != NULL;
}

static bool semantic_u8_slice_type_is_exact(const XrSemanticPlan *semantic, uint32_t type_index) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, type_index);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    const uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW;
    const uint8_t allowed = required | XR_SEM_TYPE_CONST;
    if (!type || type->kind != XR_KIND_SLICE || type->builtin_type != XR_TID_NULL ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->aggregate_extent != 0 ||
        type->aggregate_align != 0 || type->child_count != 1 || type->child_begin >= child_count ||
        (type->flags & required) != required || (type->flags & ~allowed) != 0)
        return false;
    const XrSemanticTypeRecord *element =
        xr_semantic_plan_type(semantic, children[type->child_begin]);
    return element && element->kind == XR_KIND_INT && element->builtin_type == XR_TID_NULL &&
           element->scalar_rep == XR_NATIVE_U8 && element->flags == 0 &&
           element->child_count == 0 && element->aggregate_extent == 0 &&
           element->aggregate_align == 0;
}

static bool semantic_u8_slice_parameter_is_exact(const XrSemanticPlan *semantic,
                                                 const XrSemanticParameterRecord *parameter) {
    return parameter && parameter->function < xr_semantic_plan_function_count(semantic) &&
           parameter->value != XR_SEMANTIC_INDEX_NONE && parameter->mode == XR_PARAM_READ &&
           parameter->ownership == XI_OWN_BORROWED &&
           parameter->transfer_mode == XR_TRANSFER_SHARE &&
           (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) == 0 && parameter->reserved == 0 &&
           semantic_u8_slice_type_is_exact(semantic, parameter->type);
}

static bool verifier_channel_type_is_exact(const XrSemanticPlan *semantic, uint32_t type_index,
                                           uint32_t *element_type) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, type_index);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    uint8_t allowed = required | XR_SEM_TYPE_CONST;
    if (!semantic || !type || type->kind != XR_KIND_CHANNEL ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 1 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        (type->flags & required) != required || (type->flags & ~allowed) != 0 ||
        type->child_begin >= child_count ||
        children[type->child_begin] >= xr_semantic_plan_type_count(semantic))
        return false;
    if (element_type)
        *element_type = children[type->child_begin];
    return true;
}

static bool operation_is_exact_array_member_tagged_store(const XrSemanticPlan *semantic,
                                                         const XrSemanticOperationRecord *operation,
                                                         uint32_t *semantic_operand,
                                                         uint32_t *element_value,
                                                         uint32_t *element_type_index) {
    uint32_t element = XR_SEMANTIC_INDEX_NONE;
    uint32_t operand_count = 0, metadata_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    if (!operation_is_exact_array_member_scalar(semantic, operation, NULL, &element, NULL) ||
        !operands || !metadata || !children || operation->metadata_begin >= metadata_count ||
        operation->operand_begin >= operand_count)
        return false;
    const XrArrayMemberShape *shape =
        xr_array_member_shape(metadata[operation->metadata_begin], operation->operand_count);
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(semantic, receiver->type);
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
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, type_index);
    uint32_t operand_index = operation->operand_begin + shape->element_operand;
    if (!type || operand_index >= operand_count ||
        xr_semantic_class_instance_type_source_class(semantic, type) == XR_SEMANTIC_INDEX_NONE ||
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

static bool verifier_channel_capacity_type_is_exact(const XrSemanticPlan *semantic,
                                                    uint32_t type_index) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(semantic, type_index);
    uint16_t kind = XR_MACHINE_REP_COUNT;
    return type && type->kind == XR_KIND_INT && semantic_type_expected_rep(type, &kind) == 1 &&
           kind >= XR_MACHINE_REP_I8 && kind <= XR_MACHINE_REP_USIZE;
}

static bool verifier_channel_allocation_is_exact(const XrSemanticPlan *semantic,
                                                 const XrSemanticOperationRecord *operation) {
    if (!semantic || !operation)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t element_type = XR_SEMANTIC_INDEX_NONE;
    if (operation->opcode != XI_CHAN_NEW || operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        !xr_semantic_allocation_identity_is_canonical(operation) ||
        !verifier_channel_type_is_exact(semantic, operation->result_type, &element_type) ||
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
           capacity->type < xr_semantic_plan_type_count(semantic) &&
           capacity->role == XR_SEM_OPERAND_VALUE && capacity->parameter == -1 &&
           capacity->flags == 0 &&
           verifier_channel_capacity_type_is_exact(semantic, capacity->type) &&
           element_type < xr_semantic_plan_type_count(semantic);
}

static bool verifier_channel_identity_copy_is_exact(const XrSemanticPlan *semantic,
                                                    const XrSemanticOperationRecord *operation,
                                                    const uint8_t *exact_channel_values,
                                                    uint32_t value_count) {
    if (!semantic || !operation || !exact_channel_values)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    if (operation->opcode != XI_COPY || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count ||
        operation->semantic_immediate != XI_COPY_KIND_IDENTITY || operation->allocation_key ||
        !stable_id_is_zero(operation->allocation_id) ||
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
           verifier_channel_type_is_exact(semantic, source->type, &source_element) &&
           verifier_channel_type_is_exact(semantic, operation->result_type, &result_element) &&
           source_element == result_element;
}

static bool collect_exact_channel_values(const XrTargetPlan *plan, uint8_t **out, char *error,
                                         size_t error_size) {
    if (out)
        *out = NULL;
    size_t function_count = xr_semantic_plan_function_count(plan->semantic_plan);
    size_t operation_count = xr_semantic_plan_operation_count(plan->semantic_plan);
    if (!out || function_count > UINT32_MAX || operation_count > UINT32_MAX)
        return report(error, error_size, "XR_EXEC_5003",
                      "channel outer-storage verifier budget is exhausted");
    uint32_t value_count = 0;
    if (function_count) {
        const XrSemanticFunctionRecord *last =
            xr_semantic_plan_function(plan->semantic_plan, (uint32_t) function_count - 1u);
        if (!last || last->value_begin > UINT32_MAX - last->value_count)
            return report(error, error_size, "XR_EXEC_5003",
                          "channel outer-storage value budget overflow");
        value_count = last->value_begin + last->value_count;
    }
    uint8_t *exact = value_count ? (uint8_t *) xr_calloc(value_count, sizeof(*exact)) : NULL;
    if (value_count && !exact)
        return report(error, error_size, "XR_EXEC_5003",
                      "channel outer-storage verifier allocation failed");
    for (uint32_t i = 0; i < (uint32_t) operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan->semantic_plan, i);
        if (!operation || operation->result_value >= value_count) {
            xr_free(exact);
            return report(error, error_size, "XR_TARGET_1001",
                          "channel outer-storage operation identity is invalid");
        }
        bool allocation = verifier_channel_allocation_is_exact(plan->semantic_plan, operation);
        bool alias = verifier_channel_identity_copy_is_exact(plan->semantic_plan, operation, exact,
                                                             value_count);
        if (operation->opcode == XI_CHAN_NEW && !allocation) {
            xr_free(exact);
            return report(error, error_size, "XR_TARGET_1001",
                          "channel allocation authority is not exact");
        }
        if (allocation || alias)
            exact[operation->result_value] = 1;
    }
    *out = exact;
    return true;
}

/* Verifier-side reconstruction intentionally does not call the builder
 * predicate. It proves that every tagged receive boundary is a scalar
 * Channel<T> -> T projection rooted in the canonical channel family. */
static bool verifier_channel_receive_is_exact(const XrTargetPlan *plan,
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
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan->semantic_plan, &operand_count);
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
           verifier_channel_type_is_exact(plan->semantic_plan, receiver->type, &element_type) &&
           element_type == operation->result_type &&
           semantic_type_expected_rep(
               xr_semantic_plan_type(plan->semantic_plan, operation->result_type), &result_kind) ==
               1 &&
           result_kind != XR_MACHINE_REP_VOID;
}

static bool collect_exact_channel_receive_values(const XrTargetPlan *plan,
                                                 const uint8_t *exact_channel_values, uint8_t **out,
                                                 char *error, size_t error_size) {
    if (out)
        *out = NULL;
    size_t function_count = xr_semantic_plan_function_count(plan->semantic_plan);
    size_t operation_count = xr_semantic_plan_operation_count(plan->semantic_plan);
    if (!out || !exact_channel_values || function_count > UINT32_MAX ||
        operation_count > UINT32_MAX)
        return report(error, error_size, "XR_EXEC_5003",
                      "channel receive verifier budget is exhausted");
    uint32_t value_count = 0;
    if (function_count) {
        const XrSemanticFunctionRecord *last =
            xr_semantic_plan_function(plan->semantic_plan, (uint32_t) function_count - 1u);
        if (!last || last->value_begin > UINT32_MAX - last->value_count)
            return report(error, error_size, "XR_EXEC_5003",
                          "channel receive value budget overflow");
        value_count = last->value_begin + last->value_count;
    }
    uint8_t *exact = value_count ? (uint8_t *) xr_calloc(value_count, sizeof(*exact)) : NULL;
    if (value_count && !exact)
        return report(error, error_size, "XR_EXEC_5003",
                      "channel receive verifier allocation failed");
    for (uint32_t i = 0; i < (uint32_t) operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan->semantic_plan, i);
        if (!operation || operation->result_value >= value_count) {
            xr_free(exact);
            return report(error, error_size, "XR_TARGET_1001",
                          "channel receive operation identity is invalid");
        }
        if (operation->opcode != XI_CHAN_TRY_RECV)
            continue;
        uint16_t receive_kind = XR_MACHINE_REP_COUNT;
        if (semantic_type_expected_rep(
                xr_semantic_plan_type(plan->semantic_plan, operation->result_type),
                &receive_kind) != 1 ||
            receive_kind == XR_MACHINE_REP_VOID)
            continue;
        if (!verifier_channel_receive_is_exact(plan, operation, exact_channel_values,
                                               value_count)) {
            xr_free(exact);
            return report(error, error_size, "XR_TARGET_1001",
                          "channel receive storage authority is not exact");
        }
        exact[operation->result_value] = 1;
    }
    *out = exact;
    return true;
}

static bool collect_exact_direct_local_callee_values(const XrTargetPlan *plan, uint8_t **out_exact,
                                                     uint32_t **out_targets, char *error,
                                                     size_t error_size) {
    if (out_exact)
        *out_exact = NULL;
    if (out_targets)
        *out_targets = NULL;
    size_t operation_size = xr_semantic_plan_operation_count(plan->semantic_plan);
    size_t target_size = xr_semantic_plan_call_target_count(plan->semantic_plan);
    size_t function_size = xr_semantic_plan_function_count(plan->semantic_plan);
    if (!out_exact || !out_targets || operation_size > UINT32_MAX || target_size > UINT32_MAX ||
        function_size > UINT32_MAX)
        return report(error, error_size, "XR_EXEC_5003",
                      "direct-local callee verifier budget is exhausted");
    uint32_t value_count = 0;
    if (function_size) {
        const XrSemanticFunctionRecord *last =
            xr_semantic_plan_function(plan->semantic_plan, (uint32_t) function_size - 1u);
        if (!last || last->value_begin > UINT32_MAX - last->value_count)
            return report(error, error_size, "XR_EXEC_5003",
                          "direct-local callee value budget overflow");
        value_count = last->value_begin + last->value_count;
    }
    uint32_t operation_count = (uint32_t) operation_size;
    uint32_t *definition =
        operation_count || value_count
            ? (uint32_t *) xr_calloc(value_count ? value_count : 1u, sizeof(*definition))
            : NULL;
    uint32_t *target_by_operation =
        operation_count ? (uint32_t *) xr_calloc(operation_count, sizeof(*target_by_operation))
                        : NULL;
    uint32_t *target_by_value =
        value_count ? (uint32_t *) xr_calloc(value_count, sizeof(*target_by_value)) : NULL;
    uint32_t *use_count =
        value_count ? (uint32_t *) xr_calloc(value_count, sizeof(*use_count)) : NULL;
    uint8_t *invalid = value_count ? (uint8_t *) xr_calloc(value_count, sizeof(*invalid)) : NULL;
    uint8_t *exact = value_count ? (uint8_t *) xr_calloc(value_count, sizeof(*exact)) : NULL;
    if ((value_count && (!definition || !target_by_value || !use_count || !invalid || !exact)) ||
        (operation_count && !target_by_operation)) {
        xr_free(definition);
        xr_free(target_by_operation);
        xr_free(target_by_value);
        xr_free(use_count);
        xr_free(invalid);
        xr_free(exact);
        return report(error, error_size, "XR_EXEC_5003",
                      "direct-local callee verifier allocation failed");
    }
    for (uint32_t i = 0; i < value_count; i++) {
        definition[i] = XR_SEMANTIC_INDEX_NONE;
        target_by_value[i] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < operation_count; i++) {
        target_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan->semantic_plan, i);
        if (!operation || operation->result_value >= value_count ||
            definition[operation->result_value] != XR_SEMANTIC_INDEX_NONE) {
            goto invalid_authority;
        }
        definition[operation->result_value] = i;
    }
    for (uint32_t i = 0; i < (uint32_t) target_size; i++) {
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(plan->semantic_plan, i);
        if (target && target->kind != XR_SEM_CALL_TARGET_DIRECT_LOCAL)
            continue;
        if (!target || target->operation >= operation_count ||
            target_by_operation[target->operation] != XR_SEMANTIC_INDEX_NONE)
            goto invalid_authority;
        target_by_operation[target->operation] = i;
    }
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan->semantic_plan, &operand_count);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(plan->semantic_plan, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            goto invalid_authority;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand = &operands[use->operand_begin + a];
            if (operand->value >= value_count)
                goto invalid_authority;
            uint32_t source_index = definition[operand->value];
            const XrSemanticOperationRecord *source =
                source_index == XR_SEMANTIC_INDEX_NONE
                    ? NULL
                    : xr_semantic_plan_operation(plan->semantic_plan, source_index);
            if (!source || source->opcode != XI_GET_SHARED)
                continue;
            uint32_t target_index = target_by_operation[i];
            const XrSemanticCallTargetRecord *target =
                target_index == XR_SEMANTIC_INDEX_NONE
                    ? NULL
                    : xr_semantic_plan_call_target(plan->semantic_plan, target_index);
            uint32_t value = source->result_value;
            bool use_is_exact =
                a == 0 && (use->opcode == XI_CALL || use->opcode == XI_TAIL_CALL) &&
                use->function == source->function && target && target->operation == i &&
                target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL &&
                operand->role == XR_SEM_OPERAND_CALLEE && operand->parameter == -1 &&
                (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0 &&
                operand->type == source->result_type && value == operand->value;
            if (!use_is_exact ||
                (target_by_value[value] != XR_SEMANTIC_INDEX_NONE &&
                 target_by_value[value] != target->function) ||
                use_count[value] == UINT32_MAX) {
                invalid[value] = 1;
                continue;
            }
            target_by_value[value] = target->function;
            use_count[value]++;
        }
    }
    for (uint32_t i = 0; i < (uint32_t) xr_semantic_plan_block_count(plan->semantic_plan); i++) {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(plan->semantic_plan, i);
        if (!block || block->control_value == XR_SEMANTIC_INDEX_NONE ||
            block->control_value >= value_count)
            continue;
        uint32_t source_index = definition[block->control_value];
        const XrSemanticOperationRecord *source =
            source_index == XR_SEMANTIC_INDEX_NONE
                ? NULL
                : xr_semantic_plan_operation(plan->semantic_plan, source_index);
        if (source && source->opcode == XI_GET_SHARED)
            invalid[block->control_value] = 1;
    }
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan->semantic_plan, i);
        if (!operation || operation->opcode != XI_GET_SHARED ||
            operation->result_value >= value_count ||
            target_by_value[operation->result_value] == XR_SEMANTIC_INDEX_NONE)
            continue;
        uint32_t value = operation->result_value;
        if (invalid[value] || use_count[value] == 0 ||
            !xr_semantic_direct_local_callee_type_is_exact(plan->semantic_plan, operation,
                                                           target_by_value[value]))
            goto invalid_authority;
        exact[value] = 1;
    }
    xr_free(definition);
    xr_free(target_by_operation);
    xr_free(use_count);
    xr_free(invalid);
    *out_exact = exact;
    *out_targets = target_by_value;
    return true;

invalid_authority:
    xr_free(definition);
    xr_free(target_by_operation);
    xr_free(target_by_value);
    xr_free(use_count);
    xr_free(invalid);
    xr_free(exact);
    return report(error, error_size, "XR_TARGET_1001",
                  "direct-local callee storage authority is not exact");
}

/* Shared slots live in one module-wide table owned by the module root, so a
 * slot index means the same slot in every function that names it. This table is
 * indexed by that index alone; the storing function is recorded so the owner
 * judgement can require it to be the callee's lexical parent. */
typedef struct XrVerifierGoStore {
    uint32_t function;
    uint32_t operation;
    uint8_t ambiguous;
} XrVerifierGoStore;

static XrVerifierGoStore *verifier_go_find_store(XrVerifierGoStore *stores, uint32_t slot_count,
                                                 int64_t slot) {
    if (!stores || slot < 0 || slot >= (int64_t) slot_count)
        return NULL;
    return &stores[slot];
}

static bool verifier_go_store_before_activation(const XrSemanticPlan *semantic,
                                                uint32_t function_index, uint32_t store_index) {
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(semantic, function_index);
    const XrSemanticOperationRecord *store = xr_semantic_plan_operation(semantic, store_index);
    const XrSemanticBlockRecord *entry =
        function ? xr_semantic_plan_block(semantic, function->block_begin) : NULL;
    if (!function || !store || !entry || entry->function != function_index ||
        store->block != function->block_begin || store_index < entry->operation_begin ||
        store_index >= entry->operation_begin + entry->operation_count)
        return false;
    for (uint32_t i = entry->operation_begin; i < store_index; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL ||
            operation->opcode == XI_CALL_METHOD || operation->opcode == XI_CALL_METHOD_DIRECT ||
            operation->opcode == XI_CALL_BUILTIN || operation->opcode == XI_GO ||
            operation->opcode == XI_THREAD_SPAWN)
            return false;
    }
    return true;
}

/* Independent verifier reconstruction for the GO-only shared callable family.
 * It deliberately does not call the builder collector or its predicates. */
static bool collect_exact_direct_local_go_callee_values(const XrTargetPlan *plan,
                                                        uint8_t **out_exact, uint32_t **out_targets,
                                                        char *error, size_t error_size) {
    if (out_exact)
        *out_exact = NULL;
    if (out_targets)
        *out_targets = NULL;
    const XrSemanticPlan *semantic = plan ? plan->semantic_plan : NULL;
    size_t operations_size = xr_semantic_plan_operation_count(semantic);
    size_t functions_size = xr_semantic_plan_function_count(semantic);
    if (!semantic || !out_exact || !out_targets || operations_size > UINT32_MAX ||
        functions_size > UINT32_MAX || operations_size > (1u << 24))
        return report(error, error_size, "XR_EXEC_5003",
                      "direct-local go callee verifier budget is exhausted");
    uint32_t value_count = 0;
    if (functions_size) {
        const XrSemanticFunctionRecord *last =
            xr_semantic_plan_function(semantic, (uint32_t) functions_size - 1u);
        if (!last || last->value_begin > UINT32_MAX - last->value_count)
            return report(error, error_size, "XR_EXEC_5003",
                          "direct-local go callee value budget overflow");
        value_count = last->value_begin + last->value_count;
    }
    uint32_t operation_count = (uint32_t) operations_size;
    uint32_t slot_count = 0;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (!operation ||
            (operation->opcode != XI_SET_SHARED && operation->opcode != XI_GET_SHARED))
            continue;
        if (operation->semantic_immediate < 0 || operation->semantic_immediate > UINT16_MAX)
            continue;
        uint32_t slot = (uint32_t) operation->semantic_immediate;
        if (slot + 1u > slot_count)
            slot_count = slot + 1u;
    }
    uint32_t *definition =
        value_count ? (uint32_t *) xr_malloc((size_t) value_count * sizeof(*definition)) : NULL;
    XrVerifierGoStore *stores =
        slot_count ? (XrVerifierGoStore *) xr_calloc(slot_count, sizeof(*stores)) : NULL;
    uint32_t *targets =
        value_count ? (uint32_t *) xr_malloc((size_t) value_count * sizeof(*targets)) : NULL;
    uint32_t *uses = value_count ? (uint32_t *) xr_calloc(value_count, sizeof(*uses)) : NULL;
    uint8_t *candidate = value_count ? (uint8_t *) xr_calloc(value_count, 1) : NULL;
    uint8_t *invalid = value_count ? (uint8_t *) xr_calloc(value_count, 1) : NULL;
    uint8_t *exact = value_count ? (uint8_t *) xr_calloc(value_count, 1) : NULL;
    if ((slot_count && !stores) ||
        (value_count && (!definition || !targets || !uses || !candidate || !invalid || !exact)))
        goto allocation_failed;
    for (uint32_t i = 0; i < value_count; i++) {
        definition[i] = XR_SEMANTIC_INDEX_NONE;
        targets[i] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < slot_count; i++)
        stores[i].operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->result_value >= value_count ||
            definition[operation->result_value] != XR_SEMANTIC_INDEX_NONE)
            goto invalid_authority;
        definition[operation->result_value] = i;
        if (operation->opcode == XI_SET_SHARED && operation->semantic_immediate >= 0 &&
            operation->semantic_immediate <= UINT16_MAX) {
            XrVerifierGoStore *entry =
                verifier_go_find_store(stores, slot_count, operation->semantic_immediate);
            if (!entry)
                goto invalid_authority;
            if (entry->operation != XR_SEMANTIC_INDEX_NONE)
                entry->ambiguous = 1;
            else {
                entry->operation = i;
                entry->function = operation->function;
            }
        }
    }
    XrSemanticGraph graph = {0};
    if (!xr_semantic_graph_build(semantic, &graph, error, error_size))
        goto invalid_authority;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(semantic, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin) {
            xr_semantic_graph_dispose(&graph);
            goto invalid_authority;
        }
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand = &operands[use->operand_begin + a];
            if (operand->value >= value_count) {
                xr_semantic_graph_dispose(&graph);
                goto invalid_authority;
            }
            const XrSemanticOperationRecord *load =
                definition[operand->value] < operation_count
                    ? xr_semantic_plan_operation(semantic, definition[operand->value])
                    : NULL;
            if (!load || load->opcode != XI_GET_SHARED || use->opcode != XI_GO)
                continue;
            uint32_t value = load->result_value;
            candidate[value] = 1;
            XrVerifierGoStore *entry =
                verifier_go_find_store(stores, slot_count, load->semantic_immediate);
            const XrSemanticOperationRecord *store =
                entry && !entry->ambiguous && entry->operation < operation_count
                    ? xr_semantic_plan_operation(semantic, entry->operation)
                    : NULL;
            const XrSemanticOperandRecord *stored =
                store && store->operand_count == 1 && store->operand_begin < operand_count
                    ? &operands[store->operand_begin]
                    : NULL;
            const XrSemanticOperationRecord *closure =
                stored && stored->value < value_count && definition[stored->value] < operation_count
                    ? xr_semantic_plan_operation(semantic, definition[stored->value])
                    : NULL;
            uint32_t target = closure ? closure->callable_function : XR_SEMANTIC_INDEX_NONE;
            const XrSemanticFunctionRecord *callee = xr_semantic_plan_function(semantic, target);
            /* Only a store the loading function makes itself stands in a
             * dominance relation to the load. A store in the slot's owning
             * scope is proved instead by the owner and entry-prefix judgements
             * below: it is the callee's lexical parent, and it ran before any
             * operation that could activate the loading function. */
            bool initialized =
                store && (store->function != load->function ||
                          (store->block == load->block
                               ? entry->operation < definition[value]
                               : xr_semantic_graph_dominates(&graph, store->block, load->block)));
            bool exact_use =
                entry && !entry->ambiguous && store && stored && closure && callee && a == 0 &&
                initialized &&
                verifier_go_store_before_activation(semantic, store->function, entry->operation) &&
                store->opcode == XI_SET_SHARED && callee->parent == store->function &&
                store->semantic_immediate == load->semantic_immediate && !store->allocation_key &&
                stable_id_is_zero(store->allocation_id) &&
                store->constant == XR_SEMANTIC_INDEX_NONE &&
                store->callable_function == XR_SEMANTIC_INDEX_NONE &&
                store->effects == xi_generated_op_effects(XI_SET_SHARED) &&
                store->result_ownership == xi_generated_op_result_ownership(XI_SET_SHARED) &&
                stored->role == XR_SEM_OPERAND_VALUE && stored->parameter == -1 &&
                stored->transfer_mode == XR_TRANSFER_SHARE &&
                stored->ownership_action == XR_SEM_OPERAND_CONSUME &&
                stored->parameter_mode == XR_PARAM_READ && stored->access == XR_CALL_ARG_PLAIN &&
                stored->origin == XI_PLACE_ORIGIN_NONE &&
                stored->lifetime == XI_PLACE_LIFETIME_NONE &&
                stored->escape == XI_PLACE_ESCAPE_NONE && stored->flags == 0 &&
                closure->function == store->function &&
                semantic_heap_closure_is_exact(semantic, closure) &&
                use->function == load->function &&
                use->operand_count == (uint16_t) (callee->parameter_count + 1u) &&
                !use->allocation_key && stable_id_is_zero(use->allocation_id) &&
                use->constant == XR_SEMANTIC_INDEX_NONE &&
                use->callable_function == XR_SEMANTIC_INDEX_NONE &&
                use->effects == xi_generated_op_effects(XI_GO) && operand->value == value &&
                operand->type == load->result_type && operand->role == XR_SEM_OPERAND_VALUE &&
                operand->parameter == -1 && operand->transfer_mode == XR_TRANSFER_SHARE &&
                operand->ownership_action == XR_SEM_OPERAND_BORROW &&
                operand->parameter_mode == XR_PARAM_READ && operand->access == XR_CALL_ARG_PLAIN &&
                operand->origin == XI_PLACE_ORIGIN_NONE &&
                operand->lifetime == XI_PLACE_LIFETIME_NONE &&
                operand->escape == XI_PLACE_ESCAPE_NONE && operand->flags == 0 &&
                xr_semantic_direct_local_callee_type_is_exact(semantic, load, target);
            for (uint16_t argument = 1; exact_use && argument < use->operand_count; argument++) {
                const XrSemanticOperandRecord *row = &operands[use->operand_begin + argument];
                const XrSemanticParameterRecord *parameter =
                    xr_semantic_plan_parameter(semantic, callee->parameter_begin + argument - 1u);
                exact_use = parameter && row->type == parameter->type &&
                            row->role == XR_SEM_OPERAND_VALUE && row->parameter == -1 &&
                            row->parameter_mode == XR_PARAM_READ &&
                            row->access == XR_CALL_ARG_PLAIN &&
                            row->origin == XI_PLACE_ORIGIN_NONE &&
                            row->lifetime == XI_PLACE_LIFETIME_NONE &&
                            row->escape == XI_PLACE_ESCAPE_NONE && row->flags == 0;
            }
            if (!exact_use ||
                (targets[value] != XR_SEMANTIC_INDEX_NONE && targets[value] != target) ||
                uses[value] == UINT32_MAX) {
                invalid[value] = 1;
                continue;
            }
            targets[value] = target;
            uses[value]++;
        }
    }
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(semantic, i);
        for (uint16_t a = 0; use && a < use->operand_count; a++) {
            uint32_t value = operands[use->operand_begin + a].value;
            if (value < value_count && candidate[value] && (use->opcode != XI_GO || a != 0))
                invalid[value] = 1;
        }
    }
    for (uint32_t i = 0; i < (uint32_t) xr_semantic_plan_block_count(semantic); i++) {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(semantic, i);
        if (block && block->control_value < value_count && candidate[block->control_value])
            invalid[block->control_value] = 1;
    }
    xr_semantic_graph_dispose(&graph);
    for (uint32_t i = 0; i < value_count; i++) {
        if (!candidate[i])
            continue;
        if (invalid[i] || uses[i] == 0 || targets[i] == XR_SEMANTIC_INDEX_NONE)
            goto invalid_authority;
        exact[i] = 1;
    }
    xr_free(definition);
    xr_free(stores);
    xr_free(uses);
    xr_free(candidate);
    xr_free(invalid);
    *out_exact = exact;
    *out_targets = targets;
    return true;

allocation_failed:
    xr_free(definition);
    xr_free(stores);
    xr_free(targets);
    xr_free(uses);
    xr_free(candidate);
    xr_free(invalid);
    xr_free(exact);
    return report(error, error_size, "XR_EXEC_5003",
                  "direct-local go callee verifier allocation failed");
invalid_authority:
    xr_free(definition);
    xr_free(stores);
    xr_free(targets);
    xr_free(uses);
    xr_free(candidate);
    xr_free(invalid);
    xr_free(exact);
    return report(error, error_size, "XR_TARGET_1001",
                  "direct-local go callee storage authority is not exact");
}

static bool verifier_source_namespace_operation_is_exact(const XrSemanticPlan *semantic,
                                                         const XrSemanticOperationRecord *operation,
                                                         uint16_t opcode) {
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    return semantic && operation && type && operation->opcode == opcode &&
           !operation->allocation_key && stable_id_is_zero(operation->allocation_id) &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->auxiliary_kind == 0 &&
           operation->effects == xi_generated_op_effects(opcode) &&
           operation->flags == xi_generated_op_default_flags(opcode) &&
           operation->ownership_use == xi_generated_op_own_use(opcode) &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
           operation->result_alias_operand == -1 &&
           operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
           operation->return_parameter == -1 && operation->return_complete == 1 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           ((opcode == XI_IMPORT_REF && operation->operand_count == 0 &&
             operation->semantic_immediate >= -1 && operation->semantic_immediate <= UINT16_MAX &&
             operation->metadata_count == 2) ||
            (opcode == XI_GET_SHARED && operation->operand_count == 0 &&
             operation->semantic_immediate >= 0 && operation->semantic_immediate <= UINT16_MAX &&
             operation->metadata_count == 0));
}

static bool verifier_source_import_dependency_is_exact(const XrSemanticPlan *semantic,
                                                       const XrSemanticOperationRecord *operation,
                                                       const char *const *metadata,
                                                       uint32_t metadata_count, bool named_export,
                                                       uint32_t *out_dependency) {
    if (!semantic || !operation || !metadata || !out_dependency ||
        !verifier_source_namespace_operation_is_exact(semantic, operation, XI_IMPORT_REF) ||
        operation->function != 0 ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE ||
        operation->metadata_begin + 1u >= metadata_count)
        return false;
    const char *module_path = metadata[operation->metadata_begin];
    const char *member = metadata[operation->metadata_begin + 1u];
    if (!module_path || !module_path[0] || !member || ((member[0] != '\0') != named_export))
        return false;
    uint32_t match = XR_SEMANTIC_INDEX_NONE;
    uint32_t dependency_count = (uint32_t) xr_semantic_plan_dependency_count(semantic);
    for (uint32_t i = 0; i < dependency_count; i++) {
        const XrSemanticDependencyRecord *dependency = xr_semantic_plan_dependency(semantic, i);
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

static bool verifier_source_namespace_identity_copy_is_exact(
    const XrSemanticPlan *semantic, const XrSemanticOperationRecord *operation,
    const XrSemanticOperandRecord *operands, uint32_t operand_count) {
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
    if (!semantic || !operation || !operands || !type || operation->opcode != XI_COPY ||
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

static bool collect_exact_source_namespace_values(const XrTargetPlan *plan, uint8_t **out_exact,
                                                  char *error, size_t error_size) {
    if (out_exact)
        *out_exact = NULL;
    const XrSemanticPlan *semantic = plan ? plan->semantic_plan : NULL;
    size_t function_size = xr_semantic_plan_function_count(semantic);
    size_t operation_size = xr_semantic_plan_operation_count(semantic);
    size_t target_size = xr_semantic_plan_call_target_count(semantic);
    if (!out_exact || function_size > UINT32_MAX || operation_size > UINT32_MAX ||
        target_size > UINT32_MAX)
        return report(error, error_size, "XR_EXEC_5003",
                      "source namespace verifier budget is exhausted");
    uint32_t value_count = 0;
    if (function_size) {
        const XrSemanticFunctionRecord *last =
            xr_semantic_plan_function(semantic, (uint32_t) function_size - 1u);
        if (!last || last->value_begin > UINT32_MAX - last->value_count)
            return report(error, error_size, "XR_EXEC_5003",
                          "source namespace value budget overflow");
        value_count = last->value_begin + last->value_count;
    }
    uint32_t operation_count = (uint32_t) operation_size;
    uint32_t *definition =
        value_count ? (uint32_t *) xr_calloc(value_count, sizeof(*definition)) : NULL;
    uint32_t *target_by_operation =
        operation_count ? (uint32_t *) xr_calloc(operation_count, sizeof(*target_by_operation))
                        : NULL;
    uint32_t *dependency =
        value_count ? (uint32_t *) xr_calloc(value_count, sizeof(*dependency)) : NULL;
    uint32_t *expected_uses =
        value_count ? (uint32_t *) xr_calloc(value_count, sizeof(*expected_uses)) : NULL;
    uint32_t *retain_uses =
        value_count ? (uint32_t *) xr_calloc(value_count, sizeof(*retain_uses)) : NULL;
    uint32_t *consumer =
        value_count ? (uint32_t *) xr_calloc(value_count, sizeof(*consumer)) : NULL;
    uint32_t *visit_epoch =
        value_count ? (uint32_t *) xr_calloc(value_count, sizeof(*visit_epoch)) : NULL;
    uint8_t *candidate =
        value_count ? (uint8_t *) xr_calloc(value_count, sizeof(*candidate)) : NULL;
    uint8_t *standalone_import =
        value_count ? (uint8_t *) xr_calloc(value_count, sizeof(*standalone_import)) : NULL;
    uint8_t *exact = value_count ? (uint8_t *) xr_calloc(value_count, sizeof(*exact)) : NULL;
    if ((value_count &&
         (!definition || !dependency || !expected_uses || !retain_uses || !consumer ||
          !visit_epoch || !candidate || !standalone_import || !exact)) ||
        (operation_count && !target_by_operation))
        goto allocation_failed;
    for (uint32_t i = 0; i < value_count; i++) {
        definition[i] = XR_SEMANTIC_INDEX_NONE;
        dependency[i] = XR_SEMANTIC_INDEX_NONE;
        consumer[i] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t i = 0; i < operation_count; i++) {
        target_by_operation[i] = XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->result_value >= value_count ||
            definition[operation->result_value] != XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        definition[operation->result_value] = i;
    }
    for (uint32_t i = 0; i < (uint32_t) target_size; i++) {
        const XrSemanticCallTargetRecord *target = xr_semantic_plan_call_target(semantic, i);
        if (!target || target->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT)
            continue;
        const XrSemanticOperationRecord *call =
            target->operation < operation_count
                ? xr_semantic_plan_operation(semantic, target->operation)
                : NULL;
        if (target->operation >= operation_count ||
            target->dependency >= xr_semantic_plan_dependency_count(semantic) ||
            target_by_operation[target->operation] != XR_SEMANTIC_INDEX_NONE)
            goto invalid;
        target_by_operation[target->operation] = i;
    }
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    uint32_t next_epoch = 1;
    for (uint32_t i = 0; i < operation_count; i++) {
        uint32_t target_index = target_by_operation[i];
        if (target_index == XR_SEMANTIC_INDEX_NONE)
            continue;
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(semantic, target_index);
        const XrSemanticOperationRecord *call = xr_semantic_plan_operation(semantic, i);
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
            memset(visit_epoch, 0, value_count * sizeof(*visit_epoch));
            epoch = next_epoch++;
        }
        for (uint32_t depth = 0;; depth++) {
            if (depth >= operation_count || current_value >= value_count ||
                visit_epoch[current_value] == epoch)
                goto invalid;
            visit_epoch[current_value] = epoch;
            uint32_t definition_index = definition[current_value];
            const XrSemanticOperationRecord *source =
                definition_index != XR_SEMANTIC_INDEX_NONE
                    ? xr_semantic_plan_operation(semantic, definition_index)
                    : NULL;
            if (!source || source->result_value != current_value ||
                source->result_type != namespace_type || source->function != call->function ||
                (candidate[current_value] && (dependency[current_value] != target->dependency ||
                                              consumer[current_value] != consumer_index)))
                goto invalid;
            candidate[current_value] = 1;
            dependency[current_value] = target->dependency;
            consumer[current_value] = consumer_index;
            if (verifier_source_namespace_operation_is_exact(semantic, source, XI_GET_SHARED)) {
                load = source;
                break;
            }
            if (!verifier_source_namespace_identity_copy_is_exact(semantic, source, operands,
                                                                  operand_count))
                goto invalid;
            const XrSemanticOperandRecord *input = &operands[source->operand_begin];
            consumer_index = definition_index;
            current_value = input->value;
        }
        uint32_t store_index = XR_SEMANTIC_INDEX_NONE;
        for (uint32_t j = 0; j < operation_count; j++) {
            const XrSemanticOperationRecord *store = xr_semantic_plan_operation(semantic, j);
            if (!store || store->function != 0 || store->opcode != XI_SET_SHARED ||
                store->semantic_immediate != load->semantic_immediate)
                continue;
            if (store_index != XR_SEMANTIC_INDEX_NONE)
                goto invalid;
            store_index = j;
        }
        const XrSemanticOperationRecord *store =
            store_index != XR_SEMANTIC_INDEX_NONE
                ? xr_semantic_plan_operation(semantic, store_index)
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
            memset(visit_epoch, 0, value_count * sizeof(*visit_epoch));
            epoch = next_epoch++;
        }
        for (uint32_t depth = 0;; depth++) {
            if (depth >= operation_count || current_value >= value_count ||
                visit_epoch[current_value] == epoch)
                goto invalid;
            visit_epoch[current_value] = epoch;
            uint32_t definition_index = definition[current_value];
            const XrSemanticOperationRecord *source =
                definition_index != XR_SEMANTIC_INDEX_NONE
                    ? xr_semantic_plan_operation(semantic, definition_index)
                    : NULL;
            if (!source || source->result_value != current_value ||
                source->result_type != namespace_type || source->function != 0 ||
                (candidate[current_value] && (dependency[current_value] != target->dependency ||
                                              consumer[current_value] != consumer_index)))
                goto invalid;
            candidate[current_value] = 1;
            dependency[current_value] = target->dependency;
            consumer[current_value] = consumer_index;
            if (verifier_source_namespace_operation_is_exact(semantic, source, XI_IMPORT_REF)) {
                import = source;
                break;
            }
            if (!verifier_source_namespace_identity_copy_is_exact(semantic, source, operands,
                                                                  operand_count))
                goto invalid;
            const XrSemanticOperandRecord *input = &operands[source->operand_begin];
            consumer_index = definition_index;
            current_value = input->value;
        }
        uint32_t import_dependency = XR_SEMANTIC_INDEX_NONE;
        if (!import || !load || receiver->type != load->result_type || import->function != 0 ||
            load->function != call->function || store->function != 0 ||
            store->semantic_immediate != load->semantic_immediate ||
            stored->type != import->result_type || stored->role != XR_SEM_OPERAND_VALUE ||
            stored->parameter != -1 || stored->ownership_action != XR_SEM_OPERAND_CONSUME ||
            stored->parameter_mode != XR_PARAM_READ || stored->access != XR_CALL_ARG_PLAIN ||
            stored->flags != 0 || load->result_type != import->result_type ||
            !verifier_source_import_dependency_is_exact(semantic, import, metadata, metadata_count,
                                                        named_export, &import_dependency) ||
            import_dependency != target->dependency)
            goto invalid;
    }
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *import = xr_semantic_plan_operation(semantic, i);
        if (!import || import->opcode != XI_IMPORT_REF ||
            import->import_resolution != XR_SEM_IMPORT_RESOLUTION_SOURCE_MODULE ||
            import->metadata_count != 2 || import->metadata_begin + 1u >= metadata_count ||
            !metadata || !metadata[import->metadata_begin + 1u] ||
            metadata[import->metadata_begin + 1u][0] != '\0')
            continue;
        if (import->result_value >= value_count)
            goto invalid;
        if (candidate[import->result_value])
            continue;
        uint32_t import_dependency = XR_SEMANTIC_INDEX_NONE;
        if (!verifier_source_import_dependency_is_exact(semantic, import, metadata, metadata_count,
                                                        false, &import_dependency))
            goto invalid;
        uint32_t store_index = XR_SEMANTIC_INDEX_NONE;
        int64_t shared_slot = -1;
        for (uint32_t j = 0; j < operation_count; j++) {
            const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, j);
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
            const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, j);
            if (!operation ||
                ((operation->opcode == XI_GET_SHARED || operation->opcode == XI_SET_SHARED) &&
                 operation->semantic_immediate == shared_slot && j != store_index))
                goto invalid;
        }
        candidate[import->result_value] = 1;
        standalone_import[import->result_value] = 1;
        dependency[import->result_value] = import_dependency;
        consumer[import->result_value] = store_index;
    }
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(semantic, i);
        if (!use || use->operand_begin > operand_count ||
            use->operand_count > operand_count - use->operand_begin)
            goto invalid;
        for (uint16_t a = 0; a < use->operand_count; a++) {
            const XrSemanticOperandRecord *operand = &operands[use->operand_begin + a];
            if (operand->value >= value_count || !candidate[operand->value])
                continue;
            const XrSemanticOperationRecord *source =
                xr_semantic_plan_operation(semantic, definition[operand->value]);
            bool expected = i == consumer[operand->value] && a == 0;
            if (expected && (use->opcode == XI_CALL || use->opcode == XI_CALL_METHOD))
                expected =
                    target_by_operation[i] != XR_SEMANTIC_INDEX_NONE &&
                    xr_semantic_plan_call_target(semantic, target_by_operation[i])->dependency ==
                        dependency[operand->value] &&
                    operand->role ==
                        (use->opcode == XI_CALL ? XR_SEM_OPERAND_CALLEE : XR_SEM_OPERAND_RECEIVER);
            else if (expected)
                expected = (use->opcode == XI_COPY || use->opcode == XI_SET_SHARED) &&
                           operand->role == XR_SEM_OPERAND_VALUE;
            if (expected) {
                if (expected_uses[operand->value] != 0)
                    goto invalid;
                expected_uses[operand->value] = 1;
                continue;
            }
            bool retain = source && source->opcode == XI_IMPORT_REF && use->opcode == XI_RETAIN &&
                          a == 0 && use->function == source->function &&
                          operand->role == XR_SEM_OPERAND_VALUE &&
                          operand->type == source->result_type && operand->parameter == -1 &&
                          operand->flags == 0;
            if (!retain || retain_uses[operand->value] != 0)
                goto invalid;
            retain_uses[operand->value] = 1;
        }
    }
    uint32_t block_count = (uint32_t) xr_semantic_plan_block_count(semantic);
    for (uint32_t i = 0; i < block_count; i++) {
        const XrSemanticBlockRecord *block = xr_semantic_plan_block(semantic, i);
        if (!block || (block->control_value != XR_SEMANTIC_INDEX_NONE &&
                       (block->control_value >= value_count || candidate[block->control_value])))
            goto invalid;
    }
    for (uint32_t i = 0; i < value_count; i++) {
        if (!candidate[i])
            continue;
        const XrSemanticOperationRecord *source =
            xr_semantic_plan_operation(semantic, definition[i]);
        if (!source || expected_uses[i] != 1 || (standalone_import[i] && retain_uses[i] != 1))
            goto invalid;
        exact[i] = 1;
    }
    xr_free(definition);
    xr_free(target_by_operation);
    xr_free(dependency);
    xr_free(expected_uses);
    xr_free(retain_uses);
    xr_free(consumer);
    xr_free(visit_epoch);
    xr_free(candidate);
    xr_free(standalone_import);
    *out_exact = exact;
    return true;

allocation_failed:
    xr_free(definition);
    xr_free(target_by_operation);
    xr_free(dependency);
    xr_free(expected_uses);
    xr_free(retain_uses);
    xr_free(consumer);
    xr_free(visit_epoch);
    xr_free(candidate);
    xr_free(standalone_import);
    xr_free(exact);
    return report(error, error_size, "XR_EXEC_5003", "source namespace verifier allocation failed");
invalid:
    xr_free(definition);
    xr_free(target_by_operation);
    xr_free(dependency);
    xr_free(expected_uses);
    xr_free(retain_uses);
    xr_free(consumer);
    xr_free(visit_epoch);
    xr_free(candidate);
    xr_free(standalone_import);
    xr_free(exact);
    return report(error, error_size, "XR_TARGET_1001",
                  "source namespace storage authority is not exact");
}

static bool
imported_source_class_instance_storage_is_exact_verify(const XrTargetPlan *plan,
                                                       uint32_t semantic_operation,
                                                       const XrSemanticOperationRecord *operation);

static bool rune_to_uint32_type_is_exact_target_verify(const XrSemanticTypeRecord *type,
                                                       uint16_t kind, uint8_t scalar_rep,
                                                       const char *canonical_key) {
    XrStableId zero = {{0}};
    return type && type->kind == kind && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) &&
           xr_stable_id_equal(type->source_enum_identity, zero) && !type->source_enum_key &&
           type->child_count == 0 && type->scalar_rep == scalar_rep &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 && type->enum_layout_id == 0 &&
           type->enum_member_count == 0 && type->enum_flags == 0 && type->reserved_enum == 0 &&
           type->flags == 0 && type->canonical_key &&
           strcmp(type->canonical_key, canonical_key) == 0;
}

/* The Target verifier reconstructs the frozen semantic relation itself. It
 * intentionally does
 * not call the builder's complete shape predicate. */
static bool operation_is_exact_rune_to_uint32_verify(const XrSemanticPlan *plan,
                                                     const XrSemanticOperationRecord *operation,
                                                     uint32_t *receiver_value) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || !operands || !metadata ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_RUNE_TO_UINT32 ||
        operation->opcode != XI_CALL_METHOD ||
        operation->semantic_immediate != (int64_t) XI_METHOD_SYMBOL_TO_UINT32 << 1 ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "toUInt32") != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        (operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) &&
         operation->flags != (xi_generated_op_default_flags(XI_CALL_METHOD) | XI_FLAG_TAIL)) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->return_provenance != XR_SEM_RETURN_NONE ||
        operation->return_complete != 0 || operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_NONE || operation->view_capability != 0 ||
        operation->view_lifetime != 0 || operation->view_complete != 0 ||
        operation->reserved_view[0] != 0 || operation->reserved_view[1] != 0)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    if (!rune_to_uint32_type_is_exact_target_verify(xr_semantic_plan_type(plan, receiver->type),
                                                    XR_KIND_RUNE, XR_SCALAR_REP_NONE,
                                                    "type-v3:24:0:0:0:0:0:0:0:0:255:0:") ||
        !rune_to_uint32_type_is_exact_target_verify(
            xr_semantic_plan_type(plan, operation->result_type), XR_KIND_INT, XR_NATIVE_U32,
            "type-v3:0:0:0:0:0:0:0:0:0:8:0:") ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW || receiver->parameter_mode != 0 ||
        receiver->access != 0 || receiver->origin != 0 || receiver->lifetime != 0 ||
        receiver->escape != 0 || receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return false;
    if (receiver_value)
        *receiver_value = receiver->value;
    return true;
}

static bool collect_exact_dynamic_types(
    const XrTargetPlan *plan, const uint8_t *exact_direct_callees, const uint8_t *exact_go_callees,
    const uint8_t *exact_channel_values, const uint8_t *exact_source_namespaces,
    const uint8_t *exact_native_module_namespaces, uint8_t **out, char *error, size_t error_size) {
    if (out)
        *out = NULL;
    size_t type_count = xr_semantic_plan_type_count(plan->semantic_plan);
    size_t operation_count = xr_semantic_plan_operation_count(plan->semantic_plan);
    uint32_t semantic_operand_count = 0;
    const XrSemanticOperandRecord *semantic_operands =
        xr_semantic_plan_operands(plan->semantic_plan, &semantic_operand_count);
    if (!out || type_count > UINT32_MAX || operation_count > UINT32_MAX)
        return report(error, error_size, "XR_EXEC_5003",
                      "dynamic-type verification budget is exhausted");
    uint8_t *exact_types =
        type_count ? (uint8_t *) xr_calloc(type_count, sizeof(*exact_types)) : NULL;
    if (type_count && !exact_types)
        return report(error, error_size, "XR_EXEC_5003",
                      "dynamic-type verification allocation failed");
    /* The nullable scalar family is driven by the semantic type rather than by
     * one producing operation, so its types are marked from the type table: a
     * nullable parameter has no operation whose result type would name it. */
    for (uint32_t i = 0; i < (uint32_t) type_count; i++)
        if (semantic_nullable_scalar_type_is_exact(xr_semantic_plan_type(plan->semantic_plan, i)))
            exact_types[i] = 1;
    for (uint32_t i = 0; i < (uint32_t) operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan->semantic_plan, i);
        if (!operation || operation->result_type >= type_count) {
            xr_free(exact_types);
            return report(error, error_size, "XR_TARGET_1001",
                          "dynamic-type verification input is invalid");
        }
        bool exact_array_member_result = false;
        uint8_t array_hof_kind = XR_TARGET_ARRAY_HOF_NONE;
        bool exact_array_hof_result =
            semantic_array_hof_is_exact_verify(plan->semantic_plan, operation, &array_hof_kind,
                                               NULL, NULL, NULL, NULL, NULL) &&
            array_hof_kind != XR_TARGET_ARRAY_HOF_REDUCE;
        if (!operation_is_exact_array_member_scalar(plan->semantic_plan, operation, NULL, NULL,
                                                    &exact_array_member_result))
            exact_array_member_result = false;
        if (semantic_heap_closure_is_exact(plan->semantic_plan, operation) ||
            xr_semantic_panic_catch_is_exact(plan->semantic_plan, operation) ||
            semantic_array_allocation_is_exact(plan->semantic_plan, operation) ||
            semantic_array_intrinsic_is_exact_verify(plan->semantic_plan, operation, NULL, NULL) ||
            exact_array_hof_result ||
            xr_semantic_container_copy_is_exact(plan->semantic_plan, operation, NULL, NULL) ||
            semantic_array_fill_scalar_is_exact_verify(plan->semantic_plan, operation, NULL, NULL,
                                                       NULL) ||
            semantic_string_shared_read_is_exact_verify(
                plan->semantic_plan, operation, operation->result_value, operation->result_type,
                operation->function) ||
            semantic_array_shared_read_is_exact_verify(
                plan->semantic_plan, operation, operation->result_value, operation->result_type,
                operation->function) ||
            semantic_direct_local_tagged_ref_place_load_is_exact_verify(
                plan, operation, operation->result_value, operation->result_type,
                operation->function) ||
            /* A transfer carries a dynamic value onward, so its result type is
             * exact for the same reason its source's was. */
            xr_semantic_owner_forward_is_exact(plan->semantic_plan, operation, NULL) ||
            xr_semantic_class_object_is_exact(plan->semantic_plan, operation) ||
            xr_semantic_class_instance_value_is_exact(plan->semantic_plan, operation, NULL) ||
            (operation->opcode == XI_CALL &&
             imported_source_class_instance_storage_is_exact_verify(plan, i, operation)) ||
            xr_semantic_string_literal_is_exact(plan->semantic_plan, operation) ||
            xr_semantic_bigint_value_is_exact(plan->semantic_plan, operation) ||
            xr_semantic_string_concat_is_exact(plan->semantic_plan, operation) ||
            xr_semantic_string_convert_is_exact(plan->semantic_plan, operation) ||
            semantic_direct_local_string_result_is_exact(plan->semantic_plan, i) ||
            semantic_direct_local_array_result_is_exact_verify(plan->semantic_plan, i) ||
            semantic_direct_local_adt_enum_result_is_exact(plan->semantic_plan, i) ||
            xr_semantic_adt_enum_constructor_is_exact(plan->semantic_plan, operation, NULL) ||
            semantic_stringbuilder_constructor_is_exact(plan->semantic_plan, operation) ||
            /* The method families own their result type in their own right;
             * relying on the constructor to have marked StringBuilder would
             * leave toString's String result unproven. */
            operation_is_exact_stringbuilder_append_rune(plan->semantic_plan, operation, NULL,
                                                         NULL) ||
            operation_is_exact_stringbuilder_to_string(plan->semantic_plan, operation, NULL) ||
            operation_is_exact_stringbuilder_append_string(plan->semantic_plan, operation, NULL) ||
            xr_semantic_string_runes_is_exact(plan->semantic_plan, operation, NULL) ||
            xr_semantic_iterator_rune_has_next_is_exact(plan->semantic_plan, operation, NULL) ||
            xr_semantic_iterator_rune_next_is_exact(plan->semantic_plan, operation, NULL) ||
            xr_semantic_iterator_rune_nth_is_exact(plan->semantic_plan, operation, NULL, NULL) ||
            semantic_map_entries_iterator_is_exact_verify(plan->semantic_plan, operation, NULL,
                                                          NULL) ||
            semantic_map_entry_iterator_has_next_is_exact_verify(plan->semantic_plan, operation,
                                                                 NULL) ||
            semantic_map_entry_iterator_next_is_exact_verify(plan->semantic_plan, operation,
                                                             NULL) ||
            operation_is_exact_rune_to_uint32_verify(plan->semantic_plan, operation, NULL) ||
            xr_semantic_rune_to_string_is_exact(plan->semantic_plan, operation, NULL) ||
            xr_semantic_rune_is_whitespace_is_exact(plan->semantic_plan, operation, NULL) ||
            xr_semantic_string_slice_range_is_exact(plan->semantic_plan, operation, NULL, NULL,
                                                    NULL) ||
            operation_is_exact_json_namespace_value(plan->semantic_plan, operation, NULL) ||
            xr_semantic_panic_info_constructor_is_exact(plan->semantic_plan, operation, NULL) ||
            xr_semantic_dynamic_value_is_exact(plan->semantic_plan, operation) ||
            exact_array_member_result ||
            (exact_direct_callees && exact_direct_callees[operation->result_value] != 0) ||
            (exact_go_callees && exact_go_callees[operation->result_value] != 0) ||
            (operation->opcode == XI_GO && operation->operand_count != 0 && semantic_operands &&
             operation->operand_begin < semantic_operand_count && exact_go_callees &&
             exact_go_callees[semantic_operands[operation->operand_begin].value] != 0 &&
             xr_semantic_direct_local_go_task_result_is_exact(plan->semantic_plan, operation, true,
                                                              NULL)) ||
            (exact_channel_values && exact_channel_values[operation->result_value] != 0) ||
            (exact_source_namespaces && exact_source_namespaces[operation->result_value] != 0) ||
            (exact_native_module_namespaces &&
             exact_native_module_namespaces[operation->result_value] != 0))
            exact_types[operation->result_type] = 1;
    }
    /* A class instance crossing a parameter boundary is the one dynamic value in
     * the plan that no operation result names: it is bound on entry rather than
     * computed, so its type reaches this collector through the parameter table
     * or not at all. */
    size_t parameter_count = xr_semantic_plan_parameter_count(plan->semantic_plan);
    for (uint32_t i = 0; i < (uint32_t) parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan->semantic_plan, i);
        uint8_t array_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        if (!parameter || parameter->type >= type_count ||
            (xr_semantic_class_instance_parameter_source_class(plan->semantic_plan, i) ==
                 XR_SEMANTIC_INDEX_NONE &&
             !xr_semantic_adt_enum_type_is_exact(
                 xr_semantic_plan_type(plan->semantic_plan, parameter->type)) &&
             !semantic_direct_local_tagged_ref_parameter_is_exact_verify(
                 plan->semantic_plan, parameter, &array_storage) &&
             !semantic_direct_local_array_value_parameter_is_exact_verify(
                 plan->semantic_plan, parameter, &array_storage)))
            continue;
        exact_types[parameter->type] = 1;
    }
    *out = exact_types;
    return true;
}

static bool
imported_source_class_instance_storage_is_exact_verify(const XrTargetPlan *plan,
                                                       uint32_t semantic_operation,
                                                       const XrSemanticOperationRecord *operation) {
    const XrSemanticPlan *semantic = plan ? plan->semantic_plan : NULL;
    const XrSemanticCallTargetRecord *target = NULL;
    uint32_t target_count = (uint32_t) xr_semantic_plan_call_target_count(semantic);
    for (uint32_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *candidate = xr_semantic_plan_call_target(semantic, i);
        if (!candidate || candidate->operation != semantic_operation ||
            candidate->kind != XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR ||
            candidate->dependency == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (target)
            return false;
        target = candidate;
    }
    const XrSemanticPlan *dependency =
        target && target->dependency < plan->semantic_dependency_count
            ? plan->semantic_dependencies[target->dependency]
            : NULL;
    const XrSemanticDependencyRecord *dependency_record =
        target && target->dependency < xr_semantic_plan_dependency_count(semantic)
            ? xr_semantic_plan_dependency(semantic, target->dependency)
            : NULL;
    const XrSemanticSourceExportRecord *source_export =
        dependency && target->source_export < xr_semantic_plan_source_export_count(dependency)
            ? xr_semantic_plan_source_export(dependency, target->source_export)
            : NULL;
    uint32_t constructor = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_class = target ? xr_semantic_imported_class_construction_authority_source_class(
                                         semantic, dependency, dependency_record, source_export,
                                         operation, &constructor)
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

static bool
verify_value_binding(const XrTargetPlan *plan, uint32_t semantic_value, uint32_t semantic_type,
                     uint32_t semantic_function, const XrSemanticOperationRecord *operation,
                     uint32_t operation_index, const uint8_t *exact_direct_callees,
                     const uint8_t *exact_go_callees, const uint8_t *exact_channel_values,
                     const uint8_t *exact_channel_receives, const uint8_t *exact_source_namespaces,
                     const uint8_t *exact_native_module_namespaces, uint8_t *bound_slots,
                     const uint8_t *deferred_functions, uint32_t *failure_reason) {
#define XR_VALUE_BINDING_FAIL(reason)                                                              \
    do {                                                                                           \
        if (failure_reason)                                                                        \
            *failure_reason = (reason);                                                            \
        return false;                                                                              \
    } while (0)
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan->semantic_plan, semantic_type);
    uint16_t expected_kind = XR_MACHINE_REP_COUNT;
    bool generated_result_void =
        operation && operation->opcode < XI_OP_COUNT &&
        xi_generated_op_result_kind(operation->opcode) == XI_GEN_RESULT_VOID;
    bool operation_result_void =
        xr_semantic_operation_result_void_governs_storage(
            plan->semantic_plan, operation, semantic_value, semantic_type, semantic_function) &&
        operation->effects == xi_generated_op_effects(operation->opcode) &&
        operation->result_ownership == xi_generated_op_result_ownership(operation->opcode);
    /* A unit enum reaches the table's result-void opcode legitimately and its
     * own family binds the ordinal, so it is not the inconsistency this catches. */
    if (generated_result_void && !operation_result_void &&
        !xr_semantic_unit_enum_type_is_exact(type))
        XR_VALUE_BINDING_FAIL(1);
    bool exact_heap_closure = semantic_heap_closure_is_exact(plan->semantic_plan, operation);
    bool exact_panic_catch = xr_semantic_panic_catch_is_exact(plan->semantic_plan, operation) &&
                             operation->result_value == semantic_value &&
                             operation->result_type == semantic_type &&
                             operation->function == semantic_function;
    bool exact_local_address =
        xr_semantic_local_addr_is_exact(plan->semantic_plan, operation, NULL) ||
        semantic_direct_local_scalar_ref_address_is_exact_verify(plan->semantic_plan, operation);
    bool exact_dynamic_value = xr_semantic_dynamic_value_is_exact(plan->semantic_plan, operation) &&
                               operation->result_value == semantic_value &&
                               operation->result_type == semantic_type &&
                               operation->function == semantic_function;
    bool exact_array_allocation =
        semantic_array_allocation_is_exact(plan->semantic_plan, operation);
    bool exact_array_intrinsic =
        semantic_array_intrinsic_is_exact_verify(plan->semantic_plan, operation, NULL, NULL);
    uint8_t array_hof_kind = XR_TARGET_ARRAY_HOF_NONE;
    bool exact_array_hof_result =
        semantic_array_hof_is_exact_verify(plan->semantic_plan, operation, &array_hof_kind, NULL,
                                           NULL, NULL, NULL, NULL) &&
        array_hof_kind != XR_TARGET_ARRAY_HOF_REDUCE;
    bool exact_container_copy =
        xr_semantic_container_copy_is_exact(plan->semantic_plan, operation, NULL, NULL) &&
        operation->result_value == semantic_value && operation->result_type == semantic_type &&
        operation->function == semantic_function;
    bool exact_array_fill = semantic_array_fill_scalar_is_exact_verify(plan->semantic_plan,
                                                                       operation, NULL, NULL, NULL);
    /* A String read back out of its shared cell. It borrows the cell's
     * allocation, so it is bound to the same borrowed tagged carrier an Array
     * read of that cell gets. */
    bool exact_string_shared_read = semantic_string_shared_read_is_exact_verify(
        plan->semantic_plan, operation, semantic_value, semantic_type, semantic_function);
    bool exact_array_shared_read = semantic_array_shared_read_is_exact_verify(
        plan->semantic_plan, operation, semantic_value, semantic_type, semantic_function);
    bool exact_tagged_ref_place_load = semantic_direct_local_tagged_ref_place_load_is_exact_verify(
        plan, operation, semantic_value, semantic_type, semantic_function);
    bool exact_string_literal = xr_semantic_string_literal_is_exact(plan->semantic_plan, operation);
    bool exact_bigint_value = xr_semantic_bigint_value_is_exact(plan->semantic_plan, operation);
    /* Recomputed from the plan through the shared judgement rather than read
     * back from the builder: the String a concatenation allocates is a fresh
     * owner whose only storage fact is the outer tagged value. */
    bool exact_string_concat = xr_semantic_string_concat_is_exact(plan->semantic_plan, operation) &&
                               operation && operation->result_value == semantic_value &&
                               operation->result_type == semantic_type;
    /* Rebuilt the same way and for the same reason: the String a `string(x)`
     * conversion allocates is a fresh owner whose only storage fact is the
     * outer tagged value. */
    bool exact_string_convert =
        xr_semantic_string_convert_is_exact(plan->semantic_plan, operation) && operation &&
        operation->result_value == semantic_value && operation->result_type == semantic_type;
    /* Recomputed from the plan through the shared judgement rather than read
     * back from the builder, so a builder row this verifier cannot re-derive
     * stays unproven. */
    bool exact_class_object = xr_semantic_class_object_is_exact(plan->semantic_plan, operation) &&
                              operation && operation->result_value == semantic_value &&
                              operation->result_type == semantic_type;
    /* Recomputed the same way, and for the same reason: the construction, the
     * class object read it dispatches on and the instance reads that follow it
     * all carry an outer tagged value, and whether that value owns its
     * allocation is the operation's own result ownership. */
    bool exact_class_instance =
        (xr_semantic_class_instance_value_is_exact(plan->semantic_plan, operation, NULL) ||
         (operation && operation->opcode == XI_CALL &&
          imported_source_class_instance_storage_is_exact_verify(plan, operation_index,
                                                                 operation))) &&
        operation && operation->result_value == semantic_value &&
        operation->result_type == semantic_type;
    bool exact_class_instance_borrowed =
        exact_class_instance && operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED;
    bool exact_direct_string_result =
        semantic_direct_local_string_result_is_exact(plan->semantic_plan, operation_index) &&
        operation && operation->result_value == semantic_value &&
        operation->result_type == semantic_type;
    bool exact_stringbuilder =
        semantic_stringbuilder_constructor_is_exact(plan->semantic_plan, operation);
    /* StringBuilder methods bind a dynamic result in their own right. Append
     * preserves the receiver's ownership, while toString creates a new owner. */
    bool exact_stringbuilder_append =
        operation_is_exact_stringbuilder_append_rune(plan->semantic_plan, operation, NULL, NULL);
    bool exact_stringbuilder_to_string =
        operation_is_exact_stringbuilder_to_string(plan->semantic_plan, operation, NULL);
    bool exact_stringbuilder_append_string =
        operation_is_exact_stringbuilder_append_string(plan->semantic_plan, operation, NULL);
    /* A copy that only renames its operand carries the source's binding, so the
     * result having one is expected rather than unexplained. Same statement of
     * the shape the builder used to give it that binding. */
    uint32_t owner_forward_source = XR_SEMANTIC_INDEX_NONE;
    bool exact_owner_forward =
        xr_semantic_owner_forward_is_exact(plan->semantic_plan, operation, &owner_forward_source);
    uint32_t identity_copy_source = XR_SEMANTIC_INDEX_NONE;
    bool exact_identity_copy =
        xr_semantic_identity_copy_is_exact(plan->semantic_plan, operation, &identity_copy_source);
    bool exact_string_runes =
        xr_semantic_string_runes_is_exact(plan->semantic_plan, operation, NULL);
    bool exact_map_entries_iterator =
        semantic_map_entries_iterator_is_exact_verify(plan->semantic_plan, operation, NULL, NULL);
    bool exact_map_entry_iterator_next =
        semantic_map_entry_iterator_next_is_exact_verify(plan->semantic_plan, operation, NULL);
    bool exact_string_slice_range =
        xr_semantic_string_slice_range_is_exact(plan->semantic_plan, operation, NULL, NULL, NULL);
    bool exact_rune_to_string =
        xr_semantic_rune_to_string_is_exact(plan->semantic_plan, operation, NULL) && operation &&
        operation->result_value == semantic_value && operation->result_type == semantic_type &&
        operation->function == semantic_function;
    bool exact_json_namespace_value =
        operation_is_exact_json_namespace_value(plan->semantic_plan, operation, NULL);
    bool exact_panic_info_constructor =
        xr_semantic_panic_info_constructor_is_exact(plan->semantic_plan, operation, NULL);
    /* An array member that hands back its receiver binds a dynamic result of
     * its own, exactly like the StringBuilder members that return `self`. */
    bool exact_array_member_result = false;
    if (!operation_is_exact_array_member_scalar(plan->semantic_plan, operation, NULL, NULL,
                                                &exact_array_member_result))
        exact_array_member_result = false;
    bool exact_direct_callee = exact_direct_callees && exact_direct_callees[semantic_value] != 0;
    bool exact_go_callee = exact_go_callees && exact_go_callees[semantic_value] != 0;
    uint32_t semantic_operand_count = 0;
    const XrSemanticOperandRecord *semantic_operands =
        xr_semantic_plan_operands(plan->semantic_plan, &semantic_operand_count);
    uint32_t go_task_callee = XR_SEMANTIC_INDEX_NONE;
    bool exact_go_task = operation && operation->opcode == XI_GO && operation->operand_count != 0 &&
                         operation->operand_begin < semantic_operand_count && semantic_operands &&
                         (go_task_callee = semantic_operands[operation->operand_begin].value) !=
                             XR_SEMANTIC_INDEX_NONE &&
                         exact_go_callees && exact_go_callees[go_task_callee] != 0 &&
                         xr_semantic_direct_local_go_task_result_is_exact(plan->semantic_plan,
                                                                          operation, true, NULL);
    bool exact_channel = exact_channel_values && exact_channel_values[semantic_value] != 0;
    bool exact_channel_receive =
        exact_channel_receives && exact_channel_receives[semantic_value] != 0;
    bool exact_source_namespace =
        exact_source_namespaces && exact_source_namespaces[semantic_value] != 0;
    bool exact_native_module_namespace =
        exact_native_module_namespaces && exact_native_module_namespaces[semantic_value] != 0;
    bool exact_string_byte_view =
        semantic_string_byte_slice_view_is_exact(plan->semantic_plan, operation);
    /* The view a range slice produces takes the same VIEW pair, re-proved from
     * the one judgement the builder published it from. */
    bool exact_range_slice_view =
        operation && operation->result_value == semantic_value &&
        operation->result_type == semantic_type && operation->function == semantic_function &&
        xr_semantic_range_slice_is_exact(plan->semantic_plan, operation, NULL);
    const XrSemanticParameterRecord *parameter =
        operation
            ? NULL
            : semantic_parameter_for_value(plan->semantic_plan, semantic_function, semantic_value);
    bool exact_adt_enum =
        xr_semantic_adt_enum_type_is_exact(type) &&
        ((parameter && parameter->type == semantic_type &&
          parameter->function == semantic_function &&
          (parameter->ownership == XI_OWN_NONE || parameter->ownership == XI_OWN_OWNED ||
           parameter->ownership == XI_OWN_BORROWED) &&
          parameter->mode == XR_PARAM_READ && parameter->transfer_mode == XR_TRANSFER_SHARE) ||
         (operation && operation->result_value == semantic_value &&
          operation->result_type == semantic_type &&
          (xr_semantic_adt_enum_constructor_is_exact(plan->semantic_plan, operation, NULL) ||
           semantic_direct_local_adt_enum_result_is_exact(plan->semantic_plan, operation_index))));
    bool exact_adt_enum_borrowed =
        exact_adt_enum && parameter && parameter->ownership == XI_OWN_BORROWED;
    bool exact_string_byte_parameter =
        semantic_u8_slice_parameter_is_exact(plan->semantic_plan, parameter) &&
        parameter->type == semantic_type;
    uint8_t exact_tagged_ref_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    bool exact_tagged_ref_parameter =
        semantic_direct_local_tagged_ref_parameter_is_exact_verify(plan->semantic_plan, parameter,
                                                                   &exact_tagged_ref_storage) &&
        parameter->type == semantic_type;
    /* An Array handed over by value borrows the caller's allocation for the
     * extent of the call, so it is bound to the same borrowed tagged carrier a
     * shared read of that array gets -- not to the pointer a ref parameter
     * needs. */
    uint8_t exact_array_value_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    bool exact_array_value_parameter =
        semantic_direct_local_array_value_parameter_is_exact_verify(plan->semantic_plan, parameter,
                                                                    &exact_array_value_storage) &&
        parameter->type == semantic_type;
    /* A String handed over by value. A String has no carrier other than the
     * outer tagged value, so it is bound to the same tagged binding an Array by
     * value gets; the declaration says whether the callee borrows the caller's
     * allocation or holds an owning reference it releases itself, and only the
     * ownership on the row differs between the two. */
    bool string_parameter_callee_owns = false;
    bool exact_string_value_parameter =
        xr_semantic_direct_local_string_value_parameter_is_exact(plan->semantic_plan, parameter,
                                                                 &string_parameter_callee_owns) &&
        parameter->type == semantic_type;
    /* Whether the callee owns the allocation is the parameter's own recorded
     * ownership, never a property of the family -- the same way a class
     * receiver and an ADT enum state theirs. */
    bool exact_string_value_parameter_borrowed =
        exact_string_value_parameter && !string_parameter_callee_owns;
    /* An Array a direct-local call returns is a transfer of the outer tagged
     * value, so it is bound exactly as an owned String result is. */
    bool exact_direct_array_result =
        operation && operation->result_value == semantic_value &&
        operation->result_type == semantic_type &&
        semantic_direct_local_array_result_is_exact_verify(plan->semantic_plan, operation_index);
    /* Recomputed through the shared judgement for the same reason the class
     * object and the instance are. A receiver is the one value in the family
     * whose declaration its type row cannot name, so the judgement reads it off
     * the member's own identity; a builder row this verifier cannot re-derive
     * from that identity, or from the class an ordinary parameter was declared
     * with, stays unproven. */
    uint32_t receiver_parameter =
        operation ? XR_SEMANTIC_INDEX_NONE
                  : xr_semantic_class_parameter_for_value(plan->semantic_plan, semantic_value);
    const XrSemanticParameterRecord *receiver_record =
        xr_semantic_plan_parameter(plan->semantic_plan, receiver_parameter);
    bool exact_class_receiver =
        xr_semantic_class_instance_parameter_source_class(
            plan->semantic_plan, receiver_parameter) != XR_SEMANTIC_INDEX_NONE &&
        receiver_record && receiver_record->type == semantic_type &&
        receiver_record->function == semantic_function;
    /* Whether the receiver owns its allocation is the parameter's own recorded
     * ownership, never a property of the family. */
    bool exact_class_receiver_borrowed =
        exact_class_receiver && receiver_record->ownership == XI_OWN_BORROWED;
    bool exact_unit_enum = xr_semantic_unit_enum_type_is_exact(type);
    bool exact_nullable_scalar = semantic_nullable_scalar_type_is_exact(type);
    uint16_t receive_scalar_kind = XR_MACHINE_REP_COUNT;
    bool scalar_channel_receive = operation && operation->opcode == XI_CHAN_TRY_RECV && type &&
                                  semantic_type_expected_rep(type, &receive_scalar_kind) == 1 &&
                                  receive_scalar_kind != XR_MACHINE_REP_VOID;
    if (scalar_channel_receive && !exact_channel_receive)
        XR_VALUE_BINDING_FAIL(10);
    int eligibility =
        operation_result_void || exact_heap_closure || exact_panic_catch || exact_dynamic_value ||
                exact_array_allocation || exact_array_intrinsic || exact_array_hof_result ||
                exact_container_copy || exact_array_fill || exact_string_shared_read ||
                exact_array_shared_read || exact_tagged_ref_place_load || exact_class_object ||
                exact_class_instance || exact_class_receiver || exact_string_literal ||
                exact_bigint_value || exact_string_concat || exact_string_convert ||
                exact_direct_string_result || exact_stringbuilder || exact_stringbuilder_append ||
                exact_stringbuilder_to_string || exact_stringbuilder_append_string ||
                exact_identity_copy || exact_owner_forward || exact_string_runes ||
                exact_map_entries_iterator || exact_map_entry_iterator_next ||
                exact_string_slice_range || exact_rune_to_string || exact_json_namespace_value ||
                exact_panic_info_constructor || exact_array_member_result || exact_direct_callee ||
                exact_go_callee || exact_go_task || exact_channel || exact_source_namespace ||
                exact_native_module_namespace || exact_string_byte_view || exact_range_slice_view ||
                exact_string_byte_parameter || exact_unit_enum || exact_nullable_scalar ||
                exact_adt_enum || exact_tagged_ref_parameter || exact_array_value_parameter ||
                exact_string_value_parameter || exact_direct_array_result
            ? 1
            : (type ? semantic_type_expected_rep(type, &expected_kind) : -1);
    /* The PSC4 leaf family owns its aggregate type and direct-call result
     * through public
     * program bindings.  Do not route that family through the
     * pre-PSC aggregate readers: a
     * corrupt or incomplete typed binding must
     * remain a hard refusal instead of silently
     * recovering an answer. */
    XrVerifyLeafProgramShape leaf_program = {0};
    bool leaf_family = semantic_is_leaf_program_family(plan->semantic_plan);
    bool leaf_shape_exact =
        leaf_family && verify_leaf_program_shape(plan->semantic_plan, &leaf_program);
    bool exact_leaf_aggregate_type = leaf_shape_exact && leaf_program.aggregate_binding &&
                                     leaf_program.aggregate_binding->semantic_type == semantic_type;
    bool exact_direct_aggregate_result = operation && operation->result_value == semantic_value &&
                                         operation->result_type == semantic_type &&
                                         exact_leaf_aggregate_type &&
                                         leaf_program.operation == operation &&
                                         leaf_program.call_binding->operation == operation_index;
    XrVerifyProductProgramShape product_program = {0};
    bool product_family = semantic_is_product_program_family(plan->semantic_plan);
    bool product_shape_exact =
        product_family && verify_product_program_shape(plan->semantic_plan, &product_program);
    bool exact_product_type = product_shape_exact && product_program.product &&
                              product_program.product->semantic_type == semantic_type;
    bool exact_direct_product_result = false;
    for (uint32_t caller = 0; operation && caller < 2; caller++)
        exact_direct_product_result =
            exact_direct_product_result ||
            (exact_product_type && product_program.calls[caller] == operation &&
             product_program.call_bindings[caller]->operation == operation_index &&
             operation->result_value == semantic_value);
    bool deferred_operation =
        deferred_functions[semantic_function] ||
        (!exact_direct_aggregate_result && !exact_direct_product_result && operation &&
         operation->opcode < XI_OP_COUNT &&
         (xi_generated_op_class(operation->opcode) == XI_GEN_CLASS_CALL ||
          xi_generated_op_class(operation->opcode) == XI_GEN_CLASS_COROUTINE));
    uint32_t aggregate_stack[64] = {0};
    int aggregate = 0;
    if (eligibility == 0 && !deferred_operation) {
        if (exact_leaf_aggregate_type || exact_product_type)
            aggregate = 1;
        else if (leaf_family && type && type->kind == XR_KIND_INSTANCE &&
                 (type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0)
            aggregate = -1;
        else
            aggregate = semantic_aggregate_eligibility(plan->semantic_plan, semantic_type,
                                                       aggregate_stack, 0);
    }
    int expected_layout = -1;
    if (exact_heap_closure || exact_panic_catch || exact_dynamic_value || exact_array_allocation ||
        exact_class_object || exact_array_intrinsic || exact_array_hof_result ||
        exact_container_copy || exact_string_shared_read || exact_array_shared_read ||
        exact_tagged_ref_place_load || exact_array_fill || exact_class_instance ||
        exact_class_receiver || exact_string_literal || exact_bigint_value || exact_string_concat ||
        exact_string_convert || exact_direct_string_result || exact_stringbuilder ||
        exact_stringbuilder_append || exact_stringbuilder_to_string ||
        exact_stringbuilder_append_string || exact_string_runes || exact_map_entries_iterator ||
        exact_map_entry_iterator_next || exact_string_slice_range || exact_rune_to_string ||
        exact_json_namespace_value || exact_panic_info_constructor || exact_array_member_result ||
        exact_direct_callee || exact_go_callee || exact_go_task || exact_channel ||
        exact_source_namespace || exact_native_module_namespace || exact_nullable_scalar ||
        exact_adt_enum || exact_array_value_parameter || exact_string_value_parameter ||
        exact_direct_array_result) {
        expected_kind = XR_MACHINE_REP_DYN_VALUE;
        expected_layout = target_plan_layout_for_type(plan, semantic_type);
        eligibility =
            expected_layout >= 0 && plan->layouts[expected_layout].kind == XR_TARGET_LAYOUT_DYNAMIC
                ? 1
                : -1;
    } else if (exact_owner_forward) {
        /* A transfer holds what its source held, so the expected kind is the
         * source's. An unclaimed source leaves it unclaimed rather than
         * ineligible, matching what the builder does. */
        const XrTargetValueRepRecord *forward_source =
            owner_forward_source != XR_SEMANTIC_INDEX_NONE
                ? xr_target_plan_value_rep(plan, owner_forward_source)
                : NULL;
        if (forward_source) {
            expected_kind = plan->machine_reps[forward_source->register_rep].kind;
            expected_layout = target_plan_layout_for_type(plan, semantic_type);
            eligibility = 1;
        } else {
            eligibility = 0;
        }
    } else if (exact_identity_copy) {
        /* A rename holds what its source holds, so the expected kind is the
         * source's rather than one derived from this value's type. A source
         * with no binding leaves the copy ineligible, which is the same
         * fail-closed answer the builder gives by not claiming it. */
        const XrTargetValueRepRecord *identity_source =
            identity_copy_source != XR_SEMANTIC_INDEX_NONE
                ? xr_target_plan_value_rep(plan, identity_copy_source)
                : NULL;
        if (identity_source) {
            expected_kind = plan->machine_reps[identity_source->register_rep].kind;
            expected_layout = target_plan_layout_for_type(plan, semantic_type);
            eligibility = 1;
        } else {
            /* Not ineligible, just unclaimed: a copy whose source no family
             * bound has nothing to inherit, and the builder declines to claim
             * it for the same reason. Requiring a binding here would demand one
             * the builder never produces. */
            eligibility = 0;
        }
    } else if (exact_tagged_ref_parameter || exact_local_address) {
        /* An address is a pointer whatever it points at, and the plan records
         * the subject's type on both sides of the operation because source has
         * no way to write "pointer to int". So the expected kind here cannot
         * come from the type -- asking it answers for the subject. */
        expected_kind = XR_MACHINE_REP_RAW_PTR;
        eligibility = 1;
    } else if (exact_string_byte_view || exact_string_byte_parameter || exact_range_slice_view) {
        expected_kind = XR_MACHINE_REP_VIEW;
        expected_layout = target_plan_layout_for_type(plan, semantic_type);
        eligibility =
            expected_layout >= 0 && plan->layouts[expected_layout].kind == XR_TARGET_LAYOUT_VIEW
                ? 1
                : -1;
    } else if (exact_unit_enum) {
        expected_kind = XR_MACHINE_REP_ENUM_ORDINAL;
        expected_layout = target_plan_layout_for_type(plan, semantic_type);
        eligibility =
            expected_layout >= 0 && plan->layouts[expected_layout].kind == XR_TARGET_LAYOUT_SCALAR
                ? 1
                : -1;
    } else if (eligibility == 0 && deferred_operation) {
        eligibility = 0;
    } else if (aggregate == 1) {
        expected_kind = XR_MACHINE_REP_AGGREGATE;
        expected_layout = target_plan_layout_for_type(plan, semantic_type);
        eligibility = expected_layout >= 0 ? 1 : -1;
    } else if (aggregate < 0) {
        eligibility = -1;
    } else if (aggregate == 0 && eligibility == 0) {
        eligibility = 0;
    }
    if (operation_result_void)
        expected_kind = XR_MACHINE_REP_VOID;
    const XrTargetValueRepRecord *record = xr_target_plan_value_rep(plan, semantic_value);
    if (eligibility < 0) {
        XR_VALUE_BINDING_FAIL(2);
    }
    if (eligibility == 0) {
        if (record != NULL)
            XR_VALUE_BINDING_FAIL(7);
        return true;
    }
    if (!record || plan->machine_reps[record->register_rep].kind != expected_kind ||
        plan->machine_reps[record->memory_rep].kind != expected_kind ||
        (expected_kind == XR_MACHINE_REP_AGGREGATE &&
         (plan->machine_reps[record->register_rep].detail != (uint32_t) expected_layout ||
          plan->machine_reps[record->memory_rep].detail != (uint32_t) expected_layout))) {
        XR_VALUE_BINDING_FAIL(3);
    }
    if (expected_kind == XR_MACHINE_REP_DYN_VALUE) {
        /* A nullable scalar carrier holds the null tag or a plain machine
         * scalar, so it claims no allocation, root map, or cleanup. */
        uint8_t expected_ownership =
            (exact_direct_callee || exact_go_callee || exact_go_task || exact_source_namespace ||
             exact_native_module_namespace || exact_nullable_scalar ||
             exact_class_instance_borrowed || exact_class_receiver_borrowed ||
             exact_adt_enum_borrowed || exact_string_shared_read || exact_array_shared_read ||
             exact_tagged_ref_place_load || exact_array_value_parameter ||
             exact_string_value_parameter_borrowed ||
             ((exact_stringbuilder_append || exact_stringbuilder_append_string) && operation &&
              operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED) ||
             (exact_dynamic_value && xr_semantic_dynamic_value_is_borrowed(operation)) ||
             (exact_channel && operation && operation->opcode == XI_COPY))
                ? XR_TARGET_OWNERSHIP_BORROWED
                : XR_TARGET_OWNERSHIP_OWNED;
        if (plan->machine_reps[record->register_rep].ownership != expected_ownership ||
            plan->machine_reps[record->memory_rep].ownership != expected_ownership ||
            plan->machine_reps[record->register_rep].root_kind != XR_TARGET_ROOT_DYNAMIC ||
            plan->machine_reps[record->memory_rep].root_kind != XR_TARGET_ROOT_DYNAMIC)
            XR_VALUE_BINDING_FAIL(9);
    }
    if (exact_tagged_ref_parameter &&
        (plan->machine_reps[record->register_rep].ownership != XR_TARGET_OWNERSHIP_BORROWED ||
         plan->machine_reps[record->memory_rep].ownership != XR_TARGET_OWNERSHIP_BORROWED ||
         plan->machine_reps[record->register_rep].root_kind != XR_TARGET_ROOT_NONE ||
         plan->machine_reps[record->memory_rep].root_kind != XR_TARGET_ROOT_NONE))
        XR_VALUE_BINDING_FAIL(13);
    if (expected_kind == XR_MACHINE_REP_RAW_PTR && !exact_tagged_ref_parameter &&
        (plan->machine_reps[record->register_rep].ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
         plan->machine_reps[record->memory_rep].ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
         plan->machine_reps[record->register_rep].root_kind != XR_TARGET_ROOT_NONE ||
         plan->machine_reps[record->memory_rep].root_kind != XR_TARGET_ROOT_NONE))
        XR_VALUE_BINDING_FAIL(14);
    if (expected_kind == XR_MACHINE_REP_VIEW &&
        (plan->machine_reps[record->register_rep].detail != semantic_type ||
         plan->machine_reps[record->memory_rep].detail != semantic_type ||
         plan->machine_reps[record->register_rep].root_kind != XR_TARGET_ROOT_VIEW_OWNER ||
         plan->machine_reps[record->memory_rep].root_kind != XR_TARGET_ROOT_VIEW_OWNER ||
         plan->machine_reps[record->register_rep].ownership != XR_TARGET_OWNERSHIP_BORROWED ||
         plan->machine_reps[record->memory_rep].ownership != XR_TARGET_OWNERSHIP_BORROWED))
        XR_VALUE_BINDING_FAIL(11);
    if (expected_kind == XR_MACHINE_REP_ENUM_ORDINAL &&
        (plan->machine_reps[record->register_rep].detail != semantic_type ||
         plan->machine_reps[record->memory_rep].detail != semantic_type ||
         plan->machine_reps[record->register_rep].root_kind != XR_TARGET_ROOT_NONE ||
         plan->machine_reps[record->memory_rep].root_kind != XR_TARGET_ROOT_NONE ||
         plan->machine_reps[record->register_rep].ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
         plan->machine_reps[record->memory_rep].ownership != XR_TARGET_OWNERSHIP_TRIVIAL))
        XR_VALUE_BINDING_FAIL(12);
    if (expected_kind == XR_MACHINE_REP_VOID) {
        if (record->slot != XR_SEMANTIC_INDEX_NONE)
            XR_VALUE_BINDING_FAIL(8);
        return true;
    }
    if (target_plan_layout_for_type(plan, semantic_type) < 0)
        XR_VALUE_BINDING_FAIL(4);
    /* A direct-local callee is compile-time resolution authority carried by
     * the call record. Its representation remains fingerprinted, but the
     * canonical TargetPlan must not invent a runtime frame slot for it. */
    if (exact_direct_callee) {
        if (record->slot != XR_SEMANTIC_INDEX_NONE)
            XR_VALUE_BINDING_FAIL(8);
        return true;
    }
    if (semantic_function >= plan->functions_count || record->slot >= plan->slots_count)
        XR_VALUE_BINDING_FAIL(5);
    const XrTargetFunctionRecord *target_function = &plan->functions[semantic_function];
    const XrTargetSlotRecord *slot = &plan->slots[record->slot];
    const XrTargetMachineRepRecord *memory = &plan->machine_reps[record->memory_rep];
    XrStableId expected_slot_identity;
    if (target_function->id != semantic_function ||
        target_function->semantic_function != semantic_function ||
        !range_valid(target_function->slot_begin, target_function->slot_count, plan->slots_count) ||
        record->slot < target_function->slot_begin ||
        record->slot >= target_function->slot_begin + target_function->slot_count ||
        bound_slots[record->slot] || slot->function != semantic_function ||
        !reconstruct_value_slot_identity(plan, slot, semantic_value, semantic_function,
                                         &expected_slot_identity) ||
        !xr_stable_id_equal(slot->identity, expected_slot_identity) ||
        slot->register_rep != record->register_rep || slot->memory_rep != record->memory_rep ||
        slot->size != memory->memory_size || slot->align != memory->memory_align ||
        slot->root_kind != memory->root_kind || slot->ownership != memory->ownership)
        XR_VALUE_BINDING_FAIL(6);
    bound_slots[record->slot] = 1;
#undef XR_VALUE_BINDING_FAIL
    return true;
}

static bool verify_value_reps(const XrTargetPlan *plan, const uint8_t *exact_direct_callees,
                              const uint8_t *exact_go_callees, const uint8_t *exact_channel_values,
                              const uint8_t *exact_channel_receives,
                              const uint8_t *exact_source_namespaces,
                              const uint8_t *exact_native_module_namespaces, char *error,
                              size_t error_size) {
    uint32_t expected_values = 0;
    size_t semantic_functions = xr_semantic_plan_function_count(plan->semantic_plan);
    if (semantic_functions > UINT32_MAX)
        return report(error, error_size, "XR_EXEC_5003", "semantic function index budget overflow");
    if (plan->functions_count != semantic_functions)
        return report(error, error_size, "XR_TARGET_1001",
                      "target functions do not cover semantic value ownership");
    for (uint32_t function_index = 0; function_index < (uint32_t) semantic_functions;
         function_index++) {
        const XrSemanticFunctionRecord *function =
            xr_semantic_plan_function(plan->semantic_plan, function_index);
        if (!function || function->value_begin != expected_values ||
            function->value_count > UINT32_MAX - expected_values)
            return report(error, error_size, "XR_TARGET_1001",
                          "semantic value ranges are not globally dense");
        expected_values += function->value_count;
    }
    if (expected_values > SIZE_MAX / sizeof(uint32_t))
        return report(error, error_size, "XR_EXEC_5003", "value representation budget overflow");
    uint32_t previous_value = 0;
    for (uint32_t index = 0; index < plan->value_reps_count; index++) {
        const XrTargetValueRepRecord *record = &plan->value_reps[index];
        if (record->semantic_value >= expected_values ||
            (index && record->semantic_value <= previous_value) ||
            record->register_rep >= plan->machine_reps_count ||
            record->memory_rep >= plan->machine_reps_count ||
            !machine_rep_allows_conversion(plan, record->register_rep, record->memory_rep))
            return report(error, error_size, "XR_TARGET_1001",
                          "value representation binding is unordered, invalid, or not convertible");
        previous_value = record->semantic_value;
    }
    uint8_t *defined = NULL;
    uint8_t *bound_slots = NULL;
    uint8_t *deferred_functions = NULL;
    if (expected_values) {
        defined = (uint8_t *) xr_calloc(expected_values, sizeof(*defined));
    }
    if (plan->slots_count)
        bound_slots = (uint8_t *) xr_calloc(plan->slots_count, sizeof(*bound_slots));
    if (semantic_functions)
        deferred_functions = (uint8_t *) xr_calloc(semantic_functions, sizeof(*deferred_functions));
    if ((expected_values && !defined) || (plan->slots_count && !bound_slots) ||
        (semantic_functions && !deferred_functions) ||
        !mark_coroutine_functions(plan->semantic_plan, deferred_functions,
                                  (uint32_t) semantic_functions)) {
        xr_free(defined);
        xr_free(bound_slots);
        xr_free(deferred_functions);
        return report(error, error_size, "XR_EXEC_5003",
                      "value representation verifier allocation failed");
    }
    bool valid = true;
    uint32_t failed_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t failed_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t failed_opcode = XI_OP_COUNT;
    uint32_t failure_reason = 0;
    uint32_t parameters = (uint32_t) xr_semantic_plan_parameter_count(plan->semantic_plan);
    for (uint32_t index = 0; valid && index < parameters; index++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan->semantic_plan, index);
        if (!parameter || parameter->value >= expected_values)
            valid = false;
        else if (!defined[parameter->value]) {
            defined[parameter->value] = 1;
            valid = verify_value_binding(
                plan, parameter->value, parameter->type, parameter->function, NULL,
                XR_SEMANTIC_INDEX_NONE, exact_direct_callees, exact_go_callees,
                exact_channel_values, exact_channel_receives, exact_source_namespaces,
                exact_native_module_namespaces, bound_slots, deferred_functions, &failure_reason);
            if (!valid) {
                failed_value = parameter->value;
                failed_type = parameter->type;
            }
        }
    }
    uint32_t operations = (uint32_t) xr_semantic_plan_operation_count(plan->semantic_plan);
    for (uint32_t index = 0; valid && index < operations; index++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan->semantic_plan, index);
        if (!operation)
            valid = false;
        else if (operation->result_value != XR_SEMANTIC_INDEX_NONE) {
            if (operation->result_value >= expected_values)
                valid = false;
            else if (!defined[operation->result_value]) {
                defined[operation->result_value] = 1;
                valid = verify_value_binding(
                    plan, operation->result_value, operation->result_type, operation->function,
                    operation, index, exact_direct_callees, exact_go_callees, exact_channel_values,
                    exact_channel_receives, exact_source_namespaces, exact_native_module_namespaces,
                    bound_slots, deferred_functions, &failure_reason);
                if (!valid) {
                    failed_value = operation->result_value;
                    failed_type = operation->result_type;
                    failed_opcode = operation->opcode;
                }
            }
        }
    }
    for (uint32_t value = 0; value < plan->value_reps_count; value++) {
        const XrTargetValueRepRecord *record = &plan->value_reps[value];
        if (!defined[record->semantic_value])
            valid = false;
    }
    for (uint32_t slot = 0; valid && slot < plan->slots_count; slot++)
        if (!bound_slots[slot])
            valid = false;
    xr_free(defined);
    xr_free(bound_slots);
    xr_free(deferred_functions);
    if (!valid) {
        const XrSemanticTypeRecord *failed_semantic_type =
            failed_type == XR_SEMANTIC_INDEX_NONE
                ? NULL
                : xr_semantic_plan_type(plan->semantic_plan, failed_type);
        if (error && error_size)
            snprintf(error, error_size,
                     "XR_TARGET_1001: value representation binding is incomplete, "
                     "incompatible, or unlocated "
                     "(value=%u type=%u:%s opcode=%u:%s reason=%u)",
                     failed_value, failed_type,
                     failed_semantic_type && failed_semantic_type->canonical_key
                         ? failed_semantic_type->canonical_key
                         : "<unknown>",
                     failed_opcode,
                     failed_opcode < XI_OP_COUNT ? xi_generated_op_name((XiOp) failed_opcode)
                                                 : "<parameter>",
                     failure_reason);
        return false;
    }
    return true;
}

static bool verify_extents(const XrTargetPlan *plan, char *error, size_t error_size) {
    for (uint32_t i = 0; i < plan->extents_count; i++) {
        const XrTargetExtentRecord *extent = &plan->extents[i];
        if (extent->id != i || extent->kind > XR_TARGET_EXTENT_PROVIDER_DEFINED ||
            (extent->alignment && !is_power_of_two(extent->alignment)) ||
            (extent->flags &
             ~(XR_TARGET_EXTENT_ZERO | XR_TARGET_EXTENT_ACCOUNT | XR_TARGET_EXTENT_CLONE |
               XR_TARGET_EXTENT_TEARDOWN | XR_TARGET_EXTENT_SIZED_DEALLOC)) != 0)
            return report(error, error_size, "XR_TARGET_1002", "extent record is invalid");
        if (extent->kind == XR_TARGET_EXTENT_FIXED) {
            if (extent->operand_count || extent->alignment || extent->stride || extent->provider ||
                extent->element_layout != XR_SEMANTIC_INDEX_NONE || extent->flags)
                return report(error, error_size, "XR_TARGET_1002",
                              "fixed extent carries variable-size facts");
            continue;
        }
        return report(error, error_size, "XR_TARGET_1002",
                      "variable extent lacks independently frozen semantic shape facts");
    }
    return true;
}

static bool verify_extent_references(const XrTargetPlan *plan, char *error, size_t error_size) {
    uint8_t *referenced = NULL;
    if (plan->extents_count) {
        referenced = (uint8_t *) xr_calloc(plan->extents_count, sizeof(*referenced));
        if (!referenced)
            return report(error, error_size, "XR_EXEC_5003",
                          "extent reference verifier allocation failed");
    }
    for (uint32_t i = 0; i < plan->layouts_count; i++)
        referenced[plan->layouts[i].extent] = 1;
    for (uint32_t i = 0; i < plan->extents_count; i++) {
        if (!referenced[i]) {
            xr_free(referenced);
            return report(error, error_size, "XR_TARGET_1002",
                          "extent table contains a row outside the layout reference domain");
        }
    }
    xr_free(referenced);
    return true;
}

static bool verify_aggregate_layout_acyclic(const XrTargetPlan *plan, uint32_t layout,
                                            uint8_t *states, uint32_t depth) {
    if (depth > 64 || states[layout] == 1)
        return false;
    if (states[layout] == 2)
        return true;
    states[layout] = 1;
    const XrTargetLayoutRecord *record = &plan->layouts[layout];
    for (uint32_t i = 0; i < record->field_count; i++) {
        const XrTargetFieldRecord *field = &plan->fields[record->field_begin + i];
        if (field->memory_rep >= plan->machine_reps_count)
            return false;
        const XrTargetMachineRepRecord *rep = &plan->machine_reps[field->memory_rep];
        if (rep->kind == XR_MACHINE_REP_AGGREGATE &&
            (rep->detail >= plan->layouts_count ||
             !verify_aggregate_layout_acyclic(plan, rep->detail, states, depth + 1u)))
            return false;
    }
    states[layout] = 2;
    return true;
}

static bool verify_layouts(const XrTargetPlan *plan, const uint8_t *exact_dynamic_types,
                           char *error, size_t error_size) {
    size_t semantic_types = xr_semantic_plan_type_count(plan->semantic_plan);
    uint32_t child_table_count = 0;
    const uint32_t *children =
        xr_semantic_plan_type_children(plan->semantic_plan, &child_table_count);
    uint32_t previous_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t next_field = 0;
    for (uint32_t i = 0; i < plan->layouts_count; i++) {
        const XrTargetLayoutRecord *layout = &plan->layouts[i];
        if (layout->id != i || layout->semantic_type >= semantic_types ||
            (previous_type != XR_SEMANTIC_INDEX_NONE && layout->semantic_type <= previous_type) ||
            (layout->kind != XR_TARGET_LAYOUT_SCALAR &&
             layout->kind != XR_TARGET_LAYOUT_AGGREGATE &&
             layout->kind != XR_TARGET_LAYOUT_DYNAMIC && layout->kind != XR_TARGET_LAYOUT_VIEW) ||
            layout->array_element_storage >= XR_TARGET_ARRAY_STORAGE_COUNT ||
            !is_power_of_two(layout->align) || layout->fixed_prefix_size % layout->align != 0 ||
            layout->extent >= plan->extents_count || layout->field_begin != next_field ||
            !range_valid(layout->field_begin, layout->field_count, plan->fields_count))
            return report(error, error_size, "XR_TARGET_1002", "layout record is invalid");
        const XrSemanticTypeRecord *semantic_type =
            xr_semantic_plan_type(plan->semantic_plan, layout->semantic_type);
        XrVerifyLeafProgramShape leaf_program = {0};
        bool leaf_family = semantic_is_leaf_program_family(plan->semantic_plan);
        bool leaf_shape_exact =
            !leaf_family || verify_leaf_program_shape(plan->semantic_plan, &leaf_program);
        bool program_leaf_aggregate =
            leaf_shape_exact && leaf_family && leaf_program.aggregate_binding &&
            leaf_program.aggregate_binding->semantic_type == layout->semantic_type;
        XrVerifyProductProgramShape product_program = {0};
        bool product_family = semantic_is_product_program_family(plan->semantic_plan);
        bool product_shape_exact =
            !product_family || verify_product_program_shape(plan->semantic_plan,
                                                            &product_program);
        bool program_leaf_product =
            product_shape_exact && product_family && product_program.product &&
            product_program.product->semantic_type == layout->semantic_type;
        if (!leaf_shape_exact || !product_shape_exact)
            return report(error, error_size, "XR_TARGET_1002",
                          "leaf program layout authority is incomplete");
        uint8_t expected_array_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        bool exact_array_layout = semantic_direct_local_array_type_is_exact_verify(
            plan->semantic_plan, layout->semantic_type, false, &expected_array_storage);
        if (layout->array_element_storage !=
            (exact_array_layout ? expected_array_storage : XR_TARGET_ARRAY_STORAGE_NONE))
            return report(error, error_size, "XR_TARGET_1002",
                          "layout element storage disagrees with its semantic type");
        uint16_t expected_rep = XR_MACHINE_REP_COUNT;
        int scalar = semantic_type ? semantic_type_expected_rep(semantic_type, &expected_rep) : -1;
        if (xr_semantic_unit_enum_type_is_exact(semantic_type)) {
            scalar = 1;
            expected_rep = XR_MACHINE_REP_ENUM_ORDINAL;
        }
        if (!semantic_type || !stable_id_is_zero(layout->destructor) ||
            !stable_id_is_zero(layout->clone) || !stable_id_is_zero(layout->equality_hash) ||
            plan->extents[layout->extent].kind != XR_TARGET_EXTENT_FIXED)
            return report(error, error_size, "XR_TARGET_1002",
                          "layout lacks an independently provable semantic contract");
        if (layout->kind == XR_TARGET_LAYOUT_SCALAR) {
            if (scalar != 1 || expected_rep == XR_MACHINE_REP_VOID || layout->field_count != 0 ||
                layout->root_field_count != 0)
                return report(error, error_size, "XR_TARGET_1002",
                              "scalar layout semantic contract is incomplete");
            bool physical_match = false;
            for (uint32_t r = 0; r < plan->machine_reps_count; r++)
                if (plan->machine_reps[r].kind == expected_rep &&
                    (expected_rep != XR_MACHINE_REP_ENUM_ORDINAL ||
                     plan->machine_reps[r].detail == layout->semantic_type) &&
                    plan->machine_reps[r].memory_size == layout->fixed_prefix_size &&
                    plan->machine_reps[r].memory_align == layout->align)
                    physical_match = true;
            if (!physical_match)
                return report(error, error_size, "XR_TARGET_1002",
                              "scalar layout disagrees with its canonical machine representation");
        } else if (layout->kind == XR_TARGET_LAYOUT_DYNAMIC) {
            bool exact_dynamic_type =
                exact_dynamic_types && exact_dynamic_types[layout->semantic_type] != 0;
            uint32_t representation_count = 0;
            for (uint32_t r = 0; r < plan->machine_reps_count; r++) {
                const XrTargetMachineRepRecord *rep = &plan->machine_reps[r];
                representation_count += rep->kind == XR_MACHINE_REP_DYN_VALUE &&
                                        rep->memory_size == layout->fixed_prefix_size &&
                                        rep->memory_align == layout->align;
            }
            if (scalar != 0 || !exact_dynamic_type || layout->field_count != 0 ||
                layout->root_field_count != 0 || representation_count == 0)
                return report(error, error_size, "XR_TARGET_1002",
                              "dynamic value layout semantic contract is incomplete");
        } else if (layout->kind == XR_TARGET_LAYOUT_VIEW) {
            uint32_t representation_count = 0;
            for (uint32_t r = 0; r < plan->machine_reps_count; r++) {
                const XrTargetMachineRepRecord *rep = &plan->machine_reps[r];
                representation_count +=
                    rep->kind == XR_MACHINE_REP_VIEW && rep->detail == layout->semantic_type &&
                    rep->memory_size == 16 && rep->memory_align == 8 && rep->register_bits == 128 &&
                    rep->root_kind == XR_TARGET_ROOT_VIEW_OWNER &&
                    rep->ownership == XR_TARGET_OWNERSHIP_BORROWED;
            }
            if (scalar != 0 || semantic_type->kind != XR_KIND_SLICE ||
                semantic_type->child_count != 1 || layout->field_count != 0 ||
                layout->root_field_count != 0 || layout->fixed_prefix_size != 16 ||
                layout->align != 8 || representation_count != 1)
                return report(error, error_size, "XR_TARGET_1002",
                              "view layout semantic contract is incomplete");
        } else {
            uint32_t expected_fields = semantic_type->aggregate_extent;
            if (scalar != 0 ||
                (!program_leaf_aggregate && !program_leaf_product &&
                 xr_semantic_aggregate_type_kind(semantic_type) != 1) ||
                semantic_type->child_begin > child_table_count ||
                semantic_type->child_count > child_table_count - semantic_type->child_begin ||
                semantic_type->aggregate_align > UINT16_MAX ||
                (semantic_type->kind == XR_KIND_FIXED_ARRAY &&
                 (semantic_type->child_count != 1 ||
                  !semantic_fixed_array_count(plan->semantic_plan, layout->semantic_type,
                                              &expected_fields))) ||
                (semantic_type->kind != XR_KIND_FIXED_ARRAY &&
                 semantic_type->aggregate_extent != semantic_type->child_count) ||
                layout->field_count != expected_fields)
                return report(error, error_size, "XR_TARGET_1002",
                              "aggregate layout semantic field facts are incomplete");
            uint32_t representation_count = 0;
            for (uint32_t r = 0; r < plan->machine_reps_count; r++)
                representation_count += plan->machine_reps[r].kind == XR_MACHINE_REP_AGGREGATE &&
                                        plan->machine_reps[r].detail == i;
            if (representation_count != 1)
                return report(error, error_size, "XR_TARGET_1002",
                              "aggregate layout has no unique machine representation");
        }
        previous_type = layout->semantic_type;
        uint32_t previous_end = 0;
        uint32_t expected_align = 1;
        uint32_t roots = 0;
        XrSemanticValueAggregateShape aggregate_shape = {0};
        bool named_value_aggregate =
            !program_leaf_aggregate && !program_leaf_product &&
            xr_semantic_value_aggregate_shape_for_type(plan->semantic_plan, layout->semantic_type,
                                                       &aggregate_shape);
        if (semantic_type->kind == XR_KIND_INSTANCE &&
            (semantic_type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0 &&
            !program_leaf_aggregate && !program_leaf_product &&
            !named_value_aggregate)
            return report(error, error_size, "XR_TARGET_1002",
                          "value aggregate field identity is incomplete");
        for (uint32_t f = 0; f < layout->field_count; f++) {
            const XrTargetFieldRecord *field = &plan->fields[layout->field_begin + f];
            uint32_t child_ordinal = semantic_type->kind == XR_KIND_FIXED_ARRAY ? 0u : f;
            uint32_t child_type = children[semantic_type->child_begin + child_ordinal];
            int child_layout_index = target_plan_layout_for_type(plan, child_type);
            uint32_t expected_offset = 0;
            uint32_t expected_name = named_value_aggregate
                                         ? aggregate_shape.field_metadata_begin + f
                                         : XR_SEMANTIC_INDEX_NONE;
            if (field->layout != i || !field->size || !is_power_of_two(field->align) ||
                field->semantic_field != f || field->semantic_name != expected_name ||
                child_layout_index < 0 ||
                !xr_checked_align_u32(previous_end, field->align, &expected_offset) ||
                field->offset != expected_offset || field->offset > layout->fixed_prefix_size ||
                field->size > layout->fixed_prefix_size - field->offset ||
                field->memory_rep >= plan->machine_reps_count ||
                field->root_kind > XR_TARGET_ROOT_VIEW_OWNER || field->flags != 0 ||
                field->reserved != 0)
                return report(error, error_size, "XR_TARGET_1002",
                              "field layout is misaligned, overlapping, or out of range");
            const XrTargetMachineRepRecord *rep = &plan->machine_reps[field->memory_rep];
            const XrTargetLayoutRecord *child_layout = &plan->layouts[child_layout_index];
            if (field->size != rep->memory_size || field->align != rep->memory_align ||
                field->root_kind != rep->root_kind ||
                field->size != child_layout->fixed_prefix_size ||
                field->align != child_layout->align ||
                (child_layout->kind == XR_TARGET_LAYOUT_AGGREGATE &&
                 (rep->kind != XR_MACHINE_REP_AGGREGATE ||
                  rep->detail != (uint32_t) child_layout_index)))
                return report(error, error_size, "XR_TARGET_1002",
                              "field representation disagrees with its layout");
            if (child_layout->kind == XR_TARGET_LAYOUT_SCALAR) {
                const XrSemanticTypeRecord *child_semantic =
                    xr_semantic_plan_type(plan->semantic_plan, child_type);
                uint16_t child_rep = XR_MACHINE_REP_COUNT;
                if (!child_semantic ||
                    semantic_type_expected_rep(child_semantic, &child_rep) != 1 ||
                    rep->kind != child_rep)
                    return report(error, error_size, "XR_TARGET_1002",
                                  "field scalar representation disagrees with SemanticPlan");
            }
            if (!checked_u32_add(field->offset, field->size, &previous_end))
                return report(error, error_size, "XR_TARGET_1002", "field layout offset overflows");
            roots += field->root_kind != XR_TARGET_ROOT_NONE;
            if (field->align > expected_align)
                expected_align = field->align;
        }
        uint32_t expected_size = previous_end ? previous_end : 1u;
        if (semantic_type->aggregate_align > expected_align)
            expected_align = semantic_type->aggregate_align;
        if (!xr_checked_align_u32(expected_size, expected_align, &expected_size) ||
            roots != layout->root_field_count ||
            (layout->kind == XR_TARGET_LAYOUT_AGGREGATE &&
             (layout->align != expected_align || layout->fixed_prefix_size != expected_size)))
            return report(error, error_size, "XR_TARGET_1002",
                          "layout padding, alignment, size, or root cardinality is inconsistent");
        XrFingerprint actual;
        xr_target_layout_compute_fingerprint(plan, i, &actual);
        if (!xr_fingerprint_equal(actual, layout->fingerprint))
            return report(error, error_size, "XR_TARGET_1002",
                          "layout fingerprint changed after freeze");
        next_field += layout->field_count;
    }
    if (next_field != plan->fields_count)
        return report(error, error_size, "XR_TARGET_1002",
                      "layout field ranges do not exactly partition the field table");
    uint8_t *states =
        plan->layouts_count ? (uint8_t *) xr_calloc(plan->layouts_count, sizeof(*states)) : NULL;
    if (plan->layouts_count && !states)
        return report(error, error_size, "XR_EXEC_5003",
                      "aggregate recursion verifier allocation failed");
    bool acyclic = true;
    for (uint32_t i = 0; acyclic && i < plan->layouts_count; i++)
        if (plan->layouts[i].kind == XR_TARGET_LAYOUT_AGGREGATE)
            acyclic = verify_aggregate_layout_acyclic(plan, i, states, 0);
    xr_free(states);
    if (!acyclic)
        return report(error, error_size, "XR_TARGET_1002",
                      "aggregate layout contains a recursive inline cycle");
    return true;
}

static bool verify_storage_and_allocations(const XrTargetPlan *plan, char *error,
                                           size_t error_size) {
    uint32_t next_operand = 0;
    for (uint32_t i = 0; i < plan->allocations_count; i++) {
        const XrTargetAllocationRecord *allocation = &plan->allocations[i];
        if (allocation->operand_begin != next_operand ||
            !range_valid(allocation->operand_begin, allocation->operand_count,
                         plan->extent_operands_count) ||
            !checked_u32_add(next_operand, allocation->operand_count, &next_operand))
            return report(error, error_size, "XR_TARGET_1002",
                          "allocation operand ranges do not exactly partition their table");
    }
    if (next_operand != plan->extent_operands_count || plan->storage_count ||
        plan->allocations_count || plan->extent_operands_count)
        return report(error, error_size, "XR_TARGET_1002",
                      "allocation tables require semantic allocation shape facts");
    return true;
}

static bool verify_functions_and_slots(const XrTargetPlan *plan, char *error, size_t error_size) {
    size_t semantic_functions = xr_semantic_plan_function_count(plan->semantic_plan);
    if (plan->functions_count != semantic_functions)
        return report(error, error_size, "XR_TARGET_1002",
                      "target function table does not cover the semantic plan");
    uint32_t next_slot = 0;
    uint32_t next_root = 0;
    uint32_t next_cleanup = 0;
    for (uint32_t i = 0; i < plan->functions_count; i++) {
        const XrTargetFunctionRecord *function = &plan->functions[i];
        if (function->id != i || function->semantic_function != i ||
            function->slot_begin != next_slot || function->root_begin != next_root ||
            function->cleanup_begin != next_cleanup || function->reserved != 0 ||
            !range_valid(function->slot_begin, function->slot_count, plan->slots_count) ||
            !range_valid(function->root_begin, function->root_count, plan->root_maps_count) ||
            !range_valid(function->cleanup_begin, function->cleanup_count, plan->cleanups_count))
            return report(error, error_size, "XR_TARGET_1002",
                          "target function table range is invalid");
        uint32_t previous_end = 0;
        uint32_t expected_frame_align = 1;
        for (uint32_t s = 0; s < function->slot_count; s++) {
            uint32_t slot_index = function->slot_begin + s;
            const XrTargetSlotRecord *slot = &plan->slots[slot_index];
            uint32_t slot_end = 0;
            uint32_t expected_offset = 0;
            if (slot->id != slot_index || slot->function != i ||
                stable_id_is_zero(slot->identity) || !slot->size || !is_power_of_two(slot->align) ||
                slot->offset % slot->align != 0 ||
                !xr_checked_align_u32(previous_end, slot->align, &expected_offset) ||
                slot->offset != expected_offset ||
                !checked_u32_add(slot->offset, slot->size, &slot_end) ||
                slot->register_rep >= plan->machine_reps_count ||
                slot->memory_rep >= plan->machine_reps_count ||
                slot->role <= XR_TARGET_SLOT_ROLE_INVALID ||
                slot->role >= XR_TARGET_SLOT_ROLE_COUNT ||
                slot->root_kind > XR_TARGET_ROOT_VIEW_OWNER ||
                slot->ownership > XR_TARGET_OWNERSHIP_SHARED || slot->reserved != 0 ||
                slot->debug_variable != XR_SEMANTIC_INDEX_NONE ||
                (s &&
                 xr_stable_id_compare(plan->slots[slot_index - 1u].identity, slot->identity) >= 0))
                return report(error, error_size, "XR_TARGET_1002",
                              "slot or its bounded debug reference is invalid");
            const XrTargetMachineRepRecord *memory = &plan->machine_reps[slot->memory_rep];
            if (slot->size != memory->memory_size || slot->align != memory->memory_align ||
                !machine_rep_allows_conversion(plan, slot->register_rep, slot->memory_rep) ||
                slot->root_kind != memory->root_kind || slot->ownership != memory->ownership)
                return report(error, error_size, "XR_TARGET_1002",
                              "slot disagrees with its memory representation");
            if (slot->align > expected_frame_align)
                expected_frame_align = slot->align;
            previous_end = slot_end;
        }
        uint32_t expected_frame_size = 0;
        if (!xr_checked_align_u32(previous_end, expected_frame_align, &expected_frame_size) ||
            function->frame_align != expected_frame_align ||
            function->frame_size != expected_frame_size)
            return report(error, error_size, "XR_TARGET_1002",
                          "function frame does not exactly pack its slot range");
        next_slot += function->slot_count;
        next_root += function->root_count;
        next_cleanup += function->cleanup_count;
    }
    if (next_slot != plan->slots_count || next_root != plan->root_maps_count ||
        next_cleanup != plan->cleanups_count)
        return report(error, error_size, "XR_TARGET_1002",
                      "target function ranges do not partition their tables");
    return true;
}

static bool reconstruct_call_identity(const char *domain, XrStableId first, XrStableId second,
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

static bool operation_is_call_shaped(const XrSemanticPlan *semantic,
                                     const XrSemanticOperationRecord *operation) {
    if (operation) {
        /* Keep the independent boundary on exact semantic op identities. */
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
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
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

static bool slot_binds_value_in_function(const XrTargetPlan *plan,
                                         const XrTargetValueRepRecord *value,
                                         uint32_t semantic_function) {
    if (!value)
        return false;
    if (plan->machine_reps[value->memory_rep].kind == XR_MACHINE_REP_VOID)
        return value->slot == XR_SEMANTIC_INDEX_NONE;
    if (value->slot >= plan->slots_count || semantic_function >= plan->functions_count)
        return false;
    const XrTargetSlotRecord *slot = &plan->slots[value->slot];
    const XrTargetFunctionRecord *function = &plan->functions[semantic_function];
    return slot->semantic_value == value->semantic_value && slot->function == semantic_function &&
           value->slot >= function->slot_begin &&
           value->slot < function->slot_begin + function->slot_count &&
           slot->register_rep == value->register_rep && slot->memory_rep == value->memory_rep;
}

/* Independent reconstruction of the sole non-static dispatch descriptor.
 * Keep this deliberately separate from
 * the builder and from every Xi/AOT method registry. */
static bool operation_is_exact_channel_close(const XrSemanticPlan *semantic,
                                             const XrSemanticOperationRecord *operation,
                                             uint32_t *receiver_type) {
    if (receiver_type)
        *receiver_type = XR_SEMANTIC_INDEX_NONE;
    if (!semantic || !operation)
        return false;
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
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
    const XrSemanticTypeRecord *receiver_record = xr_semantic_plan_type(semantic, receiver->type);
    const XrSemanticTypeRecord *result = xr_semantic_plan_type(semantic, operation->result_type);
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(semantic, operation->function);
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

static bool operation_is_exact_stringbuilder_to_string(const XrSemanticPlan *semantic,
                                                       const XrSemanticOperationRecord *operation,
                                                       uint32_t *receiver_value) {
    uint32_t operands_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operands_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_STRINGBUILDER_TO_STRING ||
        operation->opcode != XI_CALL_METHOD || operation->operand_count != 1 ||
        operation->operand_begin >= operands_count || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "toString") != 0 ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(semantic, receiver->type);
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(semantic, operation->result_type);
    bool exact = semantic_stringbuilder_type_is_exact(receiver_type) && result_type &&
                 result_type->kind == XR_KIND_STRING && receiver->role == XR_SEM_OPERAND_RECEIVER &&
                 receiver->parameter == -1 && receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT;
    if (exact && receiver_value)
        *receiver_value = receiver->value;
    return exact;
}

static bool
operation_is_exact_stringbuilder_append_string(const XrSemanticPlan *semantic,
                                               const XrSemanticOperationRecord *operation,
                                               uint32_t *argument_value) {
    uint32_t count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_STRING ||
        operation->operand_count != 2 || operation->operand_begin + 1u >= count ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "append") != 0 ||
        operation->result_alias_operand != 0)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin],
                                  *argument = receiver + 1;
    const XrSemanticTypeRecord *rt = xr_semantic_plan_type(semantic, receiver->type);
    const XrSemanticTypeRecord *at = xr_semantic_plan_type(semantic, argument->type);
    /* The same terms the builder proves before it writes the row: a verifier
     * that admits more than the builder writes cannot catch a row the builder
     * should not have written. */
    bool exact = semantic_stringbuilder_type_is_exact(rt) && at && at->kind == XR_KIND_STRING &&
                 operation->result_type == receiver->type &&
                 semantic_stringbuilder_append_result_is_exact_verify(semantic, operation) &&
                 receiver->role == XR_SEM_OPERAND_RECEIVER && receiver->parameter == -1 &&
                 receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
                 argument->role == XR_SEM_OPERAND_ARGUMENT && argument->parameter == 0 &&
                 argument->flags == XR_SEM_OPERAND_CALL_CONTRACT;
    if (exact && argument_value)
        *argument_value = argument->value;
    return exact;
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

static bool operation_is_exact_json_namespace_value(const XrSemanticPlan *semantic,
                                                    const XrSemanticOperationRecord *operation,
                                                    uint32_t *argument_value) {
    uint32_t operands_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operands_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
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
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(semantic, receiver->type);
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(semantic, operation->result_type);
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

/* Rebuilt from the frozen rows.  The array kind is a compiler-owned container
 * spelling: it never carries a source class index or identity, so a declared
 * receiver cannot present this record, and a frozen selector below then names
 * one implementation.  Each row states the whole shape one selector may
 * present: the operand count range, which operand carries the element, and what
 * the result is. The element clause is matched against the receiver's own
 * element entry, and this verifier independently reconstructs the row's
 * reference action, ownership, and eventual drop lifecycle. */

static bool operation_is_exact_array_reserve(const XrSemanticPlan *semantic,
                                             const XrSemanticOperationRecord *operation,
                                             uint32_t *receiver_type_index,
                                             uint32_t *capacity_value) {
    uint32_t operands_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operands_count);
    if (!semantic || !operation || !operands ||
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
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(semantic, receiver->type);
    const XrSemanticTypeRecord *capacity_type = xr_semantic_plan_type(semantic, capacity->type);
    const XrSemanticFunctionRecord *function =
        xr_semantic_plan_function(semantic, operation->function);
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

static bool verifier_array_member_reference_contract_is_exact(
    const XrSemanticPlan *semantic, const XrArrayMemberShape *shape,
    const XrSemanticOperationRecord *operation, uint32_t element_type_index,
    const XrSemanticTypeRecord *element_type) {
    if (!semantic || !shape || !operation || !element_type)
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
        xr_semantic_class_instance_type_source_class(semantic, element_type) ==
            XR_SEMANTIC_INDEX_NONE)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t semantic_operand = operation->operand_begin + shape->element_operand;
    if (!operands || semantic_operand >= operand_count)
        return false;
    const XrSemanticOperandRecord *element = &operands[semantic_operand];
    return element->type == element_type_index && element->role == XR_SEM_OPERAND_ARGUMENT &&
           element->parameter == (int16_t) (shape->element_operand - 1u) &&
           element->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
           element->ownership_action == XR_SEM_OPERAND_CONSUME;
}

static bool operation_is_exact_array_member_scalar(const XrSemanticPlan *semantic,
                                                   const XrSemanticOperationRecord *operation,
                                                   uint32_t *receiver_type_index,
                                                   uint32_t *element_value, bool *receiver_result) {
    uint32_t reserve_receiver_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t reserve_capacity = XR_SEMANTIC_INDEX_NONE;
    if (operation_is_exact_array_reserve(semantic, operation, &reserve_receiver_type,
                                         &reserve_capacity)) {
        if (receiver_type_index)
            *receiver_type_index = reserve_receiver_type;
        if (element_value)
            *element_value = reserve_capacity;
        if (receiver_result)
            *receiver_result = true;
        return true;
    }
    uint32_t operands_count = 0, metadata_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operands_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    const uint32_t *children = xr_semantic_plan_type_children(semantic, &child_count);
    if (!semantic || !operation ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR ||
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
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(semantic, receiver->type);
    if (!shape || !xr_semantic_array_type_row_is_exact(receiver_type) ||
        receiver_type->child_begin >= child_count || receiver->role != XR_SEM_OPERAND_RECEIVER ||
        receiver->parameter != -1 || receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    uint32_t element_type_index = children[receiver_type->child_begin];
    const XrSemanticTypeRecord *element_type = xr_semantic_plan_type(semantic, element_type_index);
    bool source_class_fill_result =
        element_type && strcmp(shape->selector, "fill") == 0 && operation->operand_count == 4 &&
        operation->semantic_immediate == (int64_t) XI_METHOD_SYMBOL_FILL << 1 &&
        xr_semantic_class_instance_type_source_class(semantic, element_type) !=
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
             operation, shape, xr_semantic_plan_type(semantic, operation->result_type),
             receiver->type) &&
         !source_class_fill_result) ||
        !verifier_array_member_reference_contract_is_exact(semantic, shape, operation,
                                                           element_type_index, element_type))
        return false;
    uint32_t element = XR_SEMANTIC_INDEX_NONE;
    for (uint16_t i = 1; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = receiver + i;
        const XrSemanticTypeRecord *argument_type = xr_semantic_plan_type(semantic, argument->type);
        bool is_element = i == shape->element_operand;
        if (!xr_semantic_array_member_argument_is_exact(shape, argument, argument_type, i,
                                                        element_type_index))
            return false;
        if (is_element)
            element = argument->value;
    }
    if (receiver_type_index)
        *receiver_type_index = receiver->type;
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

/* Rebuilt from the frozen rows alone, never from the builder. A native stdlib
 * namespace receiver cannot be spelled by a declaration: the module-init import
 * record classifies its resolution against the native definition registry and
 * names the module path with an empty member, and the registry then names one
 * implementation for that path plus the selector. */
static bool verifier_native_module_import_is_exact(const XrSemanticPlan *semantic,
                                                   const XrSemanticOperationRecord *record,
                                                   const char **out_module_path) {
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    const XrSemanticTypeRecord *type =
        record ? xr_semantic_plan_type(semantic, record->result_type) : NULL;
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

static bool verifier_native_module_load_is_exact(const XrSemanticPlan *semantic,
                                                 const XrSemanticOperationRecord *record) {
    const XrSemanticTypeRecord *type =
        record ? xr_semantic_plan_type(semantic, record->result_type) : NULL;
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

static const char *verifier_native_module_namespace_path(const XrSemanticPlan *semantic,
                                                         uint32_t receiver_value) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    const XrSemanticOperationRecord *load = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->result_value != receiver_value)
            continue;
        if (load)
            return NULL;
        load = candidate;
    }
    if (!verifier_native_module_load_is_exact(semantic, load))
        return NULL;
    const XrSemanticOperationRecord *store = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
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
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
        if (!candidate || candidate->result_value != stored->value)
            continue;
        if (import)
            return NULL;
        import = candidate;
    }
    const char *module_path = NULL;
    return import && import->result_type == load->result_type &&
                   verifier_native_module_import_is_exact(semantic, import, &module_path)
               ? module_path
               : NULL;
}

static bool verifier_native_module_call_shape_is_exact(const XrSemanticPlan *semantic,
                                                       const XrSemanticOperationRecord *operation,
                                                       const char **out_selector,
                                                       uint32_t *out_receiver_value,
                                                       uint32_t *out_arity) {
    uint32_t operands_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operands_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!semantic || !operation ||
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
            xr_semantic_plan_type(semantic, operation->result_type), true))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    if (receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    for (uint16_t i = 1; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = receiver + i;
        if (argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != (int16_t) (i - 1) ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            !xr_semantic_native_module_boundary_type_is_exact(
                xr_semantic_plan_type(semantic, argument->type), false))
            return false;
    }
    if (out_selector)
        *out_selector = metadata[operation->metadata_begin];
    if (out_receiver_value)
        *out_receiver_value = receiver->value;
    if (out_arity)
        *out_arity = (uint32_t) (operation->operand_count - 1u);
    return true;
}

static bool operation_is_exact_native_module_scalar_call(const XrSemanticPlan *semantic,
                                                         const XrSemanticOperationRecord *operation,
                                                         uint32_t *argument_count) {
    const char *selector = NULL;
    uint32_t receiver_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t arity = 0;
    if (!verifier_native_module_call_shape_is_exact(semantic, operation, &selector, &receiver_value,
                                                    &arity))
        return false;
    const char *module_path = verifier_native_module_namespace_path(semantic, receiver_value);
    if (!module_path ||
        !xr_stdlib_metadata_exact_native_direct_member(module_path, selector, (uint16_t) arity))
        return false;
    if (argument_count)
        *argument_count = arity;
    return true;
}

/* Independently reconstruct the frozen yieldable target that authorizes a
 * namespace receiver use. The registry tuple and stable target key must agree;
 * a matching selector without that upstream identity is not authority. */
static bool verifier_native_module_yieldable_call_is_exact(
    const XrSemanticPlan *semantic, uint32_t operation_index,
    const XrSemanticOperationRecord *operation, const char *module_path, uint32_t receiver_value,
    uint32_t receiver_type) {
    uint32_t operand_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!semantic || !operation || !module_path || !module_path[0] || !operands || !metadata ||
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
    uint32_t target_count = (uint32_t) xr_semantic_plan_call_target_count(semantic);
    for (uint32_t i = 0; i < target_count; i++) {
        const XrSemanticCallTargetRecord *candidate = xr_semantic_plan_call_target(semantic, i);
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

/* Every use of the namespace handle is a reference-count edge, the module-init
 * store, or the receiver of a member call this plan already proved exact. */
static bool
verifier_native_module_namespace_value_is_exact(const XrSemanticPlan *semantic,
                                                const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    if (!operation)
        return false;
    if (operation->opcode == XI_IMPORT_REF) {
        if (!verifier_native_module_import_is_exact(semantic, operation, NULL))
            return false;
    } else if (operation->opcode == XI_GET_SHARED) {
        if (!verifier_native_module_load_is_exact(semantic, operation) ||
            !verifier_native_module_namespace_path(semantic, operation->result_value))
            return false;
    } else {
        return false;
    }
    bool consumed = false;
    const char *module_path = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *use = xr_semantic_plan_operation(semantic, i);
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
                module_path =
                    verifier_native_module_namespace_path(semantic, operation->result_value);
            bool scalar = verifier_native_module_call_shape_is_exact(semantic, use, &selector,
                                                                     &receiver_value, &arity) &&
                          receiver_value == operation->result_value && module_path &&
                          xr_stdlib_metadata_exact_native_direct_member(module_path, selector,
                                                                        (uint16_t) arity);
            bool yieldable = verifier_native_module_yieldable_call_is_exact(
                semantic, i, use, module_path, operation->result_value, operation->result_type);
            if (!scalar && !yieldable)
                return false;
            consumed = true;
        }
    }
    return operation->opcode == XI_IMPORT_REF || consumed;
}

static bool collect_exact_native_module_namespace_values(const XrTargetPlan *plan,
                                                         uint8_t **out_exact, char *error,
                                                         size_t error_size) {
    if (out_exact)
        *out_exact = NULL;
    const XrSemanticPlan *semantic = plan ? plan->semantic_plan : NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    uint32_t value_count = 0;
    uint32_t function_count = (uint32_t) xr_semantic_plan_function_count(semantic);
    for (uint32_t i = 0; i < function_count; i++) {
        const XrSemanticFunctionRecord *function = xr_semantic_plan_function(semantic, i);
        if (!function || function->value_count > UINT32_MAX - function->value_begin)
            return report(error, error_size, "XR_TARGET_1001",
                          "semantic value ranges are not globally dense");
        if (function->value_begin + function->value_count > value_count)
            value_count = function->value_begin + function->value_count;
    }
    uint8_t *exact = value_count ? (uint8_t *) xr_calloc(value_count, sizeof(*exact)) : NULL;
    if (value_count && !exact)
        return report(error, error_size, "XR_EXEC_5003",
                      "native module namespace verifier allocation failed");
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (!operation ||
            (operation->opcode != XI_IMPORT_REF && operation->opcode != XI_GET_SHARED) ||
            operation->result_value >= value_count ||
            !verifier_native_module_namespace_value_is_exact(semantic, operation))
            continue;
        exact[operation->result_value] = 1;
    }
    if (out_exact)
        *out_exact = exact;
    else
        xr_free(exact);
    return true;
}

static bool operation_is_exact_stringbuilder_append_rune(const XrSemanticPlan *semantic,
                                                         const XrSemanticOperationRecord *operation,
                                                         uint32_t *receiver_value,
                                                         uint32_t *argument_value) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(semantic, &metadata_count);
    if (!semantic || !operation ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_STRINGBUILDER_APPEND_RUNE ||
        operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & 1) != 0 || operation->operand_count != 2 ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "append") != 0 ||
        operation->result_alias_operand != 0 ||
        !semantic_stringbuilder_append_result_is_exact_verify(semantic, operation))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *argument = receiver + 1;
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(semantic, receiver->type);
    const XrSemanticTypeRecord *argument_type = xr_semantic_plan_type(semantic, argument->type);
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

static bool overflow_predicate_covers_call_operation(const XrTargetPlan *plan,
                                                     uint32_t operation_index) {
    const XrSemanticPlan *semantic = plan ? plan->semantic_plan : NULL;
    const XrSemanticProgramProvenance *program =
        semantic ? xr_semantic_plan_program_provenance(semantic) : NULL;
    const XrSemanticProgramCallBinding *binding =
        semantic ? xr_semantic_plan_program_call_for_operation(semantic, operation_index) : NULL;
    if (!program ||
        program->program_family != XR_PROGRAM_SEMANTIC_FAMILY_I64_OVERFLOW_PREDICATE ||
        !binding || binding->program_row >= plan->i64_overflow_predicates_count)
        return false;
    const XrTargetI64OverflowPredicateRecord *row =
        &plan->i64_overflow_predicates[binding->program_row];
    return row->id == binding->program_row && row->program_row == binding->program_row &&
           row->semantic_operation == operation_index;
}

static bool verify_calls(const XrTargetPlan *plan, char *error, size_t error_size) {
    const XrSemanticPlan *semantic = plan->semantic_plan;
    uint32_t semantic_operations = (uint32_t) xr_semantic_plan_operation_count(semantic);
    uint32_t semantic_targets = (uint32_t) xr_semantic_plan_call_target_count(semantic);
    uint32_t semantic_functions = (uint32_t) xr_semantic_plan_function_count(semantic);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    uint8_t *covered = (uint8_t *) xr_calloc(semantic_operations, sizeof(*covered));
    uint8_t *state_counts = (uint8_t *) xr_calloc(semantic_operations, sizeof(*state_counts));
    uint8_t *suspendable = (uint8_t *) xr_calloc(semantic_functions, sizeof(*suspendable));
    uint32_t *reverse_head =
        semantic_functions
            ? (uint32_t *) xr_malloc((size_t) semantic_functions * sizeof(*reverse_head))
            : NULL;
    uint32_t *reverse_next =
        semantic_targets ? (uint32_t *) xr_malloc((size_t) semantic_targets * sizeof(*reverse_next))
                         : NULL;
    uint32_t *queue = semantic_functions
                          ? (uint32_t *) xr_malloc((size_t) semantic_functions * sizeof(*queue))
                          : NULL;
    if ((semantic_operations && (!covered || !state_counts)) ||
        (semantic_functions && (!suspendable || !reverse_head || !queue)) ||
        (semantic_targets && !reverse_next)) {
        xr_free(covered);
        xr_free(state_counts);
        xr_free(suspendable);
        xr_free(reverse_head);
        xr_free(reverse_next);
        xr_free(queue);
        return report(error, error_size, "XR_EXEC_5003", "call verifier allocation failed");
    }
    bool valid = true;
    uint32_t expected_calls = semantic_targets;
    uint32_t semantic_entities = (uint32_t) xr_semantic_plan_entity_count(semantic);
    for (uint32_t i = 0; valid && i < semantic_entities; i++) {
        const XrSemanticEntityRecord *entity = xr_semantic_plan_entity(semantic, i);
        if (!entity || entity->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        if (entity->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION ||
            entity->subject >= semantic_operations || ++state_counts[entity->subject] != 1)
            valid = false;
    }
    for (uint32_t function = 0; function < semantic_functions; function++)
        reverse_head[function] = XR_SEMANTIC_INDEX_NONE;
    uint32_t queue_begin = 0;
    uint32_t queue_end = 0;
    for (uint32_t operation_index = 0; valid && operation_index < semantic_operations;
         operation_index++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, operation_index);
        if (!operation || operation->function >= semantic_functions) {
            valid = false;
            break;
        }
        if (((operation->effects & XI_EFFECT_MAY_SUSPEND) != 0 || operation->opcode == XI_GO) &&
            !suspendable[operation->function]) {
            suspendable[operation->function] = 1;
            queue[queue_end++] = operation->function;
        }
        if (operation_is_exact_channel_close(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (semantic_stringbuilder_constructor_is_exact(semantic, operation)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (xr_semantic_scalar_copy_is_exact(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (xr_semantic_container_copy_is_exact(semantic, operation, NULL, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (semantic_array_intrinsic_is_exact_verify(semantic, operation, NULL, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (semantic_array_fill_scalar_is_exact_verify(semantic, operation, NULL, NULL, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (semantic_array_hof_is_exact_verify(semantic, operation, NULL, NULL, NULL, NULL, NULL,
                                               NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (semantic_string_byte_slice_view_is_exact(semantic, operation)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (operation_is_exact_stringbuilder_append_rune(semantic, operation, NULL, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (xr_semantic_string_runes_is_exact(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (xr_semantic_iterator_rune_has_next_is_exact(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (xr_semantic_iterator_rune_next_is_exact(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (xr_semantic_iterator_rune_nth_is_exact(semantic, operation, NULL, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (semantic_map_entries_iterator_is_exact_verify(semantic, operation, NULL, NULL) ||
            semantic_map_entry_iterator_has_next_is_exact_verify(semantic, operation, NULL) ||
            semantic_map_entry_iterator_next_is_exact_verify(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (operation_is_exact_rune_to_uint32_verify(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (xr_semantic_rune_to_string_is_exact(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (xr_semantic_rune_is_whitespace_is_exact(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (xr_semantic_string_slice_range_is_exact(semantic, operation, NULL, NULL, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (operation_is_exact_stringbuilder_to_string(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (operation_is_exact_stringbuilder_append_string(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (operation_is_exact_native_module_scalar_call(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (xr_semantic_native_target_leaf_call_is_exact(semantic, operation, NULL, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (operation_is_exact_json_namespace_value(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (xr_semantic_panic_info_constructor_is_exact(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (operation_is_exact_array_member_scalar(semantic, operation, NULL, NULL, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
        if (xr_semantic_adt_enum_constructor_is_exact(semantic, operation, NULL)) {
            if (expected_calls == UINT32_MAX) {
                valid = false;
                break;
            }
            expected_calls++;
        }
    }
    valid = valid && plan->calls_count == expected_calls;
    for (uint32_t target_index = 0; valid && target_index < semantic_targets; target_index++) {
        const XrSemanticCallTargetRecord *target =
            xr_semantic_plan_call_target(semantic, target_index);
        const XrSemanticOperationRecord *operation =
            target && target->operation < semantic_operations
                ? xr_semantic_plan_operation(semantic, target->operation)
                : NULL;
        bool direct = target && (target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
                                 xr_semantic_call_target_binds_instance_method(
                                     target, semantic, plan->semantic_dependencies,
                                     plan->semantic_dependency_count));
        bool source = target && target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT;
        bool native_namespace =
            target && target->kind == XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE;
        bool class_construction =
            target && target->kind == XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR;
        bool builtin_instance =
            target && target->kind == XR_SEM_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE;
        if (!target || !operation ||
            (!direct && !source && !native_namespace && !class_construction && !builtin_instance) ||
            (direct && !xr_semantic_call_target_names_local_function(target, operation,
                                                                     semantic_functions)) ||
            (source && operation->opcode != XI_CALL_METHOD && operation->opcode != XI_CALL) ||
            (native_namespace && operation->opcode != XI_CALL_METHOD) ||
            (class_construction && operation->opcode != XI_CALL) ||
            (builtin_instance && operation->opcode != XI_CALL_METHOD)) {
            valid = false;
            break;
        }
        reverse_next[target_index] = XR_SEMANTIC_INDEX_NONE;
        if (direct) {
            reverse_next[target_index] = reverse_head[target->function];
            reverse_head[target->function] = target_index;
        } else if (!class_construction && state_counts[target->operation] != 0 &&
                   !suspendable[operation->function]) {
            suspendable[operation->function] = 1;
            queue[queue_end++] = operation->function;
        }
    }
    while (valid && queue_begin < queue_end) {
        uint32_t callee = queue[queue_begin++];
        for (uint32_t target_index = reverse_head[callee]; target_index != XR_SEMANTIC_INDEX_NONE;
             target_index = reverse_next[target_index]) {
            const XrSemanticCallTargetRecord *target =
                xr_semantic_plan_call_target(semantic, target_index);
            const XrSemanticOperationRecord *operation =
                target ? xr_semantic_plan_operation(semantic, target->operation) : NULL;
            if (!operation || operation->function >= semantic_functions) {
                valid = false;
                break;
            }
            if (!suspendable[operation->function]) {
                suspendable[operation->function] = 1;
                queue[queue_end++] = operation->function;
            }
        }
    }
    uint32_t next_argument = 0;
    uint32_t next_adapter = 0;
    uint32_t previous_operation = XR_SEMANTIC_INDEX_NONE;
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(plan->profile);
    for (uint32_t i = 0; valid && i < plan->calls_count; i++) {
        const XrTargetCallRecord *call = &plan->calls[i];
        bool semantic_target = call->semantic_call_target != XR_SEMANTIC_INDEX_NONE;
        const XrSemanticCallTargetRecord *target =
            semantic_target ? xr_semantic_plan_call_target(semantic, call->semantic_call_target)
                            : NULL;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, call->semantic_operation);
        bool direct = target && (target->kind == XR_SEM_CALL_TARGET_DIRECT_LOCAL ||
                                 xr_semantic_call_target_binds_instance_method(
                                     target, semantic, plan->semantic_dependencies,
                                     plan->semantic_dependency_count));
        /* The receiver fills parameter 0, so a method call's operands line up
         * with the parameter list one to one while a direct call's run one
         * ahead. Both the head-operand check and the loop below need to know
         * which. */
        bool method = xr_semantic_local_call_operand_shift(target) == 0u;
        bool source = target && target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT;
        bool native_namespace =
            target && target->kind == XR_SEM_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE;
        /* Re-derived from the plan, never read back from the row: a target row
         * that claims a construction the shared judgement cannot re-prove is
         * rejected together with the intent that names it. */
        bool imported_class_construction =
            target && target->kind == XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR &&
            target->dependency != XR_SEMANTIC_INDEX_NONE;
        /* Re-proved from the same shared judgement the builder used, so a row
         * naming a roster entry this plan cannot re-prove is rejected here. */
        uint32_t builtin_receiver_type = XR_SEMANTIC_INDEX_NONE;
        bool builtin_instance = target && xr_semantic_builtin_instance_yieldable_call_is_exact(
                                              semantic, target, operation, &builtin_receiver_type);
        const XrSemanticFunctionRecord *callee =
            direct ? xr_semantic_plan_function(semantic, target->function) : NULL;
        const XrSemanticPlan *dependency =
            (source || imported_class_construction) &&
                    target->dependency < plan->semantic_dependency_count
                ? plan->semantic_dependencies[target->dependency]
                : NULL;
        const XrSemanticSourceExportRecord *source_export =
            dependency && target->source_export < xr_semantic_plan_source_export_count(dependency)
                ? xr_semantic_plan_source_export(dependency, target->source_export)
                : NULL;
        const XrSemanticFunctionRecord *source_callee =
            source_export && source_export->kind == XR_SEM_SOURCE_EXPORT_FUNCTION
                ? xr_semantic_plan_function(dependency, source_export->function)
                : NULL;
        uint32_t imported_constructor = XR_SEMANTIC_INDEX_NONE;
        uint32_t imported_source_class =
            imported_class_construction
                ? xr_semantic_imported_class_construction_authority_source_class(
                      semantic, dependency,
                      target->dependency < xr_semantic_plan_dependency_count(semantic)
                          ? xr_semantic_plan_dependency(semantic, target->dependency)
                          : NULL,
                      source_export, operation, &imported_constructor)
                : XR_SEMANTIC_INDEX_NONE;
        bool class_construction =
            target && target->kind == XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR &&
            (imported_class_construction ? imported_source_class != XR_SEMANTIC_INDEX_NONE
                                         : xr_semantic_class_construction_source_class(
                                               semantic, operation) != XR_SEMANTIC_INDEX_NONE);
        const XrSemanticTypeRecord *source_result_type =
            source_callee ? xr_semantic_plan_type(dependency, source_callee->return_type) : NULL;
        const XrSemanticTypeRecord *caller_result_type =
            operation ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
        bool source_callee_suspendable = false;
        bool source_suspendability_exact =
            !source ||
            (source_callee && semantic_function_suspendability_is_exact(
                                  dependency, source_export->function, &source_callee_suspendable));
        XrStableId expected_identity;
        uint32_t receiver_type_index = XR_SEMANTIC_INDEX_NONE;
        bool channel_close = !semantic_target && operation_is_exact_channel_close(
                                                     semantic, operation, &receiver_type_index);
        bool stringbuilder_constructor =
            !semantic_target && semantic_stringbuilder_constructor_is_exact(semantic, operation);
        uint32_t container_copy_argument = XR_SEMANTIC_INDEX_NONE;
        uint8_t container_copy_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        bool container_copy =
            !semantic_target &&
            xr_semantic_container_copy_is_exact(semantic, operation, &container_copy_argument,
                                                NULL) &&
            xr_target_container_copy_storage(semantic, operation, &container_copy_storage);
        uint32_t scalar_copy_argument = XR_SEMANTIC_INDEX_NONE;
        bool scalar_copy = !semantic_target && xr_semantic_scalar_copy_is_exact(
                                                   semantic, operation, &scalar_copy_argument);
        bool string_byte_slice_view =
            !semantic_target && semantic_string_byte_slice_view_is_exact(semantic, operation);
        uint32_t append_receiver = XR_SEMANTIC_INDEX_NONE;
        uint32_t append_argument = XR_SEMANTIC_INDEX_NONE;
        bool stringbuilder_append_rune =
            !semantic_target && operation_is_exact_stringbuilder_append_rune(
                                    semantic, operation, &append_receiver, &append_argument);
        uint32_t string_runes_receiver = XR_SEMANTIC_INDEX_NONE;
        bool string_runes = !semantic_target && xr_semantic_string_runes_is_exact(
                                                    semantic, operation, &string_runes_receiver);
        uint32_t iterator_rune_has_next_receiver = XR_SEMANTIC_INDEX_NONE;
        bool iterator_rune_has_next =
            !semantic_target && xr_semantic_iterator_rune_has_next_is_exact(
                                    semantic, operation, &iterator_rune_has_next_receiver);
        uint32_t iterator_rune_next_receiver = XR_SEMANTIC_INDEX_NONE;
        bool iterator_rune_next =
            !semantic_target && xr_semantic_iterator_rune_next_is_exact(
                                    semantic, operation, &iterator_rune_next_receiver);
        uint32_t iterator_rune_nth_receiver = XR_SEMANTIC_INDEX_NONE;
        uint32_t iterator_rune_nth_index = XR_SEMANTIC_INDEX_NONE;
        bool iterator_rune_nth =
            !semantic_target &&
            xr_semantic_iterator_rune_nth_is_exact(semantic, operation, &iterator_rune_nth_receiver,
                                                   &iterator_rune_nth_index);
        uint32_t map_entry_iterator_receiver = XR_SEMANTIC_INDEX_NONE;
        bool map_entries_iterator =
            !semantic_target && semantic_map_entries_iterator_is_exact_verify(
                                    semantic, operation, &map_entry_iterator_receiver, NULL);
        bool map_entry_iterator_has_next =
            !semantic_target && semantic_map_entry_iterator_has_next_is_exact_verify(
                                    semantic, operation, &map_entry_iterator_receiver);
        bool map_entry_iterator_next =
            !semantic_target && semantic_map_entry_iterator_next_is_exact_verify(
                                    semantic, operation, &map_entry_iterator_receiver);
        uint32_t rune_to_uint32_receiver = XR_SEMANTIC_INDEX_NONE;
        bool rune_to_uint32 =
            !semantic_target &&
            operation_is_exact_rune_to_uint32_verify(semantic, operation, &rune_to_uint32_receiver);
        uint32_t rune_to_string_receiver = XR_SEMANTIC_INDEX_NONE;
        bool rune_to_string =
            !semantic_target &&
            xr_semantic_rune_to_string_is_exact(semantic, operation, &rune_to_string_receiver);
        uint32_t rune_is_whitespace_receiver = XR_SEMANTIC_INDEX_NONE;
        bool rune_is_whitespace =
            !semantic_target && xr_semantic_rune_is_whitespace_is_exact(
                                    semantic, operation, &rune_is_whitespace_receiver);
        uint32_t string_slice_receiver = XR_SEMANTIC_INDEX_NONE;
        uint32_t string_slice_start = XR_SEMANTIC_INDEX_NONE;
        uint32_t string_slice_end = XR_SEMANTIC_INDEX_NONE;
        bool string_slice_range =
            !semantic_target &&
            xr_semantic_string_slice_range_is_exact(semantic, operation, &string_slice_receiver,
                                                    &string_slice_start, &string_slice_end);
        uint32_t to_string_receiver = XR_SEMANTIC_INDEX_NONE;
        bool stringbuilder_to_string =
            !semantic_target &&
            operation_is_exact_stringbuilder_to_string(semantic, operation, &to_string_receiver);
        uint32_t append_string_argument = XR_SEMANTIC_INDEX_NONE;
        bool stringbuilder_append_string =
            !semantic_target && operation_is_exact_stringbuilder_append_string(
                                    semantic, operation, &append_string_argument);
        uint32_t json_value_argument = XR_SEMANTIC_INDEX_NONE;
        bool json_namespace_value =
            !semantic_target &&
            operation_is_exact_json_namespace_value(semantic, operation, &json_value_argument);
        uint32_t panic_info_argument = XR_SEMANTIC_INDEX_NONE;
        bool panic_info_constructor =
            !semantic_target &&
            xr_semantic_panic_info_constructor_is_exact(semantic, operation, &panic_info_argument);
        uint32_t array_member_element = XR_SEMANTIC_INDEX_NONE;
        uint32_t array_member_receiver_type = XR_SEMANTIC_INDEX_NONE;
        bool array_member_receiver_result = false;
        bool array_member_scalar =
            !semantic_target && operation_is_exact_array_member_scalar(
                                    semantic, operation, &array_member_receiver_type,
                                    &array_member_element, &array_member_receiver_result);
        uint32_t array_member_store_operand = XR_SEMANTIC_INDEX_NONE;
        bool array_member_tagged_store =
            array_member_scalar &&
            operation_is_exact_array_member_tagged_store(
                semantic, operation, &array_member_store_operand, &array_member_element, NULL);
        uint32_t native_module_arity = 0;
        bool native_module_scalar =
            !semantic_target &&
            operation_is_exact_native_module_scalar_call(semantic, operation, &native_module_arity);
        const XrStdlibDefEntry *native_target_leaf_entry = NULL;
        XrStableId native_target_leaf_identity = {{0}};
        bool native_target_leaf =
            !semantic_target && xr_semantic_native_target_leaf_call_is_exact(
                                    semantic, operation, &native_target_leaf_entry,
                                    &native_target_leaf_identity);
        XrSemanticAdtEnumConstructorShape enum_constructor_shape = {0};
        bool adt_enum_constructor =
            !semantic_target &&
            xr_semantic_adt_enum_constructor_is_exact(semantic, operation, &enum_constructor_shape);
        uint8_t array_intrinsic_kind = XR_TARGET_ARRAY_INTRINSIC_NONE;
        uint8_t array_intrinsic_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        bool array_intrinsic = !semantic_target && semantic_array_intrinsic_is_exact_verify(
                                                       semantic, operation, &array_intrinsic_kind,
                                                       &array_intrinsic_storage);
        uint32_t array_fill_receiver = XR_SEMANTIC_INDEX_NONE;
        uint32_t array_fill_value = XR_SEMANTIC_INDEX_NONE;
        uint8_t array_fill_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        bool array_fill = !semantic_target && semantic_array_fill_scalar_is_exact_verify(
                                                  semantic, operation, &array_fill_receiver,
                                                  &array_fill_value, &array_fill_storage);
        uint8_t array_hof_kind = XR_TARGET_ARRAY_HOF_NONE;
        uint8_t array_hof_source_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        uint8_t array_hof_result_storage = XR_TARGET_ARRAY_STORAGE_NONE;
        uint32_t array_hof_receiver = XR_SEMANTIC_INDEX_NONE;
        uint32_t array_hof_callback = XR_SEMANTIC_INDEX_NONE;
        uint32_t array_hof_initial = XR_SEMANTIC_INDEX_NONE;
        bool array_hof =
            !semantic_target && semantic_array_hof_is_exact_verify(
                                    semantic, operation, &array_hof_kind, &array_hof_source_storage,
                                    &array_hof_result_storage, &array_hof_receiver,
                                    &array_hof_callback, &array_hof_initial);
        uint16_t result_kind = XR_MACHINE_REP_COUNT;
        const XrSemanticTypeRecord *result_type =
            operation ? xr_semantic_plan_type(semantic, operation->result_type) : NULL;
        const XrTargetValueRepRecord *result =
            operation ? xr_target_plan_value_rep(plan, operation->result_value) : NULL;
        int result_scalar =
            result_type ? semantic_type_expected_rep(result_type, &result_kind) : -1;
        bool direct_string_result = direct && semantic_direct_local_string_result_is_exact(
                                                  semantic, call->semantic_operation);
        bool direct_array_result = direct && semantic_direct_local_array_result_is_exact_verify(
                                                 semantic, call->semantic_operation);
        bool direct_adt_enum_result = direct && semantic_direct_local_adt_enum_result_is_exact(
                                                    semantic, call->semantic_operation);
        bool direct_class_instance_result =
            direct && xr_semantic_class_instance_result_source_class(semantic, operation) !=
                          XR_SEMANTIC_INDEX_NONE;
        XrVerifyLeafProgramShape leaf_program = {0};
        bool direct_leaf_program =
            direct && verify_leaf_program_shape(semantic, &leaf_program) &&
            leaf_program.call_binding->operation == call->semantic_operation &&
            leaf_program.operation == operation && leaf_program.callee == callee;
        XrVerifyProductProgramShape product_program = {0};
        bool direct_product_program = false;
        if (direct && verify_product_program_shape(semantic, &product_program))
            for (uint32_t caller = 0; caller < 2; caller++)
                direct_product_program =
                    direct_product_program ||
                    (product_program.call_bindings[caller]->operation ==
                         call->semantic_operation &&
                     product_program.calls[caller] == operation &&
                     product_program.callee == callee);
        bool direct_aggregate_result = direct_leaf_program || direct_product_program;
        if (stringbuilder_constructor || stringbuilder_to_string || stringbuilder_append_string ||
            direct_string_result || direct_array_result || direct_adt_enum_result ||
            direct_class_instance_result || json_namespace_value || class_construction ||
            adt_enum_constructor || array_intrinsic || array_fill || panic_info_constructor ||
            container_copy) {
            result_scalar = 1;
            result_kind = XR_MACHINE_REP_DYN_VALUE;
        }
        if (array_hof && array_hof_kind != XR_TARGET_ARRAY_HOF_REDUCE) {
            result_scalar = 1;
            result_kind = XR_MACHINE_REP_DYN_VALUE;
        }
        if (string_byte_slice_view) {
            result_scalar = 1;
            result_kind = XR_MACHINE_REP_VIEW;
        }
        if (stringbuilder_append_rune || string_runes || string_slice_range || rune_to_string) {
            result_scalar = 1;
            result_kind = XR_MACHINE_REP_DYN_VALUE;
        }
        bool borrowed_stringbuilder_append =
            (stringbuilder_append_rune || stringbuilder_append_string) && operation &&
            operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED;
        if (map_entries_iterator) {
            result_scalar = 1;
            result_kind = XR_MACHINE_REP_DYN_VALUE;
        }
        if (map_entry_iterator_next) {
            result_scalar = 1;
            result_kind = XR_MACHINE_REP_DYN_VALUE;
        }
        /* An array member that hands back its receiver names the container
         * again, whose one storage fact is the owned tagged outer value. */
        if (array_member_receiver_result) {
            result_scalar = 1;
            result_kind = XR_MACHINE_REP_DYN_VALUE;
        }
        /* A value aggregate is returned into the caller's own slot, so the row
         * carries the aggregate representation rather than a tagged value, and
         * no ownership: nothing was transferred to take ownership of. */
        if (direct_aggregate_result) {
            result_scalar = 1;
            result_kind = XR_MACHINE_REP_AGGREGATE;
        }
        bool suspends = operation && call->semantic_operation < semantic_operations &&
                        state_counts[call->semantic_operation] == 1;
        bool expected_suspend =
            operation &&
            ((operation->effects & XI_EFFECT_MAY_SUSPEND) != 0 || operation->opcode == XI_GO ||
             native_namespace || (source && source_callee_suspendable) ||
             (operation->opcode == XI_CALL && target && target->function < semantic_functions &&
              suspendable[target->function] != 0));
        valid =
            operation && machine && source_suspendability_exact &&
            call->semantic_operation < semantic_operations && !covered[call->semantic_operation] &&
            (previous_operation == XR_SEMANTIC_INDEX_NONE ||
             call->semantic_operation > previous_operation) &&
            operation->function < semantic_functions && result_scalar == 1 && result &&
            result->register_rep < plan->machine_reps_count &&
            result->memory_rep < plan->machine_reps_count &&
            plan->machine_reps[result->register_rep].kind == result_kind &&
            plan->machine_reps[result->memory_rep].kind == result_kind &&
            slot_binds_value_in_function(plan, result, operation->function) &&
            operation->operand_begin <= operand_count &&
            operation->operand_count <= operand_count - operation->operand_begin && call->id == i &&
            call->caller_function == operation->function &&
            call->result_value == operation->result_value && call->result_slot == result->slot &&
            call->caller_storage_slot ==
                (direct_aggregate_result ? result->slot : XR_SEMANTIC_INDEX_NONE) &&
            call->error_slot == XR_SEMANTIC_INDEX_NONE && call->argument_begin == next_argument &&
            range_valid(call->argument_begin, call->argument_count, plan->call_arguments_count) &&
            (!direct || (callee && call->argument_count == callee->parameter_count)) &&
            call->adapter_begin == next_adapter && call->adapter_count == 0 &&
            call->result_register_rep == result->register_rep &&
            call->result_memory_rep == result->memory_rep &&
            call->error_register_rep < plan->machine_reps_count &&
            call->error_memory_rep < plan->machine_reps_count &&
            plan->machine_reps[call->error_register_rep].kind == XR_MACHINE_REP_VOID &&
            plan->machine_reps[call->error_memory_rep].kind == XR_MACHINE_REP_VOID &&
            call->native_abi == machine->native_abi &&
            (native_target_leaf
                 ? (!stable_id_is_zero(call->native_callee_identity) &&
                    call->native_leaf > XR_STDLIB_TARGET_LEAF_NONE &&
                    call->native_leaf < XR_STDLIB_TARGET_LEAF_COUNT)
                 : (stable_id_is_zero(call->native_callee_identity) &&
                    call->native_leaf == XR_STDLIB_TARGET_LEAF_NONE)) &&
            call->result_mode ==
                (direct_aggregate_result ? XR_TARGET_CALL_CALLER_STORAGE
                                         : XR_TARGET_CALL_VALUE) &&
            /* Receiver-aliasing StringBuilder append preserves a borrowed
             * receiver. Every genuinely fresh dynamic family agrees on
             * RETURN_OWNED; the per-family branches below re-assert the same
             * answer, so the shared and specific checks cannot drift. */
            call->result_ownership ==
                (borrowed_stringbuilder_append ? XR_TARGET_CALL_BORROW
                 : stringbuilder_constructor || direct_string_result || direct_array_result ||
                         direct_adt_enum_result || direct_class_instance_result ||
                         stringbuilder_append_rune || string_runes || string_slice_range ||
                         rune_to_string || stringbuilder_to_string || stringbuilder_append_string ||
                         json_namespace_value || class_construction || adt_enum_constructor ||
                         array_intrinsic || panic_info_constructor || container_copy ||
                         map_entries_iterator || map_entry_iterator_next ||
                         (array_hof && array_hof_kind != XR_TARGET_ARRAY_HOF_REDUCE)
                     ? XR_TARGET_CALL_RETURN_OWNED
                 : string_byte_slice_view ? XR_TARGET_CALL_BORROW
                                          : XR_TARGET_CALL_NONE) &&
            call->error_mode == XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL &&
            (array_intrinsic ? call->array_intrinsic_kind == array_intrinsic_kind &&
                                   call->array_element_storage == array_intrinsic_storage
             : array_fill    ? call->array_intrinsic_kind == XR_TARGET_ARRAY_INTRINSIC_NONE &&
                                   call->array_element_storage == array_fill_storage
             : array_hof     ? call->array_intrinsic_kind == XR_TARGET_ARRAY_INTRINSIC_NONE &&
                                   call->array_element_storage == array_hof_source_storage &&
                                   call->array_hof_kind == array_hof_kind &&
                                   call->array_result_element_storage == array_hof_result_storage
             : container_copy
                 ? call->array_intrinsic_kind == XR_TARGET_ARRAY_INTRINSIC_NONE &&
                       call->array_element_storage == container_copy_storage &&
                       call->array_hof_kind == XR_TARGET_ARRAY_HOF_NONE &&
                       call->array_result_element_storage == XR_TARGET_ARRAY_STORAGE_NONE
             : array_member_tagged_store
                 ? call->array_intrinsic_kind == XR_TARGET_ARRAY_INTRINSIC_NONE &&
                       call->array_element_storage == XR_TARGET_ARRAY_STORAGE_TAGGED &&
                       call->array_hof_kind == XR_TARGET_ARRAY_HOF_NONE &&
                       call->array_result_element_storage == XR_TARGET_ARRAY_STORAGE_NONE
                 : call->array_intrinsic_kind == XR_TARGET_ARRAY_INTRINSIC_NONE &&
                       call->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
                       call->array_hof_kind == XR_TARGET_ARRAY_HOF_NONE &&
                       call->array_result_element_storage == XR_TARGET_ARRAY_STORAGE_NONE) &&
            call->reserved8[0] == 0 && call->reserved8[1] == 0 && call->reserved8[2] == 0;
        if (!valid)
            break;
        if (direct) {
            valid =
                target && callee && target->operation == call->semantic_operation &&
                xr_semantic_call_target_names_local_function(target, operation,
                                                             semantic_functions) &&
                suspends == expected_suspend && operation->result_type == callee->return_type &&
                operation->operand_count == (uint32_t) callee->parameter_count +
                                                xr_semantic_local_call_operand_shift(target) &&
                callee->parameter_begin <= xr_semantic_plan_parameter_count(semantic) &&
                callee->parameter_count <=
                    xr_semantic_plan_parameter_count(semantic) - callee->parameter_begin &&
                reconstruct_call_identity("xray-target-call-v5", target->id, operation->id, 0,
                                          &expected_identity) &&
                xr_stable_id_equal(call->identity, expected_identity) &&
                call->callee_function == target->function &&
                call->argument_count == callee->parameter_count &&
                call->flags == ((suspends ? XR_TARGET_CALL_SUSPEND : 0) |
                                (operation->opcode == XI_TAIL_CALL ? XR_TARGET_CALL_TAIL : 0)) &&
                call->calling_convention == XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL &&
                call->target_kind == XR_TARGET_CALL_TARGET_DIRECT_LOCAL &&
                call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                call->source_export == XR_SEMANTIC_INDEX_NONE &&
                stable_id_is_zero(call->source_export_identity) &&
                stable_id_is_zero(call->source_callee_identity) &&
                /* An owned String result carries the dynamic owned storage
                 * fact in its slot; a value aggregate is the mirror case,
                 * a slot the caller owns outright with no dynamic root to
                 * trace and no ownership to release. Every other result
                 * stays a trivial scalar slot. */
                (!(direct_string_result || direct_adt_enum_result) ||
                 (result->slot < plan->slots_count &&
                  plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                  plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED)) &&
                (!direct_aggregate_result ||
                 (result->slot < plan->slots_count &&
                  plan->slots[result->slot].root_kind == XR_TARGET_ROOT_NONE &&
                  plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_TRIVIAL));
            if (!valid)
                break;
            /* Only a direct call has a head operand outside the parameter list;
             * the method's receiver is checked in the loop like every other
             * argument, under the parameter it fills. */
            const XrSemanticOperandRecord *callee_operand = &operands[operation->operand_begin];
            valid = method || (callee_operand->role == XR_SEM_OPERAND_CALLEE &&
                               callee_operand->parameter == -1 &&
                               (callee_operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0);
            for (uint32_t ordinal = 0; valid && ordinal < call->argument_count; ordinal++) {
                const XrTargetCallArgumentRecord *argument = &plan->call_arguments[next_argument];
                uint32_t parameter_index = callee->parameter_begin + ordinal;
                uint32_t semantic_operand = operation->operand_begin + ordinal +
                                            xr_semantic_local_call_operand_shift(target);
                const XrSemanticParameterRecord *parameter =
                    xr_semantic_plan_parameter(semantic, parameter_index);
                const XrSemanticOperandRecord *operand = &operands[semantic_operand];
                uint32_t caller_storage_value = operand->value;
                bool argument_ref_place =
                    semantic_direct_local_ref_place_is_exact_verify(semantic, operand,
                                                                    operation->function,
                                                                    &caller_storage_value);
                const XrTargetValueRepRecord *caller_value =
                    xr_target_plan_value_rep(plan, caller_storage_value);
                const XrTargetValueRepRecord *callee_value =
                    parameter ? xr_target_plan_value_rep(plan, parameter->value) : NULL;
                XrStableId argument_identity;
                uint16_t argument_kind = XR_MACHINE_REP_COUNT;
                int argument_scalar =
                    operand->type < xr_semantic_plan_type_count(semantic)
                        ? semantic_type_expected_rep(xr_semantic_plan_type(semantic, operand->type),
                                                     &argument_kind)
                        : -1;
                bool argument_u8_slice =
                    semantic_u8_slice_parameter_is_exact(semantic, parameter) &&
                    semantic_u8_slice_type_is_exact(semantic, operand->type);
                bool argument_unit_enum = parameter && parameter->type == operand->type &&
                                          xr_semantic_unit_enum_type_is_exact(
                                              xr_semantic_plan_type(semantic, operand->type));
                bool argument_adt_enum = parameter && parameter->type == operand->type &&
                                         xr_semantic_adt_enum_type_is_exact(
                                             xr_semantic_plan_type(semantic, operand->type));
                uint8_t array_element_storage = XR_TARGET_ARRAY_STORAGE_NONE;
                bool argument_scalar_ref =
                    parameter && parameter->type == operand->type &&
                    semantic_direct_local_scalar_ref_parameter_is_exact_verify(
                        semantic, parameter, NULL) &&
                    operand->parameter_mode == XR_PARAM_REF && operand->access == XR_CALL_ARG_REF &&
                    operand->origin != XI_PLACE_ORIGIN_NONE &&
                    operand->lifetime == XI_PLACE_LIFETIME_CALL_BOUND &&
                    operand->escape == XI_PLACE_ESCAPE_NONE &&
                    operand->ownership_action == XR_SEM_OPERAND_BORROW &&
                    operand->transfer_mode == XR_TRANSFER_SHARE &&
                    operand->flags ==
                        (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE) &&
                    argument_ref_place;
                bool argument_tagged_ref =
                    parameter && parameter->type == operand->type &&
                    semantic_direct_local_tagged_ref_parameter_is_exact_verify(
                        semantic, parameter, &array_element_storage) &&
                    operand->parameter_mode == XR_PARAM_REF && operand->access == XR_CALL_ARG_REF &&
                    operand->origin != XI_PLACE_ORIGIN_NONE &&
                    operand->lifetime == XI_PLACE_LIFETIME_CALL_BOUND &&
                    operand->escape == XI_PLACE_ESCAPE_NONE &&
                    operand->ownership_action == XR_SEM_OPERAND_BORROW &&
                    operand->transfer_mode == XR_TRANSFER_SHARE &&
                    operand->flags == (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE) &&
                    argument_ref_place;
                bool argument_reference = argument_scalar_ref || argument_tagged_ref;
                /* An Array or a String handed over by value travels the plain
                 * argument path: the tagged value is copied and the allocation
                 * shared, so the row states no place, no element storage, and
                 * no addressability of its own. Both are reference-capable
                 * containers carried by that one tagged value, so they are one
                 * argument shape rather than two. */
                uint8_t array_value_storage = XR_TARGET_ARRAY_STORAGE_NONE;
                bool argument_string_callee_owns = false;
                bool argument_string_value =
                    parameter && parameter->type == operand->type &&
                    xr_semantic_direct_local_string_value_parameter_is_exact(
                        semantic, parameter, &argument_string_callee_owns);
                bool argument_array_value =
                    parameter && parameter->type == operand->type &&
                    semantic_direct_local_array_value_parameter_is_exact_verify(
                        semantic, parameter, &array_value_storage);
                bool argument_container_value =
                    (argument_string_value || argument_array_value) &&
                    operand->transfer_mode == XR_TRANSFER_SHARE &&
                    /* The call site must state the same allocation answer the
                     * declaration proved: a callee that holds an owning
                     * reference is one the caller hands its own over to. An
                     * Array by value is always borrowed, so it states so. */
                    operand->ownership_action ==
                        (argument_string_value && argument_string_callee_owns
                             ? XR_SEM_OPERAND_CONSUME
                             : XR_SEM_OPERAND_BORROW);
                bool argument_leaf_aggregate =
                    direct_leaf_program && ordinal == 0 && parameter == leaf_program.parameter &&
                    operand == leaf_program.argument &&
                    operand->type == leaf_program.aggregate_binding->semantic_type;
                if (argument_adt_enum)
                    argument_kind = XR_MACHINE_REP_DYN_VALUE;
                if (argument_tagged_ref || argument_container_value)
                    argument_kind = XR_MACHINE_REP_DYN_VALUE;
                if (argument_leaf_aggregate)
                    argument_kind = XR_MACHINE_REP_AGGREGATE;
                /* Recomputed through the same shared judgement the callee's own
                 * storage family uses, so an argument this verifier admits can
                 * never be a parameter that family refused. */
                /* A receiver is a class instance crossing a parameter boundary
                 * just as a declared class parameter is; the shared judgement
                 * covers both, plus the constructor receiver. Asking only about
                 * the declared-parameter form would refuse every `this`, whose
                 * type row is the anonymous instance naming no declaration. */
                bool argument_class_instance =
                    parameter && parameter->type == operand->type &&
                    xr_semantic_class_instance_parameter_source_class(semantic, parameter_index) !=
                        XR_SEMANTIC_INDEX_NONE &&
                    xr_semantic_class_parameter_call_transfer_is_exact(semantic, parameter_index,
                                                                       operand);
                uint8_t ownership = operand->ownership_action == XR_SEM_OPERAND_CONSUME
                                        ? XR_TARGET_CALL_CONSUME
                                        : XR_TARGET_CALL_READ;
                bool adt_enum_borrow_boundary =
                    argument_adt_enum && parameter->ownership == XI_OWN_BORROWED && caller_value &&
                    callee_value && caller_value->register_rep < plan->machine_reps_count &&
                    caller_value->memory_rep < plan->machine_reps_count &&
                    callee_value->register_rep < plan->machine_reps_count &&
                    callee_value->memory_rep < plan->machine_reps_count &&
                    machine_reps_have_same_call_abi(
                        &plan->machine_reps[caller_value->register_rep],
                        &plan->machine_reps[callee_value->register_rep]) &&
                    machine_reps_have_same_call_abi(
                        &plan->machine_reps[caller_value->memory_rep],
                        &plan->machine_reps[callee_value->memory_rep]) &&
                    plan->machine_reps[caller_value->register_rep].ownership ==
                        XR_TARGET_OWNERSHIP_OWNED &&
                    plan->machine_reps[caller_value->memory_rep].ownership ==
                        XR_TARGET_OWNERSHIP_OWNED &&
                    plan->machine_reps[callee_value->register_rep].ownership ==
                        XR_TARGET_OWNERSHIP_BORROWED &&
                    plan->machine_reps[callee_value->memory_rep].ownership ==
                        XR_TARGET_OWNERSHIP_BORROWED;
                bool tagged_ref_borrow_boundary =
                    argument_tagged_ref && caller_value && callee_value &&
                    caller_value->register_rep < plan->machine_reps_count &&
                    caller_value->memory_rep < plan->machine_reps_count &&
                    callee_value->register_rep < plan->machine_reps_count &&
                    callee_value->memory_rep < plan->machine_reps_count &&
                    plan->machine_reps[caller_value->register_rep].kind ==
                        XR_MACHINE_REP_DYN_VALUE &&
                    plan->machine_reps[caller_value->memory_rep].kind == XR_MACHINE_REP_DYN_VALUE &&
                    plan->machine_reps[callee_value->register_rep].kind == XR_MACHINE_REP_RAW_PTR &&
                    plan->machine_reps[callee_value->memory_rep].kind == XR_MACHINE_REP_RAW_PTR &&
                    plan->machine_reps[caller_value->register_rep].ownership ==
                        plan->machine_reps[caller_value->memory_rep].ownership &&
                    (plan->machine_reps[caller_value->register_rep].ownership ==
                         XR_TARGET_OWNERSHIP_OWNED ||
                     plan->machine_reps[caller_value->register_rep].ownership ==
                         XR_TARGET_OWNERSHIP_BORROWED) &&
                    plan->machine_reps[callee_value->register_rep].ownership ==
                        XR_TARGET_OWNERSHIP_BORROWED &&
                    plan->machine_reps[callee_value->memory_rep].ownership ==
                        XR_TARGET_OWNERSHIP_BORROWED;
                /* The callee always borrows a container it is handed by value.
                 * What the caller holds is its own business: a freshly built
                 * array or a fresh concatenation is owned, a shared read of a
                 * local is borrowed, and all of them hand over the same tagged
                 * carrier. So the two sides agree on representation and may
                 * differ on ownership. */
                bool container_value_borrow_boundary =
                    argument_container_value &&
                    verify_tagged_container_value_boundary(plan, caller_value, callee_value,
                                                           argument_string_value &&
                                                                   argument_string_callee_owns
                                                               ? XR_TARGET_OWNERSHIP_OWNED
                                                               : XR_TARGET_OWNERSHIP_BORROWED);
                /* Rebuild the class hand-over from the semantic parameter. The
                 * same carrier is borrowed or consumed according to that
                 * declaration; the serialized call row does not choose. */
                bool class_instance_boundary =
                    argument_class_instance &&
                    verify_tagged_container_value_boundary(plan, caller_value, callee_value,
                                                           parameter->ownership == XI_OWN_OWNED
                                                               ? XR_TARGET_OWNERSHIP_OWNED
                                                               : XR_TARGET_OWNERSHIP_BORROWED);
                const char *argument_identity_domain =
                    argument_scalar_ref ? "xray-target-direct-scalar-ref-argument-v1"
                    : argument_tagged_ref ? "xray-target-direct-tagged-ref-argument-v2"
                                          : "xray-target-call-argument-v1";
                /* Kept in step with the builder through one shared judgement:
                 * a
                 * raw pointer argument may state a mutability the parameter
                 * does
                 * not ask for, and that is the only way these two types
                 * are
                 * allowed to differ. */
                bool argument_pointer_weakens =
                    parameter && operand->type != parameter->type &&
                    xr_semantic_raw_pointer_argument_satisfies_parameter(
                        xr_semantic_plan_type(semantic, operand->type),
                        xr_semantic_plan_type(semantic, parameter->type));
                bool receiver_slot = method && ordinal == 0;
                valid =
                    parameter &&
                    operand->role ==
                        (receiver_slot ? XR_SEM_OPERAND_RECEIVER : XR_SEM_OPERAND_ARGUMENT) &&
                    operand->parameter ==
                        (method ? (int16_t) ((int32_t) ordinal - 1) : (int16_t) ordinal) &&
                    (operand->type == parameter->type || argument_pointer_weakens) &&
                    operand->parameter_mode == parameter->mode &&
                    operand->transfer_mode == parameter->transfer_mode &&
                    (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) != 0 &&
                    (argument_scalar == 1 || argument_u8_slice || argument_unit_enum ||
                     argument_adt_enum || argument_class_instance || argument_tagged_ref ||
                     argument_container_value || argument_leaf_aggregate) &&
                    (argument_reference ||
                     (parameter->mode == XR_PARAM_READ && operand->access == XR_CALL_ARG_PLAIN &&
                      (operand->flags & XR_SEM_OPERAND_ADDRESSABLE) == 0)) &&
                    /* A String by value is absent from this table because its
 * own judgement
                     * already admitted exactly the two ownerships
                     * a String
                     * parameter may declare and required the call site to state the matching one.
                     */
                    (parameter->ownership == XI_OWN_NONE || argument_string_value ||
                     argument_class_instance ||
                     (argument_adt_enum && parameter->ownership == XI_OWN_OWNED) ||
                     ((argument_u8_slice || argument_unit_enum || argument_adt_enum ||
                       argument_tagged_ref || argument_container_value) &&
                      parameter->ownership == XI_OWN_BORROWED)) &&
                    caller_value && callee_value &&
                    slot_binds_value_in_function(plan, caller_value, operation->function) &&
                    slot_binds_value_in_function(plan, callee_value, target->function) &&
                    reconstruct_call_identity(argument_identity_domain, target->id, parameter->id,
                                              ordinal, &argument_identity) &&
                    xr_stable_id_equal(argument->identity, argument_identity) &&
                    argument->call == i && argument->semantic_operand == semantic_operand &&
                    argument->semantic_value == operand->value &&
                    argument->callee_parameter == parameter_index &&
                    argument->caller_slot == caller_value->slot &&
                    argument->callee_slot == callee_value->slot &&
                    argument->register_rep == caller_value->register_rep &&
                    argument->memory_rep == caller_value->memory_rep &&
                    argument->callee_register_rep == callee_value->register_rep &&
                    argument->callee_memory_rep == callee_value->memory_rep &&
                    ((caller_value->register_rep == callee_value->register_rep &&
                      caller_value->memory_rep == callee_value->memory_rep) ||
                     adt_enum_borrow_boundary || tagged_ref_borrow_boundary ||
                     container_value_borrow_boundary || class_instance_boundary) &&
                    plan->machine_reps[argument->register_rep].kind ==
                        (argument_u8_slice         ? XR_MACHINE_REP_VIEW
                         : argument_unit_enum      ? XR_MACHINE_REP_ENUM_ORDINAL
                         : argument_class_instance ? XR_MACHINE_REP_DYN_VALUE
                                                   : argument_kind) &&
                    argument->ordinal == ordinal &&
                    argument->mode ==
                        (argument_reference ? XR_TARGET_CALL_REFERENCE : XR_TARGET_CALL_VALUE) &&
                    argument->ownership ==
                        (argument_reference ? XR_TARGET_CALL_BORROW : ownership) &&
                    argument->transfer_mode == operand->transfer_mode &&
                    argument->flags ==
                        (argument_reference ? XR_TARGET_CALL_ARGUMENT_ADDRESSABLE : 0) &&
                    argument->array_element_storage == (argument_tagged_ref
                                                            ? array_element_storage
                                                            : XR_TARGET_ARRAY_STORAGE_NONE) &&
                    argument->reserved8[0] == 0 && argument->reserved8[1] == 0 &&
                    argument->reserved8[2] == 0;
                next_argument++;
            }
        } else if (source) {
            valid = source_export && source_export->kind == XR_SEM_SOURCE_EXPORT_FUNCTION &&
                    source_callee &&
                    xr_stable_id_equal(source_export->exported_entity, source_callee->id) &&
                    source_result_type && caller_result_type &&
                    xr_stable_id_equal(source_result_type->id, caller_result_type->id) &&
                    suspends == expected_suspend &&
                    (operation->opcode == XI_CALL_METHOD || operation->opcode == XI_CALL) &&
                    operation->operand_count == (uint32_t) source_callee->parameter_count + 1u &&
                    reconstruct_call_identity("xray-target-call-v5", target->id, operation->id, 0,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == target->dependency &&
                    call->source_export == target->source_export &&
                    xr_stable_id_equal(call->source_export_identity, target->export_identity) &&
                    xr_stable_id_equal(call->source_callee_identity, target->callee_function) &&
                    call->argument_count == source_callee->parameter_count &&
                    call->flags == (suspends ? XR_TARGET_CALL_SUSPEND : 0) &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_SOURCE_EXPORT &&
                    call->target_kind == XR_TARGET_CALL_TARGET_SOURCE_EXPORT;
            if (!valid)
                break;
            for (uint32_t ordinal = 0; valid && ordinal < call->argument_count; ordinal++) {
                const XrTargetCallArgumentRecord *argument = &plan->call_arguments[next_argument];
                uint32_t parameter_index = source_callee->parameter_begin + ordinal;
                uint32_t semantic_operand = operation->operand_begin + ordinal + 1u;
                const XrSemanticParameterRecord *parameter =
                    xr_semantic_plan_parameter(dependency, parameter_index);
                const XrSemanticOperandRecord *operand = &operands[semantic_operand];
                const XrSemanticTypeRecord *parameter_type =
                    parameter ? xr_semantic_plan_type(dependency, parameter->type) : NULL;
                const XrSemanticTypeRecord *operand_type =
                    xr_semantic_plan_type(semantic, operand->type);
                const XrTargetValueRepRecord *caller_value =
                    xr_target_plan_value_rep(plan, operand->value);
                bool reference = parameter && parameter->mode == XR_PARAM_REF;
                bool read = parameter && parameter->mode == XR_PARAM_READ;
                bool addressable = (operand->flags & XR_SEM_OPERAND_ADDRESSABLE) != 0;
                uint8_t expected_mode = reference ? XR_TARGET_CALL_REFERENCE : XR_TARGET_CALL_VALUE;
                uint8_t expected_ownership = reference ? XR_TARGET_CALL_WRITEBACK
                                             : operand->ownership_action == XR_SEM_OPERAND_CONSUME
                                                 ? XR_TARGET_CALL_CONSUME
                                                 : XR_TARGET_CALL_READ;
                uint8_t expected_flags = reference ? XR_TARGET_CALL_ARGUMENT_ADDRESSABLE : 0;
                XrStableId argument_identity;
                valid = parameter && parameter_type && operand_type && caller_value &&
                        parameter->function == source_export->function &&
                        parameter->ordinal == ordinal &&
                        xr_semantic_parameter_type_admits_argument(dependency, parameter_type,
                                                                   operand_type) &&
                        operand->role == XR_SEM_OPERAND_ARGUMENT &&
                        operand->parameter == (int16_t) ordinal &&
                        operand->parameter_mode == parameter->mode &&
                        operand->transfer_mode == parameter->transfer_mode &&
                        (operand->flags & XR_SEM_OPERAND_CALL_CONTRACT) != 0 &&
                        (read || reference) &&
                        (!read || (operand->access == XR_CALL_ARG_PLAIN && !addressable)) &&
                        (!reference || (operand->access == XR_CALL_ARG_REF && addressable &&
                                        operand->ownership_action == XR_SEM_OPERAND_BORROW)) &&
                        slot_binds_value_in_function(plan, caller_value, operation->function) &&
                        reconstruct_call_identity("xray-target-source-call-argument-v1", target->id,
                                                  parameter->id, ordinal, &argument_identity) &&
                        xr_stable_id_equal(argument->identity, argument_identity) &&
                        argument->call == i && argument->semantic_operand == semantic_operand &&
                        argument->semantic_value == operand->value &&
                        argument->callee_parameter == parameter_index &&
                        argument->caller_slot == caller_value->slot &&
                        argument->callee_slot == XR_SEMANTIC_INDEX_NONE &&
                        argument->register_rep == caller_value->register_rep &&
                        argument->memory_rep == caller_value->memory_rep &&
                        argument->callee_register_rep == caller_value->register_rep &&
                        argument->callee_memory_rep == caller_value->memory_rep &&
                        argument->ordinal == ordinal && argument->mode == expected_mode &&
                        argument->ownership == expected_ownership &&
                        argument->transfer_mode == operand->transfer_mode &&
                        argument->flags == expected_flags;
                next_argument++;
            }
        } else if (native_namespace) {
            valid =
                target && suspends && expected_suspend && operation->opcode == XI_CALL_METHOD &&
                operation->operand_count >= 1 && target->function == XR_SEMANTIC_INDEX_NONE &&
                target->dependency == XR_SEMANTIC_INDEX_NONE &&
                target->source_export == XR_SEMANTIC_INDEX_NONE &&
                target->callable_type == XR_SEMANTIC_INDEX_NONE &&
                reconstruct_call_identity("xray-target-native-namespace-yieldable-v1", target->id,
                                          operation->id, operation->operand_count - 1u,
                                          &expected_identity) &&
                xr_stable_id_equal(call->identity, expected_identity) &&
                call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                call->source_export == XR_SEMANTIC_INDEX_NONE &&
                stable_id_is_zero(call->source_export_identity) &&
                stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                call->flags == XR_TARGET_CALL_SUSPEND &&
                call->calling_convention == XR_TARGET_CALL_CONVENTION_NATIVE_NAMESPACE_YIELDABLE &&
                call->target_kind == XR_TARGET_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE;
            if (!valid)
                break;
        } else if (builtin_instance) {
            /* The roster entry the receiver's frozen builtin id and arity select
             * is the whole authority: the row names no callee, no dependency and
             * no export, it always suspends, and the receiver is the dispatch
             * target rather than an argument the row would have to bind. */
            const XrSemanticTypeRecord *receiver_record =
                xr_semantic_plan_type(semantic, builtin_receiver_type);
            valid =
                suspends && receiver_record &&
                reconstruct_call_identity("xray-target-builtin-instance-yieldable-v1", target->id,
                                          receiver_record->id, operation->operand_count - 1u,
                                          &expected_identity) &&
                xr_stable_id_equal(call->identity, expected_identity) &&
                call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                call->source_export == XR_SEMANTIC_INDEX_NONE &&
                stable_id_is_zero(call->source_export_identity) &&
                stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                call->flags == XR_TARGET_CALL_SUSPEND &&
                call->calling_convention == XR_TARGET_CALL_CONVENTION_BUILTIN_INSTANCE_YIELDABLE &&
                call->target_kind == XR_TARGET_CALL_TARGET_BUILTIN_INSTANCE_YIELDABLE;
            if (!valid)
                break;
        } else if (class_construction) {
            /* The construction passes the declaration's own constructor
             * parameters
             * after its receiver. The shared judgement has already
             * proved that
             * contract, so what stays this loop's own is that every
             * argument row
             * names the operand and the parameter the contract
             * fixed, and that
             * caller and callee agree on one representation. */
            uint32_t constructor_function =
                imported_class_construction
                    ? imported_constructor
                    : xr_semantic_class_constructor_function(
                          semantic,
                          xr_semantic_class_construction_source_class(semantic, operation));
            const XrSemanticFunctionRecord *constructor_callee =
                constructor_function != XR_SEMANTIC_INDEX_NONE
                    ? xr_semantic_plan_function(imported_class_construction ? dependency : semantic,
                                                constructor_function)
                    : NULL;
            bool constructor_identity =
                !imported_class_construction ||
                (source_export &&
                 xr_stable_id_equal(call->source_export_identity, source_export->id) &&
                 xr_stable_id_equal(target->export_identity, source_export->id) &&
                 ((constructor_callee &&
                   xr_stable_id_equal(target->callee_function, constructor_callee->id) &&
                   xr_stable_id_equal(call->source_callee_identity, constructor_callee->id)) ||
                  (!constructor_callee && stable_id_is_zero(target->callee_function) &&
                   stable_id_is_zero(call->source_callee_identity))));
            valid =
                !suspends && operation->opcode == XI_CALL &&
                reconstruct_call_identity("xray-target-source-class-constructor-v1", target->id,
                                          operation->id, 0, &expected_identity) &&
                xr_stable_id_equal(call->identity, expected_identity) &&
                call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                call->source_dependency ==
                    (imported_class_construction ? target->dependency : XR_SEMANTIC_INDEX_NONE) &&
                call->source_export == (imported_class_construction ? target->source_export
                                                                    : XR_SEMANTIC_INDEX_NONE) &&
                constructor_identity &&
                (imported_class_construction ||
                 (stable_id_is_zero(call->source_export_identity) &&
                  stable_id_is_zero(call->source_callee_identity))) &&
                call->argument_count == operation->operand_count - 1u &&
                (call->argument_count == 0 || constructor_callee) && call->flags == 0 &&
                call->calling_convention == XR_TARGET_CALL_CONVENTION_SOURCE_CLASS_CONSTRUCTOR &&
                call->target_kind == XR_TARGET_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR &&
                call->result_ownership == XR_TARGET_CALL_RETURN_OWNED && result &&
                result->slot < plan->slots_count &&
                plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED;
            if (!valid)
                break;
            for (uint32_t ordinal = 0; valid && ordinal < call->argument_count; ordinal++) {
                const XrTargetCallArgumentRecord *argument = &plan->call_arguments[next_argument];
                uint32_t parameter_index = constructor_callee->parameter_begin + 1u + ordinal;
                uint32_t semantic_operand = operation->operand_begin + ordinal + 1u;
                const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(
                    imported_class_construction ? dependency : semantic, parameter_index);
                const XrSemanticOperandRecord *operand = &operands[semantic_operand];
                const XrTargetValueRepRecord *caller_value =
                    xr_target_plan_value_rep(plan, operand->value);
                const XrTargetValueRepRecord *callee_value =
                    !imported_class_construction && parameter
                        ? xr_target_plan_value_rep(plan, parameter->value)
                        : NULL;
                const XrSemanticTypeRecord *operand_type =
                    xr_semantic_plan_type(semantic, operand->type);
                const XrSemanticTypeRecord *parameter_type =
                    parameter
                        ? xr_semantic_plan_type(imported_class_construction ? dependency : semantic,
                                                parameter->type)
                        : NULL;
                XrStableId argument_identity;
                uint16_t argument_kind = XR_MACHINE_REP_COUNT;
                int argument_scalar =
                    operand->type < xr_semantic_plan_type_count(semantic)
                        ? semantic_type_expected_rep(xr_semantic_plan_type(semantic, operand->type),
                                                     &argument_kind)
                        : -1;
                uint8_t ownership = operand->ownership_action == XR_SEM_OPERAND_CONSUME
                                        ? XR_TARGET_CALL_CONSUME
                                        : XR_TARGET_CALL_READ;
                bool imported_storage = imported_class_construction && caller_value &&
                                        operand_type && parameter_type &&
                                        xr_stable_id_equal(operand_type->id, parameter_type->id);
                valid =
                    parameter && caller_value &&
                    (imported_storage || (argument_scalar == 1 && callee_value)) &&
                    slot_binds_value_in_function(plan, caller_value, operation->function) &&
                    (imported_class_construction ||
                     (slot_binds_value_in_function(plan, callee_value, constructor_function) &&
                      caller_value->register_rep == callee_value->register_rep &&
                      caller_value->memory_rep == callee_value->memory_rep)) &&
                    reconstruct_call_identity("xray-target-call-argument-v1", target->id,
                                              parameter->id, ordinal, &argument_identity) &&
                    xr_stable_id_equal(argument->identity, argument_identity) &&
                    argument->call == i && argument->semantic_operand == semantic_operand &&
                    argument->semantic_value == operand->value &&
                    argument->callee_parameter == parameter_index &&
                    argument->caller_slot == caller_value->slot &&
                    argument->callee_slot == (imported_class_construction ? XR_SEMANTIC_INDEX_NONE
                                                                          : callee_value->slot) &&
                    argument->register_rep == caller_value->register_rep &&
                    argument->memory_rep == caller_value->memory_rep &&
                    argument->callee_register_rep == (imported_class_construction
                                                          ? caller_value->register_rep
                                                          : callee_value->register_rep) &&
                    argument->callee_memory_rep == (imported_class_construction
                                                        ? caller_value->memory_rep
                                                        : callee_value->memory_rep) &&
                    (imported_class_construction ||
                     plan->machine_reps[argument->register_rep].kind == argument_kind) &&
                    argument->ordinal == ordinal && argument->mode == XR_TARGET_CALL_VALUE &&
                    argument->ownership == ownership &&
                    argument->transfer_mode == operand->transfer_mode && argument->flags == 0 &&
                    argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
                    argument->reserved8[0] == 0 && argument->reserved8[1] == 0 &&
                    argument->reserved8[2] == 0;
                next_argument++;
            }
            if (!valid)
                break;
        } else if (array_hof) {
            const XrSemanticFunctionRecord *callee =
                xr_semantic_plan_function(semantic, operation->callable_function);
            uint32_t discriminator = ((uint32_t) array_hof_kind << 16) |
                                     ((uint32_t) array_hof_source_storage << 8) |
                                     array_hof_result_storage;
            valid = callee && !suspends &&
                    reconstruct_call_identity("xray-target-array-hof-v1", operation->id, callee->id,
                                              discriminator, &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == operation->callable_function &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) &&
                    call->argument_count == operation->operand_count && call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_ARRAY_HOF &&
                    call->target_kind == XR_TARGET_CALL_TARGET_ARRAY_HOF &&
                    call->result_ownership == (array_hof_kind == XR_TARGET_ARRAY_HOF_REDUCE
                                                   ? XR_TARGET_CALL_NONE
                                                   : XR_TARGET_CALL_RETURN_OWNED) &&
                    result && result->slot < plan->slots_count &&
                    (array_hof_kind == XR_TARGET_ARRAY_HOF_REDUCE
                         ? plan->slots[result->slot].root_kind == XR_TARGET_ROOT_NONE &&
                               plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_TRIVIAL
                         : plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                               plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED);
            for (uint16_t ordinal = 0; valid && ordinal < call->argument_count; ordinal++) {
                uint32_t semantic_operand = operation->operand_begin + ordinal;
                const XrSemanticOperandRecord *operand = &operands[semantic_operand];
                const XrSemanticTypeRecord *operand_type =
                    xr_semantic_plan_type(semantic, operand->type);
                const XrTargetValueRepRecord *caller_value =
                    xr_target_plan_value_rep(plan, operand->value);
                const XrTargetCallArgumentRecord *argument = &plan->call_arguments[next_argument];
                uint32_t expected_value = ordinal == 0   ? array_hof_receiver
                                          : ordinal == 1 ? array_hof_callback
                                                         : array_hof_initial;
                XrStableId argument_identity;
                valid =
                    operand_type && caller_value && operand->value == expected_value &&
                    slot_binds_value_in_function(plan, caller_value, operation->function) &&
                    reconstruct_call_identity("xray-target-array-hof-argument-v1", operation->id,
                                              operand_type->id, ordinal, &argument_identity) &&
                    xr_stable_id_equal(argument->identity, argument_identity) &&
                    argument->call == i && argument->semantic_operand == semantic_operand &&
                    argument->semantic_value == expected_value &&
                    argument->callee_parameter == XR_SEMANTIC_INDEX_NONE &&
                    argument->caller_slot == caller_value->slot &&
                    argument->callee_slot == XR_SEMANTIC_INDEX_NONE &&
                    argument->register_rep == caller_value->register_rep &&
                    argument->memory_rep == caller_value->memory_rep &&
                    argument->callee_register_rep == caller_value->register_rep &&
                    argument->callee_memory_rep == caller_value->memory_rep &&
                    argument->ordinal == ordinal && argument->mode == XR_TARGET_CALL_VALUE &&
                    argument->ownership ==
                        (ordinal == 0 ? XR_TARGET_CALL_BORROW : XR_TARGET_CALL_CONSUME) &&
                    argument->transfer_mode == operand->transfer_mode && argument->flags == 0 &&
                    argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
                    argument->reserved8[0] == 0 && argument->reserved8[1] == 0 &&
                    argument->reserved8[2] == 0;
                next_argument++;
            }
            if (!valid)
                break;
        } else if (array_fill) {
            const XrSemanticTypeRecord *receiver_type =
                xr_semantic_plan_type(semantic, operation->result_type);
            valid = !suspends && receiver_type &&
                    reconstruct_call_identity("xray-target-array-fill-scalar-v1", operation->id,
                                              receiver_type->id, array_fill_storage,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 2 &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_ARRAY_FILL_SCALAR &&
                    call->target_kind == XR_TARGET_CALL_TARGET_ARRAY_FILL_SCALAR &&
                    call->result_ownership == XR_TARGET_CALL_NONE && result &&
                    result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                    plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED;
            for (uint16_t ordinal = 0; valid && ordinal < 2; ordinal++) {
                uint32_t semantic_operand = operation->operand_begin + ordinal;
                const XrSemanticOperandRecord *operand = &operands[semantic_operand];
                const XrSemanticTypeRecord *operand_type =
                    xr_semantic_plan_type(semantic, operand->type);
                const XrTargetValueRepRecord *caller_value =
                    xr_target_plan_value_rep(plan, operand->value);
                const XrTargetCallArgumentRecord *argument = &plan->call_arguments[next_argument];
                XrStableId argument_identity;
                valid = operand_type && caller_value &&
                        operand->value == (ordinal == 0 ? array_fill_receiver : array_fill_value) &&
                        reconstruct_call_identity("xray-target-array-fill-scalar-argument-v1",
                                                  operation->id, operand_type->id, ordinal,
                                                  &argument_identity) &&
                        xr_stable_id_equal(argument->identity, argument_identity) &&
                        argument->call == i && argument->semantic_operand == semantic_operand &&
                        argument->semantic_value == operand->value &&
                        argument->callee_parameter == XR_SEMANTIC_INDEX_NONE &&
                        argument->caller_slot == caller_value->slot &&
                        argument->callee_slot == XR_SEMANTIC_INDEX_NONE &&
                        argument->register_rep == caller_value->register_rep &&
                        argument->memory_rep == caller_value->memory_rep &&
                        argument->callee_register_rep == caller_value->register_rep &&
                        argument->callee_memory_rep == caller_value->memory_rep &&
                        argument->ordinal == ordinal && argument->mode == XR_TARGET_CALL_VALUE &&
                        argument->ownership ==
                            (ordinal == 0 ? XR_TARGET_CALL_BORROW : XR_TARGET_CALL_CONSUME) &&
                        argument->transfer_mode == operand->transfer_mode && argument->flags == 0 &&
                        argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
                        argument->reserved8[0] == 0 && argument->reserved8[1] == 0 &&
                        argument->reserved8[2] == 0;
                next_argument++;
            }
            if (!valid)
                break;
        } else if (array_intrinsic) {
            uint32_t discriminator =
                ((uint32_t) array_intrinsic_kind << 8) | array_intrinsic_storage;
            valid = !suspends &&
                    reconstruct_call_identity("xray-target-array-intrinsic-v1", operation->id,
                                              operation->allocation_id, discriminator,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) &&
                    call->argument_count == operation->operand_count && call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_ARRAY_INTRINSIC &&
                    call->target_kind == XR_TARGET_CALL_TARGET_ARRAY_INTRINSIC &&
                    call->result_ownership == XR_TARGET_CALL_RETURN_OWNED && result &&
                    result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                    plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED;
            for (uint16_t ordinal = 0; valid && ordinal < call->argument_count; ordinal++) {
                uint32_t semantic_operand = operation->operand_begin + ordinal;
                const XrSemanticOperandRecord *operand = &operands[semantic_operand];
                const XrSemanticTypeRecord *operand_type =
                    xr_semantic_plan_type(semantic, operand->type);
                const XrTargetValueRepRecord *caller_value =
                    xr_target_plan_value_rep(plan, operand->value);
                const XrTargetCallArgumentRecord *argument = &plan->call_arguments[next_argument];
                XrStableId argument_identity;
                valid = operand_type && caller_value &&
                        reconstruct_call_identity("xray-target-array-intrinsic-argument-v1",
                                                  operation->id, operand_type->id, ordinal,
                                                  &argument_identity) &&
                        xr_stable_id_equal(argument->identity, argument_identity) &&
                        argument->call == i && argument->semantic_operand == semantic_operand &&
                        argument->semantic_value == operand->value &&
                        argument->callee_parameter == XR_SEMANTIC_INDEX_NONE &&
                        argument->caller_slot == caller_value->slot &&
                        argument->callee_slot == XR_SEMANTIC_INDEX_NONE &&
                        argument->register_rep == caller_value->register_rep &&
                        argument->memory_rep == caller_value->memory_rep &&
                        argument->callee_register_rep == caller_value->register_rep &&
                        argument->callee_memory_rep == caller_value->memory_rep &&
                        argument->ordinal == ordinal && argument->mode == XR_TARGET_CALL_VALUE &&
                        argument->ownership == XR_TARGET_CALL_CONSUME &&
                        argument->transfer_mode == operand->transfer_mode && argument->flags == 0 &&
                        argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
                        argument->reserved8[0] == 0 && argument->reserved8[1] == 0 &&
                        argument->reserved8[2] == 0;
                next_argument++;
            }
            if (!valid)
                break;
        } else if (adt_enum_constructor) {
            valid = !suspends && operation->opcode == XI_CALL_METHOD &&
                    reconstruct_call_identity(
                        "xray-target-adt-enum-constructor-v1", operation->id, result_type->id,
                        enum_constructor_shape.member_ordinal, &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_ADT_ENUM_CONSTRUCTOR &&
                    call->target_kind == XR_TARGET_CALL_TARGET_ADT_ENUM_CONSTRUCTOR &&
                    call->result_ownership == XR_TARGET_CALL_RETURN_OWNED &&
                    result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                    plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED;
            if (!valid)
                break;
        } else if (channel_close) {
            const XrSemanticTypeRecord *receiver_type =
                xr_semantic_plan_type(semantic, receiver_type_index);
            valid = channel_close && receiver_type && !suspends &&
                    reconstruct_call_identity(
                        "xray-target-call-v5", operation->id, receiver_type->id,
                        (uint32_t) operation->semantic_immediate, &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_CHANNEL_CLOSE &&
                    call->target_kind == XR_TARGET_CALL_TARGET_CHANNEL_CLOSE;
            if (!valid)
                break;
        } else if (container_copy) {
            /* The result is a fresh allocation the allocation family bound, and
             * the call hands its ownership to the caller -- which is the one
             * field that separates this row from the scalar spelling. */
            valid = result_type && !suspends &&
                    reconstruct_call_identity("xray-target-container-copy-v1", operation->id,
                                              result_type->id, container_copy_argument,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_CONTAINER_COPY &&
                    call->target_kind == XR_TARGET_CALL_TARGET_CONTAINER_COPY &&
                    call->array_element_storage == container_copy_storage &&
                    call->result_ownership == XR_TARGET_CALL_RETURN_OWNED;
            if (!valid)
                break;
        } else if (scalar_copy) {
            /* The result is a scalar the scalar family already bound, so this
             * row states no returned ownership and no slot of its own: what it
             * proves is the dispatch, not the storage. */
            valid = result_type && !suspends &&
                    reconstruct_call_identity("xray-target-scalar-copy-v1", operation->id,
                                              result_type->id, scalar_copy_argument,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_SCALAR_COPY &&
                    call->target_kind == XR_TARGET_CALL_TARGET_SCALAR_COPY &&
                    call->result_ownership == XR_TARGET_CALL_NONE;
            if (!valid)
                break;
        } else if (stringbuilder_constructor) {
            valid =
                stringbuilder_constructor && !suspends &&
                reconstruct_call_identity("xray-target-stringbuilder-constructor-v1", operation->id,
                                          operation->allocation_id, 0, &expected_identity) &&
                xr_stable_id_equal(call->identity, expected_identity) &&
                call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                call->source_export == XR_SEMANTIC_INDEX_NONE &&
                stable_id_is_zero(call->source_export_identity) &&
                stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                call->flags == 0 &&
                call->calling_convention == XR_TARGET_CALL_CONVENTION_STRINGBUILDER_CONSTRUCTOR &&
                call->target_kind == XR_TARGET_CALL_TARGET_STRINGBUILDER_CONSTRUCTOR && result &&
                result->slot < plan->slots_count &&
                plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED;
            if (!valid)
                break;
        } else if (stringbuilder_append_rune) {
            const XrSemanticTypeRecord *receiver_type =
                xr_semantic_plan_type(semantic, operation->result_type);
            bool borrowed = operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED;
            valid =
                receiver_type && !suspends &&
                reconstruct_call_identity("xray-target-stringbuilder-append-rune-v1", operation->id,
                                          receiver_type->id, append_argument, &expected_identity) &&
                xr_stable_id_equal(call->identity, expected_identity) &&
                call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                call->source_export == XR_SEMANTIC_INDEX_NONE &&
                stable_id_is_zero(call->source_export_identity) &&
                stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                call->flags == 0 &&
                call->calling_convention == XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_RUNE &&
                call->target_kind == XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_RUNE &&
                call->result_ownership ==
                    (borrowed ? XR_TARGET_CALL_BORROW : XR_TARGET_CALL_RETURN_OWNED) &&
                result && result->slot < plan->slots_count &&
                plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                plan->slots[result->slot].ownership ==
                    (borrowed ? XR_TARGET_OWNERSHIP_BORROWED : XR_TARGET_OWNERSHIP_OWNED);
            if (!valid)
                break;
        } else if (string_runes) {
            valid = result_type && !suspends &&
                    reconstruct_call_identity("xray-target-string-runes-v1", operation->id,
                                              result_type->id, string_runes_receiver,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_STRING_RUNES &&
                    call->target_kind == XR_TARGET_CALL_TARGET_STRING_RUNES &&
                    call->result_ownership == XR_TARGET_CALL_RETURN_OWNED && result &&
                    result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                    plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED;
            if (!valid)
                break;
        } else if (iterator_rune_has_next) {
            valid = result_type && !suspends &&
                    reconstruct_call_identity(
                        "xray-target-iterator-rune-has-next-v1", operation->id, result_type->id,
                        iterator_rune_has_next_receiver, &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_HAS_NEXT &&
                    call->target_kind == XR_TARGET_CALL_TARGET_ITERATOR_RUNE_HAS_NEXT &&
                    call->result_ownership == XR_TARGET_CALL_NONE && result &&
                    result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == XR_TARGET_ROOT_NONE &&
                    plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
            if (!valid)
                break;
        } else if (iterator_rune_next) {
            valid = result_type && result_kind == XR_MACHINE_REP_RUNE && !suspends &&
                    reconstruct_call_identity("xray-target-iterator-rune-next-v1", operation->id,
                                              result_type->id, iterator_rune_next_receiver,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_NEXT &&
                    call->target_kind == XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NEXT &&
                    call->result_ownership == XR_TARGET_CALL_NONE && result &&
                    result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == XR_TARGET_ROOT_NONE &&
                    plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
            if (!valid)
                break;
        } else if (iterator_rune_nth) {
            uint32_t semantic_operand = operation->operand_begin + 1u;
            const XrSemanticOperandRecord *operand =
                semantic_operand < operand_count ? &operands[semantic_operand] : NULL;
            const XrSemanticTypeRecord *operand_type =
                operand ? xr_semantic_plan_type(semantic, operand->type) : NULL;
            const XrTargetValueRepRecord *caller_value =
                operand ? xr_target_plan_value_rep(plan, operand->value) : NULL;
            const XrTargetCallArgumentRecord *argument = next_argument < plan->call_arguments_count
                                                             ? &plan->call_arguments[next_argument]
                                                             : NULL;
            XrStableId argument_identity;
            uint16_t argument_kind = XR_MACHINE_REP_COUNT;
            valid =
                result_type && result_kind == XR_MACHINE_REP_RUNE && !suspends && operand &&
                operand_type && caller_value && argument &&
                operand->value == iterator_rune_nth_index &&
                semantic_type_expected_rep(operand_type, &argument_kind) == 1 &&
                argument_kind == XR_MACHINE_REP_I64 &&
                slot_binds_value_in_function(plan, caller_value, operation->function) &&
                reconstruct_call_identity("xray-target-iterator-rune-nth-v1", operation->id,
                                          result_type->id, iterator_rune_nth_receiver,
                                          &expected_identity) &&
                xr_stable_id_equal(call->identity, expected_identity) &&
                reconstruct_call_identity("xray-target-iterator-rune-nth-argument-v1",
                                          operation->id, operand_type->id, 0, &argument_identity) &&
                xr_stable_id_equal(argument->identity, argument_identity) &&
                call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                call->source_export == XR_SEMANTIC_INDEX_NONE &&
                stable_id_is_zero(call->source_export_identity) &&
                stable_id_is_zero(call->source_callee_identity) && call->argument_count == 1 &&
                call->flags == 0 &&
                call->calling_convention == XR_TARGET_CALL_CONVENTION_ITERATOR_RUNE_NTH &&
                call->target_kind == XR_TARGET_CALL_TARGET_ITERATOR_RUNE_NTH &&
                call->result_ownership == XR_TARGET_CALL_NONE && result &&
                result->slot < plan->slots_count &&
                plan->slots[result->slot].root_kind == XR_TARGET_ROOT_NONE &&
                plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
                argument->call == i && argument->semantic_operand == semantic_operand &&
                argument->semantic_value == iterator_rune_nth_index &&
                argument->callee_parameter == XR_SEMANTIC_INDEX_NONE &&
                argument->caller_slot == caller_value->slot &&
                argument->callee_slot == XR_SEMANTIC_INDEX_NONE &&
                argument->register_rep < plan->machine_reps_count &&
                argument->memory_rep < plan->machine_reps_count &&
                argument->callee_register_rep < plan->machine_reps_count &&
                argument->callee_memory_rep < plan->machine_reps_count &&
                argument->register_rep == caller_value->register_rep &&
                argument->memory_rep == caller_value->memory_rep &&
                argument->callee_register_rep == caller_value->register_rep &&
                argument->callee_memory_rep == caller_value->memory_rep &&
                plan->machine_reps[argument->register_rep].kind == XR_MACHINE_REP_I64 &&
                plan->machine_reps[argument->memory_rep].kind == XR_MACHINE_REP_I64 &&
                argument->ordinal == 0 && argument->mode == XR_TARGET_CALL_VALUE &&
                argument->ownership == XR_TARGET_CALL_CONSUME &&
                argument->transfer_mode == operand->transfer_mode && argument->flags == 0 &&
                argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
                argument->reserved8[0] == 0 && argument->reserved8[1] == 0 &&
                argument->reserved8[2] == 0;
            next_argument++;
            if (!valid)
                break;
        } else if (map_entries_iterator || map_entry_iterator_has_next || map_entry_iterator_next) {
            const char *domain = map_entries_iterator ? "xray-target-map-entries-iterator-v1"
                                 : map_entry_iterator_has_next
                                     ? "xray-target-map-entry-iterator-has-next-v1"
                                     : "xray-target-map-entry-iterator-next-v1";
            uint8_t convention = map_entries_iterator
                                     ? XR_TARGET_CALL_CONVENTION_MAP_ENTRIES_ITERATOR
                                 : map_entry_iterator_has_next
                                     ? XR_TARGET_CALL_CONVENTION_MAP_ENTRY_ITERATOR_HAS_NEXT
                                     : XR_TARGET_CALL_CONVENTION_MAP_ENTRY_ITERATOR_NEXT;
            uint8_t target_kind = map_entries_iterator ? XR_TARGET_CALL_TARGET_MAP_ENTRIES_ITERATOR
                                  : map_entry_iterator_has_next
                                      ? XR_TARGET_CALL_TARGET_MAP_ENTRY_ITERATOR_HAS_NEXT
                                      : XR_TARGET_CALL_TARGET_MAP_ENTRY_ITERATOR_NEXT;
            uint8_t expected_ownership =
                map_entry_iterator_has_next ? XR_TARGET_CALL_NONE : XR_TARGET_CALL_RETURN_OWNED;
            uint8_t expected_root =
                map_entry_iterator_has_next ? XR_TARGET_ROOT_NONE : XR_TARGET_ROOT_DYNAMIC;
            uint8_t expected_slot_ownership = map_entry_iterator_has_next
                                                  ? XR_TARGET_OWNERSHIP_TRIVIAL
                                                  : XR_TARGET_OWNERSHIP_OWNED;
            valid = result_type && !suspends &&
                    reconstruct_call_identity(domain, operation->id, result_type->id,
                                              map_entry_iterator_receiver, &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == 0 && call->calling_convention == convention &&
                    call->target_kind == target_kind &&
                    call->result_ownership == expected_ownership && result &&
                    result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == expected_root &&
                    plan->slots[result->slot].ownership == expected_slot_ownership;
            if (!valid)
                break;
        } else if (rune_to_uint32) {
            valid = result_type && result_kind == XR_MACHINE_REP_U32 && !suspends &&
                    reconstruct_call_identity("xray-target-rune-to-uint32-v1", operation->id,
                                              result_type->id, rune_to_uint32_receiver,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_RUNE_TO_UINT32 &&
                    call->target_kind == XR_TARGET_CALL_TARGET_RUNE_TO_UINT32 &&
                    call->result_ownership == XR_TARGET_CALL_NONE && result &&
                    result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == XR_TARGET_ROOT_NONE &&
                    plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
            if (!valid)
                break;
        } else if (rune_to_string) {
            const XrTargetSlotRecord *result_slot =
                result && result->slot < plan->slots_count ? &plan->slots[result->slot] : NULL;
            const XrTargetMachineRepRecord *result_register =
                result && result->register_rep < plan->machine_reps_count
                    ? &plan->machine_reps[result->register_rep]
                    : NULL;
            const XrTargetMachineRepRecord *result_memory =
                result && result->memory_rep < plan->machine_reps_count
                    ? &plan->machine_reps[result->memory_rep]
                    : NULL;
            valid = result_type && result_kind == XR_MACHINE_REP_DYN_VALUE && !suspends &&
                    reconstruct_call_identity("xray-target-rune-to-string-v1", operation->id,
                                              result_type->id, rune_to_string_receiver,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_RUNE_TO_STRING &&
                    call->target_kind == XR_TARGET_CALL_TARGET_RUNE_TO_STRING &&
                    call->result_ownership == XR_TARGET_CALL_RETURN_OWNED && result &&
                    result_slot && result_register && result_memory &&
                    result_register->kind == XR_MACHINE_REP_DYN_VALUE &&
                    result_memory->kind == XR_MACHINE_REP_DYN_VALUE &&
                    result_register->root_kind == XR_TARGET_ROOT_DYNAMIC &&
                    result_memory->root_kind == XR_TARGET_ROOT_DYNAMIC &&
                    result_register->ownership == XR_TARGET_OWNERSHIP_OWNED &&
                    result_memory->ownership == XR_TARGET_OWNERSHIP_OWNED &&
                    result_register->null_encoding == XR_TARGET_NULL_TAGGED &&
                    result_memory->null_encoding == XR_TARGET_NULL_TAGGED &&
                    result_slot->function == operation->function &&
                    result_slot->semantic_value == operation->result_value &&
                    result_slot->semantic_operation == call->semantic_operation &&
                    result_slot->logical_slot == XR_SEMANTIC_INDEX_NONE &&
                    result_slot->role == XR_TARGET_SLOT_TEMPORARY &&
                    result_slot->register_rep == result->register_rep &&
                    result_slot->memory_rep == result->memory_rep &&
                    result_slot->root_kind == XR_TARGET_ROOT_DYNAMIC &&
                    result_slot->ownership == XR_TARGET_OWNERSHIP_OWNED &&
                    result_slot->debug_variable == XR_SEMANTIC_INDEX_NONE &&
                    result_slot->reserved == 0;
            if (!valid)
                break;
        } else if (rune_is_whitespace) {
            valid = result_type && result_kind == XR_MACHINE_REP_I1 && !suspends &&
                    reconstruct_call_identity("xray-target-rune-is-whitespace-v1", operation->id,
                                              result_type->id, rune_is_whitespace_receiver,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_RUNE_IS_WHITESPACE &&
                    call->target_kind == XR_TARGET_CALL_TARGET_RUNE_IS_WHITESPACE &&
                    call->result_ownership == XR_TARGET_CALL_NONE && result &&
                    result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == XR_TARGET_ROOT_NONE &&
                    plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
            if (!valid)
                break;
        } else if (string_slice_range) {
            valid = result_type && !suspends &&
                    reconstruct_call_identity("xray-target-string-slice-range-v1", operation->id,
                                              result_type->id, string_slice_receiver,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == XR_TARGET_CALL_TAIL &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_STRING_SLICE_RANGE &&
                    call->target_kind == XR_TARGET_CALL_TARGET_STRING_SLICE_RANGE &&
                    call->result_ownership == XR_TARGET_CALL_RETURN_OWNED && result &&
                    result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                    plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED;
            (void) string_slice_start;
            (void) string_slice_end;
            if (!valid)
                break;
        } else if (stringbuilder_to_string) {
            valid = result_type && !suspends &&
                    reconstruct_call_identity("xray-target-stringbuilder-to-string-v1",
                                              operation->id, result_type->id, to_string_receiver,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_STRINGBUILDER_TO_STRING &&
                    call->target_kind == XR_TARGET_CALL_TARGET_STRINGBUILDER_TO_STRING &&
                    call->result_ownership == XR_TARGET_CALL_RETURN_OWNED && result &&
                    result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                    plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED;
            if (!valid)
                break;
        } else if (stringbuilder_append_string) {
            bool borrowed = operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED;
            valid =
                result_type && !suspends &&
                reconstruct_call_identity("xray-target-stringbuilder-append-string-v1",
                                          operation->id, result_type->id, append_string_argument,
                                          &expected_identity) &&
                xr_stable_id_equal(call->identity, expected_identity) &&
                call->semantic_call_target == XR_SEMANTIC_INDEX_NONE && call->argument_count == 0 &&
                call->flags == 0 &&
                call->calling_convention == XR_TARGET_CALL_CONVENTION_STRINGBUILDER_APPEND_STRING &&
                call->target_kind == XR_TARGET_CALL_TARGET_STRINGBUILDER_APPEND_STRING &&
                call->result_ownership ==
                    (borrowed ? XR_TARGET_CALL_BORROW : XR_TARGET_CALL_RETURN_OWNED) &&
                result && result->slot < plan->slots_count &&
                plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                plan->slots[result->slot].ownership ==
                    (borrowed ? XR_TARGET_OWNERSHIP_BORROWED : XR_TARGET_OWNERSHIP_OWNED);
            if (!valid)
                break;
        } else if (panic_info_constructor) {
            /* The class is compiler-owned, so no source export, dependency, or
             * callee index can name it: the row carries the sealed identity
             * alone and the result owns the record the constructor allocates. */
            valid = result_type && !suspends &&
                    reconstruct_call_identity("xray-target-panic-info-constructor-v1",
                                              operation->id, result_type->id, panic_info_argument,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_PANIC_INFO_CONSTRUCTOR &&
                    call->target_kind == XR_TARGET_CALL_TARGET_PANIC_INFO_CONSTRUCTOR &&
                    call->result_ownership == XR_TARGET_CALL_RETURN_OWNED && result &&
                    result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                    plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED;
            if (!valid)
                break;
        } else if (json_namespace_value) {
            valid = result_type && !suspends &&
                    reconstruct_call_identity("xray-target-json-namespace-value-v1", operation->id,
                                              result_type->id, json_value_argument,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_JSON_NAMESPACE_VALUE &&
                    call->target_kind == XR_TARGET_CALL_TARGET_JSON_NAMESPACE_VALUE &&
                    call->result_ownership == XR_TARGET_CALL_RETURN_OWNED && result &&
                    result->slot < plan->slots_count &&
                    plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                    plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED;
            if (!valid)
                break;
        } else if (native_module_scalar) {
            /* A plain scalar result leaves nothing to release, so the row
             * claims no returned ownership and no callee function index: the
             * frozen definition registry names the implementation. */
            valid = result_type && !suspends &&
                    reconstruct_call_identity("xray-target-native-module-scalar-v1", operation->id,
                                              result_type->id, native_module_arity,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_NATIVE_MODULE_SCALAR &&
                    call->target_kind == XR_TARGET_CALL_TARGET_NATIVE_MODULE_SCALAR &&
                    call->result_ownership == XR_TARGET_CALL_NONE;
            if (!valid)
                break;
        } else if (native_target_leaf) {
            valid = result_type && native_target_leaf_entry && !suspends &&
                    reconstruct_call_identity("xray-target-native-target-leaf-scalar-v1",
                                              operation->id, native_target_leaf_identity,
                                              native_target_leaf_entry->target_leaf,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) &&
                    xr_stable_id_equal(call->native_callee_identity,
                                       native_target_leaf_identity) &&
                    call->native_leaf == native_target_leaf_entry->target_leaf &&
                    call->argument_count == 0 && call->flags == 0 &&
                    call->calling_convention ==
                        XR_TARGET_CALL_CONVENTION_NATIVE_TARGET_LEAF_SCALAR &&
                    call->target_kind == XR_TARGET_CALL_TARGET_NATIVE_TARGET_LEAF_SCALAR &&
                    call->result_ownership == XR_TARGET_CALL_NONE;
            if (!valid)
                break;
        } else if (array_member_scalar) {
            const XrSemanticTypeRecord *receiver_type =
                xr_semantic_plan_type(semantic, array_member_receiver_type);
            /* The call row claims no returned ownership. A unit or scalar result
             * owns nothing, and a receiver result is the receiver's own
             * reference, whose storage row the member-result family already
             * states; the only transfer the family admits is a consumed scalar
             * argument, which the receiver copies. A unit result binds no slot,
             * while every other result shape binds the one its own storage row
             * names. */
            valid = receiver_type && !suspends &&
                    reconstruct_call_identity("xray-target-array-member-scalar-v1", operation->id,
                                              receiver_type->id, array_member_element,
                                              &expected_identity) &&
                    xr_stable_id_equal(call->identity, expected_identity) &&
                    call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                    call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                    call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                    call->source_export == XR_SEMANTIC_INDEX_NONE &&
                    stable_id_is_zero(call->source_export_identity) &&
                    stable_id_is_zero(call->source_callee_identity) &&
                    call->argument_count ==
                        (array_member_tagged_store ? operation->operand_count : 0) &&
                    call->flags == 0 &&
                    call->calling_convention == XR_TARGET_CALL_CONVENTION_ARRAY_MEMBER_SCALAR &&
                    call->target_kind == XR_TARGET_CALL_TARGET_ARRAY_MEMBER_SCALAR &&
                    /* A member handing its receiver back claims nothing, while one that
                     * builds a string returns a fresh value the caller releases. Both
                     * bind the same dynamic owned slot, so the ownership word is what
                     * tells them apart. */
                    (array_member_receiver_result
                         ? (call->result_ownership == XR_TARGET_CALL_NONE ||
                            call->result_ownership == XR_TARGET_CALL_RETURN_OWNED)
                         : call->result_ownership == XR_TARGET_CALL_NONE) &&
                    result &&
                    (array_member_receiver_result
                         ? result->slot < plan->slots_count &&
                               plan->slots[result->slot].root_kind == XR_TARGET_ROOT_DYNAMIC &&
                               plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_OWNED
                     : result_kind == XR_MACHINE_REP_VOID
                         ? result->slot == XR_SEMANTIC_INDEX_NONE
                         : result->slot < plan->slots_count &&
                               plan->slots[result->slot].semantic_value == operation->result_value);
            for (uint16_t ordinal = 0;
                 valid && array_member_tagged_store && ordinal < operation->operand_count;
                 ordinal++) {
                uint32_t semantic_operand = operation->operand_begin + ordinal;
                const XrSemanticOperandRecord *operand = &operands[semantic_operand];
                const XrSemanticTypeRecord *operand_type =
                    xr_semantic_plan_type(semantic, operand->type);
                const XrTargetValueRepRecord *caller_value =
                    xr_target_plan_value_rep(plan, operand->value);
                const XrTargetCallArgumentRecord *argument =
                    next_argument < plan->call_arguments_count
                        ? &plan->call_arguments[next_argument]
                        : NULL;
                const XrTargetMachineRepRecord *caller_register =
                    caller_value ? xr_target_plan_machine_rep(plan, caller_value->register_rep)
                                 : NULL;
                const XrTargetMachineRepRecord *caller_memory =
                    caller_value ? xr_target_plan_machine_rep(plan, caller_value->memory_rep)
                                 : NULL;
                const XrTargetSlotRecord *caller_slot =
                    caller_value && caller_value->slot < plan->slots_count
                        ? &plan->slots[caller_value->slot]
                        : NULL;
                XrStableId argument_identity;
                valid =
                    argument && operand_type && caller_value && caller_register && caller_memory &&
                    caller_slot &&
                    reconstruct_call_identity("xray-target-array-member-tagged-store-argument-v1",
                                              operation->id, operand_type->id, ordinal,
                                              &argument_identity) &&
                    xr_stable_id_equal(argument->identity, argument_identity) &&
                    argument->call == i && argument->semantic_operand == semantic_operand &&
                    argument->semantic_value == operand->value &&
                    argument->callee_parameter == XR_SEMANTIC_INDEX_NONE &&
                    argument->caller_slot == caller_value->slot &&
                    argument->callee_slot == XR_SEMANTIC_INDEX_NONE &&
                    argument->register_rep == caller_value->register_rep &&
                    argument->memory_rep == caller_value->memory_rep &&
                    argument->callee_register_rep == caller_value->register_rep &&
                    argument->callee_memory_rep == caller_value->memory_rep &&
                    argument->ordinal == ordinal && argument->mode == XR_TARGET_CALL_VALUE &&
                    argument->ownership ==
                        (ordinal == 0 ? XR_TARGET_CALL_BORROW : XR_TARGET_CALL_CONSUME) &&
                    argument->transfer_mode == operand->transfer_mode && argument->flags == 0 &&
                    argument->array_element_storage ==
                        (semantic_operand == array_member_store_operand
                             ? XR_TARGET_ARRAY_STORAGE_TAGGED
                             : XR_TARGET_ARRAY_STORAGE_NONE) &&
                    argument->reserved8[0] == 0 && argument->reserved8[1] == 0 &&
                    argument->reserved8[2] == 0 &&
                    caller_register->kind ==
                        (ordinal < 2 ? XR_MACHINE_REP_DYN_VALUE : XR_MACHINE_REP_I64) &&
                    caller_memory->kind ==
                        (ordinal < 2 ? XR_MACHINE_REP_DYN_VALUE : XR_MACHINE_REP_I64) &&
                    caller_slot->register_rep == caller_value->register_rep &&
                    caller_slot->memory_rep == caller_value->memory_rep &&
                    caller_slot->ownership == caller_register->ownership &&
                    caller_register->ownership == caller_memory->ownership &&
                    (ordinal == 0   ? (caller_register->ownership == XR_TARGET_OWNERSHIP_OWNED ||
                                       caller_register->ownership == XR_TARGET_OWNERSHIP_BORROWED)
                     : ordinal == 1 ? caller_register->ownership == XR_TARGET_OWNERSHIP_OWNED
                                    : caller_register->ownership == XR_TARGET_OWNERSHIP_TRIVIAL);
                next_argument++;
            }
            if (!valid)
                break;
        } else {
            const XrSemanticTypeRecord *view_type =
                xr_semantic_plan_type(semantic, operation->result_type);
            valid =
                string_byte_slice_view && view_type && !suspends &&
                reconstruct_call_identity("xray-target-string-byte-slice-view-v1", operation->id,
                                          view_type->id, 0, &expected_identity) &&
                xr_stable_id_equal(call->identity, expected_identity) &&
                call->semantic_call_target == XR_SEMANTIC_INDEX_NONE &&
                call->callee_function == XR_SEMANTIC_INDEX_NONE &&
                call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
                call->source_export == XR_SEMANTIC_INDEX_NONE &&
                stable_id_is_zero(call->source_export_identity) &&
                stable_id_is_zero(call->source_callee_identity) && call->argument_count == 0 &&
                call->flags == 0 &&
                call->calling_convention == XR_TARGET_CALL_CONVENTION_STRING_BYTE_SLICE_VIEW &&
                call->target_kind == XR_TARGET_CALL_TARGET_STRING_BYTE_SLICE_VIEW && result &&
                result->slot < plan->slots_count &&
                plan->slots[result->slot].root_kind == XR_TARGET_ROOT_VIEW_OWNER &&
                plan->slots[result->slot].ownership == XR_TARGET_OWNERSHIP_BORROWED;
            if (!valid)
                break;
        }
        covered[call->semantic_operation] = 1;
        previous_operation = call->semantic_operation;
        XrFingerprint fingerprint;
        xr_target_call_compute_fingerprint(plan, i, &fingerprint);
        valid = valid && xr_fingerprint_equal(fingerprint, call->fingerprint);
    }
    for (uint32_t i = 0; valid && i < semantic_operations; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (operation_is_call_shaped(semantic, operation) && !covered[i] &&
            !overflow_predicate_covers_call_operation(plan, i)) {
            valid = false;
        }
    }
    valid = valid && next_argument == plan->call_arguments_count &&
            next_adapter == plan->adapters_count;
    xr_free(covered);
    xr_free(state_counts);
    xr_free(suspendable);
    xr_free(reverse_head);
    xr_free(reverse_next);
    xr_free(queue);
    return valid || report(error, error_size, "XR_TARGET_1003",
                           "call/adapter tables do not exactly cover target authority");
}

static bool verify_leaf_aggregate_machine_rep(const XrTargetPlan *plan, uint16_t rep_index,
                                              uint32_t layout_index) {
    if (rep_index >= plan->machine_reps_count)
        return false;
    const XrTargetMachineRepRecord *rep = &plan->machine_reps[rep_index];
    return rep->kind == XR_MACHINE_REP_AGGREGATE && rep->register_bits == 128 &&
           rep->memory_size == 16 && rep->memory_align == 8 &&
           rep->signedness == XR_TARGET_SIGN_NONE && rep->root_kind == XR_TARGET_ROOT_NONE &&
           rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
           rep->null_encoding == XR_TARGET_NULL_NOT_NULLABLE && rep->detail == layout_index &&
           rep->lane_count == 0 && rep->reserved == 0;
}

static bool verify_leaf_scalar_machine_rep(const XrTargetPlan *plan, uint16_t rep_index) {
    if (rep_index >= plan->machine_reps_count)
        return false;
    const XrTargetMachineRepRecord *rep = &plan->machine_reps[rep_index];
    return rep->kind == XR_MACHINE_REP_I64 && rep->register_bits == 64 && rep->memory_size == 8 &&
           rep->memory_align == 8 && rep->signedness == XR_TARGET_SIGN_SIGNED &&
           rep->root_kind == XR_TARGET_ROOT_NONE && rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
           rep->null_encoding == XR_TARGET_NULL_NOT_NULLABLE && rep->detail == 0 &&
           rep->lane_count == 0 && rep->reserved == 0;
}

static bool verify_leaf_program_target_layout(const XrTargetPlan *plan,
                                              const XrVerifyLeafProgramShape *shape,
                                              uint32_t *out_layout, uint16_t *out_rep) {
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(plan->profile);
    int aggregate_index =
        target_plan_layout_for_type(plan, shape->aggregate_binding->semantic_type);
    int scalar_index = target_plan_layout_for_type(plan, shape->scalar_binding->semantic_type);
    if (!machine || machine->architecture != XR_TARGET_ARCH_X86_64 ||
        machine->data_layout.i64.size != 8 || machine->data_layout.i64.align != 8 ||
        aggregate_index < 0 || scalar_index < 0)
        return false;
    const XrTargetLayoutRecord *aggregate = &plan->layouts[aggregate_index];
    const XrTargetLayoutRecord *scalar = &plan->layouts[scalar_index];
    if (aggregate->kind != XR_TARGET_LAYOUT_AGGREGATE || aggregate->fixed_prefix_size != 16 ||
        aggregate->align != 8 || aggregate->field_count != 2 || aggregate->root_field_count != 0 ||
        aggregate->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        scalar->kind != XR_TARGET_LAYOUT_SCALAR || scalar->fixed_prefix_size != 8 ||
        scalar->align != 8 || scalar->field_count != 0 || scalar->root_field_count != 0 ||
        scalar->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        !range_valid(aggregate->field_begin, 2, plan->fields_count))
        return false;
    uint16_t scalar_rep = XR_MACHINE_REP_COUNT;
    for (uint32_t ordinal = 0; ordinal < 2; ordinal++) {
        const XrTargetFieldRecord *field = &plan->fields[aggregate->field_begin + ordinal];
        const XrSemanticProgramTypeFieldBinding *binding =
            xr_semantic_plan_program_type_field_binding(
                plan->semantic_plan, shape->aggregate_binding->field_begin + ordinal);
        if (!binding || field->layout != (uint32_t) aggregate_index ||
            field->semantic_field != binding->declaration_ordinal ||
            field->semantic_name != XR_SEMANTIC_INDEX_NONE || field->offset != ordinal * 8u ||
            field->size != 8 || field->align != 8 || field->root_kind != XR_TARGET_ROOT_NONE ||
            field->flags != 0 || field->reserved != 0 ||
            !verify_leaf_scalar_machine_rep(plan, field->memory_rep) ||
            (ordinal != 0 && field->memory_rep != scalar_rep))
            return false;
        scalar_rep = field->memory_rep;
    }
    uint16_t aggregate_rep = XR_MACHINE_REP_COUNT;
    uint32_t matches = 0;
    for (uint32_t i = 0; i < plan->machine_reps_count; i++) {
        if (!verify_leaf_aggregate_machine_rep(plan, (uint16_t) i, (uint32_t) aggregate_index))
            continue;
        aggregate_rep = (uint16_t) i;
        matches++;
    }
    if (matches != 1)
        return false;
    *out_layout = (uint32_t) aggregate_index;
    *out_rep = aggregate_rep;
    return true;
}

static bool verify_leaf_program_slot(const XrTargetPlan *plan, const XrTargetValueRepRecord *value,
                                     uint32_t function, uint16_t aggregate_rep, uint8_t role,
                                     uint32_t semantic_operation) {
    if (!value || value->slot >= plan->slots_count || value->register_rep != aggregate_rep ||
        value->memory_rep != aggregate_rep || !slot_binds_value_in_function(plan, value, function))
        return false;
    const XrTargetSlotRecord *slot = &plan->slots[value->slot];
    return slot->semantic_value == value->semantic_value &&
           slot->semantic_operation == semantic_operation &&
           slot->logical_slot == XR_SEMANTIC_INDEX_NONE && slot->size == 16 && slot->align == 8 &&
           slot->register_rep == aggregate_rep && slot->memory_rep == aggregate_rep &&
           slot->role == role && slot->root_kind == XR_TARGET_ROOT_NONE &&
           slot->ownership == XR_TARGET_OWNERSHIP_TRIVIAL && slot->reserved == 0 &&
           slot->debug_variable == XR_SEMANTIC_INDEX_NONE;
}

static bool verify_leaf_program_argument_value(const XrTargetPlan *plan,
                                               const XrVerifyLeafProgramShape *shape,
                                               uint16_t aggregate_rep,
                                               const XrTargetValueRepRecord **out) {
    const XrTargetValueRepRecord *value = xr_target_plan_value_rep(plan, shape->argument->value);
    const XrSemanticOperationRecord *producer = NULL;
    uint32_t producer_index = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(plan->semantic_plan); i++) {
        const XrSemanticOperationRecord *candidate =
            xr_semantic_plan_operation(plan->semantic_plan, i);
        if (!candidate || candidate->function != shape->caller_binding->semantic_function ||
            candidate->result_value != shape->argument->value)
            continue;
        if (producer)
            return false;
        producer = candidate;
        producer_index = i;
    }
    if (!producer || producer->result_type != shape->aggregate_binding->semantic_type ||
        !verify_leaf_program_slot(plan, value, shape->caller_binding->semantic_function,
                                  aggregate_rep, XR_TARGET_SLOT_TEMPORARY, producer_index))
        return false;
    *out = value;
    return true;
}

static bool verify_leaf_program_target_values(const XrTargetPlan *plan,
                                              const XrVerifyLeafProgramShape *shape,
                                              uint16_t aggregate_rep,
                                              const XrTargetValueRepRecord **out_argument,
                                              const XrTargetValueRepRecord **out_parameter,
                                              const XrTargetValueRepRecord **out_result) {
    const XrTargetValueRepRecord *parameter =
        xr_target_plan_value_rep(plan, shape->parameter->value);
    const XrTargetValueRepRecord *result =
        xr_target_plan_value_rep(plan, shape->operation->result_value);
    if (!verify_leaf_program_argument_value(plan, shape, aggregate_rep, out_argument) ||
        !verify_leaf_program_slot(plan, parameter, shape->callee_binding->semantic_function,
                                  aggregate_rep, XR_TARGET_SLOT_PARAMETER,
                                  XR_SEMANTIC_INDEX_NONE) ||
        !verify_leaf_program_slot(plan, result, shape->caller_binding->semantic_function,
                                  aggregate_rep, XR_TARGET_SLOT_TEMPORARY,
                                  shape->call_binding->operation))
        return false;
    *out_parameter = parameter;
    *out_result = result;
    return true;
}

static const XrTargetCallRecord *
verify_leaf_program_target_call_row(const XrTargetPlan *plan,
                                    const XrVerifyLeafProgramShape *shape) {
    const XrTargetCallRecord *match = NULL;
    for (uint32_t i = 0; i < plan->calls_count; i++) {
        const XrTargetCallRecord *candidate = &plan->calls[i];
        if (candidate->semantic_operation != shape->call_binding->operation)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

static bool verify_leaf_program_target_call(const XrTargetPlan *plan,
                                            const XrVerifyLeafProgramShape *shape,
                                            uint16_t aggregate_rep,
                                            const XrTargetValueRepRecord *argument_value,
                                            const XrTargetValueRepRecord *parameter_value,
                                            const XrTargetValueRepRecord *result_value) {
    const XrTargetCallRecord *call = verify_leaf_program_target_call_row(plan, shape);
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(plan->profile);
    if (!call || !machine || call->semantic_call_target != shape->target_index ||
        call->caller_function != shape->caller_binding->semantic_function ||
        call->callee_function != shape->callee_binding->semantic_function ||
        call->source_dependency != XR_SEMANTIC_INDEX_NONE ||
        call->source_export != XR_SEMANTIC_INDEX_NONE ||
        !stable_id_is_zero(call->source_export_identity) ||
        !stable_id_is_zero(call->source_callee_identity) ||
        call->result_value != shape->operation->result_value ||
        call->result_slot != result_value->slot ||
        call->caller_storage_slot != result_value->slot || call->argument_count != 1 ||
        call->adapter_count != 0 || call->result_register_rep != aggregate_rep ||
        call->result_memory_rep != aggregate_rep || call->native_abi != machine->native_abi ||
        call->flags != 0 || call->calling_convention != XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL ||
        call->target_kind != XR_TARGET_CALL_TARGET_DIRECT_LOCAL ||
        call->result_mode != XR_TARGET_CALL_CALLER_STORAGE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->array_intrinsic_kind != XR_TARGET_ARRAY_INTRINSIC_NONE ||
        call->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        call->array_hof_kind != XR_TARGET_ARRAY_HOF_NONE ||
        call->array_result_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        call->reserved8[0] != 0 || call->reserved8[1] != 0 || call->reserved8[2] != 0 ||
        !range_valid(call->argument_begin, 1, plan->call_arguments_count))
        return false;
    const XrTargetCallArgumentRecord *argument = &plan->call_arguments[call->argument_begin];
    return argument->call == call->id && argument->semantic_operand == shape->argument_index &&
           argument->semantic_value == shape->argument->value &&
           argument->callee_parameter == shape->parameter_index &&
           argument->caller_slot == argument_value->slot &&
           argument->callee_slot == parameter_value->slot &&
           argument->register_rep == aggregate_rep && argument->memory_rep == aggregate_rep &&
           argument->callee_register_rep == aggregate_rep &&
           argument->callee_memory_rep == aggregate_rep && argument->ordinal == 0 &&
           argument->mode == XR_TARGET_CALL_VALUE && argument->ownership == XR_TARGET_CALL_READ &&
           argument->transfer_mode == XR_TRANSFER_SHARE && argument->flags == 0 &&
           argument->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
           argument->reserved8[0] == 0 && argument->reserved8[1] == 0 &&
           argument->reserved8[2] == 0;
}

static bool verify_leaf_program_target_lifecycle(const XrTargetPlan *plan,
                                                 const XrTargetValueRepRecord *argument,
                                                 const XrTargetValueRepRecord *parameter,
                                                 const XrTargetValueRepRecord *result) {
    if (plan->adapters_count != 0)
        return false;
    for (uint32_t i = 0; i < plan->root_slots_count; i++) {
        uint32_t slot = plan->root_slots[i];
        if (slot == argument->slot || slot == parameter->slot || slot == result->slot)
            return false;
    }
    for (uint32_t i = 0; i < plan->cleanups_count; i++) {
        uint32_t slot = plan->cleanups[i].slot;
        if (slot == argument->slot || slot == parameter->slot || slot == result->slot)
            return false;
    }
    return true;
}

static bool verify_leaf_program_target(const XrTargetPlan *plan, char *error, size_t error_size) {
    if (!semantic_is_leaf_program_family(plan->semantic_plan))
        return true;
    XrVerifyLeafProgramShape shape = {0};
    uint32_t aggregate_layout = XR_SEMANTIC_INDEX_NONE;
    uint16_t aggregate_rep = XR_MACHINE_REP_COUNT;
    const XrTargetValueRepRecord *argument = NULL;
    const XrTargetValueRepRecord *parameter = NULL;
    const XrTargetValueRepRecord *result = NULL;
    bool valid =
        verify_leaf_program_shape(plan->semantic_plan, &shape) &&
        verify_leaf_program_target_layout(plan, &shape, &aggregate_layout, &aggregate_rep) &&
        verify_leaf_program_target_values(plan, &shape, aggregate_rep, &argument, &parameter,
                                          &result) &&
        verify_leaf_program_target_call(plan, &shape, aggregate_rep, argument, parameter, result) &&
        verify_leaf_program_target_lifecycle(plan, argument, parameter, result);
    (void) aggregate_layout;
    return valid || report(error, error_size, "XR_TARGET_1003",
                           "leaf aggregate program target projection is invalid");
}

static bool verify_product_u8_machine_rep(const XrTargetPlan *plan, uint16_t rep_index) {
    if (rep_index >= plan->machine_reps_count)
        return false;
    const XrTargetMachineRepRecord *rep = &plan->machine_reps[rep_index];
    return rep->kind == XR_MACHINE_REP_U8 && rep->register_bits == 8 &&
           rep->memory_size == 1 && rep->memory_align == 1 &&
           rep->signedness == XR_TARGET_SIGN_UNSIGNED &&
           rep->root_kind == XR_TARGET_ROOT_NONE &&
           rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
           rep->null_encoding == XR_TARGET_NULL_NOT_NULLABLE && rep->detail == 0 &&
           rep->lane_count == 0 && rep->reserved == 0;
}

static bool verify_product_aggregate_machine_rep(const XrTargetPlan *plan,
                                                 uint16_t rep_index,
                                                 uint32_t layout_index) {
    if (rep_index >= plan->machine_reps_count)
        return false;
    const XrTargetMachineRepRecord *rep = &plan->machine_reps[rep_index];
    return rep->kind == XR_MACHINE_REP_AGGREGATE && rep->register_bits == 384 &&
           rep->memory_size == 48 && rep->memory_align == 8 &&
           rep->signedness == XR_TARGET_SIGN_NONE &&
           rep->root_kind == XR_TARGET_ROOT_NONE &&
           rep->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
           rep->null_encoding == XR_TARGET_NULL_NOT_NULLABLE &&
           rep->detail == layout_index && rep->lane_count == 0 && rep->reserved == 0;
}

static bool verify_product_target_layout(const XrTargetPlan *plan,
                                         const XrVerifyProductProgramShape *shape,
                                         uint16_t *out_rep) {
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(plan->profile);
    int product_index = target_plan_layout_for_type(plan, shape->product->semantic_type);
    int i64_index = target_plan_layout_for_type(plan, shape->i64->semantic_type);
    int u8_index = target_plan_layout_for_type(plan, shape->u8->semantic_type);
    if (!machine || machine->architecture != XR_TARGET_ARCH_X86_64 ||
        machine->data_layout.i64.size != 8 || machine->data_layout.i64.align != 8 ||
        product_index < 0 || i64_index < 0 || u8_index < 0)
        return false;
    const XrTargetLayoutRecord *product = &plan->layouts[product_index];
    const XrTargetLayoutRecord *i64 = &plan->layouts[i64_index];
    const XrTargetLayoutRecord *u8 = &plan->layouts[u8_index];
    if (product->kind != XR_TARGET_LAYOUT_AGGREGATE ||
        product->fixed_prefix_size != 48 || product->align != 8 ||
        product->field_count != 6 || product->root_field_count != 0 ||
        product->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        i64->kind != XR_TARGET_LAYOUT_SCALAR || i64->fixed_prefix_size != 8 ||
        i64->align != 8 || i64->field_count != 0 ||
        u8->kind != XR_TARGET_LAYOUT_SCALAR || u8->fixed_prefix_size != 1 ||
        u8->align != 1 || u8->field_count != 0 ||
        !range_valid(product->field_begin, 6, plan->fields_count))
        return false;
    for (uint32_t ordinal = 0; ordinal < 6; ordinal++) {
        const XrTargetFieldRecord *field =
            &plan->fields[product->field_begin + ordinal];
        uint32_t size = ordinal == 2 ? 1u : 8u;
        uint32_t align = ordinal == 2 ? 1u : 8u;
        if (field->layout != (uint32_t) product_index ||
            field->semantic_field != ordinal ||
            field->semantic_name != XR_SEMANTIC_INDEX_NONE ||
            field->offset != ordinal * 8u || field->size != size ||
            field->align != align || field->root_kind != XR_TARGET_ROOT_NONE ||
            field->flags != 0 || field->reserved != 0 ||
            (ordinal == 2
                 ? !verify_product_u8_machine_rep(plan, field->memory_rep)
                 : !verify_leaf_scalar_machine_rep(plan, field->memory_rep)))
            return false;
    }
    uint16_t aggregate_rep = XR_MACHINE_REP_COUNT;
    uint32_t matches = 0;
    for (uint32_t i = 0; i < plan->machine_reps_count; i++)
        if (verify_product_aggregate_machine_rep(plan, (uint16_t) i,
                                                 (uint32_t) product_index)) {
            aggregate_rep = (uint16_t) i;
            matches++;
        }
    if (matches != 1)
        return false;
    *out_rep = aggregate_rep;
    return true;
}

static bool verify_product_target_call(const XrTargetPlan *plan,
                                       const XrVerifyProductProgramShape *shape,
                                       uint32_t caller, uint16_t aggregate_rep,
                                       uint8_t *covered) {
    const XrSemanticOperationRecord *operation = shape->calls[caller];
    const XrTargetValueRepRecord *value =
        xr_target_plan_value_rep(plan, operation->result_value);
    const XrTargetCallRecord *call = NULL;
    for (uint32_t i = 0; i < plan->calls_count; i++) {
        const XrTargetCallRecord *candidate = &plan->calls[i];
        if (candidate->semantic_operation != shape->call_bindings[caller]->operation)
            continue;
        if (call)
            return false;
        call = candidate;
        covered[i] = 1;
    }
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(plan->profile);
    if (!call || !value || !machine || value->slot >= plan->slots_count ||
        value->register_rep != aggregate_rep || value->memory_rep != aggregate_rep ||
        !slot_binds_value_in_function(plan, value,
                                      shape->caller_bindings[caller]->semantic_function))
        return false;
    const XrTargetSlotRecord *slot = &plan->slots[value->slot];
    return slot->semantic_value == operation->result_value &&
           slot->semantic_operation == shape->call_bindings[caller]->operation &&
           slot->size == 48 && slot->align == 8 &&
           slot->register_rep == aggregate_rep && slot->memory_rep == aggregate_rep &&
           slot->role == XR_TARGET_SLOT_TEMPORARY &&
           slot->root_kind == XR_TARGET_ROOT_NONE &&
           slot->ownership == XR_TARGET_OWNERSHIP_TRIVIAL &&
           call->semantic_call_target == shape->target_indices[caller] &&
           call->caller_function == shape->caller_bindings[caller]->semantic_function &&
           call->callee_function == shape->callee_binding->semantic_function &&
           call->source_dependency == XR_SEMANTIC_INDEX_NONE &&
           call->source_export == XR_SEMANTIC_INDEX_NONE &&
           stable_id_is_zero(call->source_export_identity) &&
           stable_id_is_zero(call->source_callee_identity) &&
           call->result_value == operation->result_value &&
           call->result_slot == value->slot && call->caller_storage_slot == value->slot &&
           call->argument_count == 0 && call->adapter_count == 0 &&
           call->result_register_rep == aggregate_rep &&
           call->result_memory_rep == aggregate_rep && call->native_abi == machine->native_abi &&
           call->flags == 0 &&
           call->calling_convention == XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL &&
           call->target_kind == XR_TARGET_CALL_TARGET_DIRECT_LOCAL &&
           call->result_mode == XR_TARGET_CALL_CALLER_STORAGE &&
           call->result_ownership == XR_TARGET_CALL_NONE &&
           call->array_intrinsic_kind == XR_TARGET_ARRAY_INTRINSIC_NONE &&
           call->array_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
           call->array_hof_kind == XR_TARGET_ARRAY_HOF_NONE &&
           call->array_result_element_storage == XR_TARGET_ARRAY_STORAGE_NONE &&
           call->reserved8[0] == 0 && call->reserved8[1] == 0 &&
           call->reserved8[2] == 0;
}

static bool verify_product_program_target(const XrTargetPlan *plan, char *error,
                                          size_t error_size) {
    if (!semantic_is_product_program_family(plan->semantic_plan))
        return true;
    XrVerifyProductProgramShape shape = {0};
    uint16_t aggregate_rep = XR_MACHINE_REP_COUNT;
    uint8_t covered[2] = {0, 0};
    bool valid = plan->calls_count == 2 && plan->call_arguments_count == 0 &&
                 plan->adapters_count == 0 &&
                 verify_product_program_shape(plan->semantic_plan, &shape) &&
                 verify_product_target_layout(plan, &shape, &aggregate_rep) &&
                 verify_product_target_call(plan, &shape, 0, aggregate_rep, covered) &&
                 verify_product_target_call(plan, &shape, 1, aggregate_rep, covered) &&
                 covered[0] && covered[1];
    for (uint32_t i = 0; valid && i < plan->slots_count; i++) {
        const XrTargetSlotRecord *slot = &plan->slots[i];
        if (slot->register_rep != aggregate_rep && slot->memory_rep != aggregate_rep)
            continue;
        valid = slot->register_rep == aggregate_rep && slot->memory_rep == aggregate_rep &&
                slot->size == 48 && slot->align == 8 &&
                slot->root_kind == XR_TARGET_ROOT_NONE &&
                slot->ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
        for (uint32_t root = 0; valid && root < plan->root_slots_count; root++)
            valid = plan->root_slots[root] != i;
        for (uint32_t cleanup = 0; valid && cleanup < plan->cleanups_count; cleanup++)
            valid = plan->cleanups[cleanup].slot != i;
    }
    return valid || report(error, error_size, "XR_TARGET_1003",
                           "leaf product program target projection is invalid");
}

static int verify_compare_u32(const void *left, const void *right) {
    uint32_t a = *(const uint32_t *) left;
    uint32_t b = *(const uint32_t *) right;
    return a < b ? -1 : a != b;
}

typedef struct XrVerifyLifecycleProjection {
    uint32_t function;
    uint32_t state_operation;
    uint32_t producer_operation;
    uint32_t producer_value;
    uint16_t kind;
} XrVerifyLifecycleProjection;

static int verify_compare_lifecycle_projection(const void *left, const void *right) {
    const XrVerifyLifecycleProjection *a = (const XrVerifyLifecycleProjection *) left;
    const XrVerifyLifecycleProjection *b = (const XrVerifyLifecycleProjection *) right;
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

static bool verify_exact_owned_string_slot(const XrTargetPlan *plan, uint32_t function,
                                           uint32_t semantic_value, uint32_t semantic_operation,
                                           uint32_t *slot_out) {
    const XrTargetValueRepRecord *binding = xr_target_plan_value_rep(plan, semantic_value);
    const XrTargetSlotRecord *slot =
        binding && binding->slot < plan->slots_count ? &plan->slots[binding->slot] : NULL;
    const XrTargetMachineRepRecord *register_rep =
        binding ? xr_target_plan_machine_rep(plan, binding->register_rep) : NULL;
    const XrTargetMachineRepRecord *memory_rep =
        binding ? xr_target_plan_machine_rep(plan, binding->memory_rep) : NULL;
    if (!slot || !register_rep || !memory_rep || register_rep->kind != XR_MACHINE_REP_DYN_VALUE ||
        memory_rep->kind != XR_MACHINE_REP_DYN_VALUE || slot->function != function ||
        slot->semantic_value != semantic_value || slot->semantic_operation != semantic_operation ||
        slot->register_rep != binding->register_rep || slot->memory_rep != binding->memory_rep ||
        slot->root_kind != XR_TARGET_ROOT_DYNAMIC || slot->ownership != XR_TARGET_OWNERSHIP_OWNED)
        return false;
    if (slot_out)
        *slot_out = binding->slot;
    return true;
}

static bool verify_roots_and_cleanups(const XrTargetPlan *plan, char *error, size_t error_size) {
    const XrSemanticPlan *semantic = plan->semantic_plan;
    uint32_t entity_count = (uint32_t) xr_semantic_plan_entity_count(semantic);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    uint32_t projection_count = 0;
    for (uint32_t entity = 0; entity < entity_count; entity++) {
        const XrSemanticEntityRecord *record = xr_semantic_plan_entity(semantic, entity);
        projection_count += record && (record->kind == XR_SEM_ENTITY_COROUTINE_ROOT ||
                                       record->kind == XR_SEM_ENTITY_COROUTINE_DROP);
    }
    XrSemanticStringConcatReleaseIndex release_index = {0};
    XrSemanticStringConcatReleaseIndexStatus release_status =
        xr_semantic_string_concat_release_index_build(semantic, &release_index);
    if (release_status != XR_SEMANTIC_RELEASE_INDEX_OK ||
        !xr_semantic_lifecycle_work_charge_product(&release_index.linear_work, entity_count, 2u) ||
        !xr_semantic_lifecycle_work_charge(&release_index.linear_work, operation_count) ||
        !xr_semantic_lifecycle_work_charge(&release_index.linear_work, plan->functions_count) ||
        !xr_semantic_lifecycle_work_charge(&release_index.linear_work,
                                           xr_semantic_plan_block_count(semantic)) ||
        !xr_semantic_lifecycle_work_charge_product(
            &release_index.linear_work, projection_count,
            (uint64_t) xr_semantic_lifecycle_sort_height(projection_count) * 2u)) {
        xr_semantic_string_concat_release_index_dispose(&release_index);
        return report(error, error_size,
                      release_status == XR_SEMANTIC_RELEASE_INDEX_INVALID ? "XR_TARGET_1002"
                                                                          : "XR_EXEC_5003",
                      "root lifecycle projection is unavailable");
    }
    XrVerifyLifecycleProjection *projection =
        projection_count
            ? (XrVerifyLifecycleProjection *) xr_calloc(projection_count, sizeof(*projection))
            : NULL;
    uint32_t *candidates =
        entity_count ? (uint32_t *) xr_malloc((size_t) entity_count * sizeof(*candidates)) : NULL;
    if ((entity_count && !candidates) || (projection_count && !projection)) {
        xr_free(projection);
        xr_free(candidates);
        xr_semantic_string_concat_release_index_dispose(&release_index);
        return report(error, error_size, "XR_EXEC_5003", "root verifier budget exhausted");
    }
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
        if (!state || !state_operation || !producer ||
            state->kind != XR_SEM_ENTITY_COROUTINE_STATE ||
            state->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION ||
            producer->function != state_operation->function ||
            producer->result_value == XR_SEMANTIC_INDEX_NONE) {
            xr_free(projection);
            xr_free(candidates);
            return report(error, error_size, "XR_TARGET_1002",
                          "root lifecycle projection is invalid");
        }
        projection[projection_cursor++] = (XrVerifyLifecycleProjection) {
            .function = producer->function,
            .state_operation = state->subject,
            .producer_operation = record->subject,
            .producer_value = producer->result_value,
            .kind = record->kind,
        };
    }
    if (projection_cursor != projection_count) {
        xr_semantic_string_concat_release_index_dispose(&release_index);
        xr_free(projection);
        xr_free(candidates);
        return report(error, error_size,
                      release_status == XR_SEMANTIC_RELEASE_INDEX_INVALID ? "XR_TARGET_1002"
                                                                          : "XR_EXEC_5003",
                      "root lifecycle projection is unavailable");
    }
    qsort(projection, projection_count, sizeof(*projection), verify_compare_lifecycle_projection);
    uint32_t next_root = 0;
    uint32_t next_root_slot = 0;
    uint32_t next_cleanup = 0;
    uint32_t next_projection = 0;
    uint32_t next_release = 0;
    bool valid = true;
    for (uint32_t function = 0; valid && function < plan->functions_count; function++) {
        const XrSemanticFunctionRecord *semantic_function =
            xr_semantic_plan_function(semantic, function);
        const XrTargetFunctionRecord *target_function = &plan->functions[function];
        valid = semantic_function && target_function->root_begin == next_root &&
                target_function->cleanup_begin == next_cleanup;
        for (uint32_t block_offset = 0; valid && block_offset < semantic_function->block_count;
             block_offset++) {
            const XrSemanticBlockRecord *block =
                xr_semantic_plan_block(semantic, semantic_function->block_begin + block_offset);
            valid = block && block->function == function &&
                    range_valid(block->operation_begin, block->operation_count, operation_count);
            for (uint32_t operation = block ? block->operation_begin : 0;
                 valid && operation < block->operation_begin + block->operation_count;
                 operation++) {
                uint32_t root_count = 0;
                uint32_t drop_count = 0;
                while (next_projection < projection_count &&
                       projection[next_projection].function == function &&
                       projection[next_projection].state_operation == operation) {
                    const XrVerifyLifecycleProjection *record = &projection[next_projection++];
                    uint32_t slot = XR_SEMANTIC_INDEX_NONE;
                    valid = verify_exact_owned_string_slot(plan, function, record->producer_value,
                                                           record->producer_operation, &slot);
                    if (!valid)
                        break;
                    if (record->kind == XR_SEM_ENTITY_COROUTINE_ROOT)
                        candidates[root_count++] = slot;
                    else
                        candidates[entity_count - 1u - drop_count++] = slot;
                }
                if (!valid)
                    break;
                if (root_count) {
                    qsort(candidates, root_count, sizeof(*candidates), verify_compare_u32);
                    if (next_root >= plan->root_maps_count)
                        valid = false;
                    const XrTargetRootMapRecord *root = valid ? &plan->root_maps[next_root] : NULL;
                    valid = valid && root->id == next_root && root->function == function &&
                            root->semantic_operation == operation &&
                            root->slot_begin == next_root_slot && root->slot_count == root_count &&
                            root->flags == (XR_TARGET_ROOT_SUSPEND | XR_TARGET_ROOT_CANCEL |
                                            XR_TARGET_ROOT_EXIT) &&
                            range_valid(root->slot_begin, root->slot_count, plan->root_slots_count);
                    for (uint32_t i = 0; valid && i < root_count; i++)
                        valid = plan->root_slots[next_root_slot + i] == candidates[i] &&
                                (i == 0 || candidates[i - 1u] < candidates[i]);
                    next_root++;
                    next_root_slot += root_count;
                }
                if (drop_count) {
                    uint32_t *drops = candidates + entity_count - drop_count;
                    qsort(drops, drop_count, sizeof(*drops), verify_compare_u32);
                    for (uint32_t i = 0; valid && i < drop_count; i++) {
                        if (next_cleanup >= plan->cleanups_count)
                            valid = false;
                        const XrTargetCleanupRecord *cleanup =
                            valid ? &plan->cleanups[next_cleanup] : NULL;
                        valid =
                            valid && (i == 0 || drops[i - 1u] < drops[i]) &&
                            cleanup->id == next_cleanup && cleanup->function == function &&
                            cleanup->semantic_operation == operation && cleanup->slot == drops[i] &&
                            cleanup->action == XR_TARGET_CLEANUP_RELEASE &&
                            cleanup->flags == (XR_TARGET_CLEANUP_CANCEL | XR_TARGET_CLEANUP_EXIT) &&
                            cleanup->provider == 0;
                        next_cleanup++;
                    }
                }
                if (next_release >= release_index.count ||
                    release_index.rows[next_release].operation != operation)
                    continue;
                const XrSemanticStringConcatReleaseShape release =
                    release_index.rows[next_release++];
                uint32_t slot = XR_SEMANTIC_INDEX_NONE;
                valid = verify_exact_owned_string_slot(plan, function, release.released_value,
                                                       release.producer_operation, &slot);
                if (!valid || next_cleanup >= plan->cleanups_count) {
                    valid = false;
                    break;
                }
                const XrTargetCleanupRecord *cleanup = &plan->cleanups[next_cleanup];
                valid = cleanup->id == next_cleanup && cleanup->function == function &&
                        cleanup->semantic_operation == operation && cleanup->slot == slot &&
                        cleanup->action == XR_TARGET_CLEANUP_RELEASE && cleanup->flags == 0 &&
                        cleanup->provider == 0;
                next_cleanup++;
            }
        }
        valid = valid && target_function->root_count == next_root - target_function->root_begin &&
                target_function->cleanup_count == next_cleanup - target_function->cleanup_begin;
    }
    valid = valid && next_root == plan->root_maps_count &&
            next_root_slot == plan->root_slots_count && next_cleanup == plan->cleanups_count &&
            next_projection == projection_count && next_release == release_index.count;
    xr_semantic_string_concat_release_index_dispose(&release_index);
    xr_free(projection);
    xr_free(candidates);
    return valid ||
           report(error, error_size, "XR_TARGET_1002", "root and cleanup rows are not exact");
}

static bool accumulate_semantic_capability_requirements(
    const XrSemanticPlan *semantic, const XrTargetProfileDraft *facts,
    uint64_t *expected_mask, char *error, size_t error_size) {
    if (!semantic || !facts || !expected_mask)
        return report(error, error_size, "XR_TARGET_1004",
                      "target capability semantic authority is missing");
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_ASSERTION)
            continue;
        XrAssertionPlan assertion;
        if (!xr_semantic_operation_assertion_plan(operation, &assertion))
            return report(error, error_size, "XR_TARGET_1004",
                          "assertion capability requirement is not exact");
        if (facts && facts->machine.runtime_profile == XR_TARGET_RUNTIME_PROFILE_FREESTANDING &&
            assertion.kind == XR_ASSERTION_KIND_EQUAL) {
            if (!operands || operation->operand_count < 2 || operand_count < 2 ||
                operation->operand_begin > operand_count - 2u)
                return report(error, error_size, "XR_TARGET_1004",
                              "freestanding assertion equality operands are missing");
            const XrSemanticTypeRecord *left =
                xr_semantic_plan_type(semantic, operands[operation->operand_begin].type);
            const XrSemanticTypeRecord *right = xr_semantic_plan_type(
                semantic, operands[operation->operand_begin + 1u].type);
            if (!xr_target_freestanding_assertion_equality_type_supported(left) ||
                !xr_target_freestanding_assertion_equality_type_supported(right) ||
                left->kind != right->kind || left->scalar_rep != right->scalar_rep ||
                left->enum_layout_id != right->enum_layout_id)
                return report(
                    error, error_size, "XR_TARGET_1004",
                    "freestanding assertion equality type has no exact renderer/equality adapter");
        }
        if (facts && facts->machine.runtime_profile == XR_TARGET_RUNTIME_PROFILE_FREESTANDING &&
            (assertion.required_capabilities & XR_ASSERTION_CAPABILITY_FAILURE_REPORT) != 0)
            *expected_mask |= xr_target_capability_mask(XR_TARGET_CAPABILITY_ASSERTION_REPORT);
        if ((assertion.required_capabilities & XR_ASSERTION_CAPABILITY_TYPED_ERROR_BOUNDARY) != 0)
            *expected_mask |=
                xr_target_capability_mask(XR_TARGET_CAPABILITY_TYPED_ERROR_BOUNDARY);
        if ((assertion.required_capabilities & XR_ASSERTION_CAPABILITY_PANIC_BOUNDARY) != 0)
            *expected_mask |= xr_target_capability_mask(XR_TARGET_CAPABILITY_PANIC_BOUNDARY);
    }
    return true;
}

static bool verify_program_graph_machine_rep_set(const XrTargetPlan *plan,
                                                 char *error,
                                                 size_t error_size) {
    if (!plan || plan->machine_reps_count != 4u ||
        plan->machine_reps[0].kind != XR_MACHINE_REP_VOID ||
        plan->machine_reps[1].kind != XR_MACHINE_REP_I64 ||
        plan->machine_reps[1].ownership != XR_TARGET_OWNERSHIP_TRIVIAL ||
        plan->machine_reps[2].kind != XR_MACHINE_REP_DYN_VALUE ||
        plan->machine_reps[2].ownership != XR_TARGET_OWNERSHIP_BORROWED ||
        plan->machine_reps[3].kind != XR_MACHINE_REP_DYN_VALUE ||
        plan->machine_reps[3].ownership != XR_TARGET_OWNERSHIP_OWNED)
        return report(error, error_size, "XR_TARGET_1001",
                      "program graph machine representation set is not canonical");
    return true;
}

bool xr_target_semantic_capability_requirements(
    const XrSemanticPlan *const *modules, uint32_t module_count,
    const XrTargetProfile *profile, uint64_t *expected_mask,
    char *error, size_t error_size) {
    if (expected_mask)
        *expected_mask = 0u;
    const XrTargetProfileDraft *facts = xr_target_profile_facts(profile);
    if (!modules || !module_count || module_count > XR_TARGET_MAX_PROGRAM_MODULES ||
        !facts || !expected_mask)
        return report(error, error_size, "XR_TARGET_1004",
                      "target capability module-set authority is missing");
    uint64_t mask = XR_TARGET_FOUNDATION_CAPABILITY_MASK;
    for (uint32_t module = 0; module < module_count; module++)
        if (!accumulate_semantic_capability_requirements(
                modules[module], facts, &mask, error, error_size))
            return false;
    if (facts->machine.runtime_profile == XR_TARGET_RUNTIME_PROFILE_FREESTANDING &&
        (mask & (xr_target_capability_mask(XR_TARGET_CAPABILITY_TYPED_ERROR_BOUNDARY) |
                 xr_target_capability_mask(XR_TARGET_CAPABILITY_PANIC_BOUNDARY))) != 0)
        return report(error, error_size, "XR_TARGET_1004",
                      "target runtime cannot capture the required assertion failure channel");
    *expected_mask = mask;
    return true;
}

static bool verify_adapters_and_capabilities(const XrTargetPlan *plan, char *error,
                                             size_t error_size) {
    if (plan->adapters_count)
        return report(error, error_size, "XR_TARGET_1003",
                      "typed calls require an exact empty adapter partition");
    const XrTargetProfileDraft *facts = xr_target_profile_facts(plan->profile);
    uint64_t capability_mask = 0;
    uint64_t expected_mask = 0u;
    const XrSemanticPlan *ordinary_module[1] = {plan->semantic_plan};
    const XrSemanticPlan *const *modules =
        plan->semantic_module_count
            ? (const XrSemanticPlan *const *) plan->semantic_modules
            : ordinary_module;
    uint32_t module_count = plan->semantic_module_count ? plan->semantic_module_count : 1u;
    if (!xr_target_semantic_capability_requirements(
            modules, module_count, plan->profile, &expected_mask, error, error_size))
        return false;
    for (uint32_t i = 0; i < plan->capabilities_count; i++) {
        const XrTargetCapabilityRecord *record = &plan->capabilities[i];
        uint16_t expected_provider = xr_target_capability_provider(record->capability);
        uint64_t bit = xr_target_capability_mask(record->capability);
        if (record->id != i || bit == 0 || record->provider != expected_provider ||
            record->flags != XR_TARGET_CAPABILITY_REQUIRED)
            return report(error, error_size, "XR_TARGET_1004",
                          "capability record is not canonically provider-bound");
        if ((capability_mask & bit) != 0 ||
            (i != 0 && plan->capabilities[i - 1].capability >= record->capability) || !facts ||
            (record->capability != XR_TARGET_CAPABILITY_TYPED_ERROR_BOUNDARY &&
             (facts->provider_mask & bit) == 0) ||
            (expected_provider != XR_TARGET_PROVIDER_INVALID &&
             (facts->provider_mask & XR_TARGET_PROVIDER_MASK(expected_provider)) == 0))
            return report(error, error_size, "XR_TARGET_1004",
                          "capability provider is absent or duplicated");
        capability_mask |= bit;
    }
    if (capability_mask != expected_mask)
        return report(error, error_size, "XR_TARGET_1004",
                      "TargetPlan capability closure is incomplete");
    return true;
}

static bool verify_coroutine_resume_shape(const XrSemanticPlan *semantic, uint32_t operation_index,
                                          const uint32_t *edge_by_block, const uint8_t *edge_counts,
                                          uint32_t block_count, uint32_t suspend_block,
                                          uint32_t resume_block, uint32_t resume_predecessor,
                                          uint16_t predecessor_ordinal) {
    uint32_t predecessor_count = 0;
    const uint32_t *predecessors = xr_semantic_plan_predecessors(semantic, &predecessor_count);
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(semantic, operation_index);
    const XrSemanticBlockRecord *suspend =
        operation ? xr_semantic_plan_block(semantic, operation->block) : NULL;
    if (!operation || operation->block >= block_count || !suspend ||
        suspend_block != operation->block || suspend->function != operation->function ||
        suspend->operation_begin != operation_index || suspend->operation_count != 1 ||
        suspend->predecessor_count != 1 || suspend->predecessor_begin >= predecessor_count ||
        suspend->successors[0] == XR_SEMANTIC_INDEX_NONE ||
        resume_block != suspend->successors[0] || resume_predecessor != suspend_block ||
        (suspend->successors[1] != XR_SEMANTIC_INDEX_NONE &&
         suspend->successors[1] != suspend->successors[0]))
        return false;
    const XrSemanticBlockRecord *before =
        xr_semantic_plan_block(semantic, predecessors[suspend->predecessor_begin]);
    const XrSemanticBlockRecord *resume = xr_semantic_plan_block(semantic, resume_block);
    if (!before || !resume || before->function != operation->function ||
        resume->function != operation->function ||
        (before->successors[0] != suspend_block && before->successors[1] != suspend_block) ||
        resume->predecessor_count != 1 || resume->predecessor_begin >= predecessor_count ||
        predecessor_ordinal != 0 || predecessors[resume->predecessor_begin] != suspend_block ||
        edge_counts[suspend_block] != 1)
        return false;
    const XrSemanticEdgeRecord *edge =
        xr_semantic_plan_edge(semantic, edge_by_block[suspend_block]);
    return edge && edge->function == operation->function && edge->from_block == suspend_block &&
           edge->to_block == resume_block && edge->operation == XR_SEMANTIC_INDEX_NONE &&
           edge->kind == XR_SEM_EDGE_NORMAL && edge->flags == 0;
}

static bool verify_coroutines(const XrTargetPlan *plan, char *error, size_t error_size) {
    const XrSemanticPlan *semantic = plan->semantic_plan;
    uint32_t function_count = (uint32_t) xr_semantic_plan_function_count(semantic);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
    uint32_t entity_count = (uint32_t) xr_semantic_plan_entity_count(semantic);
    uint32_t block_count = (uint32_t) xr_semantic_plan_block_count(semantic);
    uint32_t *function_states =
        function_count ? (uint32_t *) xr_calloc(function_count, sizeof(*function_states)) : NULL;
    uint32_t *state_by_operation =
        operation_count
            ? (uint32_t *) xr_malloc((size_t) operation_count * sizeof(*state_by_operation))
            : NULL;
    uint32_t *call_by_operation =
        operation_count
            ? (uint32_t *) xr_malloc((size_t) operation_count * sizeof(*call_by_operation))
            : NULL;
    uint8_t *expected_by_operation =
        operation_count ? (uint8_t *) xr_calloc(operation_count, sizeof(*expected_by_operation))
                        : NULL;
    uint8_t *call_state_counts =
        plan->calls_count ? (uint8_t *) xr_calloc(plan->calls_count, sizeof(*call_state_counts))
                          : NULL;
    uint32_t *resume_instruction_by_state =
        plan->coroutines_count ? (uint32_t *) xr_malloc((size_t) plan->coroutines_count *
                                                        sizeof(*resume_instruction_by_state))
                               : NULL;
    uint32_t *edge_by_block =
        block_count ? (uint32_t *) xr_malloc((size_t) block_count * sizeof(*edge_by_block)) : NULL;
    uint8_t *edge_counts =
        block_count ? (uint8_t *) xr_calloc(block_count, sizeof(*edge_counts)) : NULL;
    if ((function_count && !function_states) ||
        (operation_count &&
         (!state_by_operation || !call_by_operation || !expected_by_operation)) ||
        (plan->calls_count && !call_state_counts) ||
        (plan->coroutines_count && !resume_instruction_by_state) ||
        (block_count && (!edge_by_block || !edge_counts))) {
        xr_free(function_states);
        xr_free(state_by_operation);
        xr_free(call_by_operation);
        xr_free(expected_by_operation);
        xr_free(call_state_counts);
        xr_free(resume_instruction_by_state);
        xr_free(edge_by_block);
        xr_free(edge_counts);
        return report(error, error_size, "XR_EXEC_5003", "coroutine verifier allocation failed");
    }
    for (uint32_t operation = 0; operation < operation_count; operation++) {
        state_by_operation[operation] = XR_SEMANTIC_INDEX_NONE;
        call_by_operation[operation] = XR_SEMANTIC_INDEX_NONE;
    }
    for (uint32_t block = 0; block < block_count; block++)
        edge_by_block[block] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t state = 0; state < plan->coroutines_count; state++)
        resume_instruction_by_state[state] = XR_SEMANTIC_INDEX_NONE;
    bool valid = plan->functions_count == function_count;
    for (uint32_t instruction = 0; valid && instruction < plan->instructions_count; instruction++) {
        const XrTargetInstructionRecord *row = &plan->instructions[instruction];
        if (row->opcode != XR_TARGET_INSTRUCTION_SUSPEND)
            continue;
        uint32_t state = XR_TARGET_INSTRUCTION_SUSPEND_STATE(row->immediate_bits);
        if (state >= plan->coroutines_count ||
            resume_instruction_by_state[state] != XR_SEMANTIC_INDEX_NONE) {
            valid = false;
            break;
        }
        resume_instruction_by_state[state] =
            XR_TARGET_INSTRUCTION_SUSPEND_RESUME(row->immediate_bits);
    }
    uint32_t semantic_edges = (uint32_t) xr_semantic_plan_edge_count(semantic);
    for (uint32_t edge_index = 0; valid && edge_index < semantic_edges; edge_index++) {
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
    uint32_t expected_states = 0;
    for (uint32_t entity_index = 0; valid && entity_index < entity_count; entity_index++) {
        const XrSemanticEntityRecord *entity = xr_semantic_plan_entity(semantic, entity_index);
        if (!entity || entity->kind != XR_SEM_ENTITY_COROUTINE_STATE)
            continue;
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, entity->subject);
        if (entity->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION || !operation ||
            operation->function >= function_count || entity->ordinal == 0 ||
            function_states[operation->function] == UINT32_MAX) {
            valid = false;
            break;
        }
        function_states[operation->function]++;
        if (++expected_by_operation[entity->subject] != 1) {
            valid = false;
            break;
        }
        expected_states++;
    }
    for (uint32_t call = 0; valid && call < plan->calls_count; call++) {
        uint32_t operation = plan->calls[call].semantic_operation;
        if (operation >= operation_count ||
            call_by_operation[operation] != XR_SEMANTIC_INDEX_NONE) {
            valid = false;
            break;
        }
        call_by_operation[operation] = call;
    }
    valid = valid && expected_states == plan->coroutines_count;
    uint32_t next_state = 0;
    for (uint32_t function = 0; valid && function < function_count; function++) {
        const XrTargetFunctionRecord *record = &plan->functions[function];
        valid =
            record->coroutine_begin == next_state &&
            record->coroutine_count == function_states[function] &&
            range_valid(record->coroutine_begin, record->coroutine_count, plan->coroutines_count);
        next_state += record->coroutine_count;
    }
    valid = valid && next_state == plan->coroutines_count;
    for (uint32_t state_index = 0; valid && state_index < plan->coroutines_count; state_index++) {
        const XrTargetCoroutineStateRecord *state = &plan->coroutines[state_index];
        const XrSemanticEntityRecord *entity =
            xr_semantic_plan_entity(semantic, state->semantic_entity);
        const XrSemanticOperationRecord *operation =
            entity ? xr_semantic_plan_operation(semantic, entity->subject) : NULL;
        if (!entity || !operation || entity->kind != XR_SEM_ENTITY_COROUTINE_STATE ||
            entity->subject_kind != XR_SEM_ENTITY_SUBJECT_OPERATION || state->id != state_index ||
            state->function != operation->function ||
            state->semantic_operation != entity->subject ||
            state->resume_instruction != resume_instruction_by_state[state_index] ||
            state->logical_state != entity->ordinal || state->function >= function_count ||
            entity->ordinal == 0 || entity->ordinal > function_states[state->function] ||
            state_index !=
                plan->functions[state->function].coroutine_begin + entity->ordinal - 1u ||
            entity->subject >= operation_count ||
            state_by_operation[entity->subject] != XR_SEMANTIC_INDEX_NONE ||
            !verify_coroutine_resume_shape(semantic, entity->subject, edge_by_block, edge_counts,
                                           block_count, state->suspend_block, state->resume_block,
                                           state->resume_predecessor,
                                           state->resume_predecessor_ordinal)) {
            valid = false;
            break;
        }
        state_by_operation[entity->subject] = state_index;
        uint32_t expected_call = call_by_operation[entity->subject];
        bool expected_method_call =
            expected_call < plan->calls_count &&
            (plan->calls[expected_call].target_kind == XR_TARGET_CALL_TARGET_SOURCE_EXPORT ||
             plan->calls[expected_call].target_kind ==
                 XR_TARGET_CALL_TARGET_NATIVE_NAMESPACE_YIELDABLE);
        if (expected_call != XR_SEMANTIC_INDEX_NONE &&
            (expected_call >= plan->calls_count ||
             (expected_method_call ? operation->opcode != XI_CALL_METHOD
                                   : operation->opcode != XI_CALL) ||
             plan->calls[expected_call].flags != XR_TARGET_CALL_SUSPEND)) {
            valid = false;
            break;
        }
        uint32_t expected_slot = XR_SEMANTIC_INDEX_NONE;
        uint16_t expected_flags = 0;
        const XrTargetValueRepRecord *result =
            xr_target_plan_value_rep(plan, operation->result_value);
        if (result) {
            if (result->memory_rep >= plan->machine_reps_count) {
                valid = false;
                break;
            }
            if (plan->machine_reps[result->memory_rep].kind != XR_MACHINE_REP_VOID) {
                if (!slot_binds_value_in_function(plan, result, operation->function)) {
                    valid = false;
                    break;
                }
                expected_slot = result->slot;
                expected_flags |= XR_TARGET_COROUTINE_RESULT_SLOT_BOUND;
            }
        }
        if (expected_call != XR_SEMANTIC_INDEX_NONE) {
            expected_flags |= XR_TARGET_COROUTINE_DIRECT_CHILD;
            if (plan->calls[expected_call].target_kind == XR_TARGET_CALL_TARGET_SOURCE_EXPORT)
                expected_flags |= XR_TARGET_COROUTINE_SOURCE_CHILD;
            if (plan->calls[expected_call].result_slot != expected_slot ||
                plan->calls[expected_call].caller_storage_slot != XR_SEMANTIC_INDEX_NONE) {
                valid = false;
                break;
            }
            call_state_counts[expected_call]++;
        }
        valid = state->direct_call == expected_call && state->result_slot == expected_slot &&
                state->flags == expected_flags;
    }
    for (uint32_t operation = 0; valid && operation < operation_count; operation++) {
        valid = (state_by_operation[operation] != XR_SEMANTIC_INDEX_NONE) ==
                (expected_by_operation[operation] == 1);
    }
    for (uint32_t call = 0; valid && call < plan->calls_count; call++) {
        uint8_t expected = (plan->calls[call].flags & XR_TARGET_CALL_SUSPEND) != 0;
        valid = call_state_counts[call] == expected;
    }
    xr_free(function_states);
    xr_free(state_by_operation);
    xr_free(call_by_operation);
    xr_free(expected_by_operation);
    xr_free(call_state_counts);
    xr_free(resume_instruction_by_state);
    xr_free(edge_by_block);
    xr_free(edge_counts);
    return valid ||
           report(error, error_size, "XR_CORO_4000", "coroutine state-call table is not exact");
}

static bool entry_rep_is_exact_i64(const XrTargetPlan *plan, uint16_t rep) {
    const XrTargetMachineRepRecord *record =
        rep < plan->machine_reps_count ? &plan->machine_reps[rep] : NULL;
    return record && record->kind == XR_MACHINE_REP_I64 && record->register_bits == 64 &&
           record->memory_size == 8 && record->memory_align == 8 &&
           record->signedness == XR_TARGET_SIGN_SIGNED &&
           record->root_kind == XR_TARGET_ROOT_NONE &&
           record->ownership == XR_TARGET_OWNERSHIP_TRIVIAL;
}

static bool semantic_type_is_exact_signed_i64(const XrSemanticTypeRecord *type) {
    uint16_t kind = XR_MACHINE_REP_COUNT;
    return type && semantic_type_expected_rep(type, &kind) == 1 && kind == XR_MACHINE_REP_I64;
}

/* The persistent expectation is a caller-site fact, not a cached resolution.
 * This pass re-derives every byte from the verified dependency export and the
 * target profile, then proves a one-to-one relation with CALL_ENTRY rows. */
static bool verify_entry_expectations(const XrTargetPlan *plan, char *error, size_t error_size) {
    const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(plan->profile);
    uint8_t *seen = plan->entry_expectations_count
                        ? (uint8_t *) xr_calloc(plan->entry_expectations_count, 1)
                        : NULL;
    if (plan->entry_expectations_count && !seen)
        return report(error, error_size, "XR_EXEC_5003",
                      "entry expectation verifier budget exhausted");
    bool valid = machine != NULL;
    for (uint32_t i = 0; valid && i < plan->entry_expectations_count; i++) {
        const XrTargetEntryExpectationRecord *record = &plan->entry_expectations[i];
        const XrTargetCallRecord *call =
            record->call < plan->calls_count ? &plan->calls[record->call] : NULL;
        const XrSemanticPlan *dependency =
            call && call->source_dependency < plan->semantic_dependency_count
                ? plan->semantic_dependencies[call->source_dependency]
                : NULL;
        const XrSemanticSourceExportRecord *export_record =
            dependency && call->source_export < xr_semantic_plan_source_export_count(dependency)
                ? xr_semantic_plan_source_export(dependency, call->source_export)
                : NULL;
        const XrSemanticFunctionRecord *callee =
            export_record && export_record->kind == XR_SEM_SOURCE_EXPORT_FUNCTION
                ? xr_semantic_plan_function(dependency, export_record->function)
                : NULL;
        XrStableId expected_identity = {{0}};
        XrTargetEntryAbiFacts facts = {0};
        XrFingerprint abi = {{0}};
        XrFingerprint adapter = {{0}};
        valid = call && dependency && export_record &&
                export_record->kind == XR_SEM_SOURCE_EXPORT_FUNCTION && callee &&
                xr_stable_id_equal(export_record->exported_entity, callee->id) && record->id == i &&
                !stable_id_is_zero(record->identity) && call->id == record->call &&
                call->flags == 0 &&
                call->calling_convention == XR_TARGET_CALL_CONVENTION_SOURCE_EXPORT &&
                call->target_kind == XR_TARGET_CALL_TARGET_SOURCE_EXPORT &&
                call->callee_function == XR_SEMANTIC_INDEX_NONE && call->adapter_count == 0 &&
                call->result_mode == XR_TARGET_CALL_VALUE &&
                call->result_ownership == XR_TARGET_CALL_NONE &&
                entry_rep_is_exact_i64(plan, call->result_register_rep) &&
                entry_rep_is_exact_i64(plan, call->result_memory_rep) &&
                call->argument_count == callee->parameter_count && callee->capture_count == 0 &&
                semantic_type_is_exact_signed_i64(
                    xr_semantic_plan_type(dependency, callee->return_type)) &&
                record->abi_schema_version == XR_TARGET_ENTRY_ABI_SCHEMA_VERSION &&
                record->parameter_count == callee->parameter_count &&
                record->native_abi == machine->native_abi &&
                record->value_kind == XR_TARGET_ENTRY_VALUE_EXACT_I64 &&
                record->adapter_kind == XR_TARGET_ENTRY_ADAPTER_IDENTITY && record->flags == 0 &&
                record->reserved32 == 0 &&
                record->target_data_layout == machine->data_layout.stable_hash &&
                xr_fingerprint_equal(record->target_profile_fingerprint,
                                     xr_target_profile_fingerprint(plan->profile)) &&
                reconstruct_call_identity("xray-target-entry-expectation-v1", call->identity,
                                          call->source_callee_identity, i, &expected_identity) &&
                xr_stable_id_equal(record->identity, expected_identity);
        for (uint16_t ordinal = 0; valid && ordinal < call->argument_count; ordinal++) {
            const XrTargetCallArgumentRecord *argument =
                &plan->call_arguments[call->argument_begin + ordinal];
            const XrSemanticParameterRecord *parameter =
                xr_semantic_plan_parameter(dependency, callee->parameter_begin + ordinal);
            valid = parameter && parameter->function == export_record->function &&
                    parameter->ordinal == ordinal && parameter->mode == XR_PARAM_READ &&
                    parameter->transfer_mode == XR_TRANSFER_SHARE &&
                    semantic_type_is_exact_signed_i64(
                        xr_semantic_plan_type(dependency, parameter->type)) &&
                    argument->call == call->id && argument->ordinal == ordinal &&
                    argument->mode == XR_TARGET_CALL_VALUE &&
                    (argument->ownership == XR_TARGET_CALL_READ ||
                     argument->ownership == XR_TARGET_CALL_CONSUME) &&
                    argument->transfer_mode == XR_TRANSFER_SHARE && argument->flags == 0 &&
                    argument->callee_slot == XR_SEMANTIC_INDEX_NONE &&
                    entry_rep_is_exact_i64(plan, argument->register_rep) &&
                    entry_rep_is_exact_i64(plan, argument->memory_rep) &&
                    entry_rep_is_exact_i64(plan, argument->callee_register_rep) &&
                    entry_rep_is_exact_i64(plan, argument->callee_memory_rep);
        }
        if (!valid)
            break;
        facts.schema_version = record->abi_schema_version;
        facts.parameter_count = record->parameter_count;
        facts.native_abi = record->native_abi;
        facts.value_kind = record->value_kind;
        facts.target_data_layout = record->target_data_layout;
        facts.target_profile_fingerprint = record->target_profile_fingerprint;
        valid = xr_target_entry_abi_fingerprint(&facts, &abi) &&
                xr_target_entry_identity_adapter_fingerprint(&abi, &adapter) &&
                xr_fingerprint_equal(abi, record->entry_abi_fingerprint) &&
                xr_fingerprint_equal(adapter, record->adapter_fingerprint);
    }
    for (uint32_t i = 0; valid && i < plan->instructions_count; i++) {
        const XrTargetInstructionRecord *instruction = &plan->instructions[i];
        if (instruction->opcode != XR_TARGET_INSTRUCTION_CALL_ENTRY_I64)
            continue;
        uint32_t expectation = (uint32_t) instruction->immediate_bits;
        if (instruction->immediate_bits > UINT32_MAX ||
            expectation >= plan->entry_expectations_count || seen[expectation]) {
            valid = false;
            break;
        }
        const XrTargetEntryExpectationRecord *record = &plan->entry_expectations[expectation];
        valid = record->call < plan->calls_count &&
                plan->calls[record->call].caller_function == instruction->function &&
                plan->calls[record->call].result_slot == instruction->result_slot;
        seen[expectation] = 1;
    }
    for (uint32_t i = 0; valid && i < plan->entry_expectations_count; i++)
        valid = seen[i] != 0;
    xr_free(seen);
    return valid || report(error, error_size, "XR_TARGET_1005",
                           "dynamic entry expectation table is not exact");
}

static const XrSemanticEntityRecord *debug_entity_for(const XrSemanticPlan *semantic, uint16_t kind,
                                                      uint8_t subject_kind, uint32_t subject) {
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

static uint32_t debug_operation_for_instruction(const XrTargetPlan *plan,
                                                const XrTargetInstructionRecord *instruction) {
    if (!plan || !instruction)
        return XR_SEMANTIC_INDEX_NONE;
    if (instruction->result_slot < plan->slots_count) {
        const XrTargetSlotRecord *slot = &plan->slots[instruction->result_slot];
        if (slot->id == instruction->result_slot && slot->function == instruction->function &&
            slot->semantic_operation != XR_SEMANTIC_INDEX_NONE)
            return slot->semantic_operation;
    }
    if ((instruction->opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_I64 ||
         instruction->opcode == XR_TARGET_INSTRUCTION_CALL_NATIVE_LEAF_I64 ||
         instruction->opcode == XR_TARGET_INSTRUCTION_CALL_ENTRY_I64) &&
        instruction->immediate_bits <= UINT32_MAX) {
        uint32_t call =
            instruction->opcode == XR_TARGET_INSTRUCTION_CALL_ENTRY_I64
                ? (instruction->immediate_bits < plan->entry_expectations_count
                       ? plan->entry_expectations[(uint32_t) instruction->immediate_bits].call
                       : XR_SEMANTIC_INDEX_NONE)
                : (uint32_t) instruction->immediate_bits;
        if (call < plan->calls_count && plan->calls[call].id == call &&
            plan->calls[call].caller_function == instruction->function)
            return plan->calls[call].semantic_operation;
    }
    if (instruction->operand_slots[0] < plan->slots_count) {
        const XrTargetSlotRecord *slot = &plan->slots[instruction->operand_slots[0]];
        if (slot->id == instruction->operand_slots[0] && slot->function == instruction->function)
            return slot->semantic_operation;
    }
    return XR_SEMANTIC_INDEX_NONE;
}

static bool verify_debug_facts(const XrTargetPlan *plan, char *error, size_t error_size) {
    const XrSemanticPlan *semantic = plan->semantic_plan;
    const XrOwnershipCertificate *ownership = xr_semantic_plan_ownership(semantic);
    const XrFingerprint zero_fingerprint = {{0}};
    if (plan->debug_facts_count != plan->instructions_count)
        return report(error, error_size, "XR_TARGET_1005",
                      "target debug facts do not cover every instruction");
    for (uint32_t i = 0; i < plan->debug_facts_count; i++) {
        const XrTargetDebugFactRecord *fact = &plan->debug_facts[i];
        const XrTargetInstructionRecord *instruction = &plan->instructions[i];
        uint32_t operation = debug_operation_for_instruction(plan, instruction);
        if (fact->id != instruction->id || fact->instruction != instruction->id ||
            fact->function != instruction->function ||
            (fact->coroutine_state != XR_SEMANTIC_INDEX_NONE &&
             fact->coroutine_state >= plan->coroutines_count))
            return report(error, error_size, "XR_TARGET_1005",
                          "target debug fact row identity is invalid");
        if (operation == XR_SEMANTIC_INDEX_NONE) {
            if (fact->semantic_operation != XR_SEMANTIC_INDEX_NONE ||
                !stable_id_is_zero(fact->semantic_operation_identity) ||
                !stable_id_is_zero(fact->source_span_identity) ||
                !stable_id_is_zero(fact->owner_identity) ||
                !stable_id_is_zero(fact->coroutine_state_identity) ||
                !fingerprint_is_zero(fact->layout_fingerprint) || fact->source_start_line ||
                fact->source_start_column || fact->source_end_line || fact->source_end_column ||
                fact->coroutine_state != XR_SEMANTIC_INDEX_NONE)
                return report(error, error_size, "XR_TARGET_1005",
                              "target debug fact invented a synthetic relation");
            continue;
        }
        const XrSemanticOperationRecord *semantic_operation =
            xr_semantic_plan_operation(semantic, operation);
        if (!semantic_operation || operation != fact->semantic_operation ||
            semantic_operation->function != instruction->function ||
            !xr_stable_id_equal(fact->semantic_operation_identity, semantic_operation->id))
            return report(error, error_size, "XR_TARGET_1005",
                          "target debug semantic operation relation is invalid");
        if (semantic_operation->source_file) {
            const XrSemanticEntityRecord *span = debug_entity_for(
                semantic, XR_SEM_ENTITY_DEBUG_SPAN, XR_SEM_ENTITY_SUBJECT_OPERATION, operation);
            if (!span || !xr_stable_id_equal(fact->source_span_identity, span->id) ||
                fact->source_start_line != semantic_operation->source_start_line ||
                fact->source_start_column != semantic_operation->source_start_column ||
                fact->source_end_line != semantic_operation->source_end_line ||
                fact->source_end_column != semantic_operation->source_end_column)
                return report(error, error_size, "XR_TARGET_1005",
                              "target debug source span relation is invalid");
        } else if (!stable_id_is_zero(fact->source_span_identity) || fact->source_start_line ||
                   fact->source_start_column || fact->source_end_line || fact->source_end_column) {
            return report(error, error_size, "XR_TARGET_1005",
                          "target debug source span was guessed");
        }
        XrFingerprint expected_layout = zero_fingerprint;
        for (uint32_t layout = 0; layout < plan->layouts_count; layout++) {
            if (plan->layouts[layout].semantic_type != semantic_operation->result_type)
                continue;
            if (!fingerprint_is_zero(expected_layout))
                return report(error, error_size, "XR_TARGET_1005",
                              "target debug layout relation is ambiguous");
            expected_layout = plan->layouts[layout].fingerprint;
        }
        if (!xr_fingerprint_equal(fact->layout_fingerprint, expected_layout))
            return report(error, error_size, "XR_TARGET_1005",
                          "target debug layout relation is invalid");
        XrStableId expected_owner = {{0}};
        if (ownership && semantic_operation->result_value != XR_SEMANTIC_INDEX_NONE) {
            for (uint32_t owner = 0; owner < xr_ownership_certificate_owner_count(ownership);
                 owner++) {
                const XrOwnershipOwnerRecord *record =
                    xr_ownership_certificate_owner(ownership, owner);
                if (!record)
                    return report(error, error_size, "XR_TARGET_1005",
                                  "target debug ownership certificate is invalid");
                if (record->function != instruction->function ||
                    record->origin_value != semantic_operation->result_value)
                    continue;
                const XrSemanticEntityRecord *entity = debug_entity_for(
                    semantic, XR_SEM_ENTITY_OWNER, XR_SEM_ENTITY_SUBJECT_OWNER, owner);
                if (!entity || !stable_id_is_zero(expected_owner))
                    return report(error, error_size, "XR_TARGET_1005",
                                  "target debug owner relation is ambiguous");
                expected_owner = entity->id;
            }
        }
        if (!xr_stable_id_equal(fact->owner_identity, expected_owner))
            return report(error, error_size, "XR_TARGET_1005",
                          "target debug owner relation is invalid");
        uint32_t expected_state = XR_SEMANTIC_INDEX_NONE;
        XrStableId expected_state_identity = {{0}};
        for (uint32_t state = 0; state < plan->coroutines_count; state++) {
            const XrTargetCoroutineStateRecord *candidate = &plan->coroutines[state];
            if (candidate->semantic_operation != operation)
                continue;
            const XrSemanticEntityRecord *entity =
                xr_semantic_plan_entity(semantic, candidate->semantic_entity);
            if (expected_state != XR_SEMANTIC_INDEX_NONE || !entity ||
                entity->kind != XR_SEM_ENTITY_COROUTINE_STATE || entity->subject != operation)
                return report(error, error_size, "XR_TARGET_1005",
                              "target debug coroutine relation is ambiguous");
            expected_state = candidate->id;
            expected_state_identity = entity->id;
        }
        if (fact->coroutine_state != expected_state ||
            !xr_stable_id_equal(fact->coroutine_state_identity, expected_state_identity))
            return report(error, error_size, "XR_TARGET_1005",
                          "target debug coroutine relation is invalid");
    }
    return true;
}

static const XrSemanticPlan *graph_partition_semantic(const XrTargetPlan *plan,
                                                      uint32_t partition) {
    if (!plan || partition >= plan->module_partitions_count)
        return NULL;
    uint32_t semantic_module = plan->module_partitions[partition].semantic_module;
    return semantic_module < plan->semantic_module_count
               ? plan->semantic_modules[semantic_module]
               : NULL;
}

static bool graph_partition_ranges_are_exact(const XrTargetPlan *plan, char *error,
                                             size_t error_size) {
#define XR_GRAPH_VERIFY_RANGES(name)                                                               \
    do {                                                                                           \
        uint32_t next = 0;                                                                         \
        for (uint32_t i = 0; i < plan->module_partitions_count; i++) {                             \
            const XrTargetModulePartitionRecord *partition = &plan->module_partitions[i];          \
            if (partition->name##_begin != next ||                                                 \
                !checked_u32_add(next, partition->name##_count, &next))                            \
                return report(error, error_size, "XR_TARGET_1001",                               \
                              "program graph module ranges overlap or have a gap");              \
        }                                                                                          \
        if (next != plan->name##_count)                                                            \
            return report(error, error_size, "XR_TARGET_1001",                                   \
                          "program graph module ranges do not cover their target table");         \
    } while (0)
    XR_GRAPH_VERIFY_RANGES(value_reps);
    XR_GRAPH_VERIFY_RANGES(extents);
    XR_GRAPH_VERIFY_RANGES(layouts);
    XR_GRAPH_VERIFY_RANGES(fields);
    XR_GRAPH_VERIFY_RANGES(storage);
    XR_GRAPH_VERIFY_RANGES(allocations);
    XR_GRAPH_VERIFY_RANGES(extent_operands);
    XR_GRAPH_VERIFY_RANGES(functions);
    XR_GRAPH_VERIFY_RANGES(slots);
    XR_GRAPH_VERIFY_RANGES(instructions);
    XR_GRAPH_VERIFY_RANGES(calls);
    XR_GRAPH_VERIFY_RANGES(call_arguments);
    XR_GRAPH_VERIFY_RANGES(root_maps);
    XR_GRAPH_VERIFY_RANGES(root_slots);
    XR_GRAPH_VERIFY_RANGES(cleanups);
    XR_GRAPH_VERIFY_RANGES(adapters);
    XR_GRAPH_VERIFY_RANGES(coroutines);
    XR_GRAPH_VERIFY_RANGES(entry_expectations);
    XR_GRAPH_VERIFY_RANGES(debug_facts);
#undef XR_GRAPH_VERIFY_RANGES
    return true;
}

static bool graph_function_in_partition(const XrTargetModulePartitionRecord *partition,
                                        uint32_t function) {
    return function >= partition->functions_begin &&
           function - partition->functions_begin < partition->functions_count;
}

static bool graph_slot_in_function(const XrTargetPlan *plan, uint32_t function, uint32_t slot) {
    if (function >= plan->functions_count || slot >= plan->slots_count)
        return false;
    const XrTargetFunctionRecord *row = &plan->functions[function];
    return slot >= row->slot_begin && slot - row->slot_begin < row->slot_count;
}

static bool graph_semantic_value_count(const XrSemanticPlan *semantic, uint32_t *count) {
    *count = 0;
    for (uint32_t i = 0; i < xr_semantic_plan_function_count(semantic); i++) {
        const XrSemanticFunctionRecord *function = xr_semantic_plan_function(semantic, i);
        uint32_t end = 0;
        if (!function || !checked_u32_add(function->value_begin, function->value_count, &end))
            return false;
        if (end > *count)
            *count = end;
    }
    return true;
}

static bool verify_program_graph_debug_partition(
    const XrTargetPlan *plan, const XrTargetModulePartitionRecord *partition,
    const XrSemanticPlan *semantic, char *error, size_t error_size) {
    if (partition->debug_facts_begin != partition->instructions_begin ||
        partition->debug_facts_count != partition->instructions_count)
        return report(error, error_size, "XR_TARGET_1005",
                      "program graph debug partition is incomplete");
    const XrOwnershipCertificate *ownership = xr_semantic_plan_ownership(semantic);
    const XrFingerprint zero_fingerprint = {{0}};
    for (uint32_t local = 0; local < partition->debug_facts_count; local++) {
        uint32_t index = partition->debug_facts_begin + local;
        const XrTargetDebugFactRecord *fact = &plan->debug_facts[index];
        const XrTargetInstructionRecord *instruction = &plan->instructions[index];
        const XrTargetFunctionRecord *target_function =
            instruction->function < plan->functions_count
                ? &plan->functions[instruction->function]
                : NULL;
        uint32_t operation = debug_operation_for_instruction(plan, instruction);
        if (!target_function || fact->id != index || fact->instruction != index ||
            fact->function != instruction->function ||
            fact->coroutine_state != XR_SEMANTIC_INDEX_NONE ||
            !stable_id_is_zero(fact->coroutine_state_identity))
            return report(error, error_size, "XR_TARGET_1005",
                          "program graph debug row identity is invalid");
        if (operation == XR_SEMANTIC_INDEX_NONE) {
            if (fact->semantic_operation != XR_SEMANTIC_INDEX_NONE ||
                !stable_id_is_zero(fact->semantic_operation_identity) ||
                !stable_id_is_zero(fact->source_span_identity) ||
                !stable_id_is_zero(fact->owner_identity) ||
                !fingerprint_is_zero(fact->layout_fingerprint) ||
                fact->source_start_line || fact->source_start_column ||
                fact->source_end_line || fact->source_end_column)
                return report(error, error_size, "XR_TARGET_1005",
                              "program graph debug row invented a synthetic relation");
            continue;
        }
        const XrSemanticOperationRecord *semantic_operation =
            xr_semantic_plan_operation(semantic, operation);
        if (!semantic_operation || operation != fact->semantic_operation ||
            semantic_operation->function != target_function->semantic_function ||
            !xr_stable_id_equal(fact->semantic_operation_identity,
                                semantic_operation->id))
            return report(error, error_size, "XR_TARGET_1005",
                          "program graph debug semantic operation is invalid");
        if (semantic_operation->source_file) {
            const XrSemanticEntityRecord *span = debug_entity_for(
                semantic, XR_SEM_ENTITY_DEBUG_SPAN,
                XR_SEM_ENTITY_SUBJECT_OPERATION, operation);
            if (!span || !xr_stable_id_equal(fact->source_span_identity, span->id) ||
                fact->source_start_line != semantic_operation->source_start_line ||
                fact->source_start_column != semantic_operation->source_start_column ||
                fact->source_end_line != semantic_operation->source_end_line ||
                fact->source_end_column != semantic_operation->source_end_column)
                return report(error, error_size, "XR_TARGET_1005",
                              "program graph debug source span is invalid");
        } else if (!stable_id_is_zero(fact->source_span_identity) ||
                   fact->source_start_line || fact->source_start_column ||
                   fact->source_end_line || fact->source_end_column) {
            return report(error, error_size, "XR_TARGET_1005",
                          "program graph debug source span was guessed");
        }
        XrFingerprint expected_layout = zero_fingerprint;
        for (uint32_t layout = 0; layout < partition->layouts_count; layout++) {
            const XrTargetLayoutRecord *candidate =
                &plan->layouts[partition->layouts_begin + layout];
            if (candidate->semantic_type != semantic_operation->result_type)
                continue;
            if (!fingerprint_is_zero(expected_layout))
                return report(error, error_size, "XR_TARGET_1005",
                              "program graph debug layout is ambiguous");
            expected_layout = candidate->fingerprint;
        }
        if (!xr_fingerprint_equal(fact->layout_fingerprint, expected_layout))
            return report(error, error_size, "XR_TARGET_1005",
                          "program graph debug layout is invalid");
        XrStableId expected_owner = {{0}};
        if (ownership && semantic_operation->result_value != XR_SEMANTIC_INDEX_NONE) {
            for (uint32_t owner = 0;
                 owner < xr_ownership_certificate_owner_count(ownership); owner++) {
                const XrOwnershipOwnerRecord *record =
                    xr_ownership_certificate_owner(ownership, owner);
                if (!record)
                    return report(error, error_size, "XR_TARGET_1005",
                                  "program graph debug ownership is invalid");
                if (record->function != target_function->semantic_function ||
                    record->origin_value != semantic_operation->result_value)
                    continue;
                const XrSemanticEntityRecord *entity = debug_entity_for(
                    semantic, XR_SEM_ENTITY_OWNER, XR_SEM_ENTITY_SUBJECT_OWNER, owner);
                if (!entity || !stable_id_is_zero(expected_owner))
                    return report(error, error_size, "XR_TARGET_1005",
                                  "program graph debug owner is ambiguous");
                expected_owner = entity->id;
            }
        }
        if (!xr_stable_id_equal(fact->owner_identity, expected_owner))
            return report(error, error_size, "XR_TARGET_1005",
                          "program graph debug owner is invalid");
    }
    return true;
}

static bool graph_semantic_value_binding(const XrSemanticPlan *semantic, uint32_t value,
                                         uint32_t *function, uint32_t *type,
                                         uint32_t *defining_operation,
                                         bool *parameter_value) {
    if (function)
        *function = XR_SEMANTIC_INDEX_NONE;
    if (type)
        *type = XR_SEMANTIC_INDEX_NONE;
    if (defining_operation)
        *defining_operation = XR_SEMANTIC_INDEX_NONE;
    if (parameter_value)
        *parameter_value = false;
    uint32_t matched_function = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t row = 0; row < xr_semantic_plan_function_count(semantic); row++) {
        const XrSemanticFunctionRecord *candidate =
            xr_semantic_plan_function(semantic, row);
        if (!candidate || value < candidate->value_begin ||
            value - candidate->value_begin >= candidate->value_count)
            continue;
        if (matched_function != XR_SEMANTIC_INDEX_NONE)
            return false;
        matched_function = row;
    }
    if (matched_function == XR_SEMANTIC_INDEX_NONE)
        return false;
    uint32_t matched_type = XR_SEMANTIC_INDEX_NONE;
    uint32_t matched_operation = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t row = 0; row < xr_semantic_plan_parameter_count(semantic); row++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(semantic, row);
        if (!parameter || parameter->function != matched_function || parameter->value != value)
            continue;
        if (matched_type != XR_SEMANTIC_INDEX_NONE)
            return false;
        matched_type = parameter->type;
        if (parameter_value)
            *parameter_value = true;
    }
    for (uint32_t row = 0; row < xr_semantic_plan_operation_count(semantic); row++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, row);
        if (!operation || operation->function != matched_function ||
            operation->result_value != value)
            continue;
        if (matched_operation != XR_SEMANTIC_INDEX_NONE ||
            (matched_type != XR_SEMANTIC_INDEX_NONE &&
             (operation->opcode != XI_PARAM || operation->result_type != matched_type)))
            return false;
        matched_type = operation->result_type;
        matched_operation = row;
    }
    if (matched_type == XR_SEMANTIC_INDEX_NONE ||
        matched_type >= xr_semantic_plan_type_count(semantic))
        return false;
    if (function)
        *function = matched_function;
    if (type)
        *type = matched_type;
    if (defining_operation)
        *defining_operation = matched_operation;
    return true;
}

static bool graph_semantic_value_storage_kind(
    const XrSemanticPlan *semantic, uint32_t value, uint32_t *function,
    uint32_t *type, uint32_t *defining_operation, bool *parameter_value,
    uint16_t *machine_kind) {
    uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t semantic_type_index = XR_SEMANTIC_INDEX_NONE;
    uint32_t operation_index = XR_SEMANTIC_INDEX_NONE;
    bool is_parameter = false;
    if (!graph_semantic_value_binding(semantic, value, &semantic_function,
                                      &semantic_type_index, &operation_index,
                                      &is_parameter))
        return false;
    const XrSemanticTypeRecord *semantic_type =
        xr_semantic_plan_type(semantic, semantic_type_index);
    uint16_t scalar_kind = XR_MACHINE_REP_COUNT;
    int scalar = semantic_type
                     ? semantic_type_expected_rep(semantic_type, &scalar_kind)
                     : -1;
    const XrSemanticOperationRecord *definition =
        operation_index < xr_semantic_plan_operation_count(semantic)
            ? xr_semantic_plan_operation(semantic, operation_index)
            : NULL;
    bool generated_exact =
        !definition ||
        (definition->opcode < XI_OP_COUNT &&
         definition->effects == xi_generated_op_effects(definition->opcode) &&
         definition->result_ownership ==
             xi_generated_op_result_ownership(definition->opcode));
    if (!semantic_type || scalar < 0 || !generated_exact)
        return false;
    bool result_void =
        definition && xr_semantic_operation_result_void_governs_storage(
                          semantic, definition, value, semantic_type_index,
                          semantic_function);
    bool generated_result_void =
        definition && definition->opcode < XI_OP_COUNT &&
        xi_generated_op_result_kind(definition->opcode) == XI_GEN_RESULT_VOID;
    if (generated_result_void && !result_void &&
        !xr_semantic_unit_enum_type_is_exact(semantic_type))
        return false;
    if (function)
        *function = semantic_function;
    if (type)
        *type = semantic_type_index;
    if (defining_operation)
        *defining_operation = operation_index;
    if (parameter_value)
        *parameter_value = is_parameter;
    if (machine_kind)
        *machine_kind = result_void ? XR_MACHINE_REP_VOID
                                    : scalar == 1 ? scalar_kind
                                                  : XR_MACHINE_REP_DYN_VALUE;
    return true;
}

static const XrTargetValueRepRecord *graph_value_rep_for_semantic_value(
    const XrTargetPlan *plan, const XrTargetModulePartitionRecord *partition,
    uint32_t semantic_value) {
    const XrTargetValueRepRecord *found = NULL;
    for (uint32_t row = 0; row < partition->value_reps_count; row++) {
        const XrTargetValueRepRecord *candidate =
            &plan->value_reps[partition->value_reps_begin + row];
        if (candidate->semantic_value != semantic_value)
            continue;
        if (found)
            return NULL;
        found = candidate;
    }
    return found;
}

static bool verify_program_graph_instruction_semantics(
    const XrTargetPlan *plan, const XrTargetModulePartitionRecord *partition,
    const XrSemanticPlan *semantic, char *error, size_t error_size) {
    uint32_t operand_count = 0u;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(semantic, &operand_count);
    for (uint32_t local = 0; local < partition->instructions_count; local++) {
        const XrTargetInstructionRecord *instruction =
            &plan->instructions[partition->instructions_begin + local];
        const XrTargetFunctionRecord *target_function =
            instruction->function < plan->functions_count
                ? &plan->functions[instruction->function]
                : NULL;
        const XrTargetSlotRecord *result =
            instruction->result_slot < plan->slots_count
                ? &plan->slots[instruction->result_slot]
                : NULL;
        const XrTargetSlotRecord *left =
            instruction->operand_slots[0] < plan->slots_count
                ? &plan->slots[instruction->operand_slots[0]]
                : NULL;
        const XrTargetSlotRecord *right =
            instruction->operand_slots[1] < plan->slots_count
                ? &plan->slots[instruction->operand_slots[1]]
                : NULL;
        if (!target_function ||
            !graph_function_in_partition(partition, instruction->function))
            return report(error, error_size, "XR_TARGET_1003",
                          "program graph instruction function is invalid");
        uint32_t semantic_function = target_function->semantic_function;
        if (instruction->opcode == XR_TARGET_INSTRUCTION_PARAM_I64) {
            const XrSemanticParameterRecord *parameter = NULL;
            for (uint32_t row = 0; row < xr_semantic_plan_parameter_count(semantic); row++) {
                const XrSemanticParameterRecord *candidate =
                    xr_semantic_plan_parameter(semantic, row);
                if (!candidate || candidate->function != semantic_function ||
                    candidate->ordinal != instruction->immediate_bits)
                    continue;
                if (parameter)
                    return report(error, error_size, "XR_TARGET_1003",
                                  "program graph parameter instruction is ambiguous");
                parameter = candidate;
            }
            if (!parameter || !result || result->role != XR_TARGET_SLOT_PARAMETER ||
                result->semantic_operation != XR_SEMANTIC_INDEX_NONE ||
                result->semantic_value != parameter->value)
                return report(error, error_size, "XR_TARGET_1003",
                              "program graph parameter instruction is invalid");
            continue;
        }
        if (instruction->opcode == XR_TARGET_INSTRUCTION_CONST_I64) {
            const XrSemanticOperationRecord *operation =
                result ? xr_semantic_plan_operation(semantic,
                                                     result->semantic_operation)
                       : NULL;
            const XrSemanticConstantRecord *constant =
                operation ? xr_semantic_plan_constant(semantic, operation->constant) : NULL;
            if (!operation || !constant || operation->function != semantic_function ||
                operation->opcode != XI_CONST || operation->result_value != result->semantic_value ||
                constant->kind != XR_SEM_CONST_INT ||
                instruction->immediate_bits != (uint64_t) constant->integer)
                return report(error, error_size, "XR_TARGET_1003",
                              "program graph constant instruction is invalid");
            continue;
        }
        if (instruction->opcode == XR_TARGET_INSTRUCTION_ADD_WRAP_I64) {
            const XrSemanticOperationRecord *operation =
                result ? xr_semantic_plan_operation(semantic,
                                                     result->semantic_operation)
                       : NULL;
            if (!operation || !left || !right || !operands ||
                operation->function != semantic_function || operation->opcode != XI_ADD ||
                operation->result_value != result->semantic_value ||
                operation->operand_count != 2u || operand_count < 2u ||
                operation->operand_begin > operand_count - 2u ||
                operands[operation->operand_begin].value != left->semantic_value ||
                operands[operation->operand_begin + 1u].value != right->semantic_value)
                return report(error, error_size, "XR_TARGET_1003",
                              "program graph add instruction is invalid");
            continue;
        }
        if (instruction->opcode == XR_TARGET_INSTRUCTION_CALL_DIRECT_I64) {
            const XrTargetCallRecord *call =
                instruction->immediate_bits < plan->calls_count
                    ? &plan->calls[(uint32_t) instruction->immediate_bits]
                    : NULL;
            const XrSemanticOperationRecord *operation =
                call ? xr_semantic_plan_operation(semantic, call->semantic_operation) : NULL;
            if (!call || !result || !operation || operation->function != semantic_function ||
                operation->result_value != result->semantic_value ||
                call->result_slot != result->id)
                return report(error, error_size, "XR_TARGET_1003",
                              "program graph call instruction is invalid");
            continue;
        }
        if (instruction->opcode == XR_TARGET_INSTRUCTION_RETURN_I64) {
            uint32_t matches = 0u;
            for (uint32_t block = 0; block < xr_semantic_plan_block_count(semantic); block++) {
                const XrSemanticBlockRecord *candidate =
                    xr_semantic_plan_block(semantic, block);
                if (candidate && candidate->function == semantic_function && left &&
                    candidate->control_value == left->semantic_value)
                    matches++;
            }
            if (!left || matches != 1u)
                return report(error, error_size, "XR_TARGET_1003",
                              "program graph return instruction is invalid");
            continue;
        }
        return report(error, error_size, "XR_TARGET_1003",
                      "program graph instruction opcode is outside the bounded family");
    }
    return true;
}

static bool verify_program_graph_instruction_coverage(
    const XrTargetPlan *plan, const XrTargetModulePartitionRecord *partition,
    const XrSemanticPlan *semantic, char *error, size_t error_size) {
    uint32_t required = 0u;
    for (uint32_t parameter_index = 0;
         parameter_index < xr_semantic_plan_parameter_count(semantic);
         parameter_index++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(semantic, parameter_index);
        uint16_t kind = XR_MACHINE_REP_COUNT;
        if (!parameter || !graph_semantic_value_storage_kind(
                              semantic, parameter->value, NULL, NULL, NULL,
                              NULL, &kind))
            return report(error, error_size, "XR_TARGET_1003",
                          "program graph parameter storage authority is invalid");
        if (kind != XR_MACHINE_REP_I64)
            continue;
        required++;
        uint32_t matches = 0u;
        for (uint32_t local = 0; local < partition->instructions_count; local++) {
            const XrTargetInstructionRecord *instruction =
                &plan->instructions[partition->instructions_begin + local];
            const XrTargetFunctionRecord *function =
                instruction->function < plan->functions_count
                    ? &plan->functions[instruction->function]
                    : NULL;
            const XrTargetSlotRecord *result =
                instruction->result_slot < plan->slots_count
                    ? &plan->slots[instruction->result_slot]
                    : NULL;
            matches += instruction->opcode == XR_TARGET_INSTRUCTION_PARAM_I64 &&
                       function && function->semantic_function == parameter->function &&
                       result && result->semantic_value == parameter->value &&
                       instruction->immediate_bits == parameter->ordinal;
        }
        if (matches != 1u)
            return report(error, error_size, "XR_TARGET_1003",
                          "program graph parameter instruction coverage is not exact");
    }
    for (uint32_t operation_index = 0;
         operation_index < xr_semantic_plan_operation_count(semantic);
         operation_index++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic, operation_index);
        uint16_t target_opcode = XR_TARGET_INSTRUCTION_COUNT;
        if (operation && operation->opcode == XI_CONST)
            target_opcode = XR_TARGET_INSTRUCTION_CONST_I64;
        else if (operation && operation->opcode == XI_ADD)
            target_opcode = XR_TARGET_INSTRUCTION_ADD_WRAP_I64;
        else if (operation && operation->opcode == XI_CALL)
            target_opcode = XR_TARGET_INSTRUCTION_CALL_DIRECT_I64;
        else
            continue;
        uint16_t kind = XR_MACHINE_REP_COUNT;
        if (!graph_semantic_value_storage_kind(
                semantic, operation->result_value, NULL, NULL, NULL, NULL,
                &kind) || kind != XR_MACHINE_REP_I64)
            return report(error, error_size, "XR_TARGET_1003",
                          "program graph lowered operation storage is invalid");
        required++;
        uint32_t matches = 0u;
        for (uint32_t local = 0; local < partition->instructions_count; local++) {
            const XrTargetInstructionRecord *instruction =
                &plan->instructions[partition->instructions_begin + local];
            const XrTargetFunctionRecord *function =
                instruction->function < plan->functions_count
                    ? &plan->functions[instruction->function]
                    : NULL;
            const XrTargetSlotRecord *result =
                instruction->result_slot < plan->slots_count
                    ? &plan->slots[instruction->result_slot]
                    : NULL;
            matches += instruction->opcode == target_opcode && function &&
                       function->semantic_function == operation->function && result &&
                       result->semantic_operation == operation_index &&
                       result->semantic_value == operation->result_value;
        }
        if (matches != 1u)
            return report(error, error_size, "XR_TARGET_1003",
                          "program graph operation instruction coverage is not exact");
    }
    for (uint32_t block_index = 0;
         block_index < xr_semantic_plan_block_count(semantic); block_index++) {
        const XrSemanticBlockRecord *block =
            xr_semantic_plan_block(semantic, block_index);
        uint16_t kind = XR_MACHINE_REP_COUNT;
        if (!block || block->control_value == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (!graph_semantic_value_storage_kind(
                semantic, block->control_value, NULL, NULL, NULL, NULL, &kind) ||
            kind != XR_MACHINE_REP_I64)
            return report(error, error_size, "XR_TARGET_1003",
                          "program graph return storage authority is invalid");
        required++;
        uint32_t matches = 0u;
        for (uint32_t local = 0; local < partition->instructions_count; local++) {
            const XrTargetInstructionRecord *instruction =
                &plan->instructions[partition->instructions_begin + local];
            const XrTargetFunctionRecord *function =
                instruction->function < plan->functions_count
                    ? &plan->functions[instruction->function]
                    : NULL;
            const XrTargetSlotRecord *operand =
                instruction->operand_slots[0] < plan->slots_count
                    ? &plan->slots[instruction->operand_slots[0]]
                    : NULL;
            matches += instruction->opcode == XR_TARGET_INSTRUCTION_RETURN_I64 &&
                       function && function->semantic_function == block->function &&
                       operand && operand->semantic_value == block->control_value;
        }
        if (matches != 1u)
            return report(error, error_size, "XR_TARGET_1003",
                          "program graph return instruction coverage is not exact");
    }
    if (required != partition->instructions_count)
        return report(error, error_size, "XR_TARGET_1003",
                      "program graph instruction table has missing or extra rows");
    return true;
}

static const XrSemanticProgramFunctionBinding *
verify_graph_function_binding(const XrSemanticPlan *semantic, uint8_t flags) {
    const XrSemanticProgramFunctionBinding *found = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_program_function_binding_count(semantic); i++) {
        const XrSemanticProgramFunctionBinding *candidate =
            xr_semantic_plan_program_function_binding(semantic, i);
        if (!candidate || candidate->flags != flags)
            continue;
        if (found)
            return NULL;
        found = candidate;
    }
    return found;
}

static bool verify_program_graph_partitions(const XrTargetPlan *plan,
                                            uint32_t *entry_partition,
                                            uint32_t *producer_partition, char *error,
                                            size_t error_size) {
    *entry_partition = UINT32_MAX;
    *producer_partition = UINT32_MAX;
    if (plan->module_partitions_count != 2u || !plan->module_partitions ||
        plan->semantic_module_count != 2u || !plan->semantic_modules ||
        plan->semantic_dependency_count != 1u || !plan->semantic_dependencies ||
        !graph_partition_ranges_are_exact(plan, error, error_size))
        return false;
    for (uint32_t i = 0; i < 2u; i++) {
        const XrTargetModulePartitionRecord *partition = &plan->module_partitions[i];
        const XrSemanticPlan *semantic = graph_partition_semantic(plan, i);
        const XrSemanticProgramProvenance *program =
            semantic ? xr_semantic_plan_program_provenance(semantic) : NULL;
        if (!program || partition->program_module_row != i ||
            program->program_module_row != i ||
            !xr_stable_id_equal(partition->module_identity, program->program_module) ||
            !xr_fingerprint_equal(partition->semantic_fingerprint,
                                  xr_semantic_plan_fingerprint(semantic)))
            return report(error, error_size, "XR_TARGET_1001",
                          "program graph module partition identity is invalid");
        if (semantic == plan->semantic_plan) {
            if (*entry_partition != UINT32_MAX)
                return report(error, error_size, "XR_TARGET_1001",
                              "program graph entry partition is ambiguous");
            *entry_partition = i;
        } else {
            if (*producer_partition != UINT32_MAX || semantic != plan->semantic_dependencies[0])
                return report(error, error_size, "XR_TARGET_1001",
                              "program graph producer partition is ambiguous");
            *producer_partition = i;
        }
    }
    return *entry_partition != UINT32_MAX && *producer_partition != UINT32_MAX;
}

static bool verify_program_graph_rows(const XrTargetPlan *plan, char *error,
                                      size_t error_size) {
    for (uint32_t partition_index = 0; partition_index < 2u; partition_index++) {
        const XrTargetModulePartitionRecord *partition =
            &plan->module_partitions[partition_index];
        const XrSemanticPlan *semantic = graph_partition_semantic(plan, partition_index);
        uint32_t semantic_value_count = 0;
        if (!semantic ||
            partition->functions_count != xr_semantic_plan_function_count(semantic) ||
            !graph_semantic_value_count(semantic, &semantic_value_count))
            return false;
        for (uint32_t local_extent = 0; local_extent < partition->extents_count;
             local_extent++) {
            uint32_t extent_index = partition->extents_begin + local_extent;
            const XrTargetExtentRecord *extent = &plan->extents[extent_index];
            uint32_t references = 0u;
            for (uint32_t local_layout = 0; local_layout < partition->layouts_count;
                 local_layout++)
                references +=
                    plan->layouts[partition->layouts_begin + local_layout].extent == extent_index;
            if (extent->id != extent_index || extent->kind != XR_TARGET_EXTENT_FIXED ||
                extent->operand_count != 0u || extent->alignment != 0u ||
                extent->element_layout != XR_SEMANTIC_INDEX_NONE || extent->stride != 0u ||
                extent->provider != 0u || extent->flags != 0u || references != 1u)
                return report(error, error_size, "XR_TARGET_1002",
                              "program graph extent row is not exact");
        }
        uint32_t previous_value = 0;
        for (uint32_t i = 0; i < partition->value_reps_count; i++) {
            const XrTargetValueRepRecord *row =
                &plan->value_reps[partition->value_reps_begin + i];
            uint32_t semantic_function = XR_SEMANTIC_INDEX_NONE;
            uint32_t semantic_type_index = XR_SEMANTIC_INDEX_NONE;
            uint32_t defining_operation = XR_SEMANTIC_INDEX_NONE;
            bool parameter_value = false;
            uint16_t expected_kind = XR_MACHINE_REP_COUNT;
            bool value_exact = graph_semantic_value_storage_kind(
                semantic, row->semantic_value, &semantic_function,
                &semantic_type_index, &defining_operation, &parameter_value,
                &expected_kind);
            const XrTargetSlotRecord *bound_slot = NULL;
            uint32_t bound_slot_count = 0u;
            for (uint32_t slot = 0; slot < partition->slots_count; slot++) {
                const XrTargetSlotRecord *candidate =
                    &plan->slots[partition->slots_begin + slot];
                if (candidate->semantic_value != row->semantic_value)
                    continue;
                bound_slot = candidate;
                bound_slot_count++;
            }
            XrStableId expected_slot_identity = {{0}};
            uint32_t local_function =
                bound_slot && bound_slot->function < plan->functions_count
                    ? plan->functions[bound_slot->function].semantic_function
                    : XR_SEMANTIC_INDEX_NONE;
            bool slot_exact =
                expected_kind == XR_MACHINE_REP_VOID
                    ? bound_slot_count == 0u && row->slot == XR_SEMANTIC_INDEX_NONE
                    : bound_slot_count == 1u && row->slot == bound_slot->id &&
                          local_function == semantic_function &&
                          bound_slot->role ==
                              (parameter_value ? XR_TARGET_SLOT_PARAMETER
                                               : XR_TARGET_SLOT_TEMPORARY) &&
                          bound_slot->semantic_operation ==
                              (parameter_value ? XR_SEMANTIC_INDEX_NONE
                                               : defining_operation) &&
                          reconstruct_value_slot_identity_for_semantic(
                              semantic, bound_slot, bound_slot->function,
                              row->semantic_value, semantic_function,
                              &expected_slot_identity) &&
                          xr_stable_id_equal(bound_slot->identity,
                                             expected_slot_identity) &&
                          bound_slot->register_rep == row->register_rep &&
                          bound_slot->memory_rep == row->memory_rep;
            if (row->semantic_value >= semantic_value_count || !value_exact ||
                (i && row->semantic_value <= previous_value) ||
                row->register_rep >= plan->machine_reps_count ||
                row->memory_rep >= plan->machine_reps_count ||
                plan->machine_reps[row->register_rep].kind != expected_kind ||
                plan->machine_reps[row->memory_rep].kind != expected_kind ||
                !machine_rep_allows_conversion(plan, row->register_rep, row->memory_rep) ||
                !slot_exact)
                return report(error, error_size, "XR_TARGET_1001",
                              "program graph value row is not partition-local");
            previous_value = row->semantic_value;
        }
        uint32_t required_value_count = 0u;
        uint32_t required_layout_count = 0u;
        for (uint32_t value = 0; value < semantic_value_count; value++) {
            uint32_t semantic_type = XR_SEMANTIC_INDEX_NONE;
            uint16_t machine_kind = XR_MACHINE_REP_COUNT;
            if (!graph_semantic_value_storage_kind(
                    semantic, value, NULL, &semantic_type, NULL, NULL,
                    &machine_kind))
                continue;
            required_value_count++;
            if (!graph_value_rep_for_semantic_value(plan, partition, value))
                return report(error, error_size, "XR_TARGET_1001",
                              "program graph required value row is missing or ambiguous");
            if (machine_kind == XR_MACHINE_REP_VOID)
                continue;
            bool first_type = true;
            for (uint32_t prior = 0; prior < value; prior++) {
                uint32_t prior_type = XR_SEMANTIC_INDEX_NONE;
                uint16_t prior_kind = XR_MACHINE_REP_COUNT;
                if (graph_semantic_value_storage_kind(
                        semantic, prior, NULL, &prior_type, NULL, NULL,
                        &prior_kind) && prior_kind != XR_MACHINE_REP_VOID &&
                    prior_type == semantic_type) {
                    first_type = false;
                    break;
                }
            }
            if (!first_type)
                continue;
            required_layout_count++;
            uint32_t matching_layouts = 0u;
            for (uint32_t layout = 0; layout < partition->layouts_count; layout++)
                matching_layouts +=
                    plan->layouts[partition->layouts_begin + layout].semantic_type ==
                    semantic_type;
            if (matching_layouts != 1u)
                return report(error, error_size, "XR_TARGET_1002",
                              "program graph required layout row is missing or ambiguous");
        }
        if (partition->value_reps_count != required_value_count ||
            partition->layouts_count != required_layout_count ||
            partition->extents_count != required_layout_count)
            return report(error, error_size, "XR_TARGET_1002",
                          "program graph derived target row coverage is incomplete");
        uint32_t previous_layout_type = XR_SEMANTIC_INDEX_NONE;
        for (uint32_t i = 0; i < partition->layouts_count; i++) {
            uint32_t index = partition->layouts_begin + i;
            const XrTargetLayoutRecord *row = &plan->layouts[index];
            const XrSemanticTypeRecord *semantic_type =
                xr_semantic_plan_type(semantic, row->semantic_type);
            uint16_t expected_rep = XR_MACHINE_REP_COUNT;
            int scalar = semantic_type
                             ? semantic_type_expected_rep(semantic_type, &expected_rep)
                             : -1;
            bool physical_match = false;
            for (uint32_t rep = 0; rep < plan->machine_reps_count; rep++) {
                uint16_t required_rep = row->kind == XR_TARGET_LAYOUT_DYNAMIC
                                            ? XR_MACHINE_REP_DYN_VALUE
                                            : expected_rep;
                if (plan->machine_reps[rep].kind == required_rep &&
                    plan->machine_reps[rep].memory_size == row->fixed_prefix_size &&
                    plan->machine_reps[rep].memory_align == row->align)
                    physical_match = true;
            }
            XrFingerprint actual;
            xr_target_layout_compute_fingerprint(plan, index, &actual);
            if (row->id != index || row->semantic_type >= xr_semantic_plan_type_count(semantic) ||
                (previous_layout_type != XR_SEMANTIC_INDEX_NONE &&
                 row->semantic_type <= previous_layout_type) ||
                (row->kind != XR_TARGET_LAYOUT_SCALAR &&
                 row->kind != XR_TARGET_LAYOUT_DYNAMIC) ||
                row->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
                !is_power_of_two(row->align) ||
                row->fixed_prefix_size % row->align != 0u ||
                (row->kind == XR_TARGET_LAYOUT_SCALAR &&
                 (scalar != 1 || expected_rep == XR_MACHINE_REP_VOID)) ||
                (row->kind == XR_TARGET_LAYOUT_DYNAMIC && scalar != 0) || !physical_match ||
                row->field_count != 0u || row->root_field_count != 0u ||
                !stable_id_is_zero(row->destructor) || !stable_id_is_zero(row->clone) ||
                !stable_id_is_zero(row->equality_hash) ||
                row->extent < partition->extents_begin ||
                row->extent - partition->extents_begin >= partition->extents_count ||
                row->field_begin < partition->fields_begin ||
                row->field_begin - partition->fields_begin > partition->fields_count ||
                row->field_count > partition->fields_count -
                                       (row->field_begin - partition->fields_begin) ||
                !xr_fingerprint_equal(row->fingerprint, actual))
                return report(error, error_size, "XR_TARGET_1002",
                              "program graph layout row is not partition-local");
            previous_layout_type = row->semantic_type;
        }
        uint32_t next_slot = partition->slots_begin;
        for (uint32_t i = 0; i < partition->functions_count; i++) {
            uint32_t index = partition->functions_begin + i;
            const XrTargetFunctionRecord *row = &plan->functions[index];
            if (row->id != index ||
                row->semantic_function != i ||
                row->slot_begin != next_slot ||
                row->slot_begin < partition->slots_begin ||
                row->slot_begin - partition->slots_begin > partition->slots_count ||
                row->slot_count > partition->slots_count -
                                      (row->slot_begin - partition->slots_begin) ||
                row->root_begin != 0u || row->root_count != 0u ||
                row->cleanup_begin != 0u || row->cleanup_count != 0u ||
                row->coroutine_begin != 0u || row->coroutine_count != 0u ||
                row->reserved != 0u)
                return report(error, error_size, "XR_TARGET_1002",
                              "program graph function row is not partition-local");
            uint32_t previous_end = 0u;
            uint32_t expected_frame_align = 1u;
            for (uint32_t local_slot = 0; local_slot < row->slot_count; local_slot++) {
                uint32_t slot_index = row->slot_begin + local_slot;
                const XrTargetSlotRecord *slot = &plan->slots[slot_index];
                uint32_t expected_offset = 0u, slot_end = 0u;
                uint32_t value_bindings = 0u;
                for (uint32_t value = 0; value < partition->value_reps_count; value++)
                    value_bindings +=
                        plan->value_reps[partition->value_reps_begin + value].slot == slot_index;
                if (slot->id != slot_index || slot->function != index ||
                    value_bindings != 1u ||
                    stable_id_is_zero(slot->identity) || !slot->size ||
                    !is_power_of_two(slot->align) || slot->offset % slot->align != 0u ||
                    !xr_checked_align_u32(previous_end, slot->align, &expected_offset) ||
                    slot->offset != expected_offset ||
                    !checked_u32_add(slot->offset, slot->size, &slot_end) ||
                    slot->register_rep >= plan->machine_reps_count ||
                    slot->memory_rep >= plan->machine_reps_count ||
                    slot->role <= XR_TARGET_SLOT_ROLE_INVALID ||
                    slot->role >= XR_TARGET_SLOT_ROLE_COUNT ||
                    slot->root_kind > XR_TARGET_ROOT_VIEW_OWNER ||
                    slot->ownership > XR_TARGET_OWNERSHIP_SHARED || slot->reserved != 0u ||
                    slot->debug_variable != XR_SEMANTIC_INDEX_NONE ||
                    (slot->semantic_value != XR_SEMANTIC_INDEX_NONE &&
                     slot->semantic_value >= semantic_value_count) ||
                    (slot->semantic_operation != XR_SEMANTIC_INDEX_NONE &&
                     slot->semantic_operation >= xr_semantic_plan_operation_count(semantic)) ||
                    (local_slot &&
                     xr_stable_id_compare(plan->slots[slot_index - 1u].identity,
                                          slot->identity) >= 0))
                    return report(error, error_size, "XR_TARGET_1002",
                                  "program graph slot row is not partition-local");
                const XrTargetMachineRepRecord *memory =
                    &plan->machine_reps[slot->memory_rep];
                if (slot->size != memory->memory_size || slot->align != memory->memory_align ||
                    !machine_rep_allows_conversion(plan, slot->register_rep,
                                                   slot->memory_rep) ||
                    slot->root_kind != memory->root_kind ||
                    slot->ownership != memory->ownership)
                    return report(error, error_size, "XR_TARGET_1002",
                                  "program graph slot representation is invalid");
                if (slot->align > expected_frame_align)
                    expected_frame_align = slot->align;
                previous_end = slot_end;
            }
            uint32_t expected_frame_size = 0u;
            if (!xr_checked_align_u32(previous_end, expected_frame_align,
                                      &expected_frame_size) ||
                row->frame_align != expected_frame_align ||
                row->frame_size != expected_frame_size)
                return report(error, error_size, "XR_TARGET_1002",
                              "program graph function frame is invalid");
            next_slot += row->slot_count;
        }
        if (next_slot != partition->slots_begin + partition->slots_count)
            return report(error, error_size, "XR_TARGET_1002",
                          "program graph slot ranges are incomplete");
        for (uint32_t i = 0; i < partition->instructions_count; i++) {
            uint32_t index = partition->instructions_begin + i;
            const XrTargetInstructionRecord *row = &plan->instructions[index];
            if (row->id != index || !graph_function_in_partition(partition, row->function) ||
                (row->result_slot != XR_SEMANTIC_INDEX_NONE &&
                 !graph_slot_in_function(plan, row->function, row->result_slot)))
                return report(error, error_size, "XR_TARGET_1003",
                              "program graph instruction row is not partition-local");
            for (uint32_t operand = 0; operand < row->operand_count; operand++)
                if (!graph_slot_in_function(plan, row->function, row->operand_slots[operand]))
                    return report(error, error_size, "XR_TARGET_1003",
                                  "program graph instruction operand is not function-local");
        }
        if (!verify_program_graph_debug_partition(plan, partition, semantic,
                                                  error, error_size))
            return false;
        if (!verify_program_graph_instruction_semantics(plan, partition, semantic,
                                                        error, error_size))
            return false;
        if (!verify_program_graph_instruction_coverage(plan, partition, semantic,
                                                       error, error_size))
            return false;
    }
    for (uint32_t i = 0; i < plan->instructions_count;) {
        uint32_t function = plan->instructions[i].function;
        uint32_t end = i + 1u;
        while (end < plan->instructions_count &&
               plan->instructions[end].function == function)
            end++;
        if (function >= plan->functions_count ||
            !xr_target_instruction_rows_control_flow_is_exact(
                &plan->instructions[i], end - i, plan->functions[function].slot_begin,
                plan->functions[function].slot_count, plan->calls, plan->calls_count,
                plan->call_arguments, plan->call_arguments_count, NULL, 0u,
                plan->coroutines, plan->coroutines_count))
            return report(error, error_size, "XR_TARGET_1003",
                          "program graph instruction control flow is not exact");
        i = end;
    }
    return true;
}

static bool verify_program_graph_plan(const XrTargetPlan *plan, char *error,
                                      size_t error_size) {
    char nested_error[512] = {0};
    if (!xr_target_semantic_program_module_set_verify(
            (const XrSemanticPlan *const *) plan->semantic_modules,
            plan->semantic_module_count, nested_error, sizeof(nested_error)))
        return report(error, error_size, "XR_TARGET_1000",
                      "program graph semantic module set is not exactly verified");
    XrFingerprint semantic_set;
    if (!xr_target_semantic_module_set_fingerprint(
            (const XrSemanticPlan *const *) plan->semantic_modules,
            plan->semantic_module_count, &semantic_set) ||
        !xr_fingerprint_equal(plan->semantic_fingerprint, semantic_set))
        return report(error, error_size, "XR_TARGET_1000",
                      "program graph semantic module-set fingerprint is invalid");
    if (!xr_target_profile_verify(plan->profile, error, error_size) ||
        !verify_resource_budgets(plan, error, error_size) ||
        !verify_machine_reps(plan, error, error_size) ||
        !verify_program_graph_machine_rep_set(plan, error, error_size))
        return false;
    uint32_t entry_partition = UINT32_MAX, producer_partition = UINT32_MAX;
    if (!verify_program_graph_partitions(plan, &entry_partition, &producer_partition, error,
                                         error_size))
        return false;
    if (plan->program_graphs_count != 1u || !plan->program_graphs ||
        plan->entry_expectations_count != 0u || plan->fields_count != 0u ||
        plan->storage_count != 0u ||
        plan->allocations_count != 0u || plan->extent_operands_count != 0u ||
        plan->root_maps_count != 0u || plan->root_slots_count != 0u ||
        plan->cleanups_count != 0u || plan->adapters_count != 0u ||
        plan->coroutines_count != 0u || plan->calls_count != 1u ||
        plan->call_arguments_count != 1u)
        return report(error, error_size, "XR_TARGET_1001",
                      "program graph bounded target tables are not exact");

    const XrTargetProgramGraphRecord *graph = &plan->program_graphs[0];
    const XrSemanticPlan *entry = graph_partition_semantic(plan, entry_partition);
    const XrSemanticPlan *producer = graph_partition_semantic(plan, producer_partition);
    const XrSemanticProgramProvenance *program = xr_semantic_plan_program_provenance(entry);
    const XrSemanticProgramFunctionBinding *entry_function =
        verify_graph_function_binding(entry, XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY);
    const XrSemanticProgramFunctionBinding *producer_function =
        verify_graph_function_binding(producer, XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED);
    const XrSemanticProgramCallBinding *call_binding =
        xr_semantic_plan_program_call_binding_count(entry) == 1u
            ? xr_semantic_plan_program_call_binding(entry, 0)
            : NULL;
    const XrSemanticOperationRecord *operation =
        call_binding ? xr_semantic_plan_operation(entry, call_binding->operation) : NULL;
    const XrTargetCallRecord *call =
        graph->target_call < plan->calls_count ? &plan->calls[graph->target_call] : NULL;
    const XrSemanticCallTargetRecord *target =
        call && call->semantic_call_target < xr_semantic_plan_call_target_count(entry)
            ? xr_semantic_plan_call_target(entry, call->semantic_call_target)
            : NULL;
    const XrSemanticSourceExportRecord *source_export =
        target && target->source_export < xr_semantic_plan_source_export_count(producer)
            ? xr_semantic_plan_source_export(producer, target->source_export)
            : NULL;
    const XrSemanticFunctionRecord *entry_semantic_function =
        entry_function ? xr_semantic_plan_function(entry, entry_function->semantic_function)
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
    const XrTargetCallArgumentRecord *argument =
        graph->target_argument < plan->call_arguments_count
            ? &plan->call_arguments[graph->target_argument]
            : NULL;
    uint32_t semantic_operand_count = 0u;
    const XrSemanticOperandRecord *semantic_operands =
        xr_semantic_plan_operands(entry, &semantic_operand_count);
    uint32_t semantic_argument = operation ? operation->operand_begin + 1u
                                           : XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperandRecord *semantic_argument_row =
        semantic_operands && semantic_argument < semantic_operand_count
            ? &semantic_operands[semantic_argument]
            : NULL;
    const XrTargetValueRepRecord *caller_value =
        semantic_argument_row
            ? graph_value_rep_for_semantic_value(
                  plan, &plan->module_partitions[entry_partition],
                  semantic_argument_row->value)
            : NULL;
    const XrTargetValueRepRecord *callee_value =
        parameter ? graph_value_rep_for_semantic_value(
                        plan, &plan->module_partitions[producer_partition],
                        parameter->value)
                  : NULL;
    const XrTargetValueRepRecord *result_value =
        operation ? graph_value_rep_for_semantic_value(
                        plan, &plan->module_partitions[entry_partition],
                        operation->result_value)
                  : NULL;
    XrStableId expected_call_identity = {{0}};
    XrStableId expected_argument_identity = {{0}};
    const XrTargetProfileDraft *profile_facts = xr_target_profile_facts(plan->profile);
    bool call_identity_exact =
        target && operation && reconstruct_call_identity(
                                   "xray-target-call-v5", target->id,
                                   operation->id, 0u, &expected_call_identity);
    bool argument_identity_exact =
        target && parameter && reconstruct_call_identity(
                                  "xray-target-source-call-argument-v1", target->id,
                                  parameter->id, 0u, &expected_argument_identity);
    bool reference_parameter = parameter && parameter->mode == XR_PARAM_REF;
    uint8_t expected_argument_mode =
        reference_parameter ? XR_TARGET_CALL_REFERENCE : XR_TARGET_CALL_VALUE;
    uint8_t expected_argument_ownership =
        reference_parameter
            ? XR_TARGET_CALL_WRITEBACK
            : semantic_argument_row &&
                      semantic_argument_row->ownership_action == XR_SEM_OPERAND_CONSUME
                  ? XR_TARGET_CALL_CONSUME
                  : XR_TARGET_CALL_READ;
    uint8_t expected_argument_flags =
        reference_parameter ? XR_TARGET_CALL_ARGUMENT_ADDRESSABLE : 0u;
    if (!program || !entry_function || !producer_function || !call_binding || !operation ||
        !call || !target || !source_export || !entry_semantic_function ||
        !producer_semantic_function || !parameter || !argument ||
        graph->schema != XR_TARGET_PROGRAM_GRAPH_SCHEMA_VERSION ||
        graph->family != XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL ||
        graph->module_count != 2u || graph->function_count != program->function_count ||
        graph->export_count != 1u || graph->entry_count != 1u || graph->call_count != 1u ||
        graph->argument_count != 1u || graph->entry_partition != entry_partition ||
        graph->producer_partition != producer_partition ||
        graph->entry_semantic_function != entry_function->semantic_function ||
        graph->producer_semantic_function != producer_function->semantic_function ||
        graph->entry_semantic_operation != call_binding->operation ||
        graph->producer_semantic_export != target->source_export ||
        graph->entry_semantic_dependency != target->dependency ||
        graph->producer_semantic_parameter != producer_semantic_function->parameter_begin ||
        graph->target_call != call->id || graph->target_argument != call->argument_begin ||
        graph->caller_slot != argument->caller_slot || graph->callee_slot != argument->callee_slot ||
        graph->argument_ordinal != argument->ordinal ||
        graph->flags !=
            (XR_TARGET_PROGRAM_GRAPH_SINGLE_PLAN | XR_TARGET_PROGRAM_GRAPH_DIRECT_I64) ||
        !xr_fingerprint_equal(graph->program_fingerprint, program->program_fingerprint) ||
        !xr_stable_id_equal(graph->generation_identity, program->generation_identity) ||
        !xr_fingerprint_equal(graph->target_profile_fingerprint,
                              xr_target_profile_fingerprint(plan->profile)) ||
        !xr_stable_id_equal(graph->entry_function_identity,
                            entry_function->program_function) ||
        !xr_stable_id_equal(graph->producer_function_identity,
                            producer_function->program_function) ||
        graph->entry_function_flags != entry_function->flags ||
        graph->producer_function_flags != producer_function->flags || graph->reserved16 != 0u ||
        !xr_stable_id_equal(graph->export_identity, source_export->id) ||
        !xr_stable_id_equal(graph->exported_function_identity,
                            source_export->exported_entity) ||
        !xr_stable_id_equal(graph->entry_identity, entry_semantic_function->id) ||
        !xr_stable_id_equal(graph->call_identity, call_binding->program_call) ||
        !xr_stable_id_equal(graph->callsite_identity, call_binding->callsite) ||
        !xr_stable_id_equal(graph->resolver_binding, call_binding->resolver_binding) ||
        !xr_stable_id_equal(graph->argument_identity, argument->identity) ||
        !xr_stable_id_equal(graph->parameter_identity, parameter->id))
        return report(error, error_size, "XR_TARGET_1001",
                      "program graph stable witness is not exact");

    const XrTargetFunctionRecord *entry_target_function =
        graph->entry_target_function < plan->functions_count
            ? &plan->functions[graph->entry_target_function]
            : NULL;
    const XrTargetFunctionRecord *producer_target_function =
        graph->producer_target_function < plan->functions_count
            ? &plan->functions[graph->producer_target_function]
            : NULL;
    if (!entry_target_function || !producer_target_function ||
        !graph_function_in_partition(&plan->module_partitions[entry_partition],
                                     graph->entry_target_function) ||
        !graph_function_in_partition(&plan->module_partitions[producer_partition],
                                     graph->producer_target_function) ||
        entry_target_function->semantic_function != entry_function->semantic_function ||
        producer_target_function->semantic_function != producer_function->semantic_function ||
        call->calling_convention != XR_TARGET_CALL_CONVENTION_PROGRAM_DIRECT ||
        call->target_kind != XR_TARGET_CALL_TARGET_PROGRAM_DIRECT ||
        !call_identity_exact || !argument_identity_exact ||
        !xr_stable_id_equal(call->identity, expected_call_identity) ||
        call->semantic_call_target >= xr_semantic_plan_call_target_count(entry) ||
        target->operation != call_binding->operation ||
        target->kind != XR_SEM_CALL_TARGET_SOURCE_EXPORT ||
        call->semantic_operation != call_binding->operation ||
        call->caller_function != graph->entry_target_function ||
        call->callee_function != graph->producer_target_function ||
        call->source_dependency != target->dependency ||
        call->source_export != target->source_export ||
        !xr_stable_id_equal(call->source_export_identity, target->export_identity) ||
        !xr_stable_id_equal(call->source_callee_identity, target->callee_function) ||
        call->result_value != operation->result_value || !result_value ||
        call->result_slot != result_value->slot ||
        call->caller_storage_slot != XR_SEMANTIC_INDEX_NONE ||
        call->error_slot != XR_SEMANTIC_INDEX_NONE ||
        call->argument_begin != graph->target_argument || call->adapter_begin != 0u ||
        call->result_register_rep != result_value->register_rep ||
        call->result_memory_rep != result_value->memory_rep ||
        call->error_register_rep >= plan->machine_reps_count ||
        call->error_memory_rep >= plan->machine_reps_count ||
        plan->machine_reps[call->error_register_rep].kind != XR_MACHINE_REP_VOID ||
        plan->machine_reps[call->error_memory_rep].kind != XR_MACHINE_REP_VOID ||
        call->argument_count != 1u || call->adapter_count != 0u ||
        !profile_facts || call->native_abi != profile_facts->machine.native_abi ||
        call->flags != 0u || call->result_mode != XR_TARGET_CALL_VALUE ||
        call->result_ownership != XR_TARGET_CALL_NONE ||
        call->error_mode != XR_TARGET_CALL_NO_CALL_OWNED_CHANNEL ||
        call->array_intrinsic_kind != XR_TARGET_ARRAY_INTRINSIC_NONE ||
        call->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        call->array_hof_kind != XR_TARGET_ARRAY_HOF_NONE ||
        call->array_result_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        call->reserved8[0] != 0u || call->reserved8[1] != 0u ||
        call->reserved8[2] != 0u ||
        operation->opcode != XI_CALL || operation->operand_count != 2u ||
        !semantic_argument_row || semantic_argument_row->role != XR_SEM_OPERAND_ARGUMENT ||
        semantic_argument_row->parameter != 0 ||
        (semantic_argument_row->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0u ||
        !parameter || parameter->function != producer_function->semantic_function ||
        parameter->ordinal != 0u ||
        semantic_argument_row->parameter_mode != parameter->mode ||
        semantic_argument_row->transfer_mode != parameter->transfer_mode ||
        !xr_semantic_parameter_type_admits_argument(
            producer, xr_semantic_plan_type(producer, parameter->type),
            xr_semantic_plan_type(entry, semantic_argument_row->type)) ||
        !xr_stable_id_equal(argument->identity, expected_argument_identity) ||
        argument->call != call->id || argument->semantic_operand != semantic_argument ||
        argument->semantic_value != semantic_argument_row->value ||
        argument->ordinal != 0u ||
        argument->callee_parameter != producer_semantic_function->parameter_begin ||
        !caller_value || !callee_value || argument->caller_slot != caller_value->slot ||
        argument->callee_slot != callee_value->slot ||
        argument->register_rep != caller_value->register_rep ||
        argument->memory_rep != caller_value->memory_rep ||
        argument->callee_register_rep != callee_value->register_rep ||
        argument->callee_memory_rep != callee_value->memory_rep ||
        argument->mode != expected_argument_mode ||
        argument->ownership != expected_argument_ownership ||
        argument->transfer_mode != semantic_argument_row->transfer_mode ||
        argument->flags != expected_argument_flags ||
        argument->array_element_storage != XR_TARGET_ARRAY_STORAGE_NONE ||
        argument->reserved8[0] != 0u || argument->reserved8[1] != 0u ||
        argument->reserved8[2] != 0u ||
        !graph_slot_in_function(plan, graph->entry_target_function, argument->caller_slot) ||
        !graph_slot_in_function(plan, graph->producer_target_function, argument->callee_slot) ||
        argument->register_rep != argument->callee_register_rep ||
        argument->memory_rep != argument->callee_memory_rep)
        return report(error, error_size, "XR_TARGET_1003",
                      "program graph direct call join is not exact");

    uint32_t direct_calls = 0, entry_calls = 0;
    for (uint32_t i = 0; i < plan->instructions_count; i++) {
        const XrTargetInstructionRecord *instruction = &plan->instructions[i];
        entry_calls += instruction->opcode == XR_TARGET_INSTRUCTION_CALL_ENTRY_I64;
        if (instruction->opcode != XR_TARGET_INSTRUCTION_CALL_DIRECT_I64)
            continue;
        direct_calls++;
        if (instruction->function != graph->entry_target_function ||
            instruction->result_slot != call->result_slot ||
            instruction->immediate_bits != graph->target_call)
            return report(error, error_size, "XR_TARGET_1003",
                          "program graph direct-call instruction is invalid");
    }
    if (direct_calls != 1u || entry_calls != 0u ||
        !verify_extents(plan, error, error_size) ||
        !verify_program_graph_rows(plan, error, error_size) ||
        !verify_extent_references(plan, error, error_size) ||
        !xr_target_instruction_program_verify(plan, error, error_size) ||
        !verify_adapters_and_capabilities(plan, error, error_size))
        return false;
    XrFingerprint call_fingerprint, plan_fingerprint;
    xr_target_call_compute_fingerprint(plan, graph->target_call, &call_fingerprint);
    xr_target_plan_compute_fingerprint(plan, &plan_fingerprint);
    if (!xr_fingerprint_equal(call->fingerprint, call_fingerprint) ||
        !xr_fingerprint_equal(plan->fingerprint, plan_fingerprint))
        return report(error, error_size, "XR_TARGET_1005",
                      "program graph target fingerprint changed after freeze");
    return true;
}

bool xr_target_plan_verify(const XrTargetPlan *plan, char *error, size_t error_size) {
    if (!plan || !plan->frozen || !plan->semantic_plan || !plan->profile)
        return report(error, error_size, "XR_EXEC_5000", "verifier requires a frozen TargetPlan");
    if (plan->schema_version != XR_TARGET_PLAN_SCHEMA_VERSION)
        return report(error, error_size, "XR_ARTIFACT_2000",
                      "TargetPlan schema version is not exactly supported");
    if (plan->completed_family_mask != XR_TARGET_REQUIRED_FAMILIES)
        return report(error, error_size, "XR_TARGET_1001",
                      "TargetPlan family coverage is incomplete or unsupported");
    const XrSemanticProgramProvenance *program =
        xr_semantic_plan_program_provenance(plan->semantic_plan);
    if (program && program->program_family ==
                       XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL)
        return verify_program_graph_plan(plan, error, error_size);
    if (plan->program_graphs_count || plan->module_partitions_count ||
        plan->semantic_module_count || plan->semantic_modules)
        return report(error, error_size, "XR_TARGET_1001",
                      "non-graph TargetPlan carries program graph authority");
    char nested_error[512] = {0};
    bool semantic_verified =
        plan->semantic_dependency_count == 0
            ? xr_semantic_plan_verify(plan->semantic_plan, nested_error, sizeof(nested_error))
            : xr_semantic_plan_verify_module_set(
                  plan->semantic_plan, (const XrSemanticPlan *const *) plan->semantic_dependencies,
                  plan->semantic_dependency_count, nested_error, sizeof(nested_error));
    if (!semantic_verified ||
        plan->semantic_dependency_count != xr_semantic_plan_dependency_count(plan->semantic_plan) ||
        !xr_fingerprint_equal(plan->semantic_fingerprint,
                              xr_semantic_plan_fingerprint(plan->semantic_plan)))
        return report(error, error_size, "XR_TARGET_1000",
                      "TargetPlan semantic fingerprint does not match its exact input");
    if (!xr_target_profile_verify(plan->profile, error, error_size) ||
        !verify_resource_budgets(plan, error, error_size) ||
        !verify_machine_reps(plan, error, error_size))
        return false;
    uint8_t *exact_direct_callees = NULL;
    uint32_t *direct_callee_targets = NULL;
    if (!collect_exact_direct_local_callee_values(plan, &exact_direct_callees,
                                                  &direct_callee_targets, error, error_size))
        return false;
    uint8_t *exact_go_callees = NULL;
    uint32_t *go_callee_targets = NULL;
    if (!collect_exact_direct_local_go_callee_values(plan, &exact_go_callees, &go_callee_targets,
                                                     error, error_size)) {
        xr_free(exact_direct_callees);
        xr_free(direct_callee_targets);
        return false;
    }
    uint8_t *exact_channel_values = NULL;
    if (!collect_exact_channel_values(plan, &exact_channel_values, error, error_size)) {
        xr_free(exact_direct_callees);
        xr_free(direct_callee_targets);
        xr_free(exact_go_callees);
        xr_free(go_callee_targets);
        return false;
    }
    uint8_t *exact_channel_receives = NULL;
    if (!collect_exact_channel_receive_values(plan, exact_channel_values, &exact_channel_receives,
                                              error, error_size)) {
        xr_free(exact_direct_callees);
        xr_free(direct_callee_targets);
        xr_free(exact_go_callees);
        xr_free(go_callee_targets);
        xr_free(exact_channel_values);
        return false;
    }
    uint8_t *exact_source_namespaces = NULL;
    if (!collect_exact_source_namespace_values(plan, &exact_source_namespaces, error, error_size)) {
        xr_free(exact_direct_callees);
        xr_free(direct_callee_targets);
        xr_free(exact_go_callees);
        xr_free(go_callee_targets);
        xr_free(exact_channel_values);
        xr_free(exact_channel_receives);
        return false;
    }
    uint8_t *exact_native_module_namespaces = NULL;
    if (!collect_exact_native_module_namespace_values(plan, &exact_native_module_namespaces, error,
                                                      error_size)) {
        xr_free(exact_direct_callees);
        xr_free(direct_callee_targets);
        xr_free(exact_go_callees);
        xr_free(go_callee_targets);
        xr_free(exact_channel_values);
        xr_free(exact_channel_receives);
        xr_free(exact_source_namespaces);
        return false;
    }
    if (!verify_value_reps(plan, exact_direct_callees, exact_go_callees, exact_channel_values,
                           exact_channel_receives, exact_source_namespaces,
                           exact_native_module_namespaces, error, error_size) ||
        !verify_extents(plan, error, error_size)) {
        xr_free(exact_direct_callees);
        xr_free(direct_callee_targets);
        xr_free(exact_go_callees);
        xr_free(go_callee_targets);
        xr_free(exact_channel_values);
        xr_free(exact_channel_receives);
        xr_free(exact_source_namespaces);
        xr_free(exact_native_module_namespaces);
        return false;
    }
    uint8_t *exact_dynamic_types = NULL;
    if (!collect_exact_dynamic_types(plan, exact_direct_callees, exact_go_callees,
                                     exact_channel_values, exact_source_namespaces,
                                     exact_native_module_namespaces, &exact_dynamic_types, error,
                                     error_size)) {
        xr_free(exact_direct_callees);
        xr_free(direct_callee_targets);
        xr_free(exact_go_callees);
        xr_free(go_callee_targets);
        xr_free(exact_channel_values);
        xr_free(exact_channel_receives);
        xr_free(exact_source_namespaces);
        xr_free(exact_native_module_namespaces);
        return false;
    }
    bool verified = verify_layouts(plan, exact_dynamic_types, error, error_size) &&
                    verify_extent_references(plan, error, error_size) &&
                    verify_storage_and_allocations(plan, error, error_size) &&
                    verify_functions_and_slots(plan, error, error_size) &&
                    xr_i64_overflow_target_program_verify(plan, error, error_size) &&
                    xr_target_instruction_program_verify(plan, error, error_size) &&
                    verify_calls(plan, error, error_size) &&
                    verify_leaf_program_target(plan, error, error_size) &&
                    verify_product_program_target(plan, error, error_size) &&
                    verify_roots_and_cleanups(plan, error, error_size) &&
                    verify_adapters_and_capabilities(plan, error, error_size) &&
                    verify_coroutines(plan, error, error_size) &&
                    verify_entry_expectations(plan, error, error_size) &&
                    verify_debug_facts(plan, error, error_size);
    xr_free(exact_dynamic_types);
    xr_free(exact_direct_callees);
    xr_free(direct_callee_targets);
    xr_free(exact_go_callees);
    xr_free(go_callee_targets);
    xr_free(exact_channel_values);
    xr_free(exact_channel_receives);
    xr_free(exact_source_namespaces);
    xr_free(exact_native_module_namespaces);
    if (!verified)
        return false;
    XrFingerprint actual;
    xr_target_plan_compute_fingerprint(plan, &actual);
    if (!xr_fingerprint_equal(actual, plan->fingerprint))
        return report(error, error_size, "XR_TARGET_1000",
                      "TargetPlan fingerprint changed after freeze");
    return true;
}
