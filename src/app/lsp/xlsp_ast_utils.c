/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_ast_utils.c - Shared AST utility functions for LSP
 */

#include "xlsp_ast_utils.h"
#include "../../frontend/parser/xast_nodes.h"

int xlsp_get_node_end_line(AstNode *node) {
    if (!node) return 0;
    int max_line = node->line;

    switch (node->type) {
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) {
                int end = xlsp_get_node_end_line(node->as.block.statements[i]);
                if (end > max_line) max_line = end;
            }
            break;
        case AST_FUNCTION_DECL:
            if (node->as.function_decl.body) {
                int end = xlsp_get_node_end_line(node->as.function_decl.body);
                if (end > max_line) max_line = end;
            }
            break;
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
            for (int i = 0; i < node->as.class_decl.method_count; i++) {
                int end = xlsp_get_node_end_line(node->as.class_decl.methods[i]);
                if (end > max_line) max_line = end;
            }
            break;
        case AST_METHOD_DECL:
            if (node->as.method_decl.body) {
                int end = xlsp_get_node_end_line(node->as.method_decl.body);
                if (end > max_line) max_line = end;
            }
            break;
        case AST_IF_STMT:
            if (node->as.if_stmt.then_branch) {
                int end = xlsp_get_node_end_line(node->as.if_stmt.then_branch);
                if (end > max_line) max_line = end;
            }
            if (node->as.if_stmt.else_branch) {
                int end = xlsp_get_node_end_line(node->as.if_stmt.else_branch);
                if (end > max_line) max_line = end;
            }
            break;
        case AST_WHILE_STMT:
            if (node->as.while_stmt.body) {
                int end = xlsp_get_node_end_line(node->as.while_stmt.body);
                if (end > max_line) max_line = end;
            }
            break;
        case AST_FOR_STMT:
            if (node->as.for_stmt.body) {
                int end = xlsp_get_node_end_line(node->as.for_stmt.body);
                if (end > max_line) max_line = end;
            }
            break;
        case AST_TRY_CATCH:
            if (node->as.try_catch.try_body) {
                int end = xlsp_get_node_end_line(node->as.try_catch.try_body);
                if (end > max_line) max_line = end;
            }
            if (node->as.try_catch.catch_body) {
                int end = xlsp_get_node_end_line(node->as.try_catch.catch_body);
                if (end > max_line) max_line = end;
            }
            if (node->as.try_catch.finally_body) {
                int end = xlsp_get_node_end_line(node->as.try_catch.finally_body);
                if (end > max_line) max_line = end;
            }
            break;
        case AST_MATCH_EXPR:
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                int end = xlsp_get_node_end_line(node->as.match_expr.arms[i]);
                if (end > max_line) max_line = end;
            }
            break;
        default:
            break;
    }
    return max_line;
}
