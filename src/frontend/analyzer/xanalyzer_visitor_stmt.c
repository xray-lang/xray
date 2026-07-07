/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_visitor_stmt.c - Pass 2 statement type inference visitors
 *
 * KEY CONCEPT:
 *   Type inference for statements: variable declarations, assignments,
 *   control flow, loops, class/enum declarations, and import handling.
 */

#include "xanalyzer_visitor_internal.h"
#include "xconsteval.h"
#include "xtype_ref_resolve.h"
#include "../../base/xchecks.h"
#include <stdint.h>

static bool xa_is_module_level_scope(const XaAnalyzer *analyzer) {
    return analyzer && analyzer->current_scope && analyzer->current_scope->kind == XA_SCOPE_GLOBAL;
}

static XrAttribute *xa_var_attr(const VarDeclNode *var, AttributeKind kind) {
    if (!var || !var->attributes)
        return NULL;
    for (int i = 0; i < var->attr_count; i++) {
        if (var->attributes[i] && var->attributes[i]->kind == kind)
            return var->attributes[i];
    }
    return NULL;
}

static bool xa_var_has_static_data_attr(const VarDeclNode *var) {
    return xa_var_attr(var, ATTR_SECTION) || xa_var_attr(var, ATTR_USED);
}

static bool xa_type_has_fixed_layout_data_object(const XrType *type) {
    return type && (type->kind == XR_KIND_CLASS || type->kind == XR_KIND_INSTANCE) &&
           type->instance.class_ref && type->instance.class_ref->struct_layout;
}

static bool xa_type_supports_const_static_data_object(const XrType *type) {
    if (!type)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_CHAR:
        case XR_KIND_STRING:
        case XR_KIND_NULL:
        case XR_KIND_FIXED_ARRAY:
        case XR_KIND_TUPLE:
            return true;
        case XR_KIND_CLASS:
        case XR_KIND_INSTANCE:
            return xa_type_has_fixed_layout_data_object(type);
        default:
            return false;
    }
}

static void xa_validate_const_static_data_attrs(XaInferContext *ctx, AstNode *node,
                                                VarDeclNode *var, XaSymbolLinks *links,
                                                XrType *var_type) {
    if (!ctx || !node || !var || !xa_var_has_static_data_attr(var))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    if (!xa_freestanding_profile_enabled(ctx->analyzer)) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
            "@section/@used const data is currently only supported in freestanding profile", &loc);
        return;
    }
    XrAttribute *section = xa_var_attr(var, ATTR_SECTION);
    if (section && (!section->str_arg || section->str_arg[0] == '\0')) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
                                   "@section requires a non-empty section name", &loc);
        return;
    }
    if (!xa_is_module_level_scope(ctx->analyzer) || node->type != AST_CONST_DECL) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
            "@section/@used can only annotate a module-level const data declaration", &loc);
        return;
    }
    if (!links || !links->has_ct_value) {
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
                                   "@section/@used const data requires a compile-time initializer",
                                   &loc);
        return;
    }
    if (!xa_type_supports_const_static_data_object(var_type)) {
        xa_analyzer_add_diagnostic(
            ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
            "@section/@used const data currently requires a scalar, string, fixed-array, tuple, "
            "struct, or union static object",
            &loc);
    }
}

static bool xa_freestanding_top_const_aggregate_value_allowed(const XrCtValue *value,
                                                              bool allow_string_array_elements);

static bool xa_freestanding_top_const_tuple_element_allowed(const XrCtValue *value) {
    if (!value)
        return false;
    switch (value->kind) {
        case XR_CT_INT:
        case XR_CT_FLOAT:
        case XR_CT_BOOL:
        case XR_CT_CHAR:
        case XR_CT_STRING:
            return true;
        case XR_CT_TUPLE:
            return xa_freestanding_top_const_aggregate_value_allowed(value, false);
        default:
            return false;
    }
}

static bool xa_freestanding_top_const_aggregate_scalar_allowed(const XrCtValue *value) {
    if (!value)
        return false;
    switch (value->kind) {
        case XR_CT_INT:
        case XR_CT_FLOAT:
        case XR_CT_BOOL:
        case XR_CT_CHAR:
            return true;
        case XR_CT_FIXED_ARRAY:
        case XR_CT_STRUCT_VALUE:
            return xa_freestanding_top_const_aggregate_value_allowed(value, false);
        default:
            return false;
    }
}

static bool xa_freestanding_top_const_fixed_array_element_allowed(const XrCtValue *value,
                                                                  bool allow_strings) {
    if (allow_strings && value && value->kind == XR_CT_STRING)
        return true;
    return xa_freestanding_top_const_aggregate_scalar_allowed(value);
}

static bool xa_freestanding_top_const_struct_field_allowed(const XrCtValue *value) {
    if (!value)
        return false;
    if (value->kind == XR_CT_STRING)
        return true;
    if (value->kind == XR_CT_FIXED_ARRAY)
        return xa_freestanding_top_const_aggregate_value_allowed(value, true);
    return xa_freestanding_top_const_aggregate_scalar_allowed(value);
}

static bool xa_freestanding_top_const_aggregate_value_allowed(const XrCtValue *value,
                                                              bool allow_string_array_elements) {
    if (!value)
        return false;
    switch (value->kind) {
        case XR_CT_FIXED_ARRAY: {
            const XrCtFixedArrayValue *array = &value->as.fixed_array_val;
            if (array->count <= 0 || !array->elements)
                return false;
            for (int i = 0; i < array->count; i++) {
                if (!xa_freestanding_top_const_fixed_array_element_allowed(
                        &array->elements[i], allow_string_array_elements))
                    return false;
            }
            return true;
        }
        case XR_CT_TUPLE: {
            const XrCtTupleValue *tuple = &value->as.tuple_val;
            if (tuple->count <= 0 || !tuple->elements)
                return false;
            for (int i = 0; i < tuple->count; i++) {
                if (!xa_freestanding_top_const_tuple_element_allowed(&tuple->elements[i]))
                    return false;
            }
            return true;
        }
        case XR_CT_STRUCT_VALUE: {
            const XrCtStructValue *st = &value->as.struct_val;
            if (st->field_count <= 0 || !st->field_values)
                return false;
            for (int i = 0; i < st->field_count; i++) {
                if (!xa_freestanding_top_const_struct_field_allowed(&st->field_values[i]))
                    return false;
            }
            return true;
        }
        default:
            return false;
    }
}

static bool xa_freestanding_top_const_ct_value_allowed(const XrCtValue *value) {
    if (!value)
        return false;
    switch (value->kind) {
        case XR_CT_INT:
        case XR_CT_FLOAT:
        case XR_CT_BOOL:
        case XR_CT_CHAR:
        case XR_CT_STRING:
        case XR_CT_NULL:
            return true;
        case XR_CT_FIXED_ARRAY:
            return xa_freestanding_top_const_aggregate_value_allowed(value, true);
        case XR_CT_TUPLE:
        case XR_CT_STRUCT_VALUE:
            return xa_freestanding_top_const_aggregate_value_allowed(value, false);
        default:
            return false;
    }
}

static int xa_analyzer_error_diagnostic_count(XaAnalyzer *analyzer) {
    int count = 0;
    if (!analyzer)
        return 0;
    for (XaDiagnostic *d = analyzer->diagnostics; d; d = d->next) {
        if (d->severity == XR_DIAG_SEV_ERROR)
            count++;
    }
    return count;
}

static bool xa_warning_already_reported(XaAnalyzer *analyzer, const XrLocation *loc,
                                        const char *message) {
    if (!analyzer || !loc || !message)
        return false;
    for (XaDiagnostic *d = analyzer->diagnostics; d; d = d->next) {
        if (d->severity != XR_DIAG_SEV_WARNING || d->code != XR_ERR_ANALYZE || !d->message ||
            strcmp(d->message, message) != 0)
            continue;
        if (d->location.line != loc->line || d->location.column != loc->column)
            continue;
        if (d->location.file == loc->file ||
            (d->location.file && loc->file && strcmp(d->location.file, loc->file) == 0))
            return true;
    }
    return false;
}

static bool xa_symbol_is_local_thread_handle(XaSymbol *sym, XaScope *current_scope) {
    if (!sym || sym->kind != XA_SYM_VARIABLE || !current_scope)
        return false;
    if (sym->scope != current_scope || sym->scope->kind == XA_SCOPE_GLOBAL)
        return false;
    if (sym->is_shared || sym->is_exported || sym->is_imported)
        return false;
    XaSymbolLinks *links = &sym->links;
    return links->type && xr_type_is_named_class(links->type, "Thread");
}

static bool xa_block_node_statements(AstNode *node, AstNode ***out_statements, int *out_count);

typedef struct XaThreadHandleLintAlias {
    XaSymbol *sym;
    struct XaThreadHandleLintAlias *next;
} XaThreadHandleLintAlias;

typedef struct XaThreadHandleLintState {
    XaSymbol *root;
    AstNode *decl_stmt;
    XaThreadHandleLintAlias *aliases;
    bool finalized;
    bool transferred;
    struct XaThreadHandleLintState *next;
} XaThreadHandleLintState;

typedef struct XaThreadHandleLintSnapshot {
    bool finalized;
    bool transferred;
} XaThreadHandleLintSnapshot;

static void xa_thread_lint_free_states(XaThreadHandleLintState *states) {
    while (states) {
        XaThreadHandleLintState *next = states->next;
        XaThreadHandleLintAlias *alias = states->aliases;
        while (alias) {
            XaThreadHandleLintAlias *alias_next = alias->next;
            xr_free(alias);
            alias = alias_next;
        }
        xr_free(states);
        states = next;
    }
}

static void xa_thread_lint_add_alias(XaThreadHandleLintState *state, XaSymbol *sym) {
    if (!state || !sym || sym->id == 0)
        return;
    for (XaThreadHandleLintAlias *a = state->aliases; a; a = a->next) {
        if (a->sym == sym || (a->sym && a->sym->id == sym->id))
            return;
    }
    XaThreadHandleLintAlias *alias = xr_calloc(1, sizeof(XaThreadHandleLintAlias));
    if (!alias)
        return;
    alias->sym = sym;
    alias->next = state->aliases;
    state->aliases = alias;
}

static XaThreadHandleLintState *xa_thread_lint_find_by_symbol_id(XaThreadHandleLintState *states,
                                                                 uint32_t symbol_id) {
    if (symbol_id == 0)
        return NULL;
    for (XaThreadHandleLintState *s = states; s; s = s->next) {
        for (XaThreadHandleLintAlias *a = s->aliases; a; a = a->next) {
            if (a->sym && a->sym->id == symbol_id)
                return s;
        }
    }
    return NULL;
}

static int xa_thread_lint_state_count(XaThreadHandleLintState *states) {
    int count = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next)
        count++;
    return count;
}

static void xa_thread_lint_snapshot_states(XaThreadHandleLintState *states,
                                           XaThreadHandleLintSnapshot *snapshots) {
    if (!snapshots)
        return;
    int i = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next, i++) {
        snapshots[i].finalized = s->finalized;
        snapshots[i].transferred = s->transferred;
    }
}

static void xa_thread_lint_restore_states(XaThreadHandleLintState *states,
                                          XaThreadHandleLintSnapshot *snapshots) {
    if (!snapshots)
        return;
    int i = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next, i++) {
        s->finalized = snapshots[i].finalized;
        s->transferred = snapshots[i].transferred;
    }
}

static bool xa_thread_lint_snapshot_closed(XaThreadHandleLintSnapshot *snapshot) {
    return snapshot && (snapshot->finalized || snapshot->transferred);
}

static AstNode *xa_thread_lint_unwrap_expr(AstNode *expr) {
    while (expr && (expr->type == AST_GROUPING || expr->type == AST_FORCE_UNWRAP))
        expr = expr->type == AST_GROUPING ? expr->as.grouping : expr->as.unary.operand;
    return expr;
}

static uint32_t xa_thread_lint_expr_symbol_id(AstNode *expr) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr || expr->type != AST_VARIABLE)
        return 0;
    return expr->as.variable.symbol_id;
}

static void xa_thread_lint_scan_expr(XaThreadHandleLintState *states, AstNode *expr,
                                     bool return_value, bool can_escape);
static void xa_thread_lint_scan_stmt(XaThreadHandleLintState *states, AstNode *stmt,
                                     bool can_escape);
static void xa_thread_lint_scan_match_expr(XaThreadHandleLintState *states, AstNode *expr,
                                           bool can_escape);

static void xa_thread_lint_scan_expr_array(XaThreadHandleLintState *states, AstNode **nodes,
                                           int count, bool return_value, bool can_escape) {
    if (!nodes || count <= 0)
        return;
    for (int i = 0; i < count; i++)
        xa_thread_lint_scan_expr(states, nodes[i], return_value, can_escape);
}

static bool xa_thread_lint_scan_join_or_detach_call(XaThreadHandleLintState *states, AstNode *expr,
                                                    bool can_escape) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr || expr->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    AstNode *callee = xa_thread_lint_unwrap_expr(call->callee);
    if (!callee || callee->type != AST_MEMBER_ACCESS)
        return false;
    MemberAccessNode *ma = &callee->as.member_access;
    if (!ma->name || (strcmp(ma->name, "join") != 0 && strcmp(ma->name, "detach") != 0))
        return false;
    uint32_t receiver_id = xa_thread_lint_expr_symbol_id(ma->object);
    XaThreadHandleLintState *state = xa_thread_lint_find_by_symbol_id(states, receiver_id);
    if (!state)
        return false;
    if (can_escape)
        state->finalized = true;
    return true;
}

