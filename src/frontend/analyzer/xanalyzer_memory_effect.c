/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_memory_effect.c - Root-relative memory-effect inference
 */

#include "xanalyzer_memory_effect.h"
#include "xa_memory_effect_db.h"
#include "xa_selection.h"
#include "xanalyzer.h"
#include "xanalyzer_ast_visitor.h"
#include "xanalyzer_symbol.h"
#include "xbuiltin_receiver_registry.h"
#include "../parser/xast_nodes.h"
#include "../../base/xmalloc.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/xerror_codes.h"
#include "../../module/xnative_package.h"
#include "../../toolchain/xcompiler_session.h"
#include <stdio.h>
#include <string.h>

typedef struct XaMemoryFunctionRow {
    AstNode *node;
    XaSymbol *symbol;
    XaMemoryEffectSummary direct;
    XaMemoryEffectSummary result;
} XaMemoryFunctionRow;

typedef struct XaMemoryPass {
    XaAnalyzer *analyzer;
    XaMemoryFunctionRow *rows;
    int row_count;
    int row_capacity;
    bool resource_failure;
} XaMemoryPass;

typedef struct XaMemoryScan {
    XaMemoryPass *pass;
    XaMemoryFunctionRow *row;
    XaMemoryEffectSummary *summary;
    int nested_function_depth;
} XaMemoryScan;

static AstNode *memory_function_body(AstNode *node) {
    if (!node)
        return NULL;
    if (node->type == AST_FUNCTION_DECL)
        return node->as.function_decl.body;
    if (node->type == AST_FUNCTION_EXPR)
        return node->as.function_expr.body;
    if (node->type == AST_METHOD_DECL)
        return node->as.method_decl.body;
    return NULL;
}

static void memory_function_params(AstNode *node, XrParamNode ***params, int *count) {
    *params = NULL;
    *count = 0;
    if (!node)
        return;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR) {
        *params = node->as.function_decl.params;
        *count = node->as.function_decl.param_count;
    } else if (node->type == AST_METHOD_DECL) {
        *params = node->as.method_decl.params;
        *count = node->as.method_decl.param_count;
    }
}

static XaSymbol *memory_symbol_by_id(XaAnalyzer *analyzer, uint32_t id) {
    return analyzer && id ? xa_scope_lookup_by_id(analyzer->global_scope, id) : NULL;
}

static XrType *memory_expr_type(XaMemoryPass *pass, AstNode *expr) {
    if (!pass || !pass->analyzer || !expr)
        return NULL;
    XrType *type = xa_analyzer_get_node_type(pass->analyzer, expr);
    if (type)
        return type;
    while (expr) {
        switch (expr->type) {
            case AST_GROUPING:
                expr = expr->as.grouping;
                continue;
            case AST_FORCE_UNWRAP:
            case AST_MOVE_EXPR:
                expr = expr->as.unary.operand;
                continue;
            case AST_AS_EXPR:
                expr = expr->as.as_expr.expr;
                continue;
            case AST_VARIABLE: {
                XaSymbol *symbol = memory_symbol_by_id(pass->analyzer, expr->as.variable.symbol_id);
                return symbol
                           ? (symbol->links.type ? symbol->links.type : symbol->links.declared_type)
                           : NULL;
            }
            default:
                return NULL;
        }
    }
    return NULL;
}

static XaSymbol *memory_resolve_function_symbol(XaAnalyzer *analyzer, AstNode *node) {
    if (!analyzer || !node)
        return NULL;
    XaScope *scope = xa_scope_find_by_node(analyzer->global_scope, node);
    if (scope && scope->function_symbol)
        return scope->function_symbol;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR)
        return memory_symbol_by_id(analyzer, node->as.function_decl.symbol_id);
    return NULL;
}

static XaMemoryFunctionRow *memory_row_for_symbol(XaMemoryPass *pass, XaSymbol *symbol) {
    if (!pass || !symbol)
        return NULL;
    for (int i = 0; i < pass->row_count; i++) {
        XaSymbol *candidate = pass->rows[i].symbol;
        if (candidate == symbol || (candidate && candidate->id && candidate->id == symbol->id) ||
            (symbol->links.function_decl_node &&
             symbol->links.function_decl_node == pass->rows[i].node))
            return &pass->rows[i];
    }
    return NULL;
}

