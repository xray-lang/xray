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
#include "../../plan/semantic/xr_source_semantic_identity.h"
#include "xa_scalar_program_authority.h"
#include "xa_typed_program.h"
#include "xa_resolved_call.h"
#include "xanalyzer.h"
#include "xanalyzer_symbol.h"
#include "../parser/xast_nodes.h"
#include "../parser/xast_walk.h"
#include "../../base/xsha256.h"
#include "../../module/xmodule_graph.h"
#include "../../module/xmodule_identity.h"
#include "../../runtime/class/xclass_info.h"
#include "../../runtime/value/xtype.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include "../../plan/semantic/xr_scalar_call_semantics.h"
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

static int find_function(const XaScalarProgramAuthority *authority, XrStableId declaration,
                         XrStableId instance) {
    int found = -1;
    for (uint32_t i = 0; i < XA_SCALAR_PROGRAM_FUNCTION_COUNT; i++) {
        const XaScalarFunctionAuthority *row = xa_scalar_program_authority_function(authority, i);
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
                                           XrProgramSemanticClosure **out, char *error,
                                           size_t error_size) {
    if (out)
        *out = NULL;
    if (!out || !xa_typed_program_is_verified(typed_program))
        return bridge_fail(error, error_size,
                           "scalar closure requires a verified typed publication");
    const XaScalarProgramAuthority *authority = xa_typed_program_scalar_authority(typed_program);
    if (!authority || !xa_scalar_program_authority_verify(authority, error, error_size))
        return false;
    const XaScalarModuleAuthority *module = xa_scalar_program_authority_module(authority);
    const XaScalarCallAuthority *call = xa_scalar_program_authority_call(authority);
    if (!module || !call)
        return bridge_fail(error, error_size, "scalar authority rows are incomplete");
    int caller =
        find_function(authority, call->caller_declaration_identity, call->caller_instance_identity);
    int callee =
        find_function(authority, call->callee_declaration_identity, call->callee_instance_identity);
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
    if (!xr_program_semantic_closure_create(&limits, xa_scalar_program_authority_policy(authority),
                                            &closure, error, error_size) ||
        !xr_program_semantic_closure_set_family(
            closure, XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_DIRECT_CALL, error, error_size))
        return false;
    XrProgramSemanticModuleInput module_input = {
        .module_identity = module->module_identity,
        .module_authority_fingerprint = module->module_authority_fingerprint,
        .source_fingerprint = module->source_fingerprint,
        .export_fingerprint = module->export_fingerprint,
    };
    XrStableId function_ids[XA_SCALAR_PROGRAM_FUNCTION_COUNT] = {0};
    bool ok = xr_program_semantic_closure_add_module(closure, &module_input, error, error_size);
    for (uint32_t i = 0; ok && i < XA_SCALAR_PROGRAM_FUNCTION_COUNT; i++) {
        const XaScalarFunctionAuthority *row = xa_scalar_program_authority_function(authority, i);
        XrProgramSemanticFunctionInput input = {
            .module_identity = module->module_identity,
            .declaration_identity = row->declaration_identity,
            .concrete_instance_identity = row->concrete_instance_identity,
            .declaration_locator =
                {
                    .kind = row->declaration_span.kind,
                    .start_line = row->declaration_span.start_line,
                    .start_column = row->declaration_span.start_column,
                    .end_line = row->declaration_span.end_line,
                    .end_column = row->declaration_span.end_column,
                },
            .signature_fingerprint = row->signature_fingerprint,
            .effect_fingerprint = row->effect_fingerprint,
            .capability_mask = row->capability_mask,
            .flags = (row->flags & XA_SCALAR_PROGRAM_FUNCTION_ENTRY)
                         ? XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY
                         : 0,
        };
        ok = xr_program_semantic_closure_add_function(closure, &input, &function_ids[i], error,
                                                      error_size);
    }
    if (ok) {
        XrProgramSemanticCallInput input = {
            .callsite_identity = call->callsite_identity,
            .locator =
                {
                    .kind = call->callsite_span.kind,
                    .start_line = call->callsite_span.start_line,
                    .start_column = call->callsite_span.start_column,
                    .end_line = call->callsite_span.end_line,
                    .end_column = call->callsite_span.end_column,
                },
            .caller_function = function_ids[caller],
            .callee_function = function_ids[callee],
            .contract_fingerprint = call->contract_fingerprint,
        };
        XrStableId call_id;
        ok = xr_program_semantic_closure_add_call(closure, &input, &call_id, error, error_size);
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

typedef struct XaLeafCallScan {
    const AstNode *call;
    bool unsupported;
} XaLeafCallScan;

static void leaf_hash_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void leaf_hash_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void leaf_hash_bytes(XrSHA256Context *context, const uint8_t *bytes, size_t size) {
    leaf_hash_u64(context, (uint64_t) size);
    if (size)
        xr_sha256_update(context, bytes, size);
}

static void leaf_hash_text(XrSHA256Context *context, const char *text) {
    leaf_hash_bytes(context, (const uint8_t *) text, text ? strlen(text) : 0);
}

static void leaf_hash_id(XrSHA256Context *context, XrStableId id) {
    leaf_hash_bytes(context, id.bytes, sizeof(id.bytes));
}

static void leaf_hash_fingerprint(XrSHA256Context *context, XrFingerprint fingerprint) {
    leaf_hash_bytes(context, fingerprint.bytes, sizeof(fingerprint.bytes));
}

static void leaf_hash_span(XrSHA256Context *context, XrProgramSemanticSourceLocator span) {
    leaf_hash_u32(context, span.kind);
    leaf_hash_u32(context, span.start_line);
    leaf_hash_u32(context, span.start_column);
    leaf_hash_u32(context, span.end_line);
    leaf_hash_u32(context, span.end_column);
}

static void leaf_hash_begin(XrSHA256Context *context, const char *domain) {
    xr_sha256_init(context);
    leaf_hash_text(context, domain);
    leaf_hash_u32(context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
}

static XrStableId leaf_finish_id(XrSHA256Context *context) {
    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrStableId id;
    xr_sha256_final(context, digest);
    memcpy(id.bytes, digest, sizeof(id.bytes));
    return id;
}

static XrFingerprint leaf_finish_fingerprint(XrSHA256Context *context) {
    XrFingerprint fingerprint;
    xr_sha256_final(context, fingerprint.bytes);
    return fingerprint;
}

static XrProgramSemanticSourceLocator leaf_locator(const AstNode *node) {
    return node ? (XrProgramSemanticSourceLocator) {
                      .kind = (uint32_t) node->type,
                      .start_line = node->line > 0 ? (uint32_t) node->line : 0,
                      .start_column = node->column > 0 ? (uint32_t) node->column : 0,
                      .end_line = node->end_line > 0 ? (uint32_t) node->end_line : 0,
                      .end_column = node->end_column > 0
                                        ? (uint32_t) node->end_column
                                        : 0,
                  }
                : (XrProgramSemanticSourceLocator) {0};
}

static bool leaf_locator_valid(XrProgramSemanticSourceLocator locator) {
    return locator.kind != 0 && locator.start_line != 0 && locator.start_column != 0 &&
           locator.end_line != 0 && locator.end_column != 0 &&
           (locator.end_line > locator.start_line ||
            (locator.end_line == locator.start_line && locator.end_column > locator.start_column));
}

static bool leaf_scan_child(AstNode *child, void *context);

static bool leaf_scan_calls(const AstNode *node, XaLeafCallScan *scan) {
    if (!node || !scan || scan->unsupported)
        return false;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR) {
        scan->unsupported = true;
        return false;
    }
    if (node->type == AST_CALL_EXPR) {
        if (scan->call) {
            scan->unsupported = true;
            return false;
        }
        scan->call = node;
    }
    if (!xr_ast_for_each_child(node, leaf_scan_child, scan)) {
        scan->unsupported = true;
        return false;
    }
    return true;
}

static bool leaf_scan_child(AstNode *child, void *context) {
    return !child || leaf_scan_calls(child, (XaLeafCallScan *) context);
}

static bool leaf_module_authority(const XrModuleSpec *spec, XrProgramSemanticModuleInput *out) {
    if (!spec || !out || spec->status != XR_MODSPEC_ANALYZED || !spec->ast || !spec->canonical ||
        !xr_module_identity_valid(spec->canonical, NULL) || spec->export_symbols_invalid)
        return false;
    uint8_t source = 0;
    for (uint32_t i = 0; i < sizeof(spec->source_content_fingerprint.bytes); i++)
        source |= spec->source_content_fingerprint.bytes[i];
    if (source == 0)
        return false;
    return xr_source_semantic_module_authority(spec->canonical, spec->source_content_fingerprint,
                                               out, NULL);
}

static const XrExactScalarDesc *leaf_exact_scalar(const XrType *type) {
    const XrExactScalarDesc *scalar =
        type ? xr_exact_scalar_by_native_type(type->scalar_rep) : NULL;
    return type && scalar &&
                   ((scalar->family == XR_EXACT_SCALAR_FAMILY_INTEGER &&
                     type->kind == XR_KIND_INT) ||
                    (scalar->family == XR_EXACT_SCALAR_FAMILY_FLOAT &&
                     type->kind == XR_KIND_FLOAT)) &&
                   !type->is_nullable && !type->is_const && !type->is_literal &&
                   !type->is_value_type
               ? scalar
               : NULL;
}

static bool leaf_aggregate_type(const XrType *type, const XrClassInfo *owner) {
    return type && owner && type->kind == XR_KIND_INSTANCE && type->instance.class_ref == owner &&
           owner->struct_layout && !type->is_nullable && !type->is_const && !type->is_literal &&
           type->instance.type_arg_count == 0 && !owner->is_overlay_union && !owner->base &&
           !owner->base_name && owner->interface_count == 0 && owner->method_count == 0 &&
           owner->field_count > 0 && owner->field_count <= XR_MAX_AGG_FIELDS && owner->fields;
}

typedef enum XaLeafAuthorityStatus {
    XA_LEAF_AUTHORITY_OUTSIDE = 0,
    XA_LEAF_AUTHORITY_INVALID,
    XA_LEAF_AUTHORITY_READY,
} XaLeafAuthorityStatus;

static XaLeafAuthorityStatus leaf_effect_fingerprint(const XaAnalyzer *analyzer,
                                                     const XaSymbol *symbol, XrFingerprint *out) {
    const XaSymbolLinks *links = symbol ? &symbol->links : NULL;
    const XaEffectSummary *effect =
        links && links->summary_owner == analyzer && links->effect_id != XA_EFFECT_NONE
            ? xa_effect_db_get(analyzer->effect_db, links->effect_id)
            : NULL;
    const XaMemoryEffectSummary *memory =
        links && links->summary_owner == analyzer &&
                links->memory_effect_id != XA_MEMORY_EFFECT_NONE
            ? xa_memory_effect_db_get(analyzer->memory_effect_db, links->memory_effect_id)
            : NULL;
    if (!out || !links || links->summary_owner != analyzer || !effect || !memory ||
        !links->alloc_effect_complete)
        return XA_LEAF_AUTHORITY_INVALID;
    if (effect->completeness != XA_EFFECT_COMPLETE || effect->unknown_reasons != 0 ||
        effect->unknown_semantic_effects != XA_SEM_EFFECT_NONE ||
        effect->error_set_completeness != XA_EFFECT_COMPLETE ||
        effect->error_unknown_reasons != 0 || memory->completeness != XA_EFFECT_COMPLETE ||
        memory->unknown_reasons != 0 || links->alloc_state == XA_ALLOC_UNKNOWN)
        return XA_LEAF_AUTHORITY_INVALID;
    if (effect->semantic_effects != XA_SEM_EFFECT_NONE || effect->escaping.count != 0 ||
        effect->contains_unsafe_op || effect->requires_unsafe_at_call || memory->root_count != 0 ||
        links->alloc_state != XA_ALLOC_PROVEN_NONE ||
        links->alloc_reason_bits != XA_ALLOC_REASON_NONE)
        return XA_LEAF_AUTHORITY_OUTSIDE;
    XrSHA256Context context;
    leaf_hash_begin(&context, "xray-leaf-value-pure-effect-v1");
    leaf_hash_u32(&context, 0);
    *out = leaf_finish_fingerprint(&context);
    return XA_LEAF_AUTHORITY_READY;
}

static XrStableId leaf_aggregate_declaration(const XrProgramSemanticModuleInput *module,
                                             XrProgramSemanticSourceLocator locator,
                                             const XrProgramSemanticTypeFieldInput *fields,
                                             uint32_t field_count) {
    XrSHA256Context context;
    leaf_hash_begin(&context, "xray-source-leaf-value-declaration-v1");
    leaf_hash_id(&context, module->module_identity);
    leaf_hash_fingerprint(&context, module->source_fingerprint);
    leaf_hash_span(&context, locator);
    leaf_hash_u32(&context, field_count);
    for (uint32_t i = 0; i < field_count; i++) {
        leaf_hash_u32(&context, fields[i].declaration_ordinal);
        leaf_hash_id(&context, fields[i].field_type);
    }
    return leaf_finish_id(&context);
}

static XrStableId leaf_aggregate_instance(XrStableId declaration) {
    XrSHA256Context context;
    leaf_hash_begin(&context, "xray-source-nongeneric-leaf-value-instance-v1");
    leaf_hash_id(&context, declaration);
    return leaf_finish_id(&context);
}

static XrFingerprint leaf_signature(XrStableId aggregate_type, uint32_t parameter_count) {
    XrSHA256Context context;
    leaf_hash_begin(&context, "xray-leaf-value-function-signature-v1");
    leaf_hash_id(&context, aggregate_type);
    leaf_hash_u32(&context, parameter_count);
    if (parameter_count) {
        leaf_hash_id(&context, aggregate_type);
        leaf_hash_u32(&context, XR_PARAM_READ);
    }
    return leaf_finish_fingerprint(&context);
}

static XrStableId leaf_function_declaration(const XrProgramSemanticModuleInput *module,
                                            XrProgramSemanticSourceLocator locator,
                                            XrFingerprint signature) {
    XrSHA256Context context;
    leaf_hash_begin(&context, "xray-source-leaf-value-function-declaration-v1");
    leaf_hash_id(&context, module->module_identity);
    leaf_hash_fingerprint(&context, module->source_fingerprint);
    leaf_hash_span(&context, locator);
    leaf_hash_fingerprint(&context, signature);
    return leaf_finish_id(&context);
}

static XrStableId leaf_function_instance(XrStableId declaration, XrFingerprint signature) {
    XrSHA256Context context;
    leaf_hash_begin(&context, "xray-source-nongeneric-leaf-function-instance-v1");
    leaf_hash_id(&context, declaration);
    leaf_hash_fingerprint(&context, signature);
    return leaf_finish_id(&context);
}

static XrFingerprint leaf_call_contract(XrStableId aggregate_type, XrFingerprint signature,
                                        XrFingerprint effect) {
    XrSHA256Context context;
    leaf_hash_begin(&context, "xray-leaf-value-direct-call-contract-v1");
    leaf_hash_id(&context, aggregate_type);
    leaf_hash_fingerprint(&context, signature);
    leaf_hash_fingerprint(&context, effect);
    return leaf_finish_fingerprint(&context);
}

static XrStableId leaf_callsite(const XrProgramSemanticModuleInput *module,
                                XrStableId caller_declaration,
                                XrProgramSemanticSourceLocator locator) {
    XrStableId identity = {{0}};
    (void) xr_source_semantic_callsite_identity(module->source_fingerprint, module->module_identity,
                                                caller_declaration, locator, &identity);
    return identity;
}

static XaLeafAuthorityStatus
leaf_function_candidate(const XaAnalyzer *analyzer, const AstNode *node,
                        const XrClassInfo *aggregate, uint32_t parameter_count, bool entry,
                        const XaSymbol **symbol_out, XrFingerprint *effect_out) {
    const FunctionDeclNode *function =
        node && node->type == AST_FUNCTION_DECL ? &node->as.function_decl : NULL;
    const XaSymbol *symbol =
        function && function->symbol_id
            ? xa_analyzer_symbol_by_id((XaAnalyzer *) analyzer, function->symbol_id)
            : NULL;
    const XrType *type = symbol ? symbol->links.type : NULL;
    if (!function || function->type_param_count != 0 || function->is_generator ||
        function->is_extern || !function->body || function->param_count != (int) parameter_count ||
        (node->is_exported && !entry))
        return XA_LEAF_AUTHORITY_OUTSIDE;
    if (!symbol || !symbol->links.type || symbol->links.summary_owner != analyzer ||
        symbol->links.function_decl_node != node)
        return XA_LEAF_AUTHORITY_INVALID;
    if (symbol->kind != XA_SYM_FUNCTION || symbol->is_builtin || symbol->is_imported ||
        symbol->is_exported != node->is_exported || symbol->parent ||
        type->kind != XR_KIND_FUNCTION ||
        type->function.param_count != (int) parameter_count ||
        type->function.min_params != (int) parameter_count || type->function.is_variadic ||
        type->function.is_c_abi || type->function.throw_effect != XR_FN_EFFECT_NO_THROW ||
        type->function.type_param_count != 0 ||
        !leaf_aggregate_type(type->function.return_type, aggregate))
        return XA_LEAF_AUTHORITY_OUTSIDE;
    if (!leaf_locator_valid(leaf_locator(node)))
        return XA_LEAF_AUTHORITY_INVALID;
    if (parameter_count &&
        (!type->function.params || !leaf_aggregate_type(type->function.params[0].type, aggregate) ||
         type->function.params[0].mode != XR_PARAM_READ))
        return XA_LEAF_AUTHORITY_OUTSIDE;
    XaLeafAuthorityStatus effect = leaf_effect_fingerprint(analyzer, symbol, effect_out);
    if (effect != XA_LEAF_AUTHORITY_READY)
        return effect;
    *symbol_out = symbol;
    return XA_LEAF_AUTHORITY_READY;
}

XaProgramSemanticClosurePublishStatus xa_program_semantic_closure_publish_leaf_aggregate(
    XaAnalyzer *analyzer, const AstNode *syntax, const XrModuleSpec *module_spec,
    XrProgramSemanticClosure **out, char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!out || !analyzer || !syntax)
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    if (syntax->type != AST_PROGRAM || syntax->as.program.count != 3 ||
        !syntax->as.program.statements)
        return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;

    const AstNode *aggregate_node = NULL;
    const AstNode *functions[2] = {0};
    uint32_t function_count = 0;
    for (int i = 0; i < syntax->as.program.count; i++) {
        const AstNode *statement = syntax->as.program.statements[i];
        if (statement && statement->type == AST_STRUCT_DECL && !aggregate_node)
            aggregate_node = statement;
        else if (statement && statement->type == AST_FUNCTION_DECL && function_count < 2)
            functions[function_count++] = statement;
        else
            return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;
    }
    if (!aggregate_node || function_count != 2)
        return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;

    /* Claim this bounded family from syntax alone. Once the exact one-struct,
     * two-function, nullary-to-unary direct-call shape is present, missing
     * analyzer authorities are invalid rather than a license to fall back. */
    XaLeafCallScan scans[2] = {0};
    const AstNode *call = NULL;
    int caller = -1;
    for (int i = 0; i < 2; i++) {
        if (!leaf_scan_calls(functions[i]->as.function_decl.body, &scans[i]) ||
            scans[i].unsupported)
            return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;
        if (!scans[i].call)
            continue;
        if (call)
            return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;
        call = scans[i].call;
        caller = i;
    }
    if (!call || caller < 0 || call->as.call_expr.arg_count != 1 || !call->as.call_expr.arguments ||
        !call->as.call_expr.arguments[0] || !call->as.call_expr.callee ||
        call->as.call_expr.callee->type != AST_VARIABLE ||
        functions[caller]->as.function_decl.param_count != 0 ||
        functions[1 - caller]->as.function_decl.param_count != 1)
        return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;

    const ClassDeclNode *decl = &aggregate_node->as.struct_decl;
    XaSymbol *aggregate_symbol =
        decl->symbol_id ? xa_analyzer_symbol_by_id(analyzer, decl->symbol_id) : NULL;
    XrClassInfo *aggregate = aggregate_symbol ? aggregate_symbol->links.class_info : NULL;
    const XaSymbol *first_function_symbol =
        functions[0] && functions[0]->as.function_decl.symbol_id
            ? xa_analyzer_symbol_by_id(analyzer, functions[0]->as.function_decl.symbol_id)
            : NULL;
    const XrType *first_return = first_function_symbol && first_function_symbol->links.type &&
                                         first_function_symbol->links.type->kind == XR_KIND_FUNCTION
                                     ? first_function_symbol->links.type->function.return_type
                                     : NULL;
    if (!aggregate_symbol || !aggregate || !aggregate_symbol->links.type ||
        !first_function_symbol || !first_function_symbol->links.type)
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    if (aggregate_symbol->kind != XA_SYM_CLASS || aggregate_symbol->is_builtin ||
        aggregate_symbol->is_imported || aggregate_symbol->is_exported ||
        aggregate_node->is_exported || decl->type_param_count != 0 || decl->method_count != 0 ||
        decl->interface_count != 0 || decl->is_packed || decl->explicit_align != 0 || !aggregate ||
        !aggregate_symbol->links.type || !aggregate_symbol->links.type->is_value_type ||
        !leaf_aggregate_type(first_return, aggregate))
        return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;

    XrExactScalarId scalar_ids[XR_MAX_AGG_FIELDS] = {0};
    for (int i = 0; i < aggregate->field_count; i++) {
        const XaSymbol *field = aggregate->fields[i];
        const XrExactScalarDesc *scalar = field && !field->is_static && !field->is_weak
                                              ? leaf_exact_scalar(field->links.type)
                                              : NULL;
        if (!field || !field->links.type)
            return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
        if (!scalar)
            return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;
        scalar_ids[i] = scalar->id;
    }

    const XaSymbol *function_symbols[2] = {0};
    XrFingerprint effects[2] = {{{0}}, {{0}}};
    for (int i = 0; i < 2; i++) {
        uint32_t parameters = i == caller ? 0u : 1u;
        XaLeafAuthorityStatus status =
            leaf_function_candidate(analyzer, functions[i], aggregate, parameters, i == caller,
                                    &function_symbols[i], &effects[i]);
        if (status != XA_LEAF_AUTHORITY_READY)
            return status == XA_LEAF_AUTHORITY_INVALID ? XA_PROGRAM_SEMANTIC_CLOSURE_INVALID
                                                       : XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;
    }
    const XaResolvedCall *resolved = xa_analyzer_get_resolved_call(analyzer, call);
    if (!resolved || resolved->reason != XA_RESOLVED_CALL_REASON_RESOLVED ||
        resolved->source_node_id != call->node_id ||
        resolved->target_symbol_id != function_symbols[1 - caller]->id ||
        resolved->intrinsic_id != XA_INTRINSIC_NONE || resolved->flags != 0)
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    const XrType *call_type = xa_analyzer_get_node_type(analyzer, call);
    const XrType *argument_type =
        xa_analyzer_get_node_type(analyzer, call->as.call_expr.arguments[0]);
    if (!call_type || !argument_type)
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    if (!leaf_aggregate_type(call_type, aggregate) ||
        !leaf_aggregate_type(argument_type, aggregate))
        return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;

    XrProgramSemanticModuleInput module;
    if (!leaf_module_authority(module_spec, &module) || module_spec->ast != syntax)
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    XrSHA256Context context;
    leaf_hash_begin(&context, "xray-leaf-value-direct-call-policy-v1");
    leaf_hash_u32(&context, 1);
    leaf_hash_u32(&context, 2);
    leaf_hash_u32(&context, 1);
    XrFingerprint policy = leaf_finish_fingerprint(&context);
    XrProgramSemanticClosureLimits limits = {
        .max_modules = 1,
        .max_dependencies = 0,
        .max_types = (uint32_t) aggregate->field_count + 1u,
        .max_type_fields = (uint32_t) aggregate->field_count,
        .max_functions = 2,
        .max_function_parameters = 1,
        .max_calls = 1,
    };
    XrProgramSemanticClosure *closure = NULL;
    if (!xr_program_semantic_closure_create(&limits, policy, &closure, error, error_size))
        return XA_PROGRAM_SEMANTIC_CLOSURE_RESOURCE_FAILURE;
    bool ok = xr_program_semantic_closure_set_family(
                  closure, XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_AGGREGATE_DIRECT_CALL, error,
                  error_size) &&
              xr_program_semantic_closure_add_module(closure, &module, error, error_size);
    XrStableId unique_scalar_types[XR_MAX_AGG_FIELDS] = {{{0}}};
    XrExactScalarId unique_scalars[XR_MAX_AGG_FIELDS] = {0};
    uint32_t unique_count = 0;
    XrProgramSemanticTypeFieldInput fields[XR_MAX_AGG_FIELDS] = {0};
    for (int i = 0; ok && i < aggregate->field_count; i++) {
        uint32_t found = unique_count;
        for (uint32_t j = 0; j < unique_count; j++)
            if (unique_scalars[j] == scalar_ids[i]) {
                found = j;
                break;
            }
        if (found == unique_count) {
            XrProgramSemanticTypeInput input;
            ok = xr_program_semantic_exact_scalar_type_input((uint8_t) scalar_ids[i], &input) &&
                 xr_program_semantic_closure_add_type(
                     closure, &input, &unique_scalar_types[unique_count], error, error_size);
            if (ok)
                unique_scalars[unique_count++] = scalar_ids[i];
        }
        fields[i] = (XrProgramSemanticTypeFieldInput) {
            .field_type = unique_scalar_types[found],
            .declaration_ordinal = (uint32_t) i,
        };
    }
    XrStableId aggregate_type = {{0}};
    XrProgramSemanticSourceLocator aggregate_locator = leaf_locator(aggregate_node);
    if (ok && !leaf_locator_valid(aggregate_locator))
        ok = false;
    if (ok) {
        XrStableId declaration = leaf_aggregate_declaration(&module, aggregate_locator, fields,
                                                            (uint32_t) aggregate->field_count);
        XrProgramSemanticTypeInput input = {
            .module_identity = module.module_identity,
            .declaration_identity = declaration,
            .concrete_instance_identity = leaf_aggregate_instance(declaration),
            .declaration_locator = aggregate_locator,
            .fields = fields,
            .field_count = (uint32_t) aggregate->field_count,
            .kind = XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_AGGREGATE,
            .flags = XR_PROGRAM_SEMANTIC_TYPE_NONNULLABLE | XR_PROGRAM_SEMANTIC_TYPE_NONGENERIC |
                     XR_PROGRAM_SEMANTIC_TYPE_VALUE | XR_PROGRAM_SEMANTIC_TYPE_POINTER_FREE,
        };
        ok = xr_program_semantic_closure_add_type(closure, &input, &aggregate_type, error,
                                                  error_size);
    }
    XrStableId function_ids[2] = {{{0}}, {{0}}};
    XrFingerprint signatures[2] = {{{0}}, {{0}}};
    for (int i = 0; ok && i < 2; i++) {
        uint32_t parameter_count = i == caller ? 0u : 1u;
        XrProgramSemanticSourceLocator locator = leaf_locator(functions[i]);
        signatures[i] = leaf_signature(aggregate_type, parameter_count);
        XrStableId declaration = leaf_function_declaration(&module, locator, signatures[i]);
        XrProgramSemanticFunctionParameterInput parameter = {
            .type = aggregate_type,
            .declaration_ordinal = 0,
            .mode = XR_PARAM_READ,
        };
        XrProgramSemanticFunctionInput input = {
            .module_identity = module.module_identity,
            .declaration_identity = declaration,
            .concrete_instance_identity = leaf_function_instance(declaration, signatures[i]),
            .declaration_locator = locator,
            .signature_fingerprint = signatures[i],
            .effect_fingerprint = effects[i],
            .return_type = aggregate_type,
            .parameters = parameter_count ? &parameter : NULL,
            .parameter_count = parameter_count,
            .flags = (uint8_t) ((i == caller ? XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY : 0) |
                                (functions[i]->is_exported
                                     ? XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED
                                     : 0)),
        };
        ok = xr_program_semantic_closure_add_function(closure, &input, &function_ids[i], error,
                                                      error_size);
    }
    if (ok) {
        XrProgramSemanticSourceLocator locator = leaf_locator(call);
        XrStableId caller_declaration =
            leaf_function_declaration(&module, leaf_locator(functions[caller]), signatures[caller]);
        XrProgramSemanticCallInput input = {
            .callsite_identity = leaf_callsite(&module, caller_declaration, locator),
            .locator = locator,
            .caller_function = function_ids[caller],
            .callee_function = function_ids[1 - caller],
            .contract_fingerprint =
                leaf_call_contract(aggregate_type, signatures[1 - caller], effects[1 - caller]),
        };
        XrStableId call_identity;
        ok = xr_program_semantic_closure_add_call(closure, &input, &call_identity, error,
                                                  error_size);
    }
    ok = ok && xr_program_semantic_closure_freeze(closure, error, error_size) &&
         xr_program_semantic_closure_verify(closure, error, error_size);
    if (!ok) {
        XrProgramSemanticClosureFailureKind failure =
            xr_program_semantic_closure_failure_kind(closure);
        xr_program_semantic_closure_free(closure);
        return failure == XR_PROGRAM_SEMANTIC_CLOSURE_FAILURE_RESOURCE
                   ? XA_PROGRAM_SEMANTIC_CLOSURE_RESOURCE_FAILURE
                   : XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }
    *out = closure;
    return XA_PROGRAM_SEMANTIC_CLOSURE_READY;
}

enum {
    XA_LEAF_PRODUCT_MEMBER_COUNT = 6,
    XA_LEAF_PRODUCT_FUNCTION_COUNT = 3,
    XA_LEAF_PRODUCT_CALL_COUNT = 2,
};

typedef struct XaLeafProductSource {
    XrProgramSemanticModuleInput module;
    const AstNode *functions[XA_LEAF_PRODUCT_FUNCTION_COUNT];
    const XaSymbol *symbols[XA_LEAF_PRODUCT_FUNCTION_COUNT];
    const AstNode *calls[XA_LEAF_PRODUCT_CALL_COUNT];
    uint32_t callers[XA_LEAF_PRODUCT_CALL_COUNT];
    XrFingerprint effects[XA_LEAF_PRODUCT_FUNCTION_COUNT];
    uint32_t callee;
} XaLeafProductSource;

static bool leaf_product_type_exact(const XrType *type) {
    static const uint8_t expected[XA_LEAF_PRODUCT_MEMBER_COUNT] = {
        XR_EXACT_SCALAR_I64, XR_EXACT_SCALAR_I64, XR_EXACT_SCALAR_U8,
        XR_EXACT_SCALAR_I64, XR_EXACT_SCALAR_I64, XR_EXACT_SCALAR_I64,
    };
    if (!type || type->kind != XR_KIND_TUPLE || type->is_nullable || type->is_const ||
        type->is_literal || xr_type_tuple_count((XrType *) type) != XA_LEAF_PRODUCT_MEMBER_COUNT)
        return false;
    for (uint32_t i = 0; i < XA_LEAF_PRODUCT_MEMBER_COUNT; i++) {
        const XrExactScalarDesc *scalar =
            leaf_exact_scalar(xr_type_tuple_get((XrType *) type, (int) i));
        if (!scalar || scalar->id != expected[i])
            return false;
    }
    return true;
}

static XaLeafAuthorityStatus leaf_product_function_candidate(
    const XaAnalyzer *analyzer, const AstNode *node, const XaSymbol **symbol_out,
    XrFingerprint *effect_out) {
    const FunctionDeclNode *function =
        node && node->type == AST_FUNCTION_DECL ? &node->as.function_decl : NULL;
    const XaSymbol *symbol =
        function && function->symbol_id
            ? xa_analyzer_symbol_by_id((XaAnalyzer *) analyzer, function->symbol_id)
            : NULL;
    const XrType *type = symbol ? symbol->links.type : NULL;
    if (!function || function->type_param_count != 0 || function->is_generator ||
        function->is_extern || !function->body || function->param_count != 0 || node->is_exported)
        return XA_LEAF_AUTHORITY_OUTSIDE;
    if (!symbol || symbol->links.summary_owner != analyzer ||
        symbol->links.function_decl_node != node)
        return XA_LEAF_AUTHORITY_INVALID;
    if (symbol->kind != XA_SYM_FUNCTION || symbol->is_builtin || symbol->is_imported ||
        symbol->is_exported || symbol->parent || !type || type->kind != XR_KIND_FUNCTION ||
        type->function.param_count != 0 || type->function.min_params != 0 ||
        type->function.is_variadic || type->function.is_c_abi ||
        type->function.throw_effect != XR_FN_EFFECT_NO_THROW ||
        type->function.type_param_count != 0 || !leaf_product_type_exact(type->function.return_type))
        return XA_LEAF_AUTHORITY_OUTSIDE;
    if (!leaf_locator_valid(leaf_locator(node)))
        return XA_LEAF_AUTHORITY_INVALID;
    XaLeafAuthorityStatus effect = leaf_effect_fingerprint(analyzer, symbol, effect_out);
    if (effect == XA_LEAF_AUTHORITY_READY)
        *symbol_out = symbol;
    return effect;
}

static XaProgramSemanticClosurePublishStatus leaf_product_collect(
    XaAnalyzer *analyzer, const AstNode *syntax, const XrModuleSpec *module_spec,
    XaLeafProductSource *source) {
    if (!analyzer || !syntax || !source || syntax->type != AST_PROGRAM ||
        syntax->as.program.count != XA_LEAF_PRODUCT_FUNCTION_COUNT ||
        !syntax->as.program.statements)
        return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;
    memset(source, 0, sizeof(*source));
    source->callee = UINT32_MAX;
    uint32_t call_count = 0;
    for (uint32_t i = 0; i < XA_LEAF_PRODUCT_FUNCTION_COUNT; i++) {
        source->functions[i] = syntax->as.program.statements[i];
        XaLeafAuthorityStatus status = leaf_product_function_candidate(
            analyzer, source->functions[i], &source->symbols[i], &source->effects[i]);
        if (status != XA_LEAF_AUTHORITY_READY)
            return status == XA_LEAF_AUTHORITY_INVALID ? XA_PROGRAM_SEMANTIC_CLOSURE_INVALID
                                                       : XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;
        XaLeafCallScan scan = {0};
        if (!leaf_scan_calls(source->functions[i]->as.function_decl.body, &scan) ||
            scan.unsupported)
            return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;
        if (!scan.call) {
            if (source->callee != UINT32_MAX)
                return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;
            source->callee = i;
        } else if (call_count < XA_LEAF_PRODUCT_CALL_COUNT) {
            source->calls[call_count] = scan.call;
            source->callers[call_count++] = i;
        } else {
            return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;
        }
    }
    if (source->callee == UINT32_MAX || call_count != XA_LEAF_PRODUCT_CALL_COUNT)
        return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;
    for (uint32_t i = 0; i < XA_LEAF_PRODUCT_CALL_COUNT; i++) {
        const AstNode *call = source->calls[i];
        const XaResolvedCall *resolved = xa_analyzer_get_resolved_call(analyzer, call);
        if (!call || call->as.call_expr.arg_count != 0 ||
            !call->as.call_expr.callee || call->as.call_expr.callee->type != AST_VARIABLE ||
            !resolved || resolved->reason != XA_RESOLVED_CALL_REASON_RESOLVED ||
            resolved->source_node_id != call->node_id ||
            resolved->target_symbol_id != source->symbols[source->callee]->id ||
            resolved->intrinsic_id != XA_INTRINSIC_NONE || resolved->flags != 0 ||
            !leaf_product_type_exact(xa_analyzer_get_node_type(analyzer, call)))
            return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }
    if (!leaf_module_authority(module_spec, &source->module) || module_spec->ast != syntax)
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    return XA_PROGRAM_SEMANTIC_CLOSURE_READY;
}

static XrStableId leaf_product_declaration(
    const XrProgramSemanticModuleInput *module,
    const XrProgramSemanticTypeFieldInput fields[XA_LEAF_PRODUCT_MEMBER_COUNT]) {
    XrSHA256Context context;
    leaf_hash_begin(&context, "xray-source-leaf-value-product-v1");
    leaf_hash_id(&context, module->module_identity);
    leaf_hash_fingerprint(&context, module->source_fingerprint);
    leaf_hash_u32(&context, XA_LEAF_PRODUCT_MEMBER_COUNT);
    for (uint32_t i = 0; i < XA_LEAF_PRODUCT_MEMBER_COUNT; i++) {
        leaf_hash_u32(&context, fields[i].declaration_ordinal);
        leaf_hash_id(&context, fields[i].field_type);
    }
    return leaf_finish_id(&context);
}

static XrStableId leaf_product_instance(XrStableId declaration) {
    XrSHA256Context context;
    leaf_hash_begin(&context, "xray-source-nongeneric-leaf-value-product-v1");
    leaf_hash_id(&context, declaration);
    return leaf_finish_id(&context);
}

static XrFingerprint leaf_product_policy(void) {
    XrSHA256Context context;
    leaf_hash_begin(&context, "xray-leaf-value-product-direct-call-policy-v1");
    leaf_hash_u32(&context, 1);
    leaf_hash_u32(&context, XA_LEAF_PRODUCT_FUNCTION_COUNT);
    leaf_hash_u32(&context, XA_LEAF_PRODUCT_CALL_COUNT);
    leaf_hash_u32(&context, XA_LEAF_PRODUCT_MEMBER_COUNT);
    return leaf_finish_fingerprint(&context);
}

XaProgramSemanticClosurePublishStatus xa_program_semantic_closure_publish_leaf_product(
    XaAnalyzer *analyzer, const AstNode *syntax, const XrModuleSpec *module_spec,
    XrProgramSemanticClosure **out, char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!out)
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    XaLeafProductSource source;
    XaProgramSemanticClosurePublishStatus status =
        leaf_product_collect(analyzer, syntax, module_spec, &source);
    if (status != XA_PROGRAM_SEMANTIC_CLOSURE_READY)
        return status;
    XrProgramSemanticClosureLimits limits = {
        .max_modules = 1, .max_types = 3,
        .max_type_fields = XA_LEAF_PRODUCT_MEMBER_COUNT,
        .max_functions = XA_LEAF_PRODUCT_FUNCTION_COUNT,
        .max_calls = XA_LEAF_PRODUCT_CALL_COUNT,
    };
    XrProgramSemanticClosure *closure = NULL;
    if (!xr_program_semantic_closure_create(&limits, leaf_product_policy(), &closure, error,
                                            error_size))
        return XA_PROGRAM_SEMANTIC_CLOSURE_RESOURCE_FAILURE;
    bool ok = xr_program_semantic_closure_set_family(
                  closure, XR_PROGRAM_SEMANTIC_FAMILY_LEAF_VALUE_PRODUCT_DIRECT_CALL, error,
                  error_size) &&
              xr_program_semantic_closure_add_module(closure, &source.module, error, error_size);
    XrStableId scalar_types[2] = {{{0}}, {{0}}};
    static const uint8_t scalars[2] = {XR_EXACT_SCALAR_I64, XR_EXACT_SCALAR_U8};
    for (uint32_t i = 0; ok && i < 2; i++) {
        XrProgramSemanticTypeInput input;
        ok = xr_program_semantic_exact_scalar_type_input(scalars[i], &input) &&
             xr_program_semantic_closure_add_type(closure, &input, &scalar_types[i], error,
                                                  error_size);
    }
    XrProgramSemanticTypeFieldInput fields[XA_LEAF_PRODUCT_MEMBER_COUNT] = {0};
    for (uint32_t i = 0; i < XA_LEAF_PRODUCT_MEMBER_COUNT; i++)
        fields[i] = (XrProgramSemanticTypeFieldInput) {
            .field_type = scalar_types[i == 2 ? 1 : 0], .declaration_ordinal = i,
        };
    XrStableId product_type = {{0}};
    XrStableId product_declaration = leaf_product_declaration(&source.module, fields);
    XrProgramSemanticTypeInput product = {
        .module_identity = source.module.module_identity,
        .declaration_identity = product_declaration,
        .concrete_instance_identity = leaf_product_instance(product_declaration),
        .fields = fields,
        .field_count = XA_LEAF_PRODUCT_MEMBER_COUNT,
        .kind = XR_PROGRAM_SEMANTIC_TYPE_LEAF_VALUE_PRODUCT,
        .flags = XR_PROGRAM_SEMANTIC_TYPE_NONNULLABLE | XR_PROGRAM_SEMANTIC_TYPE_NONGENERIC |
                 XR_PROGRAM_SEMANTIC_TYPE_VALUE | XR_PROGRAM_SEMANTIC_TYPE_POINTER_FREE,
    };
    ok = ok && xr_program_semantic_closure_add_type(closure, &product, &product_type, error,
                                                    error_size);
    XrStableId function_ids[XA_LEAF_PRODUCT_FUNCTION_COUNT] = {{{0}}};
    XrFingerprint signature = leaf_signature(product_type, 0);
    for (uint32_t i = 0; ok && i < XA_LEAF_PRODUCT_FUNCTION_COUNT; i++) {
        XrProgramSemanticSourceLocator locator = leaf_locator(source.functions[i]);
        XrStableId declaration = leaf_function_declaration(&source.module, locator, signature);
        XrProgramSemanticFunctionInput input = {
            .module_identity = source.module.module_identity,
            .declaration_identity = declaration,
            .concrete_instance_identity = leaf_function_instance(declaration, signature),
            .declaration_locator = locator,
            .signature_fingerprint = signature,
            .effect_fingerprint = source.effects[i],
            .return_type = product_type,
            .flags = i == source.callee ? 0 : XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY,
        };
        ok = xr_program_semantic_closure_add_function(closure, &input, &function_ids[i], error,
                                                      error_size);
    }
    for (uint32_t i = 0; ok && i < XA_LEAF_PRODUCT_CALL_COUNT; i++) {
        uint32_t caller = source.callers[i];
        XrProgramSemanticSourceLocator locator = leaf_locator(source.calls[i]);
        XrStableId caller_declaration =
            leaf_function_declaration(&source.module, leaf_locator(source.functions[caller]),
                                      signature);
        XrProgramSemanticCallInput input = {
            .callsite_identity = leaf_callsite(&source.module, caller_declaration, locator),
            .locator = locator,
            .caller_function = function_ids[caller],
            .callee_function = function_ids[source.callee],
            .contract_fingerprint =
                leaf_call_contract(product_type, signature, source.effects[source.callee]),
        };
        XrStableId call_identity;
        ok = xr_program_semantic_closure_add_call(closure, &input, &call_identity, error,
                                                  error_size);
    }
    ok = ok && xr_program_semantic_closure_freeze(closure, error, error_size) &&
         xr_program_semantic_closure_verify(closure, error, error_size);
    if (!ok) {
        XrProgramSemanticClosureFailureKind failure =
            xr_program_semantic_closure_failure_kind(closure);
        xr_program_semantic_closure_free(closure);
        return failure == XR_PROGRAM_SEMANTIC_CLOSURE_FAILURE_RESOURCE
                   ? XA_PROGRAM_SEMANTIC_CLOSURE_RESOURCE_FAILURE
                   : XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }
    *out = closure;
    return XA_PROGRAM_SEMANTIC_CLOSURE_READY;
}

static XrStableId scalar_graph_function_declaration(
    const XrProgramSemanticModuleInput *module, XrProgramSemanticSourceLocator locator,
    XrFingerprint signature) {
    XrSHA256Context context;
    leaf_hash_begin(&context, "xray-source-scalar-graph-function-declaration-v1");
    leaf_hash_id(&context, module->module_identity);
    leaf_hash_fingerprint(&context, module->source_fingerprint);
    leaf_hash_span(&context, locator);
    leaf_hash_fingerprint(&context, signature);
    return leaf_finish_id(&context);
}

static XrStableId scalar_graph_function_instance(XrStableId declaration,
                                                 XrFingerprint signature) {
    XrSHA256Context context;
    leaf_hash_begin(&context, "xray-source-scalar-graph-function-instance-v1");
    leaf_hash_id(&context, declaration);
    leaf_hash_fingerprint(&context, signature);
    return leaf_finish_id(&context);
}

static XrFingerprint scalar_graph_dependency_contract(
    const XrProgramSemanticModuleInput *source, const XrProgramSemanticModuleInput *dependency,
    XrStableId exported_declaration, XrStableId exported_function, XrStableId resolver_binding,
    XrStableId return_type, XrFingerprint signature, XrFingerprint effect, uint64_t capability_mask,
    XrProgramSemanticSourceLocator import_locator) {
    XrSHA256Context context;
    leaf_hash_begin(&context, "xray-source-scalar-graph-dependency-contract-v2");
    leaf_hash_id(&context, source->module_identity);
    leaf_hash_fingerprint(&context, source->module_authority_fingerprint);
    leaf_hash_fingerprint(&context, source->source_fingerprint);
    leaf_hash_fingerprint(&context, source->export_fingerprint);
    leaf_hash_id(&context, dependency->module_identity);
    leaf_hash_fingerprint(&context, dependency->module_authority_fingerprint);
    leaf_hash_fingerprint(&context, dependency->source_fingerprint);
    leaf_hash_fingerprint(&context, dependency->export_fingerprint);
    leaf_hash_span(&context, import_locator);
    leaf_hash_id(&context, exported_declaration);
    leaf_hash_id(&context, exported_function);
    leaf_hash_id(&context, resolver_binding);
    leaf_hash_id(&context, return_type);
    leaf_hash_fingerprint(&context, signature);
    leaf_hash_fingerprint(&context, effect);
    leaf_hash_u64(&context, capability_mask);
    leaf_hash_u32(&context, 1);
    leaf_hash_u32(&context, XR_PARAM_READ);
    return leaf_finish_fingerprint(&context);
}

static bool scalar_graph_function_exact(const XaAnalyzer *analyzer, const AstNode *node,
                                        uint32_t parameter_count, bool exported,
                                        const XaSymbol **symbol_out) {
    const FunctionDeclNode *function =
        node && node->type == AST_FUNCTION_DECL ? &node->as.function_decl : NULL;
    const XaSymbol *symbol =
        function && function->symbol_id
            ? xa_analyzer_symbol_by_id((XaAnalyzer *) analyzer, function->symbol_id)
            : NULL;
    const XrType *type = symbol ? symbol->links.type : NULL;
    if (symbol_out)
        *symbol_out = NULL;
    if (!analyzer || !function || !symbol_out || !function->body ||
        function->type_param_count != 0 || function->is_generator || function->is_extern ||
        function->param_count != (int) parameter_count || node->is_exported != exported ||
        !leaf_locator_valid(leaf_locator(node)) || !symbol || symbol->kind != XA_SYM_FUNCTION ||
        symbol->is_builtin || symbol->is_imported || symbol->is_exported != exported ||
        symbol->parent || symbol->links.summary_owner != analyzer ||
        symbol->links.function_decl_node != node || !type || type->kind != XR_KIND_FUNCTION ||
        type->function.param_count != (int) parameter_count ||
        type->function.min_params != (int) parameter_count || type->function.is_variadic ||
        type->function.is_c_abi || type->function.throw_effect != XR_FN_EFFECT_NO_THROW ||
        type->function.type_param_count != 0 ||
        !leaf_exact_scalar(type->function.return_type) ||
        leaf_exact_scalar(type->function.return_type)->id != XR_EXACT_SCALAR_I64)
        return false;
    if (parameter_count &&
        (!type->function.params || type->function.params[0].mode != XR_PARAM_READ ||
         !leaf_exact_scalar(type->function.params[0].type) ||
         leaf_exact_scalar(type->function.params[0].type)->id != XR_EXACT_SCALAR_I64))
        return false;
    XrFingerprint ignored;
    if (leaf_effect_fingerprint(analyzer, symbol, &ignored) != XA_LEAF_AUTHORITY_READY)
        return false;
    *symbol_out = symbol;
    return true;
}

static bool scalar_graph_unary_i64_symbol_type_exact(const XaAnalyzer *analyzer,
                                                     const XaSymbol *symbol) {
    const XrType *type = symbol ? symbol->links.type : NULL;
    return analyzer && symbol && symbol->kind == XA_SYM_FUNCTION && !symbol->is_builtin &&
           !symbol->parent && symbol->links.summary_owner == analyzer && type &&
           type->kind == XR_KIND_FUNCTION && type->function.param_count == 1 &&
           type->function.min_params == 1 && !type->function.is_variadic &&
           !type->function.is_c_abi && type->function.throw_effect == XR_FN_EFFECT_NO_THROW &&
           type->function.type_param_count == 0 && type->function.params &&
           type->function.params[0].mode == XR_PARAM_READ &&
           leaf_exact_scalar(type->function.params[0].type) &&
           leaf_exact_scalar(type->function.params[0].type)->id == XR_EXACT_SCALAR_I64 &&
           leaf_exact_scalar(type->function.return_type) &&
           leaf_exact_scalar(type->function.return_type)->id == XR_EXACT_SCALAR_I64;
}

static bool scalar_graph_import_symbol_exact(const XaAnalyzer *analyzer, const XaSymbol *symbol) {
    return scalar_graph_unary_i64_symbol_type_exact(analyzer, symbol) && symbol->is_imported &&
           !symbol->is_exported;
}

static bool
scalar_graph_symbol_export_declaration(const XaAnalyzer *analyzer, const XaSymbol *symbol,
                                       bool imported,
                                       const XrProgramSemanticModuleInput *dependency_module,
                                       XrFingerprint signature, XrStableId *out) {
    const AstNode *declaration = symbol ? symbol->links.function_decl_node : NULL;
    XrFingerprint ignored_effect;
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !dependency_module || !symbol || symbol->kind != XA_SYM_FUNCTION ||
        symbol->is_imported != imported || symbol->is_builtin || symbol->parent ||
        symbol->links.summary_owner != analyzer || !declaration ||
        declaration->type != AST_FUNCTION_DECL || !leaf_locator_valid(leaf_locator(declaration)) ||
        leaf_effect_fingerprint(analyzer, symbol, &ignored_effect) != XA_LEAF_AUTHORITY_READY ||
        (imported ? symbol->is_exported
                  : !symbol->is_exported || declaration->as.function_decl.param_count != 1))
        return false;
    if (imported ? !scalar_graph_import_symbol_exact(analyzer, symbol)
                 : !scalar_graph_unary_i64_symbol_type_exact(analyzer, symbol))
        return false;
    *out =
        scalar_graph_function_declaration(dependency_module, leaf_locator(declaration), signature);
    return true;
}

