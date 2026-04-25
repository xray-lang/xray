/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_analysis.c - Code analysis implementation
 */

#include "xlsp_analysis.h"
#include "xlsp_ast_utils.h"
#include "xlsp_cache.h"
#include "../../base/xjson.h"
#include "xlsp_workspace.h"
#include "xlsp_utils.h"
#include <string.h>
#include <stdio.h>
#include "../../base/xmalloc.h"

// xray parser headers
#include "../../frontend/lexer/xlex.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast.h"
#include "../../frontend/parser/xast_api.h"
#include "../../frontend/parser/xast_nodes.h"
#include "xlsp_stdlib.h"
#include "xlsp_imports.h"
#include "xlsp_builtins.h"

// Static analyzer
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/analyzer/xanalyzer_escape.h"
#include "../../runtime/value/xtype.h"

// AST formatter (moved from app/cli to frontend/format in CLI Phase 2)
#include "../../frontend/format/xfmt.h"
#include "../../frontend/analyzer/xanalyzer_ast_visitor.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/value/xtype_pool.h"

// Use analyzer's scope system for semantic rename (unified design)

// Data-driven keyword/builtin documentation table
typedef struct {
    const char *name;
    const char *doc;
} XlspDocEntry;

static const XlspDocEntry keyword_docs[] = {
    {"let",     "```xray\nlet <name> = <value>\n```\n\nDeclares a mutable variable."},
    {"const",   "```xray\nconst <name> = <value>\n```\n\nDeclares an immutable constant."},
    {"fn",      "```xray\nfn <name>(<params>) { <body> }\n```\n\nDeclares a function."},
    {"class",   "```xray\nclass <name> { <members> }\n```\n\nDeclares a class."},
    {"go",      "```xray\ngo <expr>\n```\n\nSpawns a new coroutine."},
    {"await",   "```xray\nawait <task>\n```\n\nWaits for a coroutine to complete."},
    {"Channel", "```xray\nChannel(size?)\n```\n\nCreates a channel for coroutine communication."},
    {"shared",  "```xray\nshared const|let <name> = <value>\n```\n\nDeclares a variable shared across coroutines."},
    {"if",      "```xray\nif (<cond>) { ... } else { ... }\n```\n\nConditional statement."},
    {"else",    "```xray\nif (<cond>) { ... } else { ... }\n```\n\nAlternate branch of if statement."},
    {"while",   "```xray\nwhile (<cond>) { ... }\n```\n\nLoop while condition is true."},
    {"for",     "```xray\nfor (<init>; <cond>; <step>) { ... }\nfor (<item> in <iter>) { ... }\n```\n\nLoop statement."},
    {"return",  "```xray\nreturn <value>?\n```\n\nReturns from a function."},
    {"break",   "```xray\nbreak\n```\n\nExits the innermost loop."},
    {"continue","```xray\ncontinue\n```\n\nSkips to the next loop iteration."},
    {"import",  "```xray\nimport <module>\n```\n\nImports a module."},
    {"true",    "Boolean literal `true`."},
    {"false",   "Boolean literal `false`."},
    {"null",    "Null literal representing absence of value."},
    {"new",     "```xray\nnew <Class>(<args>)\n```\n\nCreates a new class instance."},
    {"this",    "Reference to the current class instance."},
    {"super",   "Reference to the parent class."},
    {NULL, NULL}
};

static const XlspDocEntry builtin_docs[] = {
    {"print",   "```xray\nprint(value, ...)\n```\n\nPrints values to stdout."},
    {"typeof",  "```xray\ntypeof(value): string\n```\n\nReturns the type name of a value."},
    {"assert",  "```xray\nassert(condition, message?)\n```\n\nAsserts that condition is true."},
    {"assert_true",  "```xray\nassert_true(value)\n```\n\nAsserts that value is truthy."},
    {"assert_false",  "```xray\nassert_false(value)\n```\n\nAsserts that value is falsy."},
    {"assert_eq",  "```xray\nassert_eq(actual, expected)\n```\n\nAsserts that actual equals expected."},
    {"assert_ne",  "```xray\nassert_ne(actual, unexpected)\n```\n\nAsserts that actual does not equal unexpected."},
    {"len",     "```xray\nlen(value): int\n```\n\nReturns the length of a string, array, or map."},
    {"range",   "```xray\nrange(start, end, step?): Array\n```\n\nCreates an array of numbers."},
    {"int",     "```xray\nint(value): int\n```\n\nConverts value to integer."},
    {"float",   "```xray\nfloat(value): float\n```\n\nConverts value to float."},
    {"string",  "```xray\nstring(value): string\n```\n\nConverts value to string."},
    {"input",   "```xray\ninput(prompt?): string\n```\n\nReads a line from stdin."},
    {"sleep",   "```xray\nsleep(ms)\n```\n\nPauses execution for milliseconds."},
    {NULL, NULL}
};

static const char *lookup_doc(const XlspDocEntry *table, const char *name) {
    for (int i = 0; table[i].name; i++) {
        if (strcmp(table[i].name, name) == 0) {
            return table[i].doc;
        }
    }
    return NULL;
}

// Shared keyword definitions
#include "xlsp_keywords.h"

// xlsp_is_ident_char now in xlsp_ast_utils.h

char *xlsp_word_at_position(XrLspDocument *doc, XrLspPosition pos,
                            uint32_t *out_start, uint32_t *out_end) {
    if (!doc || !doc->content) return NULL;

    uint32_t offset = xlsp_position_to_offset(doc, pos);
    if (offset >= doc->length) return NULL;

    const char *content = doc->content;
    uint32_t start = offset;
    uint32_t end = offset;

    while (start > 0 && xlsp_is_ident_char(content[start - 1])) start--;
    while (end < doc->length && xlsp_is_ident_char(content[end])) end++;

    if (start == end) return NULL;

    size_t word_len = end - start;
    char *word = xr_malloc(word_len + 1);
    if (!word) return NULL;
    memcpy(word, content + start, word_len);
    word[word_len] = '\0';

    if (out_start) *out_start = start;
    if (out_end) *out_end = end;
    return word;
}

