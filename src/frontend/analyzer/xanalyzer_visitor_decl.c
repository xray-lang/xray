/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor_decl.c - Pass 1 collect helpers for declarations,
 *                            Pass 1.5 class-inheritance linking, and
 *                            return-type inference scanner
 *
 * KEY CONCEPT:
 *   Holds the bulk of "declaration-shaped" analyzer code that used to
 *   crowd xanalyzer_visitor.c past the 2500-line mark:
 *
 *     - collect_return_types / xa_infer_function_return_type
 *         (post-hoc return-type inference for unannotated functions)
 *
 *     - xa_visit_collect_function_decl_only / _function_body /
 *       xa_visit_collect_function (two-phase function symbol collect
 *       supporting mutual recursion via hoisting)
 *
 *     - contains_this_expr / stmt_contains_this
 *         (constructor super() validation: no `this` access before
 *          super() returns)
 *
 *     - xa_visit_collect_class
 *         (class symbol creation, field / method / generic param
 *          registration, struct layout)
 *
 *     - xa_visit_collect_var_decl
 *         (top-level let/const symbol)
 *
 *     - build_class_vtable / xa_link_class_inheritance
 *         (Pass 1.5 entry point: resolve base class names to
 *          XrClassInfo pointers and build vtables)
 *
 *   This file holds the declaration-shaped subset of the analyzer
 *   visitor. The two collect helpers reachable from the hoisting loop
 *   in xanalyzer_visitor.c are non-static so they can be called
 *   cross-TU; see xanalyzer_visitor_internal.h.
 */

#include "xanalyzer_visitor_internal.h"
#include "xanalyzer_builtin_interfaces.h"
#include "xtype_ref_resolve.h"
#include "../parser/xtype_ref.h"
#include "../../base/xchecks.h"
#include "../../runtime/value/xstruct_layout.h"

static bool xa_class_attr_has(const ClassDeclNode *cls, AttributeKind kind) {
    if (!cls || !cls->attributes)
        return false;
    for (int i = 0; i < cls->attr_count; i++) {
        if (cls->attributes[i] && cls->attributes[i]->kind == kind)
            return true;
    }
    return false;
}

static uint8_t xa_class_attr_align(const ClassDeclNode *cls) {
    if (!cls || !cls->attributes)
        return 0;
    for (int i = 0; i < cls->attr_count; i++) {
        XrAttribute *attr = cls->attributes[i];
        if (attr && attr->kind == ATTR_ALIGN && attr->timeout > 0 && attr->timeout <= UINT8_MAX) {
            return (uint8_t) attr->timeout;
        }
    }
    return 0;
}

static XrAttribute *xa_function_attr(const FunctionDeclNode *fn, AttributeKind kind) {
    if (!fn || !fn->attributes)
        return NULL;
    for (int i = 0; i < fn->attr_count; i++) {
        if (fn->attributes[i] && fn->attributes[i]->kind == kind)
            return fn->attributes[i];
    }
    return NULL;
}

static bool xa_c_symbol_is_identifier(const char *name) {
    if (!name || !name[0])
        return false;
    char c = name[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'))
        return false;
    for (const char *p = name + 1; *p; p++) {
        c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_'))
            return false;
    }
    return true;
}

static bool xa_c_export_type_supported(XrType *type, bool is_return) {
    if (!type)
        return false;
    switch (type->kind) {
        case XR_KIND_UNIT:
            return is_return;
        case XR_KIND_BOOL:
        case XR_KIND_FLOAT:
        case XR_KIND_INT:
        case XR_KIND_POINTER:
            return true;
        default:
            return false;
    }
}

static void xa_validate_extern_function_abi(XaInferContext *ctx, AstNode *node,
                                            const FunctionDeclNode *fn, XrType **param_types,
                                            XrType *return_type) {
    if (!ctx || !ctx->analyzer || !fn)
        return;
    for (int i = 0; i < fn->param_count; i++) {
        XrType *type = param_types ? param_types[i] : NULL;
        if (!type || !XR_TYPE_IS_FUNCTION(type) || XR_TYPE_IS_C_FUNCTION(type))
            continue;
        XrParamNode *param = fn->params ? fn->params[i] : NULL;
        XrLocation loc = {.file = ctx->file_path,
                          .line = param ? param->line : (node ? node->line : 0),
                          .column = param ? param->column : (node ? node->column : 0)};
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "extern function parameter '%s' uses Xray function type; use CFn<...> for C "
                 "function pointers",
                 param && param->name ? param->name : "?");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
    }
    if (return_type && XR_TYPE_IS_FUNCTION(return_type) && !XR_TYPE_IS_C_FUNCTION(return_type)) {
        XrLocation loc = {.file = ctx->file_path,
                          .line = node ? node->line : 0,
                          .column = node ? node->column : 0};
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
            "extern function returns Xray function type; use CFn<...> for C function pointers",
            &loc);
    }
}

static void xa_validate_c_export_unique_symbol(XaInferContext *ctx, AstNode *node, XaSymbol *sym,
                                               XrAttribute *attr) {
    if (!ctx || !ctx->analyzer || !attr || !xa_c_symbol_is_identifier(attr->str_arg))
        return;

    int count = 0;
    XaSymbol **symbols = xa_scope_get_all_symbols(ctx->analyzer->global_scope, &count);
    for (int i = 0; i < count; i++) {
        XaSymbol *other = symbols[i];
        if (!other || other == sym || other->kind != XA_SYM_FUNCTION)
            continue;
        XaSymbolLinks *other_links = xa_analyzer_get_links(ctx->analyzer, other);
        if (!other_links || !other_links->is_c_export || !other_links->c_export_symbol)
            continue;
        if (strcmp(other_links->c_export_symbol, attr->str_arg) != 0)
            continue;

        XrLocation loc = {.file = ctx->file_path,
                          .line = node ? node->line : 0,
                          .column = node ? node->column : 0};
        char msg[256];
        snprintf(msg, sizeof(msg), "duplicate @c_export symbol '%s' already used by function '%s'",
                 attr->str_arg, other->name ? other->name : "?");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
        break;
    }
    if (symbols)
        xr_free(symbols);
}

static void xa_validate_c_export_function_abi(XaInferContext *ctx, AstNode *node,
                                              const FunctionDeclNode *fn, XaSymbol *sym,
                                              XrType **param_types, XrType *return_type,
                                              XrAttribute *attr, bool is_extern) {
    if (!ctx || !ctx->analyzer || !fn || !attr)
        return;

    XrLocation fn_loc = {
        .file = ctx->file_path, .line = node ? node->line : 0, .column = node ? node->column : 0};

    if (!xa_c_symbol_is_identifier(attr->str_arg)) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                   "@c_export requires a non-empty C identifier symbol name",
                                   &fn_loc);
    }

    xa_validate_c_export_unique_symbol(ctx, node, sym, attr);

    if (is_extern) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                   "@c_export cannot be used on an @extern function", &fn_loc);
    }

    if (!fn->body) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE,
                                   "@c_export requires a function body", &fn_loc);
    }

    for (int i = 0; i < fn->param_count; i++) {
        XrType *type = param_types ? param_types[i] : NULL;
        if (xa_c_export_type_supported(type, false))
            continue;
        XrParamNode *param = fn->params ? fn->params[i] : NULL;
        XrLocation loc = {.file = ctx->file_path,
                          .line = param ? param->line : fn_loc.line,
                          .column = param ? param->column : fn_loc.column};
        char msg[256];
        snprintf(
            msg, sizeof(msg), "C export function parameter '%s' uses unsupported C ABI type '%s'",
            param && param->name ? param->name : "?", type ? xr_type_to_string(type) : "<unknown>");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &loc);
    }

    if (!xa_c_export_type_supported(return_type, true)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "C export function returns unsupported C ABI type '%s'",
                 return_type ? xr_type_to_string(return_type) : "<unknown>");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_ARG_TYPE, msg,
                                   &fn_loc);
    }
}

static bool xa_expr_is_this(AstNode *node) {
    if (!node)
        return false;
    if (node->type == AST_THIS_EXPR)
        return true;
    return node->type == AST_VARIABLE && node->as.variable.name &&
           strcmp(node->as.variable.name, "this") == 0;
}

typedef struct XaParamEscapeSummary {
    XrType **param_types;
    const char **param_names;
    int param_count;
    uint8_t *escapes;
    XrType *return_type;
    const char *aliases[128];
    int alias_slot[128];
    int alias_count;
    XaInferContext *ctx;
    XrClassInfo *receiver_info;
} XaParamEscapeSummary;

static int xa_summary_param_slot(XaParamEscapeSummary *summary, const char *name) {
    if (!summary || !name)
        return -1;
    for (int i = summary->alias_count - 1; i >= 0; i--) {
        if (summary->aliases[i] && strcmp(summary->aliases[i], name) == 0)
            return summary->alias_slot[i];
    }
    for (int i = 0; i < summary->param_count; i++) {
        if (summary->param_names && summary->param_names[i] &&
            strcmp(summary->param_names[i], name) == 0)
            return i;
    }
    return -1;
}

static void xa_summary_set_alias(XaParamEscapeSummary *summary, const char *name, int slot) {
    if (!summary || !name)
        return;
    for (int i = summary->alias_count - 1; i >= 0; i--) {
        if (summary->aliases[i] && strcmp(summary->aliases[i], name) == 0) {
            summary->alias_slot[i] = slot;
            return;
        }
    }
    if (summary->alias_count >= 128)
        return;
    summary->aliases[summary->alias_count] = name;
    summary->alias_slot[summary->alias_count] = slot;
    summary->alias_count++;
}

static int xa_summary_expr_root_param_slot(XaParamEscapeSummary *summary, AstNode *expr) {
    while (expr) {
        switch (expr->type) {
            case AST_VARIABLE:
                return xa_summary_param_slot(summary, expr->as.variable.name);
            case AST_MEMBER_ACCESS:
                expr = expr->as.member_access.object;
                break;
            case AST_INDEX_GET:
                expr = expr->as.index_get.array;
                break;
            case AST_SLICE_EXPR:
                expr = expr->as.slice_expr.source;
                break;
            case AST_OPTIONAL_CHAIN:
                expr = expr->as.optional_chain.object;
                break;
            case AST_GROUPING:
                expr = expr->as.grouping;
                break;
            case AST_FORCE_UNWRAP:
                expr = expr->as.unary.operand;
                break;
            case AST_MOVE_EXPR:
                expr = expr->as.move_expr.expr;
                break;
            case AST_CALL_EXPR: {
                CallExprNode *call = &expr->as.call_expr;
                if (call->callee && call->callee->type == AST_VARIABLE &&
                    call->callee->as.variable.name &&
                    strcmp(call->callee->as.variable.name, "copy") == 0)
                    return -1;
                return -1;
            }
            default:
                return -1;
        }
    }
    return -1;
}

static bool xa_summary_method_stores_argument(const char *method_name, int slot);
static XaSymbolLinks *xa_summary_function_links(XaParamEscapeSummary *summary, AstNode *callee);
static XrClassInfo *xa_type_class_info(XrType *type);
static XrClassInfo *xa_summary_type_class_info(XaParamEscapeSummary *summary, XrType *type);
static XaSymbol *xa_receiver_method_symbol_for_call(XrClassInfo *receiver_info, AstNode *object,
                                                    const char *method_name);
static void xa_summary_mark_expr(XaParamEscapeSummary *summary, AstNode *expr);
static void xa_summary_walk(XaParamEscapeSummary *summary, AstNode *node);

static XrClassInfo *xa_summary_type_class_info(XaParamEscapeSummary *summary, XrType *type) {
    XrClassInfo *info = xa_type_class_info(type);
    if (info || !summary || !summary->ctx || !summary->ctx->analyzer || !type)
        return info;
    const char *class_name = xr_type_get_class_name(type);
    if (!class_name)
        return NULL;
    XaSymbol *sym = xa_scope_lookup(summary->ctx->analyzer->global_scope, class_name);
    XaSymbolLinks *links = sym ? xa_analyzer_get_links(summary->ctx->analyzer, sym) : NULL;
    return links ? links->class_info : NULL;
}

static XrClassInfo *xa_summary_expr_class_info(XaParamEscapeSummary *summary, AstNode *node) {
    if (!summary || !node)
        return NULL;
    if (xa_expr_is_this(node))
        return summary->receiver_info;
    switch (node->type) {
        case AST_VARIABLE: {
            int slot = xa_summary_param_slot(summary, node->as.variable.name);
            if (slot < 0 || slot >= summary->param_count || !summary->param_types)
                return NULL;
            return xa_summary_type_class_info(summary, summary->param_types[slot]);
        }
        case AST_MEMBER_ACCESS: {
            XrClassInfo *owner_info =
                xa_summary_expr_class_info(summary, node->as.member_access.object);
            if (!owner_info)
                return NULL;
            XaSymbol *member = xa_class_info_lookup_member(owner_info, node->as.member_access.name);
            if (!member || (member->kind != XA_SYM_FIELD && member->kind != XA_SYM_PROPERTY))
                return NULL;
            return xa_summary_type_class_info(summary, member->links.type);
        }
        case AST_GROUPING:
            return xa_summary_expr_class_info(summary, node->as.grouping);
        case AST_FORCE_UNWRAP:
            return xa_summary_expr_class_info(summary, node->as.unary.operand);
        case AST_MOVE_EXPR:
            return xa_summary_expr_class_info(summary, node->as.move_expr.expr);
        default:
            return NULL;
    }
}

static XaSymbolLinks *xa_summary_receiver_method_links(XaParamEscapeSummary *summary,
                                                       AstNode *callee) {
    if (!summary || !summary->ctx || !callee || callee->type != AST_MEMBER_ACCESS)
        return NULL;
    MemberAccessNode *ma = &callee->as.member_access;
    XrClassInfo *target_info = xa_summary_expr_class_info(summary, ma->object);
    XaSymbol *method_sym = target_info ? xa_class_info_lookup_member(target_info, ma->name) : NULL;
    if (!method_sym || method_sym->kind != XA_SYM_METHOD)
        return NULL;
    return method_sym ? xa_analyzer_get_links(summary->ctx->analyzer, method_sym) : NULL;
}

static void xa_summary_mark_call_expr(XaParamEscapeSummary *summary, AstNode *expr) {
    if (!summary || !expr || expr->type != AST_CALL_EXPR)
        return;
    CallExprNode *call = &expr->as.call_expr;
    if (call->callee && call->callee->type == AST_VARIABLE && call->callee->as.variable.name &&
        strcmp(call->callee->as.variable.name, "copy") == 0)
        return;
    XaSymbolLinks *fn_links = xa_summary_function_links(summary, call->callee);
    if (fn_links && fn_links->param_escapes) {
        for (int i = 0; i < call->arg_count && i < fn_links->param_escape_count; i++) {
            if (fn_links->param_escapes[i])
                xa_summary_mark_expr(summary, call->arguments[i]);
        }
    }
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        const char *method_name = call->callee->as.member_access.name;
        for (int i = 0; i < call->arg_count; i++) {
            if (xa_summary_method_stores_argument(method_name, i))
                xa_summary_mark_expr(summary, call->arguments[i]);
        }
        XaSymbolLinks *method_links = xa_summary_receiver_method_links(summary, call->callee);
        if (method_links && method_links->param_escapes) {
            for (int i = 0; i < call->arg_count && i < method_links->param_escape_count; i++) {
                if (method_links->param_escapes[i])
                    xa_summary_mark_expr(summary, call->arguments[i]);
            }
        }
    }
}

