/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_completion.c - Code completion implementation
 *
 * KEY CONCEPT:
 *   Provides code completion by analyzing symbols, modules, and types.
 */

#include "xlsp_completion.h"
#include "xlsp_json.h"
#include "xlsp_utils.h"
#include "xlsp_stdlib.h"
#include "xlsp_imports.h"
#include "xlsp_builtins.h"
#include "xlsp_workspace.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/analyzer/xanalyzer_builtins.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../frontend/parser/xast_types.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// lsp_log declared in xlsp_server.h (included via xlsp_completion.h)

// Shared keyword definitions
#include "xlsp_keywords.h"

// Literal type markers
#define LITERAL_STRING "__string__"
#define LITERAL_ARRAY  "__array__"
#define LITERAL_MAP    "__map__"

// Create a completion item with markdown documentation for syntax highlighting
static XrJsonValue *make_completion_item(const char *label, int kind, const char *detail) {
    XrJsonValue *item = xlsp_json_new_object();
    xlsp_json_object_set(item, "label", xlsp_json_new_string(label));
    xlsp_json_object_set(item, "kind", xlsp_json_new_number(kind));
    if (detail) {
        xlsp_json_object_set(item, "detail", xlsp_json_new_string(detail));
        
        // Add markdown documentation with syntax highlighting
        char doc_buf[XLSP_MAX_PATH];
        snprintf(doc_buf, sizeof(doc_buf), "```xray\n%s\n```", detail);
        
        XrJsonValue *documentation = xlsp_json_new_object();
        xlsp_json_object_set(documentation, "kind", xlsp_json_new_string("markdown"));
        xlsp_json_object_set(documentation, "value", xlsp_json_new_string(doc_buf));
        xlsp_json_object_set(item, "documentation", documentation);
    }
    return item;
}

// Generate completions for enum value instance (name, value, ordinal, toString)
static XrJsonValue *make_enum_value_completions(const char *enum_name) {
    XrJsonValue *items = xlsp_json_new_array();
    char detail[256];

    snprintf(detail, sizeof(detail), "%s.name: string", enum_name);
    XrJsonValue *i1 = make_completion_item("name", 10, detail);
    xlsp_json_object_set(i1, "sortText", xlsp_json_new_string("0"));
    xlsp_json_array_push(items, i1);

    snprintf(detail, sizeof(detail), "%s.value: any", enum_name);
    XrJsonValue *i2 = make_completion_item("value", 10, detail);
    xlsp_json_object_set(i2, "sortText", xlsp_json_new_string("1"));
    xlsp_json_array_push(items, i2);

    snprintf(detail, sizeof(detail), "%s.ordinal: int", enum_name);
    XrJsonValue *i3 = make_completion_item("ordinal", 10, detail);
    xlsp_json_object_set(i3, "sortText", xlsp_json_new_string("2"));
    xlsp_json_array_push(items, i3);

    snprintf(detail, sizeof(detail), "%s.toString(): string", enum_name);
    XrJsonValue *i4 = make_completion_item("toString", 2, detail);
    xlsp_json_object_set(i4, "sortText", xlsp_json_new_string("3"));
    xlsp_json_array_push(items, i4);

    return items;
}

// Find enum declaration by name in document AST
static AstNode *find_enum_in_ast(AstNode *ast, const char *name) {
    if (!ast || ast->type != AST_PROGRAM) return NULL;
    for (int i = 0; i < ast->as.program.count; i++) {
        AstNode *stmt = ast->as.program.statements[i];
        if (!stmt) continue;
        // Direct enum declaration
        if (stmt->type == AST_ENUM_DECL && stmt->as.enum_decl.name &&
            strcmp(stmt->as.enum_decl.name, name) == 0) {
            return stmt;
        }
        // Exported enum: export wraps enum_decl
        if (stmt->type == AST_EXPORT_STMT && stmt->as.export_stmt.declaration &&
            stmt->as.export_stmt.declaration->type == AST_ENUM_DECL &&
            stmt->as.export_stmt.declaration->as.enum_decl.name &&
            strcmp(stmt->as.export_stmt.declaration->as.enum_decl.name, name) == 0) {
            return stmt->as.export_stmt.declaration;
        }
    }
    return NULL;
}

