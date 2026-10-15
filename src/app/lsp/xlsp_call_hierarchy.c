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
#include <string.h>

// Helper to create a call hierarchy item
static XrJsonValue *create_call_hierarchy_item(const char *name, int kind, const char *uri,
                                               int line, int col, int end_line, int end_col) {
    XrJsonValue *item = xjson_new_object();
    xjson_object_set(item, "name", xjson_new_string(name));
    xjson_object_set(item, "kind", xjson_new_number(kind));
    xjson_object_set(item, "uri", xjson_new_string(uri));
    xjson_object_set(item, "range", xjson_make_range(line, col, end_line, end_col));
    xjson_object_set(item, "selectionRange", xjson_make_range(line, col, end_line, end_col));
    return item;
}

// ============================================================================
// Type Hierarchy
// ============================================================================

XrJsonValue *xlsp_handle_prepare_type_hierarchy(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *position = xjson_get_object(params, "position");
    if (!textDocument || !position)
        return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->ast)
        return xjson_new_array();

    XrLspPosition pos = {.line = (uint32_t) xjson_get_int(position, "line"),
                         .character = (uint32_t) xjson_get_int(position, "character")};

    XrJsonValue *items = xjson_new_array();

    AstNode *ast = doc->ast;
    if (ast->type == AST_PROGRAM) {
        for (int i = 0; i < ast->as.program.count; i++) {
            AstNode *stmt = ast->as.program.statements[i];
            if (!stmt)
                continue;

            if (stmt->type == AST_CLASS_DECL && stmt->line - 1 == (int) pos.line) {
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
    if (!item)
        return xjson_new_array();

    const char *super_name = xjson_get_string(item, "detail");
    const char *uri = xjson_get_string(item, "uri");
    if (!super_name || !uri)
        return xjson_new_array();

    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->ast)
        return xjson_new_array();

    XrJsonValue *supertypes = xjson_new_array();

    AstNode *ast = doc->ast;
    if (ast->type == AST_PROGRAM) {
        for (int i = 0; i < ast->as.program.count; i++) {
            AstNode *stmt = ast->as.program.statements[i];
            if (!stmt)
                continue;

            if (stmt->type == AST_CLASS_DECL && strcmp(stmt->as.class_decl.name, super_name) == 0) {
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
    if (!item)
        return xjson_new_array();

    const char *class_name = xjson_get_string(item, "name");
    const char *uri = xjson_get_string(item, "uri");
    if (!class_name || !uri)
        return xjson_new_array();

    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->ast)
        return xjson_new_array();

    XrJsonValue *subtypes = xjson_new_array();

    AstNode *ast = doc->ast;
    if (ast->type == AST_PROGRAM) {
        for (int i = 0; i < ast->as.program.count; i++) {
            AstNode *stmt = ast->as.program.statements[i];
            if (!stmt)
                continue;

            if (stmt->type == AST_CLASS_DECL && stmt->as.class_decl.super_name &&
                strcmp(stmt->as.class_decl.super_name, class_name) == 0) {
                XrJsonValue *sub_item = xjson_new_object();
                xjson_object_set(sub_item, "name", xjson_new_string(stmt->as.class_decl.name));
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
    if (!textDocument || !position)
        return xjson_new_null();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc)
        return xjson_new_null();

    XrLspPosition pos = {.line = (uint32_t) xjson_get_int(position, "line"),
                         .character = (uint32_t) xjson_get_int(position, "character")};

    return xlsp_analyze_definition(server, doc, pos);
}