static void xa_summary_mark_expr(XaParamEscapeSummary *summary, AstNode *expr) {
    if (!summary || !expr)
        return;
    int slot = xa_summary_expr_root_param_slot(summary, expr);
    if (slot >= 0 && slot < summary->param_count)
        summary->escapes[slot] = 1;
    switch (expr->type) {
        case AST_CALL_EXPR:
            xa_summary_mark_call_expr(summary, expr);
            break;
        case AST_GROUPING:
            xa_summary_mark_expr(summary, expr->as.grouping);
            break;
        case AST_FORCE_UNWRAP:
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            xa_summary_mark_expr(summary, expr->as.unary.operand);
            break;
        case AST_MOVE_EXPR:
            xa_summary_mark_expr(summary, expr->as.move_expr.expr);
            break;
        case AST_SLICE_EXPR:
            xa_summary_mark_expr(summary, expr->as.slice_expr.source);
            break;
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_NULLISH_COALESCE:
            xa_summary_mark_expr(summary, expr->as.binary.left);
            xa_summary_mark_expr(summary, expr->as.binary.right);
            break;
        case AST_TERNARY:
            xa_summary_mark_expr(summary, expr->as.ternary.true_expr);
            xa_summary_mark_expr(summary, expr->as.ternary.false_expr);
            break;
        case AST_ARRAY_LITERAL:
            for (int i = 0; i < expr->as.array_literal.count; i++)
                xa_summary_mark_expr(summary, expr->as.array_literal.elements[i]);
            break;
        case AST_OBJECT_LITERAL:
            for (int i = 0; i < expr->as.object_literal.count; i++)
                xa_summary_mark_expr(summary, expr->as.object_literal.values[i]);
            break;
        case AST_MAP_LITERAL:
            for (int i = 0; i < expr->as.map_literal.count; i++) {
                xa_summary_mark_expr(summary, expr->as.map_literal.keys[i]);
                xa_summary_mark_expr(summary, expr->as.map_literal.values[i]);
            }
            break;
        default:
            break;
    }
}

static bool xa_summary_method_stores_argument(const char *method_name, int slot) {
    if (!method_name || slot < 0)
        return false;
    if ((strcmp(method_name, "push") == 0 || strcmp(method_name, "unshift") == 0 ||
         strcmp(method_name, "fill") == 0 || strcmp(method_name, "add") == 0 ||
         strcmp(method_name, "send") == 0 || strcmp(method_name, "trySend") == 0 ||
         strcmp(method_name, "sendTimeout") == 0) &&
        slot == 0)
        return true;
    return strcmp(method_name, "set") == 0 && (slot == 0 || slot == 1);
}

static XaSymbolLinks *xa_summary_function_links(XaParamEscapeSummary *summary, AstNode *callee) {
    if (!summary || !summary->ctx || !callee || callee->type != AST_VARIABLE ||
        !callee->as.variable.name)
        return NULL;
    XaSymbol *sym =
        xa_scope_lookup(summary->ctx->analyzer->current_scope, callee->as.variable.name);
    if (!sym && summary->ctx->analyzer->global_scope)
        sym = xa_scope_lookup(summary->ctx->analyzer->global_scope, callee->as.variable.name);
    if (!sym || sym->kind != XA_SYM_FUNCTION)
        return NULL;
    return xa_analyzer_get_links(summary->ctx->analyzer, sym);
}

static void xa_summary_mark_capture_refs(XaParamEscapeSummary *summary, AstNode *node) {
    if (!summary || !node)
        return;
    int slot = xa_summary_expr_root_param_slot(summary, node);
    if (slot >= 0 && slot < summary->param_count)
        summary->escapes[slot] = 1;

    switch (node->type) {
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++)
                xa_summary_mark_capture_refs(summary, node->as.block.statements[i]);
            break;
        case AST_EXPR_STMT:
            xa_summary_mark_capture_refs(summary, node->as.expr_stmt);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            xa_summary_mark_capture_refs(summary, node->as.var_decl.initializer);
            xa_summary_set_alias(summary, node->as.var_decl.name, -1);
            break;
        case AST_ASSIGNMENT:
            xa_summary_mark_capture_refs(summary, node->as.assignment.value);
            break;
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                xa_summary_mark_capture_refs(summary, node->as.return_stmt.values[i]);
            break;
        case AST_CALL_EXPR:
            xa_summary_mark_capture_refs(summary, node->as.call_expr.callee);
            for (int i = 0; i < node->as.call_expr.arg_count; i++)
                xa_summary_mark_capture_refs(summary, node->as.call_expr.arguments[i]);
            break;
        case AST_MEMBER_ACCESS:
            xa_summary_mark_capture_refs(summary, node->as.member_access.object);
            break;
        case AST_MEMBER_SET:
            xa_summary_mark_capture_refs(summary, node->as.member_set.object);
            xa_summary_mark_capture_refs(summary, node->as.member_set.value);
            break;
        case AST_INDEX_GET:
            xa_summary_mark_capture_refs(summary, node->as.index_get.array);
            xa_summary_mark_capture_refs(summary, node->as.index_get.index);
            break;
        case AST_INDEX_SET:
            xa_summary_mark_capture_refs(summary, node->as.index_set.array);
            xa_summary_mark_capture_refs(summary, node->as.index_set.index);
            xa_summary_mark_capture_refs(summary, node->as.index_set.value);
            break;
        case AST_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.count; i++)
                xa_summary_mark_capture_refs(summary, node->as.array_literal.elements[i]);
            break;
        case AST_OBJECT_LITERAL:
            for (int i = 0; i < node->as.object_literal.count; i++)
                xa_summary_mark_capture_refs(summary, node->as.object_literal.values[i]);
            break;
        case AST_MAP_LITERAL:
            for (int i = 0; i < node->as.map_literal.count; i++) {
                xa_summary_mark_capture_refs(summary, node->as.map_literal.keys[i]);
                xa_summary_mark_capture_refs(summary, node->as.map_literal.values[i]);
            }
            break;
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++)
                xa_summary_mark_capture_refs(summary, node->as.print_stmt.exprs[i]);
            break;
        case AST_IF_STMT:
            xa_summary_mark_capture_refs(summary, node->as.if_stmt.condition);
            xa_summary_mark_capture_refs(summary, node->as.if_stmt.then_branch);
            xa_summary_mark_capture_refs(summary, node->as.if_stmt.else_branch);
            break;
        case AST_WHILE_STMT:
            xa_summary_mark_capture_refs(summary, node->as.while_stmt.condition);
            xa_summary_mark_capture_refs(summary, node->as.while_stmt.body);
            break;
        case AST_FOR_STMT:
            xa_summary_mark_capture_refs(summary, node->as.for_stmt.initializer);
            xa_summary_mark_capture_refs(summary, node->as.for_stmt.condition);
            xa_summary_mark_capture_refs(summary, node->as.for_stmt.increment);
            xa_summary_mark_capture_refs(summary, node->as.for_stmt.body);
            break;
        case AST_FUNCTION_EXPR:
            break;
        case AST_GROUPING:
            xa_summary_mark_capture_refs(summary, node->as.grouping);
            break;
        case AST_FORCE_UNWRAP:
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            xa_summary_mark_capture_refs(summary, node->as.unary.operand);
            break;
        case AST_MOVE_EXPR:
            xa_summary_mark_capture_refs(summary, node->as.move_expr.expr);
            break;
        case AST_SLICE_EXPR:
            xa_summary_mark_capture_refs(summary, node->as.slice_expr.source);
            xa_summary_mark_capture_refs(summary, node->as.slice_expr.start);
            xa_summary_mark_capture_refs(summary, node->as.slice_expr.end);
            break;
        case AST_TERNARY:
            xa_summary_mark_capture_refs(summary, node->as.ternary.condition);
            xa_summary_mark_capture_refs(summary, node->as.ternary.true_expr);
            xa_summary_mark_capture_refs(summary, node->as.ternary.false_expr);
            break;
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_NULLISH_COALESCE:
            xa_summary_mark_capture_refs(summary, node->as.binary.left);
            xa_summary_mark_capture_refs(summary, node->as.binary.right);
            break;
        default:
            break;
    }
}

static void xa_summary_walk_call(XaParamEscapeSummary *summary, AstNode *node) {
    CallExprNode *call = &node->as.call_expr;
    if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
        const char *method_name = call->callee->as.member_access.name;
        for (int i = 0; i < call->arg_count; i++) {
            AstNode *arg = call->arguments[i];
            if (xa_summary_method_stores_argument(method_name, i))
                xa_summary_mark_expr(summary, arg);
            xa_summary_walk(summary, arg);
        }
        XaSymbolLinks *method_links = xa_summary_receiver_method_links(summary, call->callee);
        if (method_links && method_links->param_escapes) {
            for (int i = 0; i < call->arg_count && i < method_links->param_escape_count; i++) {
                if (method_links->param_escapes[i])
                    xa_summary_mark_expr(summary, call->arguments[i]);
            }
        }
        xa_summary_walk(summary, call->callee);
        return;
    }

    XaSymbolLinks *fn_links = xa_summary_function_links(summary, call->callee);
    if (fn_links && fn_links->param_escapes) {
        for (int i = 0; i < call->arg_count && i < fn_links->param_escape_count; i++) {
            if (fn_links->param_escapes[i])
                xa_summary_mark_expr(summary, call->arguments[i]);
        }
    }
    xa_summary_walk(summary, call->callee);
    for (int i = 0; i < call->arg_count; i++)
        xa_summary_walk(summary, call->arguments[i]);
}

static void xa_summary_walk_function_expr(XaParamEscapeSummary *summary, AstNode *node) {
    FunctionDeclNode *fn = &node->as.function_expr;
    if (!fn->body)
        return;
    int saved_alias_count = summary->alias_count;
    for (int i = 0; i < fn->param_count; i++) {
        XrParamNode *param = fn->params ? fn->params[i] : NULL;
        xa_summary_set_alias(summary, param ? param->name : NULL, -1);
    }
    xa_summary_mark_capture_refs(summary, fn->body);
    xa_summary_walk(summary, fn->body);
    summary->alias_count = saved_alias_count;
}

static void xa_summary_walk(XaParamEscapeSummary *summary, AstNode *node) {
    if (!summary || !node)
        return;
    switch (node->type) {
        case AST_BLOCK: {
            int saved_alias_count = summary->alias_count;
            for (int i = 0; i < node->as.block.count; i++)
                xa_summary_walk(summary, node->as.block.statements[i]);
            summary->alias_count = saved_alias_count;
            break;
        }
        case AST_EXPR_STMT:
            xa_summary_walk(summary, node->as.expr_stmt);
            break;
        case AST_VAR_DECL:
        case AST_CONST_DECL: {
            VarDeclNode *var = &node->as.var_decl;
            int slot = xa_summary_expr_root_param_slot(summary, var->initializer);
            xa_summary_set_alias(summary, var->name, slot);
            xa_summary_walk(summary, var->initializer);
            break;
        }
        case AST_ASSIGNMENT: {
            AssignmentNode *assign = &node->as.assignment;
            int slot = xa_summary_expr_root_param_slot(summary, assign->value);
            xa_summary_set_alias(summary, assign->name, slot);
            xa_summary_walk(summary, assign->value);
            break;
        }
        case AST_RETURN_STMT: {
            ReturnStmtNode *ret = &node->as.return_stmt;
            for (int i = 0; i < ret->value_count; i++) {
                if (xa_type_needs_borrow_escape_guard(summary->return_type))
                    xa_summary_mark_expr(summary, ret->values[i]);
                xa_summary_walk(summary, ret->values[i]);
            }
            break;
        }
        case AST_MEMBER_SET:
            xa_summary_walk(summary, node->as.member_set.object);
            xa_summary_mark_expr(summary, node->as.member_set.value);
            xa_summary_walk(summary, node->as.member_set.value);
            break;
        case AST_INDEX_SET:
            xa_summary_walk(summary, node->as.index_set.array);
            xa_summary_walk(summary, node->as.index_set.index);
            xa_summary_mark_expr(summary, node->as.index_set.value);
            xa_summary_walk(summary, node->as.index_set.value);
            break;
        case AST_CALL_EXPR:
            xa_summary_walk_call(summary, node);
            break;
        case AST_FUNCTION_EXPR:
            xa_summary_walk_function_expr(summary, node);
            break;
        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++)
                xa_summary_walk(summary, node->as.print_stmt.exprs[i]);
            break;
        case AST_IF_STMT:
            xa_summary_walk(summary, node->as.if_stmt.condition);
            xa_summary_walk(summary, node->as.if_stmt.then_branch);
            xa_summary_walk(summary, node->as.if_stmt.else_branch);
            break;
        case AST_WHILE_STMT:
            xa_summary_walk(summary, node->as.while_stmt.condition);
            xa_summary_walk(summary, node->as.while_stmt.body);
            break;
        case AST_FOR_STMT:
            xa_summary_walk(summary, node->as.for_stmt.initializer);
            xa_summary_walk(summary, node->as.for_stmt.condition);
            xa_summary_walk(summary, node->as.for_stmt.increment);
            xa_summary_walk(summary, node->as.for_stmt.body);
            break;
        case AST_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.count; i++)
                xa_summary_walk(summary, node->as.array_literal.elements[i]);
            break;
        case AST_OBJECT_LITERAL:
            for (int i = 0; i < node->as.object_literal.count; i++)
                xa_summary_walk(summary, node->as.object_literal.values[i]);
            break;
        case AST_MAP_LITERAL:
            for (int i = 0; i < node->as.map_literal.count; i++) {
                xa_summary_walk(summary, node->as.map_literal.keys[i]);
                xa_summary_walk(summary, node->as.map_literal.values[i]);
            }
            break;
        case AST_GROUPING:
            xa_summary_walk(summary, node->as.grouping);
            break;
        case AST_FORCE_UNWRAP:
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            xa_summary_walk(summary, node->as.unary.operand);
            break;
        case AST_MOVE_EXPR:
            xa_summary_walk(summary, node->as.move_expr.expr);
            break;
        case AST_SLICE_EXPR:
            xa_summary_walk(summary, node->as.slice_expr.source);
            xa_summary_walk(summary, node->as.slice_expr.start);
            xa_summary_walk(summary, node->as.slice_expr.end);
            break;
        case AST_MEMBER_ACCESS:
            xa_summary_walk(summary, node->as.member_access.object);
            break;
        case AST_INDEX_GET:
            xa_summary_walk(summary, node->as.index_get.array);
            xa_summary_walk(summary, node->as.index_get.index);
            break;
        case AST_TERNARY:
            xa_summary_walk(summary, node->as.ternary.condition);
            xa_summary_walk(summary, node->as.ternary.true_expr);
            xa_summary_walk(summary, node->as.ternary.false_expr);
            break;
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_NULLISH_COALESCE:
            xa_summary_walk(summary, node->as.binary.left);
            xa_summary_walk(summary, node->as.binary.right);
            break;
        default:
            break;
    }
}