static void xa_thread_lint_scan_expr(XaThreadHandleLintState *states, AstNode *expr,
                                     bool return_value, bool can_escape) {
    expr = xa_thread_lint_unwrap_expr(expr);
    if (!expr)
        return;

    if (return_value && can_escape) {
        uint32_t sym_id = xa_thread_lint_expr_symbol_id(expr);
        XaThreadHandleLintState *state = xa_thread_lint_find_by_symbol_id(states, sym_id);
        if (state) {
            state->transferred = true;
            return;
        }
    }

    switch (expr->type) {
        case AST_VARIABLE:
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_BIGINT:
        case AST_LITERAL_STRING:
        case AST_LITERAL_CHAR:
        case AST_LITERAL_REGEX:
        case AST_LITERAL_NULL:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
            return;

        case AST_CALL_EXPR: {
            if (xa_thread_lint_scan_join_or_detach_call(states, expr, can_escape))
                return;
            CallExprNode *call = &expr->as.call_expr;
            xa_thread_lint_scan_expr(states, call->callee, false, can_escape);
            xa_thread_lint_scan_expr_array(states, call->arguments, call->arg_count, false,
                                           can_escape);
            return;
        }

        case AST_MEMBER_ACCESS:
            xa_thread_lint_scan_expr(states, expr->as.member_access.object, false, can_escape);
            return;
        case AST_MEMBER_SET:
            xa_thread_lint_scan_expr(states, expr->as.member_set.object, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.member_set.value, false, can_escape);
            return;
        case AST_INDEX_GET:
            xa_thread_lint_scan_expr(states, expr->as.index_get.array, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.index_get.index, false, can_escape);
            return;
        case AST_INDEX_SET:
            xa_thread_lint_scan_expr(states, expr->as.index_set.array, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.index_set.index, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.index_set.value, false, can_escape);
            return;

        case AST_ASSIGNMENT:
            xa_thread_lint_scan_expr(states, expr->as.assignment.value, false, can_escape);
            return;
        case AST_COMPOUND_ASSIGNMENT:
            xa_thread_lint_scan_expr(states, expr->as.compound_assignment.object, false,
                                     can_escape);
            xa_thread_lint_scan_expr(states, expr->as.compound_assignment.value, false, can_escape);
            return;
        case AST_INC:
        case AST_DEC:
            return;

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
            xa_thread_lint_scan_expr(states, expr->as.binary.left, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.binary.right, false, can_escape);
            return;

        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
            xa_thread_lint_scan_expr(states, expr->as.unary.operand, false, can_escape);
            return;
        case AST_GROUPING:
            xa_thread_lint_scan_expr(states, expr->as.grouping, false, can_escape);
            return;

        case AST_ARRAY_LITERAL:
            if (expr->as.array_literal.is_repeat) {
                xa_thread_lint_scan_expr(states, expr->as.array_literal.repeat_value, false,
                                         can_escape);
                xa_thread_lint_scan_expr(states, expr->as.array_literal.repeat_count, false,
                                         can_escape);
            } else {
                xa_thread_lint_scan_expr_array(states, expr->as.array_literal.elements,
                                               expr->as.array_literal.count, false, can_escape);
            }
            return;
        case AST_TUPLE_LITERAL:
            xa_thread_lint_scan_expr_array(states, expr->as.tuple_literal.elements,
                                           expr->as.tuple_literal.count, false, can_escape);
            return;
        case AST_SPREAD_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.spread_expr.expr, false, can_escape);
            return;
        case AST_OBJECT_LITERAL:
            xa_thread_lint_scan_expr_array(states, expr->as.object_literal.keys,
                                           expr->as.object_literal.count, false, can_escape);
            xa_thread_lint_scan_expr_array(states, expr->as.object_literal.values,
                                           expr->as.object_literal.count, false, can_escape);
            return;
        case AST_MAP_LITERAL:
            xa_thread_lint_scan_expr_array(states, expr->as.map_literal.keys,
                                           expr->as.map_literal.count, false, can_escape);
            xa_thread_lint_scan_expr_array(states, expr->as.map_literal.values,
                                           expr->as.map_literal.count, false, can_escape);
            return;
        case AST_SET_LITERAL:
            xa_thread_lint_scan_expr_array(states, expr->as.set_literal.elements,
                                           expr->as.set_literal.count, false, can_escape);
            return;
        case AST_STRUCT_LITERAL:
            xa_thread_lint_scan_expr_array(states, expr->as.struct_literal.field_values,
                                           expr->as.struct_literal.field_count, false, can_escape);
            return;

        case AST_TERNARY:
            xa_thread_lint_scan_expr(states, expr->as.ternary.condition, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.ternary.true_expr, false, false);
            xa_thread_lint_scan_expr(states, expr->as.ternary.false_expr, false, false);
            return;
        case AST_OPTIONAL_CHAIN:
            xa_thread_lint_scan_expr(states, expr->as.optional_chain.object, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.optional_chain.index, false, can_escape);
            return;
        case AST_RANGE:
            xa_thread_lint_scan_expr(states, expr->as.range.start, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.range.end, false, can_escape);
            return;
        case AST_IS_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.is_expr.expr, false, can_escape);
            return;
        case AST_AS_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.as_expr.expr, false, can_escape);
            return;
        case AST_COMPTIME_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.comptime_expr.expr, false, can_escape);
            return;
        case AST_UNSAFE_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.unsafe_expr.operand, false, can_escape);
            return;
        case AST_MOVE_EXPR: {
            uint32_t sym_id = xa_thread_lint_expr_symbol_id(expr->as.move_expr.expr);
            XaThreadHandleLintState *state = xa_thread_lint_find_by_symbol_id(states, sym_id);
            if (state && can_escape)
                state->transferred = true;
            xa_thread_lint_scan_expr(states, expr->as.move_expr.expr, false, can_escape);
            return;
        }

        case AST_GO_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.go_expr.expr, false, can_escape);
            return;
        case AST_AWAIT_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.await_expr.expr, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.await_expr.timeout, false, can_escape);
            xa_thread_lint_scan_expr(states, expr->as.await_expr.into, false, can_escape);
            return;
        case AST_PARALLEL_REDUCE_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.parallel_reduce_expr.range, false,
                                     can_escape);
            xa_thread_lint_scan_expr(states, expr->as.parallel_reduce_expr.worker_count, false,
                                     can_escape);
            xa_thread_lint_scan_expr(states, expr->as.parallel_reduce_expr.initial, false,
                                     can_escape);
            xa_thread_lint_scan_expr(states, expr->as.parallel_reduce_expr.combine, false,
                                     can_escape);
            xa_thread_lint_scan_expr(states, expr->as.parallel_reduce_expr.body, false, false);
            return;
        case AST_PARALLEL_COLLECT_EXPR:
            xa_thread_lint_scan_expr(states, expr->as.parallel_collect_expr.range, false,
                                     can_escape);
            xa_thread_lint_scan_expr(states, expr->as.parallel_collect_expr.worker_count, false,
                                     can_escape);
            xa_thread_lint_scan_expr(states, expr->as.parallel_collect_expr.into, false,
                                     can_escape);
            xa_thread_lint_scan_expr(states, expr->as.parallel_collect_expr.body, false, false);
            xa_thread_lint_scan_expr(states, expr->as.parallel_collect_expr.final_body, false,
                                     false);
            return;
        case AST_MATCH_EXPR:
            xa_thread_lint_scan_match_expr(states, expr, can_escape);
            return;

        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
        case AST_UNION_DECL:
        case AST_METHOD_DECL:
        case AST_FIELD_DECL:
        case AST_ENUM_DECL:
        case AST_ENUM_MEMBER:
        case AST_IMPORT_STMT:
            return;

        default:
            return;
    }
}

static void xa_thread_lint_scan_block(XaThreadHandleLintState *states, AstNode *block,
                                      bool can_escape) {
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(block, &statements, &count))
        return;
    for (int i = 0; i < count; i++)
        xa_thread_lint_scan_stmt(states, statements[i], can_escape);
}

static void xa_thread_lint_scan_if_stmt(XaThreadHandleLintState *states, AstNode *stmt,
                                        bool can_escape) {
    xa_thread_lint_scan_expr(states, stmt->as.if_stmt.condition, false, can_escape);
    if (!can_escape) {
        xa_thread_lint_scan_stmt(states, stmt->as.if_stmt.then_branch, false);
        xa_thread_lint_scan_stmt(states, stmt->as.if_stmt.else_branch, false);
        return;
    }

    int state_count = xa_thread_lint_state_count(states);
    if (state_count <= 0)
        return;

    size_t snapshot_size = sizeof(XaThreadHandleLintSnapshot) * (size_t) state_count;
    XaThreadHandleLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *then_after = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *else_after = xr_calloc(1, snapshot_size);
    if (!before || !then_after || !else_after) {
        xr_free(before);
        xr_free(then_after);
        xr_free(else_after);
        xa_thread_lint_scan_stmt(states, stmt->as.if_stmt.then_branch, false);
        xa_thread_lint_scan_stmt(states, stmt->as.if_stmt.else_branch, false);
        return;
    }

    xa_thread_lint_snapshot_states(states, before);
    xa_thread_lint_scan_stmt(states, stmt->as.if_stmt.then_branch, true);
    xa_thread_lint_snapshot_states(states, then_after);

    xa_thread_lint_restore_states(states, before);
    if (stmt->as.if_stmt.else_branch)
        xa_thread_lint_scan_stmt(states, stmt->as.if_stmt.else_branch, true);
    xa_thread_lint_snapshot_states(states, else_after);

    xa_thread_lint_restore_states(states, before);
    int i = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next, i++) {
        bool before_closed = xa_thread_lint_snapshot_closed(&before[i]);
        bool then_closed = xa_thread_lint_snapshot_closed(&then_after[i]);
        bool else_closed = xa_thread_lint_snapshot_closed(&else_after[i]);
        if (!before_closed && then_closed && else_closed)
            s->finalized = true;
    }

    xr_free(before);
    xr_free(then_after);
    xr_free(else_after);
}

static void xa_thread_lint_scan_try_catch_stmt(XaThreadHandleLintState *states, AstNode *stmt,
                                               bool can_escape) {
    if (!stmt || stmt->type != AST_TRY_CATCH)
        return;
    TryCatchNode *tc = &stmt->as.try_catch;

    if (!can_escape) {
        xa_thread_lint_scan_stmt(states, tc->try_body, false);
        for (int i = 0; i < tc->catch_count; i++) {
            if (tc->catch_clauses[i])
                xa_thread_lint_scan_stmt(states, tc->catch_clauses[i]->body, false);
        }
        return;
    }

    int state_count = xa_thread_lint_state_count(states);
    if (state_count <= 0)
        return;

    size_t snapshot_size = sizeof(XaThreadHandleLintSnapshot) * (size_t) state_count;
    XaThreadHandleLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *try_after = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *catch_after = xr_calloc(1, snapshot_size);
    bool *all_paths_closed = xr_calloc((size_t) state_count, sizeof(bool));
    if (!before || !try_after || !catch_after || !all_paths_closed) {
        xr_free(before);
        xr_free(try_after);
        xr_free(catch_after);
        xr_free(all_paths_closed);
        xa_thread_lint_scan_stmt(states, tc->try_body, false);
        for (int i = 0; i < tc->catch_count; i++) {
            if (tc->catch_clauses[i])
                xa_thread_lint_scan_stmt(states, tc->catch_clauses[i]->body, false);
        }
        return;
    }

    xa_thread_lint_snapshot_states(states, before);
    xa_thread_lint_scan_stmt(states, tc->try_body, true);
    xa_thread_lint_snapshot_states(states, try_after);

    for (int i = 0; i < state_count; i++)
        all_paths_closed[i] = xa_thread_lint_snapshot_closed(&try_after[i]);

    for (int ci = 0; ci < tc->catch_count; ci++) {
        XrCatchClause *cc = tc->catch_clauses[ci];
        if (!cc) {
            for (int i = 0; i < state_count; i++)
                all_paths_closed[i] = false;
            continue;
        }
        xa_thread_lint_restore_states(states, before);
        memset(catch_after, 0, snapshot_size);
        xa_thread_lint_scan_stmt(states, cc->body, true);
        xa_thread_lint_snapshot_states(states, catch_after);
        for (int i = 0; i < state_count; i++)
            all_paths_closed[i] =
                all_paths_closed[i] && xa_thread_lint_snapshot_closed(&catch_after[i]);
    }

    xa_thread_lint_restore_states(states, before);
    int i = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next, i++) {
        bool before_closed = xa_thread_lint_snapshot_closed(&before[i]);
        if (!before_closed && all_paths_closed[i])
            s->finalized = true;
    }

    xr_free(before);
    xr_free(try_after);
    xr_free(catch_after);
    xr_free(all_paths_closed);
}