static bool memory_summary_copy(XaMemoryEffectSummary *target,
                                const XaMemoryEffectSummary *source) {
    xa_memory_effect_summary_init(target);
    if (!source)
        return true;
    if (!xa_memory_effect_summary_add_summary(target, source)) {
        xa_memory_effect_summary_clear(target);
        return false;
    }
    return true;
}

static bool memory_summary_equal(const XaMemoryEffectSummary *a, const XaMemoryEffectSummary *b) {
    if (!a || !b || a->completeness != b->completeness ||
        a->unknown_reasons != b->unknown_reasons || a->root_count != b->root_count ||
        xa_memory_effect_summary_fingerprint(a) != xa_memory_effect_summary_fingerprint(b))
        return false;
    return true;
}

static bool memory_add_row(XaMemoryPass *pass, AstNode *node, XaSymbol *symbol) {
    if (!pass || !node || !symbol || memory_row_for_symbol(pass, symbol))
        return symbol != NULL;
    if (pass->row_count >= pass->row_capacity) {
        int capacity = pass->row_capacity ? pass->row_capacity * 2 : 32;
        XaMemoryFunctionRow *rows = (XaMemoryFunctionRow *) xr_realloc(
            pass->rows, (size_t) capacity * sizeof(XaMemoryFunctionRow));
        if (!rows) {
            pass->resource_failure = true;
            return false;
        }
        memset(&rows[pass->row_capacity], 0,
               (size_t) (capacity - pass->row_capacity) * sizeof(XaMemoryFunctionRow));
        pass->rows = rows;
        pass->row_capacity = capacity;
    }
    XaMemoryFunctionRow *row = &pass->rows[pass->row_count++];
    row->node = node;
    row->symbol = symbol;
    xa_memory_effect_summary_init(&row->direct);
    xa_memory_effect_summary_init(&row->result);
    return true;
}

static void memory_collect_pre(AstNode *node, void *userdata) {
    XaMemoryPass *pass = (XaMemoryPass *) userdata;
    if (pass && node &&
        (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
         node->type == AST_METHOD_DECL))
        memory_add_row(pass, node, memory_resolve_function_symbol(pass->analyzer, node));
}

static bool memory_apply_root_effect(XaMemoryEffectSummary *target, XaMemoryRootRef root,
                                     const XaMemoryRootEffect *effect) {
    if (!target || !effect)
        return false;
    for (uint32_t i = 0; i < effect->write_count; i++) {
        if (!xa_memory_effect_summary_add_write(target, root, effect->writes[i]))
            return false;
    }
    if (effect->descriptor_rebind && !xa_memory_effect_summary_mark_descriptor_rebind(target, root))
        return false;
    if (effect->relocation == XA_MEMORY_MAY_RELOCATE &&
        !xa_memory_effect_summary_mark_relocation(target, root))
        return false;
    if (effect->shortening == XA_MEMORY_MAY_SHORTEN &&
        !xa_memory_effect_summary_mark_shortening(target, root, effect->shortening_range))
        return false;
    if (effect->invalidation == XA_MEMORY_INVALIDATES_VIEWS &&
        !xa_memory_effect_summary_mark_invalidation(target, root))
        return false;
    return true;
}

static bool memory_root_for_expr(XaMemoryFunctionRow *row, AstNode *expr,
                                 XaMemoryRootRef *out_root) {
    if (!row || !expr || !out_root)
        return false;
    for (;;) {
        switch (expr->type) {
            case AST_GROUPING:
                expr = expr->as.grouping;
                continue;
            case AST_FORCE_UNWRAP:
            case AST_MOVE_EXPR:
                expr = expr->as.unary.operand;
                continue;
            case AST_AS_EXPR:
                expr = expr->as.as_expr.expr;
                continue;
            case AST_MEMBER_ACCESS:
                expr = expr->as.member_access.object;
                continue;
            case AST_INDEX_GET:
                expr = expr->as.index_get.array;
                continue;
            case AST_SLICE_EXPR:
                expr = expr->as.slice_expr.source;
                continue;
            case AST_THIS_EXPR:
                *out_root = (XaMemoryRootRef) {.kind = XA_MEMORY_ROOT_RECEIVER, .index = 0};
                return true;
            case AST_VARIABLE: {
                XrParamNode **params = NULL;
                int count = 0;
                memory_function_params(row->node, &params, &count);
                for (int i = 0; i < count; i++) {
                    XrParamNode *param = params ? params[i] : NULL;
                    if (param &&
                        ((param->symbol_id && param->symbol_id == expr->as.variable.symbol_id) ||
                         (param->name && expr->as.variable.name &&
                          strcmp(param->name, expr->as.variable.name) == 0))) {
                        *out_root =
                            (XaMemoryRootRef) {.kind = XA_MEMORY_ROOT_PARAM, .index = (uint32_t) i};
                        return true;
                    }
                }
                return false;
            }
            default:
                return false;
        }
    }
}

