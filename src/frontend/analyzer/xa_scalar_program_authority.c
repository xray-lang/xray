/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_scalar_program_authority.c - Analyzer-owned scalar snapshot publisher
 */

#include "xa_scalar_program_authority_internal.h"
#include "xa_resolved_call.h"
#include "xanalyzer.h"
#include "xanalyzer_symbol.h"
#include "../parser/xast.h"
#include "../parser/xast_nodes.h"
#include "../../base/xmalloc.h"
#include "../../base/xsha256.h"
#include "../../module/xmodule_graph.h"
#include "../../module/xmodule_identity.h"
#include "../../plan/semantic/xr_scalar_call_semantics.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include <string.h>

typedef struct XaScalarFunctionFact {
    const AstNode *node;
    const XaSymbol *symbol;
    XaScalarFunctionAuthority row;
} XaScalarFunctionFact;

typedef struct XaScalarCallScan {
    const AstNode *call;
    bool unsupported;
} XaScalarCallScan;

static bool bytes_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
}

static int compare_id(XrStableId left, XrStableId right) {
    return memcmp(left.bytes, right.bytes, sizeof(left.bytes));
}

static bool equal_span(XaScalarSourceSpan left, XaScalarSourceSpan right) {
    return left.kind == right.kind && left.start_line == right.start_line &&
           left.start_column == right.start_column && left.end_line == right.end_line &&
           left.end_column == right.end_column;
}

static void hash_u8(XrSHA256Context *context, uint8_t value) {
    xr_sha256_update(context, &value, sizeof(value));
}

