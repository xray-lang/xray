/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_program_semantic_verify.c - Independent PSC-to-Xi authority verifier
 */

#include "xi_program_semantic.h"
#include "xi_core_api.h"
#include "xi_effect.h"
#include "../frontend/analyzer/xanalyzer_symbol.h"
#include "../plan/target/xr_target_profile.h"
#include "../runtime/value/xtype.h"
#include "../runtime/class/xclass_info.h"
#include "../shared/xr_exact_scalar_registry.h"
#include <stdio.h>
#include <string.h>

typedef struct XiScalarVerifyState {
    const XiModule *module;
    const XrProgramSemanticClosure *closure;
    const XrProgramSemanticCallRecord *call_row;
    const XiFunc *caller;
    const XiFunc *callee;
    const XiValue *call;
    uint32_t indexed_call_count;
    uint32_t xi_call_count;
} XiScalarVerifyState;

static bool verify_fail(char *error, size_t error_size, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_SEM_0019: %s", detail);
    return false;
}

static bool verify_same_id(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static bool verify_span_empty(XiSourceSpan span) {
    return span.start_line == 0 && span.start_column == 0 && span.end_line == 0 &&
           span.end_column == 0;
}

static bool verify_locator_exact(XiSourceLocator xi, XrProgramSemanticSourceLocator psc) {
    return xi.kind != 0 && xi.kind == psc.kind && xi.span.start_line != 0 &&
           xi.span.start_line == psc.start_line && xi.span.start_column != 0 &&
           xi.span.start_column == psc.start_column && xi.span.end_line != 0 &&
           xi.span.end_line == psc.end_line && xi.span.end_column != 0 &&
           xi.span.end_column == psc.end_column &&
           (xi.span.end_line > xi.span.start_line ||
            (xi.span.end_line == xi.span.start_line && xi.span.end_column > xi.span.start_column));
}

static bool verify_i64(const XrType *type) {
    return type && type->kind == XR_KIND_INT && !type->is_nullable &&
           type->scalar_rep == XR_NATIVE_I64;
}

static bool verify_source_module_exact(const XiModule *module) {
    const XrProgramSemanticClosure *closure = module ? module->program_semantic_closure : NULL;
    const XrProgramSemanticModuleRecord *row =
        closure && xr_program_semantic_closure_module_count(closure) == 1
            ? xr_program_semantic_closure_module(closure, 0)
            : NULL;
    const XrProgramSemanticModuleInput *source =
        module && module->source_semantic_module_present ? &module->source_semantic_module : NULL;
    return row && source && verify_same_id(row->module_identity, source->module_identity) &&
           memcmp(row->module_authority_fingerprint.bytes,
                  source->module_authority_fingerprint.bytes,
                  sizeof(row->module_authority_fingerprint.bytes)) == 0 &&
           memcmp(row->source_fingerprint.bytes, source->source_fingerprint.bytes,
                  sizeof(row->source_fingerprint.bytes)) == 0 &&
           memcmp(row->export_fingerprint.bytes, source->export_fingerprint.bytes,
                  sizeof(row->export_fingerprint.bytes)) == 0;
}

static const XrProgramSemanticTypeRecord *
verify_program_type(const XrProgramSemanticClosure *closure, uint32_t index) {
    return index == XI_PSC_ROW_NONE ? NULL : xr_program_semantic_closure_type(closure, index);
}

static bool verify_type_matches_program(const XrProgramSemanticClosure *closure,
                                        const XrProgramSemanticTypeRecord *row,
                                        const XrType *type) {
    if (!closure || !row || !type || type->is_nullable || type->is_const || type->is_literal)
        return false;
    if (row->kind == XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR) {
        const XrExactScalarDesc *scalar = xr_exact_scalar_by_native_type(type->scalar_rep);
        return scalar && scalar->id == row->exact_scalar &&
               ((scalar->family == XR_EXACT_SCALAR_FAMILY_INTEGER && type->kind == XR_KIND_INT) ||
                (scalar->family == XR_EXACT_SCALAR_FAMILY_FLOAT && type->kind == XR_KIND_FLOAT)) &&
               !type->is_value_type;
    }
    if (row->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE ||
        type->kind != XR_KIND_INSTANCE || type->instance.type_arg_count != 0 ||
        !type->instance.class_ref)
        return false;
    const XrClassInfo *info = type->instance.class_ref;
    if (!info->struct_layout || info->is_overlay_union || info->base || info->base_name ||
        info->interface_count != 0 || info->method_count != 0 || info->field_count <= 0 ||
        (uint32_t) info->field_count != row->field_count || !info->fields)
        return false;
    for (uint32_t i = 0; i < row->field_count; i++) {
        const XrProgramSemanticTypeFieldRecord *field =
            xr_program_semantic_closure_type_field(closure, row->field_begin + i);
        const XrProgramSemanticTypeRecord *child = NULL;
        for (uint32_t t = 0; field && t < xr_program_semantic_closure_type_count(closure); t++) {
            const XrProgramSemanticTypeRecord *candidate =
                xr_program_semantic_closure_type(closure, t);
            if (candidate && verify_same_id(candidate->id, field->field_type)) {
                child = candidate;
                break;
            }
        }
        const XaSymbol *source_field = info->fields[i];
        if (!field || field->declaration_ordinal != i || !child || !source_field ||
            source_field->is_static || source_field->is_weak ||
            !verify_type_matches_program(closure, child, source_field->links.type))
            return false;
    }
    return true;
}

static bool verify_bound_type(const XrProgramSemanticClosure *closure, const XiValue *value) {
    const XrProgramSemanticTypeRecord *row =
        value ? verify_program_type(closure, value->psc_type_index) : NULL;
    return row && verify_type_matches_program(closure, row, value->type);
}

static bool expected_program_type_row(const XrProgramSemanticClosure *closure, const XrType *type,
                                      uint32_t *out) {
    if (out)
        *out = XI_PSC_ROW_NONE;
    if (!closure || !type || !out)
        return false;
    uint32_t match = XI_PSC_ROW_NONE;
    for (uint32_t i = 0; closure && i < xr_program_semantic_closure_type_count(closure); i++) {
        const XrProgramSemanticTypeRecord *row = xr_program_semantic_closure_type(closure, i);
        if (!verify_type_matches_program(closure, row, type))
            continue;
        if (match != XI_PSC_ROW_NONE)
            return false;
        match = i;
    }
    *out = match;
    return true;
}

static bool verify_exact_type_metadata(const XrProgramSemanticClosure *closure,
                                       const XiFunc *function, char *error, size_t error_size) {
    uint32_t expected = XI_PSC_ROW_NONE;
    if (!function || !expected_program_type_row(closure, function->return_type, &expected) ||
        function->psc_return_type_index != expected)
        return verify_fail(error, error_size, "Xi return type PSC binding is not exact");
    for (uint16_t p = 0; p < function->nparams; p++)
        if (!function->params[p] ||
            !expected_program_type_row(closure, function->params[p]->type, &expected) ||
            function->params[p]->psc_type_index != expected)
            return verify_fail(error, error_size, "Xi parameter type PSC binding is not exact");
    for (uint32_t b = 0; b < function->nblocks; b++) {
        const XiBlock *block = function->blocks[b];
        if (!block)
            return verify_fail(error, error_size, "Xi type metadata block is NULL");
        for (const XiPhi *phi = block->phis; phi; phi = phi->next)
            if (!expected_program_type_row(closure, phi->value.type, &expected) ||
                phi->value.psc_type_index != expected)
                return verify_fail(error, error_size, "Xi phi type PSC binding is not exact");
        for (uint32_t v = 0; v < block->nvalues; v++)
            if (!block->values[v] ||
                !expected_program_type_row(closure, block->values[v]->type, &expected) ||
                block->values[v]->psc_type_index != expected)
                return verify_fail(error, error_size, "Xi value type PSC binding is not exact");
    }
    return true;
}

static const XiFunc *verify_physical_callee(const XiModule *module, const XiValue *value) {
    const XiValue *cursor = value;
    while (cursor && cursor->op == XI_COPY && cursor->aux_int == XI_COPY_KIND_IDENTITY &&
           cursor->nargs == 1)
        cursor = cursor->args[0];
    if (!cursor)
        return NULL;
    if (cursor->op == XI_CLOSURE_NEW && cursor->aux)
        return (const XiFunc *) cursor->aux;
    if (cursor->op == XI_STACK_ALLOC && cursor->aux_int == XI_CLOSURE_NEW && cursor->aux)
        return (const XiFunc *) cursor->aux;
    if (cursor->op == XI_GET_SHARED && cursor->aux_int >= 0 && module && module->slot_funcs &&
        cursor->aux_int < module->nslots)
        return module->slot_funcs[cursor->aux_int];
    return NULL;
}

static bool verify_value(XiScalarVerifyState *state, const XiFunc *owner, const XiValue *value,
                         char *error, size_t error_size) {
    if (!value)
        return verify_fail(error, error_size, "Xi scalar value inventory contains NULL");
    if (value->op == XI_CALL)
        state->xi_call_count++;
    if (value->psc_call_index == XI_PSC_ROW_NONE) {
        if (value->op == XI_CALL)
            return verify_fail(error, error_size, "bounded Xi call has no PSC call row");
        return true;
    }
    state->indexed_call_count++;
    if (state->indexed_call_count != 1 || value->op != XI_CALL ||
        value->psc_call_index >= xr_program_semantic_closure_call_count(state->closure))
        return verify_fail(error, error_size, "Xi PSC call row is duplicated or out of range");
    const XrProgramSemanticCallRecord *row =
        xr_program_semantic_closure_call(state->closure, value->psc_call_index);
    if (!row || row != state->call_row || owner != state->caller ||
        owner->psc_function_index == XI_PSC_ROW_NONE)
        return verify_fail(error, error_size, "Xi PSC call row has the wrong containing caller");
    const XrProgramSemanticFunctionRecord *owner_row =
        xr_program_semantic_closure_function(state->closure, owner->psc_function_index);
    XiSourceLocator source = {
        .kind = value->source_kind,
        .span = value->source_span,
    };
    if (!owner_row || !verify_same_id(owner_row->id, row->caller_function) ||
        !verify_locator_exact(source, row->locator) || value->nargs != 2 || !value->args ||
        !value->args[0] || !value->args[1] || !verify_i64(value->type) ||
        !verify_i64(value->args[1]->type) ||
        verify_physical_callee(state->module, value->args[0]) != state->callee)
        return verify_fail(error, error_size, "Xi call does not implement the exact PSC call row");
    if (value->flags != xi_op_default_effects(XI_CALL) || value->aux_int != 0 || value->call_plan ||
        value->xg_callsite_id != 0 || value->error_region ||
        value->call_return_ownership.kind != XI_RETURN_OWNERSHIP_UNKNOWN ||
        value->call_return_ownership.param_index != -1 || value->call_return_ownership.complete ||
        value->result_alias_operand != -1)
        return verify_fail(error, error_size,
                           "Xi scalar call adds an unapproved execution contract");
    state->call = value;
    return true;
}

static bool verify_function_values(XiScalarVerifyState *state, const XiFunc *function, char *error,
                                   size_t error_size) {
    if (!function)
        return verify_fail(error, error_size, "Xi scalar function inventory contains NULL");
    for (uint32_t b = 0; b < function->nblocks; b++) {
        const XiBlock *block = function->blocks[b];
        if (!block)
            return verify_fail(error, error_size, "Xi scalar block inventory contains NULL");
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            if (!verify_value(state, function, &phi->value, error, error_size))
                return false;
        }
        for (uint32_t i = 0; i < block->nvalues; i++) {
            if (!verify_value(state, function, block->values[i], error, error_size))
                return false;
        }
    }
    return true;
}