// Create a diagnostic JSON object
static XrJsonValue *make_diagnostic(int start_line, int start_char,
                                     int end_line, int end_char,
                                     int severity, const char *message) {
    XrJsonValue *diag = xjson_new_object();

    xjson_object_set(diag, "range", xjson_make_range(start_line, start_char, end_line, end_char));

    // Severity (1=Error, 2=Warning, 3=Info, 4=Hint)
    xjson_object_set(diag, "severity", xjson_new_number(severity));

    // Source
    xjson_object_set(diag, "source", xjson_new_string("xray"));

    // Message
    xjson_object_set(diag, "message", xjson_new_string(message));

    return diag;
}

// ============================================================================
// Document Parsing with Error Recovery
// ============================================================================

// Error collection context for LSP
typedef struct {
    XrJsonValue *diagnostics;
} LspErrorContext;

// Error callback for parser
static void lsp_error_callback(void *user_data, int line, int column,
                                int end_line, int end_column,
                                const char *message) {
    LspErrorContext *ctx = (LspErrorContext *)user_data;

    // Create diagnostic (LSP uses 0-indexed lines)
    XrJsonValue *diag = make_diagnostic(
        line - 1, column,
        end_line - 1, end_column,
        1,  // Error severity
        message
    );

    xjson_array_push(ctx->diagnostics, diag);
}

// lsp_log declared in xlsp_server.h (included via xlsp_analysis.h)

// Parse and index imported files on demand
static void index_imports_on_demand(XrLspServer *server, AstNode *ast, const char *base_uri) {
    if (!server || !ast || !base_uri || !server->workspace_analyzer) return;
    if (ast->type != AST_PROGRAM) return;

    // Extract base directory from URI
    char base_dir[XLSP_MAX_PATH];
    const char *uri_path = base_uri;
    uri_path = xlsp_uri_to_path(uri_path);
    strncpy(base_dir, uri_path, sizeof(base_dir) - 1);
    base_dir[sizeof(base_dir) - 1] = '\0';
    char *last_slash = strrchr(base_dir, '/');
    if (last_slash) *last_slash = '\0';

    // Scan for import statements
    for (int i = 0; i < ast->as.program.count; i++) {
        AstNode *stmt = ast->as.program.statements[i];
        if (!stmt || stmt->type != AST_IMPORT_STMT) continue;

        const char *import_path = stmt->as.import_stmt.module_name;
        if (!import_path) continue;

        // Skip stdlib imports
        if (import_path[0] != '.' && import_path[0] != '/') continue;

        // Resolve relative path
        char full_path[XLSP_MAX_PATH];
        if (import_path[0] == '/') {
            strncpy(full_path, import_path, sizeof(full_path) - 1);
        } else if (strncmp(import_path, "./", 2) == 0) {
            snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, import_path + 2);
        } else if (strncmp(import_path, "../", 3) == 0) {
            // Handle parent directory
            char parent_dir[XLSP_MAX_PATH];
            strncpy(parent_dir, base_dir, sizeof(parent_dir) - 1);
            const char *rel = import_path;
            while (strncmp(rel, "../", 3) == 0) {
                char *slash = strrchr(parent_dir, '/');
                if (slash) *slash = '\0';
                rel += 3;
            }
            snprintf(full_path, sizeof(full_path), "%s/%s", parent_dir, rel);
        } else {
            continue;
        }

        // Build import URI
        char import_uri[1100];
        snprintf(import_uri, sizeof(import_uri), "file://%s", full_path);

        // Check if document is already open
        XrLspDocument *open_doc = xlsp_document_get(server, import_uri);
        if (open_doc) {
            // Document is open - check if it needs re-analysis (dirty = has unsaved changes)
            if (open_doc->dirty && open_doc->ast) {
                lsp_log("import: %s is open and dirty, re-analyzing", full_path);
                xa_analyzer_refresh_file(server->workspace_analyzer, import_uri,
                                         (XrAstNode*)open_doc->ast, open_doc->content_hash);
                open_doc->dirty = false;
            } else {
                lsp_log("import: %s already open (symbols up to date)", full_path);
            }
            continue;
        }

        // Document not open - read from disk
        FILE *f = fopen(full_path, "r");
        if (!f) {
            lsp_log("import: cannot open %s", full_path);
            continue;
        }

        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        char *content = xr_malloc(size + 1);
        if (!content) {
            fclose(f);
            continue;
        }

        size_t read_size = fread(content, 1, size, f);
        content[read_size] = '\0';
        fclose(f);

        lsp_log("import: indexing from disk %s", full_path);

        // Ensure type pool is set
        XrTypePool *pool = xr_isolate_get_analyzer_pool(server->isolate);
        if (server->isolate && pool) {
            xr_type_set_current_pool(pool, &pool->next_type_id);
        }

        // Parse the imported file using a temporary arena
        XrArena temp_arena;
        xr_arena_init(&temp_arena, 64 * 1024);  // 64KB initial size

        Parser parser;
        xr_parser_init(&parser, server->isolate, content, import_uri, &temp_arena);

        // Set max errors to avoid getting stuck on very broken files
        parser.max_errors = 50;

        AstNode *import_ast = xr_parse_recoverable(&parser);

        // Analyze even with parse errors to get partial symbols for completion
        if (import_ast) {
            xa_analyzer_analyze(server->workspace_analyzer, import_uri, (XrAstNode*)import_ast);
            lsp_log("import: indexed %s%s", full_path, parser.had_error ? " (with errors)" : "");
        } else {
            lsp_log("import: failed to parse %s", full_path);
        }

        // Destroy temp arena - releases all AST memory at once
        xr_arena_destroy(&temp_arena);

        xr_free(content);
    }
}

