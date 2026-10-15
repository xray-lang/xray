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
#include "xlsp_source_open.h"
#include "../../api/xsource_compile.h"
#include "../../api/xsource_authority.h"
#include "xlsp_ast_utils.h"
#include "xlsp_cache.h"
#include "../../base/xfileio.h"
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
#include "xlsp_enum_fields.h"
#include "xlsp_builtins.h"
#include "xlsp_index_pool.h"
#include "xlsp_source_index_store.h"

// Static analyzer
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/analyzer/xanalyzer_escape.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xtype_pool.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../module/xmodule_identity.h"
#include "../../module/xmodule_graph.h"
#include "../../runtime/xisolate_api.h"
#include "../../api/xsource_authority.h"

// AST formatter (lives in frontend/format).
#include "../../frontend/format/xfmt.h"
#include "../../frontend/analyzer/xanalyzer_ast_visitor.h"



// Use analyzer's scope system for semantic rename (unified design)

// Data-driven keyword/builtin documentation table
typedef struct {
    const char *name;
    const char *doc;
} XlspDocEntry;

static const XlspDocEntry keyword_docs[] = {
    {"var", "```xray\nvar <name> = <value>\n```\n\nDeclares a mutable variable."},
    {"const", "```xray\nconst <name> = <value>\nvar snapshot: const Config = "
              "load()\n```\n\nDeclares a stable binding or a deep-read-only type capability."},
    {"fn", "```xray\nfn <name>(<params>) { <body> }\n```\n\nDeclares a function."},
    {"class", "```xray\nclass <name> { <members> }\n```\n\nDeclares a class."},
    {"go", "```xray\ngo <expr>\n```\n\nSpawns a new coroutine."},
    {"await", "```xray\nawait <task>\n```\n\nWaits for a coroutine to complete."},
    {"Channel",
     "```xray\nconst ch = Channel(size?)\n```\n\nCreates a stable audited synchronization handle "
     "for coroutine communication."},
    {"if", "```xray\nif (<cond>) { ... } else { ... }\n```\n\nConditional statement."},
    {"else", "```xray\nif (<cond>) { ... } else { ... }\n```\n\nAlternate branch of if statement."},
    {"while", "```xray\nwhile (<cond>) { ... }\n```\n\nLoop while condition is true."},
    {"for", "```xray\nfor (<init>; <cond>; <step>) { ... }\nfor (<item> in <iter>) { ... "
            "}\n```\n\nLoop statement."},
    {"return", "```xray\nreturn <value>?\n```\n\nReturns from a function."},
    {"break", "```xray\nbreak\n```\n\nExits the innermost loop."},
    {"continue", "```xray\ncontinue\n```\n\nSkips to the next loop iteration."},
    {"import", "```xray\nimport <module>\n```\n\nImports a module."},
    {"true", "Boolean literal `true`."},
    {"false", "Boolean literal `false`."},
    {"null", "Null literal representing absence of value."},
    {"new", "`new` has been removed. Construct classes and structs with `ClassName(args)`."},
    {"this", "Reference to the current class instance."},
    {"super", "Reference to the parent class."},
    {NULL, NULL}};

static const XlspDocEntry builtin_docs[] = {
    {"print", "```xray\nprint(value, ...)\n```\n\nPrints values to stdout."},
    {"typeOf", "```xray\ntypeOf(value): Type\n```\n\nReturns the stable TypeId for a value."},
    {"typeName", "```xray\ntypeName(value): string\ntypeName<T>(): string\n```\n\nReturns the "
                 "debug/display type name of a value or static type."},
    {"assert", "```xray\nassert(condition, message?)\n```\n\nAsserts that condition is true."},
    {"assertEqual",
     "```xray\nassertEqual(actual, expected, message?)\n```\n\nAsserts deep equality for two "
     "values of the same static type."},
    {"assertThrows",
     "```xray\nassertThrows(action, message?)\n```\n\nPasses only when action returns a "
     "typed error."},
    {"assertPanics",
     "```xray\nassertPanics(action, message?)\n```\n\nPasses only when action panics."},
    {"len", "```xray\nlen(value): i64\n```\n\nReturns the length of a string, array, or map."},
    {"range", "```xray\nrange(start, end, step?): Array\n```\n\nCreates an array of numbers."},
    {"string", "```xray\nstring(value): string\n```\n\nConverts value to string."},
    {"input", "```xray\ninput(prompt?): string\n```\n\nReads a line from stdin."},
    {NULL, NULL}};

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

