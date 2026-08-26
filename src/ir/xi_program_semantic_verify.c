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
#include "xi_value_query.h"
#include "../frontend/analyzer/xanalyzer_symbol.h"
#include "../plan/target/xr_target_profile.h"
#include "../runtime/value/xtype.h"
#include "../runtime/class/xclass_info.h"
#include "../shared/xr_exact_scalar_registry.h"
#include "../plan/semantic/xr_source_semantic_identity.h"
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

static bool verify_graph_function_signature_effect(const XiFunc *function,
                                                   uint16_t parameter_count) {
    return function && function->nparams == parameter_count &&
           function->min_params == parameter_count && !function->is_vararg &&
           !function->is_extern && !function->is_generic_template &&
           function->error_effect_nothrow && function->analyzer_effect_complete &&
           function->semantic_effects == 0 && function->unknown_semantic_effects == 0 &&
           function->effect_unknown_reasons == 0 && !function->contains_unsafe_op &&
           !function->requires_unsafe_at_call && function->entry_type == 0;
}

static bool verify_graph_export_function_type(const XrType *type, const XiFunc *function) {
    return type && function && type->kind == XR_KIND_FUNCTION && !type->is_nullable &&
           !type->is_const && !type->is_literal && type->function.params &&
           type->function.param_count == 1 &&
           type->function.min_params == 1 && !type->function.is_variadic &&
           !type->function.is_c_abi && type->function.throw_effect == XR_FN_EFFECT_NO_THROW &&
           type->function.type_param_count == 0 && verify_i64(type->function.return_type) &&
           verify_i64(type->function.params[0].type) &&
           type->function.params[0].mode == XR_PARAM_READ &&
           verify_graph_function_signature_effect(function, 1);
}

static bool verify_source_module_exact(const XiModule *module) {
    const XrProgramSemanticClosure *closure = module ? module->program_semantic_closure : NULL;
    const XrProgramSemanticModuleRecord *row =
        closure ? xr_program_semantic_closure_module(closure, module->psc_module_index) : NULL;
    const XrProgramSemanticModuleInput *source =
        module && module->source_semantic_module_present ? &module->source_semantic_module : NULL;
    bool graph = closure && xr_program_semantic_closure_family(closure) ==
                                XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL;
    XrProgramSemanticModuleInput rebuilt = {0};
    return row && source && module->identity &&
           xr_source_semantic_module_authority(module->identity, source->source_fingerprint,
                                               &rebuilt, NULL) &&
           verify_same_id(rebuilt.module_identity, source->module_identity) &&
           memcmp(rebuilt.module_authority_fingerprint.bytes,
                  source->module_authority_fingerprint.bytes,
                  sizeof(rebuilt.module_authority_fingerprint.bytes)) == 0 &&
           verify_same_id(row->module_identity, source->module_identity) &&
           memcmp(row->module_authority_fingerprint.bytes,
                  source->module_authority_fingerprint.bytes,
                  sizeof(row->module_authority_fingerprint.bytes)) == 0 &&
           memcmp(row->source_fingerprint.bytes, source->source_fingerprint.bytes,
                  sizeof(row->source_fingerprint.bytes)) == 0 &&
           (graph || memcmp(row->export_fingerprint.bytes, source->export_fingerprint.bytes,
                            sizeof(row->export_fingerprint.bytes)) == 0);
}

static const XrProgramSemanticTypeRecord *
verify_program_type(const XrProgramSemanticClosure *closure, uint32_t index) {
    return index == XI_PSC_ROW_NONE ? NULL : xr_program_semantic_closure_type(closure, index);
}