static void hash_u32(XrSHA256Context *context, uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_u64(XrSHA256Context *context, uint64_t value) {
    uint8_t bytes[8];
    for (uint32_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(context, bytes, sizeof(bytes));
}

static void hash_bytes(XrSHA256Context *context, const uint8_t *bytes, size_t size) {
    hash_u64(context, (uint64_t) size);
    if (size)
        xr_sha256_update(context, bytes, size);
}

static void hash_text(XrSHA256Context *context, const char *text) {
    hash_bytes(context, (const uint8_t *) text, text ? strlen(text) : 0);
}

static void hash_id(XrSHA256Context *context, XrStableId id) {
    hash_bytes(context, id.bytes, sizeof(id.bytes));
}

static void hash_fingerprint(XrSHA256Context *context, XrFingerprint fingerprint) {
    hash_bytes(context, fingerprint.bytes, sizeof(fingerprint.bytes));
}

static void hash_span(XrSHA256Context *context, XaScalarSourceSpan span) {
    hash_u32(context, span.kind);
    hash_u32(context, span.start_line);
    hash_u32(context, span.start_column);
    hash_u32(context, span.end_line);
    hash_u32(context, span.end_column);
}

static void hash_begin(XrSHA256Context *context, const char *domain) {
    xr_sha256_init(context);
    hash_text(context, domain);
    hash_u32(context, XA_SCALAR_PROGRAM_AUTHORITY_SCHEMA);
}

static XrFingerprint finish_fingerprint(XrSHA256Context *context) {
    XrFingerprint fingerprint;
    xr_sha256_final(context, fingerprint.bytes);
    return fingerprint;
}

static XrStableId finish_id(XrSHA256Context *context) {
    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrStableId id;
    xr_sha256_final(context, digest);
    memcpy(id.bytes, digest, sizeof(id.bytes));
    return id;
}

static XaScalarSourceSpan source_span(const AstNode *node) {
    return node ? (XaScalarSourceSpan) {
                      .kind = (uint32_t) node->type,
                      .start_line = node->line > 0 ? (uint32_t) node->line : 0,
                      .start_column = node->column > 0 ? (uint32_t) node->column : 0,
                      .end_line = node->end_line > 0 ? (uint32_t) node->end_line : 0,
                      .end_column = node->end_column > 0 ? (uint32_t) node->end_column : 0,
                  }
                : (XaScalarSourceSpan) {0};
}

static bool span_valid(XaScalarSourceSpan span, uint32_t kind) {
    if (span.kind != kind || span.start_line == 0 || span.start_column == 0 ||
        span.end_line == 0 || span.end_column == 0)
        return false;
    return span.end_line > span.start_line ||
           (span.end_line == span.start_line && span.end_column > span.start_column);
}

static XrFingerprint scalar_policy(void) {
    XrSHA256Context context;
    hash_begin(&context, "xray-pre-xi-scalar-authority-policy-v1");
    hash_u32(&context, XA_SCALAR_PROGRAM_FUNCTION_COUNT);
    hash_u32(&context, 1);
    return finish_fingerprint(&context);
}

static bool module_authority(const XrModuleSpec *spec, XaScalarModuleAuthority *out) {
    if (!spec || !out || spec->status != XR_MODSPEC_ANALYZED || !spec->ast ||
        !spec->canonical || !xr_module_identity_valid(spec->canonical, NULL) ||
        spec->export_symbols_invalid ||
        bytes_zero(spec->source_content_fingerprint.bytes,
                   sizeof(spec->source_content_fingerprint.bytes)))
        return false;
    XrSHA256Context context;
    hash_begin(&context, "xray-source-module-authority-v1");
    hash_text(&context, spec->canonical);
    XrFingerprint full = finish_fingerprint(&context);

    hash_begin(&context, "xray-source-module-identity-v1");
    hash_fingerprint(&context, full);
    XrStableId identity = finish_id(&context);

    hash_begin(&context, "xray-source-module-empty-exports-v1");
    hash_id(&context, identity);
    hash_u32(&context, 0);
    *out = (XaScalarModuleAuthority) {
        .module_identity = identity,
        .module_authority_fingerprint = full,
        .source_fingerprint = spec->source_content_fingerprint,
        .export_fingerprint = finish_fingerprint(&context),
    };
    return true;
}

static bool exact_i64(const XrType *type, XrExactScalarId *out_id) {
    const XrExactScalarDesc *scalar =
        type ? xr_exact_scalar_by_native_type(type->scalar_rep) : NULL;
    if (!type || !scalar || scalar->id != XR_EXACT_SCALAR_I64 || type->kind != XR_KIND_INT ||
        type->is_nullable || type->is_const || type->is_literal)
        return false;
    if (out_id)
        *out_id = scalar->id;
    return true;
}

static bool signature_fingerprint(const XaSymbol *symbol, XrFingerprint *out) {
    const XrType *type = symbol ? symbol->links.type : NULL;
    XrExactScalarId return_id = XR_EXACT_SCALAR_NONE;
    if (!out || !type || type->kind != XR_KIND_FUNCTION || type->function.param_count < 0 ||
        type->function.param_count > 1 ||
        type->function.min_params != type->function.param_count || type->function.is_variadic ||
        type->function.is_c_abi || type->function.throw_effect != XR_FN_EFFECT_NO_THROW ||
        type->function.type_param_count != 0 ||
        !exact_i64(type->function.return_type, &return_id))
        return false;
    for (int i = 0; i < type->function.param_count; i++) {
        XrExactScalarId param_id = XR_EXACT_SCALAR_NONE;
        if (!exact_i64(type->function.params[i].type, &param_id) ||
            type->function.params[i].mode != XR_PARAM_READ)
            return false;
    }
    XrScalarI64FunctionContract contract;
    if (!xr_scalar_i64_function_contract(
            type->function.param_count == 0 ? XR_SCALAR_I64_FUNCTION_NULLARY
                                            : XR_SCALAR_I64_FUNCTION_UNARY,
            &contract))
        return false;
    *out = contract.signature_fingerprint;
    return true;
}

static bool effect_fingerprint(const XaAnalyzer *analyzer, const XaSymbol *symbol,
                               XrFingerprint *out) {
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
    if (!out || !effect || !memory || effect->completeness != XA_EFFECT_COMPLETE ||
        effect->unknown_reasons != 0 || effect->semantic_effects != XA_SEM_EFFECT_NONE ||
        effect->unknown_semantic_effects != XA_SEM_EFFECT_NONE ||
        effect->error_set_completeness != XA_EFFECT_COMPLETE ||
        effect->error_unknown_reasons != 0 || effect->escaping.count != 0 ||
        effect->contains_unsafe_op || effect->requires_unsafe_at_call ||
        memory->completeness != XA_EFFECT_COMPLETE || memory->unknown_reasons != 0 ||
        memory->root_count != 0 || !links->alloc_effect_complete ||
        links->alloc_state != XA_ALLOC_PROVEN_NONE ||
        links->alloc_reason_bits != XA_ALLOC_REASON_NONE)
        return false;
    XrScalarI64FunctionContract contract;
    if (!xr_scalar_i64_function_contract(XR_SCALAR_I64_FUNCTION_NULLARY,
                                         &contract))
        return false;
    *out = contract.effect_fingerprint;
    return true;
}

static XrStableId declaration_identity(const XaScalarModuleAuthority *module,
                                       XaScalarSourceSpan span,
                                       XrFingerprint signature) {
    XrSHA256Context context;
    hash_begin(&context, "xray-source-scalar-function-declaration-v1");
    hash_id(&context, module->module_identity);
    hash_fingerprint(&context, module->source_fingerprint);
    hash_span(&context, span);
    hash_fingerprint(&context, signature);
    return finish_id(&context);
}

static XrStableId concrete_instance_identity(XrStableId module, XrStableId declaration,
                                             XrFingerprint signature) {
    XrSHA256Context context;
    hash_begin(&context, "xray-source-nongeneric-scalar-instance-v1");
    hash_id(&context, module);
    hash_id(&context, declaration);
    hash_fingerprint(&context, signature);
    return finish_id(&context);
}

static bool function_fact(const XaAnalyzer *analyzer,
                          const XaScalarModuleAuthority *module, const AstNode *node,
                          XaScalarFunctionFact *out) {
    const FunctionDeclNode *function = node && node->type == AST_FUNCTION_DECL
                                           ? &node->as.function_decl
                                           : NULL;
    XaSymbol *symbol = function && function->symbol_id
                           ? xa_analyzer_symbol_by_id((XaAnalyzer *) analyzer,
                                                      function->symbol_id)
                           : NULL;
    XaScalarSourceSpan span = source_span(node);
    if (!out || !function || !symbol || symbol->kind != XA_SYM_FUNCTION ||
        symbol->is_builtin || symbol->is_imported || symbol->is_exported || symbol->parent ||
        node->is_exported || symbol->links.summary_owner != analyzer ||
        symbol->links.function_decl_node != node || symbol->id != function->symbol_id ||
        function->type_param_count != 0 || function->is_generator || function->is_extern ||
        !function->body || !span_valid(span, AST_FUNCTION_DECL))
        return false;
    XrFingerprint signature;
    XrFingerprint effect;
    if (!signature_fingerprint(symbol, &signature) ||
        !effect_fingerprint(analyzer, symbol, &effect))
        return false;
    XrStableId declaration = declaration_identity(module, span, signature);
    *out = (XaScalarFunctionFact) {
        .node = node,
        .symbol = symbol,
        .row = {
            .declaration_identity = declaration,
            .concrete_instance_identity = concrete_instance_identity(
                module->module_identity, declaration, signature),
            .signature_fingerprint = signature,
            .effect_fingerprint = effect,
            .declaration_span = span,
            .parameter_count = (uint8_t) function->param_count,
        },
    };
    return true;
}

static bool scan_call_child(AstNode *child, void *context);

static bool scan_calls(const AstNode *node, XaScalarCallScan *scan) {
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
    if (!xr_ast_for_each_child(node, scan_call_child, scan)) {
        scan->unsupported = true;
        return false;
    }
    return true;
}

static bool scan_call_child(AstNode *child, void *context) {
    return !child || scan_calls(child, (XaScalarCallScan *) context);
}

static XrStableId callsite_identity(const XaScalarModuleAuthority *module,
                                    XrStableId caller, XaScalarSourceSpan span) {
    XrSHA256Context context;
    hash_begin(&context, "xray-source-scalar-callsite-v1");
    hash_fingerprint(&context, module->source_fingerprint);
    hash_id(&context, module->module_identity);
    hash_id(&context, caller);
    hash_span(&context, span);
    return finish_id(&context);
}

static XrFingerprint call_contract(const XaScalarFunctionAuthority *callee) {
    XrScalarI64FunctionContract contract;
    XrFingerprint result = {{0}};
    if (!callee || !xr_scalar_i64_function_contract(
                       XR_SCALAR_I64_FUNCTION_UNARY, &contract) ||
        memcmp(contract.signature_fingerprint.bytes,
               callee->signature_fingerprint.bytes,
               sizeof(contract.signature_fingerprint.bytes)) != 0 ||
        memcmp(contract.effect_fingerprint.bytes, callee->effect_fingerprint.bytes,
               sizeof(contract.effect_fingerprint.bytes)) != 0 ||
        callee->capability_mask != 0 ||
        !xr_scalar_i64_call_contract(&contract, &result))
        return (XrFingerprint) {{0}};
    return result;
}

static XrFingerprint authority_fingerprint(const XaScalarProgramAuthority *authority) {
    XrSHA256Context context;
    hash_begin(&context, "xray-scalar-program-authority-freeze-v1");
    hash_fingerprint(&context, authority->policy_fingerprint);
    hash_id(&context, authority->module.module_identity);
    hash_fingerprint(&context, authority->module.module_authority_fingerprint);
    hash_fingerprint(&context, authority->module.source_fingerprint);
    hash_fingerprint(&context, authority->module.export_fingerprint);
    for (uint32_t i = 0; i < XA_SCALAR_PROGRAM_FUNCTION_COUNT; i++) {
        const XaScalarFunctionAuthority *row = &authority->functions[i];
        hash_id(&context, row->declaration_identity);
        hash_id(&context, row->concrete_instance_identity);
        hash_fingerprint(&context, row->signature_fingerprint);
        hash_fingerprint(&context, row->effect_fingerprint);
        hash_span(&context, row->declaration_span);
        hash_u64(&context, row->capability_mask);
        hash_u8(&context, row->flags);
        hash_u8(&context, row->parameter_count);
    }
    hash_id(&context, authority->call.callsite_identity);
    hash_id(&context, authority->call.caller_declaration_identity);
    hash_id(&context, authority->call.caller_instance_identity);
    hash_id(&context, authority->call.callee_declaration_identity);
    hash_id(&context, authority->call.callee_instance_identity);
    hash_fingerprint(&context, authority->call.contract_fingerprint);
    hash_span(&context, authority->call.callsite_span);
    return finish_fingerprint(&context);
}

static int function_index_for_symbol(const XaScalarFunctionFact facts[2], uint32_t symbol_id) {
    for (int i = 0; i < 2; i++)
        if (facts[i].symbol && facts[i].symbol->id == symbol_id)
            return i;
    return -1;
}

static XaSymbol *candidate_function_symbol(XaAnalyzer *analyzer, const AstNode *node) {
    const FunctionDeclNode *function = node && node->type == AST_FUNCTION_DECL
                                           ? &node->as.function_decl
                                           : NULL;
    XaSymbol *symbol = function && function->symbol_id
                           ? xa_analyzer_symbol_by_id(analyzer, function->symbol_id)
                           : NULL;
    if (!function || !symbol || symbol->kind != XA_SYM_FUNCTION || symbol->is_builtin ||
        symbol->is_imported || symbol->parent || symbol->links.summary_owner != analyzer ||
        symbol->links.function_decl_node != node || symbol->id != function->symbol_id ||
        !function->body)
        return NULL;
    return symbol;
}

static bool candidate_signature_semantics_complete(const XaSymbol *symbol) {
    const XrType *type = symbol ? symbol->links.type : NULL;
    if (!type || type->kind != XR_KIND_FUNCTION || !type->function.return_type ||
        XR_TYPE_IS_UNKNOWN(type->function.return_type) ||
        XR_TYPE_IS_ERROR(type->function.return_type) || type->function.param_count < 0)
        return false;
    for (int i = 0; i < type->function.param_count; i++) {
        if (!type->function.params || !type->function.params[i].type ||
            XR_TYPE_IS_UNKNOWN(type->function.params[i].type) ||
            XR_TYPE_IS_ERROR(type->function.params[i].type))
            return false;
    }
    return true;
}

XaScalarProgramAuthorityStatus xa_scalar_program_authority_publish(
    XaAnalyzer *analyzer, const AstNode *syntax, const XrModuleSpec *module_spec,
    XaScalarProgramAuthority **out) {
    if (out)
        *out = NULL;
    if (!out || !analyzer || !syntax)
        return XA_SCALAR_PROGRAM_AUTHORITY_INVALID;
    if (syntax->type != AST_PROGRAM || syntax->as.program.count != 2 ||
        !syntax->as.program.statements || !syntax->as.program.statements[0] ||
        !syntax->as.program.statements[1] ||
        syntax->as.program.statements[0]->type != AST_FUNCTION_DECL ||
        syntax->as.program.statements[1]->type != AST_FUNCTION_DECL)
        return XA_SCALAR_PROGRAM_AUTHORITY_UNSUPPORTED;

    const AstNode *function_nodes[2] = {
        syntax->as.program.statements[0], syntax->as.program.statements[1]};
    XaScalarCallScan scans[2] = {0};
    const AstNode *call = NULL;
    int caller = -1;
    for (int i = 0; i < 2; i++) {
        if (!scan_calls(function_nodes[i]->as.function_decl.body, &scans[i]) ||
            scans[i].unsupported)
            return XA_SCALAR_PROGRAM_AUTHORITY_UNSUPPORTED;
        if (!scans[i].call)
            continue;
        if (call)
            return XA_SCALAR_PROGRAM_AUTHORITY_UNSUPPORTED;
        call = scans[i].call;
        caller = i;
    }
    if (!call || caller < 0 || call->as.call_expr.arg_count != 1 ||
        !call->as.call_expr.arguments || !call->as.call_expr.arguments[0] ||
        !call->as.call_expr.callee || call->as.call_expr.callee->type != AST_VARIABLE ||
        function_nodes[caller]->as.function_decl.param_count != 0 ||
        function_nodes[1 - caller]->as.function_decl.param_count != 1)
        return XA_SCALAR_PROGRAM_AUTHORITY_UNSUPPORTED;

    XaSymbol *candidate_symbols[2] = {0};
    for (int i = 0; i < 2; i++) {
        candidate_symbols[i] = candidate_function_symbol(analyzer, function_nodes[i]);
        if (!candidate_symbols[i] ||
            !candidate_signature_semantics_complete(candidate_symbols[i]))
            return XA_SCALAR_PROGRAM_AUTHORITY_INVALID;
        XrFingerprint signature;
        if (function_nodes[i]->as.function_decl.type_param_count != 0 ||
            function_nodes[i]->as.function_decl.is_generator ||
            function_nodes[i]->as.function_decl.is_extern || function_nodes[i]->is_exported ||
            candidate_symbols[i]->is_exported ||
            !signature_fingerprint(candidate_symbols[i], &signature))
            return XA_SCALAR_PROGRAM_AUTHORITY_UNSUPPORTED;
    }

    XaScalarModuleAuthority module;
    if (!module_authority(module_spec, &module) || module_spec->ast != syntax)
        return XA_SCALAR_PROGRAM_AUTHORITY_INVALID;

    XaScalarFunctionFact facts[2] = {0};
    for (int i = 0; i < 2; i++) {
        if (!function_fact(analyzer, &module, function_nodes[i], &facts[i]))
            return XA_SCALAR_PROGRAM_AUTHORITY_INVALID;
    }
    XaScalarSourceSpan call_span = source_span(call);
    if (!span_valid(call_span, AST_CALL_EXPR) ||
        call->as.call_expr.callee->as.variable.symbol_id == 0)
        return XA_SCALAR_PROGRAM_AUTHORITY_INVALID;
    const XaResolvedCall *resolved = xa_analyzer_get_resolved_call(analyzer, call);
    if (!resolved || resolved->reason != XA_RESOLVED_CALL_REASON_RESOLVED ||
        resolved->source_node_id != call->node_id || resolved->target_symbol_id == 0 ||
        resolved->intrinsic_id != XA_INTRINSIC_NONE || resolved->flags != 0 ||
        resolved->target_symbol_id != call->as.call_expr.callee->as.variable.symbol_id)
        return XA_SCALAR_PROGRAM_AUTHORITY_INVALID;
    int callee = function_index_for_symbol(facts, resolved->target_symbol_id);
    if (callee < 0 || callee == caller)
        return XA_SCALAR_PROGRAM_AUTHORITY_UNSUPPORTED;
    XrExactScalarId ignored = XR_EXACT_SCALAR_NONE;
    if (!exact_i64(xa_analyzer_get_node_type(analyzer, call), &ignored) ||
        !exact_i64(xa_analyzer_get_node_type(analyzer, call->as.call_expr.arguments[0]),
                   &ignored))
        return XA_SCALAR_PROGRAM_AUTHORITY_INVALID;

    facts[caller].row.flags = XA_SCALAR_PROGRAM_FUNCTION_ENTRY;
    if (equal_span(facts[0].row.declaration_span, facts[1].row.declaration_span) ||
        compare_id(facts[0].row.declaration_identity,
                   facts[1].row.declaration_identity) == 0)
        return XA_SCALAR_PROGRAM_AUTHORITY_INVALID;
    if (compare_id(facts[0].row.declaration_identity,
                   facts[1].row.declaration_identity) > 0) {
        XaScalarFunctionFact swap = facts[0];
        facts[0] = facts[1];
        facts[1] = swap;
        caller = 1 - caller;
        callee = 1 - callee;
    }

    XaScalarProgramAuthority *authority =
        (XaScalarProgramAuthority *) xr_calloc(1, sizeof(*authority));
    if (!authority)
        return XA_SCALAR_PROGRAM_AUTHORITY_RESOURCE_FAILURE;
    authority->schema = XA_SCALAR_PROGRAM_AUTHORITY_SCHEMA;
    authority->policy_fingerprint = scalar_policy();
    authority->module = module;
    authority->functions[0] = facts[0].row;
    authority->functions[1] = facts[1].row;
    authority->call = (XaScalarCallAuthority) {
        .callsite_identity = callsite_identity(
            &module, facts[caller].row.declaration_identity, call_span),
        .caller_declaration_identity = facts[caller].row.declaration_identity,
        .caller_instance_identity = facts[caller].row.concrete_instance_identity,
        .callee_declaration_identity = facts[callee].row.declaration_identity,
        .callee_instance_identity = facts[callee].row.concrete_instance_identity,
        .contract_fingerprint = call_contract(&facts[callee].row),
        .callsite_span = call_span,
    };
    authority->fingerprint = authority_fingerprint(authority);
    authority->verified = 1;
    char verify_error[128];
    if (!xa_scalar_program_authority_verify(authority, verify_error,
                                            sizeof(verify_error))) {
        xa_scalar_program_authority_free(authority);
        return XA_SCALAR_PROGRAM_AUTHORITY_INVALID;
    }
    *out = authority;
    return XA_SCALAR_PROGRAM_AUTHORITY_READY;
}

void xa_scalar_program_authority_free(XaScalarProgramAuthority *authority) {
    if (!authority)
        return;
    memset(authority, 0, sizeof(*authority));
    xr_free(authority);
}

XrFingerprint xa_scalar_program_authority_policy(
    const XaScalarProgramAuthority *authority) {
    return authority && authority->verified ? authority->policy_fingerprint
                                            : (XrFingerprint) {0};
}

XrFingerprint xa_scalar_program_authority_fingerprint(
    const XaScalarProgramAuthority *authority) {
    return authority && authority->verified ? authority->fingerprint
                                            : (XrFingerprint) {0};
}

const XaScalarModuleAuthority *xa_scalar_program_authority_module(
    const XaScalarProgramAuthority *authority) {
    return authority && authority->verified ? &authority->module : NULL;
}

const XaScalarFunctionAuthority *xa_scalar_program_authority_function(
    const XaScalarProgramAuthority *authority, uint32_t index) {
    return authority && authority->verified && index < XA_SCALAR_PROGRAM_FUNCTION_COUNT
               ? &authority->functions[index]
               : NULL;
}

const XaScalarCallAuthority *xa_scalar_program_authority_call(
    const XaScalarProgramAuthority *authority) {
    return authority && authority->verified ? &authority->call : NULL;
}
