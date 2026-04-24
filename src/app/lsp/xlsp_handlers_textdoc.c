/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_handlers_textdoc.c - LSP text document handlers
 *   didOpen, didChange, didClose, completion, hover, definition,
 *   references, rename, formatting, semantic tokens, inlay hints, etc.
 */

#include "xlsp_handlers_textdoc.h"
#include "xlsp_server.h"
#include "../../base/xjson.h"
#include "xlsp_analysis.h"
#include "xlsp_rename.h"
#include "xlsp_semantic_tokens.h"
#include "xlsp_inlay_hints.h"
#include "xlsp_utils.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../frontend/lexer/xlex.h"
#include "../../base/xhash.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <string.h>

void xlsp_handle_td_did_open(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    if (!textDocument) return;

    const char *uri = xjson_get_string(textDocument, "uri");
    const char *text = xjson_get_string(textDocument, "text");
    int version = (int)xjson_get_int(textDocument, "version");

    if (uri && text) {
        XrLspDocument *doc = xlsp_document_open(server, uri, text, version);
        if (doc) {
            // Parse document to AST
            xlsp_parse_document(doc, server);
            if (server->config.diagnostics_enabled) {
                xlsp_publish_diagnostics(server, doc);
            }
        }
    }
}

void xlsp_handle_td_did_change(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    if (!textDocument) return;

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return;

    doc->version = (int)xjson_get_int(textDocument, "version");

    XrJsonValue *changes = xjson_get_array(params, "contentChanges");
    if (!changes) return;

    int change_count = xjson_array_len(changes);
    for (int i = 0; i < change_count; i++) {
        XrJsonValue *change = xjson_array_get(changes, i);
        const char *text = xjson_get_string(change, "text");
        XrJsonValue *range_obj = xjson_get_object(change, "range");

        if (text) {
            if (range_obj) {
                // Incremental change
                XrJsonValue *start = xjson_get_object(range_obj, "start");
                XrJsonValue *end = xjson_get_object(range_obj, "end");

                XrLspRange range = {
                    .start = {
                        .line = (uint32_t)xjson_get_int(start, "line"),
                        .character = (uint32_t)xjson_get_int(start, "character")
                    },
                    .end = {
                        .line = (uint32_t)xjson_get_int(end, "line"),
                        .character = (uint32_t)xjson_get_int(end, "character")
                    }
                };
                xlsp_document_change(doc, &range, text);
            } else {
                // Full sync
                xlsp_document_change(doc, NULL, text);
            }
        }
    }

    // Track previous error state to detect recovery
    bool had_error_before = doc->parse_error;

    // Re-parse document after change
    lsp_log("didChange: parsing %s", uri);
    xlsp_parse_document(doc, server);

    // If document recovered from error, trigger re-analysis of dependent documents
    if (had_error_before && !doc->parse_error) {
        lsp_log("didChange: document recovered from error, triggering dependent re-analysis");
        // Re-parse all open documents that might import this file
        if (server->doc_table) {
            XrLspDocTable *table = server->doc_table;
            for (int i = 0; i < table->bucket_count; i++) {
                XrLspDocBucket *bucket = table->buckets[i];
                while (bucket) {
                    XrLspDocument *other = bucket->doc;
                    if (other != doc && other->content) {
                        other->dirty = true;
                        xlsp_parse_document(other, server);
                        xlsp_schedule_diagnostics(server, other);
                    }
                    bucket = bucket->next;
                }
            }
        }
    }

    // Schedule debounced diagnostics (waits 300ms for more changes)
    xlsp_schedule_diagnostics(server, doc);
    lsp_log("didChange: diagnostics scheduled");
}

void xlsp_handle_td_did_close(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    if (!textDocument) return;

    const char *uri = xjson_get_string(textDocument, "uri");
    if (uri) {
        xlsp_document_close(server, uri);
    }
}

