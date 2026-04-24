/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_navigation.c - Semantic navigation features
 *   definition, references, document highlight
 */

#include "xlsp_navigation.h"
#include "xlsp_server.h"
#include "xlsp_json.h"
#include "xlsp_analysis.h"
#include "xlsp_imports.h"
#include "xlsp_utils.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../frontend/parser/xast_api.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <string.h>

// ============================================================================
// Go to Definition
// ============================================================================

XrJsonValue *xlsp_analyze_definition(XrLspServer *server, XrLspDocument *doc, XrLspPosition pos) {
    if (!doc || !doc->content) return NULL;
    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;

    uint32_t start, end;
    char *word = xlsp_word_at_position(doc, pos, &start, &end);
    if (!word) return NULL;

    const char *content = doc->content;
    XrJsonValue *result = NULL;

    // First try XaAnalyzer for accurate definition lookup (position-aware)
    if (analyzer) {
        // lookup_at resolves the correct symbol even with shadowing
        XaSymbol *sym = xa_analyzer_lookup_at(analyzer, doc->uri, pos.line + 1, pos.character + 1);
        if (!sym) sym = xa_analyzer_lookup(analyzer, word);

        if (sym && sym->location.line > 0) {
            const char *target_uri = (sym->location.file && sym->location.file[0])
                ? sym->location.file : doc->uri;
            result = xlsp_json_new_object();
            xlsp_json_object_set(result, "uri", xlsp_json_new_string(target_uri));

            int col = sym->location.column > 0 ? sym->location.column - 1 : 0;
            xlsp_json_object_set(result, "range",
                xlsp_json_make_range(sym->location.line - 1, col,
                                     sym->location.line - 1, col + (int)strlen(sym->name)));
        }
    }

    // Fall back to lexer-based symbol table
    if (!result) {
        SymbolTable table;
        xlsp_symbol_table_init(&table);
        xlsp_extract_symbols(doc, &table);

        for (int i = 0; i < table.count; i++) {
            if (strcmp(table.entries[i].name, word) == 0) {
                SymbolEntry *entry = &table.entries[i];

                result = xlsp_json_new_object();
                xlsp_json_object_set(result, "uri", xlsp_json_new_string(doc->uri));

                xlsp_json_object_set(result, "range",
                    xlsp_json_make_range(entry->line, entry->start_char,
                                         entry->end_line, entry->end_char));

                break;
            }
        }
        xlsp_symbol_table_free(&table);
    }

    // If not found locally, check if it's a module.member pattern
    if (!result && start > 1 && content[start - 1] == '.') {
        uint32_t mod_end = start - 1;
        uint32_t mod_start = mod_end;
        while (mod_start > 0 && (content[mod_start - 1] == '_' ||
               (content[mod_start - 1] >= 'a' && content[mod_start - 1] <= 'z') ||
               (content[mod_start - 1] >= 'A' && content[mod_start - 1] <= 'Z') ||
               (content[mod_start - 1] >= '0' && content[mod_start - 1] <= '9'))) {
            mod_start--;
        }

        if (mod_start < mod_end) {
            size_t mod_len = mod_end - mod_start;
            char *mod_name = xr_malloc(mod_len + 1);
            if (mod_name) {
                memcpy(mod_name, content + mod_start, mod_len);
                mod_name[mod_len] = '\0';

                // Try to get definition from import
                result = xlsp_get_import_definition(doc, mod_name, word);

                xr_free(mod_name);
            }
        }
    }

    // Check if word is an imported module name (for "import xxx" goto definition)
    if (!result) {
        result = xlsp_get_module_file_location(doc, word);
    }

    xr_free(word);
    return result;
}

// ============================================================================
// Find References (cross-file, scope-aware)
// ============================================================================

// Helper: create a reference location JSON object
static XrJsonValue *make_ref_location(const char *uri, int line, int start_col, int end_col) {
    return xlsp_json_make_location(uri, line, start_col, line, end_col);
}

// Reference context for AST traversal
typedef struct {
    const char *target_name;    // Name to search for
    XaScope *def_scope;         // Scope where symbol is defined (for scoping)
    XaScope *current_scope;     // Current scope during traversal
    XaScope *global_scope;      // Global scope
    XrJsonValue *refs;          // Collected references (JSON array)
    const char *uri;            // Document URI
} RefFindContext;

// Forward declarations
static void collect_refs_from_ast(AstNode *node, RefFindContext *ctx);
static XaScope *find_child_scope_for_refs(XaScope *parent, void *ast_node);

