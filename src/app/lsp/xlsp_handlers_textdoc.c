#include "xlsp_navigation.h"
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
#include "xlsp_workspace.h"
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

void xlsp_handle_td_did_open(XrLspServer *server,XrJsonValue *params) {
    if(!server)return;
    XrJsonValue *document=xjson_get_object(params,"textDocument");
    XrJsonValue *uri=xjson_get(document,"uri"),*text=xjson_get(document,"text"),*version=xjson_get(document,"version");
    if(!uri||uri->type!=XR_JSON_STRING||!uri->as.string||!text||text->type!=XR_JSON_STRING||!text->as.string||
        !version||version->type!=XR_JSON_NUMBER||!version->is_integer||version->as.integer<INT32_MIN||version->as.integer>INT32_MAX) {
        lsp_log("didOpen rejected: invalid bounded document input");return;
    }
    XlspSourceDocument input={uri->as.string,text->as.string,uri->string_len,text->string_len,version->as.integer};
    XrLspDocument *doc=xlsp_document_open(server,&input);
    if(!doc){lsp_log("didOpen failed; document table unchanged");return;}
    if(server->config.diagnostics_enabled)xlsp_publish_diagnostics(server,doc);
}

void xlsp_handle_td_did_change(XrLspServer *server,XrJsonValue *params) {
    if(!server)return;
    XrJsonValue *document=xjson_get_object(params,"textDocument");
    XrJsonValue *uri=xjson_get(document,"uri"),*version=xjson_get(document,"version");
    if(!uri||uri->type!=XR_JSON_STRING||!uri->as.string||memchr(uri->as.string,0,uri->string_len)||
        !version||version->type!=XR_JSON_NUMBER||!version->is_integer||version->as.integer<INT32_MIN||version->as.integer>INT32_MAX) {
        lsp_log("didChange rejected: invalid URI or version");return;
    }
    XrLspDocument *doc=xlsp_document_get(server,uri->as.string);
    if(!doc)return;
    XrXirStatus status=xlsp_document_apply_changes(doc,xjson_get(params,"contentChanges"),(int)version->as.integer);
    if(status!=XR_XIR_OK) {lsp_log("didChange rejected: status %u, previous version retained",(unsigned)status);return;}
    const char *uri_text=uri->as.string;
    (void)uri_text;
    /* Syntax and the one Source pipeline already ran before commit. */
    xlsp_schedule_diagnostics(server,doc);
}

void xlsp_handle_td_did_close(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    if (!textDocument)
        return;

    const char *uri = xjson_get_string(textDocument, "uri");
    if (uri) {
        xlsp_document_close(server, uri);
        // The closed document and obsolete Source workspace owners are gone.
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
    if (!doc)
        return xjson_new_null();

    XrLspPosition pos = {.line = (uint32_t) xjson_get_int(position, "line"),
                         .character = (uint32_t) xjson_get_int(position, "character")};

    lsp_log("handle_completion: uri=%s, line=%d, char=%d", uri, pos.line, pos.character);

    XrJsonValue *items = xlsp_analyze_completion(server, doc, pos);

    int item_count = items ? xjson_array_len(items) : 0;
    int max_items = server->config.completion_max_items;
    bool truncated = (max_items > 0 && item_count > max_items);
    if (truncated) {
        xjson_array_truncate(items, max_items);
        item_count = max_items;
    }
    lsp_log("handle_completion: returning %d items%s", item_count, truncated ? " (truncated)" : "");

    XrJsonValue *result = xjson_new_object();
    xjson_object_set(result, "isIncomplete", xjson_new_bool(truncated));
    xjson_object_set(result, "items", items);

    return result;
}

// Completion resolve: add detailed documentation from analyzer
XrJsonValue *xlsp_handle_td_completion_resolve(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *data = xjson_get_object(params, "data");
    if (!data)
        return xjson_clone(params);

    const char *uri = xjson_get_string(data, "uri");
    const char *name = xjson_get_string(params, "label");

    XrJsonValue *result = xjson_clone(params);
    if (!name)
        return result;

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
                        if (i > 0)
                            len += snprintf(doc_str + len, sizeof(doc_str) - len, ", ");
                        const char *pname = (links->param_names && links->param_names[i])
                                                ? links->param_names[i]
                                                : "_";
                        XrType *ptype_obj = (links->param_types && links->param_types[i])
                                                ? links->param_types[i]
                                                : NULL;
                        len = xlsp_append_param_display(doc_str, sizeof(doc_str), len, pname,
                                                        ptype_obj,
                                                        xlsp_function_param_mode(links->type, i));
                    }
                }
                const char *ret =
                    (links && links->return_type) ? xr_type_to_string(links->return_type) : "void";
                snprintf(doc_str + len, sizeof(doc_str) - len, "): %s\n```", ret);
                resolved = true;
            } else if (sym->kind == XA_SYM_CLASS) {
                snprintf(doc_str, sizeof(doc_str), "```xray\nclass %s\n```", name);
                resolved = true;
            } else {
                XrType *type = xa_analyzer_get_type(analyzer, sym);
                const char *type_str = type ? xr_type_to_string(type) : "<error>";
                const char *kw = sym->is_const ? "const" : "var";
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
    return xlsp_navigation_handle(server, params, 3);
}

