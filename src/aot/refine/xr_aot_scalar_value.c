/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_aot_scalar_value.c - Xi to scalar TargetPlan identity bridge
 */

#include "xr_aot_scalar_value.h"
#include "../../plan/semantic/xr_semantic_enum_shape.h"
#include "../../plan/semantic/xr_semantic_string_shape.h"
#include "../../plan/semantic/xr_semantic_string_slice_shape.h"
#include "../../ir/xi_own.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>

static bool fail(char *error, size_t error_size, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_TARGET_1001: %s", detail);
    return false;
}

static bool block_belongs_to_function(const XiFunc *function, const XiBlock *block) {
    if (!function || !block || block->func != function)
        return false;
    for (uint32_t i = 0; i < function->nblocks; i++) {
        if (function->blocks[i] == block)
            return true;
    }
    return false;
}

static bool value_belongs_to_block(const XiBlock *block, const XiValue *value) {
    for (uint32_t i = 0; i < block->nvalues; i++) {
        if (block->values[i] == value)
            return true;
    }
    for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
        if (&phi->value == value)
            return true;
    }
    return false;
}

static bool parameter_identity_is_exact(const XiFunc *function, const XiValue *value) {
    if (value->op != XI_PARAM)
        return true;
    if (!function->params || value->aux_int < 0 || value->aux_int >= function->nparams)
        return false;
    uint16_t parameter = (uint16_t) value->aux_int;
    return function->params[parameter] == value;
}

/* Independently reconstruct the builtin declaration identity used by the
 * SemanticPlan type producer. A matching spelling with a source class_ref is
 * deliberately not a builtin and cannot inherit its frozen storage flags. */
static uint32_t live_frozen_builtin_type(const XrType *type) {
    if (xr_type_is_builtin_named_class(type, "StringBuilder"))
        return XR_TID_STRINGBUILDER;
    if (xr_type_is_builtin_named_class(type, "Task"))
        return XR_TID_COROUTINE;
    if (xr_type_is_builtin_named_class(type, "WorkQueue"))
        return XR_TID_WORKQUEUE;
    if (xr_type_is_builtin_named_class(type, "ResultGroup"))
        return XR_TID_RESULTGROUP;
    if (xr_type_is_builtin_named_class(type, "CountdownLatch"))
        return XR_TID_COUNTDOWNLATCH;
    if (xr_type_is_builtin_named_class(type, "Semaphore"))
        return XR_TID_SEMAPHORE;
    if (xr_type_is_builtin_named_class(type, "EventCount"))
        return XR_TID_EVENTCOUNT;
    return XR_TID_NULL;
}

static bool semantic_type_is_exact(const XrSemanticTypeRecord *semantic,
                                   const XrType *type) {
    if (!semantic || !type || semantic->kind != (uint32_t) type->kind ||
        semantic->scalar_rep != type->scalar_rep)
        return false;
    uint32_t builtin_type = live_frozen_builtin_type(type);
    bool reference_capable =
        builtin_type != XR_TID_NULL || xi_own_type_is_rc(type);
    bool borrow_view = type->kind == XR_KIND_SLICE;
    uint8_t flags =
        (uint8_t) ((type->is_nullable ? XR_SEM_TYPE_NULLABLE : 0u) |
                   (type->is_const ? XR_SEM_TYPE_CONST : 0u) |
                   (type->is_value_type ? XR_SEM_TYPE_VALUE : 0u) |
                   (type->is_literal ? XR_SEM_TYPE_LITERAL : 0u) |
                   (reference_capable ? XR_SEM_TYPE_REFERENCE_CAPABLE : 0u) |
                   (borrow_view ? XR_SEM_TYPE_BORROW_VIEW : 0u) |
                   (reference_capable && !borrow_view
                        ? XR_SEM_TYPE_OWNERSHIP_ROOT
                        : 0u));
    /* AGGREGATE_EXACT is a frozen SemanticPlan/TargetPlan conclusion, not a
     * mutable XrType bit. The live value only has to retain the value-instance
     * category from which that conclusion was built; the exact fields and
     * layout are verified in the immutable plans. */
    if ((semantic->flags & XR_SEM_TYPE_AGGREGATE_EXACT) != 0) {
        if (type->kind != XR_KIND_INSTANCE || !type->is_value_type ||
            semantic->child_count == 0 ||
            semantic->aggregate_extent != semantic->child_count)
            return false;
        flags |= XR_SEM_TYPE_AGGREGATE_EXACT;
    }
    return semantic->builtin_type == builtin_type && semantic->flags == flags;
}