// Check if the current scope can see the definition scope (handles shadowing)
static bool can_see_definition(RefFindContext *ctx) {
    if (!ctx->def_scope || !ctx->current_scope) return false;

    // Check if current scope is the definition scope or a descendant
    if (!xa_scope_is_descendant(ctx->current_scope, ctx->def_scope)) {
        return false;
    }

    // Check for shadowing: if current scope has a local definition with same name,
    // and it's not our target definition's scope, then it shadows our target
    if (ctx->current_scope != ctx->def_scope) {
        XaSymbol *local = xa_scope_lookup_local(ctx->current_scope, ctx->target_name);
        if (local) {
            // There's a local definition in current scope - it shadows our target
            return false;
        }
    }

    return true;
}

// Helper: find child scope by AST node (same as rename)
static XaScope *find_child_scope_for_refs(XaScope *parent, void *ast_node) {
    if (!parent) return NULL;
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i]->ast_node == ast_node) {
            return parent->children[i];
        }
    }
    return NULL;
}

// Add a reference if we're in the right scope
static void add_ref_if_visible(RefFindContext *ctx, int line, int col, int name_len) {
    if (can_see_definition(ctx)) {
        XrJsonValue *loc = make_ref_location(ctx->uri, line - 1, col - 1, col - 1 + name_len);
        xlsp_json_array_push(ctx->refs, loc);
    }
}