static bool memory_receiver_matches(const XrType *receiver, XaBuiltinReceiverKind kind) {
    switch (kind) {
        case XA_BUILTIN_RECEIVER_EXACT_INTEGER:
            return receiver && receiver->kind == XR_KIND_INT && !receiver->is_nullable;
        case XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER:
            return xr_type_is_exact_unsigned_integer(receiver);
        case XA_BUILTIN_RECEIVER_U8_ARRAY:
            return xr_type_is_u8_array(receiver);
        case XA_BUILTIN_RECEIVER_ARRAY:
            return receiver && receiver->kind == XR_KIND_ARRAY;
        case XA_BUILTIN_RECEIVER_U8_SLICE:
            return xr_type_is_u8_slice(receiver);
        case XA_BUILTIN_RECEIVER_POD_SLICE:
            return receiver && receiver->kind == XR_KIND_SLICE;
    }
    return false;
}

static const XaBuiltinReceiverMethodSpec *memory_builtin_method(const XrType *receiver,
                                                                const char *name) {
    if (!receiver || !name)
        return NULL;
    for (size_t i = 0; i < xa_builtin_receiver_method_count(); i++) {
        const XaBuiltinReceiverMethodSpec *spec = &xa_builtin_receiver_methods[i];
        if (strcmp(spec->source_name, name) == 0 &&
            memory_receiver_matches(receiver, spec->receiver))
            return spec;
    }
    return NULL;
}

static void memory_mark_failure(XaMemoryScan *scan) {
    if (!scan)
        return;
    scan->pass->resource_failure = true;
    xa_memory_effect_summary_mark_incomplete(scan->summary, XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE);
}

static void memory_apply_effect_set(XaMemoryScan *scan, XaMemoryRootRef root,
                                    XaBuiltinMethodMemoryEffectSet effect) {
    bool ok = true;
    if (effect & XA_BUILTIN_MEMORY_WRITE)
        ok = xa_memory_effect_summary_add_write(scan->summary, root,
                                                XA_MEMORY_PLACE_PATH_WILDCARD) &&
             ok;
    if (effect & XA_BUILTIN_MEMORY_MAY_RELOCATE)
        ok = xa_memory_effect_summary_mark_relocation(scan->summary, root) && ok;
    if (effect & XA_BUILTIN_MEMORY_MAY_SHORTEN)
        ok = xa_memory_effect_summary_mark_shortening(scan->summary, root,
                                                      XA_MEMORY_RANGE_EXPR_NONE) &&
             ok;
    if (effect & XA_BUILTIN_MEMORY_INVALIDATES_VIEWS)
        ok = xa_memory_effect_summary_mark_invalidation(scan->summary, root) && ok;
    if (!ok)
        memory_mark_failure(scan);
}

static void memory_apply_builtin(XaMemoryScan *scan, XaMemoryRootRef root,
                                 const XaBuiltinReceiverMethodSpec *spec) {
    memory_apply_effect_set(scan, root, xa_builtin_receiver_method_memory_effect(spec));
}

static XaSymbol *memory_call_symbol(XaMemoryPass *pass, AstNode *callee) {
    if (!pass || !callee)
        return NULL;
    if (callee->type == AST_VARIABLE) {
        XaSymbol *symbol = memory_symbol_by_id(pass->analyzer, callee->as.variable.symbol_id);
        return symbol ? symbol : xa_analyzer_lookup_deep(pass->analyzer, callee->as.variable.name);
    }
    if (callee->type == AST_MEMBER_ACCESS) {
        const XaSelection *selection =
            xa_selection_table_get((XaSelectionTable *) pass->analyzer->selection_table, callee);
        return selection ? selection->target_symbol : NULL;
    }
    return NULL;
}

static const XaMemoryEffectSummary *memory_callee_summary(XaMemoryPass *pass, XaSymbol *callee) {
    XaMemoryFunctionRow *local = memory_row_for_symbol(pass, callee);
    if (local)
        return &local->result;
    return callee ? xa_symbol_links_memory_effect_summary(&callee->links) : NULL;
}