void xlsp_parse_document(XrLspDocument *doc, XrLspServer *server) {
    if (!doc || !doc->content || !server) return;

    // Short-circuit: if the document is not dirty and we already hold
    // a valid AST + cached diagnostics, there is nothing to do. This
    // path fires on redundant publishes (e.g. didChange with identical
    // content after xlsp_document_change's hash guard, or re-entries
    // from workspace re-index), and shaves the full parse + analyze
    // pipeline off of every such call.
    if (!doc->dirty && doc->ast && doc->cached_diagnostics) {
        return;
    }

    lsp_log("parse_document: start %s", doc->uri ? doc->uri : "(null)");
    XrayIsolate *isolate = server->isolate;

    // Free previous diagnostics cache before resetting arena
    xlsp_free_document_cache(doc);

    // Reset arena - releases ALL previous allocations (AST, strings, etc.)
    // This is O(1) and replaces all manual xr_free() calls
    xr_arena_reset(&doc->arena);

    // Clear pointers (memory freed by arena reset)
    doc->ast = NULL;
    doc->error_message = NULL;
    doc->cached_diagnostics = NULL;

    // Clear error state
    doc->parse_error = false;
    doc->error_line = 0;

    if (!isolate) {
        return;
    }

    // Ensure type pool is set for parser (xr_type_* functions need it)
    XrTypePool *apool = xr_isolate_get_analyzer_pool(isolate);
    if (apool) {
        xr_type_set_current_pool(apool, &apool->next_type_id);
        lsp_log("parse_document: type pool set");
    } else {
        lsp_log("parse_document: WARNING - analyzer_pool is NULL!");
    }

    // Initialize parser with document arena for AST allocation
    lsp_log("parse_document: initializing parser, content_len=%zu", doc->length);
    if (!doc->content) {
        lsp_log("parse_document: ERROR - content is NULL!");
        return;
    }
    Parser parser;
    xr_parser_init(&parser, isolate, doc->content, doc->uri, &doc->arena);

    // Set up error collection
    LspErrorContext error_ctx = { .diagnostics = xjson_new_array() };
    xr_parser_set_error_callback(&parser, lsp_error_callback, &error_ctx, 100);

    // Parse with error recovery - returns partial AST even on errors
    lsp_log("parse_document: parsing");
    AstNode *ast = xr_parse_recoverable(&parser);
    lsp_log("parse_document: parsing done");

    doc->ast = ast;
    doc->parse_error = parser.had_error;
    doc->cached_diagnostics = error_ctx.diagnostics;

    // Index imported files on demand (before analyzing current file)
    // Now safe after fixing parser infinite loop issues
    if (ast) {
        index_imports_on_demand(server, ast, doc->uri);
    }

    // Run static analysis using workspace-level analyzer (incremental update)
    // Always try incremental update even with parse errors - we need symbols for completion
    if (ast && server->workspace_analyzer) {
        lsp_log("parse_document: incremental update%s", doc->parse_error ? " (with parse errors)" : "");
        // Use incremental update with content hash for true change detection
        xa_analyzer_refresh_file(server->workspace_analyzer, doc->uri,
                                 (XrAstNode*)ast, doc->content_hash);
        lsp_log("parse_document: incremental update done");

        // Run coroutine sharing validation (emits E0363 diagnostics for
        // illegal `go` captures and `move` targets under the explicit
        // sharing model). The pass is pure diagnostic, no AST mutation.
        xa_escape_analyze((AstNode*)ast, server->workspace_analyzer);

        // Drain analyzer diagnostics into this file's diagnostic array.
        // The analyzer's list is cleared at the next update_incremental,
        // so we must capture them now.
        int a_count = 0;
        XaDiagnostic *a_list = xa_analyzer_get_diagnostics(
            server->workspace_analyzer, &a_count);
        for (XaDiagnostic *d = a_list; d; d = d->next) {
            if (!d->message || d->code == 0) continue;
            // Only include diagnostics for this file (analyzer is shared
            // across the workspace). Location.file may be NULL if the
            // analyzer did not tag it; in that case, assume current file.
            if (d->location.file && doc->uri &&
                strcmp(d->location.file, doc->uri) != 0) {
                continue;
            }
            int line = d->location.line > 0 ? d->location.line - 1 : 0;
            int col = d->location.column > 0 ? d->location.column - 1 : 0;
            // LSP severity: 1=Error, 2=Warning, 3=Info, 4=Hint
            int sev = (d->severity == XR_DIAG_SEV_WARNING) ? 2 :
                      (d->severity == XR_DIAG_SEV_INFO)    ? 3 :
                      (d->severity == XR_DIAG_SEV_HINT)    ? 4 : 1;
            XrJsonValue *diag = make_diagnostic(line, col, line, col + 1,
                                                sev, d->message);
            xjson_array_push(error_ctx.diagnostics, diag);
        }

        // Check for cross-file dependencies that need re-analysis
        int dirty_count = 0;
        const char **dirty_files = xa_analyzer_get_dirty_files(
            server->workspace_analyzer, &dirty_count);
        if (dirty_files && dirty_count > 0) {
            lsp_log("parse_document: %d dirty files", dirty_count);
            // Mark affected documents as needing re-analysis
            for (int i = 0; i < dirty_count; i++) {
                XrLspDocument *affected = xlsp_document_get(server, dirty_files[i]);
                if (affected && affected != doc) {
                    affected->dirty = true;
                }
            }
            xr_free(dirty_files);
        }
    }

    lsp_log("parse_document: done");
    doc->dirty = false;
}