// Collect references from AST with scope tracking
static void collect_refs_from_ast(AstNode *node, RefFindContext *ctx) {
    if (!node) return;

    switch (node->type) {
        case AST_PROGRAM:
            ctx->current_scope = ctx->global_scope;
            for (int i = 0; i < node->as.program.count; i++) {
                collect_refs_from_ast(node->as.program.statements[i], ctx);
            }
            break;

        case AST_FUNCTION_DECL: {
            FunctionDeclNode *fn = &node->as.function_decl;
            // Check function name (defined in parent scope)
            if (fn->name && strcmp(fn->name, ctx->target_name) == 0) {
                add_ref_if_visible(ctx, node->line, node->column > 0 ? node->column : 1,
                                   (int)strlen(fn->name));
            }

            // Enter function scope
            XaScope *saved_scope = ctx->current_scope;
            XaScope *fn_scope = find_child_scope_for_refs(ctx->current_scope, node);
            if (fn_scope) ctx->current_scope = fn_scope;

            // Check parameters
            for (int i = 0; i < fn->param_count; i++) {
                XrParamNode *param = fn->params[i];
                if (param && param->name && strcmp(param->name, ctx->target_name) == 0) {
                    add_ref_if_visible(ctx, param->line, param->column > 0 ? param->column : 1,
                                       (int)strlen(param->name));
                }
            }

            collect_refs_from_ast(fn->body, ctx);
            ctx->current_scope = saved_scope;
            break;
        }

        case AST_VAR_DECL:
        case AST_CONST_DECL: {
            VarDeclNode *var = &node->as.var_decl;
            if (var->name && strcmp(var->name, ctx->target_name) == 0) {
                add_ref_if_visible(ctx, node->line, node->column > 0 ? node->column : 1,
                                   (int)strlen(var->name));
            }
            collect_refs_from_ast(var->initializer, ctx);
            break;
        }

        case AST_VARIABLE: {
            if (node->as.variable.name && strcmp(node->as.variable.name, ctx->target_name) == 0) {
                add_ref_if_visible(ctx, node->line, node->column > 0 ? node->column : 1,
                                   (int)strlen(node->as.variable.name));
            }
            break;
        }

        case AST_ASSIGNMENT: {
            if (node->as.assignment.name && strcmp(node->as.assignment.name, ctx->target_name) == 0) {
                add_ref_if_visible(ctx, node->line, node->column > 0 ? node->column : 1,
                                   (int)strlen(node->as.assignment.name));
            }
            collect_refs_from_ast(node->as.assignment.value, ctx);
            break;
        }

        case AST_BLOCK: {
            XaScope *saved_scope = ctx->current_scope;
            XaScope *block_scope = find_child_scope_for_refs(ctx->current_scope, node);
            if (block_scope) ctx->current_scope = block_scope;

            for (int i = 0; i < node->as.block.count; i++) {
                collect_refs_from_ast(node->as.block.statements[i], ctx);
            }
            ctx->current_scope = saved_scope;
            break;
        }

        case AST_IF_STMT:
            collect_refs_from_ast(node->as.if_stmt.condition, ctx);
            collect_refs_from_ast(node->as.if_stmt.then_branch, ctx);
            collect_refs_from_ast(node->as.if_stmt.else_branch, ctx);
            break;

        case AST_WHILE_STMT:
            collect_refs_from_ast(node->as.while_stmt.condition, ctx);
            collect_refs_from_ast(node->as.while_stmt.body, ctx);
            break;

        case AST_FOR_STMT: {
            XaScope *saved_scope = ctx->current_scope;
            XaScope *for_scope = find_child_scope_for_refs(ctx->current_scope, node);
            if (for_scope) ctx->current_scope = for_scope;

            collect_refs_from_ast(node->as.for_stmt.initializer, ctx);
            collect_refs_from_ast(node->as.for_stmt.condition, ctx);
            collect_refs_from_ast(node->as.for_stmt.increment, ctx);
            collect_refs_from_ast(node->as.for_stmt.body, ctx);
            ctx->current_scope = saved_scope;
            break;
        }

        case AST_FOR_IN_STMT: {
            ForInStmtNode *fi = &node->as.for_in_stmt;
            collect_refs_from_ast(fi->collection, ctx);

            XaScope *saved_scope = ctx->current_scope;
            XaScope *for_scope = find_child_scope_for_refs(ctx->current_scope, node);
            if (for_scope) ctx->current_scope = for_scope;

            // Check loop variable
            if (fi->item_name && strcmp(fi->item_name, ctx->target_name) == 0) {
                add_ref_if_visible(ctx, node->line, node->column > 0 ? node->column : 1,
                                   (int)strlen(fi->item_name));
            }

            collect_refs_from_ast(fi->body, ctx);
            ctx->current_scope = saved_scope;
            break;
        }

        case AST_FUNCTION_EXPR: {
            FunctionDeclNode *fn_expr = &node->as.function_expr;

            XaScope *saved_scope = ctx->current_scope;
            XaScope *fn_scope = find_child_scope_for_refs(ctx->current_scope, node);
            if (fn_scope) ctx->current_scope = fn_scope;

            // Check parameters
            for (int i = 0; i < fn_expr->param_count; i++) {
                XrParamNode *param = fn_expr->params[i];
                if (param && param->name && strcmp(param->name, ctx->target_name) == 0) {
                    add_ref_if_visible(ctx, param->line, param->column > 0 ? param->column : 1,
                                       (int)strlen(param->name));
                }
            }

            collect_refs_from_ast(fn_expr->body, ctx);
            ctx->current_scope = saved_scope;
            break;
        }

        case AST_EXPR_STMT:
            collect_refs_from_ast(node->as.expr_stmt, ctx);
            break;

        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++) {
                collect_refs_from_ast(node->as.print_stmt.exprs[i], ctx);
            }
            break;

        case AST_CALL_EXPR:
            collect_refs_from_ast(node->as.call_expr.callee, ctx);
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                collect_refs_from_ast(node->as.call_expr.arguments[i], ctx);
            }
            break;

        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            collect_refs_from_ast(node->as.binary.left, ctx);
            collect_refs_from_ast(node->as.binary.right, ctx);
            break;

        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
            collect_refs_from_ast(node->as.unary.operand, ctx);
            break;

        case AST_INDEX_GET:
            collect_refs_from_ast(node->as.index_get.array, ctx);
            collect_refs_from_ast(node->as.index_get.index, ctx);
            break;

        case AST_MEMBER_ACCESS:
            collect_refs_from_ast(node->as.member_access.object, ctx);
            // Don't check member name - it's a different symbol
            break;

        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++) {
                collect_refs_from_ast(node->as.return_stmt.values[i], ctx);
            }
            break;

        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            // Check class/struct name
            if (node->as.class_decl.name &&
                strcmp(node->as.class_decl.name, ctx->target_name) == 0) {
                add_ref_if_visible(ctx, node->line, node->column > 0 ? node->column : 1,
                                   (int)strlen(node->as.class_decl.name));
            }

            // Process methods
            for (int i = 0; i < node->as.class_decl.method_count; i++) {
                collect_refs_from_ast(node->as.class_decl.methods[i], ctx);
            }
            break;
        }

        default:
            break;
    }
}

