/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_call_hierarchy.c - Call hierarchy and type hierarchy support
 */

#include "xlsp_call_hierarchy.h"
#include "xlsp_analysis.h"
#include "xlsp_workspace.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include <string.h>

// Helper to create a call hierarchy item
static XrJsonValue *create_call_hierarchy_item(const char *name, int kind,
    const char *uri, int line, int col, int end_line, int end_col) {
    XrJsonValue *item = xjson_new_object();
    xjson_object_set(item, "name", xjson_new_string(name));
    xjson_object_set(item, "kind", xjson_new_number(kind));
    xjson_object_set(item, "uri", xjson_new_string(uri));
    xjson_object_set(item, "range", xjson_make_range(line, col, end_line, end_col));
    xjson_object_set(item, "selectionRange", xjson_make_range(line, col, end_line, end_col));
    return item;
}

// ============================================================================
// Call Hierarchy
// ============================================================================

XrJsonValue *xlsp_handle_prepare_call_hierarchy(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *position = xjson_get_object(params, "position");
    if (!textDocument || !position) return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->ast) return xjson_new_array();

    XrLspPosition pos = {
        .line = (uint32_t)xjson_get_int(position, "line"),
        .character = (uint32_t)xjson_get_int(position, "character")
    };

    XrJsonValue *items = xjson_new_array();

    AstNode *ast = doc->ast;
    if (ast->type == AST_PROGRAM) {
        for (int i = 0; i < ast->as.program.count; i++) {
            AstNode *stmt = ast->as.program.statements[i];
            if (!stmt) continue;

            if (stmt->type == AST_FUNCTION_DECL && stmt->line - 1 == (int)pos.line) {
                XrJsonValue *item = create_call_hierarchy_item(
                    stmt->as.function_decl.name, LSP_SYMBOL_FUNCTION, uri,
                    stmt->line - 1, 0, stmt->line - 1,
                    strlen(stmt->as.function_decl.name));
                xjson_array_push(items, item);
                break;
            }
        }
    }

    return items;
}

// Context for finding incoming calls
typedef struct {
    const char *target_name;
    const char *current_func;
    const char *uri;
    XrJsonValue *calls;
} CallHierarchyCtx;

static void find_calls_in_ast(AstNode *node, CallHierarchyCtx *ctx) {
    if (!node) return;

    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) {
                find_calls_in_ast(node->as.program.statements[i], ctx);
            }
            break;

        case AST_FUNCTION_DECL: {
            const char *prev_func = ctx->current_func;
            ctx->current_func = node->as.function_decl.name;
            find_calls_in_ast(node->as.function_decl.body, ctx);
            ctx->current_func = prev_func;
            break;
        }

        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) {
                find_calls_in_ast(node->as.block.statements[i], ctx);
            }
            break;

        case AST_CALL_EXPR: {
            AstNode *callee = node->as.call_expr.callee;
            if (callee && callee->type == AST_VARIABLE) {
                const char *called_name = callee->as.variable.name;
                if (called_name && strcmp(called_name, ctx->target_name) == 0) {
                    XrJsonValue *call = xjson_new_object();
                    XrJsonValue *from_item = create_call_hierarchy_item(
                        ctx->current_func ? ctx->current_func : "<global>",
                        LSP_SYMBOL_FUNCTION, ctx->uri,
                        node->line - 1, node->column > 0 ? node->column - 1 : 0,
                        node->line - 1, node->column > 0 ? node->column - 1 + (int)strlen(called_name) : 10);
                    xjson_object_set(call, "from", from_item);

                    XrJsonValue *from_ranges = xjson_new_array();
                    int line = node->line - 1;
                    int col = node->column > 0 ? node->column - 1 : 0;
                    xjson_array_push(from_ranges,
                        xjson_make_range(line, col, line, col + strlen(called_name)));
                    xjson_object_set(call, "fromRanges", from_ranges);

                    xjson_array_push(ctx->calls, call);
                }
            }
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                find_calls_in_ast(node->as.call_expr.arguments[i], ctx);
            }
            find_calls_in_ast(node->as.call_expr.callee, ctx);
            break;
        }

        case AST_IF_STMT:
            find_calls_in_ast(node->as.if_stmt.condition, ctx);
            find_calls_in_ast(node->as.if_stmt.then_branch, ctx);
            find_calls_in_ast(node->as.if_stmt.else_branch, ctx);
            break;

        case AST_WHILE_STMT:
            find_calls_in_ast(node->as.while_stmt.condition, ctx);
            find_calls_in_ast(node->as.while_stmt.body, ctx);
            break;

        case AST_FOR_STMT:
            find_calls_in_ast(node->as.for_stmt.initializer, ctx);
            find_calls_in_ast(node->as.for_stmt.condition, ctx);
            find_calls_in_ast(node->as.for_stmt.increment, ctx);
            find_calls_in_ast(node->as.for_stmt.body, ctx);
            break;

        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++) {
                find_calls_in_ast(node->as.return_stmt.values[i], ctx);
            }
            break;

        case AST_VAR_DECL:
            find_calls_in_ast(node->as.var_decl.initializer, ctx);
            break;

        case AST_ASSIGNMENT:
            find_calls_in_ast(node->as.assignment.value, ctx);
            break;

        case AST_BINARY_ADD: case AST_BINARY_SUB: case AST_BINARY_MUL:
        case AST_BINARY_DIV: case AST_BINARY_MOD: case AST_BINARY_AND:
        case AST_BINARY_OR: case AST_BINARY_EQ: case AST_BINARY_NE:
        case AST_BINARY_LT: case AST_BINARY_LE: case AST_BINARY_GT:
        case AST_BINARY_GE:
            find_calls_in_ast(node->as.binary.left, ctx);
            find_calls_in_ast(node->as.binary.right, ctx);
            break;

        case AST_UNARY_NEG: case AST_UNARY_NOT: case AST_UNARY_BNOT:
            find_calls_in_ast(node->as.unary.operand, ctx);
            break;

        default:
            break;
    }
}

