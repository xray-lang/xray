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
#include "xr_semantic_array_type_shape.h"
#include "xr_semantic_channel_type_shape.h"
#include "xr_semantic_task_shape.h"
#include "xr_semantic_enum_shape.h"
#include "xr_semantic_type_admission_shape.h"
#include "xr_semantic_range_shape.h"
#include "xr_semantic_string_shape.h"
#include "../../frontend/analyzer/xbuiltin_receiver_registry.h"
#include "../../ir/xi_builtin_map_entry_iterator_shape.h"
#include "../../ir/xi_ops_gen.h"
#include <stdio.h>
#include <string.h>

enum {
    XR_SEM_BUILTIN_RUNTIME_METHOD_EVIDENCE_REGISTRY_ID = 7
};

typedef enum XrSemanticBuiltinRuntimeMethodResultClass {
    XR_SEM_BUILTIN_RUNTIME_METHOD_RESULT_INVALID = 0,
    XR_SEM_BUILTIN_RUNTIME_METHOD_RESULT_SCALAR,
    XR_SEM_BUILTIN_RUNTIME_METHOD_RESULT_OWNED_DYNAMIC,
} XrSemanticBuiltinRuntimeMethodResultClass;

/* The registry and instantiated result type jointly determine the carrier:
 * an optional scalar has no heap owner, while an optional reference transfers
 * its element owner. Selectors and modules grant no representation authority. */
