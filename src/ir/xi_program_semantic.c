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
#include "xi_effect.h"
#include "../frontend/analyzer/xanalyzer_symbol.h"
#include "../base/xmalloc.h"
#include "../runtime/class/xclass_info.h"
#include "../runtime/value/xtype.h"
#include "../shared/xr_exact_scalar_registry.h"
#include "../plan/semantic/xr_source_semantic_identity.h"
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
                                      const XrProgramSemanticClosure *closure,
                                      uint32_t module_index) {
    const XrProgramSemanticModuleRecord *row =
        closure ? xr_program_semantic_closure_module(closure, module_index) : NULL;
    const XrProgramSemanticModuleInput *source =
        module && module->source_semantic_module_present ? &module->source_semantic_module : NULL;
    bool graph = closure && (xr_program_semantic_closure_family(closure) ==
                                 XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL ||
                             xr_program_semantic_closure_family(closure) ==
                                 XR_PROGRAM_SEMANTIC_FAMILY_SOURCE_MODULE_SCALAR_PRIVATE_LEAF_CALL);
    return row && source && same_id(row->module_identity, source->module_identity) &&
           same_bytes(row->module_authority_fingerprint.bytes,
                      source->module_authority_fingerprint.bytes,
                      sizeof(row->module_authority_fingerprint.bytes)) &&
           same_bytes(row->source_fingerprint.bytes, source->source_fingerprint.bytes,
                      sizeof(row->source_fingerprint.bytes)) &&
           (graph || same_bytes(row->export_fingerprint.bytes, source->export_fingerprint.bytes,
                                sizeof(row->export_fingerprint.bytes)));
}

static bool module_identity_matches_source(const XiModule *module) {
    const XrProgramSemanticModuleInput *source =
        module && module->source_semantic_module_present ? &module->source_semantic_module : NULL;
    XrProgramSemanticModuleInput rebuilt = {0};
    return source && module->identity &&
           xr_source_semantic_module_authority(module->identity, source->source_fingerprint,
                                               &rebuilt, NULL) &&
           same_id(rebuilt.module_identity, source->module_identity) &&
           same_bytes(rebuilt.module_authority_fingerprint.bytes,
                      source->module_authority_fingerprint.bytes,
                      sizeof(rebuilt.module_authority_fingerprint.bytes));
}

static bool stable_id_is_zero(XrStableId id) {
    static const XrStableId zero = {{0}};
    return same_id(id, zero);
}

static bool source_private_leaf_family(const XrProgramSemanticClosure *closure) {
    return closure && xr_program_semantic_closure_family(closure) ==
                          XR_PROGRAM_SEMANTIC_FAMILY_SOURCE_MODULE_SCALAR_PRIVATE_LEAF_CALL;
}

static bool function_belongs_to_module(const XrProgramSemanticClosure *closure,
                                       const XrProgramSemanticFunctionRecord *function,
                                       uint32_t module_index) {
    const XrProgramSemanticModuleRecord *module =
        closure ? xr_program_semantic_closure_module(closure, module_index) : NULL;
    return module && function && same_id(function->module_identity, module->module_identity);
}