static bool xa_symbol_links_set_param_escape_summary(XaInferContext *ctx, XaSymbolLinks *links,
                                                     XrType **param_types, const char **param_names,
                                                     int param_count, XrType *return_type,
                                                     AstNode *body, XrClassInfo *receiver_info) {
    if (!links || param_count <= 0 || !body)
        return false;
    uint8_t *escapes = xr_calloc((size_t) param_count, sizeof(uint8_t));
    if (!escapes)
        return false;
    XaParamEscapeSummary summary = {.param_types = param_types,
                                    .param_names = param_names,
                                    .param_count = param_count,
                                    .escapes = escapes,
                                    .return_type = return_type,
                                    .ctx = ctx,
                                    .receiver_info = receiver_info};
    xa_summary_walk(&summary, body);

    bool changed = links->param_escape_count != param_count || !links->param_escapes;
    if (!changed) {
        for (int i = 0; i < param_count; i++) {
            if (links->param_escapes[i] != escapes[i]) {
                changed = true;
                break;
            }
        }
    }
    if (links->param_escapes)
        xr_free(links->param_escapes);
    links->param_escapes = escapes;
    links->param_escape_count = param_count;
    return changed;
}

static bool xa_expr_roots_at_this(AstNode *node) {
    if (!node)
        return false;
    if (xa_expr_is_this(node))
        return true;
    switch (node->type) {
        case AST_MEMBER_ACCESS:
            return xa_expr_roots_at_this(node->as.member_access.object);
        case AST_INDEX_GET:
            return xa_expr_roots_at_this(node->as.index_get.array);
        default:
            return false;
    }
}

static XrClassInfo *xa_type_class_info(XrType *type) {
    if (!type || (!XR_TYPE_IS_INSTANCE(type) && !XR_TYPE_IS_CLASS(type)))
        return NULL;
    return type->instance.class_ref;
}

static XrClassInfo *xa_receiver_expr_class_info(XrClassInfo *receiver_info, AstNode *node) {
    if (!receiver_info || !node)
        return NULL;
    if (xa_expr_is_this(node))
        return receiver_info;
    if (node->type != AST_MEMBER_ACCESS)
        return NULL;

    XrClassInfo *owner_info =
        xa_receiver_expr_class_info(receiver_info, node->as.member_access.object);
    if (!owner_info)
        return NULL;

    XaSymbol *member = xa_class_info_lookup_member(owner_info, node->as.member_access.name);
    if (!member || (member->kind != XA_SYM_FIELD && member->kind != XA_SYM_PROPERTY))
        return NULL;

    return xa_type_class_info(member->links.type);
}

static XaSymbol *xa_receiver_method_symbol_for_call(XrClassInfo *receiver_info, AstNode *object,
                                                    const char *method_name) {
    XrClassInfo *target_info = xa_receiver_expr_class_info(receiver_info, object);
    if (!target_info)
        return NULL;
    XaSymbol *method = xa_class_info_lookup_member(target_info, method_name);
    return (method && method->kind == XA_SYM_METHOD) ? method : NULL;
}

static bool xa_method_body_mutates_receiver(AstNode *node, XrClassInfo *receiver_info) {
    if (!node)
        return false;
    switch (node->type) {
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) {
                if (xa_method_body_mutates_receiver(node->as.block.statements[i], receiver_info))
                    return true;
            }
            return false;
        case AST_EXPR_STMT:
            return xa_method_body_mutates_receiver(node->as.expr_stmt, receiver_info);
        case AST_MEMBER_SET:
            return xa_expr_roots_at_this(node->as.member_set.object) ||
                   xa_method_body_mutates_receiver(node->as.member_set.value, receiver_info);
        case AST_INDEX_SET:
            return xa_expr_roots_at_this(node->as.index_set.array) ||
                   xa_method_body_mutates_receiver(node->as.index_set.index, receiver_info) ||
                   xa_method_body_mutates_receiver(node->as.index_set.value, receiver_info);
        case AST_CALL_EXPR: {
            CallExprNode *call = &node->as.call_expr;
            if (call->callee && call->callee->type == AST_MEMBER_ACCESS) {
                MemberAccessNode *ma = &call->callee->as.member_access;
                if (xa_expr_roots_at_this(ma->object)) {
                    XaSymbol *method =
                        xa_receiver_method_symbol_for_call(receiver_info, ma->object, ma->name);
                    if (method) {
                        if (method->mutates_receiver)
                            return true;
                    } else if (!xa_expr_is_this(ma->object) &&
                               xa_method_name_mutates_receiver(ma->name)) {
                        return true;
                    }
                }
            }
            for (int i = 0; i < call->arg_count; i++) {
                if (xa_method_body_mutates_receiver(call->arguments[i], receiver_info))
                    return true;
            }
            return false;
        }
        case AST_ASSIGNMENT:
            return (node->as.assignment.name && strcmp(node->as.assignment.name, "this") == 0) ||
                   xa_method_body_mutates_receiver(node->as.assignment.value, receiver_info);
        case AST_COMPOUND_ASSIGNMENT:
            return (node->as.compound_assignment.name &&
                    strcmp(node->as.compound_assignment.name, "this") == 0) ||
                   xa_method_body_mutates_receiver(node->as.compound_assignment.object,
                                                   receiver_info) ||
                   xa_method_body_mutates_receiver(node->as.compound_assignment.value,
                                                   receiver_info);
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            return xa_method_body_mutates_receiver(node->as.var_decl.initializer, receiver_info);
        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++) {
                if (xa_method_body_mutates_receiver(node->as.return_stmt.values[i], receiver_info))
                    return true;
            }
            return false;
        case AST_IF_STMT:
            return xa_method_body_mutates_receiver(node->as.if_stmt.condition, receiver_info) ||
                   xa_method_body_mutates_receiver(node->as.if_stmt.then_branch, receiver_info) ||
                   xa_method_body_mutates_receiver(node->as.if_stmt.else_branch, receiver_info);
        case AST_WHILE_STMT:
            return xa_method_body_mutates_receiver(node->as.while_stmt.condition, receiver_info) ||
                   xa_method_body_mutates_receiver(node->as.while_stmt.body, receiver_info);
        case AST_FOR_STMT:
            return xa_method_body_mutates_receiver(node->as.for_stmt.initializer, receiver_info) ||
                   xa_method_body_mutates_receiver(node->as.for_stmt.condition, receiver_info) ||
                   xa_method_body_mutates_receiver(node->as.for_stmt.increment, receiver_info) ||
                   xa_method_body_mutates_receiver(node->as.for_stmt.body, receiver_info);
        case AST_FOR_IN_STMT:
            return xa_method_body_mutates_receiver(node->as.for_in_stmt.collection,
                                                   receiver_info) ||
                   xa_method_body_mutates_receiver(node->as.for_in_stmt.body, receiver_info);
        case AST_DESTRUCTURE_DECL:
            return xa_method_body_mutates_receiver(node->as.destructure_decl.initializer,
                                                   receiver_info);
        case AST_DESTRUCTURE_ASSIGN:
            return xa_method_body_mutates_receiver(node->as.destructure_assign.value,
                                                   receiver_info);
        case AST_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.count; i++) {
                if (xa_method_body_mutates_receiver(node->as.array_literal.elements[i],
                                                    receiver_info))
                    return true;
            }
            return false;
        case AST_TUPLE_LITERAL:
            for (int i = 0; i < node->as.tuple_literal.count; i++) {
                if (xa_method_body_mutates_receiver(node->as.tuple_literal.elements[i],
                                                    receiver_info))
                    return true;
            }
            return false;
        case AST_OBJECT_LITERAL:
            for (int i = 0; i < node->as.object_literal.count; i++) {
                if (xa_method_body_mutates_receiver(node->as.object_literal.keys[i],
                                                    receiver_info) ||
                    xa_method_body_mutates_receiver(node->as.object_literal.values[i],
                                                    receiver_info))
                    return true;
            }
            return false;
        case AST_MAP_LITERAL:
            for (int i = 0; i < node->as.map_literal.count; i++) {
                if (xa_method_body_mutates_receiver(node->as.map_literal.keys[i], receiver_info) ||
                    xa_method_body_mutates_receiver(node->as.map_literal.values[i], receiver_info))
                    return true;
            }
            return false;
        case AST_SET_LITERAL:
            for (int i = 0; i < node->as.set_literal.count; i++) {
                if (xa_method_body_mutates_receiver(node->as.set_literal.elements[i],
                                                    receiver_info))
                    return true;
            }
            return false;
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
            return xa_method_body_mutates_receiver(node->as.binary.left, receiver_info) ||
                   xa_method_body_mutates_receiver(node->as.binary.right, receiver_info);
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            return xa_method_body_mutates_receiver(node->as.unary.operand, receiver_info);
        case AST_INDEX_GET:
            return xa_method_body_mutates_receiver(node->as.index_get.array, receiver_info) ||
                   xa_method_body_mutates_receiver(node->as.index_get.index, receiver_info);
        case AST_MEMBER_ACCESS:
            return xa_method_body_mutates_receiver(node->as.member_access.object, receiver_info);
        case AST_TERNARY:
            return xa_method_body_mutates_receiver(node->as.ternary.condition, receiver_info) ||
                   xa_method_body_mutates_receiver(node->as.ternary.true_expr, receiver_info) ||
                   xa_method_body_mutates_receiver(node->as.ternary.false_expr, receiver_info);
        case AST_MOVE_EXPR:
            return xa_method_body_mutates_receiver(node->as.move_expr.expr, receiver_info);
        case AST_UNSAFE_EXPR:
            return xa_method_body_mutates_receiver(node->as.unsafe_expr.operand, receiver_info);
        case AST_AWAIT_EXPR:
            return xa_method_body_mutates_receiver(node->as.await_expr.expr, receiver_info) ||
                   xa_method_body_mutates_receiver(node->as.await_expr.timeout, receiver_info);
        case AST_SCOPE_BLOCK:
            return xa_method_body_mutates_receiver(node->as.scope_block.body, receiver_info);
        case AST_DEFER_STMT:
            return xa_method_body_mutates_receiver(node->as.defer_stmt.expr, receiver_info);
        default:
            return false;
    }
}

static bool xa_class_propagate_receiver_mutations(XrClassInfo *info, ClassDeclNode *cls) {
    if (!info || !cls)
        return false;

    bool any_changed = false;
    bool changed;
    do {
        changed = false;
        for (int i = 0; i < cls->method_count; i++) {
            AstNode *method = cls->methods[i];
            if (!method || method->type != AST_METHOD_DECL)
                continue;
            MethodDeclNode *md = &method->as.method_decl;
            if (md->is_static)
                continue;
            XaSymbol *method_sym = xa_class_info_lookup_member(info, md->name);
            if (!method_sym || method_sym->kind != XA_SYM_METHOD || method_sym->mutates_receiver)
                continue;
            if (xa_method_body_mutates_receiver(md->body, info)) {
                method_sym->mutates_receiver = true;
                changed = true;
                any_changed = true;
            }
        }
    } while (changed);
    return any_changed;
}

XR_FUNC bool xa_propagate_receiver_mutations_for_ast(XaAnalyzer *analyzer, AstNode *node) {
    if (!analyzer || !node)
        return false;

    bool changed = false;
    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) {
                if (xa_propagate_receiver_mutations_for_ast(analyzer,
                                                            node->as.program.statements[i]))
                    changed = true;
            }
            return changed;
        case AST_EXPORT_STMT:
            return xa_propagate_receiver_mutations_for_ast(analyzer,
                                                           node->as.export_stmt.declaration);
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            ClassDeclNode *cls =
                (node->type == AST_STRUCT_DECL) ? &node->as.struct_decl : &node->as.class_decl;
            if (!cls->name)
                return false;
            XaSymbol *class_sym = xa_scope_lookup(analyzer->global_scope, cls->name);
            XaSymbolLinks *links = class_sym ? xa_analyzer_get_links(analyzer, class_sym) : NULL;
            return links && links->class_info
                       ? xa_class_propagate_receiver_mutations(links->class_info, cls)
                       : false;
        }
        default:
            return false;
    }
}

static bool xa_propagate_function_param_escape_summary(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node || node->type != AST_FUNCTION_DECL)
        return false;
    FunctionDeclNode *fn = &node->as.function_decl;
    if (!fn->name)
        return false;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, fn->name);
    if (!sym && ctx->analyzer->global_scope)
        sym = xa_scope_lookup(ctx->analyzer->global_scope, fn->name);
    if (!sym || sym->kind != XA_SYM_FUNCTION)
        return false;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    return xa_symbol_links_set_param_escape_summary(
        ctx, links, links ? links->param_types : NULL, links ? links->param_names : NULL,
        links ? links->param_count : 0, links ? links->return_type : NULL, fn->body, NULL);
}

static bool xa_propagate_class_param_escape_summaries(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node || (node->type != AST_CLASS_DECL && node->type != AST_STRUCT_DECL))
        return false;
    ClassDeclNode *cls =
        (node->type == AST_STRUCT_DECL) ? &node->as.struct_decl : &node->as.class_decl;
    if (!cls->name)
        return false;

    XaSymbol *class_sym = xa_scope_lookup(ctx->analyzer->global_scope, cls->name);
    XaSymbolLinks *class_links = class_sym ? xa_analyzer_get_links(ctx->analyzer, class_sym) : NULL;
    XrClassInfo *info = class_links ? class_links->class_info : NULL;
    if (!info)
        return false;

    bool changed = false;
    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_CLASS, node);
    for (int i = 0; i < cls->method_count; i++) {
        AstNode *method = cls->methods ? cls->methods[i] : NULL;
        if (!method || method->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *md = &method->as.method_decl;
        XaSymbol *method_sym = xa_class_info_lookup_member(info, md->name);
        XaSymbolLinks *method_links =
            method_sym ? xa_analyzer_get_links(ctx->analyzer, method_sym) : NULL;
        if (!method_links)
            continue;
        if (xa_symbol_links_set_param_escape_summary(
                ctx, method_links, method_links->param_types, method_links->param_names,
                method_links->param_count, method_links->return_type, md->body, info)) {
            changed = true;
        }
    }
    xa_analyzer_exit_scope(ctx->analyzer);
    return changed;
}

XR_FUNC bool xa_propagate_param_escape_summaries_for_ast(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !ctx->analyzer || !node)
        return false;

    bool changed = false;
    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) {
                if (xa_propagate_param_escape_summaries_for_ast(ctx,
                                                                node->as.program.statements[i]))
                    changed = true;
            }
            return changed;
        case AST_EXPORT_STMT:
            return xa_propagate_param_escape_summaries_for_ast(ctx,
                                                               node->as.export_stmt.declaration);
        case AST_FUNCTION_DECL:
            return xa_propagate_function_param_escape_summary(ctx, node);
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
            return xa_propagate_class_param_escape_summaries(ctx, node);
        default:
            return false;
    }
}

