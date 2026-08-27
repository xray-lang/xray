/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * Analyzer-owned publication for the closed i64 overflow predicate family.
 */

#include "xa_i64_overflow_program.h"
#include "xa_resolved_call.h"
#include "xa_selection.h"
#include "xanalyzer.h"
#include "xanalyzer_builtins.h"
#include "xanalyzer_symbol.h"
#include "../parser/xast_nodes.h"
#include "../parser/xast_walk.h"
#include "../../base/xsha256.h"
#include "../../module/xmodule_graph.h"
#include "../../plan/semantic/xr_i64_overflow_predicate_semantics.h"
#include "../../plan/semantic/xr_source_semantic_identity.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include <string.h>

enum { XA_I64_OVERFLOW_MAX_CALLS = 16 };

typedef struct XaI64OverflowCallFact {
    const AstNode *node;
    XrI64OverflowPredicateKind kind;
} XaI64OverflowCallFact;

typedef struct XaI64OverflowScan {
    XaI64OverflowCallFact calls[XA_I64_OVERFLOW_MAX_CALLS];
    uint32_t count;
    bool unsupported;
} XaI64OverflowScan;

typedef struct XaI64OverflowScanContext {
    XaI64OverflowScan *scan;
    XaAnalyzer *analyzer;
} XaI64OverflowScanContext;

