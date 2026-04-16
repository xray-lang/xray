/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_code_action.c - Code action support (quickfix, refactor)
 */

#include "xlsp_code_action.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../../base/xmalloc.h"

static const char *STDLIB_MODULES[] = {
    "time", "json", "http", "log", "os", "fs", "math", "crypto",
    "regex", "base64", "url", "path", "strings", "bytes", "fmt", NULL
};

XrJsonValue *xlsp_handle_code_action(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xlsp_json_get_object(params, "textDocument");
    XrJsonValue *context = xlsp_json_get_object(params, "context");
    XrJsonValue *range_obj = xlsp_json_get_object(params, "range");
    if (!textDocument) return xlsp_json_new_array();
    
    const char *uri = xlsp_json_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->content) return xlsp_json_new_array();
    
    XrJsonValue *actions = xlsp_json_new_array();
    
    // Get selection range
    int sel_start_line = 0, sel_start_char = 0, sel_end_line = 0, sel_end_char = 0;
    bool has_selection = false;
    if (range_obj) {
        XrJsonValue *start = xlsp_json_get_object(range_obj, "start");
        XrJsonValue *end = xlsp_json_get_object(range_obj, "end");
        if (start && end) {
            sel_start_line = xlsp_json_get_int(start, "line");
            sel_start_char = xlsp_json_get_int(start, "character");
            sel_end_line = xlsp_json_get_int(end, "line");
            sel_end_char = xlsp_json_get_int(end, "character");
            has_selection = (sel_start_line != sel_end_line || sel_start_char != sel_end_char);
        }
    }
    
    // Check for diagnostics with undefined symbols or unused variables
    XrJsonValue *diagnostics = xlsp_json_get(context, "diagnostics");
    if (diagnostics && diagnostics->type == XR_JSON_ARRAY) {
        for (int i = 0; i < xlsp_json_array_len(diagnostics); i++) {
            XrJsonValue *diag = xlsp_json_array_get(diagnostics, i);
            const char *msg = xlsp_json_get_string(diag, "message");
            
            // QuickFix: Remove unused variable
            if (msg && (strstr(msg, "unused") || strstr(msg, "never used"))) {
                XrJsonValue *diag_range = xlsp_json_get_object(diag, "range");
                if (diag_range) {
                    XrJsonValue *diag_start = xlsp_json_get_object(diag_range, "start");
                    int diag_line = xlsp_json_get_int(diag_start, "line");
                    
                    XrJsonValue *action = xlsp_json_new_object();
                    xlsp_json_object_set(action, "title", xlsp_json_new_string("Remove unused variable"));
                    xlsp_json_object_set(action, "kind", xlsp_json_new_string("quickfix"));
                    
                    XrJsonValue *edit = xlsp_json_new_object();
                    XrJsonValue *changes = xlsp_json_new_object();
                    XrJsonValue *edits = xlsp_json_new_array();
                    
                    XrJsonValue *text_edit = xlsp_json_new_object();
                    xlsp_json_object_set(text_edit, "newText", xlsp_json_new_string(""));
                    xlsp_json_object_set(text_edit, "range", 
                        xlsp_json_make_range(diag_line, 0, diag_line + 1, 0));
                    
                    xlsp_json_array_push(edits, text_edit);
                    xlsp_json_object_set(changes, uri, edits);
                    xlsp_json_object_set(edit, "changes", changes);
                    xlsp_json_object_set(action, "edit", edit);
                    
                    xlsp_json_array_push(actions, action);
                }
            }
            
            // QuickFix: Auto import
            if (msg && strstr(msg, "undefined")) {
                for (int m = 0; STDLIB_MODULES[m]; m++) {
                    if (strstr(msg, STDLIB_MODULES[m])) {
                        char title[128];
                        snprintf(title, sizeof(title), "Import '%s' module", STDLIB_MODULES[m]);
                        
                        XrJsonValue *action = xlsp_json_new_object();
                        xlsp_json_object_set(action, "title", xlsp_json_new_string(title));
                        xlsp_json_object_set(action, "kind", xlsp_json_new_string("quickfix"));
                        
                        XrJsonValue *edit = xlsp_json_new_object();
                        XrJsonValue *changes = xlsp_json_new_object();
                        XrJsonValue *edits = xlsp_json_new_array();
                        
                        XrJsonValue *text_edit = xlsp_json_new_object();
                        char import_text[64];
                        snprintf(import_text, sizeof(import_text), "import %s\n", STDLIB_MODULES[m]);
                        xlsp_json_object_set(text_edit, "newText", xlsp_json_new_string(import_text));
                        xlsp_json_object_set(text_edit, "range", xlsp_json_make_range(0, 0, 0, 0));
                        
                        xlsp_json_array_push(edits, text_edit);
                        xlsp_json_object_set(changes, uri, edits);
                        xlsp_json_object_set(edit, "changes", changes);
                        xlsp_json_object_set(action, "edit", edit);
                        
                        xlsp_json_array_push(actions, action);
                        break;
                    }
                }
            }
        }
    }
    
    // Refactor: Convert let → const (when variable is never reassigned)
    if (doc->content && doc->ast) {
        // Find if cursor is on a "let" declaration line
        const char *cur_line_start = doc->content;
        int cl = 0;
        while (cl < sel_start_line && *cur_line_start) {
            if (*cur_line_start == '\n') cl++;
            cur_line_start++;
        }
        const char *trimmed = cur_line_start;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        
        if (strncmp(trimmed, "let ", 4) == 0) {
            // Extract variable name
            const char *name_start = trimmed + 4;
            while (*name_start == ' ' || *name_start == '\t') name_start++;
            const char *name_end = name_start;
            while ((*name_end >= 'a' && *name_end <= 'z') || (*name_end >= 'A' && *name_end <= 'Z') ||
                   (*name_end >= '0' && *name_end <= '9') || *name_end == '_') name_end++;
            
            if (name_end > name_start) {
                size_t name_len = name_end - name_start;
                char var_name[128];
                if (name_len < sizeof(var_name)) {
                    memcpy(var_name, name_start, name_len);
                    var_name[name_len] = '\0';
                    
                    // Check if variable is reassigned (simple: look for "var_name =" not "var_name ==")
                    char assign_pat[136];
                    snprintf(assign_pat, sizeof(assign_pat), "%s =", var_name);
                    const char *sp = doc->content;
                    int occurrences = 0;
                    while ((sp = strstr(sp, assign_pat)) != NULL) {
                        // Skip the declaration itself and == comparisons
                        const char *after_eq = sp + strlen(assign_pat);
                        if (*after_eq != '=' && sp != name_start) {
                            occurrences++;
                        }
                        sp++;
                    }
                    // occurrences > 1 means reassignment (1 = declaration itself)
                    if (occurrences <= 1) {
                        int let_col = (int)(trimmed - cur_line_start);
                        
                        XrJsonValue *action = xlsp_json_new_object();
                        xlsp_json_object_set(action, "title", 
                            xlsp_json_new_string("Convert 'let' to 'const'"));
                        xlsp_json_object_set(action, "kind", xlsp_json_new_string("refactor.rewrite"));
                        
                        XrJsonValue *edit = xlsp_json_new_object();
                        XrJsonValue *changes = xlsp_json_new_object();
                        XrJsonValue *edits_arr = xlsp_json_new_array();
                        
                        XrJsonValue *text_edit = xlsp_json_new_object();
                        xlsp_json_object_set(text_edit, "newText", xlsp_json_new_string("const"));
                        xlsp_json_object_set(text_edit, "range",
                            xlsp_json_make_range(sel_start_line, let_col, sel_start_line, let_col + 3));
                        
                        xlsp_json_array_push(edits_arr, text_edit);
                        xlsp_json_object_set(changes, uri, edits_arr);
                        xlsp_json_object_set(edit, "changes", changes);
                        xlsp_json_object_set(action, "edit", edit);
                        
                        xlsp_json_array_push(actions, action);
                    }
                }
            }
        }
    }
    
    // Refactor: Extract Variable (only if there's a selection)
    if (has_selection) {
        XrLspPosition start_pos = { .line = sel_start_line, .character = sel_start_char };
        XrLspPosition end_pos = { .line = sel_end_line, .character = sel_end_char };
        uint32_t start_offset = xlsp_position_to_offset(doc, start_pos);
        uint32_t end_offset = xlsp_position_to_offset(doc, end_pos);
        
        if (start_offset < end_offset && end_offset <= doc->length) {
            size_t sel_len = end_offset - start_offset;
            if (sel_len > 0 && sel_len < 200) {
                char *selected = xr_malloc(sel_len + 1);
                memcpy(selected, doc->content + start_offset, sel_len);
                selected[sel_len] = '\0';
                
                bool looks_like_expr = true;
                for (size_t i = 0; i < sel_len && looks_like_expr; i++) {
                    if (selected[i] == ';' || selected[i] == '{' || selected[i] == '}') {
                        looks_like_expr = false;
                    }
                }
                
                if (looks_like_expr && sel_len > 1) {
                    XrJsonValue *action = xlsp_json_new_object();
                    xlsp_json_object_set(action, "title", xlsp_json_new_string("Extract to variable"));
                    xlsp_json_object_set(action, "kind", xlsp_json_new_string("refactor.extract"));
                    
                    XrJsonValue *edit = xlsp_json_new_object();
                    XrJsonValue *changes = xlsp_json_new_object();
                    XrJsonValue *edits = xlsp_json_new_array();
                    
                    XrJsonValue *decl_edit = xlsp_json_new_object();
                    char decl_text[256];
                    snprintf(decl_text, sizeof(decl_text), "let extracted = %s\n", selected);
                    xlsp_json_object_set(decl_edit, "newText", xlsp_json_new_string(decl_text));
                    xlsp_json_object_set(decl_edit, "range",
                        xlsp_json_make_range(sel_start_line, 0, sel_start_line, 0));
                    xlsp_json_array_push(edits, decl_edit);
                    
                    XrJsonValue *repl_edit = xlsp_json_new_object();
                    xlsp_json_object_set(repl_edit, "newText", xlsp_json_new_string("extracted"));
                    xlsp_json_object_set(repl_edit, "range",
                        xlsp_json_make_range(sel_start_line + 1, sel_start_char, 
                                             sel_end_line + 1, sel_end_char));
                    xlsp_json_array_push(edits, repl_edit);
                    
                    xlsp_json_object_set(changes, uri, edits);
                    xlsp_json_object_set(edit, "changes", changes);
                    xlsp_json_object_set(action, "edit", edit);
                    
                    xlsp_json_array_push(actions, action);
                }
                xr_free(selected);
            }
        }
    }
    
    // Refactor: Extract Function (multi-line selection with statements)
    if (has_selection && sel_end_line > sel_start_line) {
        XrLspPosition start_pos = { .line = sel_start_line, .character = sel_start_char };
        XrLspPosition end_pos = { .line = sel_end_line, .character = sel_end_char };
        uint32_t s_off = xlsp_position_to_offset(doc, start_pos);
        uint32_t e_off = xlsp_position_to_offset(doc, end_pos);
        
        if (s_off < e_off && e_off <= doc->length) {
            size_t sel_len = e_off - s_off;
            if (sel_len > 1 && sel_len < 4096) {
                char *selected = xr_malloc(sel_len + 1);
                memcpy(selected, doc->content + s_off, sel_len);
                selected[sel_len] = '\0';
                
                // Only offer if selection looks like statements (contains newline or ;)
                bool has_stmt = false;
                for (size_t i = 0; i < sel_len; i++) {
                    if (selected[i] == '\n' || selected[i] == ';') { has_stmt = true; break; }
                }
                
                if (has_stmt) {
                    // Find indent of first selected line
                    const char *first_line = doc->content;
                    int fl = 0;
                    while (fl < sel_start_line && *first_line) {
                        if (*first_line == '\n') fl++;
                        first_line++;
                    }
                    int indent = 0;
                    while (first_line[indent] == ' ' || first_line[indent] == '\t') indent++;
                    
                    // Build function definition
                    char *func_def = xr_malloc(sel_len + 256);
                    int pos = 0;
                    pos += snprintf(func_def + pos, sel_len + 256 - pos, "\nfn extracted() {\n");
                    // Add body with extra indent
                    const char *lp = selected;
                    while (*lp) {
                        pos += snprintf(func_def + pos, sel_len + 256 - pos, "    ");
                        while (*lp && *lp != '\n') {
                            func_def[pos++] = *lp++;
                        }
                        func_def[pos++] = '\n';
                        if (*lp == '\n') lp++;
                    }
                    pos += snprintf(func_def + pos, sel_len + 256 - pos, "}\n");
                    func_def[pos] = '\0';
                    
                    // Build indent string for call site
                    char indent_str[128];
                    int ni = indent < (int)sizeof(indent_str) - 1 ? indent : (int)sizeof(indent_str) - 1;
                    memset(indent_str, ' ', ni);
                    indent_str[ni] = '\0';
                    
                    char call_text[256];
                    snprintf(call_text, sizeof(call_text), "%sextracted()\n", indent_str);
                    
                    XrJsonValue *action = xlsp_json_new_object();
                    xlsp_json_object_set(action, "title", xlsp_json_new_string("Extract to function"));
                    xlsp_json_object_set(action, "kind", xlsp_json_new_string("refactor.extract"));
                    
                    XrJsonValue *edit = xlsp_json_new_object();
                    XrJsonValue *changes = xlsp_json_new_object();
                    XrJsonValue *edits_arr = xlsp_json_new_array();
                    
                    // Replace selection with function call
                    XrJsonValue *repl = xlsp_json_new_object();
                    xlsp_json_object_set(repl, "newText", xlsp_json_new_string(call_text));
                    xlsp_json_object_set(repl, "range",
                        xlsp_json_make_range(sel_start_line, 0, sel_end_line, sel_end_char));
                    xlsp_json_array_push(edits_arr, repl);
                    
                    // Insert function definition at end of file
                    int last_line = 0;
                    const char *cp = doc->content;
                    while (*cp) { if (*cp == '\n') last_line++; cp++; }
                    
                    XrJsonValue *ins = xlsp_json_new_object();
                    xlsp_json_object_set(ins, "newText", xlsp_json_new_string(func_def));
                    xlsp_json_object_set(ins, "range",
                        xlsp_json_make_range(last_line, 0, last_line, 0));
                    xlsp_json_array_push(edits_arr, ins);
                    
                    xlsp_json_object_set(changes, uri, edits_arr);
                    xlsp_json_object_set(edit, "changes", changes);
                    xlsp_json_object_set(action, "edit", edit);
                    
                    xlsp_json_array_push(actions, action);
                    
                    xr_free(func_def);
                }
                xr_free(selected);
            }
        }
    }
    
    // Always offer "Organize Imports" action
    XrJsonValue *organize = xlsp_json_new_object();
    xlsp_json_object_set(organize, "title", xlsp_json_new_string("Organize Imports"));
    xlsp_json_object_set(organize, "kind", xlsp_json_new_string("source.organizeImports"));
    xlsp_json_array_push(actions, organize);
    
    return actions;
}
