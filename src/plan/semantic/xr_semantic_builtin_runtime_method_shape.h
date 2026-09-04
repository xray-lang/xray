/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_builtin_runtime_method_shape.h - Typed runtime-method authority
 */

#ifndef XR_SEMANTIC_BUILTIN_RUNTIME_METHOD_SHAPE_H
#define XR_SEMANTIC_BUILTIN_RUNTIME_METHOD_SHAPE_H

#include "xr_semantic_plan.h"
#include "xr_semantic_string_shape.h"
#include "../../frontend/analyzer/xbuiltin_receiver_registry.h"
#include "../../ir/xi_builtin_map_entry_iterator_shape.h"
#include "../../ir/xi_ops_gen.h"
#include <stdio.h>
#include <string.h>

enum {
    XR_SEM_BUILTIN_RUNTIME_METHOD_EVIDENCE_REGISTRY_ID = 7
};

static inline const XaBuiltinReceiverMethodSpec *
xr_semantic_builtin_runtime_method_spec(const XrSemanticOperationRecord *operation) {
    if (!operation || operation->evidence[XR_SEM_BUILTIN_RUNTIME_METHOD_EVIDENCE_REGISTRY_ID] >=
                          XA_BUILTIN_RECEIVER_METHOD_COUNT)
        return NULL;
    const XaBuiltinReceiverMethodSpec *spec = xa_builtin_receiver_method_by_id(
        (XaBuiltinReceiverMethodId)
            operation->evidence[XR_SEM_BUILTIN_RUNTIME_METHOD_EVIDENCE_REGISTRY_ID]);
    return xa_builtin_runtime_receiver_method_spec_is_valid(spec) ? spec : NULL;
}

static inline bool
xr_semantic_builtin_runtime_method_type_is_exact(const XrSemanticPlan *plan,
                                                 const XrSemanticTypeRecord *type,
                                                 XaBuiltinMethodTypeKind expected) {
    if (!plan || !type)
        return false;
    if (expected == XA_BUILTIN_TYPE_STRING)
        return xr_semantic_tagged_string_type_is_exact(type);
    if (expected == XA_BUILTIN_TYPE_ARRAY_OF_STRING) {
        uint32_t child_count = 0;
        const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
        const XrSemanticTypeRecord *element =
            children && type->child_count == 1 && type->child_begin < child_count
                ? xr_semantic_plan_type(plan, children[type->child_begin])
                : NULL;
        uint8_t forbidden = XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE | XR_SEM_TYPE_BORROW_VIEW |
                            XR_SEM_TYPE_AGGREGATE_EXACT;
        uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
        return type->kind == XR_KIND_ARRAY && type->builtin_type == XR_TID_NULL &&
               type->source_class == XR_SEMANTIC_INDEX_NONE && type->child_count == 1 &&
               type->aggregate_extent == 0 && type->aggregate_align == 0 &&
               type->scalar_rep == XR_SCALAR_REP_NONE && (type->flags & forbidden) == 0 &&
               (type->flags & required) == required &&
               xr_semantic_tagged_string_type_is_exact(element);
    }
    return false;
}

static inline bool xr_semantic_builtin_runtime_method_receiver_type_is_exact(
    const XrSemanticPlan *plan, const XrSemanticTypeRecord *type, XaBuiltinReceiverKind receiver) {
    switch (receiver) {
        case XA_BUILTIN_RECEIVER_STRING:
            return xr_semantic_builtin_runtime_method_type_is_exact(plan, type,
                                                                    XA_BUILTIN_TYPE_STRING);
        default:
            return false;
    }
}

static inline bool xr_semantic_builtin_runtime_method_operand_type(
    const XaBuiltinReceiverMethodSpec *spec, uint16_t operand, XaBuiltinMethodTypeKind *out_type) {
    if (!xa_builtin_runtime_receiver_method_spec_is_valid(spec) || !out_type ||
        operand > (uint16_t) spec->param_count)
        return false;
    if (operand == 0) {
        switch (spec->receiver) {
            case XA_BUILTIN_RECEIVER_STRING:
                *out_type = XA_BUILTIN_TYPE_STRING;
                return true;
            default:
                return false;
        }
    }
    *out_type = spec->params[operand - 1u];
    return true;
}

