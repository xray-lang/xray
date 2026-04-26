/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_rename.c - Semantic rename support (extracted from xlsp_analysis.c)
 */

#include "xlsp_rename.h"
#include "xlsp_analysis.h"
#include "../../base/xjson.h"
#include "xlsp_utils.h"
#include "xlsp_keywords.h"
#include "../../base/xmalloc.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include <string.h>

// ============================================================================
// Rename (Semantic-aware)
// ============================================================================

// Rename context - uses XaScope from analyzer for unified scope handling
typedef struct {
    const char *target_name;  // Name to find/rename
    int target_name_len;      // Length of target name
    int target_line;          // Line of cursor position (1-indexed)
    int target_col;           // Column of cursor position (1-indexed)
    XaScope *global_scope;    // Global scope from analyzer
    XaScope *def_scope;       // Scope where symbol is defined
    XaScope *current_scope;   // Current scope during traversal
    bool found_def;           // Whether we found the definition
    bool is_global;           // Whether the symbol is global
    XrJsonValue *edits;       // Collected edit locations
    const char *new_name;     // New name for replacement
} RenameContext;

// Forward declarations
static void find_symbol_definition(AstNode *node, RenameContext *ctx);
static void collect_rename_locations(AstNode *node, RenameContext *ctx);

// Add a text edit for renaming
static void add_rename_edit(RenameContext *ctx, int line, int col, int len);

// Helper: check if cursor is on this identifier (line and column match)
// Returns true if the cursor position matches this identifier location
static bool is_cursor_on_identifier(RenameContext *ctx, int node_line, int node_col, int name_len) {
    if (node_line != ctx->target_line)
        return false;

    // If node column is 0 (unknown), only match by line
    if (node_col <= 0)
        return true;

    // Check if cursor column is within the identifier range
    // target_col is 1-indexed, node_col is 1-indexed
    return ctx->target_col >= node_col && ctx->target_col < node_col + name_len;
}

// Helper: determine expected scope kind from AST node type
static XaScopeKind get_expected_scope_kind(AstNode *node) {
    if (!node)
        return XA_SCOPE_BLOCK;
    switch (node->type) {
        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            return XA_SCOPE_FUNCTION;
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
            return XA_SCOPE_CLASS;
        case AST_FOR_STMT:
        case AST_FOR_IN_STMT:
            return XA_SCOPE_LOOP;
        case AST_BLOCK:
        default:
            return XA_SCOPE_BLOCK;
    }
}

// Helper: find child scope by AST node with fallback strategies
// Priority: 1) exact pointer match, 2) scope kind + position heuristic
static XaScope *find_child_scope_xa(XaScope *parent, void *ast_node) {
    if (!parent)
        return NULL;

    AstNode *node = (AstNode *) ast_node;

    // Strategy 1: exact AST node pointer match (preferred)
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i]->ast_node == ast_node) {
            return parent->children[i];
        }
    }

    // Strategy 2: fallback when AST pointers don't match (doc re-parsed)
    // Find a child scope with matching kind that could correspond to this node
    if (node && parent->child_count > 0) {
        XaScopeKind expected_kind = get_expected_scope_kind(node);

        // For functions, try to match by name if available
        if (expected_kind == XA_SCOPE_FUNCTION) {
            const char *fn_name = NULL;
            if (node->type == AST_FUNCTION_DECL) {
                fn_name = node->as.function_decl.name;
            }

            if (fn_name) {
                // Look for a function scope that defines this name
                for (int i = 0; i < parent->child_count; i++) {
                    XaScope *child = parent->children[i];
                    if (child->kind == XA_SCOPE_FUNCTION) {
                        // Check if this scope or parent defines the function name
                        XaSymbol *sym = xa_scope_lookup_local(parent, fn_name);
                        if (sym && sym->kind == XA_SYM_FUNCTION) {
                            // This is likely the correct scope
                            return child;
                        }
                    }
                }
            }
        }

        // Generic fallback: return first child scope of expected kind
        // This is a last resort and may not always be correct
        for (int i = 0; i < parent->child_count; i++) {
            if (parent->children[i]->kind == expected_kind) {
                return parent->children[i];
            }
        }
    }

    return NULL;
}