// Helper: lexer-based fallback scan for references (when AST/analyzer unavailable)
// Uses line_offsets for O(1) column calculation instead of O(N) linear scan.
static void scan_doc_for_refs_lexer(XrLspDocument *doc, const char *search_word,
                                     size_t word_len, XrJsonValue *refs) {
    if (!doc || !doc->content || !search_word) return;

    Scanner scanner;
    xr_scanner_init(&scanner, doc->content);

    Token token;
    while (1) {
        token = xr_scanner_scan(&scanner);
        if (token.type == TK_EOF) break;
        if (token.type == TK_ERROR) continue;

        if (token.type == TK_NAME &&
            (size_t)token.length == word_len &&
            strncmp(token.start, search_word, word_len) == 0) {

            // O(1) column calculation using line_offsets
            int line_idx = token.line - 1;
            int char_pos = 0;
            if (doc->line_offsets && line_idx >= 0 && line_idx < doc->line_count) {
                uint32_t token_offset = (uint32_t)(token.start - doc->content);
                char_pos = (int)(token_offset - doc->line_offsets[line_idx]);
            } else {
                // Fallback: linear scan (only if line_offsets unavailable)
                const char *line_start = doc->content;
                const char *p = doc->content;
                while (p < token.start) {
                    if (*p == '\n') line_start = p + 1;
                    p++;
                }
                char_pos = (int)(token.start - line_start);
            }

            XrJsonValue *loc = make_ref_location(doc->uri, line_idx, char_pos, char_pos + token.length);
            xlsp_json_array_push(refs, loc);
        }
    }
}

XrJsonValue *xlsp_analyze_references(XrLspServer *server, XrLspDocument *doc, XrLspPosition pos) {
    XrJsonValue *refs = xlsp_json_new_array();

    if (!doc || !doc->content) return refs;

    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;

    uint32_t start, end;
    char *search_word = xlsp_word_at_position(doc, pos, &start, &end);
    if (!search_word) return refs;

    size_t word_len = end - start;

    // =========================================================================
    // Scope-aware reference finding using AST and XaAnalyzer
    // =========================================================================

    bool used_semantic_search = false;

    if (doc->ast && analyzer && analyzer->global_scope) {
        // Find the scope where the symbol at cursor is defined
        XaScope *def_scope = NULL;

        // First try to find by position (more accurate)
        XaSymbol *sym = xa_analyzer_lookup_at(analyzer, doc->uri, pos.line + 1, pos.character + 1);

        // If not found by position, try by name
        if (!sym) {
            sym = xa_analyzer_lookup(analyzer, search_word);
        }

        if (sym) {
            // Find the scope where this symbol is defined
            def_scope = xa_scope_find_definition(analyzer->global_scope, sym->name);
            if (!def_scope) {
                def_scope = analyzer->global_scope;
            }

            // Use scope-aware AST traversal
            RefFindContext ctx = {
                .target_name = search_word,
                .def_scope = def_scope,
                .current_scope = analyzer->global_scope,
                .global_scope = analyzer->global_scope,
                .refs = refs,
                .uri = doc->uri
            };

            collect_refs_from_ast(doc->ast, &ctx);
            used_semantic_search = true;

            lsp_log("References (semantic): found %d refs for '%s' in %s",
                    xlsp_json_array_len(refs), search_word, doc->uri);
        }
    }

    // Fallback to lexer-based search if semantic search not available
    if (!used_semantic_search) {
        scan_doc_for_refs_lexer(doc, search_word, word_len, refs);
        lsp_log("References (lexer fallback): found %d refs for '%s' in %s",
                xlsp_json_array_len(refs), search_word, doc->uri);
    }

    // =========================================================================
    // Cross-file reference search via analyzer dependency graph.
    // xa_analyzer_find_references_at covers all indexed files (not just
    // open documents), giving uniform behaviour for opened/unopened files.
    // =========================================================================

    if (analyzer) {
        int ref_count = 0;
        XaSymbolRef *arefs = xa_analyzer_find_references_at(
            analyzer, doc->uri, pos.line + 1, pos.character + 1, &ref_count);

        for (XaSymbolRef *r = arefs; r; r = r->next) {
            // Skip refs already covered by the in-document search above
            if (r->file && strcmp(r->file, doc->uri) == 0) continue;

            const char *ref_uri = r->file ? r->file : doc->uri;
            int line = r->line > 0 ? (int)r->line - 1 : 0;
            int col  = r->column > 0 ? (int)r->column - 1 : 0;
            int end_col = col + (int)strlen(search_word);

            XrJsonValue *loc = xlsp_json_new_object();
            xlsp_json_object_set(loc, "uri", xlsp_json_new_string(ref_uri));
            xlsp_json_object_set(loc, "range",
                xlsp_json_make_range(line, col, line, end_col));
            xlsp_json_array_push(refs, loc);
        }

        if (ref_count > 0) {
            lsp_log("References (cross-file analyzer): %d refs for '%s'",
                    ref_count, search_word);
        }

        xa_analyzer_free_references(arefs);
    }

    xr_free(search_word);
    return refs;
}