XrJsonValue *xlsp_analyze_diagnostics(XrLspDocument *doc) {
    if (!doc || !doc->content) {
        return xjson_new_array();
    }

    // Use cached diagnostics from parser if available
    // Return a clone so caller can safely free it
    if (doc->cached_diagnostics) {
        return xjson_clone((XrJsonValue *)doc->cached_diagnostics);
    }

    // Fallback: simple lexical scan for obvious errors
    // This path should rarely trigger since xlsp_parse_document always sets cached_diagnostics
    XrJsonValue *diagnostics = xjson_new_array();
    Scanner scanner;
    xr_scanner_init(&scanner, doc->content);

    Token token;
    int paren_depth = 0;
    int brace_depth = 0;
    int bracket_depth = 0;

    while (1) {
        token = xr_scanner_scan(&scanner);

        if (token.type == TK_EOF) break;

        if (token.type == TK_ERROR) {
            // Lexer error: diagnostic text is in error_message (L-03).
            const char *msg = token.error_message ? token.error_message : "lexical error";
            XrJsonValue *diag = make_diagnostic(
                token.line - 1, 0,
                token.line - 1, 100,
                1,  // Error
                msg
            );
            xjson_array_push(diagnostics, diag);
            continue;
        }

        // Track balanced delimiters
        switch (token.type) {
            case TK_LPAREN: paren_depth++; break;
            case TK_RPAREN:
                paren_depth--;
                if (paren_depth < 0) {
                    XrJsonValue *diag = make_diagnostic(
                        token.line - 1, 0, token.line - 1, 1,
                        1, "Unmatched closing parenthesis"
                    );
                    xjson_array_push(diagnostics, diag);
                    paren_depth = 0;
                }
                break;
            case TK_LBRACE: brace_depth++; break;
            case TK_RBRACE:
                brace_depth--;
                if (brace_depth < 0) {
                    XrJsonValue *diag = make_diagnostic(
                        token.line - 1, 0, token.line - 1, 1,
                        1, "Unmatched closing brace"
                    );
                    xjson_array_push(diagnostics, diag);
                    brace_depth = 0;
                }
                break;
            case TK_LBRACKET: bracket_depth++; break;
            case TK_RBRACKET:
                bracket_depth--;
                if (bracket_depth < 0) {
                    XrJsonValue *diag = make_diagnostic(
                        token.line - 1, 0, token.line - 1, 1,
                        1, "Unmatched closing bracket"
                    );
                    xjson_array_push(diagnostics, diag);
                    bracket_depth = 0;
                }
                break;
            default:
                break;
        }
    }

    // Check unclosed delimiters at end of file
    if (paren_depth > 0) {
        XrJsonValue *diag = make_diagnostic(
            doc->line_count - 1, 0, doc->line_count - 1, 0,
            1, "Unclosed parenthesis"
        );
        xjson_array_push(diagnostics, diag);
    }
    if (brace_depth > 0) {
        XrJsonValue *diag = make_diagnostic(
            doc->line_count - 1, 0, doc->line_count - 1, 0,
            1, "Unclosed brace"
        );
        xjson_array_push(diagnostics, diag);
    }
    if (bracket_depth > 0) {
        XrJsonValue *diag = make_diagnostic(
            doc->line_count - 1, 0, doc->line_count - 1, 0,
            1, "Unclosed bracket"
        );
        xjson_array_push(diagnostics, diag);
    }

    return diagnostics;
}


XrJsonValue *xlsp_analyze_hover(XrLspServer *server, XrLspDocument *doc, XrLspPosition pos) {
    if (!doc || !doc->content) return NULL;
    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;

    uint32_t start, end;
    char *word = xlsp_word_at_position(doc, pos, &start, &end);
    if (!word) return NULL;

    const char *content = doc->content;

    // Check if it's a keyword (data-driven lookup)
    const char *description = lookup_doc(keyword_docs, word);

    // Check if it's a builtin (data-driven lookup)
    if (!description) {
        description = lookup_doc(builtin_docs, word);
    }

    // Check if it's a stdlib module
    char hover_buf[2048];
    if (!description) {
        const XlspModuleInfo *module = xlsp_stdlib_find_module(word);
        if (module) {
            snprintf(hover_buf, sizeof(hover_buf),
                "```xray\nimport %s\n```\n\n%s\n\nMembers: %d",
                module->name, module->documentation, module->symbol_count);
            description = hover_buf;
        }
    }

    // Check if it's a local variable/function using XaAnalyzer
    // Prefer position-aware lookup to resolve shadowed names correctly,
    // then fall back to global name lookup.
    if (!description && analyzer) {
        XaSymbol *sym = xa_analyzer_lookup_at(analyzer, doc->uri,
                                              pos.line + 1, pos.character + 1);
        if (!sym) sym = xa_analyzer_lookup(analyzer, word);
        if (sym) {
            XrType *type = xa_analyzer_get_type(analyzer, sym);
            const char *type_str = type ? xr_type_to_string(type) : "unknown";

            if (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD) {
                snprintf(hover_buf, sizeof(hover_buf),
                    "```xray\nfn %s(...): %s\n```\n\n%s",
                    sym->name, type_str,
                    sym->is_exported ? "(exported function)" : "(function)");
            } else if (sym->kind == XA_SYM_CLASS) {
                snprintf(hover_buf, sizeof(hover_buf),
                    "```xray\nclass %s\n```\n\n(class definition)",
                    sym->name);
            } else if (sym->kind == XA_SYM_TYPE_ALIAS) {
                XrType *alias_type = (XrType *)sym->alias_type;
                const char *alias_str = alias_type ? xr_type_to_string(alias_type) : "unknown";
                snprintf(hover_buf, sizeof(hover_buf),
                    "```xray\ntype %s = %s\n```\n\n(type alias)",
                    sym->name, alias_str);
            } else if (sym->kind == XA_SYM_ENUM) {
                snprintf(hover_buf, sizeof(hover_buf),
                    "```xray\nenum %s\n```\n\n(enum definition)",
                    sym->name);
            } else {
                snprintf(hover_buf, sizeof(hover_buf),
                    "```xray\n%s %s: %s\n```\n\n%s",
                    sym->is_const ? "const" : "let",
                    sym->name, type_str,
                    sym->kind == XA_SYM_PARAMETER ? "(parameter)" : "(local variable)");
            }
            description = hover_buf;
        }
    }

    // Check if it's a module.member pattern (look for dot before word)
    if (!description && start > 1 && content[start - 1] == '.') {
        // Find module name before dot
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
            if (!mod_name) goto cleanup_word;  // Skip module resolution on alloc failure

            memcpy(mod_name, content + mod_start, mod_len);
            mod_name[mod_len] = '\0';

            // First try stdlib
            const XlspModuleInfo *module = xlsp_stdlib_find_module(mod_name);
            if (module) {
                const XlspSymbolInfo *sym = xlsp_stdlib_find_symbol(module, word);
                if (sym) {
                    snprintf(hover_buf, sizeof(hover_buf),
                        "```xray\n%s.%s\n%s\n```\n\n%s",
                        mod_name, sym->name,
                        sym->signature ? sym->signature : "",
                        sym->documentation ? sym->documentation : "");
                    description = hover_buf;
                }
            }

            // Try local import
            if (!description) {
                const char *import_hover = xlsp_get_import_hover(doc, mod_name, word,
                                                                  hover_buf, sizeof(hover_buf));
                if (import_hover) {
                    description = import_hover;
                }
            }

            // Try builtin type methods (e.g., Array.push, String.split)
            if (!description) {
                XlspBuiltinType bt = xlsp_builtin_type_from_name(mod_name);
                if (bt != XLSP_TYPE_UNKNOWN) {
                    const char *builtin_hover = xlsp_builtin_get_hover(
                        bt, word, hover_buf, sizeof(hover_buf));
                    if (builtin_hover) {
                        description = builtin_hover;
                    }
                }
            }

            // Try infer variable type for method hover (e.g., arr.push where arr is Array)
            if (!description) {
                XlspBuiltinType var_type = xlsp_infer_variable_type(server, doc, mod_name);
                if (var_type != XLSP_TYPE_UNKNOWN) {
                    const char *var_hover = xlsp_builtin_get_hover(
                        var_type, word, hover_buf, sizeof(hover_buf));
                    if (var_hover) {
                        description = var_hover;
                    }
                }
            }

            xr_free(mod_name);
        }
    }