// Recursively collect all return types from a statement tree
static void collect_return_types(XaInferContext *ctx, AstNode *node, XrType ***types, int *count,
                                 int *cap) {
    XR_DCHECK(ctx != NULL, "collect_return_types: NULL ctx");
    if (!node)
        return;

    switch (node->type) {
        case AST_RETURN_STMT: {
            ReturnStmtNode *ret = &node->as.return_stmt;
            XrType *rt = xr_type_new_unit(NULL);
            if (ret->value_count == 1 && ret->values && ret->values[0]) {
                rt = xa_visit_infer(ctx, ret->values[0]);
            } else if (ret->value_count > 1) {
                XrType **elems = xr_malloc(sizeof(XrType *) * ret->value_count);
                for (int i = 0; i < ret->value_count; i++) {
                    elems[i] = ret->values[i] ? xa_visit_infer(ctx, ret->values[i])
                                              : xr_type_new_unknown(NULL);
                }
                rt = xr_type_new_tuple(ctx->analyzer->isolate, elems, ret->value_count);
                xr_free(elems);
            }
            // Add to collected types
            if (*count >= *cap) {
                int new_cap = *cap ? *cap * 2 : 8;
                XrType **tmp = xr_realloc(*types, sizeof(XrType *) * new_cap);
                if (!tmp)
                    break;
                *types = tmp;
                *cap = new_cap;
            }
            (*types)[(*count)++] = rt;
            break;
        }
        case AST_BLOCK: {
            BlockNode *block = &node->as.block;
            for (int i = 0; i < block->count; i++) {
                collect_return_types(ctx, block->statements[i], types, count, cap);
            }
            break;
        }
        case AST_IF_STMT:
            collect_return_types(ctx, node->as.if_stmt.then_branch, types, count, cap);
            collect_return_types(ctx, node->as.if_stmt.else_branch, types, count, cap);
            break;
        case AST_WHILE_STMT:
            collect_return_types(ctx, node->as.while_stmt.body, types, count, cap);
            break;
        case AST_FOR_STMT:
            collect_return_types(ctx, node->as.for_stmt.body, types, count, cap);
            break;
        case AST_FOR_IN_STMT:
            collect_return_types(ctx, node->as.for_in_stmt.body, types, count, cap);
            break;
        case AST_TRY_CATCH:
            collect_return_types(ctx, node->as.try_catch.try_body, types, count, cap);
            for (int ci = 0; ci < node->as.try_catch.catch_count; ci++) {
                XrCatchClause *cc = node->as.try_catch.catch_clauses[ci];
                if (cc)
                    collect_return_types(ctx, cc->body, types, count, cap);
            }
            // finally return is NOT collected: a return inside finally overrides the
            // try/catch return value entirely, so it must not be unioned with them.
            break;
        case AST_MATCH_EXPR: {
            MatchExprNode *m = &node->as.match_expr;
            for (int i = 0; i < m->arm_count; i++) {
                if (m->arms[i] && m->arms[i]->type == AST_MATCH_ARM) {
                    collect_return_types(ctx, m->arms[i]->as.match_arm.body, types, count, cap);
                }
            }
            break;
        }
        default:
            break;
    }
}

// Infer return type by scanning all return statements in function/method body
XrType *xa_infer_function_return_type(XaInferContext *ctx, AstNode *body) {
    if (!body)
        return NULL;

    XrType **types = NULL;
    int count = 0, cap = 0;
    collect_return_types(ctx, body, &types, &count, &cap);

    if (count == 0) {
        if (types)
            xr_free(types);
        return NULL;
    }

    // Union all collected return types
    XrType *result = types[0];
    for (int i = 1; i < count; i++) {
        if (!xr_type_equals(result, types[i])) {
            result = xr_type_union(ctx->analyzer->isolate, result, types[i]);
        }
    }

    xr_free(types);
    return result;
}

static XrClassInfo *xa_default_init_class_info(XaInferContext *ctx, XrType *type) {
    if (!ctx || !ctx->analyzer || !type)
        return NULL;
    if ((type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) &&
        type->instance.class_ref) {
        return type->instance.class_ref;
    }
    if (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE)
        return NULL;

    const char *name = type->instance.class_name;
    if (!name)
        return NULL;
    XaSymbol *sym = xa_analyzer_lookup(ctx->analyzer, name);
    if (!sym || sym->kind != XA_SYM_CLASS) {
        sym = xa_analyzer_lookup_in_scope(ctx->analyzer, name, ctx->analyzer->global_scope);
    }
    if (!sym || sym->kind != XA_SYM_CLASS) {
        sym = xa_analyzer_lookup_deep(ctx->analyzer, name);
    }
    if (!sym || sym->kind != XA_SYM_CLASS)
        return NULL;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    return links ? links->class_info : NULL;
}

static bool xa_type_is_default_initializable_depth(XaInferContext *ctx, XrType *type, int depth) {
    if (!type)
        return false;
    if (depth > 16)
        return false;

    if (xr_type_is_default_initializable(type))
        return true;

    if (type->kind == XR_KIND_FIXED_ARRAY) {
        return type->fixed_array.length >= 0 && xa_type_is_default_initializable_depth(
                                                    ctx, type->fixed_array.element_type, depth + 1);
    }

    if (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE)
        return false;

    XrClassInfo *info = xa_default_init_class_info(ctx, type);
    if (!info)
        return false;

    bool is_struct = type->is_value_type || info->struct_layout != NULL;
    if (!is_struct)
        return false;

    for (int i = 0; i < info->field_count; i++) {
        XaSymbol *field = info->fields[i];
        XaSymbolLinks *links = field ? xa_analyzer_get_links(ctx->analyzer, field) : NULL;
        if (!links || !links->type)
            return false;
        if (!xa_type_is_default_initializable_depth(ctx, links->type, depth + 1))
            return false;
    }
    return true;
}

bool xa_type_is_default_initializable(XaInferContext *ctx, XrType *type) {
    return xa_type_is_default_initializable_depth(ctx, type, 0);
}

// Phase 1: Collect function declaration only (symbol, not body).
// Cross-TU: called from xa_visit_collect_statements_with_hoisting() in
// xanalyzer_visitor.c during the hoisting pass.
void xa_visit_collect_function_decl_only(XaInferContext *ctx, AstNode *node) {
    if (!node)
        return;

    FunctionDeclNode *fn = &node->as.function_decl;

    // Create function symbol
    XaSymbol *sym = xa_symbol_new(fn->name, XA_SYM_FUNCTION);
    sym->location.line = node->line;
    sym->is_const = true;

    // Build function type and collect param names
    XrType **param_types = NULL;
    const char **param_names = NULL;
    bool has_rest = false;

    if (fn->param_count > 0) {
        param_types = xr_malloc(sizeof(XrType *) * fn->param_count);
        param_names = xr_malloc(sizeof(const char *) * fn->param_count);
        if (!param_types || !param_names) {
            xr_free(param_types);
            xr_free(param_names);
            return;
        }
        for (int i = 0; i < fn->param_count; i++) {
            XrParamNode *param = fn->params[i];
            param_types[i] = (param && param->type)
                                 ? xr_tref_resolve(ctx->analyzer->isolate, param->type)
                                 : xr_type_new_unknown(NULL);
            param_names[i] = param ? param->name : NULL;
            if (param && param->is_rest)
                has_rest = true;

            // Warn: function parameter missing type annotation
            if (param && !param->type && !param->is_rest) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Parameter '%s' of function '%s' is missing type annotation", param->name,
                         fn->name ? fn->name : "<anonymous>");
                XrLocation loc = {
                    .file = ctx->file_path, .line = param->line, .column = param->column};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
            }
        }
    }

    // Omitted return type defaults to void; error if body has 'return <expr>'
    XrType *return_type = fn->return_type ? xr_tref_resolve(ctx->analyzer->isolate, fn->return_type)
                                          : xr_type_new_unit(NULL);
    if (!fn->return_type && fn->name && fn->body) {
        if (xa_body_has_return_expr(fn->body)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Function '%s' returns a value but has no return type annotation", fn->name);
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
        }
    }
    // Resolve CLASS("T") → TYPE_PARAM("T") for generic functions
    if (fn->type_param_count > 0 && fn->type_params) {
        const char *tp_buf[8];
        const char **tp_names = (fn->type_param_count <= 8)
                                    ? tp_buf
                                    : xr_malloc(sizeof(const char *) * fn->type_param_count);
        for (int i = 0; i < fn->type_param_count; i++)
            tp_names[i] = fn->type_params[i]->name;
        for (int i = 0; i < fn->param_count; i++)
            param_types[i] =
                resolve_class_to_type_param(NULL, param_types[i], tp_names, fn->type_param_count);
        return_type =
            resolve_class_to_type_param(NULL, return_type, tp_names, fn->type_param_count);
        if (tp_names != tp_buf)
            xr_free((void *) tp_names);
    }

    XrType *fn_type = xr_type_new_function(ctx->analyzer->isolate, param_types, fn->param_count,
                                           return_type, has_rest);
    xa_set_function_type_params_from_ast(ctx, fn_type, fn->type_params, fn->type_param_count);

    // Set min_params for default parameter support
    if (fn_type) {
        fn_type->function.min_params = fn->required_count;

        // Propagate in/ref passing modes to the function type
        bool has_modes = false;
        for (int i = 0; i < fn->param_count && !has_modes; i++) {
            if (fn->params[i] && fn->params[i]->passing_mode != XR_PARAM_VALUE)
                has_modes = true;
        }
        if (has_modes) {
            uint8_t *modes = xr_calloc(fn->param_count, sizeof(uint8_t));
            if (modes) {
                for (int i = 0; i < fn->param_count; i++) {
                    if (fn->params[i])
                        modes[i] = fn->params[i]->passing_mode;
                }
                fn_type->function.param_passing_modes = modes;
            }
        }
    }

    // Add to scope
    xa_visit_add_symbol_checked(ctx, sym, 0);
    fn->symbol_id = sym->id;

    // Create symbol links with type and param names
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    links->type = fn_type;
    links->declared_type = fn_type;
    links->file_path = ctx->file_path;

    // FFI: mark @extern functions so call sites can require `unsafe { }`.
    XrAttribute *c_export_attr = xa_function_attr(fn, ATTR_C_EXPORT);
    links->is_extern = xa_function_attr(fn, ATTR_EXTERN) != NULL;
    links->is_c_export = c_export_attr != NULL;
    links->c_export_symbol =
        c_export_attr && c_export_attr->str_arg ? xr_strdup(c_export_attr->str_arg) : NULL;
    if (links->is_extern)
        xa_validate_extern_function_abi(ctx, node, fn, param_types, return_type);
    if (c_export_attr)
        xa_validate_c_export_function_abi(ctx, node, fn, sym, param_types, return_type,
                                          c_export_attr, links->is_extern);

    // Store parameter names for LSP inlay hints
    xa_symbol_links_set_function_sig(links, param_types, param_names, fn->param_count, return_type);
    xa_symbol_links_set_param_escape_summary(ctx, links, param_types, param_names, fn->param_count,
                                             return_type, fn->body, NULL);

    // Record per-parameter default expressions for caller-side default filling.
    if (fn->param_count > 0) {
        AstNode **defs = (AstNode **) xr_calloc(fn->param_count, sizeof(AstNode *));
        if (defs) {
            for (int i = 0; i < fn->param_count; i++)
                defs[i] = fn->params[i] ? fn->params[i]->default_value : NULL;
            xa_symbol_links_set_param_defaults(links, defs, fn->param_count);
            xr_free(defs);
        }
    }

    // Store generic type parameters and intersection-style constraint lists.
    if (fn->type_param_count > 0 && fn->type_params) {
        int n = fn->type_param_count;
        const char **type_param_names = xr_malloc(sizeof(const char *) * n);
        XrType ***constraint_lists = xr_malloc(sizeof(XrType **) * n);
        int *constraint_counts = xr_malloc(sizeof(int) * n);

        if (type_param_names && constraint_lists && constraint_counts) {
            for (int i = 0; i < n; i++) {
                XrGenericParam *gp = fn->type_params[i];
                type_param_names[i] = gp->name;

                int cn = gp->constraint_count;
                if (cn > 0 && gp->constraints) {
                    XrType **resolved = xr_malloc(sizeof(XrType *) * cn);
                    for (int j = 0; j < cn; j++) {
                        // Use analyzer-aware resolver so class-bounded constraints
                        // (e.g. <T: Animal>) keep their inheritance chain.
                        resolved[j] =
                            gp->constraints[j]
                                ? xr_tref_resolve_in_analyzer(ctx->analyzer, gp->constraints[j])
                                : NULL;
                    }
                    constraint_lists[i] = resolved;
                    constraint_counts[i] = cn;
                } else {
                    constraint_lists[i] = NULL;
                    constraint_counts[i] = 0;
                }
            }

            xa_symbol_links_set_type_params(links, type_param_names, constraint_lists,
                                            constraint_counts, n);

            // set_type_params deep-copies constraint arrays — release temporaries.
            for (int i = 0; i < n; i++) {
                if (constraint_lists[i])
                    xr_free(constraint_lists[i]);
            }
        }

        xr_free(type_param_names);
        xr_free(constraint_lists);
        xr_free(constraint_counts);
    }

    XrLocation sig_loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    for (int i = 0; i < fn->param_count; i++) {
        XrParamNode *param = fn->params ? fn->params[i] : NULL;
        XrLocation param_loc = {.file = ctx->file_path,
                                .line = param ? param->line : node->line,
                                .column = param ? param->column : node->column};
        xa_validate_hashable_key_type(ctx, param_types ? param_types[i] : NULL, links,
                                      "function parameter type", &param_loc);
    }
    xa_validate_hashable_key_type(ctx, return_type, links, "function return type", &sig_loc);

    if (param_types)
        xr_free(param_types);
    if (param_names)
        xr_free(param_names);
}

// Collect return-value AST nodes from a function body.
// Only collects object-literal returns; sets out_bad if a non-object, non-null return is found.
static void xa_collect_returns(AstNode *node, AstNode **out, int *count, int cap, bool *out_bad) {
    if (!node || *out_bad)
        return;
    switch (node->type) {
        case AST_BLOCK: {
            BlockNode *blk = &node->as.block;
            for (int i = 0; i < blk->count; i++)
                xa_collect_returns(blk->statements[i], out, count, cap, out_bad);
            break;
        }
        case AST_IF_STMT: {
            IfStmtNode *ifn = &node->as.if_stmt;
            xa_collect_returns(ifn->then_branch, out, count, cap, out_bad);
            if (ifn->else_branch)
                xa_collect_returns(ifn->else_branch, out, count, cap, out_bad);
            break;
        }
        case AST_RETURN_STMT: {
            ReturnStmtNode *ret = &node->as.return_stmt;
            if (ret->value_count == 0)
                break;
            AstNode *val = ret->values[0];
            if (val->type == AST_LITERAL_NULL)
                break;
            if (val->type == AST_OBJECT_LITERAL) {
                if (*count < cap)
                    out[(*count)++] = val;
            } else {
                *out_bad = true;
            }
            break;
        }
        default:
            break;
    }
}