// Phase 1: Find the scope where the symbol at cursor is defined
// Uses XaScope from analyzer for unified scope handling
static void find_symbol_definition(AstNode *node, RenameContext *ctx) {
    if (!node || ctx->found_def)
        return;

    switch (node->type) {
        case AST_PROGRAM:
            ctx->current_scope = ctx->global_scope;
            for (int i = 0; i < node->as.program.count; i++) {
                find_symbol_definition(node->as.program.statements[i], ctx);
            }
            break;

        case AST_FUNCTION_DECL: {
            FunctionDeclNode *fn = &node->as.function_decl;
            // Check if cursor is on function name (defined in parent scope)
            if (fn->name && strcmp(fn->name, ctx->target_name) == 0 &&
                is_cursor_on_identifier(ctx, node->line, node->column, (int) strlen(fn->name))) {
                ctx->def_scope = ctx->current_scope;
                ctx->is_global = (ctx->current_scope == ctx->global_scope);
                ctx->found_def = true;
                return;
            }

            // Find function's scope
            XaScope *saved_scope = ctx->current_scope;
            XaScope *fn_scope = find_child_scope_xa(ctx->current_scope, node);
            if (fn_scope)
                ctx->current_scope = fn_scope;

            // Check parameters
            for (int i = 0; i < fn->param_count; i++) {
                XrParamNode *param = fn->params[i];
                if (param && param->name && strcmp(param->name, ctx->target_name) == 0 &&
                    is_cursor_on_identifier(ctx, param->line, param->column,
                                            (int) strlen(param->name))) {
                    ctx->def_scope = ctx->current_scope;
                    ctx->is_global = false;
                    ctx->found_def = true;
                    ctx->current_scope = saved_scope;
                    return;
                }
            }

            find_symbol_definition(fn->body, ctx);
            ctx->current_scope = saved_scope;
            break;
        }

        case AST_VAR_DECL:
        case AST_CONST_DECL: {
            VarDeclNode *var = &node->as.var_decl;
            if (var->name && strcmp(var->name, ctx->target_name) == 0 &&
                is_cursor_on_identifier(ctx, node->line, node->column, (int) strlen(var->name))) {
                ctx->def_scope = ctx->current_scope;
                ctx->is_global = (ctx->current_scope == ctx->global_scope);
                ctx->found_def = true;
                return;
            }
            find_symbol_definition(var->initializer, ctx);
            break;
        }

        case AST_VARIABLE: {
            const char *var_name = node->as.variable.name;
            if (var_name && strcmp(var_name, ctx->target_name) == 0 &&
                is_cursor_on_identifier(ctx, node->line, node->column, (int) strlen(var_name))) {
                // Use XaScope API to find definition scope
                ctx->def_scope = xa_scope_find_definition(ctx->current_scope, ctx->target_name);
                if (!ctx->def_scope) {
                    ctx->def_scope = ctx->global_scope;
                }
                ctx->is_global = (ctx->def_scope == ctx->global_scope);
                ctx->found_def = true;
                return;
            }
            break;
        }

        case AST_BLOCK: {
            XaScope *saved_scope = ctx->current_scope;
            XaScope *block_scope = find_child_scope_xa(ctx->current_scope, node);
            if (block_scope)
                ctx->current_scope = block_scope;

            for (int i = 0; i < node->as.block.count; i++) {
                find_symbol_definition(node->as.block.statements[i], ctx);
            }
            ctx->current_scope = saved_scope;
            break;
        }

        case AST_IF_STMT:
            find_symbol_definition(node->as.if_stmt.condition, ctx);
            find_symbol_definition(node->as.if_stmt.then_branch, ctx);
            find_symbol_definition(node->as.if_stmt.else_branch, ctx);
            break;

        case AST_WHILE_STMT:
            find_symbol_definition(node->as.while_stmt.condition, ctx);
            find_symbol_definition(node->as.while_stmt.body, ctx);
            break;

        case AST_FOR_STMT: {
            XaScope *saved_scope = ctx->current_scope;
            XaScope *for_scope = find_child_scope_xa(ctx->current_scope, node);
            if (for_scope)
                ctx->current_scope = for_scope;

            find_symbol_definition(node->as.for_stmt.initializer, ctx);
            find_symbol_definition(node->as.for_stmt.condition, ctx);
            find_symbol_definition(node->as.for_stmt.increment, ctx);
            find_symbol_definition(node->as.for_stmt.body, ctx);
            ctx->current_scope = saved_scope;
            break;
        }

        case AST_FOR_IN_STMT: {
            ForInStmtNode *fi = &node->as.for_in_stmt;
            find_symbol_definition(fi->collection, ctx);

            XaScope *saved_scope = ctx->current_scope;
            XaScope *for_scope = find_child_scope_xa(ctx->current_scope, node);
            if (for_scope)
                ctx->current_scope = for_scope;

            if (fi->item_name && strcmp(fi->item_name, ctx->target_name) == 0 &&
                is_cursor_on_identifier(ctx, node->line, node->column,
                                        (int) strlen(fi->item_name))) {
                ctx->def_scope = ctx->current_scope;
                ctx->is_global = false;
                ctx->found_def = true;
                ctx->current_scope = saved_scope;
                return;
            }

            find_symbol_definition(fi->body, ctx);
            ctx->current_scope = saved_scope;
            break;
        }

        case AST_FUNCTION_EXPR: {
            FunctionDeclNode *fn_expr = &node->as.function_expr;

            XaScope *saved_scope = ctx->current_scope;
            XaScope *fn_scope = find_child_scope_xa(ctx->current_scope, node);
            if (fn_scope)
                ctx->current_scope = fn_scope;

            // Check parameters
            for (int i = 0; i < fn_expr->param_count; i++) {
                XrParamNode *param = fn_expr->params[i];
                if (param && param->name && strcmp(param->name, ctx->target_name) == 0 &&
                    is_cursor_on_identifier(ctx, param->line, param->column,
                                            (int) strlen(param->name))) {
                    ctx->def_scope = ctx->current_scope;
                    ctx->is_global = false;
                    ctx->found_def = true;
                    ctx->current_scope = saved_scope;
                    return;
                }
            }

            find_symbol_definition(fn_expr->body, ctx);
            ctx->current_scope = saved_scope;
            break;
        }

        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            ClassDeclNode *cls = &node->as.class_decl;
            // Check if cursor is on class/struct name
            if (cls->name && strcmp(cls->name, ctx->target_name) == 0 &&
                is_cursor_on_identifier(ctx, node->line, node->column, (int) strlen(cls->name))) {
                ctx->def_scope = ctx->current_scope;
                ctx->is_global = (ctx->current_scope == ctx->global_scope);
                ctx->found_def = true;
                return;
            }

            // Enter class scope
            XaScope *saved_scope = ctx->current_scope;
            XaScope *class_scope = find_child_scope_xa(ctx->current_scope, node);
            if (class_scope)
                ctx->current_scope = class_scope;

            // Search in fields
            for (int i = 0; i < cls->field_count; i++) {
                find_symbol_definition(cls->fields[i], ctx);
            }

            // Search in methods
            for (int i = 0; i < cls->method_count; i++) {
                find_symbol_definition(cls->methods[i], ctx);
            }

            ctx->current_scope = saved_scope;
            break;
        }

        case AST_TRY_CATCH: {
            TryCatchNode *tc = &node->as.try_catch;

            // Search in try body
            find_symbol_definition(tc->try_body, ctx);

            // Check catch variable with precise position matching
            if (tc->catch_var && strcmp(tc->catch_var, ctx->target_name) == 0 &&
                is_cursor_on_identifier(ctx, tc->catch_var_line, tc->catch_var_column,
                                        (int) strlen(tc->catch_var))) {
                // The catch variable is defined in the catch block scope
                XaScope *catch_scope = find_child_scope_xa(ctx->current_scope, tc->catch_body);
                if (catch_scope) {
                    ctx->def_scope = catch_scope;
                    ctx->is_global = false;
                    ctx->found_def = true;
                    return;
                }
            }

            // Search in catch body
            if (tc->catch_body) {
                XaScope *saved_scope = ctx->current_scope;
                XaScope *catch_scope = find_child_scope_xa(ctx->current_scope, tc->catch_body);
                if (catch_scope)
                    ctx->current_scope = catch_scope;

                find_symbol_definition(tc->catch_body, ctx);
                ctx->current_scope = saved_scope;
            }

            // Search in finally body
            find_symbol_definition(tc->finally_body, ctx);
            break;
        }

        case AST_EXPR_STMT:
            find_symbol_definition(node->as.expr_stmt, ctx);
            break;

        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++) {
                find_symbol_definition(node->as.print_stmt.exprs[i], ctx);
            }
            break;

        case AST_CALL_EXPR:
            find_symbol_definition(node->as.call_expr.callee, ctx);
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                find_symbol_definition(node->as.call_expr.arguments[i], ctx);
            }
            break;

        case AST_ASSIGNMENT: {
            const char *assign_name = node->as.assignment.name;
            if (assign_name && strcmp(assign_name, ctx->target_name) == 0 &&
                is_cursor_on_identifier(ctx, node->line, node->column, (int) strlen(assign_name))) {
                ctx->def_scope = xa_scope_find_definition(ctx->current_scope, ctx->target_name);
                if (!ctx->def_scope) {
                    ctx->def_scope = ctx->global_scope;
                }
                ctx->is_global = (ctx->def_scope == ctx->global_scope);
                ctx->found_def = true;
                return;
            }
            find_symbol_definition(node->as.assignment.value, ctx);
            break;
        }

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
            find_symbol_definition(node->as.binary.left, ctx);
            find_symbol_definition(node->as.binary.right, ctx);
            break;

        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            find_symbol_definition(node->as.unary.operand, ctx);
            break;

        case AST_INDEX_GET:
            find_symbol_definition(node->as.index_get.array, ctx);
            find_symbol_definition(node->as.index_get.index, ctx);
            break;

        case AST_INDEX_SET:
            find_symbol_definition(node->as.index_set.array, ctx);
            find_symbol_definition(node->as.index_set.index, ctx);
            find_symbol_definition(node->as.index_set.value, ctx);
            break;

        case AST_MEMBER_ACCESS:
            find_symbol_definition(node->as.member_access.object, ctx);
            break;

        case AST_MEMBER_SET:
            find_symbol_definition(node->as.member_set.object, ctx);
            find_symbol_definition(node->as.member_set.value, ctx);
            break;

        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++) {
                find_symbol_definition(node->as.return_stmt.values[i], ctx);
            }
            break;

        case AST_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.count; i++) {
                find_symbol_definition(node->as.array_literal.elements[i], ctx);
            }
            break;

        case AST_TERNARY:
            find_symbol_definition(node->as.ternary.condition, ctx);
            find_symbol_definition(node->as.ternary.true_expr, ctx);
            find_symbol_definition(node->as.ternary.false_expr, ctx);
            break;

        default:
            break;
    }
}