XrJsonValue *xlsp_handle_call_hierarchy_incoming(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *item = xjson_get_object(params, "item");
    if (!item) return xjson_new_array();

    const char *name = xjson_get_string(item, "name");
    const char *uri = xjson_get_string(item, "uri");
    if (!name || !uri) return xjson_new_array();

    XrJsonValue *calls = xjson_new_array();

    // Search all open documents via AST traversal
    if (server && server->doc_table) {
        XrLspDocTable *table = server->doc_table;
        for (int i = 0; i < table->bucket_count; i++) {
            XrLspDocBucket *bucket = table->buckets[i];
            while (bucket) {
                XrLspDocument *doc = bucket->doc;
                if (doc && doc->ast) {
                    CallHierarchyCtx ctx = {
                        .target_name = name,
                        .current_func = NULL,
                        .uri = doc->uri,
                        .calls = calls
                    };
                    find_calls_in_ast(doc->ast, &ctx);
                }
                bucket = bucket->next;
            }
        }
    }

    // Search workspace analyzer for references in files not currently open.
    // This gives call hierarchy coverage for unopened files via the
    // dependency graph built during background indexing.
    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;
    if (analyzer) {
        int ref_count = 0;
        XaSymbolRef *arefs = xa_analyzer_find_references(
            analyzer, name, false, &ref_count);

        for (XaSymbolRef *r = arefs; r; r = r->next) {
            if (!r->file) continue;
            // Skip files already covered by the open-document AST search
            bool already_covered = false;
            if (server->doc_table) {
                XrLspDocument *d = xlsp_document_get(server, r->file);
                if (d && d->ast) already_covered = true;
            }
            if (already_covered) continue;

            int line = r->line > 0 ? (int)r->line - 1 : 0;
            int col  = r->column > 0 ? (int)r->column - 1 : 0;

            // Create a call hierarchy incoming item.
            // The caller name is approximate (file-level) since we lack
            // the caller function context from the analyzer alone.
            XrJsonValue *incoming = xjson_new_object();
            XrJsonValue *from = create_call_hierarchy_item(
                name, 12 /* Function */, r->file,
                line, col, line, col + (int)strlen(name));
            xjson_object_set(incoming, "from", from);

            XrJsonValue *ranges = xjson_new_array();
            xjson_array_push(ranges,
                xjson_make_range(line, col, line, col + (int)strlen(name)));
            xjson_object_set(incoming, "fromRanges", ranges);
            xjson_array_push(calls, incoming);
        }

        xa_analyzer_free_references(arefs);
    }

    return calls;
}