static inline XrSemanticBuiltinRuntimeMethodResultClass
xr_semantic_builtin_runtime_method_result_class(const XaBuiltinReceiverMethodSpec *spec,
                                                const XrSemanticTypeRecord *result) {
    if (!xa_builtin_runtime_receiver_method_spec_is_valid(spec))
        return XR_SEM_BUILTIN_RUNTIME_METHOD_RESULT_INVALID;
    switch (spec->result) {
        case XA_BUILTIN_TYPE_UNIT:
        case XA_BUILTIN_TYPE_BOOL:
        case XA_BUILTIN_TYPE_INT:
        case XA_BUILTIN_TYPE_SEND_RESULT:
            return XR_SEM_BUILTIN_RUNTIME_METHOD_RESULT_SCALAR;
        case XA_BUILTIN_TYPE_TASK_RESULT_OF_RECEIVER_ELEM:
        case XA_BUILTIN_TYPE_RECV_OF_RECEIVER_ELEM:
        case XA_BUILTIN_TYPE_STRING:
        case XA_BUILTIN_TYPE_ARRAY_OF_STRING:
        case XA_BUILTIN_TYPE_U8_ARRAY:
            return XR_SEM_BUILTIN_RUNTIME_METHOD_RESULT_OWNED_DYNAMIC;
        case XA_BUILTIN_TYPE_RECEIVER_ELEM_NULLABLE:
            if (!result)
                return XR_SEM_BUILTIN_RUNTIME_METHOD_RESULT_INVALID;
            return (result->flags & XR_SEM_TYPE_OWNERSHIP_ROOT) != 0
                ? XR_SEM_BUILTIN_RUNTIME_METHOD_RESULT_OWNED_DYNAMIC
                : XR_SEM_BUILTIN_RUNTIME_METHOD_RESULT_SCALAR;
        default:
            return XR_SEM_BUILTIN_RUNTIME_METHOD_RESULT_INVALID;
    }
}

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
    if (expected == XA_BUILTIN_TYPE_UNIT) {
        char key[96];
        snprintf(key, sizeof(key), "type-v3:%u:0:%u:0:0:0:0:0:0:%u:0:",
                 (unsigned)XR_KIND_UNIT, (unsigned)XR_TID_NULL, (unsigned)XR_SCALAR_REP_NONE);
        XrStableId zero = {{0}};
        return type->kind == XR_KIND_UNIT && type->builtin_type == XR_TID_NULL &&
               type->flags == 0 && type->child_count == 0 &&
               type->source_class == XR_SEMANTIC_INDEX_NONE &&
               xr_stable_id_equal(type->source_class_identity, zero) &&
               xr_stable_id_equal(type->source_enum_identity, zero) && !type->source_enum_key &&
               type->enum_layout_id == 0 && type->enum_member_count == 0 &&
               type->enum_flags == 0 && type->reserved_enum == 0 &&
               type->aggregate_extent == 0 && type->aggregate_align == 0 &&
               type->scalar_rep == XR_SCALAR_REP_NONE && type->canonical_key &&
               strcmp(type->canonical_key, key) == 0;
    }
    if (expected == XA_BUILTIN_TYPE_SEND_RESULT)
        return xr_semantic_builtin_enum_declaration_is_exact(type, XR_GLOBAL_VAR_SEND_RESULT);
    if (expected == XA_BUILTIN_TYPE_BOOL) {
        XrStableId zero = {{0}};
        const char expected_key[] = "type-v3:3:0:0:0:0:0:0:0:0:255:0:";
        return type->kind == XR_KIND_BOOL && type->builtin_type == XR_TID_NULL &&
               type->source_class == XR_SEMANTIC_INDEX_NONE &&
               xr_stable_id_equal(type->source_class_identity, zero) &&
               xr_stable_id_equal(type->source_enum_identity, zero) && !type->source_enum_key &&
               type->child_count == 0 && type->aggregate_extent == 0 &&
               type->aggregate_align == 0 && type->enum_layout_id == 0 &&
               type->enum_member_count == 0 && type->enum_flags == 0 &&
               type->reserved_enum == 0 && type->scalar_rep == XR_SCALAR_REP_NONE &&
               type->flags == 0 && type->canonical_key &&
               strcmp(type->canonical_key, expected_key) == 0;
    }
    if (expected == XA_BUILTIN_TYPE_INT)
        return xr_semantic_range_bound_type_is_exact(type);
    if (expected == XA_BUILTIN_TYPE_STRING)
        return xr_semantic_tagged_string_type_is_exact(type);
    if (expected == XA_BUILTIN_TYPE_U8_ARRAY) {
        uint32_t count = 0;
        const uint32_t *children = xr_semantic_plan_type_children(plan, &count);
        if (!xr_semantic_array_type_row_is_exact(type) ||
            type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
            !children || type->child_begin >= count)
            return false;
        const XrSemanticTypeRecord *element =
            xr_semantic_plan_type(plan, children[type->child_begin]);
        XrStableId zero = {{0}};
        char key[96];
        snprintf(key, sizeof(key), "type-v3:0:0:0:0:0:0:0:0:0:%u:0:",
                 (unsigned) XR_NATIVE_U8);
        const char *suffix = strstr(type->canonical_key, ";element:");
        return element && element->kind == XR_KIND_INT && element->scalar_rep == XR_NATIVE_U8 &&
               element->builtin_type == XR_TID_NULL && element->flags == 0 &&
               element->child_count == 0 && element->aggregate_extent == 0 &&
               element->aggregate_align == 0 && element->source_class == XR_SEMANTIC_INDEX_NONE &&
               xr_stable_id_equal(element->source_class_identity, zero) &&
               xr_stable_id_equal(element->source_enum_identity, zero) && !element->source_enum_key &&
               element->canonical_key && strcmp(element->canonical_key, key) == 0 &&
               suffix && strcmp(suffix + strlen(";element:"), key) == 0;
    }
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
        case XA_BUILTIN_RECEIVER_RANGE:
            return xr_semantic_range_type_is_exact(type);
        case XA_BUILTIN_RECEIVER_TASK:
            return xr_semantic_task_type_row_is_exact(plan, type);
        case XA_BUILTIN_RECEIVER_CHANNEL:
            return xr_semantic_channel_type_row_is_exact(plan, type);
        case XA_BUILTIN_RECEIVER_ARRAY:
            return xr_semantic_array_type_row_is_exact(type) &&
                   type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT);
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
            case XA_BUILTIN_RECEIVER_ARRAY:
            case XA_BUILTIN_RECEIVER_RANGE:
            case XA_BUILTIN_RECEIVER_CHANNEL:
            case XA_BUILTIN_RECEIVER_TASK:
                /* A nominal runtime receiver is carried as the tagged value
                 * named by the registry row, not as its result component. */
                *out_type = XA_BUILTIN_TYPE_RECEIVER;
                return true;
            default:
                return false;
        }
    }
    *out_type = spec->params[operand - 1u];
    return true;
}