// Infer Json return type for a function whose returns are all same-shape object literals.
// Returns an interned XrType (XR_KIND_JSON) or NULL.
static XrType *xa_infer_return_json_type(XrayIsolate *X, FunctionDeclNode *fn) {
    if (!fn->body || fn->return_type)
        return NULL;

    static const int MAX_RETURNS = 32;
    static const int MAX_FIELDS = 32;
    AstNode *rets[32];
    int nrets = 0;
    bool bad = false;
    xa_collect_returns(fn->body, rets, &nrets, MAX_RETURNS, &bad);
    if (bad || nrets == 0)
        return NULL;

    ObjectLiteralNode *first = &rets[0]->as.object_literal;
    int fc = 0;
    for (int i = 0; i < first->count; i++) {
        if ((!first->computed || !first->computed[i]) && first->keys[i]->type == AST_LITERAL_STRING)
            fc++;
    }
    if (fc == 0 || fc > MAX_FIELDS)
        return NULL;

    // Verify all returns have same static field names (order-insensitive)
    for (int r = 1; r < nrets; r++) {
        ObjectLiteralNode *o = &rets[r]->as.object_literal;
        int ofc = 0;
        for (int i = 0; i < o->count; i++)
            if ((!o->computed || !o->computed[i]) && o->keys[i]->type == AST_LITERAL_STRING)
                ofc++;
        if (ofc != fc)
            return NULL;
        for (int i = 0; i < first->count; i++) {
            if (first->computed && first->computed[i])
                continue;
            if (first->keys[i]->type != AST_LITERAL_STRING)
                continue;
            const char *fname = first->keys[i]->as.literal.raw_value.string_val;
            bool found = false;
            for (int j = 0; j < o->count; j++) {
                if (o->computed && o->computed[j])
                    continue;
                if (o->keys[j]->type != AST_LITERAL_STRING)
                    continue;
                if (strcmp(o->keys[j]->as.literal.raw_value.string_val, fname) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found)
                return NULL;
        }
    }

    // Build field name + type arrays from first return's object literal
    const char *names[32];
    XrType *types[32];
    int idx = 0;
    for (int i = 0; i < first->count && idx < 32; i++) {
        if (first->computed && first->computed[i])
            continue;
        if (first->keys[i]->type != AST_LITERAL_STRING)
            continue;
        names[idx] = first->keys[i]->as.literal.raw_value.string_val;
        // Infer field type from AST literal (Pass 1: no full inference available)
        AstNode *val = first->values[i];
        switch (val ? val->type : 0) {
            case AST_LITERAL_INT:
                types[idx] = xr_type_new_int(NULL);
                break;
            case AST_LITERAL_FLOAT:
                types[idx] = xr_type_new_float(NULL);
                break;
            case AST_LITERAL_STRING:
                types[idx] = xr_type_new_string(NULL);
                break;
            case AST_LITERAL_TRUE:
            case AST_LITERAL_FALSE:
                types[idx] = xr_type_new_bool(NULL);
                break;
            case AST_LITERAL_NULL:
                types[idx] = xr_type_new_unknown(NULL);
                break;
            default:
                types[idx] = xr_type_new_unknown(NULL);
                break;
        }
        idx++;
    }
    return xr_type_new_json_with_fields(X, names, types, fc, true);
}

// Phase 2: Collect function body (parameters and body declarations).
// Cross-TU: called from xa_visit_collect_statements_with_hoisting() in
// xanalyzer_visitor.c after Phase 1 has hoisted all symbols.
void xa_visit_collect_function_body(XaInferContext *ctx, AstNode *node) {
    if (!node)
        return;

    FunctionDeclNode *fn = &node->as.function_decl;

    // Get function type from already-created symbol
    XaSymbol *sym = xa_scope_lookup_local(ctx->analyzer->current_scope, fn->name);
    XaSymbolLinks *links = sym ? xa_analyzer_get_links(ctx->analyzer, sym) : NULL;

    // Enter function scope and collect body
    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, node);
    ctx->analyzer->current_scope->function_symbol = sym;

    // Add parameters to scope
    for (int i = 0; i < fn->param_count; i++) {
        XrParamNode *p = fn->params[i];
        if (p && p->name) {
            XaSymbol *param = xa_symbol_new(p->name, XA_SYM_PARAMETER);
            param->location.line = p->line > 0 ? p->line : node->line;
            param->passing_mode = p->passing_mode;
            xa_visit_add_symbol_checked(ctx, param, 0);
            p->symbol_id = param->id;

            XaSymbolLinks *param_links = xa_analyzer_get_links(ctx->analyzer, param);
            if (p->is_rest) {
                // Rest parameter is packed into Array at runtime
                XrType *elem_type = (links && links->param_types && i < links->param_count)
                                        ? links->param_types[i]
                                        : xr_type_new_unknown(NULL);
                param_links->type = xr_type_new_array(ctx->analyzer->isolate, elem_type);
            } else {
                param_links->type = (links && links->param_types && i < links->param_count)
                                        ? links->param_types[i]
                                        : xr_type_new_unknown(NULL);
            }
            param_links->is_definitely_assigned = true;
        }
    }

    // Collect body declarations
    if (fn->body) {
        xa_visit_collect(ctx, fn->body);
    }

    if (links) {
        xa_symbol_links_set_param_escape_summary(ctx, links, links->param_types, links->param_names,
                                                 links->param_count, links->return_type, fn->body,
                                                 NULL);
    }

    // Infer return type for unannotated functions that always return same-shape objects.
    // This updates the function's return type so that call-site type propagation
    // can see a concrete Json type instead of unknown.
    if (links && !fn->return_type) {
        XrType *inferred_ret = xa_infer_return_json_type(ctx->analyzer->isolate, fn);
        if (inferred_ret) {
            links->return_type = inferred_ret;
            links->return_type_inferred = true;
            // Also update the function type object so xa_visit_call sees it
            if (links->type && XR_TYPE_IS_FUNCTION(links->type)) {
                links->type->function.return_type = inferred_ret;
            }
        }
    }

    xa_analyzer_exit_scope(ctx->analyzer);
}

// Combined: for direct calls (not through hoisting)
void xa_visit_collect_function(XaInferContext *ctx, AstNode *node) {
    xa_visit_collect_function_decl_only(ctx, node);
    xa_visit_collect_function_body(ctx, node);
}

/* ----------------------------------------------------------------------------
 * Constructor super() Validation
 * -------------------------------------------------------------------------- */

// Check if AST node contains 'this' expression (before super() call)
static bool contains_this_expr(AstNode *node) {
    if (!node)
        return false;

    switch (node->type) {
        case AST_THIS_EXPR:
            return true;
        case AST_CALL_EXPR:
            if (contains_this_expr(node->as.call_expr.callee))
                return true;
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                if (contains_this_expr(node->as.call_expr.arguments[i]))
                    return true;
            }
            break;
        case AST_MEMBER_ACCESS:
            return contains_this_expr(node->as.member_access.object);
        case AST_INDEX_GET:
            return contains_this_expr(node->as.index_get.array) ||
                   contains_this_expr(node->as.index_get.index);
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            return contains_this_expr(node->as.binary.left) ||
                   contains_this_expr(node->as.binary.right);
        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            return contains_this_expr(node->as.unary.operand);
        case AST_TERNARY:
            return contains_this_expr(node->as.ternary.condition) ||
                   contains_this_expr(node->as.ternary.true_expr) ||
                   contains_this_expr(node->as.ternary.false_expr);
        case AST_ASSIGNMENT:
            return contains_this_expr(node->as.assignment.value);
        default:
            break;
    }
    return false;
}

// Check if statement contains 'this' expression
static bool stmt_contains_this(AstNode *stmt) {
    if (!stmt)
        return false;

    switch (stmt->type) {
        case AST_EXPR_STMT:
            return contains_this_expr(stmt->as.expr_stmt);
        case AST_VAR_DECL:
        case AST_CONST_DECL:
            return contains_this_expr(stmt->as.var_decl.initializer);
        case AST_ASSIGNMENT:
            return contains_this_expr(stmt->as.assignment.value);
        case AST_MEMBER_SET:
            return contains_this_expr(stmt->as.member_set.object) ||
                   contains_this_expr(stmt->as.member_set.value);
        case AST_RETURN_STMT:
            for (int i = 0; i < stmt->as.return_stmt.value_count; i++) {
                if (contains_this_expr(stmt->as.return_stmt.values[i]))
                    return true;
            }
            break;
        default:
            break;
    }
    return false;
}

// Validate constructor super() call rules:
// 1. super() must be first statement (if called)
// 2. Cannot access 'this' before super()
// 3. Must call super() if parent has required parameters
static void validate_constructor_super_call(XaInferContext *ctx, ClassDeclNode *cls,
                                            MethodDeclNode *constructor, AstNode *method_node) {
    if (!constructor || !constructor->body)
        return;

    AstNode *body = constructor->body;
    if (body->type != AST_BLOCK)
        return;

    BlockNode *block = &body->as.block;
    bool has_super_call = false;
    int super_call_index = -1;
    int super_call_line = 0;

    // Find super() call position
    for (int i = 0; i < block->count; i++) {
        AstNode *stmt = block->statements[i];
        if (!stmt)
            continue;

        // Check for super() call (as expression statement)
        if (stmt->type == AST_EXPR_STMT && stmt->as.expr_stmt) {
            AstNode *expr = stmt->as.expr_stmt;
            if (expr->type == AST_SUPER_CALL) {
                has_super_call = true;
                super_call_index = i;
                super_call_line = stmt->line;
                break;
            }
        }
        // Also check direct super call statement
        if (stmt->type == AST_SUPER_CALL) {
            has_super_call = true;
            super_call_index = i;
            super_call_line = stmt->line;
            break;
        }
    }

    // Check 1: If class has a parent, validate super() usage
    if (cls->super_name) {
        // Check 2: super() must be first statement (if called)
        if (has_super_call && super_call_index > 0) {
            XrLocation loc = {.file = ctx->file_path, .line = super_call_line};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_SUPER_FIRST,
                                       "super() must be the first statement in constructor", &loc);
        }

        // Check 3: Cannot access 'this' before super()
        if (has_super_call) {
            for (int i = 0; i < super_call_index; i++) {
                AstNode *stmt = block->statements[i];
                if (stmt_contains_this(stmt)) {
                    XrLocation loc = {.file = ctx->file_path, .line = stmt->line};
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_SUPER_THIS,
                                               "Cannot access 'this' before calling super()", &loc);
                    break;
                }
            }
        }

        // Check 4: Smart super() requirement based on parent constructor
        // - Parent has no constructor → no super() needed
        // - Parent constructor has only optional params → auto-insert at codegen
        // - Parent constructor has required params → must call super(args)
        if (!has_super_call) {
            // Look up parent class info (search outside class scope)
            XaSymbol *parent_sym =
                xa_scope_lookup(ctx->analyzer->current_scope->parent, cls->super_name);
            XrClassInfo *parent_info = NULL;
            if (parent_sym) {
                XaSymbolLinks *parent_links = xa_analyzer_get_links(ctx->analyzer, parent_sym);
                if (parent_links)
                    parent_info = parent_links->class_info;
            }

            if (parent_info && parent_info->has_constructor &&
                parent_info->constructor_required_params > 0) {
                // Parent constructor has required params — must call super()
                XrLocation loc = {.file = ctx->file_path, .line = method_node->line};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Constructor must call super() because '%s' constructor requires %d "
                         "argument(s)",
                         cls->super_name, parent_info->constructor_required_params);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_SUPER_REQUIRED, msg, &loc);
            }
            // else: parent has no constructor or all-optional params → OK
        }
    } else {
        // No parent class - super() should not be called
        if (has_super_call) {
            XrLocation loc = {.file = ctx->file_path, .line = super_call_line};
            xa_analyzer_add_diagnostic(
                ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_SUPER_INVALID,
                "super() can only be called in a class that extends another class", &loc);
        }
    }
}

static bool expr_assigns_this_field(AstNode *expr, const char *field_name) {
    if (!expr || !field_name)
        return false;
    if (expr->type != AST_MEMBER_SET)
        return false;

    MemberSetNode *set = &expr->as.member_set;
    return set->object && set->object->type == AST_THIS_EXPR && set->member &&
           strcmp(set->member, field_name) == 0;
}

static bool stmt_definitely_assigns_this_field(AstNode *stmt, const char *field_name) {
    if (!stmt || !field_name)
        return false;

    switch (stmt->type) {
        case AST_BLOCK: {
            BlockNode *block = &stmt->as.block;
            for (int i = 0; i < block->count; i++) {
                if (stmt_definitely_assigns_this_field(block->statements[i], field_name))
                    return true;
            }
            return false;
        }
        case AST_EXPR_STMT:
            return expr_assigns_this_field(stmt->as.expr_stmt, field_name);
        case AST_MEMBER_SET:
            return expr_assigns_this_field(stmt, field_name);
        case AST_IF_STMT:
            return stmt->as.if_stmt.then_branch && stmt->as.if_stmt.else_branch &&
                   stmt_definitely_assigns_this_field(stmt->as.if_stmt.then_branch, field_name) &&
                   stmt_definitely_assigns_this_field(stmt->as.if_stmt.else_branch, field_name);
        default:
            return false;
    }
}

static bool class_has_bodyless_constructor(ClassDeclNode *cls) {
    if (!cls)
        return false;
    for (int i = 0; i < cls->method_count; i++) {
        AstNode *method = cls->methods[i];
        if (!method || method->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *md = &method->as.method_decl;
        if (md->is_constructor && !md->body)
            return true;
    }
    return false;
}

static bool class_constructors_assign_field(ClassDeclNode *cls, const char *field_name) {
    if (!cls || !field_name)
        return false;

    bool saw_constructor = false;
    for (int i = 0; i < cls->method_count; i++) {
        AstNode *method = cls->methods[i];
        if (!method || method->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *md = &method->as.method_decl;
        if (!md->is_constructor)
            continue;
        saw_constructor = true;
        if (!md->body)
            continue;
        if (!stmt_definitely_assigns_this_field(md->body, field_name))
            return false;
    }
    return saw_constructor;
}

static void validate_class_field_default_initialization(XaInferContext *ctx, AstNode *node,
                                                        ClassDeclNode *cls, XrClassInfo *info) {
    if (!ctx || !node || !cls || !info)
        return;
    if (node->type != AST_CLASS_DECL)
        return;
    if (cls->is_native || class_has_bodyless_constructor(cls))
        return;

    for (int i = 0; i < cls->field_count; i++) {
        AstNode *field_node = cls->fields[i];
        if (!field_node || field_node->type != AST_FIELD_DECL)
            continue;

        FieldDeclNode *fd = &field_node->as.field_decl;
        if (fd->initializer)
            continue;

        XaSymbol *field_sym = xa_class_info_lookup_member(info, fd->name);
        XaSymbolLinks *links = field_sym ? xa_analyzer_get_links(ctx->analyzer, field_sym) : NULL;
        XrType *field_type = links ? links->type : NULL;
        if (!field_type || XR_TYPE_IS_UNKNOWN(field_type))
            continue;
        if (xa_type_is_default_initializable(ctx, field_type))
            continue;
        if (!fd->is_static && class_constructors_assign_field(cls, fd->name))
            continue;

        XrLocation loc = {.file = ctx->file_path, .line = field_node->line};
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Field '%s' of class '%s' has type '%s' and must have an initializer or be "
                 "assigned in every constructor",
                 fd->name ? fd->name : "?", cls->name ? cls->name : "?",
                 xr_type_to_string(field_type));
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   msg, &loc);
    }
}