// Check if prefix is an enum member access (e.g., "Red" after "Color.Red.")
// Returns the enum name if found, NULL otherwise
static const char *find_enum_for_member_prefix(XrLspDocument *doc, XrLspPosition pos,
                                                 const char *prefix) {
    if (!doc || !doc->ast || !doc->content || !prefix) return NULL;

    uint32_t offset = xlsp_position_to_offset(doc, pos);
    if (offset < 2) return NULL;
    const char *content = doc->content;

    // Scan backward past prefix text and the dot before it
    // Pattern: "EnumName.MemberName." where cursor is after the last dot
    // find_module_prefix already extracted "MemberName" as prefix
    // We need to find "EnumName" before that
    uint32_t scan = offset;
    // Skip back past any partial text after last dot
    while (scan > 0 && content[scan - 1] != '.' && content[scan - 1] != '\n') scan--;
    if (scan == 0 || content[scan - 1] != '.') return NULL;
    uint32_t dot2 = scan - 1; // position of second dot (MemberName.)

    // Now scan back past the member name
    uint32_t member_end = dot2;
    while (member_end > 0 && ((content[member_end - 1] >= 'a' && content[member_end - 1] <= 'z') ||
           (content[member_end - 1] >= 'A' && content[member_end - 1] <= 'Z') ||
           (content[member_end - 1] >= '0' && content[member_end - 1] <= '9') ||
           content[member_end - 1] == '_')) {
        member_end--;
    }

    // Check for first dot
    if (member_end == 0 || content[member_end - 1] != '.') return NULL;
    uint32_t dot1 = member_end - 1;

    // Extract enum name before first dot
    uint32_t enum_end = dot1;
    uint32_t enum_start = enum_end;
    while (enum_start > 0 && ((content[enum_start - 1] >= 'a' && content[enum_start - 1] <= 'z') ||
           (content[enum_start - 1] >= 'A' && content[enum_start - 1] <= 'Z') ||
           (content[enum_start - 1] >= '0' && content[enum_start - 1] <= '9') ||
           content[enum_start - 1] == '_')) {
        enum_start--;
    }

    if (enum_start == enum_end) return NULL;
    size_t len = enum_end - enum_start;
    if (len >= 64) return NULL;

    static char enum_name_buf[64];
    memcpy(enum_name_buf, content + enum_start, len);
    enum_name_buf[len] = '\0';

    // Verify it's actually an enum in the AST
    if (find_enum_in_ast(doc->ast, enum_name_buf)) {
        return enum_name_buf;
    }
    return NULL;
}

// Find module prefix before dot (e.g., "time" in "time.now")
static const char *find_module_prefix(XrLspDocument *doc, XrLspPosition pos, 
                                       char *buf, size_t buf_size) {
    uint32_t offset = xlsp_position_to_offset(doc, pos);
    if (offset == 0) return NULL;
    
    const char *content = doc->content;
    uint32_t dot_idx = 0;
    
    if (offset > 0 && content[offset - 1] == '.') {
        dot_idx = offset - 1;
    } else {
        uint32_t scan = offset;
        while (scan > 0 && content[scan - 1] != '.' && content[scan - 1] != '\n') scan--;
        if (scan > 0 && content[scan - 1] == '.') dot_idx = scan - 1;
    }
    
    if (dot_idx == 0) return NULL;
    
    char before_dot = content[dot_idx - 1];
    
    if (before_dot == '"' || before_dot == '\'') {
        strncpy(buf, LITERAL_STRING, buf_size - 1);
        buf[buf_size - 1] = '\0';
        return buf;
    }
    if (before_dot == ']') {
        strncpy(buf, LITERAL_ARRAY, buf_size - 1);
        buf[buf_size - 1] = '\0';
        return buf;
    }
    if (before_dot == '}') {
        int brace_count = 1;
        uint32_t scan = dot_idx - 2;
        uint32_t brace_start = 0;
        bool has_arrow = false;
        while (scan > 0 && brace_count > 0) {
            if (content[scan] == '}') brace_count++;
            else if (content[scan] == '{') {
                brace_count--;
                if (brace_count == 0) brace_start = scan;
            } else if (brace_count == 1 && content[scan] == '>' && scan > 0 && content[scan-1] == '=') {
                has_arrow = true;
            }
            scan--;
        }
        bool is_map = has_arrow || (brace_start > 0 && content[brace_start - 1] == '#');
        strncpy(buf, is_map ? LITERAL_MAP : "__json__", buf_size - 1);
        buf[buf_size - 1] = '\0';
        return buf;
    }
    if (before_dot == ')') return NULL;
    
    uint32_t name_end = dot_idx;
    uint32_t name_start = dot_idx;
    while (name_start > 0 && (content[name_start - 1] == '_' ||
           (content[name_start - 1] >= 'a' && content[name_start - 1] <= 'z') ||
           (content[name_start - 1] >= 'A' && content[name_start - 1] <= 'Z') ||
           (content[name_start - 1] >= '0' && content[name_start - 1] <= '9'))) {
        name_start--;
    }
    
    size_t name_len = name_end - name_start;
    if (name_len == 0 || name_len >= buf_size) return NULL;
    
    memcpy(buf, content + name_start, name_len);
    buf[name_len] = '\0';
    return buf;
}

// Convert XrType to XrTypeId (delegates to unified analyzer function)
static XrTypeId xr_type_to_builtin(XrType *type) {
    return xr_type_to_builtin_id(type);
}