static const XrProgramSemanticTypeRecord *find_type(const XrProgramSemanticClosure *closure,
                                                    XrStableId identity, uint32_t *index) {
    const XrProgramSemanticTypeRecord *answer = NULL;
    for (uint32_t i = 0; closure && i < xr_program_semantic_closure_type_count(closure); i++) {
        const XrProgramSemanticTypeRecord *candidate = xr_program_semantic_closure_type(closure, i);
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
    if (row->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT) {
        if (xr_program_semantic_closure_family(closure) !=
                XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL ||
            type->kind != XR_KIND_TUPLE || row->field_count != 6 ||
            row->field_begin > xr_program_semantic_closure_type_field_count(closure) ||
            row->field_count >
                xr_program_semantic_closure_type_field_count(closure) - row->field_begin ||
            xr_type_tuple_count((XrType *) type) != (int) row->field_count)
            return false;
        for (uint32_t i = 0; i < row->field_count; i++) {
            const XrProgramSemanticTypeFieldRecord *field =
                xr_program_semantic_closure_type_field(closure, row->field_begin + i);
            const XrProgramSemanticTypeRecord *child =
                field ? find_type(closure, field->field_type, NULL) : NULL;
            XrType *member = xr_type_tuple_get((XrType *) type, (int) i);
            if (!field || field->declaration_ordinal != i || !child || !member ||
                !xr_type_matches_program_type(closure, child, member))
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
        if (!row || (row->kind != XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR &&
                     row->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT))
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

bool xi_program_semantic_input_prepare(const XrProgramSemanticClosure *closure,
                                       const XrScalarCallDecision *decision,
                                       const XrI64OverflowDecisionTable *overflow_decisions,
                                       const XrTargetProfile *target_profile,
                                       const XrProgramSemanticModuleInput *source_module,
                                       XiProgramSemanticInput *out, char *error,
                                       size_t error_size) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!closure || !source_module || !out)
        return scalar_fail(error, error_size, "Xi input selection authority is incomplete");
    XrProgramSemanticFamily family = xr_program_semantic_closure_family(closure);
    bool graph = family == XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL ||
                 family == XR_PROGRAM_SEMANTIC_FAMILY_SOURCE_MODULE_SCALAR_PRIVATE_LEAF_CALL;
    uint32_t match = XI_PSC_ROW_NONE;
    for (uint32_t i = 0; i < xr_program_semantic_closure_module_count(closure); i++) {
        const XrProgramSemanticModuleRecord *row = xr_program_semantic_closure_module(closure, i);
        if (!row || !same_id(row->module_identity, source_module->module_identity) ||
            !same_bytes(row->module_authority_fingerprint.bytes,
                        source_module->module_authority_fingerprint.bytes,
                        sizeof(row->module_authority_fingerprint.bytes)) ||
            !same_bytes(row->source_fingerprint.bytes, source_module->source_fingerprint.bytes,
                        sizeof(row->source_fingerprint.bytes)) ||
            (!graph &&
             !same_bytes(row->export_fingerprint.bytes, source_module->export_fingerprint.bytes,
                         sizeof(row->export_fingerprint.bytes))))
            continue;
        if (match != XI_PSC_ROW_NONE)
            return scalar_fail(error, error_size, "Xi source module PSC row is ambiguous");
        match = i;
    }
    if (match == XI_PSC_ROW_NONE)
        return scalar_fail(error, error_size, "Xi source module has no exact PSC partition");
    *out = (XiProgramSemanticInput) {
        .closure = closure,
        .decision = decision,
        .overflow_decisions = overflow_decisions,
        .target_profile = target_profile,
        .module_index = match,
    };
    if (!xi_program_semantic_input_is_consistent(out, error, error_size)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

bool xi_program_semantic_input_is_consistent(const XiProgramSemanticInput *input, char *error,
                                             size_t error_size) {
    if (!input || !input->closure || !xr_program_semantic_closure_is_frozen(input->closure) ||
        !xr_program_semantic_closure_is_verified(input->closure) ||
        !xr_program_semantic_closure_verify(input->closure, NULL, 0))
        return scalar_fail(error, error_size, "Xi input requires one verified frozen PSC");
    const XrScalarCallDecision *decision = input->decision;
    const XrI64OverflowDecisionTable *overflow_decisions = input->overflow_decisions;
    XrGenerationClosureId generation = xr_program_semantic_closure_generation_id(input->closure);
    XrFingerprint fingerprint = xr_program_semantic_closure_fingerprint(input->closure);
    const XrProgramSemanticCallRecord *call = xr_program_semantic_closure_call(input->closure, 0);
    size_t type_count = xr_program_semantic_closure_type_count(input->closure);
    XrProgramSemanticFamily family = xr_program_semantic_closure_family(input->closure);
    bool scalar = family == XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL;
    bool graph = family == XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL;
    bool private_leaf = family == XR_PROGRAM_SEMANTIC_FAMILY_SOURCE_MODULE_SCALAR_PRIVATE_LEAF_CALL;
    bool product = family == XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL;
    bool overflow = family == XR_PROGRAM_SEMANTIC_FAMILY_I64_OVERFLOW_PREDICATE;
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
    size_t module_count = xr_program_semantic_closure_module_count(input->closure);
    size_t function_count = xr_program_semantic_closure_function_count(input->closure);
    size_t call_count = xr_program_semantic_closure_call_count(input->closure);
    if ((!scalar && !graph && !private_leaf && !product && !overflow && (!aggregate || decision)) ||
        ((graph || private_leaf || product || aggregate) && (decision || overflow_decisions)) ||
        (overflow && (decision || !overflow_decisions || !input->target_profile ||
                      !xr_i64_overflow_decision_verify(overflow_decisions, input->closure,
                                                       input->target_profile, NULL, 0))) ||
        (scalar && (!decision || decision->schema != XR_SCALAR_CALL_DECISION_SCHEMA_VERSION ||
                    decision->sealed != 1 || overflow_decisions)) ||
        input->module_index >= module_count ||
        ((scalar || aggregate || product || overflow) &&
         (module_count != 1 ||
          xr_program_semantic_closure_dependency_count(input->closure) != 0)) ||
        (graph &&
         (module_count != 2 || xr_program_semantic_closure_dependency_count(input->closure) != 1 ||
          type_count != 1 || !xr_program_semantic_closure_type(input->closure, 0) ||
          xr_program_semantic_closure_type(input->closure, 0)->kind !=
              XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR ||
          xr_program_semantic_closure_type(input->closure, 0)->exact_scalar !=
              XR_EXACT_SCALAR_I64)) ||
        (private_leaf &&
         (module_count != 2 || xr_program_semantic_closure_dependency_count(input->closure) != 1 ||
          type_count != 1 || function_count != 2 ||
          xr_program_semantic_closure_function_parameter_count(input->closure) != 0 ||
          call_count != 2 || !xr_program_semantic_closure_type(input->closure, 0) ||
          xr_program_semantic_closure_type(input->closure, 0)->kind !=
              XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR ||
          xr_program_semantic_closure_type(input->closure, 0)->exact_scalar !=
              XR_EXACT_SCALAR_I64)) ||
        (product &&
         (type_count != 3 || xr_program_semantic_closure_type_field_count(input->closure) != 6 ||
          function_count != 3 || call_count != 2)) ||
        (overflow && (type_count != 1 || function_count != 1 || call_count == 0 ||
                      !xr_program_semantic_closure_type(input->closure, 0) ||
                      xr_program_semantic_closure_type(input->closure, 0)->kind !=
                          XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR ||
                      xr_program_semantic_closure_type(input->closure, 0)->exact_scalar !=
                          XR_EXACT_SCALAR_I64 ||
                      overflow_decisions->row_count != call_count)) ||
        (!product && !overflow && !private_leaf &&
         (function_count != 2 || call_count != 1 || !call)) ||
        (scalar &&
         (!same_bytes(decision->generation_id.bytes, generation.bytes, sizeof(generation.bytes)) ||
          !same_bytes(decision->closure_fingerprint.bytes, fingerprint.bytes,
                      sizeof(fingerprint.bytes)) ||
          !same_id(decision->call_identity, call->id) ||
          !same_id(decision->callsite_identity, call->callsite_identity) ||
          !same_id(decision->caller_function, call->caller_function) ||
          !same_id(decision->callee_function, call->callee_function))) ||
        (!product && !overflow && !private_leaf &&
         (!find_function(input->closure, call->caller_function, NULL) ||
          !find_function(input->closure, call->callee_function, NULL))))
        return scalar_fail(error, error_size, "Xi input does not bind its PSC partition");
    if (private_leaf) {
        const XrProgramSemanticFunctionRecord *entry = NULL;
        const XrProgramSemanticFunctionRecord *wrapper = NULL;
        const XrProgramSemanticCallRecord *source_call = NULL;
        const XrProgramSemanticCallRecord *leaf_call = NULL;
        const XrProgramSemanticDependencyRecord *dependency =
            xr_program_semantic_closure_dependency(input->closure, 0);
        const XrProgramSemanticTypeRecord *i64 =
            xr_program_semantic_closure_type(input->closure, 0);
        for (uint32_t i = 0; i < function_count; i++) {
            const XrProgramSemanticFunctionRecord *row =
                xr_program_semantic_closure_function(input->closure, i);
            if (!row || row->parameter_count != 0 || !same_id(row->return_type, i64->id))
                return scalar_fail(error, error_size,
                                   "Xi private-leaf function authority is not nullary i64");
            if (row->flags == XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY)
                entry = entry ? NULL : row;
            else if (row->flags == XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED)
                wrapper = wrapper ? NULL : row;
            else
                return scalar_fail(error, error_size,
                                   "Xi private-leaf function role is unsupported");
        }
        for (uint32_t i = 0; i < call_count; i++) {
            const XrProgramSemanticCallRecord *row =
                xr_program_semantic_closure_call(input->closure, i);
            if (!row)
                return scalar_fail(error, error_size,
                                   "Xi private-leaf call authority is incomplete");
            if (find_function(input->closure, row->callee_function, NULL))
                source_call = source_call ? NULL : row;
            else
                leaf_call = leaf_call ? NULL : row;
        }
        if (!entry || !wrapper || !source_call || !leaf_call || !dependency ||
            dependency->kind != XR_PROGRAM_SEMANTIC_DEPENDENCY_SOURCE_MODULE_EDGE ||
            !same_id(source_call->caller_function, entry->id) ||
            !same_id(source_call->callee_function, wrapper->id) ||
            stable_id_is_zero(source_call->resolver_binding) ||
            !same_id(source_call->resolver_binding, dependency->resolver_binding) ||
            !same_id(leaf_call->caller_function, wrapper->id) ||
            stable_id_is_zero(leaf_call->callee_function) ||
            !stable_id_is_zero(leaf_call->resolver_binding))
            return scalar_fail(error, error_size,
                               "Xi private-leaf source and native calls are not exact");
    }
    if (product) {
        uint32_t product_rows = 0;
        for (uint32_t i = 0; i < type_count; i++) {
            const XrProgramSemanticTypeRecord *type =
                xr_program_semantic_closure_type(input->closure, i);
            if (!type || (type->kind != XR_PROGRAM_SEMANTIC_TYPE_EXACT_SCALAR &&
                          type->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT))
                return scalar_fail(error, error_size, "Xi product input has an unsupported type");
            product_rows += type->kind == XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT;
        }
        for (uint32_t i = 0; i < call_count; i++) {
            const XrProgramSemanticCallRecord *product_call =
                xr_program_semantic_closure_call(input->closure, i);
            if (!product_call ||
                !find_function(input->closure, product_call->caller_function, NULL) ||
                !find_function(input->closure, product_call->callee_function, NULL))
                return scalar_fail(error, error_size, "Xi product call join is incomplete");
        }
        if (product_rows != 1)
            return scalar_fail(error, error_size, "Xi product type authority is not unique");
    }
    if (overflow) {
        for (uint32_t i = 0; i < call_count; i++) {
            const XrProgramSemanticCallRecord *overflow_call =
                xr_program_semantic_closure_call(input->closure, i);
            const XrI64OverflowDecisionRow *row =
                xr_i64_overflow_decision_for_program_row(overflow_decisions, i);
            if (!overflow_call || !row ||
                !find_function(input->closure, overflow_call->caller_function, NULL) ||
                !same_id(row->program_call, overflow_call->id) ||
                !same_id(row->caller_function, overflow_call->caller_function) ||
                !same_id(row->builtin_identity, overflow_call->callee_function))
                return scalar_fail(error, error_size, "Xi overflow decision rows are not exact");
        }
    }
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
    const XrProgramSemanticModuleRecord *module_row =
        xr_program_semantic_closure_module(input->closure, input->module_index);
    size_t count = xr_program_semantic_closure_function_count(input->closure);
    for (uint32_t i = 0; i < count; i++) {
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(input->closure, i);
        if (!row || !module_row || !same_id(row->module_identity, module_row->module_identity) ||
            !locator_matches(locator, row->declaration_locator))
            continue;
        if (match != XI_PSC_ROW_NONE)
            return scalar_fail(error, error_size, "PSC declaration locator is ambiguous");
        match = i;
    }
    if (match == XI_PSC_ROW_NONE && source_private_leaf_family(input->closure))
        return true;
    if (match == XI_PSC_ROW_NONE)
        return scalar_fail(error, error_size, "Xi declaration has no exact PSC function row");
    function->psc_function_index = match;
    function->psc_declaration_locator = locator;
    return true;
}

bool xi_program_semantic_bind_import(XiImportRef *ref, const XiProgramSemanticInput *input,
                                     uint32_t call_index, char *error, size_t error_size) {
    if (!ref || ref->psc_dependency_index != XI_PSC_ROW_NONE ||
        !locator_is_complete(ref->psc_import_locator) ||
        !stable_id_is_zero(ref->psc_resolver_binding) ||
        !xi_program_semantic_input_is_consistent(input, NULL, 0) ||
        (xr_program_semantic_closure_family(input->closure) !=
             XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL &&
         !source_private_leaf_family(input->closure)))
        return scalar_fail(error, error_size, "Xi import authority input is incomplete");
    const XrProgramSemanticCallRecord *call =
        xr_program_semantic_closure_call(input->closure, call_index);
    const XrProgramSemanticModuleRecord *source =
        xr_program_semantic_closure_module(input->closure, input->module_index);
    uint32_t match = XI_PSC_ROW_NONE;
    for (uint32_t i = 0;
         call && source && i < xr_program_semantic_closure_dependency_count(input->closure); i++) {
        const XrProgramSemanticDependencyRecord *dependency =
            xr_program_semantic_closure_dependency(input->closure, i);
        bool source_edge = source_private_leaf_family(input->closure);
        if (!dependency || !same_id(dependency->source_module, source->module_identity) ||
            !locator_matches(ref->psc_import_locator, dependency->import_locator) ||
            (source_edge ? (dependency->kind != XR_PROGRAM_SEMANTIC_DEPENDENCY_SOURCE_MODULE_EDGE ||
                            !stable_id_is_zero(dependency->exported_declaration) ||
                            !stable_id_is_zero(dependency->exported_function))
                         : !same_id(dependency->exported_function, call->callee_function)) ||
            !same_id(dependency->resolver_binding, call->resolver_binding))
            continue;
        if (match != XI_PSC_ROW_NONE)
            return scalar_fail(error, error_size, "PSC import resolver binding is ambiguous");
        match = i;
    }
    if (match == XI_PSC_ROW_NONE)
        return scalar_fail(error, error_size, "Xi import has no exact PSC dependency row");
    ref->psc_dependency_index = match;
    ref->psc_resolver_binding = call->resolver_binding;
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

static bool canonicalize_value_product_function(XiFunc *function,
                                                const XrProgramSemanticClosure *closure) {
    if (!function || !closure)
        return false;
    uint32_t product_index = XI_PSC_ROW_NONE;
    const XrProgramSemanticTypeRecord *product = NULL;
    for (uint32_t i = 0; i < xr_program_semantic_closure_type_count(closure); i++) {
        const XrProgramSemanticTypeRecord *row = xr_program_semantic_closure_type(closure, i);
        if (!row || row->kind != XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT)
            continue;
        if (product)
            return false;
        product = row;
        product_index = i;
    }
    if (!product || product_index == XI_PSC_ROW_NONE || product->field_count != 6)
        return false;
    for (uint32_t b = 0; b < function->nblocks; b++) {
        XiBlock *block = function->blocks[b];
        if (!block)
            return false;
        for (uint32_t v = 0; v < block->nvalues; v++) {
            XiValue *value = block->values[v];
            if (!value)
                return false;
            if (value->op == XI_TUPLE_NEW && value->psc_type_index == product_index) {
                if (value->nargs != product->field_count || !value->args ||
                    (value->aux_int & XI_TUPLE_AUX_ARITY_MASK) != (int64_t) product->field_count ||
                    value->aux || value->aux_kind != XI_AUX_KIND_NONE || value->call_plan ||
                    value->xg_callsite_id != 0 || value->error_region ||
                    value->transfer_mode != 0 || value->param_mode != XR_PARAM_READ ||
                    value->call_return_ownership.kind != XI_RETURN_OWNERSHIP_UNKNOWN ||
                    value->call_return_ownership.param_index != -1 ||
                    value->call_return_ownership.complete || value->result_alias_operand != -1)
                    return false;
                for (uint32_t i = 0; i < product->field_count; i++) {
                    const XrProgramSemanticTypeFieldRecord *field =
                        xr_program_semantic_closure_type_field(closure, product->field_begin + i);
                    uint32_t member_index = XI_PSC_ROW_NONE;
                    if (!field || field->declaration_ordinal != i || !value->args[i] ||
                        !find_type(closure, field->field_type, &member_index) ||
                        value->args[i]->psc_type_index != member_index)
                        return false;
                }
                value->op = XI_VALUE_PRODUCT_CONSTRUCT;
                value->aux_int = (int64_t) product->field_count;
                value->aux = NULL;
                value->aux_kind = XI_AUX_KIND_NONE;
                value->lowering_flags = 0;
                value->flags = xi_op_default_effects(XI_VALUE_PRODUCT_CONSTRUCT);
            } else if (value->op == XI_TUPLE_GET && value->nargs == 1 && value->args &&
                       value->args[0] && value->args[0]->psc_type_index == product_index) {
                if (value->aux_int < 0 || (uint64_t) value->aux_int >= product->field_count ||
                    value->aux || value->aux_kind != XI_AUX_KIND_NONE || value->call_plan ||
                    value->xg_callsite_id != 0 || value->error_region ||
                    value->transfer_mode != 0 || value->param_mode != XR_PARAM_READ ||
                    value->call_return_ownership.kind != XI_RETURN_OWNERSHIP_UNKNOWN ||
                    value->call_return_ownership.param_index != -1 ||
                    value->call_return_ownership.complete || value->result_alias_operand != -1)
                    return false;
                uint32_t ordinal = (uint32_t) value->aux_int;
                const XrProgramSemanticTypeFieldRecord *field =
                    xr_program_semantic_closure_type_field(closure, product->field_begin + ordinal);
                uint32_t member_index = XI_PSC_ROW_NONE;
                if (!field || field->declaration_ordinal != ordinal ||
                    !find_type(closure, field->field_type, &member_index) ||
                    value->psc_type_index != member_index)
                    return false;
                value->op = XI_VALUE_PRODUCT_PROJECT;
                value->aux = NULL;
                value->aux_kind = XI_AUX_KIND_NONE;
                value->lowering_flags = 0;
                value->flags = xi_op_default_effects(XI_VALUE_PRODUCT_PROJECT);
            }
        }
    }
    for (uint16_t i = 0; i < function->nchildren; i++)
        if (!canonicalize_value_product_function(function->children[i], closure))
            return false;
    return true;
}

bool xi_program_semantic_finalize(XiFunc *root, const XiProgramSemanticInput *input, char *error,
                                  size_t error_size) {
    if (!root || !root->module || root->module->init != root ||
        root->psc_function_index != XI_PSC_ROW_NONE ||
        !xi_program_semantic_input_is_consistent(input, NULL, 0) ||
        !source_module_matches_psc(root->module, input->closure, input->module_index))
        return scalar_fail(error, error_size, "Xi function partition is incomplete");
    uint32_t function_count = (uint32_t) xr_program_semantic_closure_function_count(input->closure);
    bool *seen = (bool *) xr_calloc(function_count, sizeof(*seen));
    uint32_t local_count = 0;
    bool private_leaf = source_private_leaf_family(input->closure);
    XiFunc *preserved_callee = NULL;
    if (!seen)
        return scalar_fail(error, error_size, "Xi function partition allocation failed");
    for (uint32_t i = 0; i < function_count; i++) {
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(input->closure, i);
        local_count += function_belongs_to_module(input->closure, row, input->module_index);
    }
    if ((!private_leaf && root->module->nfuncs != local_count) ||
        (private_leaf && root->module->nfuncs < local_count)) {
        xr_free(seen);
        return scalar_fail(error, error_size, "Xi function partition inventory is not exact");
    }
    for (uint16_t i = 0; i < root->module->nfuncs; i++) {
        XiFunc *function = root->module->functions[i];
        if (!function) {
            xr_free(seen);
            return scalar_fail(error, error_size,
                               "Xi function partition rows are incomplete or duplicated");
        }
        if (function->psc_function_index == XI_PSC_ROW_NONE && private_leaf) {
            if (function->psc_declaration_locator.kind != 0 ||
                !xi_source_span_is_empty(function->psc_declaration_locator.span)) {
                xr_free(seen);
                return scalar_fail(error, error_size,
                                   "Xi function outside the capability carries PSC authority");
            }
            continue;
        }
        if (function->psc_function_index >= function_count || seen[function->psc_function_index]) {
            xr_free(seen);
            return scalar_fail(error, error_size,
                               "Xi function partition rows are incomplete or duplicated");
        }
        seen[function->psc_function_index] = true;
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(input->closure, function->psc_function_index);
        if (!function_belongs_to_module(input->closure, row, input->module_index) ||
            !locator_matches(function->psc_declaration_locator, row->declaration_locator)) {
            xr_free(seen);
            return scalar_fail(error, error_size, "Xi function locator does not match its PSC row");
        }
        for (uint32_t c = 0; c < xr_program_semantic_closure_call_count(input->closure); c++) {
            const XrProgramSemanticCallRecord *call =
                xr_program_semantic_closure_call(input->closure, c);
            if (call && same_id(row->id, call->callee_function))
                preserved_callee = function;
        }
    }
    for (uint32_t i = 0; i < function_count; i++) {
        const XrProgramSemanticFunctionRecord *row =
            xr_program_semantic_closure_function(input->closure, i);
        if (function_belongs_to_module(input->closure, row, input->module_index) && !seen[i]) {
            xr_free(seen);
            return scalar_fail(error, error_size, "Xi function partition coverage is incomplete");
        }
    }
    xr_free(seen);
    if (preserved_callee)
        preserved_callee->inline_policy = XI_INLINE_PRESERVE_CALL;
    if (!bind_aggregate_class(root->module, input->closure, error, error_size))
        return false;
    if (private_leaf) {
        for (uint16_t i = 0; i < root->module->nfuncs; i++) {
            XiFunc *function = root->module->functions[i];
            if (function && function->psc_function_index != XI_PSC_ROW_NONE &&
                !bind_function_types(function, root->module, input->closure))
                return scalar_fail(error, error_size, "Xi PSC type bindings are incomplete");
        }
    } else if (!bind_function_types(root, root->module, input->closure)) {
        return scalar_fail(error, error_size, "Xi PSC type bindings are incomplete");
    }
    if (xr_program_semantic_closure_family(input->closure) ==
            XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL &&
        !canonicalize_value_product_function(root, input->closure))
        return scalar_fail(error, error_size, "Xi value-product operations are not canonical");
    return true;
}

const XiFunc *xi_program_semantic_function_for_row(const XiModule *module,
                                                   uint32_t program_function) {
    const XiFunc *match = NULL;
    for (uint16_t i = 0; module && i < module->nfuncs; i++) {
        const XiFunc *candidate = module->functions ? module->functions[i] : NULL;
        if (!candidate || candidate->psc_function_index != program_function)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

const XiValue *xi_program_semantic_call_for_row(const XiFunc *function, uint32_t program_call) {
    const XiValue *match = NULL;
    for (uint32_t block_index = 0; function && block_index < function->nblocks; block_index++) {
        const XiBlock *block = function->blocks[block_index];
        for (uint32_t value_index = 0; block && value_index < block->nvalues; value_index++) {
            const XiValue *candidate = block->values[value_index];
            if (!candidate || candidate->psc_call_index != program_call)
                continue;
            if (match)
                return NULL;
            match = candidate;
        }
    }
    return match;
}

bool xi_module_take_program_semantics(XiModule *module, XrProgramSemanticClosure **closure,
                                      const XrScalarCallDecision *decision,
                                      const XrI64OverflowDecisionTable *overflow_decisions,
                                      const XrTargetProfile *target_profile, uint32_t module_index,
                                      char *error, size_t error_size) {
    if (!module || !closure || !*closure || module->program_semantic_closure ||
        module->psc_module_index != XI_PSC_ROW_NONE || module->scalar_call_decision ||
        module->i64_overflow_decisions || module->scalar_target_profile ||
        !module_identity_matches_source(module) ||
        !source_module_matches_psc(module, *closure, module_index)) {
        return scalar_fail(error, error_size, "Xi authority ownership transfer is invalid");
    }
    XiProgramSemanticInput input = {
        .closure = *closure,
        .decision = decision,
        .overflow_decisions = overflow_decisions,
        .target_profile = target_profile,
        .module_index = module_index,
    };
    bool scalar = xr_program_semantic_closure_family(*closure) ==
                  XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL;
    bool aggregate = xr_program_semantic_closure_family(*closure) ==
                     XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL;
    bool product = xr_program_semantic_closure_family(*closure) ==
                   XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL;
    bool graph = xr_program_semantic_closure_family(*closure) ==
                 XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL;
    bool private_leaf = source_private_leaf_family(*closure);
    bool overflow = xr_program_semantic_closure_family(*closure) ==
                    XR_PROGRAM_SEMANTIC_FAMILY_I64_OVERFLOW_PREDICATE;
    if (!xi_program_semantic_input_is_consistent(&input, error, error_size) ||
        (scalar &&
         (!target_profile || !xr_scalar_call_decision_verify(decision, *closure, target_profile,
                                                             error, error_size))) ||
        (overflow && (!target_profile ||
                      !xr_i64_overflow_decision_verify(overflow_decisions, *closure, target_profile,
                                                       error, error_size))) ||
        (!aggregate && !product && !scalar && !graph && !private_leaf && !overflow) ||
        ((aggregate || product || graph || private_leaf) &&
         (decision || overflow_decisions || target_profile)))
        return false;
    if (!scalar && !overflow) {
        module->program_semantic_closure = *closure;
        module->psc_module_index = module_index;
        *closure = NULL;
        return true;
    }
    if (overflow) {
        XrI64OverflowDecisionTable *owned =
            (XrI64OverflowDecisionTable *) xr_calloc(1, sizeof(*owned));
        XrI64OverflowDecisionRow *rows =
            (XrI64OverflowDecisionRow *) xr_calloc(overflow_decisions->row_count, sizeof(*rows));
        XrTargetProfile *retained_profile =
            xr_target_profile_retain(target_profile);
        if (!owned || !rows || !retained_profile) {
            xr_free(rows);
            xr_free(owned);
            xr_target_profile_free(retained_profile);
            return scalar_fail(error, error_size,
                               "Xi overflow decision ownership allocation failed");
        }
        *owned = *overflow_decisions;
        memcpy(rows, overflow_decisions->rows, overflow_decisions->row_count * sizeof(*rows));
        owned->rows = rows;
        module->program_semantic_closure = *closure;
        module->psc_module_index = module_index;
        module->i64_overflow_decisions = owned;
        module->scalar_target_profile = retained_profile;
        *closure = NULL;
        return true;
    }
    XrScalarCallDecision *owned = (XrScalarCallDecision *) xr_malloc(sizeof(*owned));
    if (!owned)
        return scalar_fail(error, error_size, "Xi scalar decision ownership allocation failed");
    XrTargetProfile *retained_profile =
        xr_target_profile_retain(target_profile);
    if (!retained_profile) {
        xr_free(owned);
        return scalar_fail(error, error_size, "Xi scalar target profile retention failed");
    }
    *owned = *decision;
    module->program_semantic_closure = *closure;
    module->psc_module_index = module_index;
    module->scalar_call_decision = owned;
    module->scalar_target_profile = retained_profile;
    *closure = NULL;
    return true;
}