static bool scalar_graph_import_targets_dependency(const XrModuleGraph *graph,
                                                   const XrModuleSpec *entry,
                                                   const XrModuleSpec *dependency,
                                                   const ImportStmtNode *import) {
    if (!graph || !graph->resolver || !entry || !dependency || !import || !import->module_name ||
        !import->is_quoted)
        return false;
    XrModuleId resolved;
    int status = xr_module_resolver_resolve(graph->resolver, import->module_name,
                                            entry->source_path, &entry->authority, &resolved,
                                            NULL);
    bool exact = status == 0 && resolved.canonical && dependency->canonical &&
                 strcmp(resolved.canonical, dependency->canonical) == 0;
    if (status == 0)
        xr_module_id_cleanup(&resolved);
    return exact;
}

XaProgramSemanticClosurePublishStatus
xa_program_semantic_closure_publish_scalar_module_graph(
    XaAnalyzer *analyzer, const XrModuleGraph *graph, XrProgramSemanticClosure **out,
    char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!out || !analyzer || !graph)
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    if (graph->spec_count != 2 || graph->entry_index < 0 ||
        graph->entry_index >= graph->spec_count)
        return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;

    const XrModuleSpec *entry = &graph->specs[graph->entry_index];
    int dependency_index = graph->entry_index == 0 ? 1 : 0;
    const XrModuleSpec *dependency = &graph->specs[dependency_index];
    const AstNode *entry_syntax = entry->ast;
    const AstNode *dependency_syntax = dependency->ast;
    if (!entry_syntax || !dependency_syntax || entry_syntax->type != AST_PROGRAM ||
        dependency_syntax->type != AST_PROGRAM || !entry_syntax->as.program.statements ||
        !dependency_syntax->as.program.statements || entry_syntax->as.program.count != 2 ||
        dependency_syntax->as.program.count != 1)
        return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;

    const AstNode *import_node = NULL;
    const AstNode *entry_function = NULL;
    for (int i = 0; i < entry_syntax->as.program.count; i++) {
        const AstNode *statement = entry_syntax->as.program.statements[i];
        if (statement && statement->type == AST_IMPORT_STMT && !import_node)
            import_node = statement;
        else if (statement && statement->type == AST_FUNCTION_DECL && !entry_function)
            entry_function = statement;
        else
            return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;
    }
    const AstNode *exported_function = dependency_syntax->as.program.statements[0];
    if (!import_node || !entry_function || !exported_function ||
        exported_function->type != AST_FUNCTION_DECL || !exported_function->is_exported)
        return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;

    const ImportStmtNode *import = &import_node->as.import_stmt;
    if (import->member_count != 1 || !import->members || import->alias)
        return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;

    /* The syntax now claims the bounded family. Every remaining condition is
     * authority, so a mismatch must not fall through to an ordinary graph. */
    if (entry->status != XR_MODSPEC_ANALYZED || dependency->status != XR_MODSPEC_ANALYZED ||
        entry->dep_count != 1 || !entry->dep_indices ||
        entry->dep_indices[0] != dependency_index || dependency->dep_count != 0 ||
        graph->has_cycle || graph->topo_count != 2 || !graph->topo_order ||
        !scalar_graph_import_targets_dependency(graph, entry, dependency, import)) {
        bridge_fail(error, error_size,
                    "two-module scalar graph has no exact dependency authority");
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }

    const XaSymbol *entry_symbol = NULL;
    const XaSymbol *exported_symbol = NULL;
    if (!scalar_graph_function_exact(analyzer, entry_function, 0, false, &entry_symbol) ||
        !scalar_graph_function_exact(analyzer, exported_function, 1, true, &exported_symbol)) {
        bridge_fail(error, error_size,
                    "two-module scalar graph function authority is incomplete");
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }
    const ImportMember *member = &import->members[0];
    const XaSymbol *imported_symbol =
        member->symbol_id ? xa_analyzer_symbol_by_id(analyzer, member->symbol_id) : NULL;
    const XaSymbol *export_table_symbol =
        dependency->export_symbols && member->name
            ? (const XaSymbol *) xr_hashmap_get(dependency->export_symbols, member->name)
            : NULL;
    if (!member->name || !member->symbol_id || !exported_function->as.function_decl.name ||
        strcmp(member->name, exported_function->as.function_decl.name) != 0 ||
        !scalar_graph_import_symbol_exact(analyzer, imported_symbol) || !export_table_symbol) {
        bridge_fail(error, error_size,
                    "two-module scalar import has no exact exported target");
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }

    XaLeafCallScan entry_scan = {0};
    XaLeafCallScan dependency_scan = {0};
    if (!leaf_scan_calls(entry_function->as.function_decl.body, &entry_scan) ||
        entry_scan.unsupported || !entry_scan.call ||
        !leaf_scan_calls(exported_function->as.function_decl.body, &dependency_scan) ||
        dependency_scan.unsupported || dependency_scan.call) {
        bridge_fail(error, error_size,
                    "two-module scalar graph call inventory is not exact");
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }
    const AstNode *call_node = entry_scan.call;
    const CallExprNode *call = &call_node->as.call_expr;
    const XrType *call_type = xa_analyzer_get_node_type(analyzer, call_node);
    const XrType *argument_type =
        call->arguments && call->arg_count == 1
            ? xa_analyzer_get_node_type(analyzer, call->arguments[0])
            : NULL;
    if (call->arg_count != 1 || !call->arguments || !call->arguments[0] || !call->callee ||
        call->callee->type != AST_VARIABLE || call->default_arg_count != 0 ||
        call->type_arg_count != 0) {
        bridge_fail(error, error_size, "two-module scalar call shape authority is incomplete");
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }
    const XaSymbol *call_symbol = call->callee->as.variable.symbol_id
                                      ? xa_analyzer_symbol_by_id(
                                            analyzer, call->callee->as.variable.symbol_id)
                                      : NULL;
    if (!scalar_graph_import_symbol_exact(analyzer, call_symbol)) {
        bridge_fail(error, error_size, "two-module scalar call target authority is incomplete");
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }
    if (call->arg_accesses && call->arg_accesses[0] != XR_CALL_ARG_PLAIN) {
        bridge_fail(error, error_size, "two-module scalar call access authority is incomplete");
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }
    if (!leaf_exact_scalar(call_type) ||
        leaf_exact_scalar(call_type)->id != XR_EXACT_SCALAR_I64 ||
        !leaf_exact_scalar(argument_type) ||
        leaf_exact_scalar(argument_type)->id != XR_EXACT_SCALAR_I64) {
        bridge_fail(error, error_size, "two-module scalar call type authority is incomplete");
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }
    if (!leaf_locator_valid(leaf_locator(call_node))) {
        bridge_fail(error, error_size, "two-module scalar call locator authority is incomplete");
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }

    XrProgramSemanticModuleInput entry_module;
    XrProgramSemanticModuleInput dependency_module;
    if (!leaf_module_authority(entry, &entry_module) ||
        !leaf_module_authority(dependency, &dependency_module)) {
        bridge_fail(error, error_size,
                    "two-module scalar source authority is incomplete");
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }
    XrScalarI64FunctionContract nullary;
    XrScalarI64FunctionContract unary;
    XrFingerprint call_contract;
    if (!xr_scalar_i64_function_contract(XR_SCALAR_I64_FUNCTION_NULLARY, &nullary) ||
        !xr_scalar_i64_function_contract(XR_SCALAR_I64_FUNCTION_UNARY, &unary) ||
        !xr_scalar_i64_call_contract(&unary, &call_contract)) {
        bridge_fail(error, error_size,
                    "two-module scalar registry authority is incomplete");
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }

    XrSHA256Context policy_context;
    leaf_hash_begin(&policy_context, "xray-source-scalar-module-graph-policy-v1");
    leaf_hash_u32(&policy_context, 2);
    leaf_hash_u32(&policy_context, 1);
    leaf_hash_u32(&policy_context, 1);
    leaf_hash_u32(&policy_context, 0);
    leaf_hash_u32(&policy_context, 2);
    leaf_hash_u32(&policy_context, 1);
    leaf_hash_u32(&policy_context, 1);
    XrFingerprint policy = leaf_finish_fingerprint(&policy_context);

    XrProgramSemanticSourceLocator entry_locator = leaf_locator(entry_function);
    XrProgramSemanticSourceLocator exported_locator = leaf_locator(exported_function);
    XrStableId entry_declaration = scalar_graph_function_declaration(&entry_module, entry_locator,
                                                                     nullary.signature_fingerprint);
    XrStableId exported_declaration = scalar_graph_function_declaration(
        &dependency_module, exported_locator, unary.signature_fingerprint);
    XrStableId member_declaration = {{0}};
    XrStableId export_table_declaration = {{0}};
    XrStableId call_declaration = {{0}};
    if (!scalar_graph_symbol_export_declaration(analyzer, imported_symbol, true, &dependency_module,
                                                unary.signature_fingerprint, &member_declaration) ||
        !scalar_graph_symbol_export_declaration(analyzer, export_table_symbol, false,
                                                &dependency_module, unary.signature_fingerprint,
                                                &export_table_declaration) ||
        !scalar_graph_symbol_export_declaration(analyzer, call_symbol, true, &dependency_module,
                                                unary.signature_fingerprint, &call_declaration) ||
        !stable_id_equal(exported_declaration, member_declaration) ||
        !stable_id_equal(exported_declaration, export_table_declaration) ||
        !stable_id_equal(exported_declaration, call_declaration)) {
        bridge_fail(error, error_size,
                    "two-module scalar bindings do not rejoin one stable export declaration");
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }
    XrStableId entry_instance =
        scalar_graph_function_instance(entry_declaration, nullary.signature_fingerprint);
    XrStableId exported_instance =
        scalar_graph_function_instance(exported_declaration, unary.signature_fingerprint);
    XrProgramSemanticFunctionInput exported_identity_input = {
        .module_identity = dependency_module.module_identity,
        .declaration_identity = exported_declaration,
        .concrete_instance_identity = exported_instance,
        .signature_fingerprint = unary.signature_fingerprint,
        .effect_fingerprint = unary.effect_fingerprint,
        .capability_mask = unary.capability_mask,
    };
    XrStableId expected_exported_function_id = {{0}};
    if (!xr_program_semantic_function_identity(policy, &exported_identity_input,
                                               &expected_exported_function_id) ||
        !xr_source_semantic_scalar_i64_export_fingerprint(
            &dependency_module, exported_declaration, expected_exported_function_id,
            unary.signature_fingerprint, unary.effect_fingerprint, unary.capability_mask,
            &dependency_module.export_fingerprint)) {
        bridge_fail(error, error_size, "two-module scalar export identity authority is incomplete");
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }
    XrProgramSemanticClosureLimits limits = {
        .max_modules = 2,
        .max_dependencies = 1,
        .max_types = 1,
        .max_type_fields = 0,
        .max_functions = 2,
        .max_function_parameters = 1,
        .max_calls = 1,
    };
    XrProgramSemanticClosure *closure = NULL;
    if (!xr_program_semantic_closure_create(&limits, policy, &closure, error, error_size))
        return XA_PROGRAM_SEMANTIC_CLOSURE_RESOURCE_FAILURE;
    bool ok = xr_program_semantic_closure_set_family(
        closure, XR_PROGRAM_SEMANTIC_FAMILY_SCALAR_MODULE_GRAPH_DIRECT_CALL, error, error_size);
    XrProgramSemanticTypeInput scalar_input;
    XrStableId i64_type = {{0}};
    ok = ok && xr_program_semantic_exact_scalar_type_input(XR_EXACT_SCALAR_I64, &scalar_input) &&
         xr_program_semantic_closure_add_type(closure, &scalar_input, &i64_type, error,
                                              error_size) &&
         xr_program_semantic_closure_add_module(closure, &entry_module, error, error_size) &&
         xr_program_semantic_closure_add_module(closure, &dependency_module, error, error_size);
    XrProgramSemanticFunctionInput entry_input = {
        .module_identity = entry_module.module_identity,
        .declaration_identity = entry_declaration,
        .concrete_instance_identity = entry_instance,
        .declaration_locator = entry_locator,
        .signature_fingerprint = nullary.signature_fingerprint,
        .effect_fingerprint = nullary.effect_fingerprint,
        .return_type = i64_type,
        .capability_mask = nullary.capability_mask,
        .flags = XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY,
    };
    XrProgramSemanticFunctionParameterInput exported_parameter = {
        .type = i64_type,
        .declaration_ordinal = 0,
        .mode = XR_PARAM_READ,
    };
    XrProgramSemanticFunctionInput exported_input = {
        .module_identity = dependency_module.module_identity,
        .declaration_identity = exported_declaration,
        .concrete_instance_identity = exported_instance,
        .declaration_locator = exported_locator,
        .signature_fingerprint = unary.signature_fingerprint,
        .effect_fingerprint = unary.effect_fingerprint,
        .return_type = i64_type,
        .parameters = &exported_parameter,
        .parameter_count = 1,
        .capability_mask = unary.capability_mask,
        .flags = XR_PROGRAM_SEMANTIC_FUNCTION_EXPORTED,
    };
    XrStableId entry_function_id = {{0}};
    XrStableId exported_function_id = {{0}};
    ok = ok && xr_program_semantic_closure_add_function(
                   closure, &entry_input, &entry_function_id, error, error_size) &&
         xr_program_semantic_closure_add_function(
             closure, &exported_input, &exported_function_id, error, error_size);
    if (ok && !stable_id_equal(exported_function_id, expected_exported_function_id))
        ok = bridge_fail(error, error_size,
                         "two-module scalar exported function identity did not rejoin");
    XrStableId resolver_binding = {{0}};
    if (ok && !xr_source_semantic_scalar_i64_import_binding(
                  &entry_module, &dependency_module, leaf_locator(import_node),
                  exported_declaration, exported_function_id, i64_type, unary.signature_fingerprint,
                  unary.effect_fingerprint, unary.capability_mask, &resolver_binding))
        ok = bridge_fail(error, error_size,
                         "two-module scalar resolver binding authority is incomplete");
    XrProgramSemanticDependencyInput dependency_input = {
        .source_module = entry_module.module_identity,
        .dependency_module = dependency_module.module_identity,
        .import_locator = leaf_locator(import_node),
        .exported_declaration = exported_declaration,
        .exported_function = exported_function_id,
        .resolver_binding = resolver_binding,
        .contract_fingerprint = scalar_graph_dependency_contract(
            &entry_module, &dependency_module, exported_declaration, exported_function_id,
            resolver_binding, i64_type, unary.signature_fingerprint, unary.effect_fingerprint,
            unary.capability_mask, leaf_locator(import_node)),
        .kind = XR_PROGRAM_SEMANTIC_DEPENDENCY_SELECTIVE_FUNCTION_IMPORT,
    };
    ok = ok &&
         xr_program_semantic_closure_add_dependency(closure, &dependency_input, error, error_size);
    XrStableId callsite = {{0}};
    XrProgramSemanticSourceLocator call_locator = leaf_locator(call_node);
    ok = ok && xr_source_semantic_callsite_identity(
                   entry_module.source_fingerprint, entry_module.module_identity,
                   entry_declaration, call_locator, &callsite);
    XrProgramSemanticCallInput call_input = {
        .callsite_identity = callsite,
        .locator = call_locator,
        .caller_function = entry_function_id,
        .callee_function = exported_function_id,
        .resolver_binding = dependency_input.resolver_binding,
        .contract_fingerprint = call_contract,
    };
    XrStableId ignored_call = {{0}};
    ok = ok && xr_program_semantic_closure_add_call(
                   closure, &call_input, &ignored_call, error, error_size) &&
         xr_program_semantic_closure_freeze(closure, error, error_size) &&
         xr_program_semantic_closure_verify(closure, error, error_size);
    if (!ok) {
        XrProgramSemanticClosureFailureKind failure =
            xr_program_semantic_closure_failure_kind(closure);
        xr_program_semantic_closure_free(closure);
        return failure == XR_PROGRAM_SEMANTIC_CLOSURE_FAILURE_RESOURCE
                   ? XA_PROGRAM_SEMANTIC_CLOSURE_RESOURCE_FAILURE
                   : XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    }
    *out = closure;
    return XA_PROGRAM_SEMANTIC_CLOSURE_READY;
}