XrJsonValue *xlsp_handle_td_document_symbol(XrLspServer *server, XrJsonValue *params) {
    if(!server)return NULL;
    server->source_failure.stage=10;server->source_failure.status=XR_XIR_BAD_STRUCTURE;
    XrJsonValue *document=xjson_get_object(params,"textDocument"),*uri=xjson_get(document,"uri");
    if(!uri||uri->type!=XR_JSON_STRING||!uri->as.string||memchr(uri->as.string,0,uri->string_len))return NULL;
    XrLspDocument *doc=xlsp_document_get(server,uri->as.string);
    if(!doc||doc->server!=server||!doc->source_open||!doc->syntax_snapshot) {
        server->source_failure.status=XR_XIR_UNRESOLVED;return NULL;
    }
    XlspSourceOutline *outline=NULL;XrJsonValue *result=NULL;
    XrXirStatus status=xlsp_source_syntax_outline(doc->syntax_snapshot,&outline);
    if(status==XR_XIR_OK)status=xlsp_source_outline_json(outline,&result);
    xlsp_source_outline_free(outline);server->source_failure.status=(unsigned)status;
    if(status==XR_XIR_OK)server->source_failure.stage=0;
    return result;
}

XrJsonValue *xlsp_handle_td_definition(XrLspServer *server, XrJsonValue *params) {
    return xlsp_navigation_handle(server, params, 0);
}

XrJsonValue *xlsp_handle_td_references(XrLspServer *server, XrJsonValue *params) {
    return xlsp_navigation_handle(server, params, 1);
}

XrJsonValue *xlsp_handle_td_rename(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *position = xjson_get_object(params, "position");
    const char *new_name = xjson_get_string(params, "newName");
    if (!textDocument || !position || !new_name)
        return xjson_new_null();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc)
        return xjson_new_null();

    XrLspPosition pos = {.line = (uint32_t) xjson_get_int(position, "line"),
                         .character = (uint32_t) xjson_get_int(position, "character")};

    return xlsp_analyze_rename(server, doc, pos, new_name);
}

XrJsonValue *xlsp_handle_td_prepare_rename(XrLspServer *server, XrJsonValue *params) {
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

    return xlsp_analyze_prepare_rename(doc, pos);
}

XrJsonValue *xlsp_handle_td_formatting(XrLspServer *server, XrJsonValue *params) {
    /* Direct handler callers have the same request-local error lifetime. */
    server->formatting_failure.stage = server->formatting_failure.status = 0;
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    if (!textDocument)
        return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc)
        return xjson_new_array();

    return xlsp_analyze_format(doc);
}

// On-type formatting: auto-indent when typing }, newline, or ;
XrJsonValue *xlsp_handle_td_on_type_formatting(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *position = xjson_get_object(params, "position");
    const char *ch = xjson_get_string(params, "ch");
    if (!textDocument || !position || !ch)
        return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->content)
        return xjson_new_array();

    int line = xjson_get_int(position, "line");
    XrJsonValue *edits = xjson_new_array();

    // Get formatting options
    XrJsonValue *options = xjson_get_object(params, "options");
    int tab_size = options ? (int) xjson_get_int(options, "tabSize") : 4;
    if (tab_size <= 0)
        tab_size = 4;

    // Get the current line content
    const char *line_start = doc->content;
    int cur_line = 0;
    while (cur_line < line && *line_start) {
        if (*line_start == '\n')
            cur_line++;
        line_start++;
    }
    const char *line_end = line_start;
    while (*line_end && *line_end != '\n')
        line_end++;

    if (ch[0] == '}') {
        // Count brace nesting up to this line to determine correct indent
        int depth = 0;
        const char *p = doc->content;
        while (p < line_start) {
            if (*p == '{')
                depth++;
            else if (*p == '}')
                depth--;
            else if (*p == '/' && p[1] == '/') {
                while (p < line_start && *p != '\n')
                    p++;
                continue;
            } else if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (p < line_start && *p != q) {
                    if (*p == '\\')
                        p++;
                    p++;
                }
            }
            p++;
        }
        // } closes one level, so indent at depth-1
        if (depth > 0)
            depth--;
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
            int ws_chars = (int) (cp - line_start);
            char indent_str[256];
            int n = target_indent < (int) sizeof(indent_str) - 1 ? target_indent
                                                                 : (int) sizeof(indent_str) - 1;
            memset(indent_str, ' ', n);
            indent_str[n] = '\0';

            XrJsonValue *edit = xjson_new_object();
            xjson_object_set(edit, "range", xjson_make_range(line, 0, line, ws_chars));
            xjson_object_set(edit, "newText", xjson_new_string(indent_str));
            xjson_array_push(edits, edit);
        }
    } else if (ch[0] == '\n') {
        // Auto-indent: match previous line's indent, +1 level if prev ends with {
        if (line <= 0)
            return edits;

        // Find previous line start
        const char *prev_line_start = doc->content;
        cur_line = 0;
        while (cur_line < line - 1 && *prev_line_start) {
            if (*prev_line_start == '\n')
                cur_line++;
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
        while (*prev_end && *prev_end != '\n')
            prev_end++;
        const char *last_non_ws = prev_end - 1;
        while (last_non_ws > prev_line_start &&
               (*last_non_ws == ' ' || *last_non_ws == '\t' || *last_non_ws == '\r'))
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
            int ws_chars = (int) (cp - line_start);
            char indent_str[256];
            int n = target_indent < (int) sizeof(indent_str) - 1 ? target_indent
                                                                 : (int) sizeof(indent_str) - 1;
            memset(indent_str, ' ', n);
            indent_str[n] = '\0';

            XrJsonValue *edit = xjson_new_object();
            xjson_object_set(edit, "range", xjson_make_range(line, 0, line, ws_chars));
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
    if (!e)
        return;
    e->name = xr_malloc(len + 1);
    if (!e->name) {
        xr_free(e);
        return;
    }
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
        if (strcmp(e->name, name) == 0)
            return e->count;
    }
    return 0;
}