static void xa_thread_lint_scan_match_expr(XaThreadHandleLintState *states, AstNode *expr,
                                           bool can_escape) {
    if (!expr || expr->type != AST_MATCH_EXPR)
        return;
    MatchExprNode *match = &expr->as.match_expr;
    xa_thread_lint_scan_expr(states, match->expr, false, can_escape);

    if (!can_escape) {
        for (int i = 0; i < match->arm_count; i++)
            xa_thread_lint_scan_stmt(states, match->arms[i], false);
        return;
    }

    int state_count = xa_thread_lint_state_count(states);
    if (state_count <= 0)
        return;

    size_t snapshot_size = sizeof(XaThreadHandleLintSnapshot) * (size_t) state_count;
    XaThreadHandleLintSnapshot *before = xr_calloc(1, snapshot_size);
    XaThreadHandleLintSnapshot *arm_after = xr_calloc(1, snapshot_size);
    bool *all_paths_closed = xr_calloc((size_t) state_count, sizeof(bool));
    if (!before || !arm_after || !all_paths_closed) {
        xr_free(before);
        xr_free(arm_after);
        xr_free(all_paths_closed);
        for (int i = 0; i < match->arm_count; i++)
            xa_thread_lint_scan_stmt(states, match->arms[i], false);
        return;
    }

    xa_thread_lint_snapshot_states(states, before);
    for (int i = 0; i < state_count; i++)
        all_paths_closed[i] = match->arm_count > 0;

    for (int ai = 0; ai < match->arm_count; ai++) {
        if (!match->arms[ai]) {
            for (int i = 0; i < state_count; i++)
                all_paths_closed[i] = false;
            continue;
        }
        xa_thread_lint_restore_states(states, before);
        memset(arm_after, 0, snapshot_size);
        xa_thread_lint_scan_stmt(states, match->arms[ai], true);
        xa_thread_lint_snapshot_states(states, arm_after);
        for (int i = 0; i < state_count; i++)
            all_paths_closed[i] =
                all_paths_closed[i] && xa_thread_lint_snapshot_closed(&arm_after[i]);
    }

    xa_thread_lint_restore_states(states, before);
    int i = 0;
    for (XaThreadHandleLintState *s = states; s; s = s->next, i++) {
        bool before_closed = xa_thread_lint_snapshot_closed(&before[i]);
        if (!before_closed && all_paths_closed[i])
            s->finalized = true;
    }

    xr_free(before);
    xr_free(arm_after);
    xr_free(all_paths_closed);
}

static void xa_thread_lint_scan_stmt(XaThreadHandleLintState *states, AstNode *stmt,
                                     bool can_escape) {
    if (!stmt)
        return;
    switch (stmt->type) {
        case AST_EXPR_STMT:
            xa_thread_lint_scan_expr(states, stmt->as.expr_stmt, false, can_escape);
            return;
        case AST_VAR_DECL:
        case AST_CONST_DECL:
        case AST_SHARED_DECL:
            xa_thread_lint_scan_expr(states, stmt->as.var_decl.initializer, false, can_escape);
            return;
        case AST_ASSIGNMENT:
        case AST_COMPOUND_ASSIGNMENT:
        case AST_INC:
        case AST_DEC:
        case AST_MEMBER_SET:
        case AST_INDEX_SET:
            xa_thread_lint_scan_expr(states, stmt, false, can_escape);
            return;
        case AST_PRINT_STMT:
            xa_thread_lint_scan_expr_array(states, stmt->as.print_stmt.exprs,
                                           stmt->as.print_stmt.expr_count, false, can_escape);
            return;
        case AST_RETURN_STMT:
            xa_thread_lint_scan_expr_array(states, stmt->as.return_stmt.values,
                                           stmt->as.return_stmt.value_count, true, can_escape);
            return;
        case AST_IF_STMT:
            xa_thread_lint_scan_if_stmt(states, stmt, can_escape);
            return;
        case AST_WHILE_STMT:
            xa_thread_lint_scan_expr(states, stmt->as.while_stmt.condition, false, can_escape);
            xa_thread_lint_scan_stmt(states, stmt->as.while_stmt.body, false);
            return;
        case AST_FOR_STMT:
            xa_thread_lint_scan_stmt(states, stmt->as.for_stmt.initializer, can_escape);
            xa_thread_lint_scan_expr(states, stmt->as.for_stmt.condition, false, can_escape);
            xa_thread_lint_scan_expr(states, stmt->as.for_stmt.increment, false, false);
            xa_thread_lint_scan_stmt(states, stmt->as.for_stmt.body, false);
            return;
        case AST_FOR_IN_STMT:
            xa_thread_lint_scan_expr(states, stmt->as.for_in_stmt.collection, false, can_escape);
            xa_thread_lint_scan_stmt(states, stmt->as.for_in_stmt.body, false);
            return;
        case AST_PARALLEL_FOR_STMT:
            xa_thread_lint_scan_expr(states, stmt->as.parallel_for_stmt.range, false, can_escape);
            xa_thread_lint_scan_expr(states, stmt->as.parallel_for_stmt.worker_count, false,
                                     can_escape);
            xa_thread_lint_scan_expr(states, stmt->as.parallel_for_stmt.body, false, false);
            xa_thread_lint_scan_expr(states, stmt->as.parallel_for_stmt.final_body, false, false);
            return;
        case AST_BLOCK:
        case AST_PROGRAM:
            xa_thread_lint_scan_block(states, stmt, can_escape);
            return;
        case AST_TRY_CATCH:
            xa_thread_lint_scan_try_catch_stmt(states, stmt, can_escape);
            return;
        case AST_MATCH_EXPR:
            xa_thread_lint_scan_match_expr(states, stmt, can_escape);
            return;
        case AST_MATCH_ARM:
            xa_thread_lint_scan_expr(states, stmt->as.match_arm.pattern, false, can_escape);
            xa_thread_lint_scan_expr(states, stmt->as.match_arm.guard, false, can_escape);
            xa_thread_lint_scan_stmt(states, stmt->as.match_arm.body, can_escape);
            return;
        case AST_THROW_STMT:
            xa_thread_lint_scan_expr(states, stmt->as.throw_stmt.expression, false, can_escape);
            return;
        case AST_DEFER_STMT:
            xa_thread_lint_scan_expr(states, stmt->as.defer_stmt.expr, false, can_escape);
            return;
        case AST_SCOPE_BLOCK:
            xa_thread_lint_scan_stmt(states, stmt->as.scope_block.body, can_escape);
            return;
        case AST_EXPORT_STMT:
            xa_thread_lint_scan_stmt(states, stmt->as.export_stmt.declaration, can_escape);
            return;
        default:
            xa_thread_lint_scan_expr(states, stmt, false, can_escape);
            return;
    }
}

static XaThreadHandleLintState *
xa_thread_lint_collect_spawn_states(XaInferContext *ctx, AstNode **statements, int count) {
    if (!ctx || !statements || count <= 0)
        return NULL;
    XaThreadHandleLintState *states = NULL;
    XaThreadHandleLintState **tail = &states;
    for (int i = 0; i < count; i++) {
        AstNode *stmt = statements[i];
        if (!stmt || (stmt->type != AST_VAR_DECL && stmt->type != AST_CONST_DECL))
            continue;
        VarDeclNode *var = &stmt->as.var_decl;
        if (!var->name || !var->initializer || !xa_expr_is_sys_thread_spawn_call(var->initializer))
            continue;
        XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, var->name);
        if (!xa_symbol_is_local_thread_handle(sym, ctx->analyzer->current_scope))
            continue;
        XaThreadHandleLintState *state = xr_calloc(1, sizeof(XaThreadHandleLintState));
        if (!state)
            continue;
        state->root = sym;
        state->decl_stmt = stmt;
        xa_thread_lint_add_alias(state, sym);
        *tail = state;
        tail = &state->next;
    }
    return states;
}

static void xa_thread_lint_collect_sequence_aliases(XaInferContext *ctx,
                                                    XaThreadHandleLintState *states,
                                                    AstNode **statements, int count) {
    if (!ctx || !states || !statements || count <= 0)
        return;
    for (int i = 0; i < count; i++) {
        AstNode *stmt = statements[i];
        if (!stmt || (stmt->type != AST_VAR_DECL && stmt->type != AST_CONST_DECL))
            continue;
        VarDeclNode *var = &stmt->as.var_decl;
        if (!var->name || !var->initializer)
            continue;
        uint32_t src_id = xa_thread_lint_expr_symbol_id(var->initializer);
        XaThreadHandleLintState *state = xa_thread_lint_find_by_symbol_id(states, src_id);
        if (!state)
            continue;
        XaSymbol *alias = xa_scope_lookup(ctx->analyzer->current_scope, var->name);
        if (xa_symbol_is_local_thread_handle(alias, ctx->analyzer->current_scope))
            xa_thread_lint_add_alias(state, alias);
    }
}

static void xa_warn_unused_sys_thread_spawn_decl(XaInferContext *ctx, AstNode *stmt) {
    if (!ctx || !ctx->analyzer || !stmt ||
        (stmt->type != AST_VAR_DECL && stmt->type != AST_CONST_DECL))
        return;

    VarDeclNode *var = &stmt->as.var_decl;
    if (!var->name || !var->initializer || !xa_expr_is_sys_thread_spawn_call(var->initializer))
        return;

    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, var->name);
    if (!xa_symbol_is_local_thread_handle(sym, ctx->analyzer->current_scope))
        return;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (!links || links->ref_count != 0)
        return;

    char msg[192];
    snprintf(msg, sizeof(msg),
             "Thread handle '%s' from sys.Thread.spawn is never used; call join() or detach() "
             "explicitly",
             var->name);
    XrLocation loc = {.file = ctx->file_path, .line = stmt->line, .column = stmt->column};
    if (!xa_warning_already_reported(ctx->analyzer, &loc, msg))
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_WARNING, XR_ERR_ANALYZE, msg, &loc);
}

static void xa_warn_sys_thread_lifecycle_in_sequence(XaInferContext *ctx, AstNode **statements,
                                                     int count, int error_count_before) {
    if (!ctx || !statements || count <= 0)
        return;
    if (xa_analyzer_error_diagnostic_count(ctx->analyzer) != error_count_before)
        return;

    for (int i = 0; i < count; i++)
        xa_warn_unused_sys_thread_spawn_decl(ctx, statements[i]);

    XaThreadHandleLintState *states = xa_thread_lint_collect_spawn_states(ctx, statements, count);
    if (!states)
        return;
    xa_thread_lint_collect_sequence_aliases(ctx, states, statements, count);
    for (int i = 0; i < count; i++)
        xa_thread_lint_scan_stmt(states, statements[i], true);
    for (XaThreadHandleLintState *state = states; state; state = state->next) {
        if (!state->root || !state->decl_stmt || state->finalized || state->transferred)
            continue;
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, state->root);
        if (!links || links->ref_count == 0)
            continue;
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "Thread handle '%s' from sys.Thread.spawn is not joined or detached before "
                 "leaving scope",
                 state->root->name ? state->root->name : "?");
        XrLocation loc = {.file = ctx->file_path,
                          .line = state->decl_stmt->line,
                          .column = state->decl_stmt->column};
        if (!xa_warning_already_reported(ctx->analyzer, &loc, msg))
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_WARNING, XR_ERR_ANALYZE, msg,
                                       &loc);
    }
    xa_thread_lint_free_states(states);
}

static bool xa_freestanding_top_const_allowed(XaInferContext *ctx, VarDeclNode *var) {
    if (!ctx || !ctx->analyzer || !var || !var->initializer)
        return false;
    XrCtValue value = {0};
    const char *err = NULL;
    (void) err;
    if (!xa_consteval_expr(ctx->analyzer, var->initializer, &value, &err))
        return false;
    return xa_freestanding_top_const_ct_value_allowed(&value);
}

XR_FUNC void xa_loop_scope_push(XaInferContext *ctx, XaLoopScope *scope, const char *label,
                                AstNode *node) {
    if (!ctx || !scope)
        return;
    scope->label = label;
    scope->line = node ? node->line : 0;
    scope->prev = ctx->loop_scope;

    if (label) {
        for (XaLoopScope *it = ctx->loop_scope; it; it = it->prev) {
            if (it->label && strcmp(it->label, label) == 0) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node ? node->line : 0, .column = 1};
                char msg[160];
                snprintf(msg, sizeof(msg), "duplicate loop label '%s' in an active loop", label);
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE, msg,
                                           &loc);
                break;
            }
        }
    }

    ctx->loop_scope = scope;
    ctx->loop_depth++;
}

XR_FUNC void xa_loop_scope_pop(XaInferContext *ctx, XaLoopScope *scope) {
    if (!ctx || !scope)
        return;
    if (ctx->loop_scope == scope)
        ctx->loop_scope = scope->prev;
    else
        ctx->loop_scope = scope->prev;
    if (ctx->loop_depth > 0)
        ctx->loop_depth--;
}

XR_FUNC void xa_validate_loop_control(XaInferContext *ctx, AstNode *node, const char *label,
                                      bool is_continue) {
    if (!ctx || !node)
        return;

    const char *kind = is_continue ? "continue" : "break";
    int code = is_continue ? XR_ERR_CMP_INVALID_CONTINUE : XR_ERR_CMP_INVALID_BREAK;
    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};

    if (label) {
        for (XaLoopScope *it = ctx->loop_scope; it; it = it->prev) {
            if (it->label && strcmp(it->label, label) == 0)
                return;
        }
        char msg[160];
        snprintf(msg, sizeof(msg), "unknown loop label '%s' for '%s'", label, kind);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, code, msg, &loc);
        return;
    }

    if (ctx->loop_depth <= 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "'%s' outside of a loop", kind);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, code, msg, &loc);
    }
}

static bool xa_function_assignment_mismatch(XrType *target_type, XrType *value_type) {
    if (!target_type || !value_type)
        return true;
    if (XR_TYPE_IS_NULL(value_type))
        return !target_type->is_nullable;
    if (!XR_TYPE_IS_FUNCTION(value_type))
        return true;
    if (target_type->is_nullable || value_type->is_nullable)
        return !xa_typecheck_assignable(target_type, value_type);
    return !xr_type_equals(target_type, value_type);
}