cleanup_word:
    xr_free(word);

    if (!description) return NULL;

    // Build hover response
    XrJsonValue *hover = xjson_new_object();

    XrJsonValue *contents = xjson_new_object();
    xjson_object_set(contents, "kind", xjson_new_string("markdown"));
    xjson_object_set(contents, "value", xjson_new_string(description));
    xjson_object_set(hover, "contents", contents);

    XrLspPosition range_start = xlsp_offset_to_position(doc, start);
    XrLspPosition range_end = xlsp_offset_to_position(doc, end);
    xjson_object_set(hover, "range",
        xjson_make_range(range_start.line, range_start.character,
                             range_end.line, range_end.character));

    return hover;
}

// ============================================================================
// Document Symbols (Outline)
// ============================================================================

// LSP SymbolKind constants are in xlsp_types.h

// SymbolEntry / SymbolTable types declared in xlsp_analysis.h

void xlsp_symbol_table_init(SymbolTable *table) {
    table->entries = NULL;
    table->count = 0;
    table->capacity = 0;
}

void xlsp_symbol_table_free(SymbolTable *table) {
    for (int i = 0; i < table->count; i++) {
        xr_free(table->entries[i].name);
    }
    xr_free(table->entries);
    table->entries = NULL;
    table->count = 0;
    table->capacity = 0;
}

static void symbol_table_add(SymbolTable *table, const char *name, int kind,
                             int line, int start_char, int end_line, int end_char) {
    if (table->count >= table->capacity) {
        int new_cap = table->capacity == 0 ? 16 : table->capacity * 2;
        XR_REALLOC_OR_ABORT(table->entries,
                            (size_t)new_cap * sizeof(SymbolEntry),
                            "lsp symbol_table grow");
        table->capacity = new_cap;
    }
    SymbolEntry *entry = &table->entries[table->count++];
    entry->name = xr_strdup(name);
    entry->kind = kind;
    entry->line = line;
    entry->start_char = start_char;
    entry->end_line = end_line;
    entry->end_char = end_char;
}

// Visitor callback for symbol extraction
static void symbol_extract_visitor(AstNode *node, void *ctx) {
    SymbolTable *table = (SymbolTable *)ctx;

    switch (node->type) {
        case AST_FUNCTION_DECL:
            if (node->as.function_decl.name) {
                symbol_table_add(table, node->as.function_decl.name,
                               LSP_SYMBOL_FUNCTION,
                               node->line - 1, 0, node->line - 1,
                               (int)strlen(node->as.function_decl.name));
            }
            break;

        case AST_VAR_DECL:
            if (node->as.var_decl.name) {
                int kind = node->as.var_decl.is_const ?
                          LSP_SYMBOL_CONSTANT : LSP_SYMBOL_VARIABLE;
                symbol_table_add(table, node->as.var_decl.name, kind,
                               node->line - 1, 0, node->line - 1,
                               (int)strlen(node->as.var_decl.name));
            }
            break;

        case AST_CONST_DECL:
            if (node->as.var_decl.name) {
                symbol_table_add(table, node->as.var_decl.name,
                               LSP_SYMBOL_CONSTANT,
                               node->line - 1, 0, node->line - 1,
                               (int)strlen(node->as.var_decl.name));
            }
            break;

        case AST_CLASS_DECL:
        case AST_STRUCT_DECL:
            if (node->as.class_decl.name) {
                symbol_table_add(table, node->as.class_decl.name,
                               LSP_SYMBOL_CLASS,
                               node->line - 1, 0, node->line - 1,
                               (int)strlen(node->as.class_decl.name));
            }
            break;

        case AST_METHOD_DECL:
            if (node->as.method_decl.name) {
                int kind = node->as.method_decl.is_constructor ?
                          LSP_SYMBOL_CONSTRUCTOR : LSP_SYMBOL_METHOD;
                symbol_table_add(table, node->as.method_decl.name, kind,
                               node->line - 1, 0, node->line - 1,
                               (int)strlen(node->as.method_decl.name));
            }
            break;

        case AST_FIELD_DECL:
            if (node->as.field_decl.name) {
                symbol_table_add(table, node->as.field_decl.name,
                               LSP_SYMBOL_FIELD,
                               node->line - 1, 0, node->line - 1,
                               (int)strlen(node->as.field_decl.name));
            }
            break;

        case AST_INTERFACE_DECL:
            if (node->as.interface_decl.name) {
                symbol_table_add(table, node->as.interface_decl.name,
                               LSP_SYMBOL_INTERFACE,
                               node->line - 1, 0, node->line - 1,
                               (int)strlen(node->as.interface_decl.name));
            }
            break;

        case AST_ENUM_DECL:
            if (node->as.enum_decl.name) {
                symbol_table_add(table, node->as.enum_decl.name,
                               LSP_SYMBOL_ENUM,
                               node->line - 1, 0, node->line - 1,
                               (int)strlen(node->as.enum_decl.name));
            }
            break;

        case AST_ENUM_MEMBER:
            if (node->as.enum_member.name) {
                symbol_table_add(table, node->as.enum_member.name,
                               LSP_SYMBOL_ENUM_MEMBER,
                               node->line - 1, 0, node->line - 1,
                               (int)strlen(node->as.enum_member.name));
            }
            break;

        default:
            break;
    }
}

// Extract symbols from AST using visitor framework
static void extract_symbols_from_ast(AstNode *node, SymbolTable *table) {
    if (!node) return;
    xa_ast_walk(node, symbol_extract_visitor, NULL, table);
}

