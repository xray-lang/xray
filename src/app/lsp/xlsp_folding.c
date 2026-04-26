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
#include "../../frontend/parser/xast_nodes.h"

// Emit a folding range using the node's own span (set by the parser).
// Nodes with an unset end (end_line == 0) are silently skipped — parsers
// guarantee spans for every folding-capable construct, so an unset end
// signals an incomplete parse where folding is not meaningful anyway.
#define ADD_NODE_FOLD(ranges, start_line, node, kind)                                              \
    do {                                                                                           \
        AstNode *_n = (node);                                                                      \
        if (_n && _n->end_line > 0 && _n->end_line - 1 > (start_line)) {                           \
            XrJsonValue *_range = xjson_new_object();                                              \
            xjson_object_set(_range, "startLine", xjson_new_number(start_line));                   \
            xjson_object_set(_range, "endLine", xjson_new_number(_n->end_line - 1));               \
            xjson_object_set(_range, "kind", xjson_new_string(kind));                              \
            xjson_array_push((ranges), _range);                                                    \
        }                                                                                          \
    } while (0)

static void collect_folding_ranges(AstNode *node, XrJsonValue *ranges) {
    if (!node)
        return;

    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) {
                collect_folding_ranges(node->as.program.statements[i], ranges);
            }
            break;
        case AST_FUNCTION_DECL:
            if (node->as.function_decl.body) {
                ADD_NODE_FOLD(ranges, node->line - 1, node, "region");
                collect_folding_ranges(node->as.function_decl.body, ranges);
            }
            break;
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
            ADD_NODE_FOLD(ranges, node->line - 1, node, "region");
            for (int i = 0; i < node->as.class_decl.method_count; i++) {
                collect_folding_ranges(node->as.class_decl.methods[i], ranges);
            }
            break;
        case AST_BLOCK:
            for (int i = 0; i < node->as.block.count; i++) {
                collect_folding_ranges(node->as.block.statements[i], ranges);
            }
            break;
        case AST_IF_STMT:
            ADD_NODE_FOLD(ranges, node->line - 1, node, "region");
            collect_folding_ranges(node->as.if_stmt.then_branch, ranges);
            collect_folding_ranges(node->as.if_stmt.else_branch, ranges);
            break;
        case AST_WHILE_STMT:
            ADD_NODE_FOLD(ranges, node->line - 1, node, "region");
            collect_folding_ranges(node->as.while_stmt.body, ranges);
            break;
        case AST_FOR_STMT:
            ADD_NODE_FOLD(ranges, node->line - 1, node, "region");
            collect_folding_ranges(node->as.for_stmt.body, ranges);
            break;
        case AST_TRY_CATCH:
            ADD_NODE_FOLD(ranges, node->line - 1, node, "region");
            collect_folding_ranges(node->as.try_catch.try_body, ranges);
            collect_folding_ranges(node->as.try_catch.catch_body, ranges);
            collect_folding_ranges(node->as.try_catch.finally_body, ranges);
            break;
        case AST_MATCH_EXPR:
            ADD_NODE_FOLD(ranges, node->line - 1, node, "region");
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                collect_folding_ranges(node->as.match_expr.arms[i], ranges);
            }
            break;
        default:
            break;
    }
}

#undef ADD_NODE_FOLD

XrJsonValue *xlsp_handle_folding_range(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    if (!textDocument)
        return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc)
        return xjson_new_array();

    XrJsonValue *ranges = xjson_new_array();

    if (!doc->ast)
        return ranges;

    collect_folding_ranges(doc->ast, ranges);

    return ranges;
}