char *xlsp_word_at_position(XrLspDocument *doc, XrLspPosition pos, uint32_t *out_start,
                            uint32_t *out_end) {
    if (!doc || !doc->content)
        return NULL;

    uint32_t offset = xlsp_position_to_offset(doc, pos);
    if (offset >= doc->length)
        return NULL;

    const char *content = doc->content;
    uint32_t start = offset;
    uint32_t end = offset;

    while (start > 0 && xlsp_is_ident_char(content[start - 1]))
        start--;
    while (end < doc->length && xlsp_is_ident_char(content[end]))
        end++;

    if (start == end)
        return NULL;

    size_t word_len = end - start;
    char *word = xr_malloc(word_len + 1);
    if (!word)
        return NULL;
    memcpy(word, content + start, word_len);
    word[word_len] = '\0';

    if (out_start)
        *out_start = start;
    if (out_end)
        *out_end = end;
    return word;
}

// Create a diagnostic JSON object
void xlsp_analysis_release_module_graph(XrLspServer *server) {
    if (!server)
        return;

    if (server->workspace_analyzer)
        xa_analyzer_set_graph(server->workspace_analyzer, NULL);

    /* Dependency ASTs are owned by the graph. Remove their analyzer scopes
     * before freeing it so symbol locations and names cannot outlive the AST
     * storage they borrow. The entry is analyzed from the open document AST
     * under its URI and therefore is not graph-owned. */
    if (server->module_graph && server->workspace_analyzer) {
        for (int i = 0; i < server->module_graph->spec_count; i++) {
            if (i == server->module_graph->entry_index)
                continue;
            const char *source_path = server->module_graph->specs[i].source_path;
            if (source_path)
                xa_analyzer_remove_file(server->workspace_analyzer, source_path);
        }
    }

    if (server->module_graph) {
        xr_compile_module_graph_free(server->module_graph);
        server->module_graph = NULL;
    }
    if (server->module_graph_authority) {
        xr_source_authority_close(server->module_graph_authority);
        xr_free(server->module_graph_authority);
        server->module_graph_authority = NULL;
    }
}

void xlsp_parse_document(XrLspDocument *doc,XrLspServer *server) {
    if(!doc||!server||!doc->uri||!doc->content)return;
    if(doc->source_open) {
        doc->ast=(AstNode *)xlsp_source_syntax_ast(doc->syntax_snapshot);
        doc->parse_error=xlsp_source_syntax_status(doc->syntax_snapshot)==XR_PARSE_RECOVERED||
            xlsp_source_open_query_status(doc->source_open)!=XR_XIR_OK;
    } else {
        XrCompileResourceLimits limits=xr_source_compile_default_resource_limits();
        XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;XlspSyntaxSnapshot *fresh=NULL;
        if(xr_compile_resources_new(&limits,&resources)!=XR_COMPILE_RESOURCE_OK)return;
        XrCompilerSessionStatus opened=xr_compile_session_new(resources,&session);
        XrParseStatus parsed=XR_PARSE_BAD_ARGUMENT;
        if(opened==XR_COMPILER_SESSION_OK) {
            XlspSourceDocument input={doc->uri,doc->content,strlen(doc->uri),doc->length,doc->version};
            parsed=xlsp_source_syntax_build(session,&input,&fresh);
        }
        xr_compile_session_free(session);xr_compile_resources_release(resources);
        if(parsed!=XR_PARSE_OK&&parsed!=XR_PARSE_RECOVERED){xlsp_source_syntax_free(fresh);return;}
        XlspSyntaxSnapshot *old=doc->syntax_snapshot;
        doc->syntax_snapshot=fresh;doc->ast=(AstNode *)xlsp_source_syntax_ast(fresh);
        doc->parse_error=parsed==XR_PARSE_RECOVERED;xlsp_source_syntax_free(old);
    }
    xlsp_free_document_cache(doc);
    if(xlsp_source_syntax_diagnostics_json(doc->syntax_snapshot,(XrJsonValue **)&doc->cached_diagnostics)!=XR_XIR_OK)
        lsp_log("Syntax diagnostics unavailable; no empty success cached");
    if(doc->ast&&server->source_index) {
        XlspSourceIndex *index=NULL;XlspSourceDocument input={doc->uri,doc->content,strlen(doc->uri),doc->length,doc->version};
        XrXirStatus status=xlsp_source_index_build(server->index_resources,&input,&index);
        if(status==XR_XIR_OK)status=xlsp_source_index_store_replace(server->source_index,&index);
        xlsp_source_index_free(index);
        if(status!=XR_XIR_OK){xlsp_index_record_failure(server,status);lsp_log("Open document syntax index unavailable: %u",(unsigned)status);}
    }
    doc->dirty=false;
}
XrJsonValue *xlsp_analyze_diagnostics(XrLspDocument *doc) {
    if(!doc||!doc->syntax_snapshot)return NULL;
    XrJsonValue *out=NULL;
    XrXirStatus status=doc->source_open?
        xlsp_source_open_diagnostics_json(doc->source_open,doc->source_buffer,doc->syntax_snapshot,&out):
        xlsp_source_syntax_diagnostics_json(doc->syntax_snapshot,&out);
    if(status!=XR_XIR_OK)lsp_log("Source diagnostics unavailable: status %u",(unsigned)status);
    return out;
}

