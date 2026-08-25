/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_program_semantic_closure.c - Mechanical scalar snapshot projection
 */

#include "xa_program_semantic_closure.h"
#include "xa_scalar_program_authority.h"
#include "xa_typed_program.h"
#include <stdio.h>
#include <string.h>

static bool bridge_fail(char *error, size_t error_size, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_SEM_0019: %s", detail);
    return false;
}

static bool stable_id_equal(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

static int find_function(const XaScalarProgramAuthority *authority,
                         XrStableId declaration, XrStableId instance) {
    int found = -1;
    for (uint32_t i = 0; i < XA_SCALAR_PROGRAM_FUNCTION_COUNT; i++) {
        const XaScalarFunctionAuthority *row =
            xa_scalar_program_authority_function(authority, i);
        if (!row || !stable_id_equal(row->declaration_identity, declaration) ||
            !stable_id_equal(row->concrete_instance_identity, instance))
            continue;
        if (found >= 0)
            return -1;
        found = (int) i;
    }
    return found;
}

bool xa_typed_program_build_scalar_closure(const XaTypedProgram *typed_program,
                                           XrProgramSemanticClosure **out,
                                           char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!out || !xa_typed_program_is_verified(typed_program))
        return bridge_fail(error, error_size,
                           "scalar closure requires a verified typed publication");
    const XaScalarProgramAuthority *authority =
        xa_typed_program_scalar_authority(typed_program);
    if (!authority ||
        !xa_scalar_program_authority_verify(authority, error, error_size))
        return false;
    const XaScalarModuleAuthority *module =
        xa_scalar_program_authority_module(authority);
    const XaScalarCallAuthority *call = xa_scalar_program_authority_call(authority);
    if (!module || !call)
        return bridge_fail(error, error_size, "scalar authority rows are incomplete");
    int caller = find_function(authority, call->caller_declaration_identity,
                               call->caller_instance_identity);
    int callee = find_function(authority, call->callee_declaration_identity,
                               call->callee_instance_identity);
    if (caller < 0 || callee < 0 || caller == callee)
        return bridge_fail(error, error_size,
                           "scalar call does not resolve to one exact function pair");

    XrProgramSemanticClosureLimits limits = {
        .max_modules = 1,
        .max_dependencies = 0,
        .max_types = 0,
        .max_functions = XA_SCALAR_PROGRAM_FUNCTION_COUNT,
        .max_calls = 1,
    };
    XrProgramSemanticClosure *closure = NULL;
    if (!xr_program_semantic_closure_create(
            &limits, xa_scalar_program_authority_policy(authority), &closure,
            error, error_size))
        return false;
    XrProgramSemanticModuleInput module_input = {
        .module_identity = module->module_identity,
        .source_fingerprint = module->source_fingerprint,
        .export_fingerprint = module->export_fingerprint,
    };
    XrStableId function_ids[XA_SCALAR_PROGRAM_FUNCTION_COUNT] = {0};
    bool ok = xr_program_semantic_closure_add_module(
        closure, &module_input, error, error_size);
    for (uint32_t i = 0; ok && i < XA_SCALAR_PROGRAM_FUNCTION_COUNT; i++) {
        const XaScalarFunctionAuthority *row =
            xa_scalar_program_authority_function(authority, i);
        XrProgramSemanticFunctionInput input = {
            .module_identity = module->module_identity,
            .declaration_identity = row->declaration_identity,
            .concrete_instance_identity = row->concrete_instance_identity,
            .signature_fingerprint = row->signature_fingerprint,
            .effect_fingerprint = row->effect_fingerprint,
            .capability_mask = row->capability_mask,
            .flags = (row->flags & XA_SCALAR_PROGRAM_FUNCTION_ENTRY)
                         ? XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY
                         : 0,
        };
        ok = xr_program_semantic_closure_add_function(
            closure, &input, &function_ids[i], error, error_size);
    }
    if (ok) {
        XrProgramSemanticCallInput input = {
            .callsite_identity = call->callsite_identity,
            .caller_function = function_ids[caller],
            .callee_function = function_ids[callee],
            .contract_fingerprint = call->contract_fingerprint,
        };
        XrStableId call_id;
        ok = xr_program_semantic_closure_add_call(
            closure, &input, &call_id, error, error_size);
    }
    ok = ok && xr_program_semantic_closure_freeze(closure, error, error_size) &&
         xr_program_semantic_closure_verify(closure, error, error_size);
    if (!ok) {
        xr_program_semantic_closure_free(closure);
        return false;
    }
    *out = closure;
    return true;
}