static bool verify_leaf_aggregate_program(const XiModule *module, char *error, size_t error_size) {
    const XrProgramSemanticClosure *closure = module->program_semantic_closure;
    uint32_t aggregate_index = XI_PSC_ROW_NONE;
    const XrProgramSemanticTypeRecord *aggregate = NULL;
    for (uint32_t i = 0; i < xr_program_semantic_closure_type_count(closure); i++) {
        const XrProgramSemanticTypeRecord *row = xr_program_semantic_closure_type(closure, i);
        if (row && row->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE) {
            if (aggregate)
                return verify_fail(error, error_size, "Xi program has multiple aggregate PSC rows");
            aggregate = row;
            aggregate_index = i;
        }
    }
    const XrProgramSemanticCallRecord *call_row = xr_program_semantic_closure_call(closure, 0);
    const XiClassData *aggregate_class =
        module->nclasses == 1 && module->classes ? module->classes[0] : NULL;
    if (!aggregate || !call_row || module->nfuncs != 2 || !module->functions || !module->init ||
        module->init->nchildren != 2 || !aggregate_class ||
        aggregate_class->psc_type_index != aggregate_index ||
        !verify_locator_exact(aggregate_class->source_locator, aggregate->declaration_locator) ||
        !aggregate_class->class_info || aggregate_class->needs_runtime_type ||
        !aggregate_class->struct_layout || module->init->psc_function_index != XI_PSC_ROW_NONE ||
        module->init->psc_return_type_index != XI_PSC_ROW_NONE)
        return verify_fail(error, error_size, "Xi aggregate module inventory is not exact");
    const XiFunc *caller = NULL;
    const XiFunc *callee = NULL;
    for (uint16_t i = 0; i < module->nfuncs; i++) {
        const XiFunc *function = module->functions[i];
        uint16_t child_matches = 0;
        for (uint16_t child = 0; child < module->init->nchildren; child++)
            if (module->init->children[child] == function)
                child_matches++;
        const XrProgramSemanticFunctionRecord *row =
            function ? xr_program_semantic_closure_function(closure, function->psc_function_index)
                     : NULL;
        if (!function || child_matches != 1 || !row || function->parent_func != module->init ||
            function->nchildren != 0)
            return verify_fail(error, error_size, "Xi aggregate function inventory is not exact");
        if (function->psc_return_type_index != aggregate_index || !function->return_type ||
            function->return_type->kind != XR_KIND_INSTANCE ||
            function->return_type->instance.class_ref != aggregate_class->class_info ||
            !verify_type_matches_program(closure, aggregate, function->return_type))
            return verify_fail(error, error_size,
                               "Xi aggregate function return binding is not exact");
        if (!verify_locator_exact(function->psc_declaration_locator, row->declaration_locator))
            return verify_fail(error, error_size, "Xi aggregate declaration locator is not exact");
        if (row->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) {
            if (caller || function->nparams != 0)
                return verify_fail(error, error_size, "Xi aggregate entry contract is invalid");
            caller = function;
        } else if (row->flags == 0) {
            if (callee || function->nparams != 1 || !function->params || !function->params[0] ||
                !function->params[0]->type || function->params[0]->type->kind != XR_KIND_INSTANCE ||
                function->params[0]->type->instance.class_ref != aggregate_class->class_info ||
                function->params[0]->param_mode != XR_PARAM_READ ||
                function->params[0]->psc_type_index != aggregate_index ||
                !verify_bound_type(closure, function->params[0]) ||
                function->inline_policy != XI_INLINE_PRESERVE_CALL)
                return verify_fail(error, error_size, "Xi aggregate callee contract is invalid");
            callee = function;
        } else {
            return verify_fail(error, error_size, "Xi aggregate function flags are invalid");
        }
    }
    if (!verify_exact_type_metadata(closure, module->init, error, error_size))
        return false;
    for (uint16_t i = 0; i < module->nfuncs; i++)
        if (!verify_exact_type_metadata(closure, module->functions[i], error, error_size))
            return false;
    const XiValue *call = NULL;
    uint32_t call_count = 0;
    const XiFunc *owners[3] = {module->init, caller, callee};
    for (uint32_t f = 0; f < 3; f++) {
        const XiFunc *owner = owners[f];
        if (!owner)
            continue;
        for (uint32_t b = 0; b < owner->nblocks; b++) {
            const XiBlock *block = owner->blocks[b];
            if (!block)
                return verify_fail(error, error_size, "Xi aggregate block inventory contains NULL");
            for (const XiPhi *phi = block->phis; phi; phi = phi->next)
                if (phi->value.psc_call_index != XI_PSC_ROW_NONE)
                    return verify_fail(error, error_size,
                                       "Xi aggregate phi carries a forbidden PSC call row");
            for (uint32_t v = 0; v < block->nvalues; v++) {
                const XiValue *value = block->values[v];
                if (!value)
                    return verify_fail(error, error_size,
                                       "Xi aggregate value inventory contains NULL");
                if (value->op != XI_CALL) {
                    if (value->psc_call_index != XI_PSC_ROW_NONE)
                        return verify_fail(error, error_size,
                                           "Xi aggregate non-call carries a PSC call row");
                    continue;
                }
                call_count++;
                if (call || owner != caller || value->psc_call_index != 0 ||
                    value->psc_type_index != aggregate_index || value->nargs != 2 || !value->args ||
                    !value->args[0] || !value->args[1] ||
                    value->args[1]->psc_type_index != aggregate_index ||
                    !verify_bound_type(closure, value) ||
                    !verify_bound_type(closure, value->args[1]) ||
                    value->flags != xi_op_default_effects(XI_CALL) || value->aux_int != 0 ||
                    value->call_plan || value->xg_callsite_id != 0 || value->error_region ||
                    value->transfer_mode != 0 || value->param_mode != XR_PARAM_READ ||
                    value->call_return_ownership.kind != XI_RETURN_OWNERSHIP_UNKNOWN ||
                    value->call_return_ownership.param_index != -1 ||
                    value->call_return_ownership.complete || value->result_alias_operand != -1 ||
                    verify_physical_callee(module, value->args[0]) != callee)
                    return verify_fail(error, error_size, "Xi aggregate call binding is invalid");
                const XrProgramSemanticFunctionRecord *caller_row =
                    xr_program_semantic_closure_function(closure, caller->psc_function_index);
                XiSourceLocator locator = {
                    .kind = value->source_kind,
                    .span = value->source_span,
                };
                if (!caller_row || !verify_same_id(caller_row->id, call_row->caller_function) ||
                    !verify_locator_exact(locator, call_row->locator))
                    return verify_fail(error, error_size, "Xi aggregate call locator is invalid");
                call = value;
            }
        }
    }
    return (caller && callee && call && call_count == 1) ||
           verify_fail(error, error_size, "Xi aggregate call coverage is incomplete");
}