XrJsonValue *xlsp_analyze_hover(XrLspServer *server, XrLspDocument *doc, XrLspPosition pos) {
    if (!doc || !doc->content)
        return NULL;
    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;

    uint32_t start, end;
    char *word = xlsp_word_at_position(doc, pos, &start, &end);
    if (!word)
        return NULL;

    const char *content = doc->content;

    char hover_buf[2048];
    XlspEnumFieldOccurrence enum_field;
    if (analyzer && doc->ast &&
        xlsp_enum_field_at(analyzer, doc->ast, doc->uri, pos, &enum_field)) {
        const char *type_name =
            enum_field.field_type ? xr_type_to_string(enum_field.field_type) : "<error>";
        snprintf(hover_buf, sizeof(hover_buf), "```xray\n%s.%s.%s: %s\n```\n\n(enum payload field)",
                 enum_field.enum_name, enum_field.variant_name, enum_field.field_name, type_name);
        XrJsonValue *result = xjson_new_object();
        XrJsonValue *contents = xjson_new_object();
        xjson_object_set(contents, "kind", xjson_new_string("markdown"));
        xjson_object_set(contents, "value", xjson_new_string(hover_buf));
        xjson_object_set(result, "contents", contents);
        xjson_object_set(result, "range",
                         xjson_make_range((int) enum_field.location.line - 1,
                                          (int) enum_field.location.column - 1,
                                          (int) enum_field.location.end_line - 1,
                                          (int) enum_field.location.end_column - 1));
        xr_free(word);
        return result;
    }

    // Check if it's a keyword (data-driven lookup)
    const char *description = lookup_doc(keyword_docs, word);

    // Check if it's a builtin (data-driven lookup)
    if (!description) {
        description = lookup_doc(builtin_docs, word);
    }

    // Check if it's a stdlib module
    if (!description) {
        const XlspModuleInfo *module = xlsp_stdlib_find_module(word);
        if (module) {
            snprintf(hover_buf, sizeof(hover_buf), "```xray\nimport %s\n```\n\n%s\n\nMembers: %d",
                     module->name, module->documentation, module->symbol_count);
            description = hover_buf;
        }
    }

    // Check if it's a local variable/function using XaAnalyzer
    // Prefer position-aware lookup to resolve shadowed names correctly,
    // then fall back to global name lookup.
    if (!description && analyzer) {
        XaSymbol *sym = xa_analyzer_lookup_at(analyzer, doc->uri, pos.line + 1, pos.character + 1);
        if (!sym)
            sym = xa_analyzer_lookup(analyzer, word);
        if (sym) {
            XrType *type = xa_analyzer_get_type(analyzer, sym);
            const char *type_str = type ? xr_type_to_string(type) : "<error>";

            if (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD) {
                XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
                int len = snprintf(hover_buf, sizeof(hover_buf), "```xray\nfn %s(", sym->name);
                if (links && links->param_count > 0) {
                    for (int p = 0; p < links->param_count; p++) {
                        if (p > 0)
                            len += snprintf(hover_buf + len, sizeof(hover_buf) - len, ", ");
                        const char *pname = (links->param_names && links->param_names[p])
                                                ? links->param_names[p]
                                                : "_";
                        XrType *ptype = (links->param_types && links->param_types[p])
                                            ? links->param_types[p]
                                            : NULL;
                        len = xlsp_append_param_display(hover_buf, sizeof(hover_buf), len, pname,
                                                        ptype,
                                                        xlsp_function_param_mode(links->type, p));
                    }
                }
                const char *ret_type = (links && links->return_type)
                                           ? xr_type_to_string(links->return_type)
                                           : type_str;
                len += snprintf(hover_buf + len, sizeof(hover_buf) - len, "): %s\n```\n\n%s",
                                ret_type, sym->is_exported ? "(exported function)" : "(function)");
                if (links && links->is_deprecated) {
                    len += snprintf(hover_buf + len, sizeof(hover_buf) - (size_t) len,
                                    "\n\nDeprecated%s%s.", links->deprecated_message ? ": " : "",
                                    links->deprecated_message ? links->deprecated_message : "");
                }
            } else if (sym->kind == XA_SYM_CLASS) {
                snprintf(hover_buf, sizeof(hover_buf),
                         "```xray\nclass %s\n```\n\n(class definition)", sym->name);
            } else if (sym->kind == XA_SYM_TYPE_ALIAS) {
                XrType *alias_type = (XrType *) sym->alias_type;
                const char *alias_str = alias_type ? xr_type_to_string(alias_type) : "<error>";
                snprintf(hover_buf, sizeof(hover_buf), "```xray\ntype %s = %s\n```\n\n(type alias)",
                         sym->name, alias_str);
            } else if (sym->kind == XA_SYM_ENUM) {
                snprintf(hover_buf, sizeof(hover_buf), "```xray\nenum %s\n```\n\n(enum definition)",
                         sym->name);
            } else {
                snprintf(hover_buf, sizeof(hover_buf), "```xray\n%s %s: %s\n```\n\n%s",
                         sym->is_const ? "const" : "var", sym->name, type_str,
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
        while (mod_start > 0 &&
               (content[mod_start - 1] == '_' ||
                (content[mod_start - 1] >= 'a' && content[mod_start - 1] <= 'z') ||
                (content[mod_start - 1] >= 'A' && content[mod_start - 1] <= 'Z') ||
                (content[mod_start - 1] >= '0' && content[mod_start - 1] <= '9'))) {
            mod_start--;
        }

        if (mod_start < mod_end) {
            size_t mod_len = mod_end - mod_start;
            char *mod_name = xr_malloc(mod_len + 1);
            if (!mod_name)
                goto cleanup_word;  // Skip module resolution on alloc failure

            memcpy(mod_name, content + mod_start, mod_len);
            mod_name[mod_len] = '\0';

            // First try stdlib
            const XlspModuleInfo *module = xlsp_stdlib_find_module(mod_name);
            if (module) {
                const XlspSymbolInfo *sym = xlsp_stdlib_find_symbol(module, word);
                if (sym) {
                    snprintf(hover_buf, sizeof(hover_buf), "```xray\n%s.%s\n%s\n```\n\n%s",
                             mod_name, sym->name, sym->signature ? sym->signature : "",
                             sym->documentation ? sym->documentation : "");
                    description = hover_buf;
                }
            }

            // Try local import
            if (!description) {
                const char *import_hover =
                    xlsp_get_import_hover(doc, mod_name, word, hover_buf, sizeof(hover_buf));
                if (import_hover) {
                    description = import_hover;
                }
            }

            // Try builtin type methods (e.g., Array.push, String.split)
            if (!description) {
                XlspBuiltinType bt = xlsp_builtin_type_from_name(mod_name);
                if (bt != XLSP_TYPE_UNRESOLVED) {
                    const char *builtin_hover =
                        xlsp_builtin_get_hover(bt, word, hover_buf, sizeof(hover_buf));
                    if (builtin_hover) {
                        description = builtin_hover;
                    }
                }
            }

            // Try infer variable type for method hover (e.g., arr.push where arr is Array)
            if (!description) {
                XaSymbol *receiver_sym =
                    analyzer ? xa_analyzer_lookup_deep(analyzer, mod_name) : NULL;
                XrType *receiver_type =
                    receiver_sym ? xa_analyzer_get_type(analyzer, receiver_sym) : NULL;
                const char *var_hover =
                    receiver_type ? xlsp_builtin_get_hover_for_type(receiver_type, word, hover_buf,
                                                                    sizeof(hover_buf))
                                  : NULL;
                if (var_hover) {
                    description = var_hover;
                } else {
                    XlspBuiltinType var_type = xlsp_infer_variable_type(server, doc, mod_name);
                    if (var_type != XLSP_TYPE_UNRESOLVED) {
                        const char *bucket_hover =
                            xlsp_builtin_get_hover(var_type, word, hover_buf, sizeof(hover_buf));
                        if (bucket_hover)
                            description = bucket_hover;
                    }
                }
            }

            xr_free(mod_name);
        }
    }

cleanup_word:
    xr_free(word);

    if (!description)
        return NULL;

    // Build hover response
    XrJsonValue *hover = xjson_new_object();

    XrJsonValue *contents = xjson_new_object();
    xjson_object_set(contents, "kind", xjson_new_string("markdown"));
    xjson_object_set(contents, "value", xjson_new_string(description));
    xjson_object_set(hover, "contents", contents);

    XrLspPosition range_start = xlsp_offset_to_position(doc, start);
    XrLspPosition range_end = xlsp_offset_to_position(doc, end);
    xjson_object_set(hover, "range",
                     xjson_make_range(range_start.line, range_start.character, range_end.line,
                                      range_end.character));

    return hover;
}

// ============================================================================
// Document Symbols (Outline)
// ============================================================================

// LSP SymbolKind constants are in xlsp_types.h

// SymbolEntry / SymbolTable types declared in xlsp_analysis.h

XrJsonValue *xlsp_analyze_document_symbols(XrLspDocument *doc) {
    XlspSourceOutline *outline=NULL;XrJsonValue *json=NULL;XrXirStatus status=XR_XIR_BAD_STAGE;
    if(doc&&doc->syntax_snapshot)status=xlsp_source_syntax_outline(doc->syntax_snapshot,&outline);
    if(status==XR_XIR_OK)status=xlsp_source_outline_json(outline,&json);
    xlsp_source_outline_free(outline);
    if(status!=XR_XIR_OK&&doc&&doc->server){doc->server->source_failure.stage=3;doc->server->source_failure.status=(unsigned)status;}
    return json;
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

static const char *type_query_params[] = {"value"};
static const char *type_query_param_docs[] = {"Value to inspect"};

static const char *assert_params[] = {"condition", "message"};
static const char *assert_param_docs[] = {"Condition to assert", "Optional error message"};

static const char *assertion_equal_params[] = {"actual", "expected", "message"};
static const char *assertion_equal_param_docs[] = {"Actual value", "Expected value",
                                                   "Optional error message"};
static const char *assert_action_params[] = {"action", "message"};
static const char *assert_action_param_docs[] = {"Zero-argument action", "Optional error message"};

static const FunctionSignature builtin_signatures[] = {
    {"print", "print(value, ...)", "Prints values to stdout", print_params, print_param_docs, 2},
    {"typeOf", "typeOf(value): Type", "Returns stable TypeId for value", type_query_params,
     type_query_param_docs, 1},
    {"typeName", "typeName(value): string | typeName<T>(): string",
     "Returns debug/display type name of a value or static type", type_query_params,
     type_query_param_docs, 1},
    {"assert", "assert(condition, message?)", "Asserts condition is true", assert_params,
     assert_param_docs, 2},
    {"assertEqual", "assertEqual(actual, expected, message?)", "Asserts deep equality",
     assertion_equal_params, assertion_equal_param_docs, 3},
    {"assertThrows", "assertThrows(action, message?)", "Expects a typed error",
     assert_action_params, assert_action_param_docs, 2},
    {"assertPanics", "assertPanics(action, message?)", "Expects a panic", assert_action_params,
     assert_action_param_docs, 2},
    {NULL, NULL, NULL, NULL, NULL, 0}};

static int xlsp_signature_param_count(const char *label) {
    if (!label)
        return 0;
    const char *open = strchr(label, '(');
    if (!open)
        return 0;
    const char *p = open + 1;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == ')')
        return 0;

    int count = 1;
    int depth = 0;
    for (; *p; p++) {
        if (*p == '(' || *p == '<')
            depth++;
        else if ((*p == ')' || *p == '>') && depth > 0)
            depth--;
        else if (*p == ')' && depth == 0)
            break;
        else if (*p == ',' && depth == 0)
            count++;
    }
    return count;
}

XrJsonValue *xlsp_analyze_signature_help(XrLspDocument *doc, XrLspPosition pos) {
    if (!doc || !doc->content)
        return NULL;

    uint32_t offset = xlsp_position_to_offset(doc, pos);
    if (offset == 0 || offset > doc->length)
        return NULL;

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

    if (!found_paren || func_end == 0)
        return NULL;

    // Find function name
    func_start = func_end;
    while (func_start > 0 && (content[func_start - 1] == '_' ||
                              (content[func_start - 1] >= 'a' && content[func_start - 1] <= 'z') ||
                              (content[func_start - 1] >= 'A' && content[func_start - 1] <= 'Z') ||
                              (content[func_start - 1] >= '0' && content[func_start - 1] <= '9'))) {
        func_start--;
    }

    if (func_start == func_end)
        return NULL;

    // Extract function name
    size_t name_len = func_end - func_start;
    char *func_name = xr_malloc(name_len + 1);
    if (!func_name)
        return NULL;

    memcpy(func_name, content + func_start, name_len);
    func_name[name_len] = '\0';

    char registry_sig_label[512];
    bool has_registry_sig = false;
    int registry_param_count = 0;
    XaAnalyzer *analyzer = doc->server ? doc->server->workspace_analyzer : NULL;

    if (func_start > 1 && content[func_start - 1] == '.' && analyzer) {
        uint32_t recv_end = func_start - 1;
        uint32_t recv_start = recv_end;
        while (recv_start > 0 &&
               (content[recv_start - 1] == '_' ||
                (content[recv_start - 1] >= 'a' && content[recv_start - 1] <= 'z') ||
                (content[recv_start - 1] >= 'A' && content[recv_start - 1] <= 'Z') ||
                (content[recv_start - 1] >= '0' && content[recv_start - 1] <= '9'))) {
            recv_start--;
        }
        size_t recv_len = (size_t) (recv_end - recv_start);
        if (recv_len > 0 && recv_len < 96) {
            char recv_name[96];
            memcpy(recv_name, content + recv_start, recv_len);
            recv_name[recv_len] = '\0';
            XaSymbol *receiver_sym = xa_analyzer_lookup_deep(analyzer, recv_name);
            XrType *receiver_type =
                receiver_sym ? xa_analyzer_get_type(analyzer, receiver_sym) : NULL;
            if (xlsp_builtin_get_signature_for_type(receiver_type, func_name, registry_sig_label,
                                                    sizeof(registry_sig_label))) {
                has_registry_sig = true;
                registry_param_count = xlsp_signature_param_count(registry_sig_label);
            }
        }
    }

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

    if (has_registry_sig) {
        xjson_object_set(sig_info, "label", xjson_new_string(registry_sig_label));
        for (int i = 0; i < registry_param_count; i++) {
            XrJsonValue *param = xjson_new_object();
            char label[32];
            snprintf(label, sizeof(label), "arg%d", i);
            xjson_object_set(param, "label", xjson_new_string(label));
            xjson_array_push(params, param);
        }
        param_count = registry_param_count;
        xr_free(func_name);
    } else if (sig) {
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
        XaSymbol *sym = analyzer ? xa_analyzer_lookup(analyzer, func_name) : NULL;
        xr_free(func_name);

        if (!sym || (sym->kind != XA_SYM_FUNCTION && sym->kind != XA_SYM_METHOD)) {
            xjson_free(result);
            xjson_free(sig_info);
            xjson_free(params);
            return NULL;
        }

        XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);

        // Build signature label: fn name(a: i64, b: str): ReturnType
        char sig_label[512];
        int sig_len = 0;
        sig_len += snprintf(sig_label + sig_len, sizeof(sig_label) - sig_len, "fn %s(", sym->name);

        if (links && links->param_count > 0) {
            for (int p = 0; p < links->param_count; p++) {
                if (p > 0)
                    sig_len += snprintf(sig_label + sig_len, sizeof(sig_label) - sig_len, ", ");
                const char *pname =
                    (links->param_names && links->param_names[p]) ? links->param_names[p] : "_";
                XrType *ptype_obj =
                    (links->param_types && links->param_types[p]) ? links->param_types[p] : NULL;
                XrParamMode mode = xlsp_function_param_mode(links->type, p);
                sig_len = xlsp_append_param_display(sig_label, sizeof(sig_label), sig_len, pname,
                                                    ptype_obj, mode);

                // Add parameter info
                XrJsonValue *param = xjson_new_object();
                char param_label[128];
                xlsp_append_param_display(param_label, sizeof(param_label), 0, pname, ptype_obj,
                                          mode);
                xjson_object_set(param, "label", xjson_new_string(param_label));
                xjson_array_push(params, param);
            }
            param_count = links->param_count;
        }

        const char *ret_type =
            (links && links->return_type) ? xr_type_to_string(links->return_type) : "<error>";
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