// Fallback: infer type by scanning source text (used when analyzer unavailable)
static XlspBuiltinType infer_type_from_source(const char *content, const char *var_name) {
    if (!content || !var_name) return XLSP_TYPE_UNKNOWN;
    
    size_t var_len = strlen(var_name);
    const char *p = content;
    
    while ((p = strstr(p, var_name)) != NULL) {
        const char *line_start = p;
        while (line_start > content && line_start[-1] != '\n') line_start--;
        while (*line_start == ' ' || *line_start == '\t') line_start++;
        
        if (strncmp(line_start, "let ", 4) == 0 || strncmp(line_start, "const ", 6) == 0) {
            const char *after = p + var_len;
            while (*after == ' ' || *after == '\t') after++;
            
            if (*after == ':') {
                after++;
                while (*after == ' ' || *after == '\t') after++;
                if (strncmp(after, TYPE_NAME_INT, 3) == 0) return XLSP_TYPE_INT;
                if (strncmp(after, TYPE_NAME_FLOAT, 5) == 0) return XLSP_TYPE_FLOAT;
                if (strncmp(after, TYPE_NAME_STRING, 6) == 0) return XLSP_TYPE_STRING;
                if (strncmp(after, TYPE_NAME_BOOL, 4) == 0) return XLSP_TYPE_BOOL;
                if (strncmp(after, TYPE_NAME_ARRAY, 5) == 0) return XLSP_TYPE_ARRAY;
                if (strncmp(after, TYPE_NAME_MAP, 3) == 0) return XLSP_TYPE_MAP;
                if (strncmp(after, TYPE_NAME_SET, 3) == 0) return XLSP_TYPE_SET;
                if (strncmp(after, "Bytes", 5) == 0) return XLSP_TYPE_ARRAY;
                if (strncmp(after, TYPE_NAME_CHANNEL, 7) == 0) return XLSP_TYPE_CHANNEL;
                if (strncmp(after, TYPE_NAME_JSON, 4) == 0) return XLSP_TYPE_JSON;
            }
            if (*after == '=' && after[1] != '=') {
                after++;
                while (*after == ' ' || *after == '\t') after++;
                return xlsp_infer_literal_type(after);
            }
        }
        p++;
    }
    return XLSP_TYPE_UNKNOWN;
}

// Infer variable type: prioritize XaAnalyzer, fallback to source scanning
XlspBuiltinType xlsp_infer_variable_type(XrLspServer *server, XrLspDocument *doc, const char *var_name) {
    if (!doc || !var_name) return XLSP_TYPE_UNKNOWN;
    
    // Use XaAnalyzer if available (preferred: accurate type from AST)
    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;
    if (analyzer) {
        XaSymbol *sym = xa_analyzer_lookup(analyzer, var_name);
        if (sym) {
            XrType *type = xa_analyzer_get_type(analyzer, sym);
            XlspBuiltinType bt = xr_type_to_builtin(type);
            if (bt != XLSP_TYPE_UNKNOWN) return bt;
        }
    }
    
    // Fallback: scan source text
    return infer_type_from_source(doc->content, var_name);
}

