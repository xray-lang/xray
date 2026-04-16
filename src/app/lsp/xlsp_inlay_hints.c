/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_inlay_hints.c - Inlay hints implementation
 */

#include "xlsp_inlay_hints.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include <stdlib.h>
#include <string.h>
#include "../../base/xmalloc.h"

// Create inlay hint JSON
static XrJsonValue *make_hint(int line, int character, const char *label, 
                               XlspInlayHintKind kind) {
    XrJsonValue *hint = xlsp_json_new_object();
    
    // Position
    XrJsonValue *pos = xlsp_json_new_object();
    xlsp_json_object_set(pos, "line", xlsp_json_new_number(line));
    xlsp_json_object_set(pos, "character", xlsp_json_new_number(character));
    xlsp_json_object_set(hint, "position", pos);
    
    // Label
    xlsp_json_object_set(hint, "label", xlsp_json_new_string(label));
    
    // Kind
    xlsp_json_object_set(hint, "kind", xlsp_json_new_number(kind));
    
    // Padding
    if (kind == XLSP_HINT_TYPE) {
        xlsp_json_object_set(hint, "paddingLeft", xlsp_json_new_bool(true));
    } else {
        xlsp_json_object_set(hint, "paddingRight", xlsp_json_new_bool(true));
    }
    
    return hint;
}

// Check if line is in range
static bool in_range(int line, XrLspRange range) {
    return line >= (int)range.start.line && line <= (int)range.end.line;
}

// Find function declaration in AST by name
static AstNode *find_function_in_ast(AstNode *root, const char *name) {
    if (!root || !name) return NULL;
    
    if (root->type == AST_FUNCTION_DECL) {
        if (root->as.function_decl.name && 
            strcmp(root->as.function_decl.name, name) == 0) {
            return root;
        }
    }
    
    // Recurse into children
    switch (root->type) {
        case AST_PROGRAM:
            for (int i = 0; i < root->as.program.count; i++) {
                AstNode *found = find_function_in_ast(root->as.program.statements[i], name);
                if (found) return found;
            }
            break;
        case AST_BLOCK:
            for (int i = 0; i < root->as.block.count; i++) {
                AstNode *found = find_function_in_ast(root->as.block.statements[i], name);
                if (found) return found;
            }
            break;
        default:
            break;
    }
    return NULL;
}