XrJsonValue *xlsp_handle_td_completion(XrLspServer *server, XrJsonValue *params) {
    lsp_log("handle_completion: CALLED");
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *position = xjson_get_object(params, "position");
    if (!textDocument || !position) {
        lsp_log("handle_completion: missing textDocument or position");
        return xjson_new_null();
    }

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xjson_new_null();

    XrLspPosition pos = {
        .line = (uint32_t)xjson_get_int(position, "line"),
        .character = (uint32_t)xjson_get_int(position, "character")
    };

    lsp_log("handle_completion: uri=%s, line=%d, char=%d", uri, pos.line, pos.character);

    XrJsonValue *items = xlsp_analyze_completion(server, doc, pos);

    int item_count = items ? xjson_array_len(items) : 0;
    int max_items = server->config.completion_max_items;
    bool truncated = (max_items > 0 && item_count > max_items);
    if (truncated) {
        xjson_array_truncate(items, max_items);
        item_count = max_items;
    }
    lsp_log("handle_completion: returning %d items%s", item_count,
            truncated ? " (truncated)" : "");

    XrJsonValue *result = xjson_new_object();
    xjson_object_set(result, "isIncomplete", xjson_new_bool(truncated));
    xjson_object_set(result, "items", items);

    return result;
}

// Completion resolve: add detailed documentation from analyzer
XrJsonValue *xlsp_handle_td_completion_resolve(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *data = xjson_get_object(params, "data");
    if (!data) return xjson_clone(params);

    const char *uri = xjson_get_string(data, "uri");
    const char *name = xjson_get_string(params, "label");

    XrJsonValue *result = xjson_clone(params);
    if (!name) return result;

    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;
    char doc_str[XLSP_MAX_PATH];
    int len = 0;
    bool resolved = false;

    // Try analyzer for real type information
    if (analyzer) {
        XaSymbol *sym = xa_analyzer_lookup(analyzer, name);
        if (sym) {
            XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);

            if (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD) {
                len = snprintf(doc_str, sizeof(doc_str), "```xray\nfn %s(", name);
                if (links && links->param_count > 0) {
                    for (int i = 0; i < links->param_count; i++) {
                        if (i > 0) len += snprintf(doc_str + len, sizeof(doc_str) - len, ", ");
                        const char *pname = (links->param_names && links->param_names[i])
                            ? links->param_names[i] : "_";
                        const char *ptype = (links->param_types && links->param_types[i])
                            ? xr_type_to_string(links->param_types[i]) : "unknown";
                        len += snprintf(doc_str + len, sizeof(doc_str) - len, "%s: %s", pname, ptype);
                    }
                }
                const char *ret = (links && links->return_type)
                    ? xr_type_to_string(links->return_type) : "void";
                snprintf(doc_str + len, sizeof(doc_str) - len, "): %s\n```", ret);
                resolved = true;
            } else if (sym->kind == XA_SYM_CLASS) {
                snprintf(doc_str, sizeof(doc_str), "```xray\nclass %s\n```", name);
                resolved = true;
            } else {
                XrType *type = xa_analyzer_get_type(analyzer, sym);
                const char *type_str = type ? xr_type_to_string(type) : "unknown";
                const char *kw = sym->is_const ? "const" : "let";
                snprintf(doc_str, sizeof(doc_str), "```xray\n%s %s: %s\n```", kw, name, type_str);
                resolved = true;
            }
        }
    }

    // Fallback: generic documentation from symbol table
    if (!resolved && uri) {
        XrLspDocument *doc = xlsp_document_get(server, uri);
        if (doc && doc->ast) {
            snprintf(doc_str, sizeof(doc_str), "Symbol `%s` defined in this module.", name);
            resolved = true;
        }
    }

    if (resolved) {
        XrJsonValue *documentation = xjson_new_object();
        xjson_object_set(documentation, "kind", xjson_new_string("markdown"));
        xjson_object_set(documentation, "value", xjson_new_string(doc_str));
        xjson_object_set(result, "documentation", documentation);
    }

    return result;
}

XrJsonValue *xlsp_handle_td_hover(XrLspServer *server, XrJsonValue *params) {
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

    return xlsp_analyze_hover(server, doc, pos);
}

XrJsonValue *xlsp_handle_td_document_symbol(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    if (!textDocument) return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xjson_new_array();

    return xlsp_analyze_document_symbols(doc);
}