/* Generic result identity does not grant a concrete machine representation.
 * Preserve the complete parameter key, including its ordinal and name. */
static inline bool xr_semantic_runtime_method_nullable_parameter_is_exact(
    const XrSemanticTypeRecord *element, const XrSemanticTypeRecord *result) {
    if (!element || !result || element->kind != XR_KIND_TYPE_PARAM ||
        result->kind != XR_KIND_TYPE_PARAM || !element->canonical_key ||
        !result->canonical_key || element->flags != 0 ||
        result->flags != XR_SEM_TYPE_NULLABLE || element->child_count != 0 ||
        result->child_count != 0 || element->scalar_rep != XR_SCALAR_REP_NONE ||
        result->scalar_rep != XR_SCALAR_REP_NONE)
        return false;
    size_t element_head = 0, result_head = 0;
    unsigned element_nullable = 0, result_nullable = 0;
    const char *element_tail = xr_semantic_type_key_split_nullable(
        element->canonical_key, &element_head, &element_nullable);
    const char *result_tail = xr_semantic_type_key_split_nullable(
        result->canonical_key, &result_head, &result_nullable);
    return element_tail && result_tail && element_nullable == 0 && result_nullable == 1 &&
           element_head == result_head && strstr(element_tail, ";param:") &&
           strncmp(element->canonical_key, result->canonical_key, element_head) == 0 &&
           strcmp(element_tail, result_tail) == 0;
}

static inline bool xr_semantic_builtin_runtime_method_result_type_is_exact(
    const XrSemanticPlan *plan, const XaBuiltinReceiverMethodSpec *spec,
    const XrSemanticTypeRecord *receiver, const XrSemanticTypeRecord *result) {
    if (spec->result == XA_BUILTIN_TYPE_RECV_OF_RECEIVER_ELEM ||
        spec->result == XA_BUILTIN_TYPE_TASK_RESULT_OF_RECEIVER_ELEM) {
        bool task = spec->result == XA_BUILTIN_TYPE_TASK_RESULT_OF_RECEIVER_ELEM;
        uint32_t count = 0;
        const uint32_t *children = xr_semantic_plan_type_children(plan, &count);
        return (task ? xr_semantic_task_type_row_is_exact(plan, receiver)
                     : xr_semantic_channel_type_row_is_exact(plan, receiver)) &&
            xr_semantic_builtin_enum_declaration_is_exact(
                result, task ? XR_GLOBAL_VAR_TASK_RESULT : XR_GLOBAL_VAR_RECV) &&
            children && receiver->child_count == 1 && result->child_count == 1 &&
            receiver->child_begin < count && result->child_begin < count &&
            children[receiver->child_begin] == children[result->child_begin];
    }
    if (spec->result != XA_BUILTIN_TYPE_RECEIVER_ELEM_NULLABLE)
        return xr_semantic_builtin_runtime_method_type_is_exact(plan, result, spec->result);
    uint32_t count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &count);
    if (!receiver || receiver->kind != XR_KIND_ARRAY || receiver->child_count != 1 ||
        !children || receiver->child_begin >= count || !result ||
        (result->flags & XR_SEM_TYPE_NULLABLE) == 0)
        return false;
    const XrSemanticTypeRecord *element =
        xr_semantic_plan_type(plan, children[receiver->child_begin]);
    const char *suffix = receiver->canonical_key ? strstr(receiver->canonical_key, ";element:") : NULL;
    return element && element->canonical_key && suffix &&
           strcmp(suffix + strlen(";element:"), element->canonical_key) == 0 &&
           (element == result || xr_semantic_type_is_nullable_widening(element, result) ||
            xr_semantic_type_is_nullable_scalar_widening(element, result) ||
            xr_semantic_runtime_method_nullable_parameter_is_exact(element, result));
}