// Build name reference count table in a single lexer pass
static void build_ref_count_table(const char *content, RefCountTable *table) {
    ref_table_init(table);
    if (!content)
        return;
    Scanner scanner;
    xr_scanner_init(&scanner, content);
    Token token;
    while (1) {
        token = xr_scanner_scan(&scanner);
        if (token.type == TK_EOF)
            break;
        if (token.type == TK_ERROR)
            continue;
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
    if (refs < 0)
        refs = 0;

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
static void collect_code_lens(AstNode *node, XrJsonValue *lenses, RefCountTable *ref_table) {
    if (!node)
        return;

    if (node->type == AST_FUNCTION_DECL && node->as.function_decl.name) {
        int line = node->line > 0 ? node->line - 1 : 0;
        add_code_lens(lenses, node->as.function_decl.name, line, ref_table);
    }

    if ((node->type == AST_CLASS_DECL || node->type == AST_STRUCT_DECL ||
         node->type == AST_UNION_DECL) &&
        node->as.class_decl.name) {
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
        AstNode **stmts =
            (node->type == AST_PROGRAM) ? node->as.program.statements : node->as.block.statements;
        for (int i = 0; i < count; i++) {
            collect_code_lens(stmts[i], lenses, ref_table);
        }
    }
}

XrJsonValue *xlsp_handle_td_code_lens(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    if (!textDocument)
        return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->ast)
        return xjson_new_array();

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
    if (!textDocument || !position)
        return xjson_new_null();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc)
        return xjson_new_null();

    XrLspPosition pos = {.line = (uint32_t) xjson_get_int(position, "line"),
                         .character = (uint32_t) xjson_get_int(position, "character")};

    return xlsp_analyze_signature_help(doc, pos);
}

_Static_assert(XLSP_TOKEN_FUNCTION==12 && XLSP_TOKEN_PARAMETER==7 && XLSP_TOKEN_MODIFIER==16 && XLSP_TOKEN_COUNT==22, "semantic legend order");
_Static_assert(XLSP_MOD_DECLARATION==1 && XLSP_MOD_DEFINITION==2 && XLSP_MOD_READONLY==4 && XLSP_MOD_MODIFICATION==64, "semantic modifier bits");
XrJsonValue *xlsp_handle_td_semantic_tokens_full(XrLspServer *server,XrJsonValue *params) {return xlsp_semantic_handle(server,params,0);}
XrJsonValue *xlsp_handle_td_semantic_tokens_delta(XrLspServer *server,XrJsonValue *params) {return xlsp_semantic_handle(server,params,1);}
XrJsonValue *xlsp_handle_td_semantic_tokens_range(XrLspServer *server,XrJsonValue *params) {return xlsp_semantic_handle(server,params,2);}

XrJsonValue *xlsp_handle_td_inlay_hint(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *range_obj = xjson_get_object(params, "range");
    if (!textDocument || !range_obj)
        return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc)
        return xjson_new_array();

    XrJsonValue *start = xjson_get_object(range_obj, "start");
    XrJsonValue *end = xjson_get_object(range_obj, "end");

    XrLspRange range = {.start = {.line = (uint32_t) xjson_get_int(start, "line"),
                                  .character = (uint32_t) xjson_get_int(start, "character")},
                        .end = {.line = (uint32_t) xjson_get_int(end, "line"),
                                .character = (uint32_t) xjson_get_int(end, "character")}};

    return xlsp_analyze_inlay_hints(server, doc, range);
}

// Folding range, code action, call/type hierarchy, highlight,
// workspace symbol, selection range, document link handlers
// are now in their own files. See xlsp_folding.c, xlsp_code_action.c,
// xlsp_call_hierarchy.c, xlsp_extra_handlers.c