XrJsonValue *xlsp_handle_td_definition(XrLspServer *server, XrJsonValue *params) {
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

XrJsonValue *xlsp_handle_td_references(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *position = xjson_get_object(params, "position");
    if (!textDocument || !position) return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xjson_new_array();

    XrLspPosition pos = {
        .line = (uint32_t)xjson_get_int(position, "line"),
        .character = (uint32_t)xjson_get_int(position, "character")
    };

    return xlsp_analyze_references(server, doc, pos);
}

XrJsonValue *xlsp_handle_td_rename(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *position = xjson_get_object(params, "position");
    const char *new_name = xjson_get_string(params, "newName");
    if (!textDocument || !position || !new_name) return xjson_new_null();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xjson_new_null();

    XrLspPosition pos = {
        .line = (uint32_t)xjson_get_int(position, "line"),
        .character = (uint32_t)xjson_get_int(position, "character")
    };

    return xlsp_analyze_rename(server, doc, pos, new_name);
}

XrJsonValue *xlsp_handle_td_prepare_rename(XrLspServer *server, XrJsonValue *params) {
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

    return xlsp_analyze_prepare_rename(doc, pos);
}

XrJsonValue *xlsp_handle_td_formatting(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    if (!textDocument) return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xjson_new_array();

    return xlsp_analyze_format(doc);
}

// On-type formatting: auto-indent when typing }, newline, or ;
XrJsonValue *xlsp_handle_td_on_type_formatting(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *position = xjson_get_object(params, "position");
    const char *ch = xjson_get_string(params, "ch");
    if (!textDocument || !position || !ch) return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->content) return xjson_new_array();

    int line = xjson_get_int(position, "line");
    XrJsonValue *edits = xjson_new_array();

    // Get formatting options
    XrJsonValue *options = xjson_get_object(params, "options");
    int tab_size = options ? (int)xjson_get_int(options, "tabSize") : 4;
    if (tab_size <= 0) tab_size = 4;

    // Get the current line content
    const char *line_start = doc->content;
    int cur_line = 0;
    while (cur_line < line && *line_start) {
        if (*line_start == '\n') cur_line++;
        line_start++;
    }
    const char *line_end = line_start;
    while (*line_end && *line_end != '\n') line_end++;

    if (ch[0] == '}') {
        // Count brace nesting up to this line to determine correct indent
        int depth = 0;
        const char *p = doc->content;
        while (p < line_start) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '/' && p[1] == '/') {
                while (p < line_start && *p != '\n') p++;
                continue;
            } else if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (p < line_start && *p != q) {
                    if (*p == '\\') p++;
                    p++;
                }
            }
            p++;
        }
        // } closes one level, so indent at depth-1
        if (depth > 0) depth--;
        int target_indent = depth * tab_size;

        // Calculate current indent on this line
        int current_indent = 0;
        const char *cp = line_start;
        while (cp < line_end && (*cp == ' ' || *cp == '\t')) {
            current_indent += (*cp == '\t') ? tab_size : 1;
            cp++;
        }

        if (current_indent != target_indent) {
            // Replace existing whitespace with correct indent
            int ws_chars = (int)(cp - line_start);
            char indent_str[256];
            int n = target_indent < (int)sizeof(indent_str) - 1 ? target_indent : (int)sizeof(indent_str) - 1;
            memset(indent_str, ' ', n);
            indent_str[n] = '\0';

            XrJsonValue *edit = xjson_new_object();
            xjson_object_set(edit, "range",
                xjson_make_range(line, 0, line, ws_chars));
            xjson_object_set(edit, "newText", xjson_new_string(indent_str));
            xjson_array_push(edits, edit);
        }
    } else if (ch[0] == '\n') {
        // Auto-indent: match previous line's indent, +1 level if prev ends with {
        if (line <= 0) return edits;

        // Find previous line start
        const char *prev_line_start = doc->content;
        cur_line = 0;
        while (cur_line < line - 1 && *prev_line_start) {
            if (*prev_line_start == '\n') cur_line++;
            prev_line_start++;
        }

        int prev_indent = 0;
        const char *pp = prev_line_start;
        while (*pp && *pp != '\n' && (*pp == ' ' || *pp == '\t')) {
            prev_indent += (*pp == '\t') ? tab_size : 1;
            pp++;
        }

        // Check if prev line ends with {
        const char *prev_end = prev_line_start;
        while (*prev_end && *prev_end != '\n') prev_end++;
        const char *last_non_ws = prev_end - 1;
        while (last_non_ws > prev_line_start && (*last_non_ws == ' ' || *last_non_ws == '\t' || *last_non_ws == '\r'))
            last_non_ws--;

        int target_indent = prev_indent;
        if (last_non_ws >= prev_line_start && *last_non_ws == '{')
            target_indent += tab_size;

        // Check current line indent
        int current_indent = 0;
        const char *cp = line_start;
        while (cp < line_end && (*cp == ' ' || *cp == '\t')) {
            current_indent += (*cp == '\t') ? tab_size : 1;
            cp++;
        }

        if (current_indent != target_indent) {
            int ws_chars = (int)(cp - line_start);
            char indent_str[256];
            int n = target_indent < (int)sizeof(indent_str) - 1 ? target_indent : (int)sizeof(indent_str) - 1;
            memset(indent_str, ' ', n);
            indent_str[n] = '\0';

            XrJsonValue *edit = xjson_new_object();
            xjson_object_set(edit, "range",
                xjson_make_range(line, 0, line, ws_chars));
            xjson_object_set(edit, "newText", xjson_new_string(indent_str));
            xjson_array_push(edits, edit);
        }
    }
    // For ';' we don't do anything special yet

    return edits;
}