static XrJsonValue *complete_basic(XrLspServer *server, XrLspDocument *doc, XrLspPosition pos) {
    XrJsonValue *items = xlsp_json_new_array();
    
    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;
    if (doc && analyzer) {
        int sym_count = 0;
        XaSymbol **symbols = xa_analyzer_get_scope_symbols(analyzer, NULL, &sym_count);
        
        for (int i = 0; i < sym_count; i++) {
            XaSymbol *sym = symbols[i];
            if (!sym || !sym->name) continue;
            if (sym->location.line > (uint32_t)(pos.line + 1)) continue;
            
            int kind;
            const char *detail;
            static char detail_buf[512];
            XrType *type = xa_analyzer_get_type(analyzer, sym);
            
            if (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD) {
                kind = 3;
                
                // Build detailed function signature: fn(a: int, b: str): ReturnType
                XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
                char sig_buf[512];
                int sig_len = 0;
                sig_len += snprintf(sig_buf + sig_len, sizeof(sig_buf) - sig_len, "fn %s(", sym->name);
                
                if (links && links->param_count > 0) {
                    for (int p = 0; p < links->param_count; p++) {
                        if (p > 0) sig_len += snprintf(sig_buf + sig_len, sizeof(sig_buf) - sig_len, ", ");
                        const char *pname = (links->param_names && links->param_names[p]) 
                            ? links->param_names[p] : "_";
                        const char *ptype = (links->param_types && links->param_types[p])
                            ? xr_type_to_string(links->param_types[p]) : "unknown";
                        sig_len += snprintf(sig_buf + sig_len, sizeof(sig_buf) - sig_len, "%s: %s", pname, ptype);
                    }
                }
                
                const char *ret_type = (links && links->return_type) 
                    ? xr_type_to_string(links->return_type) 
                    : (type ? xr_type_to_string(type) : "unknown");
                snprintf(sig_buf + sig_len, sizeof(sig_buf) - sig_len, "): %s", ret_type);
                
                strncpy(detail_buf, sig_buf, sizeof(detail_buf) - 1);
                detail_buf[sizeof(detail_buf) - 1] = '\0';
                detail = detail_buf;
            } else if (sym->kind == XA_SYM_CLASS) {
                kind = 7;
                // Show class with field/method count
                XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);
                if (links && links->class_info) {
                    XrClassInfo *info = links->class_info;
                    snprintf(detail_buf, sizeof(detail_buf), "class %s { %d fields, %d methods }",
                             sym->name, info->field_count, info->method_count);
                } else {
                    snprintf(detail_buf, sizeof(detail_buf), "class %s", sym->name);
                }
                detail = detail_buf;
            } else if (sym->kind == XA_SYM_TYPE_ALIAS) {
                kind = 25;  // LSP TypeParameter kind
                XrType *alias_type = (XrType *)sym->alias_type;
                const char *alias_str = alias_type ? xr_type_to_string(alias_type) : "unknown";
                snprintf(detail_buf, sizeof(detail_buf), "type %s = %s", sym->name, alias_str);
                detail = detail_buf;
            } else if (sym->kind == XA_SYM_ENUM) {
                kind = 13;  // LSP Enum kind
                snprintf(detail_buf, sizeof(detail_buf), "enum %s", sym->name);
                detail = detail_buf;
            } else {
                kind = 6;
                const char *type_str = type ? xr_type_to_string(type) : "unknown";
                snprintf(detail_buf, sizeof(detail_buf), "%s: %s", 
                         sym->is_const ? "const" : "let", type_str);
                detail = detail_buf;
            }
            
            XrJsonValue *item = make_completion_item(sym->name, kind, detail);
            xlsp_json_object_set(item, "sortText", xlsp_json_new_string("0"));
            xlsp_json_array_push(items, item);
        }
        free(symbols);
    }
    
    for (int i = 0; xr_keywords[i]; i++) {
        XrJsonValue *item = make_completion_item(xr_keywords[i], 14, "keyword");
        xlsp_json_object_set(item, "sortText", xlsp_json_new_string("2"));
        xlsp_json_array_push(items, item);
    }
    
    for (int i = 0; xr_builtins[i]; i++) {
        XrJsonValue *item = make_completion_item(xr_builtins[i], 3, "builtin function");
        xlsp_json_object_set(item, "sortText", xlsp_json_new_string("1"));
        xlsp_json_array_push(items, item);
    }
    
    int module_count;
    const XlspModuleInfo *modules = xlsp_stdlib_get_modules(&module_count);
    for (int i = 0; i < module_count; i++) {
        XrJsonValue *item = make_completion_item(modules[i].name, 9, modules[i].documentation);
        xlsp_json_object_set(item, "sortText", xlsp_json_new_string("3"));
        xlsp_json_array_push(items, item);
    }
    
    // Enum type names from document AST (analyzer doesn't register enums)
    if (doc && doc->ast && doc->ast->type == AST_PROGRAM) {
        char detail_buf2[256];
        for (int i = 0; i < doc->ast->as.program.count; i++) {
            AstNode *stmt = doc->ast->as.program.statements[i];
            if (!stmt) continue;
            AstNode *enum_node = NULL;
            if (stmt->type == AST_ENUM_DECL) {
                enum_node = stmt;
            } else if (stmt->type == AST_EXPORT_STMT && stmt->as.export_stmt.declaration &&
                       stmt->as.export_stmt.declaration->type == AST_ENUM_DECL) {
                enum_node = stmt->as.export_stmt.declaration;
            }
            if (enum_node && enum_node->as.enum_decl.name) {
                snprintf(detail_buf2, sizeof(detail_buf2), "enum %s { %d members }",
                         enum_node->as.enum_decl.name, enum_node->as.enum_decl.member_count);
                XrJsonValue *item = make_completion_item(
                    enum_node->as.enum_decl.name, 13, detail_buf2);
                xlsp_json_object_set(item, "sortText", xlsp_json_new_string("0"));
                xlsp_json_array_push(items, item);
            }
        }
    }
    
    return items;
}

