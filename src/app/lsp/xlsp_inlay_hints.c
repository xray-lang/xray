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
#include "xlsp_utils.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/analyzer/xa_effect_db.h"
#include "../../frontend/analyzer/xanalyzer_symbol.h"
#include <stdlib.h>
#include <string.h>
#include "../../base/xmalloc.h"

// Create inlay hint JSON
static XrJsonValue *make_hint(int line, int character, const char *label, XlspInlayHintKind kind,
                              const char *tooltip) {
    XrJsonValue *hint = xjson_new_object();

    // Position
    XrJsonValue *pos = xjson_new_object();
    xjson_object_set(pos, "line", xjson_new_number(line));
    xjson_object_set(pos, "character", xjson_new_number(character));
    xjson_object_set(hint, "position", pos);

    // Label
    xjson_object_set(hint, "label", xjson_new_string(label));

    if (tooltip && tooltip[0]) {
        xjson_object_set(hint, "tooltip", xjson_new_string(tooltip));
    }

    // Kind
    xjson_object_set(hint, "kind", xjson_new_number(kind));

    // Padding
    if (kind == XLSP_HINT_TYPE) {
        xjson_object_set(hint, "paddingLeft", xjson_new_bool(true));
    } else {
        xjson_object_set(hint, "paddingRight", xjson_new_bool(true));
    }

    return hint;
}

static const char *param_mode_tooltip(XrParamMode mode, char *buf, size_t cap) {
    if (!buf || cap == 0 || mode == XR_PARAM_READ || !xr_param_mode_is_valid(mode))
        return NULL;
    snprintf(buf, cap, "parameter mode: %s", xr_param_mode_label(mode));
    return buf;
}

static XaSymbol *function_decl_symbol(XaAnalyzer *analyzer, AstNode *node) {
    if (!analyzer || !node || node->type != AST_FUNCTION_DECL)
        return NULL;
    uint32_t id = node->as.function_decl.symbol_id;
    XaSymbol *symbol = id ? xa_scope_lookup_by_id(analyzer->global_scope, id) : NULL;
    if (!symbol && node->as.function_decl.name)
        symbol = xa_analyzer_lookup_deep(analyzer, node->as.function_decl.name);
    return symbol;
}

static void format_throw_effect_hint(XaAnalyzer *analyzer, XaSymbol *symbol, char *label,
                                     size_t capacity) {
    if (!label || capacity == 0)
        return;
    label[0] = '\0';
    if (!analyzer || !symbol)
        return;
    if (symbol->links.throw_effect == XR_FN_EFFECT_NO_THROW) {
        snprintf(label, capacity, " · no_throw ✓");
        return;
    }
    size_t offset = (size_t) snprintf(label, capacity, " · may throw");
    const XaEffectSummary *summary = xa_effect_db_get(analyzer->effect_db, symbol->links.effect_id);
    if (!summary || summary->escaping.count == 0 || offset + 3 >= capacity)
        return;
    label[offset++] = ' ';
    label[offset++] = '{';
    bool wrote_name = false;
    for (uint32_t i = 0; i < summary->escaping.count && offset + 2 < capacity; i++) {
        XrType *type =
            xa_effect_db_error_type_handle(analyzer->effect_db, summary->escaping.types[i].type_id);
        const char *name = type && XR_TYPE_IS_ENUM(type) ? type->enum_type.enum_name : NULL;
        if (!name)
            continue;
        int written =
            snprintf(label + offset, capacity - offset, "%s%s", wrote_name ? ", " : "", name);
        if (written < 0 || (size_t) written >= capacity - offset) {
            label[capacity - 1] = '\0';
            return;
        }
        offset += (size_t) written;
        wrote_name = true;
    }
    if (offset + 1 < capacity) {
        label[offset++] = '}';
        label[offset] = '\0';
    }
}

// Check if line is in range
static bool in_range(int line, XrLspRange range) {
    return line >= (int) range.start.line && line <= (int) range.end.line;
}