// Name reference count table for CodeLens (single-pass scan)
#define REF_TABLE_SIZE 128

typedef struct RefCountEntry {
    char *name;
    int count;
    struct RefCountEntry *next;
} RefCountEntry;

typedef struct {
    RefCountEntry *buckets[REF_TABLE_SIZE];
} RefCountTable;

static void ref_table_init(RefCountTable *t) {
    memset(t->buckets, 0, sizeof(t->buckets));
}

static void ref_table_free(RefCountTable *t) {
    for (int i = 0; i < REF_TABLE_SIZE; i++) {
        RefCountEntry *e = t->buckets[i];
        while (e) {
            RefCountEntry *next = e->next;
            xr_free(e->name);
            xr_free(e);
            e = next;
        }
    }
}

static void ref_table_increment(RefCountTable *t, const char *name, size_t len) {
    uint32_t h = xr_hash_bytes(name, len) % REF_TABLE_SIZE;
    for (RefCountEntry *e = t->buckets[h]; e; e = e->next) {
        if (strlen(e->name) == len && strncmp(e->name, name, len) == 0) {
            e->count++;
            return;
        }
    }
    RefCountEntry *e = xr_malloc(sizeof(RefCountEntry));
    e->name = xr_malloc(len + 1);
    memcpy(e->name, name, len);
    e->name[len] = '\0';
    e->count = 1;
    e->next = t->buckets[h];
    t->buckets[h] = e;
}

static int ref_table_get(RefCountTable *t, const char *name) {
    size_t len = strlen(name);
    uint32_t h = xr_hash_bytes(name, len) % REF_TABLE_SIZE;
    for (RefCountEntry *e = t->buckets[h]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e->count;
    }
    return 0;
}

// Build name reference count table in a single lexer pass
static void build_ref_count_table(const char *content, RefCountTable *table) {
    ref_table_init(table);
    if (!content) return;
    Scanner scanner;
    xr_scanner_init(&scanner, content);
    Token token;
    while (1) {
        token = xr_scanner_scan(&scanner);
        if (token.type == TK_EOF) break;
        if (token.type == TK_ERROR) continue;
        if (token.type == TK_NAME) {
            ref_table_increment(table, token.start, token.length);
        }
    }
}

// Helper: create a CodeLens JSON object
static void add_code_lens(XrJsonValue *lenses, const char *name, int line,
                           RefCountTable *ref_table) {
    // -1 to exclude the definition itself
    int refs = ref_table_get(ref_table, name) - 1;
    if (refs < 0) refs = 0;

    char title[128];
    snprintf(title, sizeof(title), "%d reference%s", refs, refs == 1 ? "" : "s");

    XrJsonValue *lens = xjson_new_object();
    xjson_object_set(lens, "range", xjson_make_range(line, 0, line, 0));
    XrJsonValue *cmd = xjson_new_object();
    xjson_object_set(cmd, "title", xjson_new_string(title));
    xjson_object_set(cmd, "command", xjson_new_string(""));
    xjson_object_set(lens, "command", cmd);
    xjson_array_push(lenses, lens);
}