// Collect hints from AST
static void collect_hints(XrJsonValue *hints, AstNode *node, AstNode *root, XrLspRange range,
                          XaAnalyzer *analyzer, bool show_types, bool show_params) {
    if (!node) return;
    
    int line = node->line - 1;  // Convert to 0-indexed
    
    switch (node->type) {
        case AST_PROGRAM: {
            int count = node->as.program.count;
            for (int i = 0; i < count; i++) {
                collect_hints(hints, node->as.program.statements[i], root, range, analyzer, show_types, show_params);
            }
            break;
        }
        case AST_BLOCK: {
            int count = node->as.block.count;
            for (int i = 0; i < count; i++) {
                collect_hints(hints, node->as.block.statements[i], root, range, analyzer, show_types, show_params);
            }
            break;
        }
        case AST_VAR_DECL: {
            // Show type hint if no type annotation and has initializer
            if (show_types && in_range(line, range) && 
                !node->as.var_decl.type_annotation && 
                node->as.var_decl.initializer) {
                
                const char *name = node->as.var_decl.name;
                XrType *inferred = xa_analyzer_infer_expr_type(analyzer, 
                    (XrAstNode*)node->as.var_decl.initializer);
                
                if (inferred && !(inferred->kind == XR_KIND_UNKNOWN)) {
                    const char *type_str = xr_type_to_string(inferred);
                    char label[64];
                    snprintf(label, sizeof(label), ": %s", type_str);
                    
                    // Position after variable name, using AST column info
                    // node->column points to the start of var name (1-indexed)
                    int char_pos = (node->column > 0 ? node->column - 1 : 0) + strlen(name);
                    xlsp_json_array_push(hints, make_hint(line, char_pos, label, XLSP_HINT_TYPE));
                }
            }
            collect_hints(hints, node->as.var_decl.initializer, root, range, analyzer, show_types, show_params);
            break;
        }
        case AST_CALL_EXPR: {
            // Show parameter name hints for function calls
            if (show_params && in_range(line, range) && node->as.call_expr.arg_count > 0) {
                AstNode *callee = node->as.call_expr.callee;
                if (callee && callee->type == AST_VARIABLE) {
                    const char *fn_name = callee->as.variable.name;
                    
                    // First try to find function in current AST (most up-to-date)
                    AstNode *fn_decl = find_function_in_ast(root, fn_name);
                    const char **param_names = NULL;
                    const char **temp_names = NULL;
                    int param_count = 0;
                    
                    if (fn_decl) {
                        // Use params from current AST - extract names to temp array
                        FunctionDeclNode *fn = &fn_decl->as.function_decl;
                        param_count = fn->param_count;
                        if (param_count > 0) {
                            temp_names = xr_malloc(sizeof(const char*) * param_count);
                            for (int j = 0; j < param_count; j++) {
                                temp_names[j] = fn->params[j] ? fn->params[j]->name : NULL;
                            }
                            param_names = temp_names;
                        }
                    } else if (analyzer) {
                        // Fall back to workspace analyzer for imported functions
                        XaSymbol *sym = xa_analyzer_lookup_in_scope(analyzer, fn_name, 
                                                                     analyzer->global_scope);
                        if (sym && (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD)) {
                            XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
                            if (links && links->param_names) {
                                param_names = links->param_names;
                                param_count = links->param_count;
                            }
                        }
                    }
                    
                    if (param_names && param_count > 0) {
                        // Show parameter name hints
                        int hint_count = node->as.call_expr.arg_count < param_count ?
                                         node->as.call_expr.arg_count : param_count;
                        for (int i = 0; i < hint_count; i++) {
                            const char *param_name = param_names[i];
                            AstNode *arg = node->as.call_expr.arguments[i];
                            if (param_name && arg) {
                                // Skip if argument is already named (same as param)
                                if (arg->type == AST_VARIABLE && 
                                    arg->as.variable.name &&
                                    strcmp(arg->as.variable.name, param_name) == 0) {
                                    continue;
                                }
                                // Create parameter name hint
                                char label[64];
                                snprintf(label, sizeof(label), "%s:", param_name);
                                int arg_line = arg->line - 1;
                                int arg_col = arg->column > 0 ? arg->column - 1 : 0;
                                xlsp_json_array_push(hints, 
                                    make_hint(arg_line, arg_col, label, XLSP_HINT_PARAMETER));
                            }
                        }
                    }
                    if (temp_names) xr_free((void*)temp_names);
                }
            }
            collect_hints(hints, node->as.call_expr.callee, root, range, analyzer, show_types, show_params);
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                collect_hints(hints, node->as.call_expr.arguments[i], root, range, analyzer, show_types, show_params);
            }
            break;
        }
        case AST_FUNCTION_DECL:
            collect_hints(hints, node->as.function_decl.body, root, range, analyzer, show_types, show_params);
            break;
        case AST_IF_STMT:
            collect_hints(hints, node->as.if_stmt.condition, root, range, analyzer, show_types, show_params);
            collect_hints(hints, node->as.if_stmt.then_branch, root, range, analyzer, show_types, show_params);
            collect_hints(hints, node->as.if_stmt.else_branch, root, range, analyzer, show_types, show_params);
            break;
        case AST_WHILE_STMT:
            collect_hints(hints, node->as.while_stmt.condition, root, range, analyzer, show_types, show_params);
            collect_hints(hints, node->as.while_stmt.body, root, range, analyzer, show_types, show_params);
            break;
        case AST_FOR_STMT:
            collect_hints(hints, node->as.for_stmt.initializer, root, range, analyzer, show_types, show_params);
            collect_hints(hints, node->as.for_stmt.condition, root, range, analyzer, show_types, show_params);
            collect_hints(hints, node->as.for_stmt.increment, root, range, analyzer, show_types, show_params);
            collect_hints(hints, node->as.for_stmt.body, root, range, analyzer, show_types, show_params);
            break;
        case AST_EXPR_STMT:
            // Recurse into expression statement to find function calls
            collect_hints(hints, node->as.expr_stmt, root, range, analyzer, show_types, show_params);
            break;
        default:
            break;
    }
}

// Analyze document for inlay hints
XrJsonValue *xlsp_analyze_inlay_hints(XrLspServer *server, XrLspDocument *doc, XrLspRange range) {
    XrJsonValue *hints = xlsp_json_new_array();
    
    if (!doc || !doc->ast) return hints;
    
    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;
    
    // Get configuration
    bool show_types = server->config.inlay_hints_type_annotations;
    bool show_params = server->config.inlay_hints_parameter_names;
    
    // Type hints require analyzer, param hints can work with just AST
    if (!show_types && !show_params) return hints;
    if (show_types && !analyzer) show_types = false;  // Disable type hints if no analyzer
    
    // Collect hints from AST
    collect_hints(hints, doc->ast, doc->ast, range, analyzer, show_types, show_params);
    
    return hints;
}