XR_FUNC void xa_assign_check_type(XaInferContext *ctx, AstNode *node, XrType *target_type,
                                  XrType *value_type, const char *target_name,
                                  const char *target_kind) {
    if (!ctx || !node || !target_type || !value_type)
        return;
    if (XR_TYPE_IS_UNKNOWN(target_type) || XR_TYPE_IS_UNKNOWN(value_type))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
    bool null_err =
        xa_check_null_safety(ctx->analyzer, target_type, value_type, "Assignment", &loc);
    bool type_mismatch = false;
    if (XR_TYPE_IS_FUNCTION(target_type)) {
        type_mismatch = xa_function_assignment_mismatch(target_type, value_type);
    } else {
        type_mismatch = !xa_typecheck_assignable(target_type, value_type);
    }
    if (null_err || !type_mismatch)
        return;
    if (!XR_TYPE_IS_FUNCTION(target_type) && xr_is_json_coercion(target_type, value_type))
        return;

    char msg[256];
    if (target_name && target_kind) {
        snprintf(msg, sizeof(msg), "Type '%s' is not assignable to %s '%s' (type '%s')",
                 xr_type_to_string(value_type), target_kind, target_name,
                 xr_type_to_string(target_type));
    } else if (target_name) {
        snprintf(msg, sizeof(msg), "Type '%s' is not assignable to '%s' (type '%s')",
                 xr_type_to_string(value_type), target_name, xr_type_to_string(target_type));
    } else {
        snprintf(msg, sizeof(msg), "Type '%s' is not assignable to type '%s'",
                 xr_type_to_string(value_type), xr_type_to_string(target_type));
    }
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

XR_FUNC bool xa_type_needs_borrow_escape_guard(XrType *type) {
    if (!type || XR_TYPE_IS_UNKNOWN(type) || XR_TYPE_IS_NULL(type))
        return false;
    if (type->is_value_type)
        return true;
    if (XR_TYPE_IS_UNION(type)) {
        int n = xr_type_union_count(type);
        for (int i = 0; i < n; i++) {
            if (xa_type_needs_borrow_escape_guard(xr_type_union_member(type, i)))
                return true;
        }
        return false;
    }
    switch (type->kind) {
        case XR_KIND_SPAN:
        case XR_KIND_VIEW:
        case XR_KIND_ARRAY:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
        case XR_KIND_JSON:
        case XR_KIND_RECORD:
        case XR_KIND_INSTANCE:
        case XR_KIND_FUNCTION:
        case XR_KIND_TUPLE:
        case XR_KIND_FIXED_ARRAY:
        case XR_KIND_TYPE_PARAM:
            return true;
        default:
            return false;
    }
}

XR_FUNC XaSymbol *xa_borrowed_param_root_symbol(XaInferContext *ctx, AstNode *expr) {
    if (!ctx || !ctx->analyzer)
        return NULL;
    while (expr) {
        switch (expr->type) {
            case AST_VARIABLE: {
                const char *name = expr->as.variable.name;
                if (!name)
                    return NULL;
                XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, name);
                if (sym && sym->kind == XA_SYM_PARAMETER &&
                    (sym->passing_mode == XR_PARAM_IN || sym->passing_mode == XR_PARAM_REF))
                    return sym;
                if (sym && sym->borrowed_root_symbol_id != 0) {
                    XaSymbol *root = xa_scope_lookup_by_id(ctx->analyzer->current_scope,
                                                           sym->borrowed_root_symbol_id);
                    if (root && root->kind == XA_SYM_PARAMETER &&
                        (root->passing_mode == XR_PARAM_IN || root->passing_mode == XR_PARAM_REF))
                        return root;
                }
                return NULL;
            }
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
            default:
                return NULL;
        }
    }
    return NULL;
}

XR_FUNC bool xa_type_contains_span_view(XrType *type) {
    if (!type || XR_TYPE_IS_UNKNOWN(type) || XR_TYPE_IS_NULL(type))
        return false;
    if (XR_TYPE_IS_SPAN(type))
        return true;
    if (XR_TYPE_IS_UNION(type)) {
        int n = xr_type_union_count(type);
        for (int i = 0; i < n; i++) {
            if (xa_type_contains_span_view(xr_type_union_member(type, i)))
                return true;
        }
        return false;
    }
    if (XR_TYPE_IS_TUPLE(type)) {
        for (int i = 0; i < type->tuple.element_count; i++) {
            if (xa_type_contains_span_view(type->tuple.element_types[i]))
                return true;
        }
    }
    return false;
}

static bool xa_call_expr_is_borrowed_view(AstNode *expr) {
    if (!expr || expr->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &expr->as.call_expr;
    if (!call->callee || call->callee->type != AST_MEMBER_ACCESS)
        return false;
    const char *name = call->callee->as.member_access.name;
    return name && (strcmp(name, "bytes") == 0 || strcmp(name, "asSpan") == 0 ||
                    strcmp(name, "asBytes") == 0 || strcmp(name, "reinterpret") == 0);
}

XR_FUNC bool xa_expr_has_stable_borrow_owner(AstNode *expr) {
    while (expr) {
        switch (expr->type) {
            case AST_VARIABLE:
            case AST_THIS_EXPR:
                return true;
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
            case AST_CALL_EXPR:
                if (!xa_call_expr_is_borrowed_view(expr))
                    return false;
                expr = expr->as.call_expr.callee->as.member_access.object;
                break;
            default:
                return false;
        }
    }
    return false;
}

XR_FUNC bool xa_type_can_own_span_view(XrType *type) {
    if (!type || XR_TYPE_IS_UNKNOWN(type) || XR_TYPE_IS_NULL(type))
        return false;
    if (XR_TYPE_IS_ARRAY(type) || XR_TYPE_IS_STRING(type))
        return true;
    return xr_type_is_named_class(type, "Buffer");
}

XR_FUNC XaSymbol *xa_root_variable_symbol_for_expr(XaInferContext *ctx, AstNode *expr) {
    while (expr) {
        switch (expr->type) {
            case AST_VARIABLE:
                return expr->as.variable.name
                           ? xa_lookup_visible_symbol(ctx, expr->as.variable.name)
                           : NULL;
            case AST_MEMBER_ACCESS:
                expr = expr->as.member_access.object;
                break;
            case AST_INDEX_GET:
                expr = expr->as.index_get.array;
                break;
            case AST_GROUPING:
                expr = expr->as.grouping;
                break;
            case AST_FORCE_UNWRAP:
                expr = expr->as.unary.operand;
                break;
            default:
                return NULL;
        }
    }
    return NULL;
}

static XaActiveSpanBorrow *xa_active_span_borrow_for_view(XaInferContext *ctx, XaSymbol *view_sym) {
    if (!ctx || !view_sym)
        return NULL;
    for (XaActiveSpanBorrow *b = ctx->active_span_borrows; b; b = b->next) {
        if (b->view_symbol == view_sym)
            return b;
    }
    return NULL;
}

static bool xa_path_copy(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0 || !src)
        return false;
    int n = snprintf(dst, dst_size, "%s", src);
    return n >= 0 && (size_t) n < dst_size;
}

static bool xa_path_append(char *dst, size_t dst_size, const char *suffix) {
    if (!dst || dst_size == 0 || !suffix)
        return false;
    size_t len = strlen(dst);
    if (len >= dst_size)
        return false;
    int n = snprintf(dst + len, dst_size - len, "%s", suffix);
    return n >= 0 && (size_t) n < dst_size - len;
}

static bool xa_index_path_segment(AstNode *index, char *buf, size_t buf_size) {
    if (!index || !buf || buf_size == 0)
        return false;
    while (index && (index->type == AST_GROUPING || index->type == AST_FORCE_UNWRAP))
        index = index->type == AST_GROUPING ? index->as.grouping : index->as.unary.operand;
    if (!index || index->type != AST_LITERAL_INT)
        return false;
    int n = snprintf(buf, buf_size, "[%lld]", (long long) index->as.literal.raw_value.int_val);
    return n >= 0 && (size_t) n < buf_size;
}

static bool xa_path_is_same_or_nested(const char *path, const char *prefix) {
    if (!path || !prefix)
        return true;
    size_t len = strlen(prefix);
    if (strncmp(path, prefix, len) != 0)
        return false;
    return path[len] == '\0' || path[len] == '.' || path[len] == '[';
}

static bool xa_owner_paths_may_overlap(const char *borrow_path, const char *mutation_path) {
    if (!borrow_path || !mutation_path)
        return true;
    return xa_path_is_same_or_nested(borrow_path, mutation_path) ||
           xa_path_is_same_or_nested(mutation_path, borrow_path);
}

static XaSymbol *xa_root_path_for_expr(XaInferContext *ctx, AstNode *expr, char *path_buf,
                                       size_t path_buf_size, bool *out_precise,
                                       bool follow_active_view) {
    if (!ctx || !expr || !path_buf || path_buf_size == 0)
        return NULL;
    if (out_precise)
        *out_precise = true;

    while (expr && (expr->type == AST_GROUPING || expr->type == AST_FORCE_UNWRAP))
        expr = expr->type == AST_GROUPING ? expr->as.grouping : expr->as.unary.operand;
    if (!expr)
        return NULL;

    switch (expr->type) {
        case AST_VARIABLE: {
            XaSymbol *sym = expr->as.variable.name
                                ? xa_lookup_visible_symbol(ctx, expr->as.variable.name)
                                : NULL;
            XaActiveSpanBorrow *active =
                follow_active_view ? xa_active_span_borrow_for_view(ctx, sym) : NULL;
            if (active) {
                if (active->owner_path) {
                    xa_path_copy(path_buf, path_buf_size, active->owner_path);
                } else if (active->owner_symbol && active->owner_symbol->name) {
                    xa_path_copy(path_buf, path_buf_size, active->owner_symbol->name);
                    if (out_precise)
                        *out_precise = false;
                }
                return active->owner_symbol;
            }
            if (!sym || !sym->name || !xa_path_copy(path_buf, path_buf_size, sym->name))
                return NULL;
            return sym;
        }
        case AST_MEMBER_ACCESS: {
            MemberAccessNode *ma = &expr->as.member_access;
            bool precise = true;
            XaSymbol *root = xa_root_path_for_expr(ctx, ma->object, path_buf, path_buf_size,
                                                   &precise, follow_active_view);
            if (!root)
                return NULL;
            if (precise && ma->name) {
                if (!xa_path_append(path_buf, path_buf_size, ".") ||
                    !xa_path_append(path_buf, path_buf_size, ma->name))
                    precise = false;
            }
            if (out_precise)
                *out_precise = precise;
            return root;
        }
        case AST_INDEX_GET: {
            IndexGetNode *ig = &expr->as.index_get;
            bool precise = true;
            XaSymbol *root = xa_root_path_for_expr(ctx, ig->array, path_buf, path_buf_size,
                                                   &precise, follow_active_view);
            if (!root)
                return NULL;
            char segment[64];
            if (precise && xa_index_path_segment(ig->index, segment, sizeof(segment))) {
                if (!xa_path_append(path_buf, path_buf_size, segment))
                    precise = false;
            } else {
                xa_path_copy(path_buf, path_buf_size, root->name ? root->name : "");
                precise = false;
            }
            if (out_precise)
                *out_precise = precise;
            return root;
        }
        case AST_SLICE_EXPR:
            return xa_root_path_for_expr(ctx, expr->as.slice_expr.source, path_buf, path_buf_size,
                                         out_precise, follow_active_view);
        case AST_OPTIONAL_CHAIN:
            return xa_root_path_for_expr(ctx, expr->as.optional_chain.object, path_buf,
                                         path_buf_size, out_precise, follow_active_view);
        case AST_CALL_EXPR:
            if (xa_call_expr_is_borrowed_view(expr) && expr->as.call_expr.callee &&
                expr->as.call_expr.callee->type == AST_MEMBER_ACCESS) {
                return xa_root_path_for_expr(
                    ctx, expr->as.call_expr.callee->as.member_access.object, path_buf,
                    path_buf_size, out_precise, follow_active_view);
            }
            return NULL;
        default:
            return NULL;
    }
}

static bool xa_node_uses_symbol_name(AstNode *node, const char *name);

static bool xa_block_node_statements(AstNode *node, AstNode ***out_statements, int *out_count) {
    if (out_statements)
        *out_statements = NULL;
    if (out_count)
        *out_count = 0;
    if (!node)
        return false;
    if (node->type == AST_BLOCK) {
        if (out_statements)
            *out_statements = node->as.block.statements;
        if (out_count)
            *out_count = node->as.block.count;
        return true;
    }
    if (node->type == AST_PROGRAM) {
        if (out_statements)
            *out_statements = node->as.program.statements;
        if (out_count)
            *out_count = node->as.program.count;
        return true;
    }
    return false;
}

static bool xa_node_array_uses_symbol_name(AstNode **nodes, int count, const char *name) {
    if (!nodes || count <= 0 || !name)
        return false;
    for (int i = 0; i < count; i++) {
        if (xa_node_uses_symbol_name(nodes[i], name))
            return true;
    }
    return false;
}

static bool xa_parallel_locals_use_symbol_name(XrParallelLocalBinding *locals, int count,
                                               const char *name) {
    if (!locals || count <= 0 || !name)
        return false;
    for (int i = 0; i < count; i++) {
        if (xa_node_uses_symbol_name(locals[i].source, name))
            return true;
    }
    return false;
}