// Add a text edit for renaming
static void add_rename_edit(RenameContext *ctx, int line, int col, int len) {
    XrJsonValue *edit = xjson_new_object();
    xjson_object_set(edit, "range", xjson_make_range(line - 1, col - 1, line - 1, col - 1 + len));
    xjson_object_set(edit, "newText", xjson_new_string(ctx->new_name));
    xjson_array_push(ctx->edits, edit);
}

// Check if we should rename in the current context
// Uses XaScope hierarchy to determine if current scope can see the definition
static bool should_rename(RenameContext *ctx) {
    if (!ctx->def_scope || !ctx->current_scope)
        return false;

    // Check if current scope has a local definition that shadows the target
    if (ctx->current_scope != ctx->def_scope) {
        XaSymbol *local = xa_scope_lookup_local(ctx->current_scope, ctx->target_name);
        if (local) {
            // Current scope has its own definition - it shadows the one we're renaming
            return false;
        }
    }

    // Check if current scope is the definition scope or a descendant of it
    return xa_scope_is_descendant(ctx->current_scope, ctx->def_scope);
}

// Phase 2: Collect all locations to rename
// Uses XaScope tracking to properly handle shadowing and upvalues
static void collect_rename_locations(AstNode *node, RenameContext *ctx) {
    if (!node)
        return;

    switch (node->type) {
        case AST_PROGRAM:
            ctx->current_scope = ctx->global_scope;
            for (int i = 0; i < node->as.program.count; i++) {
                collect_rename_locations(node->as.program.statements[i], ctx);
            }
            break;

        case AST_FUNCTION_DECL: {
            FunctionDeclNode *fn = &node->as.function_decl;
            // Rename function name if it matches and is visible from def_scope
            if (fn->name && strcmp(fn->name, ctx->target_name) == 0 && should_rename(ctx)) {
                add_rename_edit(ctx, node->line, node->column > 0 ? node->column : 1,
                                (int) strlen(fn->name));
            }

            // Enter function scope
            XaScope *saved_scope = ctx->current_scope;
            XaScope *fn_scope = find_child_scope_xa(ctx->current_scope, node);
            if (fn_scope) {
                ctx->current_scope = fn_scope;
            }

            // Rename parameters if this is the definition scope
            if (ctx->current_scope == ctx->def_scope) {
                for (int i = 0; i < fn->param_count; i++) {
                    XrParamNode *param = fn->params[i];
                    if (param && param->name && strcmp(param->name, ctx->target_name) == 0) {
                        add_rename_edit(ctx, param->line, param->column > 0 ? param->column : 1,
                                        (int) strlen(param->name));
                        break;
                    }
                }
            }

            collect_rename_locations(fn->body, ctx);
            ctx->current_scope = saved_scope;
            break;
        }

        case AST_FUNCTION_EXPR: {
            FunctionDeclNode *fn_expr = &node->as.function_expr;

            // Enter function expression scope
            XaScope *saved_scope = ctx->current_scope;
            XaScope *fn_scope = find_child_scope_xa(ctx->current_scope, node);
            if (fn_scope) {
                ctx->current_scope = fn_scope;
            }

            // Rename parameters if this is the definition scope
            if (ctx->current_scope == ctx->def_scope) {
                for (int i = 0; i < fn_expr->param_count; i++) {
                    XrParamNode *param = fn_expr->params[i];
                    if (param && param->name && strcmp(param->name, ctx->target_name) == 0) {
                        add_rename_edit(ctx, param->line, param->column > 0 ? param->column : 1,
                                        (int) strlen(param->name));
                        break;
                    }
                }
            }

            collect_rename_locations(fn_expr->body, ctx);
            ctx->current_scope = saved_scope;
            break;
        }

        case AST_VAR_DECL:
        case AST_CONST_DECL: {
            VarDeclNode *var = &node->as.var_decl;
            if (var->name && strcmp(var->name, ctx->target_name) == 0 && should_rename(ctx)) {
                add_rename_edit(ctx, node->line, node->column > 0 ? node->column : 1,
                                (int) strlen(var->name));
            }
            collect_rename_locations(var->initializer, ctx);
            break;
        }

        case AST_VARIABLE: {
            const char *var_name = node->as.variable.name;
            if (var_name && strcmp(var_name, ctx->target_name) == 0 && should_rename(ctx)) {
                add_rename_edit(ctx, node->line, node->column > 0 ? node->column : 1,
                                (int) strlen(var_name));
            }
            break;
        }

        case AST_ASSIGNMENT: {
            const char *assign_name = node->as.assignment.name;
            if (assign_name && strcmp(assign_name, ctx->target_name) == 0 && should_rename(ctx)) {
                add_rename_edit(ctx, node->line, node->column > 0 ? node->column : 1,
                                (int) strlen(assign_name));
            }
            collect_rename_locations(node->as.assignment.value, ctx);
            break;
        }

        case AST_BLOCK: {
            // Enter block scope if it exists
            XaScope *saved_scope = ctx->current_scope;
            XaScope *block_scope = find_child_scope_xa(ctx->current_scope, node);
            if (block_scope) {
                ctx->current_scope = block_scope;
            }
            for (int i = 0; i < node->as.block.count; i++) {
                collect_rename_locations(node->as.block.statements[i], ctx);
            }
            ctx->current_scope = saved_scope;
            break;
        }

        case AST_IF_STMT:
            collect_rename_locations(node->as.if_stmt.condition, ctx);
            collect_rename_locations(node->as.if_stmt.then_branch, ctx);
            collect_rename_locations(node->as.if_stmt.else_branch, ctx);
            break;

        case AST_WHILE_STMT:
            collect_rename_locations(node->as.while_stmt.condition, ctx);
            collect_rename_locations(node->as.while_stmt.body, ctx);
            break;

        case AST_FOR_STMT: {
            // For loop has its own scope
            XaScope *saved_scope = ctx->current_scope;
            XaScope *for_scope = find_child_scope_xa(ctx->current_scope, node);
            if (for_scope) {
                ctx->current_scope = for_scope;
            }
            collect_rename_locations(node->as.for_stmt.initializer, ctx);
            collect_rename_locations(node->as.for_stmt.condition, ctx);
            collect_rename_locations(node->as.for_stmt.increment, ctx);
            collect_rename_locations(node->as.for_stmt.body, ctx);
            ctx->current_scope = saved_scope;
            break;
        }

        case AST_FOR_IN_STMT: {
            ForInStmtNode *fi = &node->as.for_in_stmt;
            // Collection is evaluated in parent scope
            collect_rename_locations(fi->collection, ctx);

            // For-in loop has its own scope
            XaScope *saved_scope = ctx->current_scope;
            XaScope *for_scope = find_child_scope_xa(ctx->current_scope, node);
            if (for_scope) {
                ctx->current_scope = for_scope;
            }

            // Rename loop variable if it matches
            if (fi->item_name && strcmp(fi->item_name, ctx->target_name) == 0 &&
                should_rename(ctx)) {
                add_rename_edit(ctx, node->line, node->column > 0 ? node->column : 1,
                                (int) strlen(fi->item_name));
            }
            collect_rename_locations(fi->body, ctx);
            ctx->current_scope = saved_scope;
            break;
        }

        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            ClassDeclNode *cls = &node->as.class_decl;
            // Rename class/struct name if it matches
            if (cls->name && strcmp(cls->name, ctx->target_name) == 0 && should_rename(ctx)) {
                add_rename_edit(ctx, node->line, node->column > 0 ? node->column : 1,
                                (int) strlen(cls->name));
            }

            // Enter class scope
            XaScope *saved_scope = ctx->current_scope;
            XaScope *class_scope = find_child_scope_xa(ctx->current_scope, node);
            if (class_scope) {
                ctx->current_scope = class_scope;
            }

            // Process fields
            for (int i = 0; i < cls->field_count; i++) {
                collect_rename_locations(cls->fields[i], ctx);
            }

            // Process methods
            for (int i = 0; i < cls->method_count; i++) {
                collect_rename_locations(cls->methods[i], ctx);
            }

            ctx->current_scope = saved_scope;
            break;
        }

        case AST_TRY_CATCH: {
            TryCatchNode *tc = &node->as.try_catch;

            // Process try body
            collect_rename_locations(tc->try_body, ctx);

            // Process catch body with its own scope
            if (tc->catch_body) {
                XaScope *saved_scope = ctx->current_scope;
                XaScope *catch_scope = find_child_scope_xa(ctx->current_scope, tc->catch_body);
                if (catch_scope) {
                    ctx->current_scope = catch_scope;
                }

                // Rename catch variable if it matches (now with precise position info)
                if (tc->catch_var && strcmp(tc->catch_var, ctx->target_name) == 0 &&
                    should_rename(ctx)) {
                    int var_line = tc->catch_var_line > 0 ? tc->catch_var_line : node->line;
                    int var_col = tc->catch_var_column > 0 ? tc->catch_var_column : 1;
                    add_rename_edit(ctx, var_line, var_col, (int) strlen(tc->catch_var));
                }

                collect_rename_locations(tc->catch_body, ctx);
                ctx->current_scope = saved_scope;
            }

            // Process finally body
            collect_rename_locations(tc->finally_body, ctx);
            break;
        }

        case AST_EXPR_STMT:
            collect_rename_locations(node->as.expr_stmt, ctx);
            break;

        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++) {
                collect_rename_locations(node->as.print_stmt.exprs[i], ctx);
            }
            break;

        case AST_CALL_EXPR:
            collect_rename_locations(node->as.call_expr.callee, ctx);
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                collect_rename_locations(node->as.call_expr.arguments[i], ctx);
            }
            break;

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
            collect_rename_locations(node->as.binary.left, ctx);
            collect_rename_locations(node->as.binary.right, ctx);
            break;

        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            collect_rename_locations(node->as.unary.operand, ctx);
            break;

        case AST_INDEX_GET:
            collect_rename_locations(node->as.index_get.array, ctx);
            collect_rename_locations(node->as.index_get.index, ctx);
            break;

        case AST_INDEX_SET:
            collect_rename_locations(node->as.index_set.array, ctx);
            collect_rename_locations(node->as.index_set.index, ctx);
            collect_rename_locations(node->as.index_set.value, ctx);
            break;

        case AST_MEMBER_ACCESS:
            collect_rename_locations(node->as.member_access.object, ctx);
            break;

        case AST_MEMBER_SET:
            collect_rename_locations(node->as.member_set.object, ctx);
            collect_rename_locations(node->as.member_set.value, ctx);
            break;

        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++) {
                collect_rename_locations(node->as.return_stmt.values[i], ctx);
            }
            break;

        case AST_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.count; i++) {
                collect_rename_locations(node->as.array_literal.elements[i], ctx);
            }
            break;

        case AST_TERNARY:
            collect_rename_locations(node->as.ternary.condition, ctx);
            collect_rename_locations(node->as.ternary.true_expr, ctx);
            collect_rename_locations(node->as.ternary.false_expr, ctx);
            break;

        default:
            break;
    }
}

