/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_folding.c - Folding range support
 */

#include "xlsp_folding.h"
#include "xlsp_ast_utils.h"
#include "../../frontend/parser/xast_nodes.h"

static void collect_folding_ranges(AstNode *node, XrJsonValue *ranges) {
    if (!node) return;
    
    #define ADD_FOLD(start_line, end_line, kind) do { \
        if ((end_line) > (start_line)) { \
            XrJsonValue *range = xlsp_json_new_object(); \
            xlsp_json_object_set(range, "startLine", xlsp_json_new_number(start_line)); \
            xlsp_json_object_set(range, "endLine", xlsp_json_new_number(end_line)); \
            xlsp_json_object_set(range, "kind", xlsp_json_new_string(kind)); \
            xlsp_json_array_push(ranges, range); \
        } \
    } while(0)
    
    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) {
                collect_folding_ranges(node->as.program.statements[i], ranges);
            }
            break;
        case AST_FUNCTION_DECL:
            if (node->as.function_decl.body) {
                int start = node->line - 1;
                int end = xlsp_get_node_end_line(node->as.function_decl.body) - 1;
                ADD_FOLD(start, end, "region");
                collect_folding_ranges(node->as.function_decl.body, ranges);
            }
            break;
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            int start = node->line - 1;
            int end = xlsp_get_node_end_line(node) - 1;
            ADD_FOLD(start, end, "region");
            for (int i = 0; i < node->as.class_decl.method_count; i++) {
                collect_folding_ranges(node->as.class_decl.methods[i], ranges);
            }
            break;
        }
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) {
                collect_folding_ranges(node->as.block.statements[i], ranges);
            }
            break;
        case AST_IF_STMT: {
            int start = node->line - 1;
            int end = xlsp_get_node_end_line(node) - 1;
            ADD_FOLD(start, end, "region");
            collect_folding_ranges(node->as.if_stmt.then_branch, ranges);
            collect_folding_ranges(node->as.if_stmt.else_branch, ranges);
            break;
        }
        case AST_WHILE_STMT: {
            int start = node->line - 1;
            int end = xlsp_get_node_end_line(node->as.while_stmt.body) - 1;
            ADD_FOLD(start, end, "region");
            collect_folding_ranges(node->as.while_stmt.body, ranges);
            break;
        }
        case AST_FOR_STMT: {
            int start = node->line - 1;
            int end = xlsp_get_node_end_line(node->as.for_stmt.body) - 1;
            ADD_FOLD(start, end, "region");
            collect_folding_ranges(node->as.for_stmt.body, ranges);
            break;
        }
        case AST_TRY_CATCH: {
            int start = node->line - 1;
            int end = xlsp_get_node_end_line(node) - 1;
            ADD_FOLD(start, end, "region");
            collect_folding_ranges(node->as.try_catch.try_body, ranges);
            collect_folding_ranges(node->as.try_catch.catch_body, ranges);
            collect_folding_ranges(node->as.try_catch.finally_body, ranges);
            break;
        }
        case AST_MATCH_EXPR: {
            int start = node->line - 1;
            int end = xlsp_get_node_end_line(node) - 1;
            ADD_FOLD(start, end, "region");
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                collect_folding_ranges(node->as.match_expr.arms[i], ranges);
            }
            break;
        }
        default:
            break;
    }
    
    #undef ADD_FOLD
}

XrJsonValue *xlsp_handle_folding_range(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    if (!textDocument) return xlsp_json_new_array();
    
    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xlsp_json_new_array();
    
    XrJsonValue *ranges = xlsp_json_new_array();
    
    if (!doc->ast) return ranges;
    
    collect_folding_ranges(doc->ast, ranges);
    
    return ranges;
}