// ============================================================================
// Document Highlight (scope-aware, single-file)
// ============================================================================

XrJsonValue *xlsp_analyze_document_highlight(XrLspServer *server, XrLspDocument *doc, XrLspPosition pos) {
    XrJsonValue *highlights = xlsp_json_new_array();
    if (!doc || !doc->content) return highlights;

    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;

    uint32_t start, end;
    char *word = xlsp_word_at_position(doc, pos, &start, &end);
    if (!word) return highlights;

    size_t word_len = end - start;
    bool used_semantic = false;

    // Scope-aware highlight using analyzer
    if (doc->ast && analyzer && analyzer->global_scope) {
        XaSymbol *sym = xa_analyzer_lookup_at(analyzer, doc->uri, pos.line + 1, pos.character + 1);
        if (!sym) sym = xa_analyzer_lookup(analyzer, word);

        if (sym) {
            XaScope *def_scope = xa_scope_find_definition(analyzer->global_scope, sym->name);
            if (!def_scope) def_scope = analyzer->global_scope;

            // Collect references in this document only
            XrJsonValue *refs = xlsp_json_new_array();
            RefFindContext ctx = {
                .target_name = word,
                .def_scope = def_scope,
                .current_scope = analyzer->global_scope,
                .global_scope = analyzer->global_scope,
                .refs = refs,
                .uri = doc->uri
            };
            collect_refs_from_ast(doc->ast, &ctx);

            // Convert Location objects to DocumentHighlight objects
            for (int i = 0; i < xlsp_json_array_len(refs); i++) {
                XrJsonValue *loc = xlsp_json_array_get(refs, i);
                XrJsonValue *range = xlsp_json_get_object(loc, "range");
                if (range) {
                    XrJsonValue *hl = xlsp_json_new_object();
                    // Deep copy range since we'll free refs
                    XrJsonValue *r_start = xlsp_json_get_object(range, "start");
                    XrJsonValue *r_end = xlsp_json_get_object(range, "end");
                    xlsp_json_object_set(hl, "range",
                        xlsp_json_make_range(
                            xlsp_json_get_int(r_start, "line"),
                            xlsp_json_get_int(r_start, "character"),
                            xlsp_json_get_int(r_end, "line"),
                            xlsp_json_get_int(r_end, "character")));

                    // Classify: definition vs read
                    int kind = LSP_HIGHLIGHT_READ;
                    if (sym->location.line > 0 && sym->location.column > 0) {
                        int def_line = sym->location.line - 1;
                        int def_col = sym->location.column - 1;
                        if (xlsp_json_get_int(r_start, "line") == def_line &&
                            xlsp_json_get_int(r_start, "character") == def_col) {
                            kind = LSP_HIGHLIGHT_WRITE;
                        }
                    }
                    xlsp_json_object_set(hl, "kind", xlsp_json_new_number(kind));
                    xlsp_json_array_push(highlights, hl);
                }
            }
            xlsp_json_free(refs);
            used_semantic = true;
        }
    }

    // Fallback: lexer-based word scan
    if (!used_semantic) {
        Scanner scanner;
        xr_scanner_init(&scanner, doc->content);
        Token token;
        while (1) {
            token = xr_scanner_scan(&scanner);
            if (token.type == TK_EOF) break;
            if (token.type == TK_ERROR) continue;
            if (token.type == TK_NAME &&
                (size_t)token.length == word_len &&
                strncmp(token.start, word, word_len) == 0) {
                const char *line_start = doc->content;
                const char *p = doc->content;
                while (p < token.start) {
                    if (*p == '\n') line_start = p + 1;
                    p++;
                }
                int col = (int)(token.start - line_start);
                XrJsonValue *hl = xlsp_json_new_object();
                xlsp_json_object_set(hl, "range",
                    xlsp_json_make_range(token.line - 1, col, token.line - 1, col + token.length));
                xlsp_json_object_set(hl, "kind", xlsp_json_new_number(LSP_HIGHLIGHT_TEXT));
                xlsp_json_array_push(highlights, hl);
            }
        }
    }

    xr_free(word);
    return highlights;
}

// Rename moved to xlsp_rename.c
