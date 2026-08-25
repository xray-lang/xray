/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_scalar_program.c - Mechanical PSC joins and Xi ownership transfer
 */

#include "xi_scalar_program.h"
#include "../base/xmalloc.h"
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
    return locator.kind != 0 && span.start_line != 0 &&
           span.start_column != 0 && span.end_line != 0 &&
           span.end_column != 0 &&
           (span.end_line > span.start_line ||
            (span.end_line == span.start_line &&
             span.end_column > span.start_column));
}

static bool locator_matches(XiSourceLocator xi,
                            XrProgramSemanticSourceLocator psc) {
    return xi.kind == psc.kind && xi.span.start_line == psc.start_line &&
           xi.span.start_column == psc.start_column &&
           xi.span.end_line == psc.end_line &&
           xi.span.end_column == psc.end_column;
}

static const XrProgramSemanticFunctionRecord *find_function(
    const XrProgramSemanticClosure *closure, XrStableId identity,
    uint32_t *index) {
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

bool xi_scalar_program_input_is_consistent(const XiScalarProgramInput *input,
                                           char *error,
                                           size_t error_size) {
    if (!input || !input->closure || !input->decision ||
        !xr_program_semantic_closure_is_frozen(input->closure) ||
        !xr_program_semantic_closure_is_verified(input->closure) ||
        !xr_program_semantic_closure_verify(input->closure, NULL, 0))
        return scalar_fail(error, error_size,
                           "Xi scalar input requires one verified frozen PSC");
    const XrScalarCallDecision *decision = input->decision;
    XrGenerationClosureId generation =
        xr_program_semantic_closure_generation_id(input->closure);
    XrFingerprint fingerprint =
        xr_program_semantic_closure_fingerprint(input->closure);
    const XrProgramSemanticCallRecord *call =
        xr_program_semantic_closure_call(input->closure, 0);
    if (decision->schema != XR_SCALAR_CALL_DECISION_SCHEMA_VERSION ||
        decision->sealed != 1 ||
        xr_program_semantic_closure_module_count(input->closure) != 1 ||
        xr_program_semantic_closure_dependency_count(input->closure) != 0 ||
        xr_program_semantic_closure_type_count(input->closure) != 0 ||
        xr_program_semantic_closure_function_count(input->closure) != 2 ||
        xr_program_semantic_closure_call_count(input->closure) != 1 || !call ||
        !same_bytes(decision->generation_id.bytes, generation.bytes,
                    sizeof(generation.bytes)) ||
        !same_bytes(decision->closure_fingerprint.bytes, fingerprint.bytes,
                    sizeof(fingerprint.bytes)) ||
        !same_id(decision->call_identity, call->id) ||
        !same_id(decision->callsite_identity, call->callsite_identity) ||
        !same_id(decision->caller_function, call->caller_function) ||
        !same_id(decision->callee_function, call->callee_function) ||
        !find_function(input->closure, call->caller_function, NULL) ||
        !find_function(input->closure, call->callee_function, NULL))
        return scalar_fail(error, error_size,
                           "Xi scalar input decision does not bind its PSC");
    return true;
}

bool xi_scalar_program_bind_function(XiFunc *function,
                                     const XiScalarProgramInput *input,
                                     XiSourceLocator locator, char *error,
                                     size_t error_size) {
    if (!function || !locator_is_complete(locator) ||
        function->psc_function_index != XI_PSC_ROW_NONE ||
        !xi_source_span_is_empty(function->psc_declaration_locator.span) ||
        function->psc_declaration_locator.kind != 0 ||
        !xi_scalar_program_input_is_consistent(input, NULL, 0))
        return scalar_fail(error, error_size,
                           "Xi function row join input is incomplete");
    uint32_t match = XI_PSC_ROW_NONE;
    size_t count = xr_program_semantic_closure_function_count(input->closure);
    for (uint32_t i = 0; i < count; i++) {
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(input->closure, i);
        if (!row || !locator_matches(locator, row->declaration_locator))
            continue;
        if (match != XI_PSC_ROW_NONE)
            return scalar_fail(error, error_size,
                               "PSC declaration locator is ambiguous");
        match = i;
    }
    if (match == XI_PSC_ROW_NONE)
        return scalar_fail(error, error_size,
                           "Xi declaration has no exact PSC function row");
    function->psc_function_index = match;
    function->psc_declaration_locator = locator;
    return true;
}

bool xi_scalar_program_find_call(const XiFunc *caller,
                                 const XiScalarProgramInput *input,
                                 XiSourceLocator locator, uint32_t *call_index,
                                 char *error, size_t error_size) {
    if (call_index)
        *call_index = XI_PSC_ROW_NONE;
    if (!caller || !call_index || !locator_is_complete(locator) ||
        caller->psc_function_index == XI_PSC_ROW_NONE ||
        !xi_scalar_program_input_is_consistent(input, NULL, 0))
        return scalar_fail(error, error_size,
                           "Xi call row join input is incomplete");
    const XrProgramSemanticFunctionRecord *caller_row =
        xr_program_semantic_closure_function(input->closure,
                                             caller->psc_function_index);
    if (!caller_row)
        return scalar_fail(error, error_size,
                           "Xi caller function row is out of range");
    uint32_t match = XI_PSC_ROW_NONE;
    size_t count = xr_program_semantic_closure_call_count(input->closure);
    for (uint32_t i = 0; i < count; i++) {
        const XrProgramSemanticCallRecord *row =
            xr_program_semantic_closure_call(input->closure, i);
        if (!row || !same_id(row->caller_function, caller_row->id) ||
            !locator_matches(locator, row->locator))
            continue;
        if (match != XI_PSC_ROW_NONE)
            return scalar_fail(error, error_size,
                               "PSC call locator is ambiguous in its caller");
        match = i;
    }
    if (match == XI_PSC_ROW_NONE)
        return scalar_fail(error, error_size,
                           "Xi call has no exact PSC call row");
    *call_index = match;
    return true;
}

bool xi_scalar_program_finalize(XiFunc *root,
                                const XiScalarProgramInput *input, char *error,
                                size_t error_size) {
    if (!root || !root->module || root->module->init != root ||
        root->psc_function_index != XI_PSC_ROW_NONE ||
        !xi_scalar_program_input_is_consistent(input, NULL, 0))
        return scalar_fail(error, error_size,
                           "Xi scalar function tree is incomplete");
    const XrProgramSemanticCallRecord *call =
        xr_program_semantic_closure_call(input->closure, 0);
    XiFunc *callee = NULL;
    bool seen[2] = {false, false};
    if (!call || root->module->nfuncs != 2)
        return scalar_fail(error, error_size,
                           "Xi scalar function inventory is not exact");
    for (uint16_t i = 0; i < root->module->nfuncs; i++) {
        XiFunc *function = root->module->functions[i];
        if (!function || function->psc_function_index >= 2 ||
            seen[function->psc_function_index])
            return scalar_fail(error, error_size,
                               "Xi scalar function rows are incomplete or duplicated");
        seen[function->psc_function_index] = true;
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(
                input->closure, function->psc_function_index);
        if (!row || !locator_matches(function->psc_declaration_locator,
                                     row->declaration_locator))
            return scalar_fail(error, error_size,
                               "Xi function locator does not match its PSC row");
        if (same_id(row->id, call->callee_function)) {
            if (callee)
                return scalar_fail(error, error_size,
                                   "Xi scalar callee row is duplicated");
            callee = function;
        }
    }
    if (!seen[0] || !seen[1] || !callee)
        return scalar_fail(error, error_size,
                           "Xi scalar callee cannot be selected by PSC identity");
    callee->inline_policy = XI_INLINE_PRESERVE_CALL;
    return true;
}

bool xi_module_take_scalar_program(XiModule *module,
                                   XrProgramSemanticClosure **closure,
                                   const XrScalarCallDecision *decision,
                                   const XrTargetProfile *target_profile,
                                   char *error, size_t error_size) {
    if (!module || !closure || !*closure || !decision || !target_profile ||
        module->program_semantic_closure || module->scalar_call_decision ||
        module->scalar_target_profile) {
        return scalar_fail(error, error_size,
                           "Xi scalar authority ownership transfer is invalid");
    }
    XiScalarProgramInput input = {*closure, decision};
    if (!xi_scalar_program_input_is_consistent(&input, error, error_size) ||
        !xr_scalar_call_decision_verify(decision, *closure, target_profile,
                                        error, error_size))
        return false;
    XrScalarCallDecision *owned =
        (XrScalarCallDecision *) xr_malloc(sizeof(*owned));
    if (!owned)
        return scalar_fail(error, error_size,
                           "Xi scalar decision ownership allocation failed");
    XrTargetProfile *retained_profile =
        xr_target_profile_retain((XrTargetProfile *) target_profile);
    if (!retained_profile) {
        xr_free(owned);
        return scalar_fail(error, error_size,
                           "Xi scalar target profile retention failed");
    }
    *owned = *decision;
    module->program_semantic_closure = *closure;
    module->scalar_call_decision = owned;
    module->scalar_target_profile = retained_profile;
    *closure = NULL;
    return true;
}