static bool semantic_value_shape_is_exact(const XrSemanticPlan *plan,
                                          uint32_t function_index,
                                          uint32_t semantic_value,
                                          const XiValue *value) {
    for (uint32_t parameter_index = 0;
         parameter_index < xr_semantic_plan_parameter_count(plan);
         parameter_index++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan, parameter_index);
        if (!parameter || parameter->function != function_index ||
            parameter->value != semantic_value)
            continue;
        return value->op == XI_PARAM &&
               semantic_type_is_exact(
                   xr_semantic_plan_type(plan, parameter->type), value->type);
    }
    for (uint32_t operation_index = 0;
         operation_index < xr_semantic_plan_operation_count(plan);
         operation_index++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan, operation_index);
        if (!operation || operation->function != function_index ||
            operation->result_value != semantic_value)
            continue;
        return operation->opcode == value->op &&
               semantic_type_is_exact(
                   xr_semantic_plan_type(plan, operation->result_type),
                   value->type);
    }
    return false;
}

bool xr_aot_scalar_semantic_value_id(const XrTargetPlan *target_plan, const XiFunc *function,
                                     const XiValue *value, uint32_t *out_semantic_function,
                                     uint32_t *out_semantic_value, char *error,
                                     size_t error_size) {
    if (out_semantic_function)
        *out_semantic_function = XR_SEMANTIC_INDEX_NONE;
    if (out_semantic_value)
        *out_semantic_value = XR_SEMANTIC_INDEX_NONE;
    if (!target_plan || !function || !value || !out_semantic_function || !out_semantic_value)
        return fail(error, error_size, "scalar semantic identity input is missing");
    if (!xr_target_plan_is_verified(target_plan))
        return fail(error, error_size,
                    "scalar semantic identity requires a verified TargetPlan");
    const XrSemanticPlan *semantic_plan = xr_target_plan_semantic_plan(target_plan);
    if (!semantic_plan || function->semantic_plan != semantic_plan ||
        function->semantic_plan_function_index == XR_SEMANTIC_INDEX_NONE)
        return fail(error, error_size,
                    "Xi function does not carry the TargetPlan semantic authority");
    if (!block_belongs_to_function(function, value->block) ||
        !value_belongs_to_block(value->block, value) ||
        !parameter_identity_is_exact(function, value))
        return fail(error, error_size,
                    "Xi value is not an exact member of the semantic function");
    uint32_t function_index = function->semantic_plan_function_index;
    const XrSemanticFunctionRecord *semantic_function =
        xr_semantic_plan_function(semantic_plan, function_index);
    if (!semantic_function || value->id >= semantic_function->value_count ||
        semantic_function->value_begin > UINT32_MAX - value->id)
        return fail(error, error_size, "Xi scalar value identity is out of range");
    uint32_t semantic_value = semantic_function->value_begin + value->id;
    if (!semantic_value_shape_is_exact(semantic_plan, function_index,
                                       semantic_value, value))
        return fail(error, error_size,
                    "Xi value opcode or type drifted from the SemanticPlan snapshot");
    *out_semantic_function = function_index;
    *out_semantic_value = semantic_value;
    return true;
}

static bool adapter_origin_matches(const XiValue *value) {
    if (!value)
        return false;
    switch ((XiBackendValueOrigin) value->backend_origin) {
        case XI_BACKEND_VALUE_REP_BOX: return value->op == XI_BOX;
        case XI_BACKEND_VALUE_REP_UNBOX: return value->op == XI_UNBOX;
        case XI_BACKEND_VALUE_ENUM_DESCRIPTOR_BOX:
            return value->op == XI_ENUM_DESCRIPTOR_BOX;
        case XI_BACKEND_VALUE_ENUM_DESCRIPTOR_UNBOX:
            return value->op == XI_ENUM_DESCRIPTOR_UNBOX;
        case XI_BACKEND_VALUE_NONE:
        case XI_BACKEND_VALUE_ORIGIN_COUNT: return false;
    }
    return false;
}