// Collect CodeLens items from AST (functions and classes)
static void collect_code_lens(AstNode *node, XrJsonValue *lenses,
                               RefCountTable *ref_table) {
    if (!node) return;

    if (node->type == AST_FUNCTION_DECL && node->as.function_decl.name) {
        int line = node->line > 0 ? node->line - 1 : 0;
        add_code_lens(lenses, node->as.function_decl.name, line, ref_table);
    }

    if ((node->type == AST_CLASS_DECL || node->type == AST_STRUCT_DECL) && node->as.class_decl.name) {
        int line = node->line > 0 ? node->line - 1 : 0;
        add_code_lens(lenses, node->as.class_decl.name, line, ref_table);

        // Also add lenses for class methods
        for (int i = 0; i < node->as.class_decl.method_count; i++) {
            collect_code_lens(node->as.class_decl.methods[i], lenses, ref_table);
        }
        return;  // Don't recurse into children again
    }

    // Recurse into children
    if (node->type == AST_PROGRAM || node->type == AST_BLOCK) {
        int count = (node->type == AST_PROGRAM) ? node->as.program.count : node->as.block.count;
        AstNode **stmts = (node->type == AST_PROGRAM) ? node->as.program.statements : node->as.block.statements;
        for (int i = 0; i < count; i++) {
            collect_code_lens(stmts[i], lenses, ref_table);
        }
    }
}

XrJsonValue *xlsp_handle_td_code_lens(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    if (!textDocument) return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->ast) return xjson_new_array();

    // Single-pass: build name→count table, then O(1) lookup per symbol
    RefCountTable ref_table;
    build_ref_count_table(doc->content, &ref_table);

    XrJsonValue *lenses = xjson_new_array();
    collect_code_lens(doc->ast, lenses, &ref_table);

    ref_table_free(&ref_table);
    return lenses;
}

XrJsonValue *xlsp_handle_td_signature_help(XrLspServer *server, XrJsonValue *params) {
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

    return xlsp_analyze_signature_help(doc, pos);
}

XrJsonValue *xlsp_handle_td_semantic_tokens_full(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    if (!textDocument) return xjson_new_null();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xjson_new_null();

    XlspSemanticTokensResult *result = xlsp_analyze_semantic_tokens(doc);

    // Cache raw tokens for delta support
    int raw_count = 0;
    uint32_t *raw = xlsp_semantic_tokens_encode_raw(result, &raw_count);
    xr_free(doc->prev_sem_tokens);
    doc->prev_sem_tokens = raw;
    doc->prev_sem_token_count = raw_count;
    doc->sem_token_result_id++;

    // Build response with resultId
    XrJsonValue *response = xjson_new_object();
    char rid[32];
    snprintf(rid, sizeof(rid), "%u", doc->sem_token_result_id);
    xjson_object_set(response, "resultId", xjson_new_string(rid));

    XrJsonValue *data = xjson_new_array();
    for (int i = 0; i < raw_count; i++) {
        xjson_array_push(data, xjson_new_number(raw[i]));
    }
    xjson_object_set(response, "data", data);

    xlsp_semantic_tokens_free(result);
    return response;
}