// Register a user-defined interface as a class-shaped symbol so the rest of
// the analyzer (constraint checks, conformance lookups, type-arg resolution)
// can find it through xa_scope_lookup.  Method and property signatures are
// kept on info->methods / info->fields, mirroring what xa_visit_collect_class
// does for real classes — that is what lets the conformance check at the end
// of class collection enforce method-name parity.
void xa_visit_collect_interface(XaInferContext *ctx, AstNode *node) {
    if (!node || node->type != AST_INTERFACE_DECL)
        return;

    InterfaceDeclNode *iface = &node->as.interface_decl;

    XaSymbol *sym = xa_symbol_new(iface->name, XA_SYM_CLASS);
    sym->location.line = node->line;
    xa_visit_add_symbol_checked(ctx, sym, 0);

    XrClassInfo *info = xa_class_info_new(iface->name);
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    links->class_info = info;
    // Represent the interface as a parameterized XR_KIND_INTERFACE: built-in
    // singletons stay as plain interface types; user `interface Foo<T>` keeps
    // its declared type parameters so generic resolution can plug arguments
    // in later (the type_args slot is empty at the declaration site).
    links->type = xr_type_new_interface(ctx->analyzer->isolate, iface->name);
    info->base = NULL;  // interfaces never carry an inheritance chain here
    info->base_name = NULL;

    // Materialise method and property signatures as XaSymbols. Names matter
    // for conformance; types are best-effort (resolved from XrTypeRef) so the
    // later signature audit can still inspect them when needed.
    for (int i = 0; i < iface->method_count; i++) {
        AstNode *m = iface->methods ? iface->methods[i] : NULL;
        if (!m || m->type != AST_INTERFACE_METHOD)
            continue;
        InterfaceMethodNode *im = &m->as.interface_method;
        if (!im->name)
            continue;
        XaSymbol *msym = xa_symbol_new(im->name, XA_SYM_METHOD);
        msym->location.line = m->line;
        XaSymbolLinks *mlinks = xa_analyzer_get_links(ctx->analyzer, msym);
        XrType **param_types = NULL;
        if (im->param_count > 0) {
            param_types = xr_malloc(sizeof(XrType *) * im->param_count);
            if (!param_types)
                continue;
            for (int j = 0; j < im->param_count; j++) {
                param_types[j] =
                    im->param_types && im->param_types[j]
                        ? xr_tref_resolve_in_analyzer(ctx->analyzer, im->param_types[j])
                        : xr_type_new_unknown(NULL);
            }
        }
        XrType *ret_type = im->return_type
                               ? xr_tref_resolve_in_analyzer(ctx->analyzer, im->return_type)
                               : xr_type_new_unit(NULL);
        mlinks->type = xr_type_new_function(ctx->analyzer->isolate, param_types, im->param_count,
                                            ret_type, false);
        if (param_types)
            xr_free(param_types);
        xa_class_info_add_method(info, msym);
    }

    for (int i = 0; i < iface->property_count; i++) {
        AstNode *p = iface->properties ? iface->properties[i] : NULL;
        if (!p || p->type != AST_INTERFACE_PROPERTY)
            continue;
        InterfacePropertyNode *ip = &p->as.interface_property;
        if (!ip->name)
            continue;
        XaSymbol *psym = xa_symbol_new(ip->name, XA_SYM_PROPERTY);
        psym->location.line = p->line;
        XaSymbolLinks *plinks = xa_analyzer_get_links(ctx->analyzer, psym);
        plinks->type = ip->prop_type ? xr_tref_resolve_in_analyzer(ctx->analyzer, ip->prop_type)
                                     : xr_type_new_unknown(NULL);
        xa_class_info_add_field(info, psym);
    }
}

// Verify that `cls_info` provides every method/property required by every
// user-defined interface listed in info->interface_types. Built-in interface
// conformance (Iterable / Comparable / ...) is checked by
// xr_type_satisfies_constraint and stays outside this loop.
static void xa_check_interface_conformance(XaInferContext *ctx, AstNode *cls_node,
                                           XrClassInfo *cls_info) {
    if (!cls_info || cls_info->interface_count == 0 || !cls_info->interface_types)
        return;

    for (int i = 0; i < cls_info->interface_count; i++) {
        XrType *iface_type = cls_info->interface_types[i];
        if (!iface_type)
            continue;

        const char *iface_name = iface_type->instance.class_name;
        if (!iface_name)
            continue;

        // Built-in interfaces have no XrClassInfo* attached. Hashable is the
        // one builtin with a user-visible structural contract.
        if (strcmp(iface_name, "Hashable") == 0) {
            xa_validate_hashable_contract_for_class(ctx, cls_node, cls_info);
            continue;
        }
        if (xa_is_builtin_interface_name(iface_name))
            continue;

        XaSymbol *iface_sym = xa_scope_lookup(ctx->analyzer->current_scope, iface_name);
        if (!iface_sym || iface_sym->kind != XA_SYM_CLASS)
            continue;
        XaSymbolLinks *iface_links = xa_analyzer_get_links(ctx->analyzer, iface_sym);
        if (!iface_links || !iface_links->class_info)
            continue;
        XrClassInfo *iface_info = iface_links->class_info;
        // Only interfaces — never classes — should drive this audit; a real
        // class will not appear in an `implements` clause once the parser is
        // happy, but guard against it anyway by skipping types that look
        // like ordinary classes (links->type kind == XR_KIND_CLASS).
        if (iface_links->type && iface_links->type->kind != XR_KIND_INTERFACE)
            continue;

        // Required methods
        for (int j = 0; j < iface_info->method_count; j++) {
            XaSymbol *required = iface_info->methods[j];
            if (!required || !required->name)
                continue;
            XaSymbol *found = xa_class_info_lookup_member(cls_info, required->name);
            if (!found || found->kind != XA_SYM_METHOD) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Class '%s' does not implement method '%s' required by interface '%s'",
                         cls_info->name ? cls_info->name : "?", required->name, iface_name);
                XrLocation loc = {.file = ctx->file_path, .line = cls_node->line};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, msg, &loc);
            }
        }

        // Required properties — accept either a plain field/property or an
        // accessor pair. Computed properties on the class side are stored as
        // methods named "get:<prop>" / "set:<prop>" (see xparse_oop), so look
        // up both shapes before reporting a missing member.
        for (int j = 0; j < iface_info->field_count; j++) {
            XaSymbol *required = iface_info->fields[j];
            if (!required || !required->name)
                continue;
            XaSymbol *found = xa_class_info_lookup_member(cls_info, required->name);
            if (!found) {
                char getter_name[128];
                snprintf(getter_name, sizeof(getter_name), "get:%s", required->name);
                found = xa_class_info_lookup_member(cls_info, getter_name);
            }
            if (!found) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Class '%s' does not provide property '%s' required by interface '%s'",
                         cls_info->name ? cls_info->name : "?", required->name, iface_name);
                XrLocation loc = {.file = ctx->file_path, .line = cls_node->line};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED, msg, &loc);
            }
        }
    }
}

void xa_visit_collect_class(XaInferContext *ctx, AstNode *node) {
    if (!node)
        return;

    ClassDeclNode *cls = &node->as.class_decl;

    // @native class is reserved for builtin type declarations embedded at
    // compile time.  User code cannot bind C implementations, so reject early.
    if (cls->is_native) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line};
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
                                   "'@native class' cannot be used in user code — "
                                   "it is reserved for builtin type declarations",
                                   &loc);
        return;
    }

    // Create class symbol
    XaSymbol *sym = xa_symbol_new(cls->name, XA_SYM_CLASS);
    sym->location.line = node->line;

    xa_visit_add_symbol_checked(ctx, sym, 0);

    /* Write back resolved symbol ID for Xi lowering (shared var key). */
    cls->symbol_id = sym->id;

    // Create class info
    XrClassInfo *info = xa_class_info_new(cls->name);
    if (cls->super_name) {
        info->base_name = xr_strdup(cls->super_name);
    }

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    links->class_info = info;
    links->type = xr_type_new_class(ctx->analyzer->isolate, cls->name);
    links->type->instance.class_ref = info;
    if (node->type == AST_STRUCT_DECL) {
        links->type->is_value_type = true;
    }

    // Resolve every entry in the 'implements' clause to a runtime XrType
    // so constraint checks and conformance lookups can compare type
    // arguments structurally instead of falling back to bare-name matches.
    if (cls->interface_count > 0 && cls->interfaces) {
        info->interface_types = xr_malloc(sizeof(XrType *) * cls->interface_count);
        if (!info->interface_types)
            goto skip_interfaces;
        info->interface_count = cls->interface_count;
        for (int i = 0; i < cls->interface_count; i++) {
            info->interface_types[i] =
                xr_tref_resolve_in_analyzer(ctx->analyzer, cls->interfaces[i]);
        }
    }

skip_interfaces:

    // Store generic type parameters and intersection-style constraint lists.
    if (cls->type_param_count > 0 && cls->type_params) {
        int n = cls->type_param_count;
        const char **type_param_names = xr_malloc(sizeof(const char *) * n);
        XrType ***constraint_lists = xr_malloc(sizeof(XrType **) * n);
        int *constraint_counts = xr_malloc(sizeof(int) * n);

        if (type_param_names && constraint_lists && constraint_counts) {
            for (int i = 0; i < n; i++) {
                XrGenericParam *gp = cls->type_params[i];
                type_param_names[i] = gp->name;

                int cn = gp->constraint_count;
                if (cn > 0 && gp->constraints) {
                    XrType **resolved = xr_malloc(sizeof(XrType *) * cn);
                    for (int j = 0; j < cn; j++) {
                        // Use analyzer-aware resolver so class-bounded constraints
                        // (e.g. <T: Animal>) keep their inheritance chain.
                        resolved[j] =
                            gp->constraints[j]
                                ? xr_tref_resolve_in_analyzer(ctx->analyzer, gp->constraints[j])
                                : NULL;
                    }
                    constraint_lists[i] = resolved;
                    constraint_counts[i] = cn;
                } else {
                    constraint_lists[i] = NULL;
                    constraint_counts[i] = 0;
                }
            }

            xa_symbol_links_set_type_params(links, type_param_names, constraint_lists,
                                            constraint_counts, n);

            for (int i = 0; i < n; i++) {
                if (constraint_lists[i])
                    xr_free(constraint_lists[i]);
            }
        }

        xr_free(type_param_names);
        xr_free(constraint_lists);
        xr_free(constraint_counts);
    }

    // Enter class scope
    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_CLASS, node);
    ctx->analyzer->current_scope->class_symbol = sym;

    // Collect fields
    for (int i = 0; i < cls->field_count; i++) {
        AstNode *field = cls->fields[i];
        if (field && field->type == AST_FIELD_DECL) {
            FieldDeclNode *fd = &field->as.field_decl;
            XaSymbol *field_sym = xa_symbol_new(fd->name, XA_SYM_PROPERTY);
            field_sym->location.line = field->line;
            field_sym->is_static = fd->is_static;
            field_sym->is_private = fd->is_private;
            field_sym->is_protected = fd->is_protected;
            field_sym->is_const = fd->is_const;
            xa_visit_add_symbol_checked(ctx, field_sym, 0);

            XaSymbolLinks *field_links = xa_analyzer_get_links(ctx->analyzer, field_sym);

            // Try explicit type annotation first
            if (fd->field_type) {
                field_links->type = fd->field_type
                                        ? xr_tref_resolve_in_analyzer(ctx->analyzer, fd->field_type)
                                        : xr_type_new_unknown(NULL);
            } else if (fd->initializer) {
                // Infer type from initializer
                field_links->type = xa_visit_infer(ctx, fd->initializer);
            } else {
                field_links->type = xr_type_new_unknown(NULL);
                // Warn: class field missing type annotation and initializer
                char msg[256];
                snprintf(
                    msg, sizeof(msg),
                    "Field '%s' is missing type annotation (and has no initializer to infer from)",
                    fd->name ? fd->name : "?");
                XrLocation loc = {.file = ctx->file_path, .line = node->line};
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
            }
            xa_class_info_add_field(info, field_sym);
        }
    }

    // Check struct-only value-type constraints before layout construction.
    bool struct_field_types_valid = true;
    if (node->type == AST_STRUCT_DECL) {
        for (int i = 0; i < info->field_count; i++) {
            XaSymbol *fs = info->fields[i];
            if (!fs)
                continue;
            XaSymbolLinks *fl = xa_analyzer_get_links(ctx->analyzer, fs);
            if (!fl || !fl->type)
                continue;
            XrType *ft = fl->type;

            if (ft->kind == XR_KIND_ARRAY || ft->kind == XR_KIND_MAP || ft->kind == XR_KIND_SET) {
                XrLocation loc = {.file = ctx->file_path, .line = fs->location.line};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Struct '%s' field '%s' cannot use a dynamic container type; "
                         "use a class field or a fixed array [N]T",
                         cls->name ? cls->name : "?", fs->name ? fs->name : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                struct_field_types_valid = false;
                continue;
            }

            if (!cls->name)
                continue;
            // Field referencing the same struct → infinite size
            const char *type_name = NULL;
            if ((ft->kind == XR_KIND_CLASS || ft->kind == XR_KIND_INSTANCE) &&
                ft->instance.class_name) {
                type_name = ft->instance.class_name;
            }
            if (type_name && strcmp(type_name, cls->name) == 0) {
                XrLocation loc = {.file = ctx->file_path, .line = node->line};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Struct '%s' cannot have a field of its own type — "
                         "this creates infinite size. Use a class instead for recursive data",
                         cls->name);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                struct_field_types_valid = false;
            }
        }
    }

    // Compute struct layout (VALUE_TYPE only, skip generic templates)
    int struct_type_param_count =
        (node->type == AST_STRUCT_DECL) ? node->as.struct_decl.type_param_count : 0;
    if (node->type == AST_STRUCT_DECL && info->field_count > 0 && struct_type_param_count == 0 &&
        struct_field_types_valid) {
        XrStructLayout *layout = xr_calloc(1, sizeof(XrStructLayout));
        if (!layout)
            goto skip_layout;
        layout->field_count = (uint16_t) info->field_count;
        if (xa_class_attr_has(cls, ATTR_REPR_PACKED)) {
            layout->repr = XR_STRUCT_REPR_PACKED;
        } else if (xa_class_attr_has(cls, ATTR_REPR_C)) {
            layout->repr = XR_STRUCT_REPR_C;
        }
        layout->explicit_align = xa_class_attr_align(cls);
        bool layout_valid = true;
        if (layout->explicit_align != 0 &&
            (layout->explicit_align & (uint8_t) (layout->explicit_align - 1)) != 0) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line};
            char msg[256];
            snprintf(msg, sizeof(msg), "Struct '%s' @align value must be a power of two",
                     cls->name ? cls->name : "?");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            layout_valid = false;
        }
        /* Populate field_names parallel to fields[] for codegen/diagnostics */
        layout->field_names = xr_calloc((size_t) info->field_count, sizeof(const char *));
        if (layout->field_names) {
            for (int i = 0; i < info->field_count; i++)
                layout->field_names[i] = info->fields[i] ? info->fields[i]->name : NULL;
        }

        for (int i = 0; i < info->field_count && i < XR_MAX_STRUCT_FIELDS; i++) {
            if (!layout_valid)
                break;
            XaSymbol *fs = info->fields[i];
            if (!fs) {
                layout_valid = false;
                break;
            }
            XaSymbolLinks *fl = xa_analyzer_get_links(ctx->analyzer, fs);
            XrType *ft = (fl && fl->type) ? fl->type : NULL;

            if (!ft || ft->kind == XR_KIND_UNKNOWN) {
                // Phase 1: struct fields must have explicit type annotations
                XrLocation loc = {.file = ctx->file_path, .line = node->line};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Struct '%s' field '%s' must have an explicit type annotation "
                         "(int, float, bool, string, fixed array, or struct)",
                         cls->name ? cls->name : "?", fs->name ? fs->name : "?");
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                layout_valid = false;
                break;
            }

            int native = xr_type_kind_to_native(ft->kind, ft->native_width);
            if (native < 0) {
                // Fixed-size array field: [N]T
                if (ft->kind == XR_KIND_FIXED_ARRAY && ft->fixed_array.element_type) {
                    XrType *elem = ft->fixed_array.element_type;
                    int elem_native = xr_type_kind_to_native(elem->kind, elem->native_width);
                    if (elem_native >= 0 && ft->fixed_array.length > 0) {
                        uint32_t field_bytes = (uint32_t) ft->fixed_array.length *
                                               xr_native_type_size((uint8_t) elem_native);
                        if (field_bytes > UINT16_MAX) {
                            XrLocation loc = {.file = ctx->file_path, .line = node->line};
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "Fixed array field '%s' in struct '%s' exceeds maximum size "
                                     "(%u bytes > 65535). For larger collections, use a class with "
                                     "Array<T>.",
                                     fs->name ? fs->name : "?", cls->name ? cls->name : "?",
                                     (unsigned) field_bytes);
                            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                            layout_valid = false;
                            break;
                        }
                        layout->fields[i].native_type = XR_NATIVE_ARRAY;
                        layout->fields[i].elem_native_type = (uint8_t) elem_native;
                        layout->fields[i].elem_count = (uint16_t) ft->fixed_array.length;
                    } else {
                        XrLocation loc = {.file = ctx->file_path, .line = node->line};
                        char msg[256];
                        snprintf(
                            msg, sizeof(msg),
                            "Struct '%s' field '%s': fixed array element must be a primitive type",
                            cls->name ? cls->name : "?", fs->name ? fs->name : "?");
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                        layout_valid = false;
                        break;
                    }
                    continue;
                }
                // Check if field type is a nested struct with known layout
                const char *field_class_name = NULL;
                if ((ft->kind == XR_KIND_CLASS || ft->kind == XR_KIND_INSTANCE) &&
                    ft->instance.class_name) {
                    field_class_name = ft->instance.class_name;
                }
                XrStructLayout *sub_layout = NULL;
                if (field_class_name) {
                    XaSymbol *sub_sym = xa_analyzer_lookup(ctx->analyzer, field_class_name);
                    if (sub_sym) {
                        XaSymbolLinks *sub_links = xa_analyzer_get_links(ctx->analyzer, sub_sym);
                        if (sub_links && sub_links->class_info &&
                            sub_links->class_info->struct_layout) {
                            sub_layout = sub_links->class_info->struct_layout;
                        }
                    }
                }
                if (sub_layout) {
                    if (xr_struct_layout_is_headerless(layout) &&
                        !xr_struct_layout_is_headerless(sub_layout)) {
                        XrLocation loc = {.file = ctx->file_path, .line = node->line};
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "@repr(C) struct '%s' field '%s' must use a @repr(C) or "
                                 "@repr(packed) nested struct",
                                 cls->name ? cls->name : "?", fs->name ? fs->name : "?");
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                        layout_valid = false;
                        break;
                    }
                    layout->fields[i].native_type = XR_NATIVE_STRUCT;
                    layout->fields[i].size = (uint16_t) xr_struct_layout_storage_size(sub_layout);
                    layout->fields[i].sub_layout_id = sub_layout->layout_id;
                    layout->fields[i].sub_layout = sub_layout;
                } else {
                    XrLocation loc = {.file = ctx->file_path, .line = node->line};
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Struct '%s' field '%s' has unsupported type — "
                             "only int, float, bool, string, fixed arrays and other structs are "
                             "supported",
                             cls->name ? cls->name : "?", fs->name ? fs->name : "?");
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                    layout_valid = false;
                    break;
                }
                continue;
            }

            layout->fields[i].native_type = (uint8_t) native;
        }

        if (layout_valid && info->field_count <= XR_MAX_STRUCT_FIELDS) {
            xr_struct_layout_compute(layout);
            if (layout->total_size > UINT16_MAX) {
                XrLocation loc = {.file = ctx->file_path, .line = node->line};
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "Struct '%s' total size exceeds maximum (%u bytes > 65535). "
                         "For larger data, use a class with Array<T> fields.",
                         cls->name ? cls->name : "?", (unsigned) layout->total_size);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                xr_free(layout->field_names);
                xr_free(layout);
            } else {
                info->struct_layout = layout;
            }
        } else {
            xr_free(layout->field_names);
            xr_free(layout);
        }
    }