static void memory_instantiate_callee(XaMemoryScan *scan, const XaMemoryEffectSummary *callee,
                                      CallExprNode *call, AstNode *receiver) {
    if (!scan || !callee || !call)
        return;
    if (!xa_memory_effect_summary_is_complete(callee)) {
        XaUnknownReasonSet reasons = callee->unknown_reasons | XA_UNKNOWN_VIEW_INVALIDATION;
        xa_memory_effect_summary_mark_incomplete(scan->summary, (XaUnknownReason) reasons);
        if (xa_memory_effect_summary_has_resource_failure(callee))
            scan->pass->resource_failure = true;
    }
    for (uint32_t i = 0; i < callee->root_count; i++) {
        const XaMemoryRootEffect *effect = &callee->roots[i];
        AstNode *actual = NULL;
        XaMemoryRootRef mapped;
        bool has_mapping = false;
        if (effect->root.kind == XA_MEMORY_ROOT_RECEIVER) {
            actual = receiver;
        } else if (effect->root.kind == XA_MEMORY_ROOT_PARAM &&
                   effect->root.index < (uint32_t) call->arg_count) {
            actual = call->arguments[effect->root.index];
        } else if (effect->root.kind == XA_MEMORY_ROOT_FOREIGN_HANDLE) {
            mapped = effect->root;
            has_mapping = true;
        }
        if (actual)
            has_mapping = memory_root_for_expr(scan->row, actual, &mapped);
        if (has_mapping && !memory_apply_root_effect(scan->summary, mapped, effect))
            memory_mark_failure(scan);
    }
}

static bool memory_call_has_visible_root(XaMemoryFunctionRow *row, CallExprNode *call,
                                         AstNode *receiver) {
    XaMemoryRootRef ignored;
    if (receiver && memory_root_for_expr(row, receiver, &ignored))
        return true;
    for (int i = 0; call && i < call->arg_count; i++) {
        if (memory_root_for_expr(row, call->arguments[i], &ignored))
            return true;
    }
    return false;
}

static bool memory_expr_is_module(XaMemoryPass *pass, AstNode *expr, const char *module_name) {
    if (!pass || !pass->analyzer || !expr || expr->type != AST_VARIABLE || !module_name)
        return false;
    XaSymbol *symbol = memory_symbol_by_id(pass->analyzer, expr->as.variable.symbol_id);
    if (!symbol && expr->as.variable.name)
        symbol = xa_analyzer_lookup_deep(pass->analyzer, expr->as.variable.name);
    XaSymbolLinks *links = symbol ? xa_analyzer_get_links(pass->analyzer, symbol) : NULL;
    return symbol && symbol->kind == XA_SYM_MODULE && links && links->module_name &&
           strcmp(links->module_name, module_name) == 0;
}

static bool memory_call_is_mem_intrinsic(XaMemoryPass *pass, const CallExprNode *call) {
    if (!pass || !call || !call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    const MemberAccessNode *member = &call->callee->as.member_access;
    static const char *const names[] = {
        "sizeOf",       "alignOf",       "offsetOf",
        "slice",        "withSliceMut",  "assumeInitialized",
        "alloc",        "allocZeroed",   "allocAligned",
        "pageAlloc",    "pageProtect",   "pageFree",
        "ptr",          "mutPtr",        "addr",
        "load",         "store",         "copy",
        "move",         "set",           "compare",
        "volatileLoad", "volatileStore",
    };
    if (!member->name)
        return false;
    bool known = false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(member->name, names[i]) == 0) {
            known = true;
            break;
        }
    }
    return known && memory_expr_is_module(pass, member->object, "mem");
}

static void memory_apply_mem_scalar_store(XaMemoryScan *scan, CallExprNode *call) {
    if (!scan || !call || !memory_call_is_mem_intrinsic(scan->pass, call) || !call->callee ||
        call->arg_count < 1 || !call->arguments)
        return;
    const char *name = call->callee->as.member_access.name;
    if (strcmp(name, "store") != 0 && strcmp(name, "copy") != 0 && strcmp(name, "move") != 0 &&
        strcmp(name, "set") != 0 && strcmp(name, "volatileStore") != 0 &&
        strcmp(name, "withSliceMut") != 0)
        return;
    XaMemoryRootRef root;
    if (memory_root_for_expr(scan->row, call->arguments[0], &root) &&
        !xa_memory_effect_summary_add_write(scan->summary, root, XA_MEMORY_PLACE_PATH_WILDCARD))
        memory_mark_failure(scan);
}