static void find_outgoing_calls(AstNode *node, const char *uri, XrJsonValue *calls) {
    if (!node) return;

    switch (node->type) {
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) {
                find_outgoing_calls(node->as.block.statements[i], uri, calls);
            }
            break;

        case AST_CALL_EXPR: {
            AstNode *callee = node->as.call_expr.callee;
            if (callee && callee->type == AST_VARIABLE) {
                const char *called_name = callee->as.variable.name;
                if (called_name) {
                    XrJsonValue *call = xjson_new_object();
                    XrJsonValue *to_item = create_call_hierarchy_item(
                        called_name, LSP_SYMBOL_FUNCTION, uri,
                        node->line - 1, node->column > 0 ? node->column - 1 : 0,
                        node->line - 1, node->column > 0 ? node->column - 1 + (int)strlen(called_name) : 10);
                    xjson_object_set(call, "to", to_item);

                    XrJsonValue *from_ranges = xjson_new_array();
                    int line = node->line - 1;
                    int col = node->column > 0 ? node->column - 1 : 0;
                    xjson_array_push(from_ranges,
                        xjson_make_range(line, col, line, col + strlen(called_name)));
                    xjson_object_set(call, "fromRanges", from_ranges);

                    xjson_array_push(calls, call);
                }
            }
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                find_outgoing_calls(node->as.call_expr.arguments[i], uri, calls);
            }
            break;
        }

        case AST_IF_STMT:
            find_outgoing_calls(node->as.if_stmt.condition, uri, calls);
            find_outgoing_calls(node->as.if_stmt.then_branch, uri, calls);
            find_outgoing_calls(node->as.if_stmt.else_branch, uri, calls);
            break;

        case AST_WHILE_STMT:
            find_outgoing_calls(node->as.while_stmt.condition, uri, calls);
            find_outgoing_calls(node->as.while_stmt.body, uri, calls);
            break;

        case AST_FOR_STMT:
            find_outgoing_calls(node->as.for_stmt.initializer, uri, calls);
            find_outgoing_calls(node->as.for_stmt.condition, uri, calls);
            find_outgoing_calls(node->as.for_stmt.increment, uri, calls);
            find_outgoing_calls(node->as.for_stmt.body, uri, calls);
            break;

        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++) {
                find_outgoing_calls(node->as.return_stmt.values[i], uri, calls);
            }
            break;

        case AST_VAR_DECL:
            find_outgoing_calls(node->as.var_decl.initializer, uri, calls);
            break;

        case AST_ASSIGNMENT:
            find_outgoing_calls(node->as.assignment.value, uri, calls);
            break;

        case AST_BINARY_ADD: case AST_BINARY_SUB: case AST_BINARY_MUL:
        case AST_BINARY_DIV: case AST_BINARY_MOD: case AST_BINARY_AND:
        case AST_BINARY_OR: case AST_BINARY_EQ: case AST_BINARY_NE:
        case AST_BINARY_LT: case AST_BINARY_LE: case AST_BINARY_GT:
        case AST_BINARY_GE:
            find_outgoing_calls(node->as.binary.left, uri, calls);
            find_outgoing_calls(node->as.binary.right, uri, calls);
            break;

        case AST_UNARY_NEG: case AST_UNARY_NOT: case AST_UNARY_BNOT:
            find_outgoing_calls(node->as.unary.operand, uri, calls);
            break;

        default:
            break;
    }
}

XrJsonValue *xlsp_handle_call_hierarchy_outgoing(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *item = xjson_get_object(params, "item");
    if (!item) return xjson_new_array();

    const char *name = xjson_get_string(item, "name");
    const char *uri = xjson_get_string(item, "uri");
    if (!name || !uri) return xjson_new_array();

    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->ast) return xjson_new_array();

    XrJsonValue *calls = xjson_new_array();

    AstNode *ast = doc->ast;
    if (ast && ast->type == AST_PROGRAM) {
        for (int i = 0; i < ast->as.program.count; i++) {
            AstNode *stmt = ast->as.program.statements[i];
            if (stmt && stmt->type == AST_FUNCTION_DECL &&
                stmt->as.function_decl.name &&
                strcmp(stmt->as.function_decl.name, name) == 0) {
                find_outgoing_calls(stmt->as.function_decl.body, uri, calls);
                break;
            }
        }
    }

    return calls;
}

// ============================================================================
// Type Hierarchy
// ============================================================================