skip_layout:

    // Collect methods
    for (int i = 0; i < cls->method_count; i++) {
        AstNode *method = cls->methods[i];
        if (method && method->type == AST_METHOD_DECL) {
            MethodDeclNode *md = &method->as.method_decl;
            XaSymbol *method_sym = xa_symbol_new(md->name, XA_SYM_METHOD);
            method_sym->location.line = method->line;
            method_sym->is_static = md->is_static;
            method_sym->is_private = md->is_private;
            method_sym->is_protected = md->is_protected;
            method_sym->is_override = md->is_override;
            method_sym->mutates_receiver =
                !md->is_static && xa_method_body_mutates_receiver(md->body, info);
            xa_visit_add_symbol_checked(ctx, method_sym, 0);

            // Build method type
            XrType **param_types = NULL;
            const char **param_names = NULL;
            if (md->param_count > 0) {
                param_types = xr_malloc(sizeof(XrType *) * md->param_count);
                param_names = xr_malloc(sizeof(char *) * md->param_count);
                if (!param_types || !param_names) {
                    xr_free(param_types);
                    xr_free(param_names);
                    param_types = NULL;
                    param_names = NULL;
                }
                for (int j = 0; param_types && j < md->param_count; j++) {
                    param_types[j] =
                        (md->param_types && md->param_types[j])
                            ? xr_tref_resolve(ctx->analyzer->isolate, md->param_types[j])
                            : xr_type_new_unknown(NULL);
                    param_names[j] = md->parameters ? md->parameters[j] : NULL;

                    // Warn: method parameter missing type annotation (skip constructor)
                    if (!(md->param_types && md->param_types[j]) && !md->is_constructor) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "Parameter '%s' of method '%s' is missing type annotation",
                                 md->parameters ? md->parameters[j] : "?",
                                 md->name ? md->name : "?");
                        XrLocation loc = {.file = ctx->file_path, .line = method->line};
                        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                                   XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
                    }
                }
            }

            // Omitted return type defaults to void; error if body has 'return <expr>'
            // Skip getter/setter (set:xxx, get:xxx) - return types are implicit
            bool is_accessor = md->name && (strncmp(md->name, "set:", 4) == 0 ||
                                            strncmp(md->name, "get:", 4) == 0);
            XrType *ret_type =
                md->return_type ? xr_tref_resolve(ctx->analyzer->isolate, md->return_type) : NULL;
            if (!ret_type && is_accessor && md->body) {
                ret_type = xa_infer_function_return_type(ctx, md->body);
            }
            if (!ret_type) {
                ret_type = xr_type_new_unit(NULL);
            }
            if (!md->return_type && !md->is_constructor && !is_accessor && md->body) {
                if (xa_body_has_return_expr(md->body)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "Method '%s' returns a value but has no return type annotation",
                             md->name ? md->name : "?");
                    XrLocation loc = {.file = ctx->file_path, .line = method->line};
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_MISSING_TYPE, msg, &loc);
                }
            }

            // Resolve CLASS("T") → TYPE_PARAM("T") for generic methods
            if (md->type_param_count > 0 && md->type_param_names) {
                for (int j = 0; j < md->param_count; j++) {
                    param_types[j] = resolve_class_to_type_param(
                        NULL, param_types[j], (const char **) md->type_param_names,
                        md->type_param_count);
                }
                ret_type = resolve_class_to_type_param(
                    NULL, ret_type, (const char **) md->type_param_names, md->type_param_count);
            }

            XrType *method_type = xr_type_new_function(ctx->analyzer->isolate, param_types,
                                                       md->param_count, ret_type, false);

            // Propagate in/ref passing modes to the method type
            if (method_type && md->param_passing_modes) {
                bool has_modes = false;
                for (int j = 0; j < md->param_count && !has_modes; j++) {
                    if (md->param_passing_modes[j] != XR_PARAM_VALUE)
                        has_modes = true;
                }
                if (has_modes) {
                    uint8_t *modes = xr_calloc(md->param_count, sizeof(uint8_t));
                    if (modes) {
                        for (int j = 0; j < md->param_count; j++)
                            modes[j] = md->param_passing_modes[j];
                        method_type->function.param_passing_modes = modes;
                    }
                }
            }

            XaSymbolLinks *method_links = xa_analyzer_get_links(ctx->analyzer, method_sym);
            method_links->type = method_type;

            // Store parameter info for LSP
            xa_symbol_links_set_function_sig(method_links, param_types, param_names,
                                             md->param_count, ret_type);
            xa_symbol_links_set_param_escape_summary(ctx, method_links, param_types, param_names,
                                                     md->param_count, ret_type, md->body, info);
            // Record method/constructor default expressions for caller-side
            // default filling (methods store defaults in md->default_values).
            if (md->param_count > 0)
                xa_symbol_links_set_param_defaults(method_links, md->default_values,
                                                   md->param_count);

            // Store generic type parameters for the method.  Method-level
            // constraints aren't tracked in the method-decl AST yet, so the
            // intersection lists are empty for now.
            if (md->type_param_count > 0 && md->type_param_names) {
                int mc = md->type_param_count;
                const char **type_param_names = xr_malloc(sizeof(const char *) * mc);

                for (int j = 0; j < mc; j++) {
                    type_param_names[j] = md->type_param_names[j];
                }

                xa_symbol_links_set_type_params(method_links, type_param_names,
                                                /* constraint_lists  */ NULL,
                                                /* constraint_counts */ NULL, mc);

                xr_free(type_param_names);
            }

            XrLocation sig_loc = {
                .file = ctx->file_path, .line = method->line, .column = method->column};
            for (int j = 0; j < md->param_count; j++) {
                xa_validate_hashable_key_type(
                    ctx,
                    method_type->function.param_types ? method_type->function.param_types[j] : NULL,
                    method_links, "method parameter type", &sig_loc);
            }
            xa_validate_hashable_key_type(ctx, method_type->function.return_type, method_links,
                                          "method return type", &sig_loc);

            xa_class_info_add_method(info, method_sym);

            // Record constructor info in class_info and validate super() rules
            if (md->is_constructor) {
                info->has_constructor = true;
                info->constructor_param_count = md->param_count;
                // Count required params (those without default values)
                int required = 0;
                for (int j = 0; j < md->param_count; j++) {
                    if (!md->default_values || !md->default_values[j])
                        required++;
                }
                info->constructor_required_params = required;
                validate_constructor_super_call(ctx, cls, md, method);
            }

            if (param_types)
                xr_free(param_types);
            if (param_names)
                xr_free(param_names);
        }
    }

    for (int i = 0; i < info->field_count; i++) {
        XaSymbol *field = info->fields[i];
        XaSymbolLinks *field_links = field ? xa_analyzer_get_links(ctx->analyzer, field) : NULL;
        XrLocation loc = {
            .file = ctx->file_path, .line = field ? field->location.line : node->line, .column = 0};
        xa_validate_hashable_key_type(ctx, field_links ? field_links->type : NULL, links,
                                      "field type", &loc);
    }

    xa_class_propagate_receiver_mutations(info, cls);

    validate_class_field_default_initialization(ctx, node, cls, info);

    // Enter each method scope and add parameters + visit body for nested declarations.
    // This creates the function scopes that Pass 2 will reuse via ast_node matching,
    // ensuring method parameters are visible during type inference.
    for (int i = 0; i < cls->method_count; i++) {
        AstNode *method = cls->methods[i];
        if (!method || method->type != AST_METHOD_DECL)
            continue;
        MethodDeclNode *md = &method->as.method_decl;
        if (!md->body)
            continue;

        xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, method);

        // Look up method symbol to get resolved param types
        XaSymbol *msym = xa_scope_lookup_local(ctx->analyzer->current_scope->parent, md->name);
        ctx->analyzer->current_scope->function_symbol = msym;
        XaSymbolLinks *mlinks = msym ? xa_analyzer_get_links(ctx->analyzer, msym) : NULL;

        for (int j = 0; j < md->param_count; j++) {
            const char *pname = md->parameters ? md->parameters[j] : NULL;
            if (!pname)
                continue;

            XaSymbol *param = xa_symbol_new(pname, XA_SYM_PARAMETER);
            param->location.line = method->line;
            if (md->param_passing_modes) {
                param->passing_mode = md->param_passing_modes[j];
            }
            xa_visit_add_symbol_checked(ctx, param, 0);

            XaSymbolLinks *plinks = xa_analyzer_get_links(ctx->analyzer, param);
            if (plinks) {
                plinks->type = (mlinks && mlinks->param_types && j < mlinks->param_count)
                                   ? mlinks->param_types[j]
                                   : xr_type_new_unknown(NULL);
                plinks->is_definitely_assigned = true;
            }
        }

        // Visit body for nested declarations (variables, nested functions, etc.)
        if (md->body)
            xa_visit_collect(ctx, md->body);

        if (mlinks) {
            xa_symbol_links_set_param_escape_summary(ctx, mlinks, mlinks->param_types,
                                                     mlinks->param_names, mlinks->param_count,
                                                     mlinks->return_type, md->body, info);
        }

        xa_analyzer_exit_scope(ctx->analyzer);
    }

    for (int pass = 0; pass < cls->method_count + 1; pass++) {
        bool changed = false;
        for (int i = 0; i < cls->method_count; i++) {
            AstNode *method = cls->methods[i];
            if (!method || method->type != AST_METHOD_DECL)
                continue;
            MethodDeclNode *md = &method->as.method_decl;
            if (!md->body)
                continue;
            XaSymbol *method_sym = xa_class_info_lookup_member(info, md->name);
            XaSymbolLinks *method_links =
                method_sym ? xa_analyzer_get_links(ctx->analyzer, method_sym) : NULL;
            if (!method_links)
                continue;
            if (xa_symbol_links_set_param_escape_summary(
                    ctx, method_links, method_links->param_types, method_links->param_names,
                    method_links->param_count, method_links->return_type, md->body, info)) {
                changed = true;
            }
        }
        if (!changed)
            break;
    }

    xa_analyzer_exit_scope(ctx->analyzer);

    // After all fields/methods are collected, enforce that every user-defined
    // interface listed in `implements` is structurally satisfied by this
    // class. Built-in interfaces (Iterable, Comparable, ...) are validated
    // separately when used as generic constraints.
    xa_check_interface_conformance(ctx, node, info);
}

void xa_visit_collect_var_decl(XaInferContext *ctx, AstNode *node) {
    XR_DCHECK(ctx != NULL, "visit_collect_var_decl: NULL ctx");
    if (!node)
        return;

    VarDeclNode *var = &node->as.var_decl;

    XaSymbol *sym = xa_symbol_new(var->name, XA_SYM_VARIABLE);
    sym->location.line = node->line;
    sym->is_const = (node->type == AST_CONST_DECL);
    sym->is_shared = (var->storage_mode == 1);  // XR_STORAGE_SHARED

    xa_visit_add_symbol_checked(ctx, sym, 0);

    /* Write back unique symbol ID so Xi lowering can use it as Braun SSA key
     * instead of name-based lookup (eliminates scope ambiguity). */
    var->symbol_id = sym->id;

    // Type will be inferred in pass 2
    // Keep NULL when no annotation (distinguishes "no annotation" from "annotated as any")
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    links->declared_type = var->type_annotation
                               ? xr_tref_resolve_in_analyzer(ctx->analyzer, var->type_annotation)
                               : NULL;
    if (links->declared_type) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        xa_validate_hashable_key_type(ctx, links->declared_type, NULL, "type annotation", &loc);
    }

    // Mark const types as immutable for concurrency safety
    if (sym->is_shared && sym->is_const && links->declared_type) {
        links->declared_type->is_const = true;
    }

    // Recurse into go { block } initializers to collect nested scopes.
    // go { ... } is parsed as go(anonymous_function_expr), whose body
    // needs Pass 1 scope collection for for-in variables, multi-value decls, etc.
    // Must mirror Pass 2 structure: function scope → block scope → statements
    // (xa_visit_function_expr enters function scope, then xa_visit_block_stmt
    //  enters block scope for the body).
    AstNode *init = var->initializer;
    if (init && init->type == AST_GO_EXPR) {
        AstNode *go_fn = init->as.go_expr.expr;
        if (go_fn && go_fn->type == AST_FUNCTION_EXPR) {
            FunctionDeclNode *fn = &go_fn->as.function_expr;
            xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_FUNCTION, go_fn);
            if (fn->body && fn->body->type == AST_BLOCK) {
                xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, fn->body);
                xa_visit_collect_statements_with_hoisting(ctx, fn->body->as.block.statements,
                                                          fn->body->as.block.count);
                xa_analyzer_exit_scope(ctx->analyzer);
            } else if (fn->body) {
                xa_visit_collect(ctx, fn->body);
            }
            xa_analyzer_exit_scope(ctx->analyzer);
        }
    }
}