bool xi_program_semantic_verify(const XiModule *module, const XrTargetProfile *target_profile,
                                char *error, size_t error_size) {
    if (!module || !module->program_semantic_closure || !module->init ||
        !verify_source_module_exact(module) ||
        !xr_program_semantic_closure_verify(module->program_semantic_closure, NULL, 0))
        return verify_fail(error, error_size, "Xi program verification requires a verified PSC");
    XrProgramSemanticFamily family =
        xr_program_semantic_closure_family(module->program_semantic_closure);
    if (family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL) {
        if (module->scalar_call_decision || module->scalar_target_profile || target_profile)
            return verify_fail(error, error_size,
                               "aggregate Xi authority cannot retain target facts");
        return verify_leaf_aggregate_program(module, error, error_size);
    }
    if (family != XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL)
        return verify_fail(error, error_size, "Xi program semantic family is unsupported");
    if (!module->scalar_call_decision || !module->scalar_target_profile || !target_profile ||
        !xr_target_profile_verify(module->scalar_target_profile, NULL, 0) ||
        !xr_target_profile_verify(target_profile, NULL, 0) ||
        !xr_target_profile_require_exact(module->scalar_target_profile, target_profile, NULL, 0) ||
        !xr_scalar_call_decision_verify(module->scalar_call_decision,
                                        module->program_semantic_closure,
                                        module->scalar_target_profile, NULL, 0))
        return verify_fail(error, error_size, "Xi scalar verification requires exact authorities");
    const XrProgramSemanticClosure *closure = module->program_semantic_closure;
    const XrScalarCallDecision *decision = module->scalar_call_decision;
    if (xr_program_semantic_closure_schema(closure) != XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION ||
        xr_program_semantic_closure_module_count(closure) != 1 ||
        xr_program_semantic_closure_dependency_count(closure) != 0 ||
        xr_program_semantic_closure_type_count(closure) != 0 ||
        xr_program_semantic_closure_function_count(closure) != 2 ||
        xr_program_semantic_closure_call_count(closure) != 1 || module->nfuncs != 2 ||
        !module->functions || module->init->nchildren != 2 ||
        module->init->psc_function_index != XI_PSC_ROW_NONE ||
        module->init->psc_declaration_locator.kind != 0 ||
        !verify_span_empty(module->init->psc_declaration_locator.span))
        return verify_fail(error, error_size, "Xi scalar module inventory is not exact");

    XiScalarVerifyState state = {
        .module = module,
        .closure = closure,
        .call_row = xr_program_semantic_closure_call(closure, 0),
    };
    bool seen[2] = {false, false};
    for (uint16_t i = 0; i < module->nfuncs; i++) {
        const XiFunc *function = module->functions[i];
        if (!function || function != module->init->children[i] ||
            function->parent_func != module->init || function->nchildren != 0 ||
            function->psc_function_index >= 2 || seen[function->psc_function_index])
            return verify_fail(error, error_size, "Xi scalar function row inventory is invalid");
        seen[function->psc_function_index] = true;
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(closure, function->psc_function_index);
        if (!row ||
            !verify_locator_exact(function->psc_declaration_locator, row->declaration_locator) ||
            !verify_i64(function->return_type))
            return verify_fail(error, error_size, "Xi function does not match its exact PSC row");
        if (verify_same_id(row->id, decision->caller_function)) {
            if (state.caller || function->nparams != 0 ||
                row->flags != XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY)
                return verify_fail(error, error_size, "Xi scalar caller contract is invalid");
            state.caller = function;
        }
        if (verify_same_id(row->id, decision->callee_function)) {
            if (state.callee || function->nparams != 1 || !function->params ||
                !function->params[0] || function->params[0]->op != XI_PARAM ||
                !verify_i64(function->params[0]->type) ||
                function->params[0]->param_mode != XR_PARAM_READ || row->flags != 0 ||
                function->inline_policy != XI_INLINE_PRESERVE_CALL)
                return verify_fail(error, error_size, "Xi scalar callee contract is invalid");
            state.callee = function;
        }
    }
    if (!seen[0] || !seen[1] || !state.caller || !state.callee || state.caller == state.callee ||
        !state.call_row || !verify_same_id(state.call_row->id, decision->call_identity) ||
        !verify_same_id(state.call_row->callsite_identity, decision->callsite_identity) ||
        !verify_same_id(state.call_row->caller_function, decision->caller_function) ||
        !verify_same_id(state.call_row->callee_function, decision->callee_function))
        return verify_fail(error, error_size, "Xi scalar identities do not match the decision");

    if (!verify_function_values(&state, module->init, error, error_size))
        return false;
    for (uint16_t i = 0; i < module->nfuncs; i++) {
        if (!verify_function_values(&state, module->functions[i], error, error_size))
            return false;
    }
    if (state.indexed_call_count != 1 || state.xi_call_count != 1 || !state.call)
        return verify_fail(error, error_size, "Xi scalar call row coverage is not exact");
    return true;
}
