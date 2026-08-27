/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Independent Xi consumer for the sealed i64 overflow-predicate table.
 */

#include "xi_i64_overflow_program.h"
#include "xi_program_semantic.h"
#include "xi_core_api.h"
#include "xi_effect.h"
#include "../plan/target/xr_target_profile.h"
#include "../runtime/value/xtype.h"
#include "../shared/xr_exact_scalar_registry.h"
#include "../base/xmalloc.h"
#include <stdio.h>
#include <string.h>

static bool fail(char *error, size_t error_size, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_SEM_0019: %s", detail);
    return false;
}

static bool same_id(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static bool span_empty(XiSourceSpan span) {
    return span.start_line == 0 && span.start_column == 0 && span.end_line == 0 &&
           span.end_column == 0;
}

static bool locator_exact(const XiValue *value,
                          XrProgramSemanticSourceLocator locator) {
    return value && value->source_kind == locator.kind && locator.kind != 0 &&
           value->source_span.start_line == locator.start_line && locator.start_line != 0 &&
           value->source_span.start_column == locator.start_column && locator.start_column != 0 &&
           value->source_span.end_line == locator.end_line && locator.end_line != 0 &&
           value->source_span.end_column == locator.end_column && locator.end_column != 0;
}

static bool function_locator_exact(const XiFunc *function,
                                   XrProgramSemanticSourceLocator locator) {
    return function && function->psc_declaration_locator.kind == locator.kind &&
           locator.kind != 0 &&
           function->psc_declaration_locator.span.start_line == locator.start_line &&
           function->psc_declaration_locator.span.start_column == locator.start_column &&
           function->psc_declaration_locator.span.end_line == locator.end_line &&
           function->psc_declaration_locator.span.end_column == locator.end_column;
}

static bool exact_i64(const XrType *type) {
    return type && type->kind == XR_KIND_INT && !type->is_nullable && !type->is_const &&
           !type->is_literal && type->scalar_rep == XR_NATIVE_I64;
}

static bool exact_bool(const XrType *type) {
    return type && type->kind == XR_KIND_BOOL && !type->is_nullable && !type->is_const &&
           !type->is_literal &&
           (type->scalar_rep == XR_SCALAR_REP_NONE || type->scalar_rep == XR_NATIVE_BOOL);
}

static bool exact_function(const XiFunc *function) {
    return function && function->nparams == 2 && function->min_params == 2 &&
           function->params && function->params[0] && function->params[1] &&
           function->params[0]->op == XI_PARAM && function->params[1]->op == XI_PARAM &&
           function->params[0]->param_mode == XR_PARAM_READ &&
           function->params[1]->param_mode == XR_PARAM_READ &&
           exact_i64(function->params[0]->type) && exact_i64(function->params[1]->type) &&
           exact_i64(function->return_type) && !function->is_vararg && !function->is_extern &&
           !function->is_generic_template && function->error_effect_nothrow &&
           function->analyzer_effect_complete && function->semantic_effects == 0 &&
           function->unknown_semantic_effects == 0 && function->effect_unknown_reasons == 0 &&
           !function->contains_unsafe_op && !function->requires_unsafe_at_call &&
           function->entry_type == 0;
}

static bool call_contract_exact(const XiValue *value) {
    return value && value->op == XI_CALL_METHOD && value->nargs == 2 && value->args &&
           value->args[0] && value->args[1] && !value->aux &&
           value->aux_kind == XI_AUX_KIND_NONE && value->lowering_flags == 0 &&
           value->flags == xi_op_default_effects(XI_CALL_METHOD) && !value->call_plan &&
           value->xg_callsite_id == 0 && !value->error_region && value->transfer_mode == 0 &&
           value->param_mode == XR_PARAM_READ &&
           value->call_return_ownership.kind == XI_RETURN_OWNERSHIP_UNKNOWN &&
           value->call_return_ownership.param_index == -1 &&
           !value->call_return_ownership.complete && value->result_alias_operand == -1;
}

static bool verify_values(const XiFunc *owner, const XrProgramSemanticClosure *closure,
                          const XrI64OverflowDecisionTable *table, bool *seen,
                          uint32_t *indexed_count, char *error, size_t error_size) {
    for (uint32_t b = 0; owner && b < owner->nblocks; b++) {
        const XiBlock *block = owner->blocks ? owner->blocks[b] : NULL;
        if (!block || (block->nvalues && !block->values))
            return fail(error, error_size, "overflow Xi block inventory is incomplete");
        for (const XiPhi *phi = block->phis; phi; phi = phi->next)
            if (phi->value.psc_call_index != XI_PSC_ROW_NONE ||
                phi->value.op == XI_CALL || phi->value.op == XI_CALL_METHOD)
                return fail(error, error_size, "overflow Xi phi carries call authority");
        for (uint32_t i = 0; i < block->nvalues; i++) {
            const XiValue *value = block->values[i];
            if (!value)
                return fail(error, error_size, "overflow Xi value inventory contains NULL");
            if (value->psc_call_index == XI_PSC_ROW_NONE) {
                if (value->op == XI_CALL || value->op == XI_CALL_METHOD)
                    return fail(error, error_size, "overflow Xi call lacks its PSC row");
                continue;
            }
            uint32_t row_index = value->psc_call_index;
            const XrProgramSemanticCallRecord *call =
                xr_program_semantic_closure_call(closure, row_index);
            const XrI64OverflowDecisionRow *decision =
                row_index < table->row_count && table->rows ? &table->rows[row_index] : NULL;
            const XrProgramSemanticFunctionRecord *function =
                owner->psc_function_index != XI_PSC_ROW_NONE
                    ? xr_program_semantic_closure_function(closure,
                                                           owner->psc_function_index)
                    : NULL;
            if (!call || !decision || !function || seen[row_index] ||
                !same_id(function->id, call->caller_function) ||
                !same_id(decision->program_call, call->id) ||
                !same_id(decision->callsite, call->callsite_identity) ||
                !same_id(decision->caller_function, call->caller_function) ||
                !same_id(decision->builtin_identity, call->callee_function) ||
                !locator_exact(value, call->locator) || !call_contract_exact(value) ||
                value->aux_int != ((int64_t) decision->method_symbol << 1) ||
                decision->receiver_rep != XR_MACHINE_REP_I64 ||
                decision->argument_rep != XR_MACHINE_REP_I64 ||
                decision->result_rep != XR_MACHINE_REP_I1 || !exact_i64(value->args[0]->type) ||
                !exact_i64(value->args[1]->type) || !exact_bool(value->type) ||
                value->args[0]->psc_type_index != 0 || value->args[1]->psc_type_index != 0 ||
                value->psc_type_index != XI_PSC_ROW_NONE)
                return fail(error, error_size, "overflow Xi call does not match its decision row");
            seen[row_index] = true;
            (*indexed_count)++;
        }
    }
    return true;
}

bool xi_i64_overflow_program_verify(const XiModule *module,
                                     const XrTargetProfile *target_profile, char *error,
                                     size_t error_size) {
    const XrProgramSemanticClosure *closure = module ? module->program_semantic_closure : NULL;
    const XrI64OverflowDecisionTable *table =
        module ? module->i64_overflow_decisions : NULL;
    if (!module || !closure || !table || !target_profile || !module->scalar_target_profile ||
        !xr_target_profile_require_exact(module->scalar_target_profile, target_profile, NULL, 0) ||
        !xr_i64_overflow_decision_verify(table, closure, target_profile, NULL, 0) ||
        xr_program_semantic_closure_family(closure) !=
            XR_PROGRAM_SEMANTIC_FAMILY_I64_OVERFLOW_PREDICATE ||
        xr_program_semantic_closure_module_count(closure) != 1 ||
        xr_program_semantic_closure_dependency_count(closure) != 0 ||
        xr_program_semantic_closure_type_count(closure) != 1 ||
        xr_program_semantic_closure_function_count(closure) != 1 ||
        xr_program_semantic_closure_call_count(closure) == 0 || module->nfuncs != 1 ||
        !module->functions || !module->init || module->init->nchildren != 1 ||
        module->init->children[0] != module->functions[0] ||
        module->init->psc_function_index != XI_PSC_ROW_NONE ||
        module->init->psc_return_type_index != XI_PSC_ROW_NONE ||
        module->init->psc_declaration_locator.kind != 0 ||
        !span_empty(module->init->psc_declaration_locator.span))
        return fail(error, error_size, "overflow Xi authorities are incomplete");
    const XrProgramSemanticTypeRecord *i64 = xr_program_semantic_closure_type(closure, 0);
    const XrProgramSemanticFunctionRecord *function_row =
        xr_program_semantic_closure_function(closure, 0);
    const XiFunc *function = module->functions[0];
    if (!i64 || i64->kind != XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR ||
        i64->exact_scalar != XR_EXACT_SCALAR_I64 || !function_row ||
        function_row->flags != XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY ||
        function->parent_func != module->init || function->psc_function_index != 0 ||
        function->psc_return_type_index != 0 || !exact_function(function) ||
        function->params[0]->psc_type_index != 0 ||
        function->params[1]->psc_type_index != 0 ||
        !function_locator_exact(function, function_row->declaration_locator))
        return fail(error, error_size, "overflow Xi function authority is incomplete");
    bool *seen = (bool *) xr_calloc(table->row_count, sizeof(*seen));
    uint32_t indexed_count = 0;
    if (!seen)
        return fail(error, error_size, "overflow Xi verification allocation failed");
    bool ok = verify_values(module->init, closure, table, seen, &indexed_count, error,
                            error_size) &&
              verify_values(function, closure, table, seen, &indexed_count, error,
                            error_size);
    for (uint32_t i = 0; ok && i < table->row_count; i++)
        ok = seen[i];
    xr_free(seen);
    return (ok && indexed_count == table->row_count) ||
           fail(error, error_size, "overflow Xi call coverage is incomplete");
}