/* ============================================================================
 * Pass 1.5: Link Class Inheritance
 * ============================================================================
 * After Pass 1 collects all class symbols, this pass links inheritance chains
 * by resolving base class names to actual XrClassInfo pointers.
 * ========================================================================== */

static bool method_types_equal_for_override(XaSymbol *method, XaSymbol *parent_method) {
    if (!method || !parent_method)
        return false;
    XrType *method_type = method->links.type;
    XrType *parent_type = parent_method->links.type;
    if (!method_type || !parent_type)
        return false;
    if (method_type->kind != XR_KIND_FUNCTION || parent_type->kind != XR_KIND_FUNCTION)
        return false;
    return xr_type_equals(method_type, parent_type);
}

static XaSymbol *find_parent_override_target(XrClassInfo *info, XaSymbol *method,
                                             bool *out_name_match) {
    if (out_name_match)
        *out_name_match = false;
    if (!info || !method || !method->name)
        return NULL;

    for (XrClassInfo *base = info->base; base; base = base->base) {
        for (int i = 0; i < base->method_count; i++) {
            XaSymbol *candidate = base->methods[i];
            if (!candidate || !candidate->name || candidate->is_static || candidate->is_private)
                continue;
            if (strcmp(candidate->name, method->name) != 0)
                continue;
            if (out_name_match)
                *out_name_match = true;
            if (method_types_equal_for_override(method, candidate))
                return candidate;
        }
    }
    return NULL;
}

static XrClassInfo *class_info_from_type(XrType *type) {
    if (!type)
        return NULL;
    if (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE)
        return NULL;
    return type->instance.class_ref;
}

static XrClassInfo *resolve_base_class_info(XaAnalyzer *analyzer, const char *base_name,
                                            XrType **out_base_type) {
    if (out_base_type)
        *out_base_type = NULL;
    if (!analyzer || !analyzer->global_scope || !base_name)
        return NULL;

    XaSymbol *base_sym = xa_scope_lookup(analyzer->global_scope, base_name);
    if (!base_sym)
        return NULL;

    XaSymbolLinks *base_links = xa_analyzer_get_links(analyzer, base_sym);
    if (!base_links)
        return NULL;

    if (out_base_type)
        *out_base_type = base_links->type;

    if (base_sym->kind == XA_SYM_CLASS && base_links->class_info)
        return base_links->class_info;

    return class_info_from_type(base_links->type);
}

static void report_override_mismatch(XaAnalyzer *analyzer, XrClassInfo *info, XaSymbol *method,
                                     const char *reason) {
    if (!analyzer || !method)
        return;
    char msg[384];
    snprintf(msg, sizeof(msg), "Method '%s.%s' is marked override but %s",
             info && info->name ? info->name : "?", method->name ? method->name : "?", reason);
    XrLocation loc = method->location;
    if (!loc.file)
        loc.file = analyzer->current_file;
    xa_analyzer_add_diagnostic(analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_OVERRIDE_MISMATCH, msg,
                               &loc);
}

static void validate_explicit_overrides(XaAnalyzer *analyzer, XrClassInfo *info) {
    if (!analyzer || !info)
        return;

    for (int i = 0; i < info->static_method_count; i++) {
        XaSymbol *method = info->static_methods[i];
        if (method && method->is_override) {
            report_override_mismatch(analyzer, info, method,
                                     "static methods cannot override parent instance methods");
        }
    }

    for (int i = 0; i < info->method_count; i++) {
        XaSymbol *method = info->methods[i];
        if (!method || !method->is_override)
            continue;
        if (strcmp(method->name, "constructor") == 0) {
            report_override_mismatch(analyzer, info, method, "constructors cannot be overridden");
            continue;
        }
        if (!info->base) {
            report_override_mismatch(analyzer, info, method, "the class has no parent class");
            continue;
        }
        bool name_match = false;
        XaSymbol *target = find_parent_override_target(info, method, &name_match);
        if (!target) {
            report_override_mismatch(analyzer, info, method,
                                     name_match ? "no parent method has the same signature"
                                                : "no matching parent method exists");
        }
    }
}

// Build virtual method table for a class (inherits base vtable + own methods)
static void build_class_vtable(XaAnalyzer *analyzer, XrClassInfo *info) {
    if (!info || info->vtable)
        return;  // already built

    // First build base vtable if needed
    if (info->base) {
        build_class_vtable(analyzer, info->base);
    }

    // Determine vtable size: base methods + new methods
    int base_size = info->base ? info->base->vtable_size : 0;
    int max_size = base_size + info->method_count;
    if (max_size == 0)
        return;

    XaMethodSlot *vtable = xr_calloc(max_size, sizeof(XaMethodSlot));
    int vt_count = 0;

    // Copy base vtable entries (inherit)
    if (info->base && info->base->vtable) {
        for (int i = 0; i < info->base->vtable_size; i++) {
            vtable[i] = info->base->vtable[i];
            vtable[i].is_final = true;  // assume final until proven otherwise
            vt_count++;
        }
    }

    // Process own methods: override existing or add new
    for (int m = 0; m < info->method_count; m++) {
        XaSymbol *method = info->methods[m];
        if (!method || !method->name)
            continue;

        // Check if this overrides a base method
        bool found = false;
        for (int v = 0; v < vt_count; v++) {
            if (vtable[v].name && strcmp(vtable[v].name, method->name) == 0) {
                // Override: mark base method as overridden
                if (info->base && info->base->vtable) {
                    for (int bv = 0; bv < info->base->vtable_size; bv++) {
                        if (info->base->vtable[bv].name &&
                            strcmp(info->base->vtable[bv].name, method->name) == 0) {
                            info->base->vtable[bv].is_overridden = true;
                            info->base->vtable[bv].is_final = false;
                            break;
                        }
                    }
                }
                // Update slot to point to overriding method
                vtable[v].symbol = method;
                vtable[v].is_overridden = false;
                vtable[v].is_final = true;
                found = true;
                break;
            }
        }

        if (!found) {
            // New method, add to vtable
            vtable[vt_count].name = method->name;
            vtable[vt_count].symbol = method;
            vtable[vt_count].is_overridden = false;
            vtable[vt_count].is_final = true;
            vtable[vt_count].vtable_index = vt_count;
            vt_count++;
        }
    }

    // Assign vtable indices
    for (int i = 0; i < vt_count; i++) {
        vtable[i].vtable_index = i;
    }

    info->vtable = vtable;
    info->vtable_size = vt_count;
}

void xa_link_class_inheritance(XaAnalyzer *analyzer) {
    if (!analyzer || !analyzer->global_scope)
        return;

    // Get all symbols from global scope
    int count = 0;
    XaSymbol **symbols = xa_scope_get_all_symbols(analyzer->global_scope, &count);
    if (!symbols)
        return;

    // Pass 1: Link all class inheritance chains
    for (int i = 0; i < count; i++) {
        XaSymbol *sym = symbols[i];
        if (!sym || sym->kind != XA_SYM_CLASS)
            continue;

        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (!links || !links->class_info)
            continue;

        XrClassInfo *info = links->class_info;
        if (!info->base_name)
            continue;

        XrType *base_type = NULL;
        XrClassInfo *base_info = resolve_base_class_info(analyzer, info->base_name, &base_type);
        if (base_info) {
            info->base = base_info;
            base_info->has_subclass = true;
            // Link XrType inheritance chain for xr_type_is_subclass_of().
            if (links->type && base_type) {
                links->type->instance.superclass = base_type;
            }
        } else {
            info->base = NULL;
        }
    }

    // Pass 2: Validate explicit override contracts now that parent links exist.
    for (int i = 0; i < count; i++) {
        XaSymbol *sym = symbols[i];
        if (!sym || sym->kind != XA_SYM_CLASS)
            continue;

        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (!links || !links->class_info)
            continue;

        validate_explicit_overrides(analyzer, links->class_info);
    }

    // Pass 3: Build virtual method tables (after all inheritance is linked)
    for (int i = 0; i < count; i++) {
        XaSymbol *sym = symbols[i];
        if (!sym || sym->kind != XA_SYM_CLASS)
            continue;

        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (!links || !links->class_info)
            continue;

        build_class_vtable(analyzer, links->class_info);
    }

    // Pass 4: Mark methods as non-final if class has subclass
    // (A method is only truly final if no subclass exists)
    for (int i = 0; i < count; i++) {
        XaSymbol *sym = symbols[i];
        if (!sym || sym->kind != XA_SYM_CLASS)
            continue;
        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (!links || !links->class_info)
            continue;
        XrClassInfo *info = links->class_info;
        if (!info->vtable)
            continue;

        // If class has no subclass, all its methods are definitively final
        // (is_final = true is already default)
        // If class has subclass, methods not overridden are still final
        // (handled above during vtable build)
    }

    xr_free(symbols);
}

/* ========== Cycle Candidate Detection ========== */

/* DFS states for Tarjan-style cycle detection in the class reference graph.
 * A class A references class B if any of A's instance fields has type B
 * (or a union containing B, or an array of B, etc.). If a strongly connected
 * component (SCC) of size > 1 exists, all its members are cycle candidates.
 * Self-referencing classes (A has a field of type A|null) are also candidates. */

#define CYC_UNVISITED 0
#define CYC_ON_STACK 1
#define CYC_DONE 2

/* Check if field type refers to a class declared in this module. */
static const char *extract_class_name_from_type(XrType *type) {
    if (!type)
        return NULL;
    switch (type->kind) {
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
            return type->instance.class_name;
        case XR_KIND_UNION:
            /* Check all union members. */
            for (int i = 0; i < type->union_type.member_count; i++) {
                const char *name = extract_class_name_from_type(type->union_type.members[i]);
                if (name)
                    return name;
            }
            return NULL;
        case XR_KIND_ARRAY:
            return extract_class_name_from_type(type->container.element_type);
        default:
            return NULL;
    }
}

/* Recursive DFS marking. Returns true if any node in the subtree is on-stack
 * (i.e., a cycle was found). */
static bool cycle_dfs(XaAnalyzer *analyzer, XaSymbol **class_syms, uint8_t *state, int idx,
                      int count, bool *is_candidate) {
    state[idx] = CYC_ON_STACK;
    bool found_cycle = false;

    XaSymbolLinks *links = xa_analyzer_get_links(analyzer, class_syms[idx]);
    if (!links || !links->class_info)
        goto done;

    XrClassInfo *info = links->class_info;

    /* Iterate instance fields, find references to other classes. */
    for (int f = 0; f < info->field_count; f++) {
        XaSymbol *field_sym = info->fields[f];
        if (!field_sym || field_sym->is_static)
            continue;
        XaSymbolLinks *fl = xa_analyzer_get_links(analyzer, field_sym);
        if (!fl || !fl->type)
            continue;

        const char *ref_name = extract_class_name_from_type(fl->type);
        if (!ref_name)
            continue;

        /* Self-reference: A has field of type A → cycle candidate. */
        if (info->name && strcmp(ref_name, info->name) == 0) {
            is_candidate[idx] = true;
            found_cycle = true;
            continue;
        }

        /* Find the referenced class in the symbol list. */
        for (int j = 0; j < count; j++) {
            if (j == idx)
                continue;
            if (!class_syms[j] || !class_syms[j]->name)
                continue;
            if (strcmp(class_syms[j]->name, ref_name) != 0)
                continue;

            if (state[j] == CYC_ON_STACK) {
                /* Back edge: cycle found. Mark both. */
                is_candidate[idx] = true;
                is_candidate[j] = true;
                found_cycle = true;
            } else if (state[j] == CYC_UNVISITED) {
                if (cycle_dfs(analyzer, class_syms, state, j, count, is_candidate)) {
                    is_candidate[idx] = true;
                    found_cycle = true;
                }
            } else if (is_candidate[j]) {
                /* j already processed and is a cycle candidate — if we
                 * reference it, we are also part of a potential cycle. */
                is_candidate[idx] = true;
                found_cycle = true;
            }
            break;
        }
    }

done:
    state[idx] = CYC_DONE;
    return found_cycle;
}

void xa_mark_cycle_candidates(XaAnalyzer *analyzer) {
    if (!analyzer || !analyzer->global_scope)
        return;

    int sym_count = 0;
    XaSymbol **all_syms = xa_scope_get_all_symbols(analyzer->global_scope, &sym_count);
    if (!all_syms || sym_count == 0)
        return;

    /* Collect only class symbols (skip structs — value types are copied). */
    int class_count = 0;
    XaSymbol **class_syms = xr_malloc(sizeof(XaSymbol *) * sym_count);
    if (!class_syms) {
        xr_free(all_syms);
        return;
    }

    for (int i = 0; i < sym_count; i++) {
        XaSymbol *sym = all_syms[i];
        if (!sym || sym->kind != XA_SYM_CLASS)
            continue;
        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
        if (!links || !links->class_info)
            continue;
        if (links->type && links->type->is_value_type)
            continue; /* Skip structs. */
        class_syms[class_count++] = sym;
    }

    if (class_count == 0) {
        xr_free(class_syms);
        xr_free(all_syms);
        return;
    }

    uint8_t *state = xr_calloc(class_count, sizeof(uint8_t));
    bool *is_candidate = xr_calloc(class_count, sizeof(bool));
    if (!state || !is_candidate) {
        xr_free(state);
        xr_free(is_candidate);
        xr_free(class_syms);
        xr_free(all_syms);
        return;
    }

    /* Run DFS from each unvisited class. */
    for (int i = 0; i < class_count; i++) {
        if (state[i] == CYC_UNVISITED)
            cycle_dfs(analyzer, class_syms, state, i, class_count, is_candidate);
    }

    /* Mark XrType for cycle candidates. The lowerer propagates this to
     * XiClassData, then the emitter sets XR_CLASS_CYCLE_CANDIDATE in the
     * class descriptor flags, which the runtime class builder propagates
     * to XrClass.flags — enabling the RC cycle collector at instance alloc. */
    for (int i = 0; i < class_count; i++) {
        if (!is_candidate[i])
            continue;
        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, class_syms[i]);
        if (links && links->type)
            links->type->is_cycle_candidate = true;
    }

    xr_free(state);
    xr_free(is_candidate);
    xr_free(class_syms);
    xr_free(all_syms);
}