static XrCallArgAccess memory_call_arg_access(const CallExprNode *call, int index);

static bool memory_dynamic_arg_is_stable_slice_read(XaMemoryPass *pass, CallExprNode *call,
                                                    XaSymbol *callee_symbol, int arg_index) {
    if (!pass || !call || !call->callee || arg_index < 0 || arg_index >= call->arg_count ||
        !call->arguments)
        return false;
    XrType *function_type = memory_expr_type(pass, call->callee);
    if ((!function_type || !XR_TYPE_IS_FUNCTION(function_type)) && callee_symbol)
        function_type = callee_symbol->links.type ? callee_symbol->links.type
                                                  : callee_symbol->links.declared_type;
    XrType *arg_type = memory_expr_type(pass, call->arguments[arg_index]);
    return function_type && XR_TYPE_IS_FUNCTION(function_type) && arg_type &&
           XR_TYPE_IS_SLICE(arg_type) &&
           xr_type_function_param_mode(function_type, arg_index) == XR_PARAM_READ &&
           memory_call_arg_access(call, arg_index) == XR_CALL_ARG_PLAIN;
}

static void memory_apply_unknown_call(XaMemoryScan *scan, CallExprNode *call, AstNode *receiver,
                                      XaSymbol *callee_symbol) {
    if (!scan || !call)
        return;
    XaMemoryRootRef root;
    if (receiver && memory_root_for_expr(scan->row, receiver, &root) &&
        !xa_memory_effect_summary_mark_invalidation(scan->summary, root))
        memory_mark_failure(scan);
    for (int i = 0; i < call->arg_count; i++) {
        AstNode *arg = call->arguments ? call->arguments[i] : NULL;
        if (!arg || !memory_root_for_expr(scan->row, arg, &root) ||
            memory_dynamic_arg_is_stable_slice_read(scan->pass, call, callee_symbol, i))
            continue;
        if (!xa_memory_effect_summary_mark_invalidation(scan->summary, root))
            memory_mark_failure(scan);
    }
}

static void memory_scan_call(XaMemoryScan *scan, AstNode *node) {
    CallExprNode *call = &node->as.call_expr;
    memory_apply_mem_scalar_store(scan, call);
    AstNode *receiver = NULL;
    const XaBuiltinReceiverMethodSpec *builtin = NULL;
    bool named_contract = false;
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        receiver = call->callee->as.member_access.object;
        XrType *receiver_type = memory_expr_type(scan->pass, receiver);
        builtin = memory_builtin_method(receiver_type, call->callee->as.member_access.name);
        XaMemoryRootRef root;
        if (builtin && memory_root_for_expr(scan->row, receiver, &root))
            memory_apply_builtin(scan, root, builtin);
        if (!builtin) {
            XaBuiltinMethodMemoryEffectSet effects = XA_BUILTIN_MEMORY_STABLE_READ;
            named_contract =
                (receiver_type && XR_TYPE_IS_POINTER(receiver_type) &&
                 xa_builtin_pointer_memory_effect(call->callee->as.member_access.name, &effects)) ||
                xa_builtin_named_receiver_memory_effect(xr_type_get_class_name(receiver_type),
                                                        call->callee->as.member_access.name,
                                                        &effects);
            if (named_contract && memory_root_for_expr(scan->row, receiver, &root))
                memory_apply_effect_set(scan, root, effects);
        }
    }

    XaSymbol *callee_symbol = memory_call_symbol(scan->pass, call->callee);
    const XaMemoryEffectSummary *callee = memory_callee_summary(scan->pass, callee_symbol);
    bool compiler_builtin_function =
        (callee_symbol && callee_symbol->is_builtin && callee_symbol->kind == XA_SYM_FUNCTION) ||
        memory_call_is_mem_intrinsic(scan->pass, call) ||
        (call->callee && call->callee->type == AST_VARIABLE && call->callee->as.variable.name &&
         strcmp(call->callee->as.variable.name, "len") == 0);
    if (callee) {
        memory_instantiate_callee(scan, callee, call, receiver);
    } else if (!builtin && !named_contract && !compiler_builtin_function &&
               memory_call_has_visible_root(scan->row, call, receiver)) {
        /* Unknown dispatch is fail-closed per visible root.  A read Slice argument is the one
         * exception: the function type itself forbids descriptor/owner mutation, and safe Slice
         * values cannot be retained elsewhere.  Keeping this root-relative avoids poisoning an
         * unrelated borrowed owner merely because another receiver is dynamically dispatched. */
        memory_apply_unknown_call(scan, call, receiver, callee_symbol);
    }
}