// Extract symbols from document using lexer-based scanning (fallback)
static void extract_symbols_lexer(XrLspDocument *doc, SymbolTable *table) {
    Scanner scanner;
    xr_scanner_init(&scanner, doc->content);

    Token prev = {0};
    Token token;

    while (1) {
        token = xr_scanner_scan(&scanner);
        if (token.type == TK_EOF) break;
        if (token.type == TK_ERROR) continue;

        // fn <name>
        if (prev.type == TK_FN && token.type == TK_NAME) {
            char *name = strndup(token.start, token.length);
            symbol_table_add(table, name, LSP_SYMBOL_FUNCTION,
                            token.line - 1, 0, token.line - 1, (int)strlen(name));
            xr_free(name);
        }
        // class <name>
        else if (prev.type == TK_CLASS && token.type == TK_NAME) {
            char *name = strndup(token.start, token.length);
            symbol_table_add(table, name, LSP_SYMBOL_CLASS,
                            token.line - 1, 0, token.line - 1, (int)strlen(name));
            xr_free(name);
        }
        // interface <name>
        else if (prev.type == TK_INTERFACE && token.type == TK_NAME) {
            char *name = strndup(token.start, token.length);
            symbol_table_add(table, name, LSP_SYMBOL_INTERFACE,
                            token.line - 1, 0, token.line - 1, (int)strlen(name));
            xr_free(name);
        }
        // enum <name>
        else if (prev.type == TK_ENUM && token.type == TK_NAME) {
            char *name = strndup(token.start, token.length);
            symbol_table_add(table, name, LSP_SYMBOL_ENUM,
                            token.line - 1, 0, token.line - 1, (int)strlen(name));
            xr_free(name);
        }
        // let <name> or const <name>
        else if ((prev.type == TK_LET || prev.type == TK_CONST) && token.type == TK_NAME) {
            char *name = strndup(token.start, token.length);
            int kind = (prev.type == TK_CONST) ? LSP_SYMBOL_CONSTANT : LSP_SYMBOL_VARIABLE;
            symbol_table_add(table, name, kind,
                            token.line - 1, 0, token.line - 1, (int)strlen(name));
            xr_free(name);
        }

        prev = token;
    }
}

// Extract symbols using AST if available, otherwise fallback to lexer
void xlsp_extract_symbols(XrLspDocument *doc, SymbolTable *table) {
    if (doc->ast) {
        extract_symbols_from_ast(doc->ast, table);
    } else {
        extract_symbols_lexer(doc, table);
    }
}

// Create a DocumentSymbol JSON object.
//
// `full_range` must fully contain `selection_range` — this is enforced by the
// LSP spec and validated by VS Code. Both ranges come directly from AST
// location fields (line, column, end_line, end_column) so there is no guessing
// or clamping here: the AST is the single source of truth.
static XrJsonValue *create_doc_symbol(const char *name, int kind,
                                       XrLspRange full_range,
                                       XrLspRange selection_range) {
    XR_DCHECK(name != NULL, "create_doc_symbol: NULL name");

    XrJsonValue *symbol = xjson_new_object();
    xjson_object_set(symbol, "name", xjson_new_string(name));
    xjson_object_set(symbol, "kind", xjson_new_number(kind));
    xjson_object_set(symbol, "range",
        xjson_make_range(full_range.start.line, full_range.start.character,
                             full_range.end.line,   full_range.end.character));
    xjson_object_set(symbol, "selectionRange",
        xjson_make_range(selection_range.start.line, selection_range.start.character,
                             selection_range.end.line,   selection_range.end.character));
    return symbol;
}

// Compute the two ranges expected by DocumentSymbol from an AST declaring
// node. AST contract:
//   (node->line, node->column)            — identifier start (1-indexed)
//   (node->end_line, node->end_column)    — declaration end, exclusive
//                                            (1-indexed; `.end_column` points
//                                             to one past the closing token)
//
// Converts to LSP's 0-indexed coordinates.
//
// Preconditions (checked by parser): node->line >= 1, node->column >= 1,
// node->end_line >= node->line, and when end_line == line:
// end_column >= column + strlen(name).
static void ast_decl_ranges(const AstNode *node, const char *name,
                            XrLspRange *full_range,
                            XrLspRange *selection_range) {
    XR_DCHECK(node != NULL && name != NULL, "ast_decl_ranges: NULL arg");
    XR_DCHECK(node->line >= 1 && node->column >= 1,
              "ast_decl_ranges: node missing identifier location");
    XR_DCHECK(node->end_line >= node->line,
              "ast_decl_ranges: end_line precedes start line");

    int start_line = node->line - 1;
    int start_col  = node->column - 1;
    int name_len   = (int)strlen(name);

    selection_range->start.line      = start_line;
    selection_range->start.character = start_col;
    selection_range->end.line        = start_line;
    selection_range->end.character   = start_col + name_len;

    full_range->start.line      = start_line;
    full_range->start.character = start_col;
    full_range->end.line        = node->end_line - 1;
    full_range->end.character   = (node->end_column > 0) ? node->end_column - 1 : 0;

    XR_DCHECK(
        full_range->end.line > full_range->start.line ||
        (full_range->end.line == full_range->start.line &&
         full_range->end.character >= selection_range->end.character),
        "ast_decl_ranges: selectionRange not contained in fullRange");
}

// Emit a DocumentSymbol for a declaring node. Returns NULL if the node has
// incomplete source-location information (in which case the symbol is quietly
// dropped — we would rather hide one bad entry than reject the whole
// response).
static XrJsonValue *emit_decl_symbol(const AstNode *node, const char *name, int kind) {
    if (!node || !name || node->line < 1 || node->column < 1 || node->end_line < 1) {
        return NULL;
    }
    XrLspRange full_range, selection_range;
    ast_decl_ranges(node, name, &full_range, &selection_range);
    return create_doc_symbol(name, kind, full_range, selection_range);
}