static bool xa_block_uses_symbol_name_from(AstNode *node, const char *name, int start_index) {
    if (!node || !name)
        return false;
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(node, &statements, &count))
        return xa_node_uses_symbol_name(node, name);
    if (start_index < 0)
        start_index = 0;
    for (int i = start_index; i < count; i++) {
        AstNode *stmt = statements[i];
        if (!stmt)
            continue;
        if ((stmt->type == AST_VAR_DECL || stmt->type == AST_CONST_DECL ||
             stmt->type == AST_SHARED_DECL) &&
            stmt->as.var_decl.name && strcmp(stmt->as.var_decl.name, name) == 0) {
            return xa_node_uses_symbol_name(stmt->as.var_decl.initializer, name);
        }
        if (xa_node_uses_symbol_name(stmt, name))
            return true;
    }
    return false;
}

static bool xa_node_uses_symbol_name(AstNode *node, const char *name) {
    if (!node || !name)
        return false;

    switch (node->type) {
        case AST_VARIABLE:
            return node->as.variable.name && strcmp(node->as.variable.name, name) == 0;

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
            return xa_node_uses_symbol_name(node->as.binary.left, name) ||
                   xa_node_uses_symbol_name(node->as.binary.right, name);

        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
        case AST_FORCE_UNWRAP:
            return xa_node_uses_symbol_name(node->as.unary.operand, name);

        case AST_GROUPING:
            return xa_node_uses_symbol_name(node->as.grouping, name);

        case AST_EXPR_STMT:
            return xa_node_uses_symbol_name(node->as.expr_stmt, name);
        case AST_PRINT_STMT:
            return xa_node_array_uses_symbol_name(node->as.print_stmt.exprs,
                                                  node->as.print_stmt.expr_count, name);
        case AST_BLOCK:
            return xa_block_uses_symbol_name_from(node, name, 0);

        case AST_VAR_DECL:
        case AST_CONST_DECL:
        case AST_SHARED_DECL:
            return xa_node_uses_symbol_name(node->as.var_decl.initializer, name);
        case AST_ASSIGNMENT:
            return xa_node_uses_symbol_name(node->as.assignment.value, name);
        case AST_COMPOUND_ASSIGNMENT:
            return (node->as.compound_assignment.name &&
                    strcmp(node->as.compound_assignment.name, name) == 0) ||
                   xa_node_uses_symbol_name(node->as.compound_assignment.object, name) ||
                   xa_node_uses_symbol_name(node->as.compound_assignment.value, name);
        case AST_INC:
        case AST_DEC:
            return node->as.inc.name && strcmp(node->as.inc.name, name) == 0;
        case AST_DESTRUCTURE_DECL:
            return xa_node_uses_symbol_name(node->as.destructure_decl.initializer, name);
        case AST_DESTRUCTURE_ASSIGN:
            return xa_node_uses_symbol_name(node->as.destructure_assign.value, name);

        case AST_IF_STMT:
            return xa_node_uses_symbol_name(node->as.if_stmt.condition, name) ||
                   xa_node_uses_symbol_name(node->as.if_stmt.then_branch, name) ||
                   xa_node_uses_symbol_name(node->as.if_stmt.else_branch, name);
        case AST_WHILE_STMT:
            return xa_node_uses_symbol_name(node->as.while_stmt.condition, name) ||
                   xa_node_uses_symbol_name(node->as.while_stmt.body, name);
        case AST_FOR_STMT:
            return xa_node_uses_symbol_name(node->as.for_stmt.initializer, name) ||
                   xa_node_uses_symbol_name(node->as.for_stmt.condition, name) ||
                   xa_node_uses_symbol_name(node->as.for_stmt.increment, name) ||
                   xa_node_uses_symbol_name(node->as.for_stmt.body, name);
        case AST_FOR_IN_STMT:
            return xa_node_uses_symbol_name(node->as.for_in_stmt.collection, name) ||
                   xa_node_uses_symbol_name(node->as.for_in_stmt.body, name);
        case AST_PARALLEL_FOR_STMT:
            return xa_parallel_locals_use_symbol_name(node->as.parallel_for_stmt.locals,
                                                      node->as.parallel_for_stmt.local_count,
                                                      name) ||
                   xa_node_uses_symbol_name(node->as.parallel_for_stmt.range, name) ||
                   xa_node_uses_symbol_name(node->as.parallel_for_stmt.worker_count, name) ||
                   xa_node_uses_symbol_name(node->as.parallel_for_stmt.body, name) ||
                   xa_node_uses_symbol_name(node->as.parallel_for_stmt.final_body, name);

        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            return xa_node_uses_symbol_name(node->as.function_decl.body, name);
        case AST_CALL_EXPR:
            return xa_node_uses_symbol_name(node->as.call_expr.callee, name) ||
                   xa_node_array_uses_symbol_name(node->as.call_expr.arguments,
                                                  node->as.call_expr.arg_count, name);
        case AST_PARALLEL_REDUCE_EXPR:
            return xa_parallel_locals_use_symbol_name(node->as.parallel_reduce_expr.locals,
                                                      node->as.parallel_reduce_expr.local_count,
                                                      name) ||
                   xa_node_uses_symbol_name(node->as.parallel_reduce_expr.range, name) ||
                   xa_node_uses_symbol_name(node->as.parallel_reduce_expr.worker_count, name) ||
                   xa_node_uses_symbol_name(node->as.parallel_reduce_expr.initial, name) ||
                   xa_node_uses_symbol_name(node->as.parallel_reduce_expr.combine, name) ||
                   xa_node_uses_symbol_name(node->as.parallel_reduce_expr.body, name);
        case AST_PARALLEL_COLLECT_EXPR:
            return xa_parallel_locals_use_symbol_name(node->as.parallel_collect_expr.locals,
                                                      node->as.parallel_collect_expr.local_count,
                                                      name) ||
                   xa_node_uses_symbol_name(node->as.parallel_collect_expr.range, name) ||
                   xa_node_uses_symbol_name(node->as.parallel_collect_expr.worker_count, name) ||
                   xa_node_uses_symbol_name(node->as.parallel_collect_expr.into, name) ||
                   xa_node_uses_symbol_name(node->as.parallel_collect_expr.body, name) ||
                   xa_node_uses_symbol_name(node->as.parallel_collect_expr.final_body, name);
        case AST_RETURN_STMT:
            return xa_node_array_uses_symbol_name(node->as.return_stmt.values,
                                                  node->as.return_stmt.value_count, name);

        case AST_ARRAY_LITERAL:
            if (node->as.array_literal.is_repeat)
                return xa_node_uses_symbol_name(node->as.array_literal.repeat_value, name) ||
                       xa_node_uses_symbol_name(node->as.array_literal.repeat_count, name);
            return xa_node_array_uses_symbol_name(node->as.array_literal.elements,
                                                  node->as.array_literal.count, name);
        case AST_TUPLE_LITERAL:
            return xa_node_array_uses_symbol_name(node->as.tuple_literal.elements,
                                                  node->as.tuple_literal.count, name);
        case AST_SPREAD_EXPR:
            return xa_node_uses_symbol_name(node->as.spread_expr.expr, name);
        case AST_INDEX_GET:
            return xa_node_uses_symbol_name(node->as.index_get.array, name) ||
                   xa_node_uses_symbol_name(node->as.index_get.index, name);
        case AST_INDEX_SET:
            return xa_node_uses_symbol_name(node->as.index_set.array, name) ||
                   xa_node_uses_symbol_name(node->as.index_set.index, name) ||
                   xa_node_uses_symbol_name(node->as.index_set.value, name);
        case AST_SLICE_EXPR:
            return xa_node_uses_symbol_name(node->as.slice_expr.source, name) ||
                   xa_node_uses_symbol_name(node->as.slice_expr.start, name) ||
                   xa_node_uses_symbol_name(node->as.slice_expr.end, name);
        case AST_MEMBER_ACCESS:
            return xa_node_uses_symbol_name(node->as.member_access.object, name);
        case AST_MEMBER_SET:
            return xa_node_uses_symbol_name(node->as.member_set.object, name) ||
                   xa_node_uses_symbol_name(node->as.member_set.value, name);
        case AST_TEMPLATE_STRING:
            return xa_node_array_uses_symbol_name(node->as.template_str.parts,
                                                  node->as.template_str.part_count, name);
        case AST_OBJECT_LITERAL:
            return xa_node_array_uses_symbol_name(node->as.object_literal.keys,
                                                  node->as.object_literal.count, name) ||
                   xa_node_array_uses_symbol_name(node->as.object_literal.values,
                                                  node->as.object_literal.count, name);
        case AST_MAP_LITERAL:
            return xa_node_array_uses_symbol_name(node->as.map_literal.keys,
                                                  node->as.map_literal.count, name) ||
                   xa_node_array_uses_symbol_name(node->as.map_literal.values,
                                                  node->as.map_literal.count, name);
        case AST_SET_LITERAL:
            return xa_node_array_uses_symbol_name(node->as.set_literal.elements,
                                                  node->as.set_literal.count, name);
        case AST_STRUCT_LITERAL:
            return xa_node_array_uses_symbol_name(node->as.struct_literal.field_values,
                                                  node->as.struct_literal.field_count, name);

        case AST_TERNARY:
            return xa_node_uses_symbol_name(node->as.ternary.condition, name) ||
                   xa_node_uses_symbol_name(node->as.ternary.true_expr, name) ||
                   xa_node_uses_symbol_name(node->as.ternary.false_expr, name);
        case AST_OPTIONAL_CHAIN:
            return xa_node_uses_symbol_name(node->as.optional_chain.object, name) ||
                   xa_node_uses_symbol_name(node->as.optional_chain.index, name);
        case AST_RANGE:
            return xa_node_uses_symbol_name(node->as.range.start, name) ||
                   xa_node_uses_symbol_name(node->as.range.end, name);
        case AST_IS_EXPR:
            return xa_node_uses_symbol_name(node->as.is_expr.expr, name);
        case AST_AS_EXPR:
            return xa_node_uses_symbol_name(node->as.as_expr.expr, name);
        case AST_NEW_EXPR:
            return xa_node_array_uses_symbol_name(node->as.new_expr.arguments,
                                                  node->as.new_expr.arg_count, name);
        case AST_SUPER_CALL:
            return xa_node_array_uses_symbol_name(node->as.super_call.arguments,
                                                  node->as.super_call.arg_count, name);

        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
        case AST_UNION_DECL:
            return xa_node_array_uses_symbol_name(node->as.class_decl.fields,
                                                  node->as.class_decl.field_count, name) ||
                   xa_node_array_uses_symbol_name(node->as.class_decl.methods,
                                                  node->as.class_decl.method_count, name);
        case AST_FIELD_DECL:
            return xa_node_uses_symbol_name(node->as.field_decl.initializer, name);
        case AST_METHOD_DECL:
            return xa_node_array_uses_symbol_name(node->as.method_decl.base_args,
                                                  node->as.method_decl.base_arg_count, name) ||
                   xa_node_array_uses_symbol_name(node->as.method_decl.default_values,
                                                  node->as.method_decl.param_count, name) ||
                   xa_node_uses_symbol_name(node->as.method_decl.body, name);

        case AST_ENUM_DECL:
            return xa_node_array_uses_symbol_name(node->as.enum_decl.members,
                                                  node->as.enum_decl.member_count, name) ||
                   xa_node_array_uses_symbol_name(node->as.enum_decl.methods,
                                                  node->as.enum_decl.method_count, name);
        case AST_ENUM_MEMBER:
            return false;
        case AST_ENUM_INDEX:
            return xa_node_uses_symbol_name(node->as.enum_index.collection, name) ||
                   xa_node_uses_symbol_name(node->as.enum_index.index_expr, name);

        case AST_TRY_CATCH: {
            TryCatchNode *tc = &node->as.try_catch;
            if (xa_node_uses_symbol_name(tc->try_body, name))
                return true;
            for (int i = 0; i < tc->catch_count; i++) {
                if (tc->catch_clauses[i] &&
                    xa_node_uses_symbol_name(tc->catch_clauses[i]->body, name))
                    return true;
            }
            return false;
        }
        case AST_THROW_STMT:
            return xa_node_uses_symbol_name(node->as.throw_stmt.expression, name);
        case AST_EXPORT_STMT:
            return xa_node_uses_symbol_name(node->as.export_stmt.declaration, name);

        case AST_MATCH_EXPR:
            return xa_node_uses_symbol_name(node->as.match_expr.expr, name) ||
                   xa_node_array_uses_symbol_name(node->as.match_expr.arms,
                                                  node->as.match_expr.arm_count, name);
        case AST_MATCH_ARM:
            return xa_node_uses_symbol_name(node->as.match_arm.pattern, name) ||
                   xa_node_uses_symbol_name(node->as.match_arm.guard, name) ||
                   xa_node_uses_symbol_name(node->as.match_arm.body, name);
        case AST_PATTERN_LITERAL:
            return xa_node_uses_symbol_name(node->as.pattern_literal.value, name);
        case AST_PATTERN_RANGE:
            return xa_node_uses_symbol_name(node->as.pattern_range.start, name) ||
                   xa_node_uses_symbol_name(node->as.pattern_range.end, name);
        case AST_PATTERN_MULTI:
            return xa_node_array_uses_symbol_name(node->as.pattern_multi.patterns,
                                                  node->as.pattern_multi.count, name);
        case AST_PATTERN_TUPLE:
            return xa_node_array_uses_symbol_name(node->as.pattern_tuple.patterns,
                                                  node->as.pattern_tuple.count, name);
        case AST_PATTERN_ADT:
            return xa_node_uses_symbol_name(node->as.pattern_adt.variant, name) ||
                   xa_node_array_uses_symbol_name(node->as.pattern_adt.patterns,
                                                  node->as.pattern_adt.count, name);
        case AST_PATTERN_OBJECT:
            return xa_node_array_uses_symbol_name(node->as.pattern_object.patterns,
                                                  node->as.pattern_object.count, name);
        case AST_PATTERN_ARRAY:
            return xa_node_array_uses_symbol_name(node->as.pattern_array.patterns,
                                                  node->as.pattern_array.count, name);

        case AST_GO_EXPR:
            return xa_node_uses_symbol_name(node->as.go_expr.expr, name);
        case AST_AWAIT_EXPR:
            return xa_node_uses_symbol_name(node->as.await_expr.expr, name) ||
                   xa_node_uses_symbol_name(node->as.await_expr.timeout, name) ||
                   xa_node_uses_symbol_name(node->as.await_expr.into, name);
        case AST_CHANNEL_NEW:
            return xa_node_uses_symbol_name(node->as.channel_new.buffer_size, name);
        case AST_SELECT_STMT:
            return xa_node_array_uses_symbol_name(node->as.select_stmt.cases,
                                                  node->as.select_stmt.case_count, name);
        case AST_SELECT_CASE:
            return xa_node_uses_symbol_name(node->as.select_case.channel, name) ||
                   xa_node_uses_symbol_name(node->as.select_case.value, name) ||
                   xa_node_uses_symbol_name(node->as.select_case.body, name);
        case AST_YIELD_STMT:
            return xa_node_uses_symbol_name(node->as.yield_stmt.value, name);
        case AST_DEFER_STMT:
            return xa_node_uses_symbol_name(node->as.defer_stmt.expr, name);
        case AST_SCOPE_BLOCK:
            return xa_node_uses_symbol_name(node->as.scope_block.body, name);
        case AST_MOVE_EXPR:
            return xa_node_uses_symbol_name(node->as.move_expr.expr, name);
        case AST_COMPTIME_EXPR:
            return xa_node_uses_symbol_name(node->as.comptime_expr.expr, name);
        case AST_UNSAFE_EXPR:
            return xa_node_uses_symbol_name(node->as.unsafe_expr.operand, name);
        case AST_PROGRAM:
            return xa_node_array_uses_symbol_name(node->as.program.statements,
                                                  node->as.program.count, name);

        default:
            return false;
    }
}