static void memory_scan_pre(AstNode *node, void *userdata) {
    XaMemoryScan *scan = (XaMemoryScan *) userdata;
    if (!scan || !node)
        return;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
        node->type == AST_METHOD_DECL) {
        scan->nested_function_depth++;
        return;
    }
    if (scan->nested_function_depth > 0)
        return;
    if (node->type == AST_CALL_EXPR) {
        memory_scan_call(scan, node);
    } else if (node->type == AST_ASSIGNMENT) {
        XrParamNode **params = NULL;
        int count = 0;
        memory_function_params(scan->row->node, &params, &count);
        for (int i = 0; i < count; i++) {
            XrParamNode *param = params ? params[i] : NULL;
            if (param && ((param->symbol_id && param->symbol_id == node->as.assignment.symbol_id) ||
                          (param->name && node->as.assignment.name &&
                           strcmp(param->name, node->as.assignment.name) == 0))) {
                XaMemoryRootRef root = {.kind = XA_MEMORY_ROOT_PARAM, .index = (uint32_t) i};
                if (!xa_memory_effect_summary_mark_descriptor_rebind(scan->summary, root))
                    memory_mark_failure(scan);
                break;
            }
        }
    }
}

static void memory_scan_post(AstNode *node, void *userdata) {
    XaMemoryScan *scan = (XaMemoryScan *) userdata;
    if (scan && node &&
        (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR ||
         node->type == AST_METHOD_DECL) &&
        scan->nested_function_depth > 0)
        scan->nested_function_depth--;
}

static bool memory_apply_native_contract(XaMemoryPass *pass, XaMemoryFunctionRow *row) {
    const XrNativePackagePlan *plan = xr_compiler_session_native_package_plan(
        pass && pass->analyzer ? pass->analyzer->compiler_session : NULL);
    const XrNativeSymbol *symbol =
        xr_native_package_find_symbol(plan, row && row->symbol ? row->symbol->name : NULL);
    if (!symbol || !symbol->contract.complete)
        return false;
    /* Arity is validated at declaration collection, where the AST signature
     * is authoritative.  Do not key semantic completeness to the optional
     * parameter-escape sidecar count: bootstrap/imported symbols can publish
     * that sidecar after memory-effect inference without changing ABI. */
    for (uint32_t i = 0; i < symbol->contract.param_count; i++) {
        const XrNativeParamContract *param = &symbol->contract.params[i];
        XaMemoryRootRef root = {.kind = XA_MEMORY_ROOT_PARAM, .index = i};
        bool ok = true;
        if (param->access == XR_NATIVE_ACCESS_WRITE || param->access == XR_NATIVE_ACCESS_READWRITE)
            ok = xa_memory_effect_summary_add_write(&row->direct, root,
                                                    XA_MEMORY_PLACE_PATH_WILDCARD) &&
                 ok;
        if (param->descriptor_rebind)
            ok = xa_memory_effect_summary_mark_descriptor_rebind(&row->direct, root) && ok;
        if (param->may_relocate)
            ok = xa_memory_effect_summary_mark_relocation(&row->direct, root) && ok;
        if (param->may_shorten)
            ok = xa_memory_effect_summary_mark_shortening(&row->direct, root,
                                                          XA_MEMORY_RANGE_EXPR_NONE) &&
                 ok;
        if (param->invalidates_views)
            ok = xa_memory_effect_summary_mark_invalidation(&row->direct, root) && ok;
        if (!ok)
            pass->resource_failure = true;
    }
    return true;
}