static bool bytes_zero(const uint8_t *bytes, size_t size) {
    uint8_t combined = 0;
    for (size_t i = 0; i < size; i++)
        combined |= bytes[i];
    return combined == 0;
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

static void hash_id(XrSHA256Context *context, XrStableId id) {
    hash_bytes(context, id.bytes, sizeof(id.bytes));
}

static void hash_fingerprint(XrSHA256Context *context, XrFingerprint fingerprint) {
    hash_bytes(context, fingerprint.bytes, sizeof(fingerprint.bytes));
}

static void hash_locator(XrSHA256Context *context, XrProgramSemanticSourceLocator locator) {
    hash_u32(context, locator.kind);
    hash_u32(context, locator.start_line);
    hash_u32(context, locator.start_column);
    hash_u32(context, locator.end_line);
    hash_u32(context, locator.end_column);
}

static XrStableId finish_id(XrSHA256Context *context) {
    uint8_t digest[XR_FINGERPRINT_BYTES];
    XrStableId result;
    xr_sha256_final(context, digest);
    memcpy(result.bytes, digest, sizeof(result.bytes));
    return result;
}

static XrProgramSemanticSourceLocator locator(const AstNode *node) {
    return node ? (XrProgramSemanticSourceLocator) {
                      .kind = (uint32_t) node->type,
                      .start_line = node->line > 0 ? (uint32_t) node->line : 0,
                      .start_column = node->column > 0 ? (uint32_t) node->column : 0,
                      .end_line = node->end_line > 0 ? (uint32_t) node->end_line : 0,
                      .end_column = node->end_column > 0 ? (uint32_t) node->end_column : 0,
                  }
                : (XrProgramSemanticSourceLocator) {0};
}

static bool locator_valid(XrProgramSemanticSourceLocator value, uint32_t kind) {
    return value.kind == kind && value.start_line && value.start_column && value.end_line &&
           value.end_column &&
           (value.end_line > value.start_line ||
            (value.end_line == value.start_line && value.end_column > value.start_column));
}

static bool exact_i64(const XrType *type) {
    return type && type->kind == XR_KIND_INT && type->scalar_rep == XR_NATIVE_I64 &&
           !type->is_nullable && !type->is_const && !type->is_literal;
}

static bool exact_bool(const XrType *type) {
    return type && type->kind == XR_KIND_BOOL &&
           (type->scalar_rep == XR_SCALAR_REP_NONE || type->scalar_rep == XR_NATIVE_BOOL) &&
           !type->is_nullable && !type->is_const && !type->is_literal;
}

static const XrType *expression_type(XaAnalyzer *analyzer, const AstNode *node) {
    const XrType *type = node ? xa_analyzer_get_node_type(analyzer, node) : NULL;
    if (type && type->kind != XR_KIND_UNKNOWN && type->kind != XR_KIND_ERROR)
        return type;
    if (node && node->type == AST_VARIABLE && node->as.variable.symbol_id) {
        const XaSymbol *symbol =
            xa_analyzer_symbol_by_id(analyzer, node->as.variable.symbol_id);
        return symbol ? symbol->links.type : NULL;
    }
    return NULL;
}

static bool builtin_contract_exact(const XrType *receiver, const char *name) {
    const XaBuiltinMember *members = NULL;
    int count = xa_builtin_get_members_for_type((XrType *) receiver, &members);
    const XaBuiltinMember *match = NULL;
    for (int i = 0; members && i < count; i++) {
        const XaBuiltinMember *candidate = &members[i];
        if (!candidate->name || strcmp(candidate->name, name) != 0)
            continue;
        if (match)
            return false;
        match = candidate;
    }
    return match && match->signature && match->is_method && !match->is_static &&
           !match->is_internal && !match->is_yieldable &&
           match->effect_contract.kind == XA_EFFECT_CONTRACT_NOTHROW &&
           !match->effect_contract.errors && match->effect_contract.error_count == 0 &&
           match->allocation_contract == XA_ALLOCATION_CONTRACT_NO_HEAP &&
           !match->mutates_receiver &&
           match->return_ownership == XA_BUILTIN_RETURN_UNKNOWN;
}

static bool kind_from_member(const char *name, XrI64OverflowPredicateKind *kind) {
    if (kind)
        *kind = XR_I64_OVERFLOW_PREDICATE_INVALID;
    if (!name || !kind)
        return false;
    if (strcmp(name, "addOverflows") == 0)
        *kind = XR_I64_OVERFLOW_PREDICATE_ADD;
    else if (strcmp(name, "subOverflows") == 0)
        *kind = XR_I64_OVERFLOW_PREDICATE_SUB;
    else if (strcmp(name, "mulOverflows") == 0)
        *kind = XR_I64_OVERFLOW_PREDICATE_MUL;
    return *kind != XR_I64_OVERFLOW_PREDICATE_INVALID;
}

static bool exact_call(XaAnalyzer *analyzer, const AstNode *node,
                       XrI64OverflowPredicateKind *kind) {
    const CallExprNode *call = node && node->type == AST_CALL_EXPR ? &node->as.call_expr : NULL;
    const AstNode *callee = call ? call->callee : NULL;
    const MemberAccessNode *member =
        callee && callee->type == AST_MEMBER_ACCESS ? &callee->as.member_access : NULL;
    const XaSelection *selection = callee ? xa_analyzer_get_selection(analyzer, callee) : NULL;
    const XaResolvedCall *resolved = node ? xa_analyzer_get_resolved_call(analyzer, node) : NULL;
    const XrType *receiver = member ? expression_type(analyzer, member->object) : NULL;
    const XrType *argument = call && call->arguments && call->arg_count == 1
                                 ? expression_type(analyzer, call->arguments[0])
                                 : NULL;
    const XrType *result = node ? xa_analyzer_get_node_type(analyzer, node) : NULL;
    XrI64OverflowPredicateKind selected = XR_I64_OVERFLOW_PREDICATE_INVALID;
    if (!call || !member || !call->arguments || call->arg_count != 1 ||
        call->default_arg_count != 0 || call->type_arg_count != 0 ||
        (call->arg_accesses && call->arg_accesses[0] != XR_CALL_ARG_PLAIN) ||
        selection || resolved || !member->name || !kind_from_member(member->name, &selected) ||
        !exact_i64(receiver) || !exact_i64(argument) || !exact_bool(result) ||
        !builtin_contract_exact(receiver, member->name))
        return false;
    *kind = selected;
    return true;
}

static bool scan_node(AstNode *node, void *context);

static bool scan_tree(const AstNode *node, XaI64OverflowScan *scan, XaAnalyzer *analyzer) {
    if (!node || !scan || scan->unsupported)
        return false;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR) {
        scan->unsupported = true;
        return false;
    }
    if (node->type == AST_CALL_EXPR) {
        XrI64OverflowPredicateKind kind = XR_I64_OVERFLOW_PREDICATE_INVALID;
        if (scan->count >= XA_I64_OVERFLOW_MAX_CALLS || !exact_call(analyzer, node, &kind)) {
            scan->unsupported = true;
            return false;
        }
        scan->calls[scan->count++] = (XaI64OverflowCallFact) {node, kind};
    }
    XaI64OverflowScanContext child = {scan, analyzer};
    if (!xr_ast_for_each_child(node, scan_node, &child)) {
        scan->unsupported = true;
        return false;
    }
    return true;
}

static bool scan_node(AstNode *node, void *context) {
    XaI64OverflowScanContext *child = (XaI64OverflowScanContext *) context;
    return !node || scan_tree(node, child->scan, child->analyzer);
}

