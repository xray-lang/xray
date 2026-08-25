/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_scalar_program_verify.c - Independent PSC/CallDecision to Xi verifier
 */

#include "xi_scalar_program.h"
#include "xi_core_api.h"
#include "xi_effect.h"
#include "../plan/target/xr_target_profile.h"
#include "../runtime/value/xtype.h"
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
    return span.start_line == 0 && span.start_column == 0 &&
           span.end_line == 0 && span.end_column == 0;
}

static bool verify_locator_exact(XiSourceLocator xi,
                                 XrProgramSemanticSourceLocator psc) {
    return xi.kind != 0 && xi.kind == psc.kind &&
           xi.span.start_line != 0 &&
           xi.span.start_line == psc.start_line &&
           xi.span.start_column != 0 &&
           xi.span.start_column == psc.start_column &&
           xi.span.end_line != 0 && xi.span.end_line == psc.end_line &&
           xi.span.end_column != 0 &&
           xi.span.end_column == psc.end_column &&
           (xi.span.end_line > xi.span.start_line ||
            (xi.span.end_line == xi.span.start_line &&
             xi.span.end_column > xi.span.start_column));
}

static bool verify_i64(const XrType *type) {
    return type && type->kind == XR_KIND_INT && !type->is_nullable &&
           type->scalar_rep == XR_NATIVE_I64;
}

static const XiFunc *verify_physical_callee(const XiModule *module,
                                            const XiValue *value) {
    const XiValue *cursor = value;
    while (cursor && cursor->op == XI_COPY &&
           cursor->aux_int == XI_COPY_KIND_IDENTITY && cursor->nargs == 1)
        cursor = cursor->args[0];
    if (!cursor)
        return NULL;
    if (cursor->op == XI_CLOSURE_NEW && cursor->aux)
        return (const XiFunc *) cursor->aux;
    if (cursor->op == XI_STACK_ALLOC && cursor->aux_int == XI_CLOSURE_NEW &&
        cursor->aux)
        return (const XiFunc *) cursor->aux;
    if (cursor->op == XI_GET_SHARED && cursor->aux_int >= 0 && module &&
        module->slot_funcs && cursor->aux_int < module->nslots)
        return module->slot_funcs[cursor->aux_int];
    return NULL;
}

static bool verify_value(XiScalarVerifyState *state, const XiFunc *owner,
                         const XiValue *value, char *error,
                         size_t error_size) {
    if (!value)
        return verify_fail(error, error_size,
                           "Xi scalar value inventory contains NULL");
    if (value->op == XI_CALL)
        state->xi_call_count++;
    if (value->psc_call_index == XI_PSC_ROW_NONE) {
        if (value->op == XI_CALL)
            return verify_fail(error, error_size,
                               "bounded Xi call has no PSC call row");
        return true;
    }
    state->indexed_call_count++;
    if (state->indexed_call_count != 1 || value->op != XI_CALL ||
        value->psc_call_index >=
            xr_program_semantic_closure_call_count(state->closure))
        return verify_fail(error, error_size,
                           "Xi PSC call row is duplicated or out of range");
    const XrProgramSemanticCallRecord *row =
        xr_program_semantic_closure_call(state->closure,
                                         value->psc_call_index);
    if (!row || row != state->call_row || owner != state->caller ||
        owner->psc_function_index == XI_PSC_ROW_NONE)
        return verify_fail(error, error_size,
                           "Xi PSC call row has the wrong containing caller");
    const XrProgramSemanticFunctionRecord *owner_row =
        xr_program_semantic_closure_function(state->closure,
                                             owner->psc_function_index);
    XiSourceLocator source = {
        .kind = value->source_kind,
        .span = value->source_span,
    };
    if (!owner_row || !verify_same_id(owner_row->id, row->caller_function) ||
        !verify_locator_exact(source, row->locator) || value->nargs != 2 ||
        !value->args || !value->args[0] || !value->args[1] ||
        !verify_i64(value->type) || !verify_i64(value->args[1]->type) ||
        verify_physical_callee(state->module, value->args[0]) != state->callee)
        return verify_fail(error, error_size,
                           "Xi call does not implement the exact PSC call row");
    if (value->flags != xi_op_default_effects(XI_CALL) ||
        value->aux_int != 0 ||
        value->call_plan || value->xg_callsite_id != 0 ||
        value->error_region ||
        value->call_return_ownership.kind != XI_RETURN_OWNERSHIP_UNKNOWN ||
        value->call_return_ownership.param_index != -1 ||
        value->call_return_ownership.complete ||
        value->result_alias_operand != -1)
        return verify_fail(error, error_size,
                           "Xi scalar call adds an unapproved execution contract");
    state->call = value;
    return true;
}