XrJsonValue *xlsp_handle_td_semantic_tokens_delta(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    if (!textDocument) return xjson_new_null();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xjson_new_null();

    // Compute new tokens
    XlspSemanticTokensResult *result = xlsp_analyze_semantic_tokens(doc);
    int new_count = 0;
    uint32_t *new_data = xlsp_semantic_tokens_encode_raw(result, &new_count);
    xlsp_semantic_tokens_free(result);

    // Take ownership of old cached data before updating
    uint32_t *old_data = doc->prev_sem_tokens;
    int old_count = doc->prev_sem_token_count;
    doc->prev_sem_tokens = NULL;
    doc->prev_sem_token_count = 0;

    // Update cache with copy of new data
    if (new_data && new_count > 0) {
        doc->prev_sem_tokens = xr_malloc(sizeof(uint32_t) * new_count);
        if (doc->prev_sem_tokens)
            memcpy(doc->prev_sem_tokens, new_data, sizeof(uint32_t) * new_count);
        doc->prev_sem_token_count = new_count;
    }
    doc->sem_token_result_id++;

    char rid[32];
    snprintf(rid, sizeof(rid), "%u", doc->sem_token_result_id);

    // If no previous data, return full response
    if (!old_data || old_count == 0) {
        XrJsonValue *response = xjson_new_object();
        xjson_object_set(response, "resultId", xjson_new_string(rid));
        XrJsonValue *data = xjson_new_array();
        for (int i = 0; i < new_count; i++)
            xjson_array_push(data, xjson_new_number(new_data[i]));
        xjson_object_set(response, "data", data);
        xr_free(old_data);
        xr_free(new_data);
        return response;
    }

    // Compute delta: find first and last differing positions
    int min_len = old_count < new_count ? old_count : new_count;
    int first_diff = 0;
    while (first_diff < min_len && old_data[first_diff] == new_data[first_diff])
        first_diff++;

    // Align to token boundary (5 values per token)
    first_diff = (first_diff / 5) * 5;

    int old_tail_match = 0;
    while (old_tail_match < (old_count - first_diff) &&
           old_tail_match < (new_count - first_diff) &&
           old_data[old_count - 1 - old_tail_match] == new_data[new_count - 1 - old_tail_match])
        old_tail_match++;
    old_tail_match = (old_tail_match / 5) * 5;

    int del_count = old_count - first_diff - old_tail_match;
    int ins_count = new_count - first_diff - old_tail_match;
    if (del_count < 0) del_count = 0;
    if (ins_count < 0) ins_count = 0;

    XrJsonValue *response = xjson_new_object();
    xjson_object_set(response, "resultId", xjson_new_string(rid));

    XrJsonValue *edits_arr = xjson_new_array();
    if (del_count > 0 || ins_count > 0) {
        XrJsonValue *edit = xjson_new_object();
        xjson_object_set(edit, "start", xjson_new_number(first_diff));
        xjson_object_set(edit, "deleteCount", xjson_new_number(del_count));
        if (ins_count > 0) {
            XrJsonValue *ins_data = xjson_new_array();
            for (int i = 0; i < ins_count; i++)
                xjson_array_push(ins_data, xjson_new_number(new_data[first_diff + i]));
            xjson_object_set(edit, "data", ins_data);
        }
        xjson_array_push(edits_arr, edit);
    }
    xjson_object_set(response, "edits", edits_arr);

    xr_free(old_data);
    xr_free(new_data);
    return response;
}

XrJsonValue *xlsp_handle_td_semantic_tokens_range(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *range_obj = xjson_get_object(params, "range");
    if (!textDocument) return xjson_new_null();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xjson_new_null();

    int range_start_line = 0, range_end_line = 0x7FFFFFFF;
    if (range_obj) {
        XrJsonValue *start = xjson_get_object(range_obj, "start");
        XrJsonValue *end = xjson_get_object(range_obj, "end");
        if (start) range_start_line = xjson_get_int(start, "line");
        if (end) range_end_line = xjson_get_int(end, "line");
    }

    XlspSemanticTokensResult *all = xlsp_analyze_semantic_tokens(doc);
    if (!all || all->count == 0) {
        xlsp_semantic_tokens_free(all);
        return xjson_new_null();
    }

    // Filter tokens to requested range
    XlspSemanticTokensResult filtered = { .tokens = all->tokens, .count = 0, .capacity = 0 };
    XlspSemanticToken *buf = xr_malloc(sizeof(XlspSemanticToken) * all->count);
    for (int i = 0; i < all->count; i++) {
        if (all->tokens[i].line >= range_start_line && all->tokens[i].line <= range_end_line) {
            buf[filtered.count++] = all->tokens[i];
        }
    }
    filtered.tokens = buf;

    XrJsonValue *response = xlsp_semantic_tokens_encode(&filtered);
    xr_free(buf);
    xlsp_semantic_tokens_free(all);

    return response;
}

XrJsonValue *xlsp_handle_td_inlay_hint(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *range_obj = xjson_get_object(params, "range");
    if (!textDocument || !range_obj) return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc) return xjson_new_array();

    XrJsonValue *start = xjson_get_object(range_obj, "start");
    XrJsonValue *end = xjson_get_object(range_obj, "end");

    XrLspRange range = {
        .start = {
            .line = (uint32_t)xjson_get_int(start, "line"),
            .character = (uint32_t)xjson_get_int(start, "character")
        },
        .end = {
            .line = (uint32_t)xjson_get_int(end, "line"),
            .character = (uint32_t)xjson_get_int(end, "character")
        }
    };

    return xlsp_analyze_inlay_hints(server, doc, range);
}

// Folding range, code action, call/type hierarchy, highlight,
// workspace symbol, selection range, document link handlers
// are now in their own files. See xlsp_folding.c, xlsp_code_action.c,
// xlsp_call_hierarchy.c, xlsp_extra_handlers.c