// Build nested document symbols from AST.
//
// Every decl node is expected to carry an accurate (line, column,
// end_line, end_column) span recorded by the parser — see the contract on
// `AstNode`. This function is therefore pure projection: no heuristics, no
// range guessing.
static void build_nested_symbols(AstNode *node, XrJsonValue *symbols) {
    if (!node) return;

    switch (node->type) {
        case AST_PROGRAM:
            for (int i = 0; i < node->as.program.count; i++) {
                build_nested_symbols(node->as.program.statements[i], symbols);
            }
            break;

        case AST_FUNCTION_DECL: {
            XrJsonValue *sym = emit_decl_symbol(node,
                node->as.function_decl.name, LSP_SYMBOL_FUNCTION);
            if (sym) xjson_array_push(symbols, sym);
            break;
        }

        case AST_VAR_DECL: {
            int kind = node->as.var_decl.is_const
                     ? LSP_SYMBOL_CONSTANT : LSP_SYMBOL_VARIABLE;
            XrJsonValue *sym = emit_decl_symbol(node, node->as.var_decl.name, kind);
            if (sym) xjson_array_push(symbols, sym);
            break;
        }

        case AST_CONST_DECL: {
            XrJsonValue *sym = emit_decl_symbol(node,
                node->as.var_decl.name, LSP_SYMBOL_CONSTANT);
            if (sym) xjson_array_push(symbols, sym);
            break;
        }

        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            XrJsonValue *sym = emit_decl_symbol(node,
                node->as.class_decl.name, LSP_SYMBOL_CLASS);
            if (!sym) break;

            XrJsonValue *children = xjson_new_array();

            for (int i = 0; i < node->as.class_decl.field_count; i++) {
                AstNode *field = node->as.class_decl.fields[i];
                if (field && field->type == AST_FIELD_DECL) {
                    XrJsonValue *child = emit_decl_symbol(field,
                        field->as.field_decl.name, LSP_SYMBOL_FIELD);
                    if (child) xjson_array_push(children, child);
                }
            }

            for (int i = 0; i < node->as.class_decl.method_count; i++) {
                AstNode *method = node->as.class_decl.methods[i];
                if (method && method->type == AST_METHOD_DECL) {
                    int kind = method->as.method_decl.is_constructor
                             ? LSP_SYMBOL_CONSTRUCTOR : LSP_SYMBOL_METHOD;
                    XrJsonValue *child = emit_decl_symbol(method,
                        method->as.method_decl.name, kind);
                    if (child) xjson_array_push(children, child);
                }
            }

            if (xjson_array_len(children) > 0) {
                xjson_object_set(sym, "children", children);
            } else {
                xjson_free(children);
            }
            xjson_array_push(symbols, sym);
            break;
        }

        case AST_INTERFACE_DECL: {
            XrJsonValue *sym = emit_decl_symbol(node,
                node->as.interface_decl.name, LSP_SYMBOL_INTERFACE);
            if (sym) xjson_array_push(symbols, sym);
            break;
        }

        case AST_ENUM_DECL: {
            XrJsonValue *sym = emit_decl_symbol(node,
                node->as.enum_decl.name, LSP_SYMBOL_ENUM);
            if (!sym) break;

            if (node->as.enum_decl.members) {
                XrJsonValue *children = xjson_new_array();
                for (int i = 0; i < node->as.enum_decl.member_count; i++) {
                    AstNode *member = node->as.enum_decl.members[i];
                    if (member && member->type == AST_ENUM_MEMBER) {
                        XrJsonValue *child = emit_decl_symbol(member,
                            member->as.enum_member.name, LSP_SYMBOL_ENUM_MEMBER);
                        if (child) xjson_array_push(children, child);
                    }
                }
                if (xjson_array_len(children) > 0) {
                    xjson_object_set(sym, "children", children);
                } else {
                    xjson_free(children);
                }
            }
            xjson_array_push(symbols, sym);
            break;
        }

        case AST_EXPORT_STMT:
            if (node->as.export_stmt.declaration) {
                build_nested_symbols(node->as.export_stmt.declaration, symbols);
            }
            break;

        default:
            break;
    }
}

XrJsonValue *xlsp_analyze_document_symbols(XrLspDocument *doc) {
    XrJsonValue *symbols = xjson_new_array();

    if (!doc || !doc->content) {
        return symbols;
    }

    // Use AST for nested symbols if available
    if (doc->ast) {
        build_nested_symbols(doc->ast, symbols);
        return symbols;
    }

    // Fallback to flat symbol table for documents without AST.
    // The lexer-only path already produces a (line, col, end_line, end_col)
    // span per entry in 0-indexed form.
    SymbolTable table;
    xlsp_symbol_table_init(&table);
    extract_symbols_lexer(doc, &table);

    for (int i = 0; i < table.count; i++) {
        SymbolEntry *entry = &table.entries[i];
        int name_len = (int)strlen(entry->name);

        XrLspRange selection_range = {
            .start = { entry->line, entry->start_char },
            .end   = { entry->line, entry->start_char + name_len },
        };
        // Ensure fullRange covers the identifier (lexer path may return an
        // end column before the identifier's end when the symbol is followed
        // by no initializer; clamp locally to keep the invariant).
        int full_end_line = entry->end_line;
        int full_end_col  = entry->end_char;
        if (full_end_line < entry->line) {
            full_end_line = entry->line;
            full_end_col  = entry->start_char + name_len;
        } else if (full_end_line == entry->line &&
                   full_end_col < entry->start_char + name_len) {
            full_end_col = entry->start_char + name_len;
        }
        XrLspRange full_range = {
            .start = { entry->line, entry->start_char },
            .end   = { full_end_line, full_end_col },
        };

        XrJsonValue *sym = create_doc_symbol(entry->name, entry->kind,
                                             full_range, selection_range);
        xjson_array_push(symbols, sym);
    }

    xlsp_symbol_table_free(&table);
    return symbols;
}

// ============================================================================
// Signature Help
// ============================================================================

// Builtin function signatures
typedef struct {
    const char *name;
    const char *signature;
    const char *documentation;
    const char **param_names;
    const char **param_docs;
    int param_count;
} FunctionSignature;

static const char *print_params[] = {"value", "..."};
static const char *print_param_docs[] = {"Value to print", "Additional values"};

static const char *typeof_params[] = {"value"};
static const char *typeof_param_docs[] = {"Value to check type"};

static const char *assert_params[] = {"condition", "message"};
static const char *assert_param_docs[] = {"Condition to assert", "Optional error message"};

static const char *assert_one_params[] = {"value"};
static const char *assert_one_param_docs[] = {"Value to check"};

static const char *assert_eq_params[] = {"actual", "expected"};
static const char *assert_eq_param_docs[] = {"Actual value", "Expected value"};

static const char *assert_ne_params[] = {"actual", "unexpected"};
static const char *assert_ne_param_docs[] = {"Actual value", "Unexpected value"};