static bool pure_function(XaAnalyzer *analyzer, const XaSymbol *symbol) {
    const XaSymbolLinks *links = symbol ? &symbol->links : NULL;
    const XaEffectSummary *effect =
        links && links->summary_owner == analyzer && links->effect_id != XA_EFFECT_NONE
            ? xa_effect_db_get(analyzer->effect_db, links->effect_id)
            : NULL;
    const XaMemoryEffectSummary *memory =
        links && links->summary_owner == analyzer && links->memory_effect_id != XA_MEMORY_EFFECT_NONE
            ? xa_memory_effect_db_get(analyzer->memory_effect_db, links->memory_effect_id)
            : NULL;
    bool exact_read_parameters = memory && memory->root_count == 2 && memory->roots;
    for (uint32_t i = 0; exact_read_parameters && i < memory->root_count; i++) {
        const XaMemoryRootEffect *root = &memory->roots[i];
        exact_read_parameters = root->root.kind == XA_MEMORY_ROOT_PARAM &&
                                root->root.index == i && root->write_count == 0 &&
                                !root->descriptor_rebind &&
                                root->relocation == XA_MEMORY_ADDRESS_STABLE &&
                                root->shortening == XA_MEMORY_NEVER_SHORTENS &&
                                root->shortening_range == XA_MEMORY_RANGE_EXPR_NONE &&
                                root->invalidation <= XA_MEMORY_INVALIDATES_VIEWS;
    }
    return links && effect && memory && effect->completeness == XA_EFFECT_COMPLETE &&
           effect->unknown_reasons == 0 && effect->semantic_effects == XA_SEM_EFFECT_NONE &&
           effect->unknown_semantic_effects == XA_SEM_EFFECT_NONE &&
           effect->error_set_completeness == XA_EFFECT_COMPLETE &&
           effect->error_unknown_reasons == 0 && effect->escaping.count == 0 &&
           !effect->contains_unsafe_op && !effect->requires_unsafe_at_call &&
           memory->completeness == XA_EFFECT_COMPLETE && memory->unknown_reasons == 0 &&
           exact_read_parameters && links->alloc_effect_complete &&
           links->alloc_state == XA_ALLOC_PROVEN_NONE &&
           links->alloc_reason_bits == XA_ALLOC_REASON_NONE;
}

