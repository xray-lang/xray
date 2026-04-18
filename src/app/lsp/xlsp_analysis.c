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
#include "xlsp_json.h"
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

// AST formatter
#include "../cli/xfmt.h"
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
    XrJsonValue *diag = xlsp_json_new_object();

    xlsp_json_object_set(diag, "range", xlsp_json_make_range(start_line, start_char, end_line, end_char));

    // Severity (1=Error, 2=Warning, 3=Info, 4=Hint)
    xlsp_json_object_set(diag, "severity", xlsp_json_new_number(severity));

    // Source
    xlsp_json_object_set(diag, "source", xlsp_json_new_string("xray"));

    // Message
    xlsp_json_object_set(diag, "message", xlsp_json_new_string(message));

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

    xlsp_json_array_push(ctx->diagnostics, diag);
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
                xa_analyzer_update_incremental(server->workspace_analyzer, import_uri,
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
    LspErrorContext error_ctx = { .diagnostics = xlsp_json_new_array() };
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
        xa_analyzer_update_incremental(server->workspace_analyzer, doc->uri,
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
            xlsp_json_array_push(error_ctx.diagnostics, diag);
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
        return xlsp_json_new_array();
    }

    // Use cached diagnostics from parser if available
    // Return a clone so caller can safely free it
    if (doc->cached_diagnostics) {
        return xlsp_json_clone((XrJsonValue *)doc->cached_diagnostics);
    }

    // Fallback: simple lexical scan for obvious errors
    // This path should rarely trigger since xlsp_parse_document always sets cached_diagnostics
    XrJsonValue *diagnostics = xlsp_json_new_array();
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
            // Lexer error
            XrJsonValue *diag = make_diagnostic(
                token.line - 1, 0,
                token.line - 1, 100,
                1,  // Error
                token.start  // Error message is in start
            );
            xlsp_json_array_push(diagnostics, diag);
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
                    xlsp_json_array_push(diagnostics, diag);
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
                    xlsp_json_array_push(diagnostics, diag);
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
                    xlsp_json_array_push(diagnostics, diag);
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
        xlsp_json_array_push(diagnostics, diag);
    }
    if (brace_depth > 0) {
        XrJsonValue *diag = make_diagnostic(
            doc->line_count - 1, 0, doc->line_count - 1, 0,
            1, "Unclosed brace"
        );
        xlsp_json_array_push(diagnostics, diag);
    }
    if (bracket_depth > 0) {
        XrJsonValue *diag = make_diagnostic(
            doc->line_count - 1, 0, doc->line_count - 1, 0,
            1, "Unclosed bracket"
        );
        xlsp_json_array_push(diagnostics, diag);
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
    if (!description && analyzer) {
        XaSymbol *sym = xa_analyzer_lookup(analyzer, word);
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
    XrJsonValue *hover = xlsp_json_new_object();

    XrJsonValue *contents = xlsp_json_new_object();
    xlsp_json_object_set(contents, "kind", xlsp_json_new_string("markdown"));
    xlsp_json_object_set(contents, "value", xlsp_json_new_string(description));
    xlsp_json_object_set(hover, "contents", contents);

    XrLspPosition range_start = xlsp_offset_to_position(doc, start);
    XrLspPosition range_end = xlsp_offset_to_position(doc, end);
    xlsp_json_object_set(hover, "range",
        xlsp_json_make_range(range_start.line, range_start.character,
                             range_end.line, range_end.character));

    return hover;
}

// ============================================================================
// Document Symbols (Outline)
// ============================================================================

// LSP SymbolKind constants are in xlsp_types.h

// Symbol entry for tracking definitions
typedef struct {
    char *name;
    int kind;           // LSP SymbolKind
    int line;           // 0-indexed line
    int start_char;     // start character
    int end_line;       // end line
    int end_char;       // end character
} SymbolEntry;

// Symbol table for the document
typedef struct {
    SymbolEntry *entries;
    int count;
    int capacity;
} SymbolTable;

static void symbol_table_init(SymbolTable *table) {
    table->entries = NULL;
    table->count = 0;
    table->capacity = 0;
}

static void symbol_table_free(SymbolTable *table) {
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
static void extract_symbols(XrLspDocument *doc, SymbolTable *table) {
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

    XrJsonValue *symbol = xlsp_json_new_object();
    xlsp_json_object_set(symbol, "name", xlsp_json_new_string(name));
    xlsp_json_object_set(symbol, "kind", xlsp_json_new_number(kind));
    xlsp_json_object_set(symbol, "range",
        xlsp_json_make_range(full_range.start.line, full_range.start.character,
                             full_range.end.line,   full_range.end.character));
    xlsp_json_object_set(symbol, "selectionRange",
        xlsp_json_make_range(selection_range.start.line, selection_range.start.character,
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
            if (sym) xlsp_json_array_push(symbols, sym);
            break;
        }

        case AST_VAR_DECL: {
            int kind = node->as.var_decl.is_const
                     ? LSP_SYMBOL_CONSTANT : LSP_SYMBOL_VARIABLE;
            XrJsonValue *sym = emit_decl_symbol(node, node->as.var_decl.name, kind);
            if (sym) xlsp_json_array_push(symbols, sym);
            break;
        }

        case AST_CONST_DECL: {
            XrJsonValue *sym = emit_decl_symbol(node,
                node->as.var_decl.name, LSP_SYMBOL_CONSTANT);
            if (sym) xlsp_json_array_push(symbols, sym);
            break;
        }

        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            XrJsonValue *sym = emit_decl_symbol(node,
                node->as.class_decl.name, LSP_SYMBOL_CLASS);
            if (!sym) break;

            XrJsonValue *children = xlsp_json_new_array();

            for (int i = 0; i < node->as.class_decl.field_count; i++) {
                AstNode *field = node->as.class_decl.fields[i];
                if (field && field->type == AST_FIELD_DECL) {
                    XrJsonValue *child = emit_decl_symbol(field,
                        field->as.field_decl.name, LSP_SYMBOL_FIELD);
                    if (child) xlsp_json_array_push(children, child);
                }
            }

            for (int i = 0; i < node->as.class_decl.method_count; i++) {
                AstNode *method = node->as.class_decl.methods[i];
                if (method && method->type == AST_METHOD_DECL) {
                    int kind = method->as.method_decl.is_constructor
                             ? LSP_SYMBOL_CONSTRUCTOR : LSP_SYMBOL_METHOD;
                    XrJsonValue *child = emit_decl_symbol(method,
                        method->as.method_decl.name, kind);
                    if (child) xlsp_json_array_push(children, child);
                }
            }

            if (xlsp_json_array_len(children) > 0) {
                xlsp_json_object_set(sym, "children", children);
            } else {
                xlsp_json_free(children);
            }
            xlsp_json_array_push(symbols, sym);
            break;
        }

        case AST_INTERFACE_DECL: {
            XrJsonValue *sym = emit_decl_symbol(node,
                node->as.interface_decl.name, LSP_SYMBOL_INTERFACE);
            if (sym) xlsp_json_array_push(symbols, sym);
            break;
        }

        case AST_ENUM_DECL: {
            XrJsonValue *sym = emit_decl_symbol(node,
                node->as.enum_decl.name, LSP_SYMBOL_ENUM);
            if (!sym) break;

            if (node->as.enum_decl.members) {
                XrJsonValue *children = xlsp_json_new_array();
                for (int i = 0; i < node->as.enum_decl.member_count; i++) {
                    AstNode *member = node->as.enum_decl.members[i];
                    if (member && member->type == AST_ENUM_MEMBER) {
                        XrJsonValue *child = emit_decl_symbol(member,
                            member->as.enum_member.name, LSP_SYMBOL_ENUM_MEMBER);
                        if (child) xlsp_json_array_push(children, child);
                    }
                }
                if (xlsp_json_array_len(children) > 0) {
                    xlsp_json_object_set(sym, "children", children);
                } else {
                    xlsp_json_free(children);
                }
            }
            xlsp_json_array_push(symbols, sym);
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
    XrJsonValue *symbols = xlsp_json_new_array();

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
    symbol_table_init(&table);
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
        xlsp_json_array_push(symbols, sym);
    }

    symbol_table_free(&table);
    return symbols;
}

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
        symbol_table_init(&table);
        extract_symbols(doc, &table);

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
        symbol_table_free(&table);
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
    // Cross-file reference search (still uses lexer for other open documents)
    // TODO: Use analyzer for cross-file semantic search when available
    // =========================================================================

    if (server && server->doc_table) {
        XrLspDocTable *table = server->doc_table;
        for (int i = 0; i < table->bucket_count; i++) {
            XrLspDocBucket *bucket = table->buckets[i];
            while (bucket) {
                XrLspDocument *other = bucket->doc;
                // Skip current document (already searched)
                if (other && other != doc && other->content) {
                    // For other files, try semantic search if AST available
                    if (other->ast && analyzer && analyzer->global_scope) {
                        // Use simple scope check - only include if symbol is exported
                        // and visible from this file
                        XaSymbol *other_sym = xa_analyzer_lookup_in_scope(
                            analyzer, search_word, analyzer->global_scope);

                        if (other_sym && other_sym->is_exported) {
                            RefFindContext ctx = {
                                .target_name = search_word,
                                .def_scope = analyzer->global_scope,  // Global for exported symbols
                                .current_scope = analyzer->global_scope,
                                .global_scope = analyzer->global_scope,
                                .refs = refs,
                                .uri = other->uri
                            };
                            collect_refs_from_ast(other->ast, &ctx);
                        }
                    } else {
                        // Fallback to lexer
                        scan_doc_for_refs_lexer(other, search_word, word_len, refs);
                    }
                }
                bucket = bucket->next;
            }
        }
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

// ============================================================================
// Rename (Semantic-aware)
// ============================================================================

// Rename context - uses XaScope from analyzer for unified scope handling
typedef struct {
    const char *target_name;   // Name to find/rename
    int target_name_len;       // Length of target name
    int target_line;           // Line of cursor position (1-indexed)
    int target_col;            // Column of cursor position (1-indexed)
    XaScope *global_scope;     // Global scope from analyzer
    XaScope *def_scope;        // Scope where symbol is defined
    XaScope *current_scope;    // Current scope during traversal
    bool found_def;            // Whether we found the definition
    bool is_global;            // Whether the symbol is global
    XrJsonValue *edits;        // Collected edit locations
    const char *new_name;      // New name for replacement
} RenameContext;

// Forward declarations
static void find_symbol_definition(AstNode *node, RenameContext *ctx);
static void collect_rename_locations(AstNode *node, RenameContext *ctx);

// Add a text edit for renaming
static void add_rename_edit(RenameContext *ctx, int line, int col, int len);

// Helper: check if cursor is on this identifier (line and column match)
// Returns true if the cursor position matches this identifier location
static bool is_cursor_on_identifier(RenameContext *ctx, int node_line, int node_col, int name_len) {
    if (node_line != ctx->target_line) return false;

    // If node column is 0 (unknown), only match by line
    if (node_col <= 0) return true;

    // Check if cursor column is within the identifier range
    // target_col is 1-indexed, node_col is 1-indexed
    return ctx->target_col >= node_col && ctx->target_col < node_col + name_len;
}

// Helper: determine expected scope kind from AST node type
static XaScopeKind get_expected_scope_kind(AstNode *node) {
    if (!node) return XA_SCOPE_BLOCK;
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
    if (!parent) return NULL;

    AstNode *node = (AstNode *)ast_node;

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
    if (!node || ctx->found_def) return;

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
                is_cursor_on_identifier(ctx, node->line, node->column, (int)strlen(fn->name))) {
                ctx->def_scope = ctx->current_scope;
                ctx->is_global = (ctx->current_scope == ctx->global_scope);
                ctx->found_def = true;
                return;
            }

            // Find function's scope
            XaScope *saved_scope = ctx->current_scope;
            XaScope *fn_scope = find_child_scope_xa(ctx->current_scope, node);
            if (fn_scope) ctx->current_scope = fn_scope;

            // Check parameters
            for (int i = 0; i < fn->param_count; i++) {
                XrParamNode *param = fn->params[i];
                if (param && param->name && strcmp(param->name, ctx->target_name) == 0 &&
                    is_cursor_on_identifier(ctx, param->line, param->column, (int)strlen(param->name))) {
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
                is_cursor_on_identifier(ctx, node->line, node->column, (int)strlen(var->name))) {
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
                is_cursor_on_identifier(ctx, node->line, node->column, (int)strlen(var_name))) {
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
            if (block_scope) ctx->current_scope = block_scope;

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
            if (for_scope) ctx->current_scope = for_scope;

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
            if (for_scope) ctx->current_scope = for_scope;

            if (fi->item_name && strcmp(fi->item_name, ctx->target_name) == 0 &&
                is_cursor_on_identifier(ctx, node->line, node->column, (int)strlen(fi->item_name))) {
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
            if (fn_scope) ctx->current_scope = fn_scope;

            // Check parameters
            for (int i = 0; i < fn_expr->param_count; i++) {
                XrParamNode *param = fn_expr->params[i];
                if (param && param->name && strcmp(param->name, ctx->target_name) == 0 &&
                    is_cursor_on_identifier(ctx, param->line, param->column, (int)strlen(param->name))) {
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
                is_cursor_on_identifier(ctx, node->line, node->column, (int)strlen(cls->name))) {
                ctx->def_scope = ctx->current_scope;
                ctx->is_global = (ctx->current_scope == ctx->global_scope);
                ctx->found_def = true;
                return;
            }

            // Enter class scope
            XaScope *saved_scope = ctx->current_scope;
            XaScope *class_scope = find_child_scope_xa(ctx->current_scope, node);
            if (class_scope) ctx->current_scope = class_scope;

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
                                        (int)strlen(tc->catch_var))) {
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
                if (catch_scope) ctx->current_scope = catch_scope;

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
                is_cursor_on_identifier(ctx, node->line, node->column, (int)strlen(assign_name))) {
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
    XrJsonValue *edit = xlsp_json_new_object();
    xlsp_json_object_set(edit, "range",
        xlsp_json_make_range(line - 1, col - 1, line - 1, col - 1 + len));
    xlsp_json_object_set(edit, "newText", xlsp_json_new_string(ctx->new_name));
    xlsp_json_array_push(ctx->edits, edit);
}

// Check if we should rename in the current context
// Uses XaScope hierarchy to determine if current scope can see the definition
static bool should_rename(RenameContext *ctx) {
    if (!ctx->def_scope || !ctx->current_scope) return false;

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
    if (!node) return;

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
                               (int)strlen(fn->name));
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
                                       (int)strlen(param->name));
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
                                       (int)strlen(param->name));
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
                               (int)strlen(var->name));
            }
            collect_rename_locations(var->initializer, ctx);
            break;
        }

        case AST_VARIABLE: {
            const char *var_name = node->as.variable.name;
            if (var_name && strcmp(var_name, ctx->target_name) == 0 && should_rename(ctx)) {
                add_rename_edit(ctx, node->line, node->column > 0 ? node->column : 1,
                               (int)strlen(var_name));
            }
            break;
        }

        case AST_ASSIGNMENT: {
            const char *assign_name = node->as.assignment.name;
            if (assign_name && strcmp(assign_name, ctx->target_name) == 0 && should_rename(ctx)) {
                add_rename_edit(ctx, node->line, node->column > 0 ? node->column : 1,
                               (int)strlen(assign_name));
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
            if (fi->item_name && strcmp(fi->item_name, ctx->target_name) == 0 && should_rename(ctx)) {
                add_rename_edit(ctx, node->line, node->column > 0 ? node->column : 1,
                               (int)strlen(fi->item_name));
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
                               (int)strlen(cls->name));
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
                if (tc->catch_var && strcmp(tc->catch_var, ctx->target_name) == 0 && should_rename(ctx)) {
                    int var_line = tc->catch_var_line > 0 ? tc->catch_var_line : node->line;
                    int var_col = tc->catch_var_column > 0 ? tc->catch_var_column : 1;
                    add_rename_edit(ctx, var_line, var_col, (int)strlen(tc->catch_var));
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
    if (!doc || !doc->content) return NULL;

    uint32_t start, end;
    char *word = xlsp_word_at_position(doc, pos, &start, &end);
    if (!word) return NULL;

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

    XrJsonValue *result = xlsp_json_new_object();
    xlsp_json_object_set(result, "range",
        xlsp_json_make_range(range_start.line, range_start.character,
                             range_end.line, range_end.character));

    return result;
}

XrJsonValue *xlsp_analyze_rename(XrLspServer *server, XrLspDocument *doc,
                                  XrLspPosition pos, const char *new_name) {
    if (!doc || !doc->content || !new_name) return NULL;

    uint32_t start, end;
    char *old_name = NULL;

    old_name = xlsp_word_at_position(doc, pos, &start, &end);
    if (!old_name) return NULL;

    // Build WorkspaceEdit with changes
    XrJsonValue *result = xlsp_json_new_object();
    XrJsonValue *changes = xlsp_json_new_object();
    XrJsonValue *edits = xlsp_json_new_array();  // Always create, attach to result at end

    // Use XaScope from analyzer for unified scope handling
    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;
    if (doc->ast && analyzer && analyzer->global_scope) {
        // Phase 1: Find the scope where the symbol is defined
        RenameContext ctx = {
            .target_name = old_name,
            .target_name_len = (int)strlen(old_name),
            .target_line = pos.line + 1,  // Convert to 1-indexed
            .target_col = pos.character + 1,
            .global_scope = analyzer->global_scope,
            .def_scope = NULL,
            .current_scope = analyzer->global_scope,
            .found_def = false,
            .is_global = false,
            .edits = edits,
            .new_name = new_name
        };

        find_symbol_definition(doc->ast, &ctx);

        if (ctx.found_def && ctx.def_scope) {
            // Phase 2: Collect all rename locations using scope hierarchy
            ctx.current_scope = analyzer->global_scope;
            collect_rename_locations(doc->ast, &ctx);

            lsp_log("Rename '%s' -> '%s': found %d locations",
                    old_name, new_name, xlsp_json_array_len(edits));
        } else {
            lsp_log("Rename '%s': symbol definition not found (line %d, col %d)",
                    old_name, pos.line + 1, pos.character + 1);
        }
    } else {
        lsp_log("Rename '%s': AST or analyzer not available", old_name);
    }

    // Always attach edits array to changes (may be empty)
    if (xlsp_json_array_len(edits) > 0) {
        xlsp_json_object_set(changes, doc->uri, edits);
    }
    // Note: if edits is empty, it will be freed when result is freed

    xr_free(old_name);

    xlsp_json_object_set(result, "changes", changes);
    return result;
}

// ============================================================================
// Formatting
// ============================================================================

// AST-based formatting with comment preservation
XrJsonValue *xlsp_analyze_format(XrLspDocument *doc) {
    XrJsonValue *edits = xlsp_json_new_array();

    if (!doc || !doc->content || doc->length == 0) {
        return edits;
    }

    // Get isolate from server
    XrayIsolate *X = doc->server ? doc->server->isolate : NULL;
    if (!X) {
        return edits;
    }

    // Parse with trivia collection (preserves comments)
    AstNode *ast = xr_parse_with_trivia(X, doc->content, doc->uri);
    if (!ast) {
        // Parse failed, return empty edits
        return edits;
    }

    // Format AST
    XrFmtConfig config = xfmt_default_config;
    char *formatted = xfmt_format_ast(ast, &config, X);

    // Free AST
    xr_ast_free(X, ast);

    if (!formatted) {
        return edits;
    }

    // Create single edit that replaces entire document
    XrJsonValue *edit = xlsp_json_new_object();
    xlsp_json_object_set(edit, "range",
        xlsp_json_make_range(0, 0, doc->line_count, 0));
    xlsp_json_object_set(edit, "newText", xlsp_json_new_string(formatted));

    xlsp_json_array_push(edits, edit);

    xr_free(formatted);
    return edits;
}

XrJsonValue *xlsp_analyze_format_range(XrLspDocument *doc, XrLspRange range) {
    // For simplicity, format entire document for now
    // A more sophisticated implementation would only format the specified range
    (void)range;
    return xlsp_analyze_format(doc);
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
    XrJsonValue *result = xlsp_json_new_object();
    XrJsonValue *signatures = xlsp_json_new_array();
    XrJsonValue *sig_info = xlsp_json_new_object();
    XrJsonValue *params = xlsp_json_new_array();
    int param_count = 0;

    if (sig) {
        // Builtin function
        xlsp_json_object_set(sig_info, "label", xlsp_json_new_string(sig->signature));
        if (sig->documentation) {
            xlsp_json_object_set(sig_info, "documentation", xlsp_json_new_string(sig->documentation));
        }
        for (int i = 0; i < sig->param_count; i++) {
            XrJsonValue *param = xlsp_json_new_object();
            xlsp_json_object_set(param, "label", xlsp_json_new_string(sig->param_names[i]));
            if (sig->param_docs && sig->param_docs[i]) {
                xlsp_json_object_set(param, "documentation", xlsp_json_new_string(sig->param_docs[i]));
            }
            xlsp_json_array_push(params, param);
        }
        param_count = sig->param_count;
        xr_free(func_name);
    } else {
        // Try user-defined function from analyzer
        XaAnalyzer *analyzer = doc->server ? doc->server->workspace_analyzer : NULL;
        XaSymbol *sym = analyzer ? xa_scope_lookup(analyzer->global_scope, func_name) : NULL;
        xr_free(func_name);

        if (!sym || (sym->kind != XA_SYM_FUNCTION && sym->kind != XA_SYM_METHOD)) {
            xlsp_json_free(result);
            xlsp_json_free(sig_info);
            xlsp_json_free(params);
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
                XrJsonValue *param = xlsp_json_new_object();
                char param_label[128];
                snprintf(param_label, sizeof(param_label), "%s: %s", pname, ptype);
                xlsp_json_object_set(param, "label", xlsp_json_new_string(param_label));
                xlsp_json_array_push(params, param);
            }
            param_count = links->param_count;
        }

        const char *ret_type = (links && links->return_type)
            ? xr_type_to_string(links->return_type) : "unknown";
        snprintf(sig_label + sig_len, sizeof(sig_label) - sig_len, "): %s", ret_type);

        xlsp_json_object_set(sig_info, "label", xlsp_json_new_string(sig_label));
    }

    xlsp_json_object_set(sig_info, "parameters", params);
    xlsp_json_array_push(signatures, sig_info);
    xlsp_json_object_set(result, "signatures", signatures);

    // Active signature and parameter
    xlsp_json_object_set(result, "activeSignature", xlsp_json_new_number(0));
    int active = (param_count > 0 && active_param < param_count) ? active_param : 0;
    xlsp_json_object_set(result, "activeParameter", xlsp_json_new_number(active));

    return result;
}