XrJsonValue *xlsp_analyze_prepare_rename(XrLspDocument *doc, XrLspPosition pos) {
    if (!doc || !doc->content)
        return NULL;

    uint32_t start, end;
    char *word = xlsp_word_at_position(doc, pos, &start, &end);
    if (!word)
        return NULL;

    // Check if it's a keyword (cannot rename)
    for (int i = 0; xr_keywords[i]; i++) {
        if (strcmp(word, xr_keywords[i]) == 0) {
            xr_free(word);
            return NULL;  // Cannot rename keywords
        }
    }

    // Check if it's a builtin (cannot rename)
    for (int i = 0; xr_builtins[i]; i++) {
        if (strcmp(word, xr_builtins[i]) == 0) {
            xr_free(word);
            return NULL;  // Cannot rename builtins
        }
    }

    xr_free(word);

    // Return the range of the symbol
    XrLspPosition range_start = xlsp_offset_to_position(doc, start);
    XrLspPosition range_end = xlsp_offset_to_position(doc, end);

    XrJsonValue *result = xjson_new_object();
    xjson_object_set(result, "range",
                     xjson_make_range(range_start.line, range_start.character, range_end.line,
                                      range_end.character));

    return result;
}

XrJsonValue *xlsp_analyze_rename(XrLspServer *server, XrLspDocument *doc, XrLspPosition pos,
                                 const char *new_name) {
    if (!doc || !doc->content || !new_name)
        return NULL;

    uint32_t start, end;
    char *old_name = NULL;

    old_name = xlsp_word_at_position(doc, pos, &start, &end);
    if (!old_name)
        return NULL;

    // Build WorkspaceEdit with changes
    XrJsonValue *result = xjson_new_object();
    XrJsonValue *changes = xjson_new_object();
    XrJsonValue *edits = xjson_new_array();  // Always create, attach to result at end

    // Use XaScope from analyzer for unified scope handling
    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;
    if (doc->ast && analyzer && analyzer->global_scope) {
        // Phase 1: Find the scope where the symbol is defined
        RenameContext ctx = {.target_name = old_name,
                             .target_name_len = (int) strlen(old_name),
                             .target_line = pos.line + 1,  // Convert to 1-indexed
                             .target_col = pos.character + 1,
                             .global_scope = analyzer->global_scope,
                             .def_scope = NULL,
                             .current_scope = analyzer->global_scope,
                             .found_def = false,
                             .is_global = false,
                             .edits = edits,
                             .new_name = new_name};

        find_symbol_definition(doc->ast, &ctx);

        if (ctx.found_def && ctx.def_scope) {
            // Phase 2: Collect all rename locations using scope hierarchy
            ctx.current_scope = analyzer->global_scope;
            collect_rename_locations(doc->ast, &ctx);

            lsp_log("Rename '%s' -> '%s': found %d locations", old_name, new_name,
                    xjson_array_len(edits));
        } else {
            lsp_log("Rename '%s': symbol definition not found (line %d, col %d)", old_name,
                    pos.line + 1, pos.character + 1);
        }
    } else {
        lsp_log("Rename '%s': AST or analyzer not available", old_name);
    }

    // Always attach edits array to changes (may be empty)
    if (xjson_array_len(edits) > 0) {
        xjson_object_set(changes, doc->uri, edits);
    }
    // Note: if edits is empty, it will be freed when result is freed

    xr_free(old_name);

    xjson_object_set(result, "changes", changes);
    return result;
}