XrJsonValue *xlsp_analyze_completion(XrLspServer *server, XrLspDocument *doc, XrLspPosition pos) {
    lsp_log("xlsp_analyze_completion_with_server: start");
    
    // Return empty list immediately if document has parse error - don't crash
    if (!doc || !doc->content) {
        lsp_log("xlsp_analyze_completion_with_server: no doc or content");
        return xlsp_json_new_array();
    }
    
    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;
    char module_name[64];
    const char *prefix = find_module_prefix(doc, pos, module_name, sizeof(module_name));
    lsp_log("xlsp_analyze_completion_with_server: prefix=%s", prefix ? prefix : "(null)");
    
    if (prefix) {
        const XlspModuleInfo *module = xlsp_stdlib_find_module(prefix);
        if (module) {
            XrJsonValue *items = xlsp_json_new_array();
            for (int i = 0; i < module->symbol_count; i++) {
                const XlspSymbolInfo *sym = &module->symbols[i];
                XrJsonValue *item = xlsp_json_new_object();
                xlsp_json_object_set(item, "label", xlsp_json_new_string(sym->name));
                xlsp_json_object_set(item, "kind", xlsp_json_new_number(sym->kind));
                if (sym->signature) xlsp_json_object_set(item, "detail", xlsp_json_new_string(sym->signature));
                xlsp_json_array_push(items, item);
            }
            return items;
        }
        
        XlspBuiltinType builtin_type = xlsp_builtin_type_from_name(prefix);
        if (builtin_type != XLSP_TYPE_UNKNOWN) return xlsp_builtin_get_completions(builtin_type);
        
        // Global objects (Coro, CoroPool, Reflect, Type) and manually-defined modules
        const XaBuiltinModule *rt_mod = xa_builtin_get_module_info(prefix);
        if (rt_mod) {
            XrJsonValue *items = xlsp_json_new_array();
            for (int i = 0; i < rt_mod->function_count; i++) {
                const XaBuiltinMember *fn = &rt_mod->functions[i];
                XrJsonValue *item = xlsp_json_new_object();
                xlsp_json_object_set(item, "label", xlsp_json_new_string(fn->name));
                int kind = fn->is_method ? XLSP_KIND_METHOD : XLSP_KIND_PROPERTY;
                xlsp_json_object_set(item, "kind", xlsp_json_new_number(kind));
                if (fn->signature) {
                    char detail[256];
                    snprintf(detail, sizeof(detail), "%s%s", fn->name, fn->signature);
                    xlsp_json_object_set(item, "detail", xlsp_json_new_string(detail));
                }
                if (fn->doc) {
                    xlsp_json_object_set(item, "documentation", xlsp_json_new_string(fn->doc));
                }
                xlsp_json_array_push(items, item);
            }
            return items;
        }
        
        if (strcmp(prefix, LITERAL_STRING) == 0) return xlsp_builtin_get_completions(XLSP_TYPE_STRING);
        if (strcmp(prefix, LITERAL_ARRAY) == 0) return xlsp_builtin_get_completions(XLSP_TYPE_ARRAY);
        if (strcmp(prefix, LITERAL_MAP) == 0) return xlsp_builtin_get_completions(XLSP_TYPE_MAP);
        if (strcmp(prefix, "__json__") == 0) return xlsp_builtin_get_completions(XLSP_TYPE_JSON);
        
        XrJsonValue *import_items = xlsp_get_import_completions(doc, prefix);
        if (xlsp_json_array_len(import_items) > 0) return import_items;
        xlsp_json_free(import_items);
        
        // Enum member completion: prefix IS an enum name (e.g., Color.Red)
        if (doc->ast) {
            AstNode *enum_node = find_enum_in_ast(doc->ast, prefix);
            if (enum_node) {
                XrJsonValue *items = xlsp_json_new_array();
                char detail_buf[256];
                for (int i = 0; i < enum_node->as.enum_decl.member_count; i++) {
                    AstNode *member = enum_node->as.enum_decl.members[i];
                    if (member && member->type == AST_ENUM_MEMBER && member->as.enum_member.name) {
                        snprintf(detail_buf, sizeof(detail_buf), "%s.%s", prefix, member->as.enum_member.name);
                        XrJsonValue *item = make_completion_item(
                            member->as.enum_member.name, XR_COMPLETION_ENUM_MEMBER, detail_buf);
                        xlsp_json_object_set(item, "sortText", xlsp_json_new_string("0"));
                        xlsp_json_array_push(items, item);
                    }
                }
                if (xlsp_json_array_len(items) > 0) return items;
                xlsp_json_free(items);
            }
        }
        
        // Class instance with cross-file support
        if (doc->content && analyzer) {
            const char *class_name = NULL;
            size_t prefix_len = strlen(prefix);
            char let_pattern[128], const_pattern[128];
            snprintf(let_pattern, sizeof(let_pattern), "let %s", prefix);
            snprintf(const_pattern, sizeof(const_pattern), "const %s", prefix);
            
            const char *p = doc->content;
            while (p && *p) {
                const char *let_match = strstr(p, let_pattern);
                const char *const_match = strstr(p, const_pattern);
                const char *match = NULL;
                int skip_len = 0;
                
                if (let_match && (!const_match || let_match < const_match)) {
                    match = let_match; skip_len = 4 + prefix_len;
                } else if (const_match) {
                    match = const_match; skip_len = 6 + prefix_len;
                }
                if (!match) break;
                
                if (match > doc->content) {
                    char prev = match[-1];
                    if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
                        (prev >= '0' && prev <= '9') || prev == '_') { p = match + 1; continue; }
                }
                
                const char *after = match + skip_len;
                if ((*after >= 'a' && *after <= 'z') || (*after >= 'A' && *after <= 'Z') ||
                    (*after >= '0' && *after <= '9') || *after == '_') { p = match + 1; continue; }
                
                while (*after == ' ' || *after == '\t') after++;
                if (*after == '=' && after[1] != '=') {
                    after++;
                    while (*after == ' ' || *after == '\t') after++;
                    
                    // Literal types
                    if (*after == '[') return xlsp_builtin_get_completions(XLSP_TYPE_ARRAY);
                    if (*after == '"' || *after == '\'') return xlsp_builtin_get_completions(XLSP_TYPE_STRING);
                    if (*after == '#' && after[1] == '{') return xlsp_builtin_get_completions(XLSP_TYPE_MAP);
                    if (*after == '#' && after[1] == '[') return xlsp_builtin_get_completions(XLSP_TYPE_SET);
                    if (*after == '{') return xlsp_builtin_get_completions(XLSP_TYPE_JSON);
                    
                    // Constructors
                    if (strncmp(after, TYPE_NAME_ARRAY, 5) == 0) return xlsp_builtin_get_completions(XLSP_TYPE_ARRAY);
                    if (strncmp(after, TYPE_NAME_MAP, 3) == 0) return xlsp_builtin_get_completions(XLSP_TYPE_MAP);
                    if (strncmp(after, TYPE_NAME_SET, 3) == 0) return xlsp_builtin_get_completions(XLSP_TYPE_SET);
                    if (strncmp(after, "Bytes", 5) == 0) return xlsp_builtin_get_completions(XLSP_TYPE_ARRAY);
                    if (strncmp(after, TYPE_NAME_CHANNEL, 7) == 0) return xlsp_builtin_get_completions(XLSP_TYPE_CHANNEL);
                    
                    if (strncmp(after, "new ", 4) == 0) {
                        after += 4;
                        while (*after == ' ' || *after == '\t') after++;
                        const char *name_start = after;
                        while ((*after >= 'a' && *after <= 'z') || (*after >= 'A' && *after <= 'Z') ||
                               (*after >= '0' && *after <= '9') || *after == '_') after++;
                        if (after > name_start) {
                            static char found_class[64];
                            size_t len = after - name_start;
                            if (len < sizeof(found_class)) {
                                memcpy(found_class, name_start, len);
                                found_class[len] = '\0';
                                class_name = found_class;
                            }
                        }
                    }
                    break;
                }
                p = match + 1;
            }
            
            // Static member completion: prefix IS a class name (e.g., Math.divmod)
            if (!class_name) {
                XrClassInfo *static_cls = xa_analyzer_get_class(analyzer, prefix);
                if (static_cls && (static_cls->static_method_count > 0 || static_cls->static_field_count > 0)) {
                    XrJsonValue *items = xlsp_json_new_array();
                    char detail_buf[512];
                    
                    for (XrClassInfo *c = static_cls; c != NULL; c = c->base) {
                        // Static fields
                        for (int i = 0; i < c->static_field_count; i++) {
                            XaSymbol *f = c->static_fields[i];
                            if (f && f->name && !f->is_private) {
                                XaSymbolLinks *links = xa_analyzer_get_links(analyzer, f);
                                const char *type_str = (links && links->type) 
                                    ? xr_type_to_string(links->type) : "unknown";
                                snprintf(detail_buf, sizeof(detail_buf), "static %s.%s: %s", 
                                         prefix, f->name, type_str);
                                XrJsonValue *item = make_completion_item(f->name, 5, detail_buf);
                                xlsp_json_object_set(item, "sortText", xlsp_json_new_string("0"));
                                xlsp_json_array_push(items, item);
                            }
                        }
                        // Static methods
                        for (int i = 0; i < c->static_method_count; i++) {
                            XaSymbol *m = c->static_methods[i];
                            if (m && m->name && !m->is_private) {
                                XaSymbolLinks *links = xa_analyzer_get_links(analyzer, m);
                                int sig_len = snprintf(detail_buf, sizeof(detail_buf), 
                                                       "static %s.%s(", prefix, m->name);
                                if (links && links->param_count > 0) {
                                    for (int p = 0; p < links->param_count; p++) {
                                        if (p > 0) sig_len += snprintf(detail_buf + sig_len, 
                                                                       sizeof(detail_buf) - sig_len, ", ");
                                        const char *pname = (links->param_names && links->param_names[p]) 
                                            ? links->param_names[p] : "_";
                                        const char *ptype = (links->param_types && links->param_types[p])
                                            ? xr_type_to_string(links->param_types[p]) : "unknown";
                                        sig_len += snprintf(detail_buf + sig_len, sizeof(detail_buf) - sig_len, 
                                                           "%s: %s", pname, ptype);
                                    }
                                }
                                const char *ret_type = (links && links->return_type) 
                                    ? xr_type_to_string(links->return_type) : "unknown";
                                snprintf(detail_buf + sig_len, sizeof(detail_buf) - sig_len, "): %s", ret_type);
                                
                                XrJsonValue *item = make_completion_item(m->name, 2, detail_buf);
                                xlsp_json_object_set(item, "sortText", xlsp_json_new_string("1"));
                                xlsp_json_array_push(items, item);
                            }
                        }
                    }
                    if (xlsp_json_array_len(items) > 0) return items;
                    xlsp_json_free(items);
                }
            }
            
            lsp_log("completion: prefix=%s, class_name=%s", prefix, class_name ? class_name : "(null)");
            if (class_name) {
                XrClassInfo *cls_info = xa_analyzer_get_class(analyzer, class_name);
                lsp_log("completion: xa_analyzer_get_class(%s) returned %p, fields=%d, methods=%d", 
                        class_name, (void*)cls_info, 
                        cls_info ? cls_info->field_count : 0,
                        cls_info ? cls_info->method_count : 0);
                
                if (cls_info) {
                    XrJsonValue *items = xlsp_json_new_array();
                    
                    // Walk inheritance chain to get all fields and methods
                    char detail_buf[512];
                    for (XrClassInfo *c = cls_info; c != NULL; c = c->base) {
                        // Add fields with type info
                        for (int i = 0; i < c->field_count; i++) {
                            XaSymbol *f = c->fields[i];
                            if (f && f->name && !f->is_private) {
                                XaSymbolLinks *links = xa_analyzer_get_links(analyzer, f);
                                const char *type_str = (links && links->type) 
                                    ? xr_type_to_string(links->type) : "unknown";
                                snprintf(detail_buf, sizeof(detail_buf), "%s.%s: %s", 
                                         class_name, f->name, type_str);
                                XrJsonValue *item = make_completion_item(f->name, 5, detail_buf);
                                xlsp_json_object_set(item, "sortText", xlsp_json_new_string("0"));
                                xlsp_json_array_push(items, item);
                            }
                        }
                        // Add methods with signature
                        for (int i = 0; i < c->method_count; i++) {
                            XaSymbol *m = c->methods[i];
                            if (m && m->name && !m->is_private && strcmp(m->name, XR_KEYWORD_CONSTRUCTOR) != 0) {
                                XaSymbolLinks *links = xa_analyzer_get_links(analyzer, m);
                                int sig_len = snprintf(detail_buf, sizeof(detail_buf), "%s.%s(", class_name, m->name);
                                
                                // Add parameters
                                if (links && links->param_count > 0) {
                                    for (int p = 0; p < links->param_count; p++) {
                                        if (p > 0) sig_len += snprintf(detail_buf + sig_len, sizeof(detail_buf) - sig_len, ", ");
                                        const char *pname = (links->param_names && links->param_names[p]) 
                                            ? links->param_names[p] : "_";
                                        const char *ptype = (links->param_types && links->param_types[p])
                                            ? xr_type_to_string(links->param_types[p]) : "unknown";
                                        sig_len += snprintf(detail_buf + sig_len, sizeof(detail_buf) - sig_len, "%s: %s", pname, ptype);
                                    }
                                }
                                
                                // Add return type
                                const char *ret_type = (links && links->return_type) 
                                    ? xr_type_to_string(links->return_type) : "unknown";
                                snprintf(detail_buf + sig_len, sizeof(detail_buf) - sig_len, "): %s", ret_type);
                                
                                XrJsonValue *item = make_completion_item(m->name, 2, detail_buf);
                                xlsp_json_object_set(item, "sortText", xlsp_json_new_string("1"));
                                xlsp_json_array_push(items, item);
                            }
                        }
                    }
                    if (xlsp_json_array_len(items) > 0) return items;
                    xlsp_json_free(items);
                }
            }
        }
        
        // XaAnalyzer type inference (covers for-in iteration variables, typed declarations, etc.)
        if (analyzer) {
            XaSymbol *sym = xa_analyzer_lookup_deep(analyzer, prefix);
            if (sym) {
                XrType *type = xa_analyzer_get_type(analyzer, sym);
                if (type) {
                    if (XR_TYPE_IS_ENUM(type)) {
                        const char *ename = type->enum_type.enum_name;
                        return make_enum_value_completions(ename ? ename : "Enum");
                    }
                    // Class instance: get members directly from analyzer (no text scanning)
                    if (XR_TYPE_IS_INSTANCE(type)) {
                        int member_count = 0;
                        XaSymbol **members = xa_analyzer_get_members(analyzer, type, &member_count);
                        if (members && member_count > 0) {
                            XrJsonValue *items = xlsp_json_new_array();
                            char detail_buf[512];
                            const char *cls_name = type->instance.class_name 
                                ? type->instance.class_name : "class";
                            for (int i = 0; i < member_count; i++) {
                                XaSymbol *m = members[i];
                                if (!m || !m->name || m->is_private) continue;
                                if (m->kind == XA_SYM_METHOD && 
                                    strcmp(m->name, XR_KEYWORD_CONSTRUCTOR) == 0) continue;
                                XaSymbolLinks *links = xa_analyzer_get_links(analyzer, m);
                                if (m->kind == XA_SYM_METHOD) {
                                    int sl = snprintf(detail_buf, sizeof(detail_buf), 
                                                     "%s.%s(", cls_name, m->name);
                                    if (links && links->param_count > 0) {
                                        for (int p = 0; p < links->param_count; p++) {
                                            if (p > 0) sl += snprintf(detail_buf + sl, 
                                                sizeof(detail_buf) - sl, ", ");
                                            const char *pn = (links->param_names && links->param_names[p]) 
                                                ? links->param_names[p] : "_";
                                            const char *pt = (links->param_types && links->param_types[p])
                                                ? xr_type_to_string(links->param_types[p]) : "unknown";
                                            sl += snprintf(detail_buf + sl, 
                                                sizeof(detail_buf) - sl, "%s: %s", pn, pt);
                                        }
                                    }
                                    const char *rt = (links && links->return_type) 
                                        ? xr_type_to_string(links->return_type) : "unknown";
                                    snprintf(detail_buf + sl, sizeof(detail_buf) - sl, "): %s", rt);
                                    xlsp_json_array_push(items, make_completion_item(m->name, 2, detail_buf));
                                } else {
                                    const char *ts = (links && links->type) 
                                        ? xr_type_to_string(links->type) : "unknown";
                                    snprintf(detail_buf, sizeof(detail_buf), "%s.%s: %s", 
                                             cls_name, m->name, ts);
                                    xlsp_json_array_push(items, make_completion_item(m->name, 5, detail_buf));
                                }
                            }
                            free(members);
                            if (xlsp_json_array_len(items) > 0) return items;
                            xlsp_json_free(items);
                        }
                        if (members) free(members);
                    }
                    XlspBuiltinType bt = XLSP_TYPE_UNKNOWN;
                    if (XR_TYPE_IS_STRING(type)) bt = XLSP_TYPE_STRING;
                    else if (XR_TYPE_IS_ARRAY(type)) bt = XLSP_TYPE_ARRAY;
                    else if (XR_TYPE_IS_MAP(type)) bt = XLSP_TYPE_MAP;
                    else if (type->kind == XR_KIND_SET) bt = XLSP_TYPE_SET;
                    else if (type->kind == XR_KIND_CHANNEL) bt = XLSP_TYPE_CHANNEL;
                    else if (XR_TYPE_IS_INT(type)) bt = XLSP_TYPE_INT;
                    else if (XR_TYPE_IS_FLOAT(type)) bt = XLSP_TYPE_FLOAT;
                    else if (XR_TYPE_IS_BOOL(type)) bt = XLSP_TYPE_BOOL;
                    else if (XR_TYPE_IS_JSON(type)) bt = XLSP_TYPE_JSON;
                    else if (xr_type_is_named_class(type, "BigInt")) bt = XLSP_TYPE_BIGINT;
                    else if (xr_type_is_named_class(type, "StringBuilder")) bt = XLSP_TYPE_STRINGBUILDER;
                    else if (xr_type_is_named_class(type, "Regex")) bt = XLSP_TYPE_REGEX;
                    else if (xr_type_is_named_class(type, "Exception")) bt = XLSP_TYPE_EXCEPTION;
                    else if (xr_type_is_named_class(type, "Task")) bt = XLSP_TYPE_COROUTINE;
                    if (bt != XLSP_TYPE_UNKNOWN) return xlsp_builtin_get_completions(bt);
                }
            }
        }
        
        // Enum value instance completion: Color.Red. -> name, value, ordinal, toString
        {
            const char *enum_name = find_enum_for_member_prefix(doc, pos, prefix);
            if (enum_name) return make_enum_value_completions(enum_name);
        }
        
        // Variable assigned from enum member: let c = Color.Green; c. -> name, value, ordinal, toString
        if (doc->content && doc->ast) {
            size_t prefix_len = strlen(prefix);
            char let_pat[128], const_pat[128];
            snprintf(let_pat, sizeof(let_pat), "let %s", prefix);
            snprintf(const_pat, sizeof(const_pat), "const %s", prefix);
            
            const char *p = doc->content;
            while (p && *p) {
                const char *lm = strstr(p, let_pat);
                const char *cm = strstr(p, const_pat);
                const char *match = NULL;
                int skip = 0;
                if (lm && (!cm || lm < cm)) { match = lm; skip = 4 + prefix_len; }
                else if (cm) { match = cm; skip = 6 + prefix_len; }
                if (!match) break;
                
                if (match > doc->content) {
                    char prev = match[-1];
                    if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
                        (prev >= '0' && prev <= '9') || prev == '_') { p = match + 1; continue; }
                }
                const char *after = match + skip;
                if ((*after >= 'a' && *after <= 'z') || (*after >= 'A' && *after <= 'Z') ||
                    (*after >= '0' && *after <= '9') || *after == '_') { p = match + 1; continue; }
                
                while (*after == ' ' || *after == '\t') after++;
                if (*after == '=' && after[1] != '=') {
                    after++;
                    while (*after == ' ' || *after == '\t') after++;
                    // Check for EnumName.Member pattern
                    const char *name_start = after;
                    while ((*after >= 'a' && *after <= 'z') || (*after >= 'A' && *after <= 'Z') ||
                           (*after >= '0' && *after <= '9') || *after == '_') after++;
                    if (*after == '.' && after > name_start) {
                        char candidate[64];
                        size_t clen = after - name_start;
                        if (clen < sizeof(candidate)) {
                            memcpy(candidate, name_start, clen);
                            candidate[clen] = '\0';
                            if (find_enum_in_ast(doc->ast, candidate)) {
                                return make_enum_value_completions(candidate);
                            }
                        }
                    }
                    break;
                }
                p = match + 1;
            }
        }
        
        // If we have a prefix (member access like "car.") but couldn't resolve type,
        // return empty list instead of falling back to global completions
        return xlsp_json_new_array();
    }
    
    return complete_basic(server, doc, pos);
}