static XrStableId function_declaration(const XrProgramSemanticModuleInput *module,
                                       XrProgramSemanticSourceLocator span,
                                       XrFingerprint signature) {
    static const uint8_t domain[] = "xray-source-i64-overflow-entry-declaration-v1";
    XrSHA256Context context;
    xr_sha256_init(&context);
    hash_bytes(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    hash_id(&context, module->module_identity);
    hash_fingerprint(&context, module->source_fingerprint);
    hash_locator(&context, span);
    hash_fingerprint(&context, signature);
    return finish_id(&context);
}

static XrStableId function_instance(XrStableId declaration, XrFingerprint signature) {
    static const uint8_t domain[] = "xray-source-i64-overflow-entry-instance-v1";
    XrSHA256Context context;
    xr_sha256_init(&context);
    hash_bytes(&context, domain, sizeof(domain) - 1u);
    hash_u32(&context, XR_PROGRAM_SEMANTIC_CLOSURE_SCHEMA_VERSION);
    hash_id(&context, declaration);
    hash_fingerprint(&context, signature);
    return finish_id(&context);
}

XaProgramSemanticClosurePublishStatus xa_i64_overflow_program_publish(
    XaAnalyzer *analyzer, const AstNode *syntax, const XrModuleSpec *module_spec,
    XrProgramSemanticClosure **out, char *error, size_t error_size) {
    if (out)
        *out = NULL;
    if (!out || !analyzer || !syntax)
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    if (syntax->type != AST_PROGRAM || syntax->as.program.count != 1 ||
        !syntax->as.program.statements || !syntax->as.program.statements[0] ||
        syntax->as.program.statements[0]->type != AST_FUNCTION_DECL)
        return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;
    const AstNode *node = syntax->as.program.statements[0];
    const FunctionDeclNode *function = &node->as.function_decl;
    XaSymbol *symbol = function->symbol_id ? xa_analyzer_symbol_by_id(analyzer, function->symbol_id)
                                           : NULL;
    const XrType *type = symbol ? symbol->links.type : NULL;
    if (!symbol || !type)
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    if (function->type_param_count != 0 || function->is_generator || function->is_extern ||
        !function->body || function->param_count != 2 || node->is_exported ||
        symbol->kind != XA_SYM_FUNCTION || symbol->is_builtin || symbol->is_imported ||
        symbol->is_exported || symbol->parent || symbol->links.summary_owner != analyzer ||
        symbol->links.function_decl_node != node || type->kind != XR_KIND_FUNCTION ||
        type->function.param_count != 2 || type->function.min_params != 2 ||
        type->function.is_variadic || type->function.is_c_abi ||
        type->function.throw_effect != XR_FN_EFFECT_NO_THROW || type->function.type_param_count != 0 ||
        !type->function.params || type->function.params[0].mode != XR_PARAM_READ ||
        type->function.params[1].mode != XR_PARAM_READ ||
        !exact_i64(type->function.params[0].type) || !exact_i64(type->function.params[1].type) ||
        !exact_i64(type->function.return_type) || !pure_function(analyzer, symbol)) {
        return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;
    }
    XrProgramSemanticSourceLocator function_locator = locator(node);
    if (!locator_valid(function_locator, AST_FUNCTION_DECL))
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    XaI64OverflowScan scan = {0};
    if (!scan_tree(function->body, &scan, analyzer) || scan.unsupported || scan.count == 0)
        return XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED;
    XrProgramSemanticModuleInput module;
    if (!module_spec || module_spec->ast != syntax || module_spec->status != XR_MODSPEC_ANALYZED ||
        !module_spec->canonical || module_spec->export_symbols_invalid ||
        bytes_zero(module_spec->source_content_fingerprint.bytes,
                   sizeof(module_spec->source_content_fingerprint.bytes)) ||
        !xr_source_semantic_module_authority(module_spec->canonical,
                                             module_spec->source_content_fingerprint, &module,
                                             NULL))
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    XrFingerprint policy;
    XrFingerprint signature;
    XrFingerprint effect;
    if (!xr_i64_overflow_predicate_policy(&policy) ||
        !xr_i64_overflow_predicate_entry_signature(&signature) ||
        !xr_i64_overflow_predicate_entry_effect(&effect))
        return XA_PROGRAM_SEMANTIC_CLOSURE_INVALID;
    XrProgramSemanticClosureLimits limits = {
        .max_modules = 1,
        .max_dependencies = 0,
        .max_types = 1,
        .max_type_fields = 0,
        .max_functions = 1,
        .max_function_parameters = 2,
        .max_calls = scan.count,
    };
    XrProgramSemanticClosure *closure = NULL;
    if (!xr_program_semantic_closure_create(&limits, policy, &closure, error, error_size))
        return XA_PROGRAM_SEMANTIC_CLOSURE_RESOURCE_FAILURE;
    XrStableId i64_type = {{0}};
    XrProgramSemanticTypeInput i64_input;
    XrStableId declaration = function_declaration(&module, function_locator, signature);
    XrProgramSemanticFunctionParameterInput parameters[2] = {
        {.declaration_ordinal = 0, .mode = XR_PARAM_READ},
        {.declaration_ordinal = 1, .mode = XR_PARAM_READ},
    };
    bool ok = xr_program_semantic_closure_set_family(
                  closure, XR_PROGRAM_SEMANTIC_FAMILY_I64_OVERFLOW_PREDICATE, error, error_size) &&
              xr_program_semantic_closure_add_module(closure, &module, error, error_size) &&
              xr_program_semantic_exact_scalar_type_input(XR_EXACT_SCALAR_I64, &i64_input) &&
              xr_program_semantic_closure_add_type(closure, &i64_input, &i64_type, error,
                                                   error_size);
    parameters[0].type = i64_type;
    parameters[1].type = i64_type;
    XrStableId function_id = {{0}};
    if (ok) {
        XrProgramSemanticFunctionInput input = {
            .module_identity = module.module_identity,
            .declaration_identity = declaration,
            .concrete_instance_identity = function_instance(declaration, signature),
            .declaration_locator = function_locator,
            .signature_fingerprint = signature,
            .effect_fingerprint = effect,
            .return_type = i64_type,
            .parameters = parameters,
            .parameter_count = 2,
            .flags = XR_PROGRAM_SEMANTIC_FUNCTION_ENTRY,
        };
        ok = xr_program_semantic_closure_add_function(closure, &input, &function_id, error,
                                                      error_size);
    }
    for (uint32_t i = 0; ok && i < scan.count; i++) {
        XrProgramSemanticSourceLocator call_locator = locator(scan.calls[i].node);
        XrStableId callsite = {{0}};
        XrStableId builtin = {{0}};
        XrFingerprint contract = {{0}};
        XrStableId ignored = {{0}};
        ok = locator_valid(call_locator, AST_CALL_EXPR) &&
             xr_source_semantic_callsite_identity(module.source_fingerprint,
                                                  module.module_identity, declaration,
                                                  call_locator, &callsite) &&
             xr_i64_overflow_predicate_builtin_identity(scan.calls[i].kind, &builtin) &&
             xr_i64_overflow_predicate_call_contract(scan.calls[i].kind, &contract);
        if (ok) {
            XrProgramSemanticCallInput input = {
                .callsite_identity = callsite,
                .locator = call_locator,
                .caller_function = function_id,
                .callee_function = builtin,
                .contract_fingerprint = contract,
            };
            ok = xr_program_semantic_closure_add_call(closure, &input, &ignored, error,
                                                      error_size);
        }
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