static bool xa_active_span_borrow_may_be_live_after_mutation(XaInferContext *ctx,
                                                             XaActiveSpanBorrow *borrow) {
    if (!ctx || !ctx->analyzer || !borrow || !borrow->view_symbol || !borrow->view_symbol->name)
        return true;
    if (ctx->loop_depth > 0)
        return true;
    const char *name = borrow->view_symbol->name;
    if (ctx->block_cursor_depth > 0) {
        int deepest = ctx->block_cursor_depth - 1;
        for (int depth = deepest; depth >= 0; depth--) {
            AstNode *block_node = ctx->block_cursor_nodes[depth];
            int stmt_index = ctx->block_cursor_indices[depth];
            AstNode **statements = NULL;
            int count = 0;
            if (!xa_block_node_statements(block_node, &statements, &count) || stmt_index < 0 ||
                stmt_index >= count)
                return true;
            if (depth == deepest && xa_node_uses_symbol_name(statements[stmt_index], name))
                return true;
            if (xa_block_uses_symbol_name_from(block_node, name, stmt_index + 1))
                return true;
        }
        return false;
    }

    AstNode *block_node = ctx->current_block_node;
    int stmt_index = ctx->current_block_stmt_index;
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(block_node, &statements, &count) || stmt_index < 0 ||
        stmt_index >= count)
        return true;
    if (xa_node_uses_symbol_name(statements[stmt_index], name))
        return true;
    return xa_block_uses_symbol_name_from(block_node, name, stmt_index + 1);
}

XR_FUNC void xa_visit_inline_statement_sequence_with_cursor(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;
    AstNode **statements = NULL;
    int count = 0;
    if (node->type == AST_BLOCK) {
        statements = node->as.block.statements;
        count = node->as.block.count;
    } else if (node->type == AST_PROGRAM) {
        statements = node->as.program.statements;
        count = node->as.program.count;
    } else {
        xa_visit_infer_stmt(ctx, node);
        return;
    }

    AstNode *saved_block = ctx->current_block_node;
    int saved_index = ctx->current_block_stmt_index;
    int saved_depth = ctx->block_cursor_depth;
    int cursor_slot = -1;
    if (ctx->block_cursor_depth < XA_BLOCK_CURSOR_MAX) {
        cursor_slot = ctx->block_cursor_depth++;
        ctx->block_cursor_nodes[cursor_slot] = node;
        ctx->block_cursor_indices[cursor_slot] = -1;
    }
    ctx->current_block_node = node;
    int error_count_before = xa_analyzer_error_diagnostic_count(ctx->analyzer);
    for (int i = 0; i < count; i++) {
        ctx->current_block_stmt_index = i;
        if (cursor_slot >= 0)
            ctx->block_cursor_indices[cursor_slot] = i;
        xa_visit_infer_stmt(ctx, statements[i]);
    }
    xa_warn_sys_thread_lifecycle_in_sequence(ctx, statements, count, error_count_before);
    ctx->block_cursor_depth = saved_depth;
    ctx->current_block_node = saved_block;
    ctx->current_block_stmt_index = saved_index;
}

XR_FUNC XaSymbol *xa_span_borrow_owner_receiver_symbol(XaInferContext *ctx, AstNode *expr,
                                                       XrType *receiver_type) {
    if (!ctx || !xa_type_can_own_span_view(receiver_type))
        return NULL;
    return xa_root_variable_symbol_for_expr(ctx, expr);
}

XR_FUNC void xa_clear_active_span_borrow_for_view(XaInferContext *ctx, XaSymbol *view_sym) {
    if (!ctx || !view_sym)
        return;
    XaActiveSpanBorrow **link = &ctx->active_span_borrows;
    while (*link) {
        XaActiveSpanBorrow *cur = *link;
        if (cur->view_symbol == view_sym) {
            *link = cur->next;
            xr_free(cur->owner_path);
            xr_free(cur);
            continue;
        }
        link = &cur->next;
    }
}

XR_FUNC void xa_clear_active_span_borrows_in_scope(XaInferContext *ctx, XaScope *scope) {
    if (!ctx || !scope)
        return;
    XaActiveSpanBorrow **link = &ctx->active_span_borrows;
    while (*link) {
        XaActiveSpanBorrow *cur = *link;
        if (cur->view_scope == scope) {
            *link = cur->next;
            xr_free(cur->owner_path);
            xr_free(cur);
            continue;
        }
        link = &cur->next;
    }
}

XR_FUNC XaSymbol *xa_span_borrow_owner_path_for_expr(XaInferContext *ctx, AstNode *expr,
                                                     char *path_buf, size_t path_buf_size) {
    if (!ctx || !expr)
        return NULL;
    char local_path[512];
    if (!path_buf || path_buf_size == 0) {
        path_buf = local_path;
        path_buf_size = sizeof(local_path);
    }
    path_buf[0] = '\0';

    XrType *expr_type = xa_analyzer_get_node_type(ctx->analyzer, expr);
    if (xa_type_can_own_span_view(expr_type) || xa_type_contains_span_view(expr_type)) {
        bool precise = true;
        XaSymbol *root = xa_root_path_for_expr(ctx, expr, path_buf, path_buf_size, &precise, true);
        if (root)
            return root;
    }
    return NULL;
}

XR_FUNC XaSymbol *xa_span_borrow_owner_path_for_owner_expr(XaInferContext *ctx, AstNode *expr,
                                                           char *path_buf, size_t path_buf_size) {
    if (!ctx || !expr)
        return NULL;
    char local_path[512];
    if (!path_buf || path_buf_size == 0) {
        path_buf = local_path;
        path_buf_size = sizeof(local_path);
    }
    path_buf[0] = '\0';

    XrType *expr_type = xa_analyzer_get_node_type(ctx->analyzer, expr);
    if (!xa_type_can_own_span_view(expr_type))
        return NULL;
    bool precise = true;
    return xa_root_path_for_expr(ctx, expr, path_buf, path_buf_size, &precise, false);
}

XR_FUNC XaSymbol *xa_span_borrow_owner_path_for_member_write(XaInferContext *ctx, AstNode *object,
                                                             const char *member,
                                                             XrType *member_type, char *path_buf,
                                                             size_t path_buf_size) {
    if (!ctx || !object || !member || !xa_type_can_own_span_view(member_type) || !path_buf ||
        path_buf_size == 0)
        return NULL;
    path_buf[0] = '\0';
    bool precise = true;
    XaSymbol *root = xa_root_path_for_expr(ctx, object, path_buf, path_buf_size, &precise, true);
    if (!root)
        return NULL;
    if (precise) {
        if (!xa_path_append(path_buf, path_buf_size, ".") ||
            !xa_path_append(path_buf, path_buf_size, member)) {
            xa_path_copy(path_buf, path_buf_size, root->name ? root->name : "");
        }
    } else {
        xa_path_copy(path_buf, path_buf_size, root->name ? root->name : "");
    }
    return root;
}

XR_FUNC XaSymbol *xa_span_borrow_owner_path_for_index_write(XaInferContext *ctx, AstNode *array,
                                                            AstNode *index, XrType *element_type,
                                                            char *path_buf, size_t path_buf_size) {
    if (!ctx || !array || !xa_type_can_own_span_view(element_type) || !path_buf ||
        path_buf_size == 0)
        return NULL;
    path_buf[0] = '\0';
    bool precise = true;
    XaSymbol *root = xa_root_path_for_expr(ctx, array, path_buf, path_buf_size, &precise, true);
    if (!root)
        return NULL;
    char segment[64];
    if (precise && xa_index_path_segment(index, segment, sizeof(segment))) {
        if (!xa_path_append(path_buf, path_buf_size, segment))
            xa_path_copy(path_buf, path_buf_size, root->name ? root->name : "");
    } else {
        xa_path_copy(path_buf, path_buf_size, root->name ? root->name : "");
    }
    return root;
}

XR_FUNC void xa_register_active_span_borrow(XaInferContext *ctx, XaSymbol *view_sym, AstNode *value,
                                            XrType *value_type) {
    if (!ctx || !view_sym)
        return;
    xa_clear_active_span_borrow_for_view(ctx, view_sym);
    if (!value || !xa_type_contains_span_view(value_type))
        return;
    char owner_path[512];
    XaSymbol *owner =
        xa_span_borrow_owner_path_for_expr(ctx, value, owner_path, sizeof(owner_path));
    if (!owner || owner == view_sym)
        return;
    XaActiveSpanBorrow *borrow = xr_calloc(1, sizeof(XaActiveSpanBorrow));
    if (!borrow)
        return;
    borrow->owner_symbol = owner;
    if (owner_path[0] != '\0')
        borrow->owner_path = xr_strdup(owner_path);
    borrow->view_symbol = view_sym;
    borrow->view_scope = view_sym->scope;
    borrow->next = ctx->active_span_borrows;
    ctx->active_span_borrows = borrow;
}

XR_FUNC void xa_check_active_span_borrow_owner_path_mutation(XaInferContext *ctx, AstNode *loc_node,
                                                             XaSymbol *owner_sym,
                                                             const char *owner_path,
                                                             const char *operation) {
    if (!ctx || !ctx->analyzer || !owner_sym || !loc_node)
        return;
    for (XaActiveSpanBorrow *b = ctx->active_span_borrows; b; b = b->next) {
        if (b->owner_symbol != owner_sym)
            continue;
        if (!xa_owner_paths_may_overlap(b->owner_path, owner_path))
            continue;
        if (!xa_active_span_borrow_may_be_live_after_mutation(ctx, b))
            continue;
        XrLocation loc = {
            .file = ctx->file_path, .line = loc_node->line, .column = loc_node->column};
        char msg[256];
        snprintf(
            msg, sizeof(msg),
            "cannot mutate owner '%s' while Span view '%s' is active; end the view scope before %s",
            owner_sym->name ? owner_sym->name : "?",
            b->view_symbol && b->view_symbol->name ? b->view_symbol->name : "?",
            operation ? operation : "mutating the owner");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   msg, &loc);
        return;
    }
}

XR_FUNC void xa_check_active_span_borrow_owner_mutation(XaInferContext *ctx, AstNode *loc_node,
                                                        XaSymbol *owner_sym,
                                                        const char *operation) {
    const char *owner_path = owner_sym && owner_sym->name ? owner_sym->name : NULL;
    xa_check_active_span_borrow_owner_path_mutation(ctx, loc_node, owner_sym, owner_path,
                                                    operation);
}

XR_FUNC void xa_check_span_value_escape(XaInferContext *ctx, AstNode *loc_node, XrType *value_type,
                                        const char *escape_context) {
    if (!ctx || !ctx->analyzer || !loc_node || !xa_type_contains_span_view(value_type))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = loc_node->line, .column = loc_node->column};
    char msg[256];
    snprintf(msg, sizeof(msg),
             "cannot %s; Span is a borrowed view, keep it local or copy the owner data into an "
             "Array",
             escape_context ? escape_context : "var Span view escape");
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