static inline const XaBuiltinReceiverMethodSpec *
xr_semantic_builtin_runtime_method_live_spec(const XiValue *value) {
    if (!value || value->op != XI_CALL_METHOD || value->aux_kind != XI_AUX_KIND_NONE ||
        value->nargs < 1 || !value->args || !value->args[0])
        return NULL;
    XiMethodSymbolId symbol = xi_call_method_symbol_id(value);
    const XaBuiltinReceiverMethodSpec *spec = xa_builtin_runtime_receiver_method_by_symbol(
        value->args[0]->type, symbol, (uint16_t) (value->nargs - 1u));
    if (!spec || !value->aux || strcmp((const char *) value->aux, spec->source_name) != 0)
        return NULL;
    return spec;
}

static inline bool xr_builtin_runtime_method_identity(const XaBuiltinReceiverMethodSpec *spec,
                                                      XrStableId *identity) {
    if (!xa_builtin_runtime_receiver_method_spec_is_valid(spec) || !identity)
        return false;
    char key[384];
    int length =
        snprintf(key, sizeof(key),
                 "xray-builtin-runtime-method-v1:id=%u;receiver=%u;symbol=%u;arity=%d;"
                 "result=%u;p0=%u;p1=%u;p2=%u;effect=%u;allocation=%u",
                 (unsigned) spec->method_id, (unsigned) spec->receiver,
                 (unsigned) spec->method_symbol, spec->param_count, (unsigned) spec->result,
                 (unsigned) spec->params[0], (unsigned) spec->params[1], (unsigned) spec->params[2],
                 (unsigned) spec->effect, (unsigned) spec->allocation);
    XrFingerprint digest;
    return length > 0 && (size_t) length < sizeof(key) &&
           xr_stable_id_from_key(key, identity, &digest);
}

static inline bool xr_semantic_builtin_runtime_method_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const XaBuiltinReceiverMethodSpec **out_spec, uint32_t *out_receiver) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const XaBuiltinReceiverMethodSpec *spec = xr_semantic_builtin_runtime_method_spec(operation);
    XrStableId zero = {{0}};
    if (!plan || !operation || !spec || !operands || !metadata ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_BUILTIN_RUNTIME_METHOD ||
        operation->opcode != XI_CALL_METHOD ||
        operation->semantic_immediate != ((int64_t) spec->method_symbol << 1) ||
        operation->operand_count != (uint16_t) (spec->param_count + 1) ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        !metadata[operation->metadata_begin] ||
        strcmp(metadata[operation->metadata_begin], spec->source_name) != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        (operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) &&
         operation->flags != (xi_generated_op_default_flags(XI_CALL_METHOD) | XI_FLAG_TAIL)) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_complete != 1 || operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_NONE || operation->view_capability != 0 ||
        operation->view_lifetime != 0 || operation->view_complete != 0 ||
        operation->reserved_view[0] != 0 || operation->reserved_view[1] != 0 ||
        operation->array_element_storage != 0 || operation->array_hof_kind != 0 ||
        operation->array_result_element_storage != 0 || operation->allocation_key != NULL ||
        !xr_stable_id_equal(operation->allocation_id, zero))
        return false;

    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(plan, receiver->type);
    const XrSemanticTypeRecord *result_type = xr_semantic_plan_type(plan, operation->result_type);
    if (!xr_semantic_builtin_runtime_method_receiver_type_is_exact(plan, receiver_type,
                                                                   spec->receiver) ||
        !xr_semantic_builtin_runtime_method_type_is_exact(plan, result_type, spec->result) ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW || receiver->parameter_mode != 0 ||
        receiver->access != 0 || receiver->origin != 0 || receiver->lifetime != 0 ||
        receiver->escape != 0 || receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return false;
    for (int i = 0; i < spec->param_count; i++) {
        const XrSemanticOperandRecord *argument = receiver + i + 1;
        const XrSemanticTypeRecord *argument_type = xr_semantic_plan_type(plan, argument->type);
        if (!xr_semantic_builtin_runtime_method_type_is_exact(plan, argument_type,
                                                              spec->params[i]) ||
            argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != i ||
            argument->transfer_mode != XR_TRANSFER_SHARE ||
            argument->ownership_action != XR_SEM_OPERAND_CONSUME || argument->parameter_mode != 0 ||
            argument->access != 0 || argument->origin != 0 || argument->lifetime != 0 ||
            argument->escape != 0 || argument->flags != XR_SEM_OPERAND_CALL_CONTRACT)
            return false;
    }
    if (out_spec)
        *out_spec = spec;
    if (out_receiver)
        *out_receiver = receiver->value;
    return true;
}

#endif /* XR_SEMANTIC_BUILTIN_RUNTIME_METHOD_SHAPE_H */