static const XrProgramSemanticTypeRecord *verify_find_type(
    const XrProgramSemanticClosure *closure, XrStableId identity, uint32_t *index) {
    const XrProgramSemanticTypeRecord *answer = NULL;
    for (uint32_t i = 0; closure && i < xr_program_semantic_closure_type_count(closure); i++) {
        const XrProgramSemanticTypeRecord *candidate =
            xr_program_semantic_closure_type(closure, i);
        if (!candidate || !verify_same_id(candidate->id, identity))
            continue;
        if (answer)
            return NULL;
        answer = candidate;
        if (index)
            *index = i;
    }
    return answer;
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
    if (row->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT) {
        if (xr_program_semantic_closure_family(closure) !=
                XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL ||
            type->kind != XR_KIND_TUPLE || row->field_count != 6 ||
            xr_type_tuple_count((XrType *) type) != (int) row->field_count)
            return false;
        for (uint32_t i = 0; i < row->field_count; i++) {
            const XrProgramSemanticTypeFieldRecord *field =
                xr_program_semantic_closure_type_field(closure, row->field_begin + i);
            const XrProgramSemanticTypeRecord *child =
                field ? verify_find_type(closure, field->field_type, NULL) : NULL;
            XrType *member = xr_type_tuple_get((XrType *) type, (int) i);
            if (!field || field->declaration_ordinal != i || !child || !member ||
                !verify_type_matches_program(closure, child, member))
                return false;
        }
        return true;
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

static bool verify_class_declaration_for_type(const XiModule *module, const XrType *type,
                                              const XiClassData **out) {
    if (out)
        *out = NULL;
    if (!module || !type || !out || type->kind != XR_KIND_INSTANCE || !type->instance.class_ref)
        return false;
    const XiClassData *match = NULL;
    for (uint16_t i = 0; i < module->nclasses; i++) {
        const XiClassData *candidate = module->classes ? module->classes[i] : NULL;
        if (!candidate || candidate->class_info != type->instance.class_ref)
            continue;
        if (match)
            return false;
        match = candidate;
    }
    *out = match;
    return match != NULL;
}

static bool expected_program_type_row(const XiModule *module,
                                      const XrProgramSemanticClosure *closure, const XrType *type,
                                      uint32_t *out) {
    if (out)
        *out = XI_PSC_ROW_NONE;
    if (!closure || !type || !out)
        return false;
    const XiClassData *declaration = NULL;
    if (type->kind == XR_KIND_INSTANCE && type->instance.class_ref) {
        if (!verify_class_declaration_for_type(module, type, &declaration))
            return false;
        const XrProgramSemanticTypeRecord *row =
            verify_program_type(closure, declaration->psc_type_index);
        if (!row || row->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE ||
            !verify_locator_exact(declaration->source_locator, row->declaration_locator) ||
            !verify_type_matches_program(closure, row, type))
            return false;
        *out = declaration->psc_type_index;
        return true;
    }
    uint32_t match = XI_PSC_ROW_NONE;
    for (uint32_t i = 0; closure && i < xr_program_semantic_closure_type_count(closure); i++) {
        const XrProgramSemanticTypeRecord *row = xr_program_semantic_closure_type(closure, i);
        if (!row || (row->kind != XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR &&
                     row->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT))
            continue;
        if (!verify_type_matches_program(closure, row, type))
            continue;
        if (match != XI_PSC_ROW_NONE)
            return false;
        match = i;
    }
    *out = match;
    return true;
}

static bool verify_bound_type(const XiModule *module, const XrProgramSemanticClosure *closure,
                              const XiValue *value) {
    uint32_t expected = XI_PSC_ROW_NONE;
    return value && expected_program_type_row(module, closure, value->type, &expected) &&
           value->psc_type_index == expected;
}

static bool verify_exact_type_metadata(const XiModule *module,
                                       const XrProgramSemanticClosure *closure,
                                       const XiFunc *function, char *error, size_t error_size) {
    uint32_t expected = XI_PSC_ROW_NONE;
    if (!function ||
        (function->nparams > 0 && !function->params) ||
        (function->nblocks > 0 && !function->blocks) ||
        !expected_program_type_row(module, closure, function->return_type, &expected) ||
        function->psc_return_type_index != expected)
        return verify_fail(error, error_size, "Xi return type PSC binding is not exact");
    for (uint16_t p = 0; p < function->nparams; p++)
        if (!function->params[p] ||
            !expected_program_type_row(module, closure, function->params[p]->type, &expected) ||
            function->params[p]->psc_type_index != expected)
            return verify_fail(error, error_size, "Xi parameter type PSC binding is not exact");
    for (uint32_t b = 0; b < function->nblocks; b++) {
        const XiBlock *block = function->blocks[b];
        if (!block)
            return verify_fail(error, error_size, "Xi type metadata block is NULL");
        if (block->nvalues > 0 && !block->values)
            return verify_fail(error, error_size, "Xi type metadata values are missing");
        for (const XiPhi *phi = block->phis; phi; phi = phi->next)
            if (!expected_program_type_row(module, closure, phi->value.type, &expected) ||
                phi->value.psc_type_index != expected)
                return verify_fail(error, error_size, "Xi phi type PSC binding is not exact");
        for (uint32_t v = 0; v < block->nvalues; v++)
            if (!block->values[v] ||
                !expected_program_type_row(module, closure, block->values[v]->type, &expected) ||
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
        if ((row->flags & XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) != 0) {
            if (caller || function->nparams != 0)
                return verify_fail(error, error_size, "Xi aggregate entry contract is invalid");
            caller = function;
        } else if (row->flags == 0) {
            if (callee || function->nparams != 1 || !function->params || !function->params[0] ||
                !function->params[0]->type || function->params[0]->type->kind != XR_KIND_INSTANCE ||
                function->params[0]->type->instance.class_ref != aggregate_class->class_info ||
                function->params[0]->param_mode != XR_PARAM_READ ||
                function->params[0]->psc_type_index != aggregate_index ||
                !verify_bound_type(module, closure, function->params[0]) ||
                function->inline_policy != XI_INLINE_PRESERVE_CALL)
                return verify_fail(error, error_size, "Xi aggregate callee contract is invalid");
            callee = function;
        } else {
            return verify_fail(error, error_size, "Xi aggregate function flags are invalid");
        }
    }
    if (!verify_exact_type_metadata(module, closure, module->init, error, error_size))
        return false;
    for (uint16_t i = 0; i < module->nfuncs; i++)
        if (!verify_exact_type_metadata(module, closure, module->functions[i], error, error_size))
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
                    !verify_bound_type(module, closure, value) ||
                    !verify_bound_type(module, closure, value->args[1]) ||
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

typedef struct XiProductFunctionState {
    const XiFunc *function;
    const XiValue *call;
    const XiValue *construct;
    const XiValue *projects[6];
    uint32_t call_count;
    uint32_t construct_count;
    uint32_t project_count;
} XiProductFunctionState;

static const XiValue *verify_product_strip_copy(const XiValue *value) {
    const XiValue *cursor = value;
    while (cursor && cursor->op == XI_COPY && cursor->aux_int == XI_COPY_KIND_IDENTITY &&
           cursor->nargs == 1 && cursor->args && cursor->args[0])
        cursor = cursor->args[0];
    return cursor;
}

static bool verify_product_proof_contract(const XiValue *value, uint16_t op) {
    return value && value->op == op && value->flags == xi_op_default_effects(op) &&
           !value->aux && value->aux_kind == XI_AUX_KIND_NONE && value->lowering_flags == 0 &&
           !value->call_plan && value->xg_callsite_id == 0 && !value->error_region &&
           value->transfer_mode == 0 && value->param_mode == XR_PARAM_READ &&
           value->call_return_ownership.kind == XI_RETURN_OWNERSHIP_UNKNOWN &&
           value->call_return_ownership.param_index == -1 &&
           !value->call_return_ownership.complete && value->result_alias_operand == -1;
}

static bool verify_product_call_contract(const XiModule *module,
                                         const XrProgramSemanticClosure *closure,
                                         const XiFunc *owner, const XiFunc *callee,
                                         const XiValue *value, uint32_t product_index) {
    if (!value || value->op != XI_CALL || value->psc_call_index >= 2 || value->nargs != 1 ||
        !value->args || !value->args[0] || value->psc_type_index != product_index ||
        verify_physical_callee(module, value->args[0]) != callee ||
        value->flags != xi_op_default_effects(XI_CALL) || value->aux_int != 0 ||
        value->call_plan || value->xg_callsite_id != 0 || value->error_region ||
        value->transfer_mode != 0 || value->param_mode != XR_PARAM_READ ||
        value->call_return_ownership.kind != XI_RETURN_OWNERSHIP_UNKNOWN ||
        value->call_return_ownership.param_index != -1 || value->call_return_ownership.complete ||
        value->result_alias_operand != -1)
        return false;
    const XrProgramSemanticCallRecord *call =
        xr_program_semantic_closure_call(closure, value->psc_call_index);
    const XrProgramSemanticFunctionRecord *owner_row =
        owner ? xr_program_semantic_closure_function(closure, owner->psc_function_index) : NULL;
    XiSourceLocator locator = {.kind = value->source_kind, .span = value->source_span};
    return call && owner_row && verify_same_id(owner_row->id, call->caller_function) &&
           verify_locator_exact(locator, call->locator);
}

static bool verify_product_init_is_authority_free(const XiFunc *init, char *error,
                                                  size_t error_size) {
    for (uint32_t b = 0; init && b < init->nblocks; b++) {
        const XiBlock *block = init->blocks[b];
        if (!block)
            return verify_fail(error, error_size,
                               "Xi value-product init block inventory contains NULL");
        for (const XiPhi *phi = block->phis; phi; phi = phi->next)
            if (phi->value.psc_call_index != XI_PSC_ROW_NONE ||
                phi->value.op == XI_VALUE_PRODUCT_CONSTRUCT ||
                phi->value.op == XI_VALUE_PRODUCT_PROJECT || phi->value.op == XI_TUPLE_NEW ||
                phi->value.op == XI_TUPLE_GET)
                return verify_fail(error, error_size,
                                   "Xi module init retains value-product authority");
        for (uint32_t v = 0; v < block->nvalues; v++) {
            const XiValue *value = block->values[v];
            if (!value || value->psc_call_index != XI_PSC_ROW_NONE ||
                value->op == XI_VALUE_PRODUCT_CONSTRUCT ||
                value->op == XI_VALUE_PRODUCT_PROJECT || value->op == XI_TUPLE_NEW ||
                value->op == XI_TUPLE_GET)
                return verify_fail(error, error_size,
                                   "Xi module init retains value-product authority");
        }
    }
    return true;
}

static bool verify_leaf_product_program(const XiModule *module, char *error, size_t error_size) {
    const XrProgramSemanticClosure *closure = module->program_semantic_closure;
    if (xr_program_semantic_closure_schema(closure) !=
            XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION ||
        xr_program_semantic_closure_module_count(closure) != 1 ||
        xr_program_semantic_closure_dependency_count(closure) != 0 ||
        xr_program_semantic_closure_type_count(closure) != 3 ||
        xr_program_semantic_closure_type_field_count(closure) != 6 ||
        xr_program_semantic_closure_function_count(closure) != 3 ||
        xr_program_semantic_closure_function_parameter_count(closure) != 0 ||
        xr_program_semantic_closure_call_count(closure) != 2 || module->nclasses != 0 ||
        module->nfuncs != 3 || !module->functions || !module->init ||
        module->init->nchildren != 3 || module->init->psc_function_index != XI_PSC_ROW_NONE ||
        module->init->psc_return_type_index != XI_PSC_ROW_NONE)
        return verify_fail(error, error_size, "Xi value-product module inventory is not exact");

    uint32_t product_index = XI_PSC_ROW_NONE;
    const XrProgramSemanticTypeRecord *product = NULL;
    uint32_t member_indices[6] = {XI_PSC_ROW_NONE, XI_PSC_ROW_NONE, XI_PSC_ROW_NONE,
                                  XI_PSC_ROW_NONE, XI_PSC_ROW_NONE, XI_PSC_ROW_NONE};
    for (uint32_t i = 0; i < 3; i++) {
        const XrProgramSemanticTypeRecord *row = xr_program_semantic_closure_type(closure, i);
        if (row && row->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT) {
            if (product)
                return verify_fail(error, error_size, "Xi value-product type row is ambiguous");
            product = row;
            product_index = i;
        }
    }
    if (!product || product->field_count != 6)
        return verify_fail(error, error_size, "Xi value-product type row is missing");
    for (uint32_t i = 0; i < 6; i++) {
        const XrProgramSemanticTypeFieldRecord *field =
            xr_program_semantic_closure_type_field(closure, product->field_begin + i);
        const XrProgramSemanticTypeRecord *member =
            field ? verify_find_type(closure, field->field_type, &member_indices[i]) : NULL;
        uint8_t expected_scalar = i == 2 ? XR_EXACT_SCALAR_U8 : XR_EXACT_SCALAR_I64;
        if (!field || field->declaration_ordinal != i || !member ||
            member->kind != XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR ||
            member->exact_scalar != expected_scalar)
            return verify_fail(error, error_size, "Xi value-product member authority is invalid");
    }

    const XrProgramSemanticCallRecord *call_rows[2] = {
        xr_program_semantic_closure_call(closure, 0),
        xr_program_semantic_closure_call(closure, 1),
    };
    if (!call_rows[0] || !call_rows[1] ||
        !verify_same_id(call_rows[0]->callee_function, call_rows[1]->callee_function) ||
        verify_same_id(call_rows[0]->caller_function, call_rows[1]->caller_function))
        return verify_fail(error, error_size, "Xi value-product call joins are not canonical");

    XiProductFunctionState states[3] = {0};
    XiFunc *callee = NULL;
    bool seen_rows[3] = {false, false, false};
    for (uint16_t i = 0; i < module->nfuncs; i++) {
        XiFunc *function = module->functions[i];
        uint16_t child_matches = 0;
        for (uint16_t child = 0; child < module->init->nchildren; child++)
            child_matches += module->init->children[child] == function;
        const XrProgramSemanticFunctionRecord *row =
            function ? xr_program_semantic_closure_function(closure,
                                                             function->psc_function_index)
                     : NULL;
        if (!function || function->psc_function_index >= 3 ||
            seen_rows[function->psc_function_index] || child_matches != 1 || !row ||
            function->parent_func != module->init || function->nchildren != 0 ||
            function->nparams != 0 || function->psc_return_type_index != product_index ||
            !verify_type_matches_program(closure, product, function->return_type) ||
            !verify_locator_exact(function->psc_declaration_locator, row->declaration_locator) ||
            (row->flags != 0 && row->flags != XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY))
            return verify_fail(error, error_size,
                               "Xi value-product function inventory is not exact");
        seen_rows[function->psc_function_index] = true;
        states[i].function = function;
        if (row->flags == 0) {
            if (callee || function->inline_policy != XI_INLINE_PRESERVE_CALL ||
                !verify_same_id(row->id, call_rows[0]->callee_function))
                return verify_fail(error, error_size,
                                   "Xi value-product callee join is not exact");
            callee = function;
        }
    }
    if (!callee || !seen_rows[0] || !seen_rows[1] || !seen_rows[2] ||
        !verify_exact_type_metadata(module, closure, module->init, error, error_size) ||
        !verify_product_init_is_authority_free(module->init, error, error_size))
        return false;
    for (uint16_t i = 0; i < 3; i++)
        if (!verify_exact_type_metadata(module, closure, states[i].function, error, error_size))
            return false;

    bool seen_calls[2] = {false, false};
    uint32_t total_calls = 0, total_constructs = 0, total_projects = 0;
    for (uint16_t f = 0; f < 3; f++) {
        XiProductFunctionState *state = &states[f];
        const XrProgramSemanticFunctionRecord *function_row = xr_program_semantic_closure_function(
            closure, state->function->psc_function_index);
        for (uint32_t b = 0; b < state->function->nblocks; b++) {
            const XiBlock *block = state->function->blocks[b];
            if (!block)
                return verify_fail(error, error_size,
                                   "Xi value-product block inventory contains NULL");
            for (const XiPhi *phi = block->phis; phi; phi = phi->next)
                if (phi->value.psc_call_index != XI_PSC_ROW_NONE)
                    return verify_fail(error, error_size,
                                       "Xi value-product phi carries a call row");
            for (uint32_t v = 0; v < block->nvalues; v++) {
                const XiValue *value = block->values[v];
                if (!value)
                    return verify_fail(error, error_size,
                                       "Xi value-product value inventory contains NULL");
                if (value->op == XI_TUPLE_NEW || value->op == XI_TUPLE_GET)
                    return verify_fail(error, error_size,
                                       "managed tuple operation retained product authority");
                if (value->op == XI_CALL) {
                    if (state->call || value->psc_call_index >= 2 ||
                        seen_calls[value->psc_call_index] ||
                        !verify_product_call_contract(module, closure, state->function, callee,
                                                      value, product_index))
                        return verify_fail(error, error_size,
                                           "Xi value-product call binding is invalid");
                    seen_calls[value->psc_call_index] = true;
                    state->call = value;
                    state->call_count++;
                    total_calls++;
                    continue;
                }
                if (value->psc_call_index != XI_PSC_ROW_NONE)
                    return verify_fail(error, error_size,
                                       "Xi value-product non-call carries a call row");
                if (value->op == XI_VALUE_PRODUCT_CONSTRUCT) {
                    state->construct_count++;
                    total_constructs++;
                    if (state->construct ||
                        !verify_product_proof_contract(value, XI_VALUE_PRODUCT_CONSTRUCT) ||
                        value->psc_type_index != product_index ||
                        value->nargs != 6 || !value->args || value->aux_int != 6)
                        return verify_fail(error, error_size,
                                           "Xi value-product construction is invalid");
                    for (uint32_t ordinal = 0; ordinal < 6; ordinal++)
                        if (!value->args[ordinal] ||
                            value->args[ordinal]->psc_type_index != member_indices[ordinal])
                            return verify_fail(error, error_size,
                                               "Xi value-product construction order is invalid");
                    state->construct = value;
                } else if (value->op == XI_VALUE_PRODUCT_PROJECT) {
                    state->project_count++;
                    total_projects++;
                    if (!verify_product_proof_contract(value, XI_VALUE_PRODUCT_PROJECT) ||
                        value->nargs != 1 || !value->args || !value->args[0] ||
                        value->aux_int < 0 || value->aux_int >= 6)
                        return verify_fail(error, error_size,
                                           "Xi value-product projection ordinal is invalid");
                    uint32_t ordinal = (uint32_t) value->aux_int;
                    if (state->projects[ordinal] ||
                        value->psc_type_index != member_indices[ordinal])
                        return verify_fail(error, error_size,
                                           "Xi value-product projection member is invalid");
                    state->projects[ordinal] = value;
                }
            }
        }
        bool entry = function_row &&
                     function_row->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY;
        if (!state->construct || state->construct_count != 1 ||
            (entry && (state->call_count != 1 || state->project_count != 6)) ||
            (!entry && (state->call_count != 0 || state->project_count != 0)))
            return verify_fail(error, error_size,
                               "Xi value-product function shape is incomplete");
        if (!state->function->nblocks ||
            verify_product_strip_copy(
                state->function->blocks[state->function->nblocks - 1]->control) !=
                state->construct)
            return verify_fail(error, error_size,
                               "Xi value-product return does not use its construction");
        if (entry) {
            for (uint32_t ordinal = 0; ordinal < 6; ordinal++)
                if (!state->projects[ordinal] ||
                    verify_product_strip_copy(state->projects[ordinal]->args[0]) != state->call ||
                    verify_product_strip_copy(state->construct->args[ordinal]) !=
                        state->projects[ordinal])
                    return verify_fail(error, error_size,
                                       "Xi value-product project/construct join is incomplete");
        }
    }
    return (seen_calls[0] && seen_calls[1] && total_calls == 2 && total_constructs == 3 &&
            total_projects == 12) ||
           verify_fail(error, error_size, "Xi value-product operation coverage is incomplete");
}

static bool verify_single_module_partition(const XiModule *module,
                                           const XrTargetProfile *target_profile, char *error,
                                           size_t error_size) {
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
    if (family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL) {
        if (module->scalar_call_decision || module->scalar_target_profile || target_profile)
            return verify_fail(error, error_size,
                               "value-product Xi authority cannot retain target facts");
        return verify_leaf_product_program(module, error, error_size);
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

typedef struct XiGraphPartitionState {
    const XiFunc *function;
    const XiValue *call;
    const XiImportRef *import_ref;
    uint32_t call_index;
    uint32_t indexed_call_count;
    uint32_t import_value_count;
} XiGraphPartitionState;

static bool graph_function_is_local(const XrProgramSemanticClosure *closure,
                                    uint32_t module_index,
                                    const XrProgramSemanticFunctionRecord *function) {
    const XrProgramSemanticModuleRecord *module =
        xr_program_semantic_closure_module(closure, module_index);
    return module && function && verify_same_id(module->module_identity, function->module_identity);
}

static bool graph_verify_values(const XiModule *module, const XiFunc *function,
                                XiGraphPartitionState *state, char *error, size_t error_size) {
    const XrProgramSemanticClosure *closure = module->program_semantic_closure;
    const XrProgramSemanticFunctionRecord *function_row =
        xr_program_semantic_closure_function(closure, function->psc_function_index);
    for (uint32_t b = 0; b < function->nblocks; b++) {
        if (!function->blocks)
            return verify_fail(error, error_size, "Xi graph block inventory is missing");
        const XiBlock *block = function->blocks[b];
        if (!block)
            return verify_fail(error, error_size, "Xi graph block inventory contains NULL");
        if (block->nvalues > 0 && !block->values)
            return verify_fail(error, error_size, "Xi graph value inventory is missing");
        for (const XiPhi *phi = block->phis; phi; phi = phi->next)
            if (phi->value.psc_call_index != XI_PSC_ROW_NONE)
                return verify_fail(error, error_size,
                                   "Xi graph phi carries a forbidden PSC call row");
        for (uint32_t i = 0; i < block->nvalues; i++) {
            const XiValue *value = block->values[i];
            if (!value)
                return verify_fail(error, error_size,
                                   "Xi graph value inventory contains NULL");
            if (value->op == XI_IMPORT_REF) {
                const XiImportRef *value_ref = value->aux ? (const XiImportRef *) value->aux : NULL;
                state->import_value_count++;
                if (!value_ref || (state->import_ref && state->import_ref != value_ref))
                    return verify_fail(error, error_size,
                                       "Xi graph import value inventory is not unique");
                state->import_ref = value_ref;
            }
            if (value->psc_call_index == XI_PSC_ROW_NONE) {
                if (value->op == XI_CALL)
                    return verify_fail(error, error_size,
                                       "Xi graph call lacks its exact PSC row");
                continue;
            }
            state->indexed_call_count++;
            const XrProgramSemanticCallRecord *call =
                xr_program_semantic_closure_call(closure, value->psc_call_index);
            XiSourceLocator locator = {
                .kind = value->source_kind,
                .span = value->source_span,
            };
            const XiImportRef *ref =
                value->nargs > 0 && value->args ? xi_value_import_ref(function, value->args[0])
                                                : NULL;
            if (state->call || !call || !function_row || value->op != XI_CALL ||
                !verify_same_id(function_row->id, call->caller_function) ||
                !verify_locator_exact(locator, call->locator) || value->psc_type_index != 0 ||
                !verify_bound_type(module, closure, value) || value->nargs != 2 || !value->args ||
                !value->args[0] || !value->args[1] || value->args[1]->psc_type_index != 0 ||
                !verify_bound_type(module, closure, value->args[1]) || !ref ||
                ref->psc_dependency_index == XI_PSC_ROW_NONE ||
                !verify_same_id(ref->psc_resolver_binding, call->resolver_binding) ||
                value->flags != xi_op_default_effects(XI_CALL) || value->aux_int != 0 ||
                value->call_plan || value->xg_callsite_id != 0 || value->error_region ||
                value->transfer_mode != 0 || value->param_mode != XR_PARAM_READ ||
                value->call_return_ownership.kind != XI_RETURN_OWNERSHIP_UNKNOWN ||
                value->call_return_ownership.param_index != -1 ||
                value->call_return_ownership.complete || value->result_alias_operand != -1)
                return verify_fail(error, error_size, "Xi graph call partition is invalid");
            const XrProgramSemanticDependencyRecord *dependency =
                xr_program_semantic_closure_dependency(closure, ref->psc_dependency_index);
            if (!dependency ||
                !verify_same_id(dependency->source_module, function_row->module_identity) ||
                !verify_locator_exact(ref->psc_import_locator, dependency->import_locator) ||
                !verify_same_id(dependency->exported_function, call->callee_function) ||
                !verify_same_id(dependency->resolver_binding, call->resolver_binding))
                return verify_fail(error, error_size,
                                   "Xi graph import dependency join is invalid");
            state->call = value;
            state->call_index = value->psc_call_index;
        }
    }
    return true;
}

static const XiValue *graph_unwrap_identity(const XiValue *value) {
    uint32_t depth = 0;
    while (value && depth++ < 64 &&
           (value->op == XI_BOX || value->op == XI_UNBOX ||
            xi_copy_is_identity_alias(value) || xi_op_is_identity_forward(value->op))) {
        if (value->nargs < 1 || !value->args || !value->args[0])
            return NULL;
        value = value->args[0];
    }
    return depth <= 64 ? value : NULL;
}

static const XiValue *graph_unique_shared_store(const XiFunc *function, int64_t slot,
                                                char *error, size_t error_size) {
    const XiValue *match = NULL;
    if (!function || (function->nblocks > 0 && !function->blocks))
        return NULL;
    for (uint32_t b = 0; b < function->nblocks; b++) {
        const XiBlock *block = function->blocks[b];
        if (!block || (block->nvalues > 0 && !block->values))
            return NULL;
        for (uint32_t i = 0; i < block->nvalues; i++) {
            const XiValue *value = block->values[i];
            if (!value || value->op != XI_SET_SHARED || value->aux_int != slot)
                continue;
            if (match) {
                verify_fail(error, error_size, "Xi graph import shared store is ambiguous");
                return NULL;
            }
            match = value;
        }
    }
    return match;
}

static bool verify_graph_partition(const XiModule *module, char *error, size_t error_size) {
    if (!module || !module->program_semantic_closure || !module->init ||
        !verify_source_module_exact(module) || module->scalar_call_decision ||
        module->scalar_target_profile ||
        !xr_program_semantic_closure_verify(module->program_semantic_closure, NULL, 0))
        return verify_fail(error, error_size, "Xi graph partition requires a verified PSC");
    const XrProgramSemanticClosure *closure = module->program_semantic_closure;
    if (xr_program_semantic_closure_family(closure) !=
            XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL ||
        xr_program_semantic_closure_module_count(closure) != 2 ||
        xr_program_semantic_closure_dependency_count(closure) != 1 ||
        xr_program_semantic_closure_type_count(closure) != 1 ||
        xr_program_semantic_closure_function_count(closure) != 2 ||
        xr_program_semantic_closure_call_count(closure) != 1 ||
        module->psc_module_index >= 2 || module->init->psc_function_index != XI_PSC_ROW_NONE ||
        module->init->psc_declaration_locator.kind != 0 ||
        !verify_span_empty(module->init->psc_declaration_locator.span) || !module->functions ||
        !module->init->children ||
        module->init->nchildren != module->nfuncs || module->nslots != module->init->nshared)
        return verify_fail(error, error_size, "Xi graph partition inventory is not exact");
    const XrProgramSemanticTypeRecord *i64 = xr_program_semantic_closure_type(closure, 0);
    if (!i64 || i64->kind != XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR ||
        i64->exact_scalar != XR_EXACT_SCALAR_I64)
        return verify_fail(error, error_size, "Xi graph scalar type is not exact i64");

    uint32_t expected_functions = 0;
    uint32_t expected_calls = 0;
    for (uint32_t i = 0; i < xr_program_semantic_closure_function_count(closure); i++) {
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(closure, i);
        expected_functions += graph_function_is_local(closure, module->psc_module_index, row);
    }
    for (uint32_t i = 0; i < xr_program_semantic_closure_call_count(closure); i++) {
        const XrProgramSemanticCallRecord *call = xr_program_semantic_closure_call(closure, i);
        const XrProgramSemanticFunctionRecord *caller = NULL;
        for (uint32_t f = 0; call && f < xr_program_semantic_closure_function_count(closure); f++) {
            const XrProgramSemanticFunctionRecord *candidate =
                xr_program_semantic_closure_function(closure, f);
            if (candidate && verify_same_id(candidate->id, call->caller_function))
                caller = candidate;
        }
        expected_calls += graph_function_is_local(closure, module->psc_module_index, caller);
    }
    if (expected_functions != 1 || module->nfuncs != expected_functions)
        return verify_fail(error, error_size, "Xi graph local function coverage is not exact");

    XiGraphPartitionState state = {.call_index = XI_PSC_ROW_NONE};
    const XrProgramSemanticFunctionRecord *local_row = NULL;
    if (!verify_exact_type_metadata(module, closure, module->init, error, error_size) ||
        !graph_verify_values(module, module->init, &state, error, error_size))
        return false;
    for (uint16_t i = 0; i < module->nfuncs; i++) {
        const XiFunc *function = module->functions[i];
        if (!function || function != module->init->children[i] ||
            function->parent_func != module->init || function->nchildren != 0 ||
            function->psc_function_index >= 2)
            return verify_fail(error, error_size, "Xi graph local function tree is invalid");
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(closure, function->psc_function_index);
        if (!graph_function_is_local(closure, module->psc_module_index, row) ||
            !verify_locator_exact(function->psc_declaration_locator, row->declaration_locator) ||
            !verify_i64(function->return_type))
            return verify_fail(error, error_size, "Xi graph function row join is invalid");
        local_row = row;
        if (row->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY) {
            if (!verify_graph_function_signature_effect(function, 0))
                return verify_fail(error, error_size, "Xi graph entry signature is invalid");
        } else if (row->flags == XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED) {
            const XrProgramSemanticFunctionParameterRecord *parameter =
                xr_program_semantic_closure_function_parameter(closure, row->parameter_begin);
            if (!verify_graph_function_signature_effect(function, 1) || !function->params ||
                !function->params[0] ||
                function->params[0]->op != XI_PARAM ||
                function->params[0]->psc_type_index != 0 ||
                !verify_i64(function->params[0]->type) ||
                function->params[0]->param_mode != XR_PARAM_READ || !parameter ||
                parameter->mode != XR_PARAM_READ || function->inline_policy != XI_INLINE_PRESERVE_CALL)
                return verify_fail(error, error_size, "Xi graph export signature is invalid");
        } else {
            return verify_fail(error, error_size, "Xi graph function role is invalid");
        }
        if (!verify_exact_type_metadata(module, closure, function, error, error_size))
            return false;
        state.function = function;
        if (!graph_verify_values(module, function, &state, error, error_size))
            return false;
    }
    if (state.indexed_call_count != expected_calls ||
        (expected_calls == 1 && (!state.call || state.call_index != 0)) ||
        (expected_calls == 0 && state.call))
        return verify_fail(error, error_size, "Xi graph local call coverage is not exact");
    uint32_t import_count = 0;
    const XiImportRef *partition_import = NULL;
    for (uint16_t slot = 0; slot < module->nslots; slot++) {
        const XiImportRef *ref = module->slot_imports ? module->slot_imports[slot] : NULL;
        if (!ref)
            continue;
        import_count++;
        partition_import = ref;
        const XrProgramSemanticDependencyRecord *dependency =
            xr_program_semantic_closure_dependency(closure, ref->psc_dependency_index);
        if (ref->psc_dependency_index != 0 || !dependency ||
            !verify_locator_exact(ref->psc_import_locator, dependency->import_locator) ||
            !verify_same_id(ref->psc_resolver_binding, dependency->resolver_binding))
            return verify_fail(error, error_size, "Xi graph import inventory is not exact");
    }
    if (import_count != expected_calls || state.import_value_count != expected_calls ||
        state.import_ref != partition_import)
        return verify_fail(error, error_size, "Xi graph import coverage is not exact");
    if (expected_calls == 1) {
        const XiValue *callee = state.call && state.call->nargs > 0 && state.call->args
                                    ? graph_unwrap_identity(state.call->args[0])
                                    : NULL;
        const XiImportRef *call_import =
            state.call && state.call->nargs > 0 && state.call->args
                ? xi_value_import_ref(module->functions[0], state.call->args[0])
                : NULL;
        const XiValue *store =
            callee && callee->op == XI_GET_SHARED && callee->aux_int >= 0 &&
                    callee->aux_int < module->nslots
                ? graph_unique_shared_store(module->init, callee->aux_int, error, error_size)
                : NULL;
        const XiValue *stored =
            store && store->nargs == 1 && store->args ? graph_unwrap_identity(store->args[0])
                                                     : NULL;
        if (module->nexports != 0 || module->exports || call_import != partition_import ||
            !callee || callee->op != XI_GET_SHARED || callee->block == NULL ||
            callee->block->func != module->functions[0] || !store || store->block == NULL ||
            store->block->func != module->init || !stored || stored->op != XI_IMPORT_REF ||
            stored->aux != partition_import || !module->slot_imports ||
            module->slot_imports[callee->aux_int] != partition_import)
            return verify_fail(error, error_size, "Xi graph entry inventory is not exact");
    } else {
        if (!local_row || local_row->flags != XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED ||
            module->nexports != 1 || !module->exports ||
            module->exports[0].function != module->functions[0] ||
            module->exports[0].class_data || module->exports[0].cell_index != -1 ||
            !verify_graph_export_function_type(module->exports[0].value_type,
                                               module->functions[0]) ||
            module->exports[0].is_live_binding ||
            module->exports[0].shared_slot >= module->nslots || !module->slot_funcs ||
            module->slot_funcs[module->exports[0].shared_slot] != module->functions[0])
            return verify_fail(error, error_size, "Xi graph export inventory is not exact");
    }
    return true;
}

bool xi_program_semantic_verify_partition(const XiModule *module,
                                          const XrTargetProfile *target_profile, char *error,
                                          size_t error_size) {
    const XrProgramSemanticClosure *closure = module ? module->program_semantic_closure : NULL;
    if (closure && xr_program_semantic_closure_family(closure) ==
                       XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL) {
        if (target_profile)
            return verify_fail(error, error_size, "Xi graph partition cannot retain target facts");
        return verify_graph_partition(module, error, error_size);
    }
    return verify_single_module_partition(module, target_profile, error, error_size);
}

bool xi_program_semantic_verify_module_set(XiModule *const *module_set, uint32_t module_count,
                                           uint32_t entry_index,
                                           const XrTargetProfile *target_profile, char *error,
                                           size_t error_size) {
    if (!module_set || module_count == 0 || entry_index >= module_count ||
        !module_set[entry_index])
        return verify_fail(error, error_size, "Xi module set entry is incomplete");
    const XiModule *entry = module_set[entry_index];
    if (!xi_program_semantic_verify_partition(entry, target_profile, error, error_size))
        return false;
    const XrProgramSemanticClosure *closure = entry->program_semantic_closure;
    if (xr_program_semantic_closure_family(closure) !=
        XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL) {
        if (module_count != 1 || entry_index != 0)
            return verify_fail(error, error_size,
                               "single-module Xi closure has a foreign module set");
        return true;
    }
    if (target_profile || module_count != 2)
        return verify_fail(error, error_size, "Xi graph module set cardinality is not exact");
    const XiModule *modules[2] = {NULL, NULL};
    uint32_t topo_by_psc[2] = {XI_PSC_ROW_NONE, XI_PSC_ROW_NONE};
    for (uint32_t i = 0; i < 2; i++) {
        const XiModule *candidate = module_set[i];
        if (!candidate || candidate->program_semantic_closure != closure ||
            candidate->psc_module_index >= 2 ||
            modules[candidate->psc_module_index] ||
            !xi_program_semantic_verify_partition(candidate, NULL, error, error_size))
            return verify_fail(error, error_size, "Xi graph module partition is not unique");
        modules[candidate->psc_module_index] = candidate;
        topo_by_psc[candidate->psc_module_index] = i;
    }
    if (!modules[0] || !modules[1])
        return verify_fail(error, error_size, "Xi graph module coverage is incomplete");
    const XrProgramSemanticDependencyRecord *dependency =
        xr_program_semantic_closure_dependency(closure, 0);
    const XrProgramSemanticCallRecord *call = xr_program_semantic_closure_call(closure, 0);
    const XrProgramSemanticFunctionRecord *caller_row = NULL;
    const XrProgramSemanticFunctionRecord *callee_row = NULL;
    uint32_t caller_index = XI_PSC_ROW_NONE;
    uint32_t callee_index = XI_PSC_ROW_NONE;
    uint32_t source_index = XI_PSC_ROW_NONE;
    uint32_t dependency_index = XI_PSC_ROW_NONE;
    for (uint32_t i = 0; dependency && call && i < 2; i++) {
        const XrProgramSemanticModuleRecord *module =
            xr_program_semantic_closure_module(closure, i);
        if (module && verify_same_id(module->module_identity, dependency->source_module))
            source_index = i;
        if (module && verify_same_id(module->module_identity, dependency->dependency_module))
            dependency_index = i;
    }
    for (uint32_t i = 0; call && i < 2; i++) {
        const XrProgramSemanticFunctionRecord *function =
            xr_program_semantic_closure_function(closure, i);
        if (function && verify_same_id(function->id, call->caller_function)) {
            caller_row = function;
            caller_index = i;
        }
        if (function && verify_same_id(function->id, call->callee_function)) {
            callee_row = function;
            callee_index = i;
        }
    }
    if (!caller_row || !callee_row || source_index >= 2 || dependency_index >= 2 ||
        source_index == dependency_index || modules[source_index] != entry ||
        topo_by_psc[source_index] != entry_index ||
        caller_row->flags != XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY ||
        callee_row->flags != XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED)
        return verify_fail(error, error_size, "Xi graph PSC roles are not exact");
    const XiFunc *caller =
        xi_program_semantic_function_for_row(modules[source_index], caller_index);
    const XiFunc *callee =
        xi_program_semantic_function_for_row(modules[dependency_index], callee_index);
    const XiValue *xi_call = xi_program_semantic_call_for_row(caller, 0);
    const XiImportRef *ref = xi_call && xi_call->nargs > 0 && xi_call->args
                                 ? xi_value_import_ref(caller, xi_call->args[0])
                                 : NULL;
    if (!caller || !callee || !xi_call || !ref || ref->psc_dependency_index != 0 ||
        !verify_locator_exact(ref->psc_import_locator, dependency->import_locator) ||
        !verify_same_id(ref->psc_resolver_binding, dependency->resolver_binding) ||
        !ref->resolution_attempted || ref->resolved_mod_index < 0 ||
        (uint32_t) ref->resolved_mod_index != topo_by_psc[dependency_index] ||
        ref->resolved_shared_slot < 0 || ref->resolved_export_slot < 0 ||
        ref->resolved_module != modules[dependency_index] || ref->resolved_func != callee ||
        ref->resolved_shared_slot >= modules[dependency_index]->nslots ||
        !modules[dependency_index]->slot_funcs ||
        modules[dependency_index]->slot_funcs[ref->resolved_shared_slot] != callee ||
        ref->resolved_export_slot >= modules[dependency_index]->nexports ||
        modules[dependency_index]->exports[ref->resolved_export_slot].function != callee ||
        !ref->member_name || ref->member_name[0] == '\0' ||
        !modules[dependency_index]->exports[ref->resolved_export_slot].name ||
        strcmp(modules[dependency_index]->exports[ref->resolved_export_slot].name,
               ref->member_name) != 0 ||
        modules[dependency_index]->exports[ref->resolved_export_slot].shared_slot !=
            ref->resolved_shared_slot)
        return verify_fail(error, error_size, "Xi graph resolved call does not match PSC authority");
    return true;
}

bool xi_program_semantic_verify(const XiModule *module, const XrTargetProfile *target_profile,
                                char *error, size_t error_size) {
    XiModule *module_set[1] = {(XiModule *) module};
    return xi_program_semantic_verify_module_set(module_set, 1, 0, target_profile, error,
                                                 error_size);
}