static inline bool xr_semantic_builtin_runtime_method_result_contract_is_exact(
    const XaBuiltinReceiverMethodSpec *spec, const XrSemanticOperationRecord *operation,
    const XrSemanticTypeRecord *result) {
    XrSemanticBuiltinRuntimeMethodResultClass result_class =
        xr_semantic_builtin_runtime_method_result_class(spec, result);
    if (!operation || operation->return_parameter != -1)
        return false;
    if (spec->result == XA_BUILTIN_TYPE_SEND_RESULT)
        return operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
               operation->return_provenance == XR_SEM_RETURN_OWNED && operation->return_complete == 1;
    if (spec->result == XA_BUILTIN_TYPE_RECEIVER_ELEM_NULLABLE)
        return operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
               operation->return_provenance == XR_SEM_RETURN_OWNED && operation->return_complete == 1;
    if (result_class == XR_SEM_BUILTIN_RUNTIME_METHOD_RESULT_SCALAR)
        return operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT &&
               operation->return_provenance == XR_SEM_RETURN_NONE &&
               operation->return_complete == 0;
    if (result_class == XR_SEM_BUILTIN_RUNTIME_METHOD_RESULT_OWNED_DYNAMIC)
        return operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
               operation->return_provenance == XR_SEM_RETURN_OWNED &&
               operation->return_complete == 1;
    return false;
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
                 "xray-builtin-runtime-method-v2:id=%u;receiver=%u;symbol=%u;arity=%d;min=%d;"
                 "result=%u;p0=%u;p1=%u;p2=%u;effect=%u;allocation=%u",
                 (unsigned) spec->method_id, (unsigned) spec->receiver,
                 (unsigned) spec->method_symbol, spec->param_count, spec->min_params, (unsigned) spec->result,
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
        operation->operand_count < (uint16_t) (spec->min_params + 1) ||
        operation->operand_count > (uint16_t) (spec->param_count + 1) ||
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
        !xr_semantic_builtin_runtime_method_result_contract_is_exact(
            spec, operation, xr_semantic_plan_type(plan, operation->result_type)) ||
        (spec->result == XA_BUILTIN_TYPE_SEND_RESULT
             ? operation->transfer_mode > XR_TRANSFER_MOVE
             : operation->transfer_mode != XR_TRANSFER_SHARE) ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->result_alias_operand != -1 ||
        operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
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
        !xr_semantic_builtin_runtime_method_result_type_is_exact(plan, spec, receiver_type,
                                                                 result_type) ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW || receiver->parameter_mode != 0 ||
        receiver->access != 0 || receiver->origin != 0 || receiver->lifetime != 0 ||
        receiver->escape != 0 || receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return false;
    for (uint16_t i = 0; i + 1u < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = receiver + i + 1;
        const XrSemanticTypeRecord *argument_type = xr_semantic_plan_type(plan, argument->type);
        bool channel_payload = spec->receiver == XA_BUILTIN_RECEIVER_CHANNEL &&
                               spec->params[i] == XA_BUILTIN_TYPE_RECEIVER_ELEM;
        uint32_t child_count = 0;
        const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
        bool type_exact = channel_payload
            ? children && receiver_type->child_count == 1 &&
                  receiver_type->child_begin < child_count &&
                  argument->type == children[receiver_type->child_begin]
            : xr_semantic_builtin_runtime_method_type_is_exact(plan, argument_type, spec->params[i]);
        uint8_t expected_transfer = channel_payload ? operation->transfer_mode : XR_TRANSFER_SHARE;
        uint8_t expected_ownership = channel_payload && expected_transfer != XR_TRANSFER_MOVE
            ? XR_SEM_OPERAND_BORROW : XR_SEM_OPERAND_CONSUME;
        if (!type_exact ||
            argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != i ||
            argument->transfer_mode != expected_transfer ||
            argument->ownership_action != expected_ownership || argument->parameter_mode != 0 ||
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

static inline bool xr_semantic_builtin_runtime_method_has_result_class(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    XrSemanticBuiltinRuntimeMethodResultClass expected) {
    const XaBuiltinReceiverMethodSpec *spec = NULL;
    return expected != XR_SEM_BUILTIN_RUNTIME_METHOD_RESULT_INVALID &&
           xr_semantic_builtin_runtime_method_is_exact(plan, operation, &spec, NULL) &&
           xr_semantic_builtin_runtime_method_result_class(
               spec, xr_semantic_plan_type(plan, operation->result_type)) == expected;
}

#endif /* XR_SEMANTIC_BUILTIN_RUNTIME_METHOD_SHAPE_H */