static void memory_build_direct(XaMemoryPass *pass, XaMemoryFunctionRow *row) {
    XaSymbolLinks *links = &row->symbol->links;
    for (int i = 0; i < links->param_effect_count; i++) {
        if (!xa_param_effect_mutates(&links->param_effects[i]))
            continue;
        XaMemoryRootRef root = {.kind = XA_MEMORY_ROOT_PARAM, .index = (uint32_t) i};
        if (!xa_memory_effect_summary_add_write(&row->direct, root, XA_MEMORY_PLACE_PATH_WILDCARD))
            pass->resource_failure = true;
    }
    if (row->node->type == AST_METHOD_DECL && row->symbol->mutates_receiver) {
        XaMemoryRootRef root = {.kind = XA_MEMORY_ROOT_RECEIVER, .index = 0};
        if (!xa_memory_effect_summary_add_write(&row->direct, root, XA_MEMORY_PLACE_PATH_WILDCARD))
            pass->resource_failure = true;
    }
    if (!memory_function_body(row->node) && links->is_extern &&
        !memory_apply_native_contract(pass, row))
        xa_memory_effect_summary_mark_incomplete(&row->direct, XA_UNKNOWN_NATIVE_CONTRACT_MISSING);
    if (!memory_summary_copy(&row->result, &row->direct))
        pass->resource_failure = true;
}

static bool memory_combine_row(XaMemoryPass *pass, XaMemoryFunctionRow *row) {
    XaMemoryEffectSummary next;
    if (!memory_summary_copy(&next, &row->direct)) {
        pass->resource_failure = true;
        return false;
    }
    AstNode *body = memory_function_body(row->node);
    if (body) {
        XaMemoryScan scan = {.pass = pass, .row = row, .summary = &next};
        xa_ast_walk(body, memory_scan_pre, memory_scan_post, &scan);
    }
    bool changed = !memory_summary_equal(&next, &row->result);
    if (changed) {
        xa_memory_effect_summary_clear(&row->result);
        row->result = next;
    } else {
        xa_memory_effect_summary_clear(&next);
    }
    return changed;
}

static void memory_report_resource_failure(XaMemoryPass *pass, XaMemoryFunctionRow *row) {
    char message[256];
    snprintf(message, sizeof(message),
             "analysis resource failure while publishing memory effect for '%s'",
             row && row->symbol && row->symbol->name ? row->symbol->name : "?");
    XrLocation location = {.file = row && row->symbol ? row->symbol->links.file_path : NULL,
                           .line = row && row->node ? (uint32_t) row->node->line : 0,
                           .column = row && row->node ? (uint32_t) row->node->column : 0};
    xa_analyzer_add_diagnostic(pass->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE, message,
                               &location);
}

typedef struct XaMemoryContractValidation {
    XaMemoryPass *pass;
} XaMemoryContractValidation;

static AstNode *memory_unwrap_ref_range(AstNode *node) {
    while (node && node->type == AST_GROUPING)
        node = node->as.grouping;
    return node && node->type == AST_SLICE_EXPR ? node : NULL;
}

static const XaMemoryRootEffect *memory_summary_root_effect(const XaMemoryEffectSummary *summary,
                                                            XaMemoryRootKind kind, uint32_t index) {
    if (!summary)
        return NULL;
    for (uint32_t i = 0; i < summary->root_count; i++) {
        const XaMemoryRootEffect *effect = &summary->roots[i];
        if (effect->root.kind == kind && effect->root.index == index)
            return effect;
    }
    return NULL;
}

static XrCallArgAccess memory_call_arg_access(const CallExprNode *call, int index) {
    if (!call || index < 0 || index >= call->arg_count || !call->arg_accesses)
        return XR_CALL_ARG_PLAIN;
    XrCallArgAccess access = call->arg_accesses[index];
    return xr_call_arg_access_is_valid(access) ? access : XR_CALL_ARG_PLAIN;
}

static const char *memory_callee_name(const CallExprNode *call, const XaSymbol *symbol) {
    if (symbol && symbol->name)
        return symbol->name;
    if (!call || !call->callee)
        return "callee";
    if (call->callee->type == AST_VARIABLE && call->callee->as.variable.name)
        return call->callee->as.variable.name;
    if (call->callee->type == AST_MEMBER_ACCESS && call->callee->as.member_access.name)
        return call->callee->as.member_access.name;
    return "callee";
}