XR_FUNC void xa_check_span_borrow_source_stable(XaInferContext *ctx, AstNode *loc_node,
                                                AstNode *source, const char *operation) {
    if (!ctx || !ctx->analyzer || !loc_node || !source || xa_expr_has_stable_borrow_owner(source))
        return;

    XrLocation loc = {.file = ctx->file_path, .line = loc_node->line, .column = loc_node->column};
    char msg[256];
    snprintf(msg, sizeof(msg),
             "cannot create Span view from temporary owner in %s; bind the owner to a local before "
             "borrowing it",
             operation ? operation : "borrow expression");
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static void xa_update_borrowed_alias_root(XaInferContext *ctx, XaSymbol *sym, AstNode *value,
                                          XrType *value_type) {
    if (!sym || sym->kind != XA_SYM_VARIABLE)
        return;
    sym->borrowed_root_symbol_id = 0;
    if (!value || !xa_type_needs_borrow_escape_guard(value_type))
        return;
    XaSymbol *root = xa_borrowed_param_root_symbol(ctx, value);
    if (root)
        sym->borrowed_root_symbol_id = root->id;
}

static void xa_check_borrowed_return_escape(XaInferContext *ctx, AstNode *return_node,
                                            AstNode *value, XrType *value_type) {
    if (!ctx || !return_node || !value || !xa_type_needs_borrow_escape_guard(value_type))
        return;
    XaSymbol *root = xa_borrowed_param_root_symbol(ctx, value);
    if (!root)
        return;

    XrLocation loc = {.file = ctx->file_path,
                      .line = value->line ? value->line : return_node->line,
                      .column = value->column ? value->column : return_node->column};
    const char *mode = root->passing_mode == XR_PARAM_REF ? "ref" : "in";
    char msg[256];
    snprintf(msg, sizeof(msg),
             "cannot return borrowed '%s' parameter '%s'; return an owned value or copy(%s)", mode,
             root->name ? root->name : "?", root->name ? root->name : "?");
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static void xa_check_span_return_escape(XaInferContext *ctx, AstNode *return_node,
                                        XrType *value_type) {
    if (!ctx || !return_node || !xa_type_contains_span_view(value_type))
        return;

    XrLocation loc = {
        .file = ctx->file_path, .line = return_node->line, .column = return_node->column};
    xa_analyzer_add_diagnostic(
        ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
        "cannot return Span view; return the owner container or copy the view into an Array", &loc);
}

static bool xa_call_is_copy_builtin(AstNode *node) {
    if (!node || node->type != AST_CALL_EXPR)
        return false;
    CallExprNode *call = &node->as.call_expr;
    return call->callee && call->callee->type == AST_VARIABLE && call->callee->as.variable.name &&
           strcmp(call->callee->as.variable.name, "copy") == 0 && call->arg_count == 1;
}

static AstNode *xa_shared_boundary_source(AstNode *init, bool *is_move) {
    if (is_move)
        *is_move = false;
    if (!init || xa_call_is_copy_builtin(init))
        return NULL;
    if (init->type == AST_MOVE_EXPR) {
        if (is_move)
            *is_move = true;
        AstNode *inner = init->as.move_expr.expr;
        return (inner && inner->type == AST_VARIABLE) ? inner : NULL;
    }
    return init->type == AST_VARIABLE ? init : NULL;
}

static XaSymbol *xa_lookup_shared_source_symbol(XaInferContext *ctx, AstNode *source) {
    if (!ctx || !source || source->type != AST_VARIABLE || !source->as.variable.name)
        return NULL;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, source->as.variable.name);
    if (!sym && ctx->analyzer->global_scope)
        sym = xa_scope_lookup(ctx->analyzer->global_scope, source->as.variable.name);
    return sym;
}

static void xa_check_shared_initializer_boundary(XaInferContext *ctx, AstNode *decl_node,
                                                 XrType *init_type) {
    if (!ctx || !decl_node)
        return;
    VarDeclNode *var = &decl_node->as.var_decl;
    if (decl_node->type != AST_SHARED_DECL || !var->initializer)
        return;

    bool is_move = false;
    AstNode *source = xa_shared_boundary_source(var->initializer, &is_move);
    if (!source)
        return;

    XaSymbol *src_sym = xa_lookup_shared_source_symbol(ctx, source);
    if (!src_sym || src_sym->kind != XA_SYM_VARIABLE)
        return;

    XrLocation loc = {
        .file = ctx->file_path, .line = var->initializer->line, .column = var->initializer->column};
    const char *src_name = source->as.variable.name ? source->as.variable.name : "?";

    if (src_sym->is_shared) {
        if (!is_move)
            return;
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "shared binding '%s' is already a shared identity and must not be moved",
                 src_name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   msg, &loc);
        return;
    }

    if (!xa_type_needs_borrow_escape_guard(init_type))
        return;

    char msg[256];
    if (is_move) {
        snprintf(msg, sizeof(msg),
                 "move cannot promote local reference value '%s' into a shared binding; "
                 "use copy(%s)",
                 src_name, src_name);
    } else {
        snprintf(msg, sizeof(msg),
                 "shared binding from local reference value '%s' requires copy(%s)", src_name,
                 src_name);
    }
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

static void xa_check_const_initializer_alias_boundary(XaInferContext *ctx, AstNode *decl_node,
                                                      XrType *init_type) {
    if (!ctx || !decl_node || decl_node->type != AST_CONST_DECL)
        return;

    VarDeclNode *var = &decl_node->as.var_decl;
    if (!var->initializer || xa_call_is_copy_builtin(var->initializer))
        return;
    if (!xa_type_needs_borrow_escape_guard(init_type) || xr_type_is_const(init_type))
        return;

    XaSymbol *root = xa_root_variable_symbol_for_expr(ctx, var->initializer);
    if (!root || root->kind != XA_SYM_VARIABLE || root->is_readonly_binding || root->is_shared)
        return;

    XrLocation loc = {
        .file = ctx->file_path,
        .line = var->initializer->line ? var->initializer->line : decl_node->line,
        .column = var->initializer->column ? var->initializer->column : decl_node->column,
    };
    const char *name = root->name ? root->name : "?";
    char msg[256];
    snprintf(msg, sizeof(msg), "const binding from mutable reference value '%s' requires copy(%s)",
             name, name);
    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH, msg,
                               &loc);
}

/* ============================================================================
 * Pass 2: Statement Visitors
 * ============================================================================
 * Type inference for statements: variable declarations, assignments,
 * control flow (if/while/for), and return statements.
 * ========================================================================== */

void xa_visit_var_decl_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    VarDeclNode *var = &node->as.var_decl;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, var->name);
    if (!sym) {
        // Symbol not found (Pass 1 missed this declaration, e.g. inside for/while/if)
        // Define it now so type inference can proceed
        xa_visit_collect_var_decl(ctx, node);
        sym = xa_scope_lookup(ctx->analyzer->current_scope, var->name);
        if (!sym)
            return;
    }

    /* Ensure symbol_id is set (covers late-discovered declarations). */
    if (var->symbol_id == 0)
        var->symbol_id = sym->id;

    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (links) {
        links->const_initializer = sym->is_const ? var->initializer : NULL;
        links->has_ct_value = false;
        links->ct_value = (XrCtValue) {0};
        links->is_comptime_local = ctx->comptime_block_depth > 0;
    }
    if (xa_freestanding_profile_enabled(ctx->analyzer) && node->type == AST_SHARED_DECL) {
        xa_freestanding_report_unavailable(
            ctx, node, "shared declaration",
            "shared storage still requires module initialization; only explicit const data "
            "static objects are supported in the current freestanding slice");
    } else if (xa_freestanding_profile_enabled(ctx->analyzer) &&
               xa_is_module_level_scope(ctx->analyzer) &&
               !(node->type == AST_CONST_DECL && xa_freestanding_top_const_allowed(ctx, var))) {
        xa_freestanding_report_unavailable(
            ctx, node,
            node->type == AST_CONST_DECL ? "top-level const declaration"
                                         : "top-level var declaration",
            node->type == AST_CONST_DECL
                ? "only int/float/bool/char/string/null consteval scalars and recursively scalar "
                  "fixed-array/tuple/struct initializers are allowed as erased or static data "
                  "objects in the current freestanding slice"
                : "module storage still requires constructor initialization; move mutable state "
                  "inside functions until freestanding global storage has explicit static "
                  "object lowering");
    }

    // Variable declarations must have a type annotation or initializer.
    if (!var->initializer && !links->declared_type) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[256];
        snprintf(msg, sizeof(msg), "Variable '%s' must have a type annotation or initializer",
                 var->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_MISSING_TYPE,
                                   msg, &loc);
    }

    // Infer type from initializer if no declared type
    XrType *var_type = NULL;
    if (var->initializer) {
        // Set expected_type for bidirectional inference
        XrType *saved_expected = ctx->expected_type;
        if (links->declared_type && !XR_TYPE_IS_UNKNOWN(links->declared_type)) {
            ctx->expected_type = links->declared_type;
        }
        uint32_t saved_initializing_symbol_id = ctx->initializing_symbol_id;
        ctx->initializing_symbol_id = sym->id;
        XrType *init_type = xa_visit_infer_expr(ctx, var->initializer);
        ctx->initializing_symbol_id = saved_initializing_symbol_id;
        ctx->expected_type = saved_expected;

        // Store inferred initializer type in the analyzer side table
        // (the canonical source for downstream codegen / LSP).
        xa_analyzer_set_node_type(ctx->analyzer, var->initializer, init_type);
        xa_check_shared_initializer_boundary(ctx, node, init_type);
        xa_check_const_initializer_alias_boundary(ctx, node, init_type);

        if (links->declared_type && !XR_TYPE_IS_UNKNOWN(links->declared_type)) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            // Check null safety first (null→T, T?→T)
            bool null_err = xa_check_null_safety(ctx->analyzer, links->declared_type, init_type,
                                                 "Variable initializer", &loc);
            // Check assignment compatibility
            bool type_mismatch = false;
            if (XR_TYPE_IS_FUNCTION(links->declared_type)) {
                type_mismatch = xa_function_assignment_mismatch(links->declared_type, init_type);
            } else {
                type_mismatch = !xa_typecheck_assignable(links->declared_type, init_type);
            }
            if (!null_err && type_mismatch) {
                // Json→concrete type: allowed at compile time, runtime check inserted by
                // codegen. e.g. var x: int = data["key"] is legal but requires runtime validation.
                if (XR_TYPE_IS_FUNCTION(links->declared_type) ||
                    !xr_is_json_coercion(links->declared_type, init_type)) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Type '%s' is not assignable to type '%s'",
                             xr_type_to_string(init_type), xr_type_to_string(links->declared_type));
                    xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                               XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
                }
            }
            var_type = links->declared_type;
        } else {
            var_type = init_type;
            // Empty array literal without type annotation: require explicit type
            if (var->initializer && var->initializer->type == AST_ARRAY_LITERAL &&
                var->initializer->as.array_literal.count == 0 && XR_TYPE_IS_ARRAY(init_type) &&
                init_type->container.element_type &&
                XR_TYPE_IS_UNKNOWN(init_type->container.element_type)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                xa_analyzer_add_diagnostic(
                    ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                    "Empty array '[]' requires a type annotation, e.g. var x: Array<int> = []",
                    &loc);
            }
        }
    } else if (links->declared_type) {
        var_type = links->declared_type;
        /* Reject non-default-initializable types without explicit initializer.
         * e.g. `var u: User` is a compile error, but `var x: int` is allowed. */
        if (!XR_TYPE_IS_UNKNOWN(links->declared_type) &&
            !xa_type_is_default_initializable(ctx, links->declared_type)) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Type '%s' is not default-initializable; "
                     "variable '%s' requires an explicit initializer",
                     xr_type_to_string(links->declared_type), var->name);
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
        }
    } else {
        // Fallback for missing type (error already reported above)
        var_type = xr_type_new_unknown(NULL);
    }

    if (sym->is_readonly_binding && var_type)
        var_type = xr_type_make_const(ctx->analyzer->isolate, var_type);

    links->type = var_type;
    xa_update_borrowed_alias_root(ctx, sym, var->initializer, var_type);
    xa_register_active_span_borrow(ctx, sym, var->initializer, var_type);

    if (var_type && xa_type_is_concurrency_handle(var_type)) {
        if (!sym->is_shared) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            const char *label = xa_concurrency_handle_label(var_type);
            char msg[160];
            snprintf(msg, sizeof(msg), "%s handle must be declared with 'shared'",
                     label ? label : "synchronization");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
        }
    }

    // Track definite assignment
    // Variables with type annotations are initialized to the type zero value.
    links->is_definitely_assigned = (var->initializer != NULL) || (links->declared_type != NULL);

    // A const with a proven compile-time initializer, or any comptime-block
    // local binding, can be reused by later consteval expressions.
    if ((sym->is_const || links->is_comptime_local) && var->initializer) {
        XrCtValue value = {0};
        const char *err = NULL;
        if (xa_consteval_expr(ctx->analyzer, var->initializer, &value, &err)) {
            links->has_ct_value = true;
            links->ct_value = value;
            links->is_const_foldable = true;
        } else if (links->is_comptime_local) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            char msg[256];
            snprintf(
                msg, sizeof(msg),
                "comptime block local binding initializer must be evaluable at compile time%s%s",
                err ? ": " : "", err ? err : "");
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                       XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
        }
    } else if (links->is_comptime_local && !var->initializer) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   "comptime block local binding requires an initializer", &loc);
    }
    xa_validate_const_static_data_attrs(ctx, node, var, links, var_type);
    links->assign_count = var->initializer ? 1 : 0;

    // Detect loop variable context
    XaScope *s = ctx->analyzer->current_scope;
    while (s) {
        if (s->kind == XA_SCOPE_LOOP) {
            links->is_loop_variable = true;
            break;
        }
        s = s->parent;
    }

    // Store the inferred type in the analyzer side table for codegen.
    xa_analyzer_set_node_type(ctx->analyzer, node, var_type);

    // Create assignment flow node
    if (ctx->flow) {
        xa_flow_create_assignment(ctx->flow, NULL, var->name, var_type);
    }
}