static const FunctionSignature builtin_signatures[] = {
    {"print", "print(value, ...)", "Prints values to stdout",
     print_params, print_param_docs, 2},
    {"typeof", "typeof(value): string", "Returns type name of value",
     typeof_params, typeof_param_docs, 1},
    {"assert", "assert(condition, message?)", "Asserts condition is true",
     assert_params, assert_param_docs, 2},
    {"assert_true", "assert_true(value)", "Asserts value is truthy",
     assert_one_params, assert_one_param_docs, 1},
    {"assert_false", "assert_false(value)", "Asserts value is falsy",
     assert_one_params, assert_one_param_docs, 1},
    {"assert_eq", "assert_eq(actual, expected)", "Asserts values are equal",
     assert_eq_params, assert_eq_param_docs, 2},
    {"assert_ne", "assert_ne(actual, unexpected)", "Asserts values are not equal",
     assert_ne_params, assert_ne_param_docs, 2},
    {NULL, NULL, NULL, NULL, NULL, 0}
};

XrJsonValue *xlsp_analyze_signature_help(XrLspDocument *doc, XrLspPosition pos) {
    if (!doc || !doc->content) return NULL;

    uint32_t offset = xlsp_position_to_offset(doc, pos);
    if (offset == 0 || offset > doc->length) return NULL;

    // Look backwards for function name and opening paren
    const char *content = doc->content;
    int paren_depth = 0;
    int active_param = 0;
    uint32_t func_end = 0;
    uint32_t func_start = 0;
    bool found_paren = false;

    for (uint32_t i = offset; i > 0; i--) {
        char c = content[i - 1];

        if (c == ')') {
            paren_depth++;
        } else if (c == '(') {
            if (paren_depth == 0) {
                func_end = i - 1;
                found_paren = true;
                break;
            }
            paren_depth--;
        } else if (c == ',' && paren_depth == 0) {
            active_param++;
        } else if (c == '\n') {
            // Don't search past line boundary
            break;
        }
    }

    if (!found_paren || func_end == 0) return NULL;

    // Find function name
    func_start = func_end;
    while (func_start > 0 && (content[func_start - 1] == '_' ||
           (content[func_start - 1] >= 'a' && content[func_start - 1] <= 'z') ||
           (content[func_start - 1] >= 'A' && content[func_start - 1] <= 'Z') ||
           (content[func_start - 1] >= '0' && content[func_start - 1] <= '9'))) {
        func_start--;
    }

    if (func_start == func_end) return NULL;

    // Extract function name
    size_t name_len = func_end - func_start;
    char *func_name = xr_malloc(name_len + 1);
    if (!func_name) return NULL;

    memcpy(func_name, content + func_start, name_len);
    func_name[name_len] = '\0';

    // Look up signature
    const FunctionSignature *sig = NULL;
    for (int i = 0; builtin_signatures[i].name; i++) {
        if (strcmp(func_name, builtin_signatures[i].name) == 0) {
            sig = &builtin_signatures[i];
            break;
        }
    }

    // Build SignatureHelp response
    XrJsonValue *result = xjson_new_object();
    XrJsonValue *signatures = xjson_new_array();
    XrJsonValue *sig_info = xjson_new_object();
    XrJsonValue *params = xjson_new_array();
    int param_count = 0;

    if (sig) {
        // Builtin function
        xjson_object_set(sig_info, "label", xjson_new_string(sig->signature));
        if (sig->documentation) {
            xjson_object_set(sig_info, "documentation", xjson_new_string(sig->documentation));
        }
        for (int i = 0; i < sig->param_count; i++) {
            XrJsonValue *param = xjson_new_object();
            xjson_object_set(param, "label", xjson_new_string(sig->param_names[i]));
            if (sig->param_docs && sig->param_docs[i]) {
                xjson_object_set(param, "documentation", xjson_new_string(sig->param_docs[i]));
            }
            xjson_array_push(params, param);
        }
        param_count = sig->param_count;
        xr_free(func_name);
    } else {
        // Try user-defined function from analyzer
        XaAnalyzer *analyzer = doc->server ? doc->server->workspace_analyzer : NULL;
        XaSymbol *sym = analyzer ? xa_scope_lookup(analyzer->global_scope, func_name) : NULL;
        xr_free(func_name);

        if (!sym || (sym->kind != XA_SYM_FUNCTION && sym->kind != XA_SYM_METHOD)) {
            xjson_free(result);
            xjson_free(sig_info);
            xjson_free(params);
            return NULL;
        }

        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);

        // Build signature label: fn name(a: int, b: str): ReturnType
        char sig_label[512];
        int sig_len = 0;
        sig_len += snprintf(sig_label + sig_len, sizeof(sig_label) - sig_len, "fn %s(", sym->name);

        if (links && links->param_count > 0) {
            for (int p = 0; p < links->param_count; p++) {
                if (p > 0) sig_len += snprintf(sig_label + sig_len, sizeof(sig_label) - sig_len, ", ");
                const char *pname = (links->param_names && links->param_names[p])
                    ? links->param_names[p] : "_";
                const char *ptype = (links->param_types && links->param_types[p])
                    ? xr_type_to_string(links->param_types[p]) : "unknown";
                sig_len += snprintf(sig_label + sig_len, sizeof(sig_label) - sig_len, "%s: %s", pname, ptype);

                // Add parameter info
                XrJsonValue *param = xjson_new_object();
                char param_label[128];
                snprintf(param_label, sizeof(param_label), "%s: %s", pname, ptype);
                xjson_object_set(param, "label", xjson_new_string(param_label));
                xjson_array_push(params, param);
            }
            param_count = links->param_count;
        }

        const char *ret_type = (links && links->return_type)
            ? xr_type_to_string(links->return_type) : "unknown";
        snprintf(sig_label + sig_len, sizeof(sig_label) - sig_len, "): %s", ret_type);

        xjson_object_set(sig_info, "label", xjson_new_string(sig_label));
    }

    xjson_object_set(sig_info, "parameters", params);
    xjson_array_push(signatures, sig_info);
    xjson_object_set(result, "signatures", signatures);

    // Active signature and parameter
    xjson_object_set(result, "activeSignature", xjson_new_number(0));
    int active = (param_count > 0 && active_param < param_count) ? active_param : 0;
    xjson_object_set(result, "activeParameter", xjson_new_number(active));

    return result;
}


