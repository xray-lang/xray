/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_program_semantic.c - Mechanical PSC joins and Xi ownership transfer
 */

#include "xi_program_semantic.h"
#include "../frontend/analyzer/xanalyzer_symbol.h"
#include "../base/xmalloc.h"
#include "../runtime/class/xclass_info.h"
#include "../runtime/value/xtype.h"
#include "../shared/xr_exact_scalar_registry.h"
#include <stdio.h>
#include <string.h>

static bool scalar_fail(char *error, size_t error_size, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_SEM_0019: %s", detail);
    return false;
}

static bool same_bytes(const void *left, const void *right, size_t size) {
    return left && right && memcmp(left, right, size) == 0;
}

static bool same_id(XrStableId left, XrStableId right) {
    return same_bytes(left.bytes, right.bytes, sizeof(left.bytes));
}

static bool locator_is_complete(XiSourceLocator locator) {
    XiSourceSpan span = locator.span;
    return locator.kind != 0 && span.start_line != 0 && span.start_column != 0 &&
           span.end_line != 0 && span.end_column != 0 &&
           (span.end_line > span.start_line ||
            (span.end_line == span.start_line && span.end_column > span.start_column));
}

static bool locator_matches(XiSourceLocator xi, XrProgramSemanticSourceLocator psc) {
    return xi.kind == psc.kind && xi.span.start_line == psc.start_line &&
           xi.span.start_column == psc.start_column && xi.span.end_line == psc.end_line &&
           xi.span.end_column == psc.end_column;
}

static const XrProgramSemanticFunctionRecord *find_function(const XrProgramSemanticClosure *closure,
                                                            XrStableId identity, uint32_t *index) {
    const XrProgramSemanticFunctionRecord *answer = NULL;
    size_t count = xr_program_semantic_closure_function_count(closure);
    for (uint32_t i = 0; i < count; i++) {
        const XrProgramSemanticFunctionRecord *candidate =
            xr_program_semantic_closure_function(closure, i);
        if (!candidate || !same_id(candidate->id, identity))
            continue;
        if (answer)
            return NULL;
        answer = candidate;
        if (index)
            *index = i;
    }
    return answer;
}

static bool source_module_matches_psc(const XiModule *module,
                                      const XrProgramSemanticClosure *closure) {
    const XrProgramSemanticModuleRecord *row =
        closure && xr_program_semantic_closure_module_count(closure) == 1
            ? xr_program_semantic_closure_module(closure, 0)
            : NULL;
    const XrProgramSemanticModuleInput *source =
        module && module->source_semantic_module_present ? &module->source_semantic_module : NULL;
    return row && source && same_id(row->module_identity, source->module_identity) &&
           same_bytes(row->module_authority_fingerprint.bytes,
                      source->module_authority_fingerprint.bytes,
                      sizeof(row->module_authority_fingerprint.bytes)) &&
           same_bytes(row->source_fingerprint.bytes, source->source_fingerprint.bytes,
                      sizeof(row->source_fingerprint.bytes)) &&
           same_bytes(row->export_fingerprint.bytes, source->export_fingerprint.bytes,
                      sizeof(row->export_fingerprint.bytes));
}

static bool xr_type_matches_program_type(const XrProgramSemanticClosure *closure,
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
        const XaSymbol *field = info->fields[i];
        const XrProgramSemanticTypeFieldRecord *program_field =
            xr_program_semantic_closure_type_field(closure, row->field_begin + i);
        const XrProgramSemanticTypeRecord *field_type = NULL;
        for (uint32_t t = 0; program_field && t < xr_program_semantic_closure_type_count(closure);
             t++) {
            const XrProgramSemanticTypeRecord *candidate =
                xr_program_semantic_closure_type(closure, t);
            if (candidate && same_id(candidate->id, program_field->field_type)) {
                field_type = candidate;
                break;
            }
        }
        if (!field || field->is_static || field->is_weak || !field_type ||
            program_field->declaration_ordinal != i ||
            !xr_type_matches_program_type(closure, field_type, field->links.type))
            return false;
    }
    return true;
}