void xa_visit_assignment_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    AssignmentNode *assign = &node->as.assignment;
    XaSymbol *sym = xa_scope_lookup(ctx->analyzer->current_scope, assign->name);

    if (!sym) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[128];
        snprintf(msg, sizeof(msg), "Undeclared variable '%s'", assign->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_UNDEFINED_VAR,
                                   msg, &loc);
        return;
    }

    /* Write back resolved symbol ID for Xi lowering (Braun SSA key). */
    assign->symbol_id = sym->id;
    xa_parallel_capture_check(ctx, node, sym, true);

    // Record write reference for Find References
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, sym);
    if (links) {
        uint32_t end_col = node->column + (assign->name ? strlen(assign->name) : 0);
        xa_symbol_add_ref(links, node->line, node->column, end_col, true);  // is_write=true
    }

    // Check immutable binding assignment
    if (sym->is_const || sym->is_shared || !sym->is_rebindable) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[128];
        snprintf(msg, sizeof(msg),
                 sym->is_shared ? "Cannot assign to shared binding '%s'"
                                : "Cannot assign to const '%s'",
                 assign->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONST_ASSIGN,
                                   msg, &loc);
        return;
    }

    // Check in-parameter immutability: cannot reassign an 'in' parameter
    if (sym->kind == XA_SYM_PARAMETER && sym->passing_mode == XR_PARAM_IN) {
        XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
        char msg[128];
        snprintf(msg, sizeof(msg), "Cannot assign to 'in' parameter '%s' (readonly reference)",
                 assign->name);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_CONST_ASSIGN,
                                   msg, &loc);
        return;
    }

    XrType *var_type = xa_analyzer_get_type(ctx->analyzer, sym);
    xa_check_active_span_borrow_owner_mutation(ctx, node, sym, "reassigning the owner");

    // Bidirectional inference: propagate target type to value expression
    XrType *saved_expected = ctx->expected_type;
    if (var_type && !XR_TYPE_IS_UNKNOWN(var_type)) {
        ctx->expected_type = var_type;
    }
    XrType *value_type = xa_visit_infer_expr(ctx, assign->value);
    ctx->expected_type = saved_expected;

    // Mark as definitely assigned.
    if (links) {
        links->is_definitely_assigned = true;
        links->assign_count++;
    }
    xa_update_borrowed_alias_root(ctx, sym, assign->value, value_type);
    xa_register_active_span_borrow(ctx, sym, assign->value, value_type);

    xa_assign_check_type(ctx, node, var_type, value_type, assign->name, NULL);

    // Update flow graph — but only if value type is known.
    // Recording unknown would downgrade a variable from its declared type.
    if (ctx->flow && value_type && !XR_TYPE_IS_UNKNOWN(value_type)) {
        xa_flow_create_assignment(ctx->flow, NULL, assign->name, value_type);
    }
}

void xa_visit_if_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    IfStmtNode *if_stmt = &node->as.if_stmt;

    // Analyze condition
    XrType *cond_type = xa_visit_infer_expr(ctx, if_stmt->condition);
    xa_check_condition_type(ctx, if_stmt->condition, cond_type);

    // Flow graph handles all type narrowing via TRUE_CONDITION / FALSE_CONDITION
    // nodes. apply_condition_narrowing() in xanalyzer_flow.c recognizes patterns:
    //   x != null, typeof(x) == Type.xxx, x is Type, truthiness, &&, ||
    // Early-return narrowing is automatic: when then-branch terminates,
    // its flow becomes unreachable → merge label only has the false-condition
    // path → opposite narrowing applies to subsequent code.

    XaFlowNode *saved = ctx->flow ? ctx->flow->current_flow : NULL;

    // Then branch: flow enters TRUE_CONDITION
    if (ctx->flow) {
        ctx->flow->current_flow = xa_flow_create_condition(ctx->flow, if_stmt->condition, true);
    }
    xa_visit_infer_stmt(ctx, if_stmt->then_branch);
    XaFlowNode *then_end = ctx->flow ? ctx->flow->current_flow : NULL;

    // Else branch: flow enters FALSE_CONDITION
    if (ctx->flow)
        ctx->flow->current_flow = saved;

    XaFlowNode *else_end = NULL;
    if (if_stmt->else_branch) {
        if (ctx->flow) {
            ctx->flow->current_flow =
                xa_flow_create_condition(ctx->flow, if_stmt->condition, false);
        }
        xa_visit_infer_stmt(ctx, if_stmt->else_branch);
        else_end = ctx->flow ? ctx->flow->current_flow : NULL;
    }

    // Merge branches
    if (ctx->flow) {
        XaFlowNode *merge = xa_flow_create_branch_label(ctx->flow);
        if (then_end)
            xa_flow_add_antecedent(merge, then_end);
        if (else_end) {
            xa_flow_add_antecedent(merge, else_end);
        } else {
            // No else: false-condition path flows through to merge
            ctx->flow->current_flow = saved;
            XaFlowNode *false_cond = xa_flow_create_condition(ctx->flow, if_stmt->condition, false);
            xa_flow_add_antecedent(merge, false_cond);
        }
        ctx->flow->current_flow = xa_flow_finish_label(ctx->flow, merge);
    }
}

void xa_visit_while_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    WhileStmtNode *while_stmt = &node->as.while_stmt;

    // Create loop label
    XaFlowNode *loop_start = NULL;
    if (ctx->flow) {
        loop_start = xa_flow_create_loop_label(ctx->flow);
    }

    // Analyze condition
    XrType *cond_type = xa_visit_infer_expr(ctx, while_stmt->condition);
    xa_check_condition_type(ctx, while_stmt->condition, cond_type);

    if (ctx->flow) {
        xa_flow_create_condition(ctx->flow, while_stmt->condition, true);
    }

    /* Analyze body. A block body goes through xa_visit_block_stmt so it
     * gets its own scope keyed on the body node, matching Pass 1. */
    XaLoopScope loop_scope;
    xa_loop_scope_push(ctx, &loop_scope, while_stmt->label, node);
    if (while_stmt->body)
        xa_visit_infer_stmt(ctx, while_stmt->body);
    xa_loop_scope_pop(ctx, &loop_scope);

    // Back edge to loop start
    if (ctx->flow && loop_start) {
        xa_flow_add_antecedent(loop_start, ctx->flow->current_flow);
    }

    // Exit condition
    if (ctx->flow) {
        xa_flow_create_condition(ctx->flow, while_stmt->condition, false);
    }
}

void xa_visit_for_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    ForStmtNode *for_stmt = &node->as.for_stmt;

    // Enter loop scope
    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);

    // Analyze initializer
    if (for_stmt->initializer) {
        xa_visit_infer_stmt(ctx, for_stmt->initializer);
    }

    // Create loop label
    if (ctx->flow) {
        xa_flow_create_loop_label(ctx->flow);
    }

    // Analyze condition
    if (for_stmt->condition) {
        XrType *cond_type = xa_visit_infer_expr(ctx, for_stmt->condition);
        xa_check_condition_type(ctx, for_stmt->condition, cond_type);
    }

    // Analyze body - inline block to match Pass 1 scope structure
    XaLoopScope loop_scope;
    xa_loop_scope_push(ctx, &loop_scope, for_stmt->label, node);
    if (for_stmt->body) {
        if (for_stmt->body->type == AST_BLOCK) {
            BlockNode *blk = &for_stmt->body->as.block;
            for (int si = 0; si < blk->count; si++) {
                xa_visit_infer_stmt(ctx, blk->statements[si]);
            }
        } else {
            xa_visit_infer_stmt(ctx, for_stmt->body);
        }
    }
    xa_loop_scope_pop(ctx, &loop_scope);

    // Analyze increment
    if (for_stmt->increment) {
        xa_visit_infer_stmt(ctx, for_stmt->increment);
    }

    xa_clear_active_span_borrows_in_scope(ctx, ctx->analyzer->current_scope);
    xa_analyzer_exit_scope(ctx->analyzer);
}

void xa_visit_return_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    ReturnStmtNode *ret = &node->as.return_stmt;

    /* Top-level return is illegal — must be inside a function body. */
    {
        XaScope *s = ctx->analyzer->current_scope;
        while (s && s->kind != XA_SCOPE_FUNCTION)
            s = s->parent;
        if (!s) {
            XrLocation loc = {.file = ctx->file_path, .line = node->line, .column = node->column};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_CMP_INVALID_RETURN,
                                       "'return' outside of a function body", &loc);
            return;
        }
    }

    XrType *return_type = xr_type_new_unit(NULL);

    if (ret->value_count == 0) {
        // No return value
        return_type = xr_type_new_unit(NULL);
    } else if (ret->value_count == 1) {
        // Single return value
        if (ret->values[0]) {
            // Bidirectional inference: propagate declared return type to return expr
            // (e.g., return (x) => x + 1 inside fn(): fn(int): int)
            XrType *saved_expected = ctx->expected_type;
            if (ctx->expected_return_type && !XR_TYPE_IS_UNKNOWN(ctx->expected_return_type)) {
                ctx->expected_type = ctx->expected_return_type;
            } else {
                // Look up enclosing function's declared return type from scope
                XaScope *s = ctx->analyzer->current_scope;
                while (s && s->kind != XA_SCOPE_FUNCTION)
                    s = s->parent;
                if (s && s->ast_node) {
                    AstNode *fn_node = (AstNode *) s->ast_node;
                    XrType *decl_ret = NULL;
                    if (fn_node->type == AST_FUNCTION_DECL &&
                        fn_node->as.function_decl.return_type) {
                        decl_ret = xr_tref_resolve(ctx->analyzer->isolate,
                                                   fn_node->as.function_decl.return_type);
                    } else if (fn_node->type == AST_METHOD_DECL &&
                               fn_node->as.method_decl.return_type) {
                        decl_ret = xr_tref_resolve(ctx->analyzer->isolate,
                                                   fn_node->as.method_decl.return_type);
                    }
                    if (decl_ret && !XR_TYPE_IS_UNKNOWN(decl_ret)) {
                        ctx->expected_type = decl_ret;
                    }
                }
            }
            return_type = xa_visit_infer_expr(ctx, ret->values[0]);
            ctx->expected_type = saved_expected;
            xa_check_borrowed_return_escape(ctx, node, ret->values[0], return_type);
            xa_check_span_return_escape(ctx, node, return_type);
        }
    } else {
        // Legacy AST multi-expression return is treated as a tuple type.
        XrType **element_types = xr_malloc(sizeof(XrType *) * ret->value_count);
        for (int i = 0; i < ret->value_count; i++) {
            if (ret->values[i]) {
                element_types[i] = xa_visit_infer_expr(ctx, ret->values[i]);
                xa_check_borrowed_return_escape(ctx, node, ret->values[i], element_types[i]);
            } else {
                element_types[i] = xr_type_new_unknown(NULL);
            }
        }
        // Create tuple type for the legacy AST shape.
        return_type = xr_type_new_tuple(ctx->analyzer->isolate, element_types, ret->value_count);

        // Store return type info in the analyzer side table.
        xa_analyzer_set_node_type(ctx->analyzer, node, return_type);
        xa_check_span_return_escape(ctx, node, return_type);

        xr_free(element_types);
    }

    // Collect return type for function inference
    xa_infer_add_return_type(ctx, return_type);

    // Check against expected return type (strict: Unit and concrete types enforced)
    if (ctx->expected_return_type && !XR_TYPE_IS_UNKNOWN(ctx->expected_return_type)) {
        if (!xa_typecheck_assignable(ctx->expected_return_type, return_type)) {
            // Json→primitive/union: allowed with runtime type check (OP_CHECKTYPE)
            if (!xr_is_json_coercion(ctx->expected_return_type, return_type)) {
                XrLocation loc = {
                    .file = ctx->file_path, .line = node->line, .column = node->column};
                char msg[256];
                snprintf(msg, sizeof(msg), "Return type mismatch: expected '%s', got '%s'",
                         xr_type_to_string(ctx->expected_return_type),
                         xr_type_to_string(return_type));
                xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                           XR_ERR_ANALYZE_TYPE_MISMATCH, msg, &loc);
            }
        }
    }

    // Mark flow as unreachable after return
    if (ctx->flow) {
        ctx->flow->current_flow = ctx->flow->unreachable_flow;
    }
}

void xa_visit_block_stmt(XaInferContext *ctx, AstNode *node) {
    if (!ctx || !node)
        return;

    xa_analyzer_enter_scope(ctx->analyzer, XA_SCOPE_BLOCK, node);

    xa_visit_inline_statement_sequence_with_cursor(ctx, node);

    xa_clear_active_span_borrows_in_scope(ctx, ctx->analyzer->current_scope);
    xa_analyzer_exit_scope(ctx->analyzer);
}