static bool adapter_target_rep_is_exact(const XrTargetPlan *target_plan,
                                        const XrTargetValueRepRecord *binding,
                                        const XiValue *value,
                                        bool source_supports_pointer_unbox) {
    if (!target_plan || !value)
        return false;
    if (value->op == XI_BOX || value->op == XI_ENUM_DESCRIPTOR_BOX)
        return value->rep == XR_REP_TAGGED;
    if (value->op == XI_ENUM_DESCRIPTOR_UNBOX)
        return value->rep == XR_REP_I64;
    if (!binding) {
        if (!value->type || value->type->is_nullable)
            return false;
        switch (value->type->kind) {
            case XR_KIND_STRING:
            case XR_KIND_SLICE:
                return value->rep == XR_REP_PTR;
            default: return false;
        }
    }
    const XrTargetMachineRepRecord *machine =
        xr_target_plan_machine_rep(target_plan, binding->memory_rep);
    if (!machine)
        return false;
    if (value->op == XI_UNBOX && source_supports_pointer_unbox &&
        machine->kind == XR_MACHINE_REP_DYN_VALUE)
        return value->rep == XR_REP_PTR;
    switch ((XrMachineRepKind) machine->kind) {
        case XR_MACHINE_REP_VOID: return value->rep == XR_REP_VOID;
        case XR_MACHINE_REP_F32:
        case XR_MACHINE_REP_F64: return value->rep == XR_REP_F64;
        case XR_MACHINE_REP_I1:
        case XR_MACHINE_REP_I8:
        case XR_MACHINE_REP_U8:
        case XR_MACHINE_REP_I16:
        case XR_MACHINE_REP_U16:
        case XR_MACHINE_REP_I32:
        case XR_MACHINE_REP_U32:
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_ENUM_ORDINAL:
        case XR_MACHINE_REP_U64:
        case XR_MACHINE_REP_ISIZE:
        case XR_MACHINE_REP_USIZE:
        case XR_MACHINE_REP_RUNE: return value->rep == XR_REP_I64;
        case XR_MACHINE_REP_RAW_PTR: return value->rep == XR_REP_RAWPTR;
        case XR_MACHINE_REP_VIEW: return value->rep == XR_REP_PTR;
        default: return false;
    }
}

static const XrSemanticTypeRecord *semantic_value_type(
    const XrSemanticPlan *plan, uint32_t semantic_value) {
    const XrSemanticTypeRecord *match = NULL;
    uint32_t operation_count =
        (uint32_t) xr_semantic_plan_operation_count(plan);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(plan, i);
        if (!operation || operation->result_value != semantic_value)
            continue;
        const XrSemanticTypeRecord *candidate =
            xr_semantic_plan_type(plan, operation->result_type);
        if (!candidate || (match && match != candidate))
            return NULL;
        match = candidate;
    }
    uint32_t parameter_count =
        (uint32_t) xr_semantic_plan_parameter_count(plan);
    for (uint32_t i = 0; i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter =
            xr_semantic_plan_parameter(plan, i);
        if (!parameter || parameter->value != semantic_value)
            continue;
        const XrSemanticTypeRecord *candidate =
            xr_semantic_plan_type(plan, parameter->type);
        if (!candidate || (match && match != candidate))
            return NULL;
        match = candidate;
    }
    return match;
}

static bool adapter_source_rep_is_exact(
    const XrTargetPlan *target_plan,
    const XrTargetValueRepRecord *binding, const XiValue *value,
    bool exact_string_slice_result) {
    const XiValue *source = value && value->nargs == 1 && value->args
                                ? value->args[0]
                                : NULL;
    if (!target_plan || !source || !value)
        return false;
    if (value->op == XI_UNBOX ||
        value->op == XI_ENUM_DESCRIPTOR_UNBOX)
        return source->rep == XR_REP_TAGGED;
    if (!binding)
        return source->rep == XR_REP_PTR;
    const XrTargetMachineRepRecord *machine =
        xr_target_plan_machine_rep(target_plan, binding->memory_rep);
    if (!machine)
        return false;
    if (value->op == XI_BOX && exact_string_slice_result &&
        machine->kind == XR_MACHINE_REP_DYN_VALUE)
        return source->rep == XR_REP_PTR;
    switch ((XrMachineRepKind) machine->kind) {
        case XR_MACHINE_REP_F32:
        case XR_MACHINE_REP_F64: return source->rep == XR_REP_F64;
        case XR_MACHINE_REP_I1:
        case XR_MACHINE_REP_I8:
        case XR_MACHINE_REP_U8:
        case XR_MACHINE_REP_I16:
        case XR_MACHINE_REP_U16:
        case XR_MACHINE_REP_I32:
        case XR_MACHINE_REP_U32:
        case XR_MACHINE_REP_I64:
        case XR_MACHINE_REP_ENUM_ORDINAL:
        case XR_MACHINE_REP_U64:
        case XR_MACHINE_REP_ISIZE:
        case XR_MACHINE_REP_USIZE:
        case XR_MACHINE_REP_RUNE: return source->rep == XR_REP_I64;
        case XR_MACHINE_REP_RAW_PTR: return source->rep == XR_REP_RAWPTR;
        case XR_MACHINE_REP_VIEW: return source->rep == XR_REP_PTR;
        default: return false;
    }
}