// Find function declaration in AST by name
static AstNode *find_function_in_ast(AstNode *root, const char *name) {
    if (!root || !name)
        return NULL;

    if (root->type == AST_FUNCTION_DECL) {
        if (root->as.function_decl.name && strcmp(root->as.function_decl.name, name) == 0) {
            return root;
        }
    }

    // Recurse into children
    switch (root->type) {
        case AST_PROGRAM:
            for (int i = 0; i < root->as.program.count; i++) {
                AstNode *found = find_function_in_ast(root->as.program.statements[i], name);
                if (found)
                    return found;
            }
            break;
        case AST_BLOCK:
            for (int i = 0; i < root->as.block.count; i++) {
                AstNode *found = find_function_in_ast(root->as.block.statements[i], name);
                if (found)
                    return found;
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
    if (!node)
        return;

    int line = node->line - 1;  // Convert to 0-indexed

    switch (node->type) {
        case AST_PROGRAM: {
            int count = node->as.program.count;
            for (int i = 0; i < count; i++) {
                collect_hints(hints, node->as.program.statements[i], root, range, analyzer,
                              show_types, show_params);
            }
            break;
        }
        case AST_BLOCK: {
            int count = node->as.block.count;
            for (int i = 0; i < count; i++) {
                collect_hints(hints, node->as.block.statements[i], root, range, analyzer,
                              show_types, show_params);
            }
            break;
        }
        case AST_VAR_DECL:
        case AST_CONST_DECL: {
            // Show type hint if no type annotation and has initializer
            if (show_types && in_range(line, range) && !node->as.var_decl.type_annotation &&
                node->as.var_decl.initializer) {
                const char *name = node->as.var_decl.name;
                XrType *inferred = xa_analyzer_infer_expr_type(
                    analyzer, (XrAstNode *) node->as.var_decl.initializer);

                if (inferred && !(inferred->kind == XR_KIND_UNKNOWN)) {
                    const char *type_str = xr_type_to_string(inferred);
                    char label[64];
                    snprintf(label, sizeof(label), ": %s", type_str);

                    // Position after variable name, using AST column info
                    // node->column points to the start of var name (1-indexed)
                    int char_pos = (node->column > 0 ? node->column - 1 : 0) + strlen(name);
                    xjson_array_push(hints, make_hint(line, char_pos, label, XLSP_HINT_TYPE, NULL));
                }
            }
            collect_hints(hints, node->as.var_decl.initializer, root, range, analyzer, show_types,
                          show_params);
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
                    FunctionDeclNode *current_fn = NULL;
                    XrType *param_contract = NULL;
                    int param_count = 0;

                    if (fn_decl) {
                        current_fn = &fn_decl->as.function_decl;
                        param_count = current_fn->param_count;
                    } else if (analyzer) {
                        // Fall back to workspace analyzer for imported functions
                        XaSymbol *sym =
                            xa_analyzer_lookup_in_scope(analyzer, fn_name, analyzer->global_scope);
                        if (sym && (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD)) {
                            XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
                            if (links && links->param_names) {
                                param_names = links->param_names;
                                param_count = links->param_count;
                                param_contract = links->type;
                            }
                        }
                    }

                    if ((current_fn || param_names) && param_count > 0) {
                        // Show parameter name hints
                        int hint_count = node->as.call_expr.arg_count < param_count
                                             ? node->as.call_expr.arg_count
                                             : param_count;
                        for (int i = 0; i < hint_count; i++) {
                            const char *param_name = NULL;
                            XrParamMode mode = XR_PARAM_READ;
                            if (current_fn) {
                                XrParamNode *param = current_fn->params[i];
                                param_name = param ? param->name : NULL;
                                mode = param ? param->passing_mode : XR_PARAM_READ;
                            } else {
                                param_name = param_names[i];
                                mode = xlsp_function_param_mode(param_contract, i);
                            }
                            AstNode *arg = node->as.call_expr.arguments[i];
                            if (param_name && arg) {
                                // Skip if argument is already named (same as param)
                                if (arg->type == AST_VARIABLE && arg->as.variable.name &&
                                    strcmp(arg->as.variable.name, param_name) == 0) {
                                    continue;
                                }
                                // Create parameter name hint
                                char label[64];
                                snprintf(label, sizeof(label), "%s:", param_name);
                                int arg_line = arg->line - 1;
                                int arg_col = arg->column > 0 ? arg->column - 1 : 0;
                                char tooltip[64];
                                const char *tooltip_text =
                                    param_mode_tooltip(mode, tooltip, sizeof(tooltip));
                                xjson_array_push(hints,
                                                 make_hint(arg_line, arg_col, label,
                                                           XLSP_HINT_PARAMETER, tooltip_text));
                            }
                        }
                    }
                }
            }
            collect_hints(hints, node->as.call_expr.callee, root, range, analyzer, show_types,
                          show_params);
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                collect_hints(hints, node->as.call_expr.arguments[i], root, range, analyzer,
                              show_types, show_params);
            }
            break;
        }
        case AST_FUNCTION_DECL: {
            if (show_types && analyzer && in_range(line, range)) {
                XaSymbol *symbol = function_decl_symbol(analyzer, node);
                char label[256];
                format_throw_effect_hint(analyzer, symbol, label, sizeof(label));
                if (label[0]) {
                    const char *name = node->as.function_decl.name;
                    int char_pos = (node->column > 0 ? node->column - 1 : 0) +
                                   (name ? (int) strlen(name) + 3 : 0);
                    xjson_array_push(hints, make_hint(line, char_pos, label, XLSP_HINT_TYPE,
                                                      "inferred error effect"));
                }
            }
            collect_hints(hints, node->as.function_decl.body, root, range, analyzer, show_types,
                          show_params);
            break;
        }
        case AST_IF_STMT:
            collect_hints(hints, node->as.if_stmt.condition, root, range, analyzer, show_types,
                          show_params);
            collect_hints(hints, node->as.if_stmt.then_branch, root, range, analyzer, show_types,
                          show_params);
            collect_hints(hints, node->as.if_stmt.else_branch, root, range, analyzer, show_types,
                          show_params);
            break;
        case AST_WHILE_STMT:
            collect_hints(hints, node->as.while_stmt.condition, root, range, analyzer, show_types,
                          show_params);
            collect_hints(hints, node->as.while_stmt.body, root, range, analyzer, show_types,
                          show_params);
            break;
        case AST_FOR_STMT:
            collect_hints(hints, node->as.for_stmt.initializer, root, range, analyzer, show_types,
                          show_params);
            collect_hints(hints, node->as.for_stmt.condition, root, range, analyzer, show_types,
                          show_params);
            collect_hints(hints, node->as.for_stmt.increment, root, range, analyzer, show_types,
                          show_params);
            collect_hints(hints, node->as.for_stmt.body, root, range, analyzer, show_types,
                          show_params);
            break;
        case AST_EXPR_STMT:
            // Recurse into expression statement to find function calls
            collect_hints(hints, node->as.expr_stmt, root, range, analyzer, show_types,
                          show_params);
            break;
        default:
            break;
    }
}

// Analyze document for inlay hints
XrJsonValue *xlsp_analyze_inlay_hints(XrLspServer *server, XrLspDocument *doc, XrLspRange range) {
    XrJsonValue *hints = xjson_new_array();

    if (!doc || !doc->ast)
        return hints;

    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;

    // Get configuration
    bool show_types = server->config.inlay_hints_type_annotations;
    bool show_params = server->config.inlay_hints_parameter_names;

    // Type hints require analyzer, param hints can work with just AST
    if (!show_types && !show_params)
        return hints;
    if (show_types && !analyzer)
        show_types = false;  // Disable type hints if no analyzer

    // Collect hints from AST
    collect_hints(hints, doc->ast, doc->ast, range, analyzer, show_types, show_params);

    return hints;
}