static bool verify_function_values(XiScalarVerifyState *state,
                                   const XiFunc *function, char *error,
                                   size_t error_size) {
    if (!function)
        return verify_fail(error, error_size,
                           "Xi scalar function inventory contains NULL");
    for (uint32_t b = 0; b < function->nblocks; b++) {
        const XiBlock *block = function->blocks[b];
        if (!block)
            return verify_fail(error, error_size,
                               "Xi scalar block inventory contains NULL");
        for (const XiPhi *phi = block->phis; phi; phi = phi->next) {
            if (!verify_value(state, function, &phi->value, error, error_size))
                return false;
        }
        for (uint32_t i = 0; i < block->nvalues; i++) {
            if (!verify_value(state, function, block->values[i], error,
                              error_size))
                return false;
        }
    }
    return true;
}

bool xi_scalar_program_verify(const XiModule *module,
                              const XrTargetProfile *target_profile,
                              char *error, size_t error_size) {
    if (!module || !module->program_semantic_closure ||
        !module->scalar_call_decision || !target_profile || !module->init ||
        !xr_program_semantic_closure_verify(
            module->program_semantic_closure, NULL, 0) ||
        !xr_target_profile_verify(target_profile, NULL, 0) ||
        !xr_scalar_call_decision_verify(
            module->scalar_call_decision,
            module->program_semantic_closure, target_profile, NULL, 0))
        return verify_fail(error, error_size,
                           "Xi scalar verification requires exact authorities");
    const XrProgramSemanticClosure *closure =
        module->program_semantic_closure;
    const XrScalarCallDecision *decision = module->scalar_call_decision;
    if (xr_program_semantic_closure_schema(closure) !=
            XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION ||
        xr_program_semantic_closure_module_count(closure) != 1 ||
        xr_program_semantic_closure_dependency_count(closure) != 0 ||
        xr_program_semantic_closure_type_count(closure) != 0 ||
        xr_program_semantic_closure_function_count(closure) != 2 ||
        xr_program_semantic_closure_call_count(closure) != 1 ||
        module->nfuncs != 2 || !module->functions ||
        module->init->nchildren != 2 ||
        module->init->psc_function_index != XI_PSC_ROW_NONE ||
        module->init->psc_declaration_locator.kind != 0 ||
        !verify_span_empty(module->init->psc_declaration_locator.span))
        return verify_fail(error, error_size,
                           "Xi scalar module inventory is not exact");

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
            function->psc_function_index >= 2 ||
            seen[function->psc_function_index])
            return verify_fail(error, error_size,
                               "Xi scalar function row inventory is invalid");
        seen[function->psc_function_index] = true;
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(
                closure, function->psc_function_index);
        if (!row || !verify_locator_exact(function->psc_declaration_locator,
                                          row->declaration_locator) ||
            !verify_i64(function->return_type))
            return verify_fail(error, error_size,
                               "Xi function does not match its exact PSC row");
        if (verify_same_id(row->id, decision->caller_function)) {
            if (state.caller || function->nparams != 0 ||
                row->flags != XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY)
                return verify_fail(error, error_size,
                                   "Xi scalar caller contract is invalid");
            state.caller = function;
        }
        if (verify_same_id(row->id, decision->callee_function)) {
            if (state.callee || function->nparams != 1 || !function->params ||
                !function->params[0] || function->params[0]->op != XI_PARAM ||
                !verify_i64(function->params[0]->type) ||
                function->params[0]->param_mode != XR_PARAM_READ ||
                row->flags != 0 ||
                function->inline_policy != XI_INLINE_PRESERVE_CALL)
                return verify_fail(error, error_size,
                                   "Xi scalar callee contract is invalid");
            state.callee = function;
        }
    }
    if (!seen[0] || !seen[1] || !state.caller || !state.callee ||
        state.caller == state.callee || !state.call_row ||
        !verify_same_id(state.call_row->id, decision->call_identity) ||
        !verify_same_id(state.call_row->callsite_identity,
                        decision->callsite_identity) ||
        !verify_same_id(state.call_row->caller_function,
                        decision->caller_function) ||
        !verify_same_id(state.call_row->callee_function,
                        decision->callee_function))
        return verify_fail(error, error_size,
                           "Xi scalar identities do not match the decision");

    if (!verify_function_values(&state, module->init, error, error_size))
        return false;
    for (uint16_t i = 0; i < module->nfuncs; i++) {
        if (!verify_function_values(&state, module->functions[i], error,
                                    error_size))
            return false;
    }
    if (state.indexed_call_count != 1 || state.xi_call_count != 1 ||
        !state.call)
        return verify_fail(error, error_size,
                           "Xi scalar call row coverage is not exact");
    return true;
}