static bool class_declaration_for_type(const XiModule *module, const XrType *type,
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

static bool program_type_for_xr_type(const XiModule *module,
                                     const XrProgramSemanticClosure *closure, const XrType *type,
                                     uint32_t *out) {
    if (out)
        *out = XI_PSC_ROW_NONE;
    if (!closure || !type || !out)
        return false;
    const XiClassData *declaration = NULL;
    if (type->kind == XR_KIND_INSTANCE && type->instance.class_ref) {
        if (!class_declaration_for_type(module, type, &declaration))
            return false;
        const XrProgramSemanticTypeRecord *row =
            xr_program_semantic_closure_type(closure, declaration->psc_type_index);
        if (declaration->psc_type_index == XI_PSC_ROW_NONE || !row ||
            row->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE ||
            !locator_matches(declaration->source_locator, row->declaration_locator) ||
            !xr_type_matches_program_type(closure, row, type))
            return false;
        *out = declaration->psc_type_index;
        return true;
    }
    uint32_t match = XI_PSC_ROW_NONE;
    for (uint32_t i = 0; i < xr_program_semantic_closure_type_count(closure); i++) {
        const XrProgramSemanticTypeRecord *row = xr_program_semantic_closure_type(closure, i);
        if (!row || row->kind != XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR)
            continue;
        if (!xr_type_matches_program_type(closure, row, type))
            continue;
        if (match != XI_PSC_ROW_NONE)
            return false;
        match = i;
    }
    *out = match;
    return true;
}

static bool bind_value_type(XiValue *value, const XiModule *module,
                            const XrProgramSemanticClosure *closure) {
    if (!value)
        return false;
    uint32_t row = XI_PSC_ROW_NONE;
    if (!program_type_for_xr_type(module, closure, value->type, &row) ||
        (value->psc_type_index != XI_PSC_ROW_NONE && value->psc_type_index != row))
        return false;
    value->psc_type_index = row;
    return true;
}

static bool bind_function_types(XiFunc *function, const XiModule *module,
                                const XrProgramSemanticClosure *closure) {
    if (!function)
        return false;
    uint32_t result = XI_PSC_ROW_NONE;
    if (!program_type_for_xr_type(module, closure, function->return_type, &result) ||
        (function->psc_return_type_index != XI_PSC_ROW_NONE &&
         function->psc_return_type_index != result))
        return false;
    function->psc_return_type_index = result;
    for (uint16_t p = 0; p < function->nparams; p++)
        if (!bind_value_type(function->params[p], module, closure))
            return false;
    for (uint32_t b = 0; b < function->nblocks; b++) {
        XiBlock *block = function->blocks[b];
        if (!block)
            return false;
        for (XiPhi *phi = block->phis; phi; phi = phi->next)
            if (!bind_value_type(&phi->value, module, closure))
                return false;
        for (uint32_t i = 0; i < block->nvalues; i++)
            if (!bind_value_type(block->values[i], module, closure))
                return false;
    }
    for (uint16_t i = 0; i < function->nchildren; i++)
        if (!bind_function_types(function->children[i], module, closure))
            return false;
    return true;
}

bool xi_program_semantic_input_is_consistent(const XiProgramSemanticInput *input, char *error,
                                             size_t error_size) {
    if (!input || !input->closure || !xr_program_semantic_closure_is_frozen(input->closure) ||
        !xr_program_semantic_closure_is_verified(input->closure) ||
        !xr_program_semantic_closure_verify(input->closure, NULL, 0))
        return scalar_fail(error, error_size, "Xi scalar input requires one verified frozen PSC");
    const XrScalarCallDecision *decision = input->decision;
    XrGenerationClosureId generation = xr_program_semantic_closure_generation_id(input->closure);
    XrFingerprint fingerprint = xr_program_semantic_closure_fingerprint(input->closure);
    const XrProgramSemanticCallRecord *call = xr_program_semantic_closure_call(input->closure, 0);
    size_t type_count = xr_program_semantic_closure_type_count(input->closure);
    XrProgramSemanticFamily family = xr_program_semantic_closure_family(input->closure);
    bool scalar = family == XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL;
    bool aggregate = false;
    if (family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL) {
        uint32_t aggregate_count = 0;
        for (uint32_t i = 0; i < type_count; i++) {
            const XrProgramSemanticTypeRecord *type =
                xr_program_semantic_closure_type(input->closure, i);
            if (!type || (type->kind != XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR &&
                          type->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE))
                return scalar_fail(error, error_size,
                                   "Xi program input has an unsupported PSC type row");
            if (type->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE)
                aggregate_count++;
        }
        aggregate = aggregate_count == 1 &&
                    xr_program_semantic_closure_type_field_count(input->closure) > 0;
    }
    if ((!scalar && (!aggregate || decision)) ||
        (scalar && (!decision || decision->schema != XR_SCALAR_CALL_DECISION_SCHEMA_VERSION ||
                    decision->sealed != 1)) ||
        xr_program_semantic_closure_module_count(input->closure) != 1 ||
        xr_program_semantic_closure_dependency_count(input->closure) != 0 ||
        xr_program_semantic_closure_function_count(input->closure) != 2 ||
        xr_program_semantic_closure_call_count(input->closure) != 1 || !call ||
        (scalar &&
         (!same_bytes(decision->generation_id.bytes, generation.bytes, sizeof(generation.bytes)) ||
          !same_bytes(decision->closure_fingerprint.bytes, fingerprint.bytes,
                      sizeof(fingerprint.bytes)) ||
          !same_id(decision->call_identity, call->id) ||
          !same_id(decision->callsite_identity, call->callsite_identity) ||
          !same_id(decision->caller_function, call->caller_function) ||
          !same_id(decision->callee_function, call->callee_function))) ||
        !find_function(input->closure, call->caller_function, NULL) ||
        !find_function(input->closure, call->callee_function, NULL))
        return scalar_fail(error, error_size, "Xi scalar input decision does not bind its PSC");
    return true;
}

bool xi_program_semantic_bind_function(XiFunc *function, const XiProgramSemanticInput *input,
                                       XiSourceLocator locator, char *error, size_t error_size) {
    if (!function || !locator_is_complete(locator) ||
        function->psc_function_index != XI_PSC_ROW_NONE ||
        !xi_source_span_is_empty(function->psc_declaration_locator.span) ||
        function->psc_declaration_locator.kind != 0 ||
        !xi_program_semantic_input_is_consistent(input, NULL, 0))
        return scalar_fail(error, error_size, "Xi function row join input is incomplete");
    uint32_t match = XI_PSC_ROW_NONE;
    size_t count = xr_program_semantic_closure_function_count(input->closure);
    for (uint32_t i = 0; i < count; i++) {
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(input->closure, i);
        if (!row || !locator_matches(locator, row->declaration_locator))
            continue;
        if (match != XI_PSC_ROW_NONE)
            return scalar_fail(error, error_size, "PSC declaration locator is ambiguous");
        match = i;
    }
    if (match == XI_PSC_ROW_NONE)
        return scalar_fail(error, error_size, "Xi declaration has no exact PSC function row");
    function->psc_function_index = match;
    function->psc_declaration_locator = locator;
    return true;
}

bool xi_program_semantic_find_call(const XiFunc *caller, const XiProgramSemanticInput *input,
                                   XiSourceLocator locator, uint32_t *call_index, char *error,
                                   size_t error_size) {
    if (call_index)
        *call_index = XI_PSC_ROW_NONE;
    if (!caller || !call_index || !locator_is_complete(locator) ||
        caller->psc_function_index == XI_PSC_ROW_NONE ||
        !xi_program_semantic_input_is_consistent(input, NULL, 0))
        return scalar_fail(error, error_size, "Xi call row join input is incomplete");
    const XrProgramSemanticFunctionRecord *caller_row =
        xr_program_semantic_closure_function(input->closure, caller->psc_function_index);
    if (!caller_row)
        return scalar_fail(error, error_size, "Xi caller function row is out of range");
    uint32_t match = XI_PSC_ROW_NONE;
    size_t count = xr_program_semantic_closure_call_count(input->closure);
    for (uint32_t i = 0; i < count; i++) {
        const XrProgramSemanticCallRecord *row =
            xr_program_semantic_closure_call(input->closure, i);
        if (!row || !same_id(row->caller_function, caller_row->id) ||
            !locator_matches(locator, row->locator))
            continue;
        if (match != XI_PSC_ROW_NONE)
            return scalar_fail(error, error_size, "PSC call locator is ambiguous in its caller");
        match = i;
    }
    if (match == XI_PSC_ROW_NONE)
        return scalar_fail(error, error_size, "Xi call has no exact PSC call row");
    *call_index = match;
    return true;
}

static bool bind_aggregate_class(XiModule *module, const XrProgramSemanticClosure *closure,
                                 char *error, size_t error_size) {
    if (xr_program_semantic_closure_family(closure) !=
        XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL)
        return true;
    uint32_t aggregate_row = XI_PSC_ROW_NONE;
    const XrProgramSemanticTypeRecord *aggregate = NULL;
    for (uint32_t i = 0; i < xr_program_semantic_closure_type_count(closure); i++) {
        const XrProgramSemanticTypeRecord *row = xr_program_semantic_closure_type(closure, i);
        if (!row || row->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE)
            continue;
        if (aggregate)
            return scalar_fail(error, error_size, "aggregate PSC type row is ambiguous");
        aggregate = row;
        aggregate_row = i;
    }
    XiClassData *match = NULL;
    for (uint16_t i = 0; module && i < module->nclasses; i++) {
        XiClassData *candidate = module->classes ? module->classes[i] : NULL;
        if (!candidate || candidate->psc_type_index != XI_PSC_ROW_NONE)
            return scalar_fail(error, error_size, "Xi class PSC binding is not empty");
        if (!aggregate ||
            !locator_matches(candidate->source_locator, aggregate->declaration_locator))
            continue;
        if (match)
            return scalar_fail(error, error_size, "aggregate class declaration is ambiguous");
        match = candidate;
    }
    if (!aggregate || aggregate_row == XI_PSC_ROW_NONE || !match)
        return scalar_fail(error, error_size, "aggregate class declaration has no PSC row");
    match->psc_type_index = aggregate_row;
    return true;
}

bool xi_program_semantic_finalize(XiFunc *root, const XiProgramSemanticInput *input, char *error,
                                  size_t error_size) {
    if (!root || !root->module || root->module->init != root ||
        root->psc_function_index != XI_PSC_ROW_NONE ||
        !xi_program_semantic_input_is_consistent(input, NULL, 0) ||
        !source_module_matches_psc(root->module, input->closure))
        return scalar_fail(error, error_size, "Xi scalar function tree is incomplete");
    const XrProgramSemanticCallRecord *call = xr_program_semantic_closure_call(input->closure, 0);
    XiFunc *callee = NULL;
    bool seen[2] = {false, false};
    if (!call || root->module->nfuncs != 2)
        return scalar_fail(error, error_size, "Xi scalar function inventory is not exact");
    for (uint16_t i = 0; i < root->module->nfuncs; i++) {
        XiFunc *function = root->module->functions[i];
        if (!function || function->psc_function_index >= 2 || seen[function->psc_function_index])
            return scalar_fail(error, error_size,
                               "Xi scalar function rows are incomplete or duplicated");
        seen[function->psc_function_index] = true;
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(input->closure, function->psc_function_index);
        if (!row || !locator_matches(function->psc_declaration_locator, row->declaration_locator))
            return scalar_fail(error, error_size, "Xi function locator does not match its PSC row");
        if (same_id(row->id, call->callee_function)) {
            if (callee)
                return scalar_fail(error, error_size, "Xi scalar callee row is duplicated");
            callee = function;
        }
    }
    if (!seen[0] || !seen[1] || !callee)
        return scalar_fail(error, error_size,
                           "Xi scalar callee cannot be selected by PSC identity");
    callee->inline_policy = XI_INLINE_PRESERVE_CALL;
    if (!bind_aggregate_class(root->module, input->closure, error, error_size))
        return false;
    return bind_function_types(root, root->module, input->closure) ||
           scalar_fail(error, error_size, "Xi PSC type bindings are incomplete");
}

bool xi_module_take_program_semantics(XiModule *module, XrProgramSemanticClosure **closure,
                                      const XrScalarCallDecision *decision,
                                      const XrTargetProfile *target_profile, char *error,
                                      size_t error_size) {
    if (!module || !closure || !*closure || module->program_semantic_closure ||
        module->scalar_call_decision || module->scalar_target_profile ||
        !source_module_matches_psc(module, *closure)) {
        return scalar_fail(error, error_size, "Xi scalar authority ownership transfer is invalid");
    }
    XiProgramSemanticInput input = {*closure, decision};
    bool scalar = xr_program_semantic_closure_family(*closure) ==
                  XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL;
    bool aggregate = xr_program_semantic_closure_family(*closure) ==
                     XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL;
    if (!xi_program_semantic_input_is_consistent(&input, error, error_size) ||
        (scalar &&
         (!target_profile || !xr_scalar_call_decision_verify(decision, *closure, target_profile,
                                                             error, error_size))) ||
        (!aggregate && !scalar) || (aggregate && (decision || target_profile)))
        return false;
    if (!scalar) {
        module->program_semantic_closure = *closure;
        *closure = NULL;
        return true;
    }
    XrScalarCallDecision *owned = (XrScalarCallDecision *) xr_malloc(sizeof(*owned));
    if (!owned)
        return scalar_fail(error, error_size, "Xi scalar decision ownership allocation failed");
    XrTargetProfile *retained_profile =
        xr_target_profile_retain((XrTargetProfile *) target_profile);
    if (!retained_profile) {
        xr_free(owned);
        return scalar_fail(error, error_size, "Xi scalar target profile retention failed");
    }
    *owned = *decision;
    module->program_semantic_closure = *closure;
    module->scalar_call_decision = owned;
    module->scalar_target_profile = retained_profile;
    *closure = NULL;
    return true;
}