XrJsonValue *xlsp_handle_prepare_type_hierarchy(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *position = xjson_get_object(params, "position");
    if (!textDocument || !position) return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->ast) return xjson_new_array();

    XrLspPosition pos = {
        .line = (uint32_t)xjson_get_int(position, "line"),
        .character = (uint32_t)xjson_get_int(position, "character")
    };

    XrJsonValue *items = xjson_new_array();

    AstNode *ast = doc->ast;
    if (ast->type == AST_PROGRAM) {
        for (int i = 0; i < ast->as.program.count; i++) {
            AstNode *stmt = ast->as.program.statements[i];
            if (!stmt) continue;

            if (stmt->type == AST_CLASS_DECL && stmt->line - 1 == (int)pos.line) {
                XrJsonValue *item = xjson_new_object();
                xjson_object_set(item, "name", xjson_new_string(stmt->as.class_decl.name));
                xjson_object_set(item, "kind", xjson_new_number(LSP_SYMBOL_CLASS));
                xjson_object_set(item, "uri", xjson_new_string(uri));

                if (stmt->as.class_decl.super_name) {
                    xjson_object_set(item, "detail",
                        xjson_new_string(stmt->as.class_decl.super_name));
                }

                int line = stmt->line - 1;
                xjson_object_set(item, "range", xjson_make_range(line, 0, line, 50));
                xjson_object_set(item, "selectionRange", xjson_make_range(line, 0, line, 50));

                xjson_array_push(items, item);
                break;
            }
        }
    }

    return items;
}

XrJsonValue *xlsp_handle_type_hierarchy_supertypes(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *item = xjson_get_object(params, "item");
    if (!item) return xjson_new_array();

    const char *super_name = xjson_get_string(item, "detail");
    const char *uri = xjson_get_string(item, "uri");
    if (!super_name || !uri) return xjson_new_array();

    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->ast) return xjson_new_array();

    XrJsonValue *supertypes = xjson_new_array();

    AstNode *ast = doc->ast;
    if (ast->type == AST_PROGRAM) {
        for (int i = 0; i < ast->as.program.count; i++) {
            AstNode *stmt = ast->as.program.statements[i];
            if (!stmt) continue;

            if (stmt->type == AST_CLASS_DECL &&
                strcmp(stmt->as.class_decl.name, super_name) == 0) {
                XrJsonValue *super_item = xjson_new_object();
                xjson_object_set(super_item, "name", xjson_new_string(super_name));
                xjson_object_set(super_item, "kind", xjson_new_number(LSP_SYMBOL_CLASS));
                xjson_object_set(super_item, "uri", xjson_new_string(uri));

                if (stmt->as.class_decl.super_name) {
                    xjson_object_set(super_item, "detail",
                        xjson_new_string(stmt->as.class_decl.super_name));
                }

                int line = stmt->line - 1;
                xjson_object_set(super_item, "range", xjson_make_range(line, 0, line, 50));
                xjson_object_set(super_item, "selectionRange", xjson_make_range(line, 0, line, 50));

                xjson_array_push(supertypes, super_item);
                break;
            }
        }
    }

    return supertypes;
}

XrJsonValue *xlsp_handle_type_hierarchy_subtypes(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *item = xjson_get_object(params, "item");
    if (!item) return xjson_new_array();

    const char *class_name = xjson_get_string(item, "name");
    const char *uri = xjson_get_string(item, "uri");
    if (!class_name || !uri) return xjson_new_array();

    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->ast) return xjson_new_array();

    XrJsonValue *subtypes = xjson_new_array();

    AstNode *ast = doc->ast;
    if (ast->type == AST_PROGRAM) {
        for (int i = 0; i < ast->as.program.count; i++) {
            AstNode *stmt = ast->as.program.statements[i];
            if (!stmt) continue;

            if (stmt->type == AST_CLASS_DECL && stmt->as.class_decl.super_name &&
                strcmp(stmt->as.class_decl.super_name, class_name) == 0) {
                XrJsonValue *sub_item = xjson_new_object();
                xjson_object_set(sub_item, "name",
                    xjson_new_string(stmt->as.class_decl.name));
                xjson_object_set(sub_item, "kind", xjson_new_number(LSP_SYMBOL_CLASS));
                xjson_object_set(sub_item, "uri", xjson_new_string(uri));

                int line = stmt->line - 1;
                xjson_object_set(sub_item, "range", xjson_make_range(line, 0, line, 50));
                xjson_object_set(sub_item, "selectionRange", xjson_make_range(line, 0, line, 50));

                xjson_array_push(subtypes, sub_item);
            }
        }
    }

    return subtypes;
}

// ============================================================================
// Implementation (delegates to definition)
// ============================================================================

XrJsonValue *xlsp_handle_implementation(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *position = xjson_get_object(params, "position");
    if (!textDocument || !position) return xjson_new_null();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xjson_new_null();

    XrLspPosition pos = {
        .line = (uint32_t)xjson_get_int(position, "line"),
        .character = (uint32_t)xjson_get_int(position, "character")
    };

    return xlsp_analyze_definition(server, doc, pos);
}