static void memory_validate_ref_range_call(AstNode *node, void *userdata) {
    XaMemoryContractValidation *validation = (XaMemoryContractValidation *) userdata;
    if (!validation || !validation->pass || !node || node->type != AST_CALL_EXPR)
        return;
    XaMemoryPass *pass = validation->pass;
    CallExprNode *call = &node->as.call_expr;
    XaSymbol *callee_symbol = memory_call_symbol(pass, call->callee);
    const XaMemoryEffectSummary *summary = memory_callee_summary(pass, callee_symbol);
    XrType *function_type = callee_symbol ? callee_symbol->links.type : NULL;

    int slot = 0;
    for (int i = 0; i < call->arg_count; i++) {
        AstNode *arg = call->arguments ? call->arguments[i] : NULL;
        if (!arg)
            continue;
        if (arg->type == AST_SPREAD_EXPR) {
            XrType *source_type =
                xa_analyzer_get_node_type(pass->analyzer, arg->as.spread_expr.expr);
            slot +=
                source_type && XR_TYPE_IS_TUPLE(source_type) ? source_type->tuple.element_count : 0;
            continue;
        }
        if (memory_call_arg_access(call, i) != XR_CALL_ARG_REF || !memory_unwrap_ref_range(arg)) {
            slot++;
            continue;
        }

        XrParamMode mode = function_type && XR_TYPE_IS_FUNCTION(function_type)
                               ? xr_type_function_param_mode(function_type, slot)
                               : XR_PARAM_READ;
        if (mode != XR_PARAM_REF) {
            slot++;
            continue;
        }
        const XaMemoryRootEffect *effect =
            memory_summary_root_effect(summary, XA_MEMORY_ROOT_PARAM, (uint32_t) slot);
        bool complete = summary && xa_memory_effect_summary_is_complete(summary);
        if (complete && (!effect || !effect->descriptor_rebind)) {
            slot++;
            continue;
        }

        XrLocation location = {.file = pass->analyzer->current_file,
                               .line = arg->line ? (uint32_t) arg->line : (uint32_t) node->line,
                               .column =
                                   arg->column ? (uint32_t) arg->column : (uint32_t) node->column};
        char message[320];
        if (effect && effect->descriptor_rebind) {
            snprintf(message, sizeof(message),
                     "cannot pass range projection as `ref` parameter %d of '%s': the callee "
                     "may rebind the Slice descriptor; bind the range to `var view` first",
                     slot + 1, memory_callee_name(call, callee_symbol));
        } else {
            snprintf(message, sizeof(message),
                     "cannot pass range projection as `ref` parameter %d of '%s': descriptor "
                     "effects are incomplete (ViewInvalidationUnknown)",
                     slot + 1, memory_callee_name(call, callee_symbol));
        }
        xa_analyzer_add_diagnostic(pass->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                   message, &location);
        slot++;
    }
}

void xa_infer_memory_effects(XaAnalyzer *analyzer, AstNode *ast) {
    if (!analyzer || !analyzer->memory_effect_db || !ast)
        return;
    XaMemoryPass pass = {.analyzer = analyzer};
    xa_ast_walk(ast, memory_collect_pre, NULL, &pass);
    for (int i = 0; i < pass.row_count; i++)
        memory_build_direct(&pass, &pass.rows[i]);

    for (int iteration = 0; iteration <= pass.row_count; iteration++) {
        bool changed = false;
        for (int i = 0; i < pass.row_count; i++)
            changed = memory_combine_row(&pass, &pass.rows[i]) || changed;
        if (!changed)
            break;
        if (iteration == pass.row_count)
            pass.resource_failure = true;
    }

    for (int i = 0; i < pass.row_count; i++) {
        XaMemoryFunctionRow *row = &pass.rows[i];
        if (pass.resource_failure)
            xa_memory_effect_summary_mark_incomplete(&row->result,
                                                     XA_UNKNOWN_ANALYSIS_RESOURCE_FAILURE);
        XaMemoryEffectId id = xa_memory_effect_db_intern(analyzer->memory_effect_db, &row->result);
        if (id == XA_MEMORY_EFFECT_NONE) {
            memory_report_resource_failure(&pass, row);
        } else {
            row->symbol->links.memory_effect_id = id;
            for (int slot = 0; slot < row->symbol->links.param_effect_count; slot++)
                row->symbol->links.param_effects[slot].memory_effects = id;
        }
    }
    XaMemoryContractValidation validation = {.pass = &pass};
    xa_ast_walk(ast, memory_validate_ref_range_call, NULL, &validation);
    for (int i = 0; i < pass.row_count; i++) {
        xa_memory_effect_summary_clear(&pass.rows[i].direct);
        xa_memory_effect_summary_clear(&pass.rows[i].result);
    }
    xr_free(pass.rows);
}