XR_FUNC bool xr_aot_rep_adapter_value_is_exact(
    const XrTargetPlan *target_plan, const XiFunc *function,
    const XiValue *value, char *error, size_t error_size) {
    if (!target_plan || !function || !value || !adapter_origin_matches(value) ||
        value->nargs != 1 || !value->args || !value->args[0] ||
        value->args[0]->backend_origin != XI_BACKEND_VALUE_NONE ||
        value->args[0]->block == NULL ||
        value->args[0]->block->func != function ||
        value->block != value->args[0]->block ||
        value->id >= function->next_value_id ||
        value->type != value->args[0]->type ||
        !block_belongs_to_function(function, value->block) ||
        !value_belongs_to_block(value->block, value))
        return fail(error, error_size,
                    "backend representation adapter provenance is invalid");

    const XrSemanticPlan *semantic_plan =
        xr_target_plan_semantic_plan(target_plan);
    const XrSemanticFunctionRecord *semantic_function =
        semantic_plan &&
                function->semantic_plan_function_index != XR_SEMANTIC_INDEX_NONE
            ? xr_semantic_plan_function(
                  semantic_plan, function->semantic_plan_function_index)
            : NULL;
    if (!semantic_function || value->id < semantic_function->value_count)
        return fail(error, error_size,
                    "backend representation adapter overlaps the semantic snapshot");

    uint32_t source_function = XR_SEMANTIC_INDEX_NONE;
    uint32_t source_value = XR_SEMANTIC_INDEX_NONE;
    if (!xr_aot_scalar_semantic_value_id(
            target_plan, function, value->args[0], &source_function,
            &source_value, error, error_size))
        return false;
    const XrTargetValueRepRecord *binding =
        xr_target_plan_value_rep(target_plan, source_value);
    if (binding) {
        const XrTargetMachineRepRecord *register_rep =
            xr_target_plan_machine_rep(target_plan, binding->register_rep);
        const XrTargetMachineRepRecord *memory_rep =
            xr_target_plan_machine_rep(target_plan, binding->memory_rep);
        if (!register_rep || !memory_rep ||
            register_rep->kind == XR_MACHINE_REP_VOID ||
            memory_rep->kind == XR_MACHINE_REP_VOID)
            return fail(error, error_size,
                        "void values cannot source representation adapters");
    }
    if (!binding) {
        const XrType *source_type = value->args[0]->type;
        if (!source_type || source_type->is_nullable ||
            (source_type->kind != XR_KIND_STRING &&
             source_type->kind != XR_KIND_SLICE))
            return fail(error, error_size,
                        "backend representation adapter source has no exact authority family");
    }
    bool exact_string_slice_result = false;
    uint32_t semantic_operation_count =
        (uint32_t) xr_semantic_plan_operation_count(semantic_plan);
    for (uint32_t i = 0; i < semantic_operation_count; i++) {
        const XrSemanticOperationRecord *operation =
            xr_semantic_plan_operation(semantic_plan, i);
        if (!operation || operation->result_value != source_value)
            continue;
        if (!xr_semantic_string_slice_range_is_exact(
                semantic_plan, operation, NULL, NULL, NULL))
            continue;
        if (exact_string_slice_result)
            return fail(error, error_size,
                        "backend String.slice result identity is ambiguous");
        exact_string_slice_result = true;
    }
    if (!adapter_source_rep_is_exact(target_plan, binding, value,
                                     exact_string_slice_result))
        return fail(error, error_size,
                    "backend representation adapter source rep is inconsistent");
    if ((value->op == XI_ENUM_DESCRIPTOR_BOX ||
         value->op == XI_ENUM_DESCRIPTOR_UNBOX) &&
        !xr_type_is_enum_metadata(value->args[0]->type))
        return fail(error, error_size,
                    "backend enum adapter source lacks enum metadata authority");
    const XrSemanticTypeRecord *source_type =
        semantic_value_type(semantic_plan, source_value);
    bool source_supports_pointer_unbox =
        xr_semantic_adt_enum_type_is_exact(source_type) ||
        xr_semantic_owned_string_type_is_exact(source_type);
    if (!adapter_target_rep_is_exact(target_plan, binding, value,
                                     source_supports_pointer_unbox))
        return fail(error, error_size,
                    "backend representation adapter output rep is inconsistent");
    return true;
}
