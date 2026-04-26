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

static const char *STDLIB_MODULES[] = {"time",    "json",   "http",  "log",    "os",  "fs",
                                       "math",    "crypto", "regex", "base64", "url", "path",
                                       "strings", "bytes",  "fmt",   NULL};

/*
 * Find the declaration of `name` as a `let NAME = ...` or `const NAME = ...`
 * (not `shared` prefixed) inside `content`. On success returns true and
 * fills the 0-indexed line plus the column range covering just the
 * `let`/`const` keyword so the caller can rewrite it.
 */
static bool find_plain_decl_keyword(const char *content, const char *name, int *out_line,
                                    int *out_start_col, int *out_end_col) {
    if (!content || !name || !*name)
        return false;
    size_t name_len = strlen(name);
    int line = 0;
    const char *p = content;
    while (*p) {
        const char *line_start = p;
        const char *t = p;
        while (*t == ' ' || *t == '\t')
            t++;
        int col = (int) (t - line_start);
        const char *kw_end = NULL;
        int kw_len = 0;
        if (strncmp(t, "let ", 4) == 0) {
            kw_len = 3;
            kw_end = t + 3;
        } else if (strncmp(t, "const ", 6) == 0) {
            kw_len = 5;
            kw_end = t + 5;
        }
        if (kw_end) {
            const char *n = kw_end;
            while (*n == ' ' || *n == '\t')
                n++;
            if (strncmp(n, name, name_len) == 0) {
                char after = n[name_len];
                if (after == ' ' || after == '\t' || after == ':' || after == '=') {
                    if (out_line)
                        *out_line = line;
                    if (out_start_col)
                        *out_start_col = col;
                    if (out_end_col)
                        *out_end_col = col + kw_len;
                    return true;
                }
            }
        }
        while (*p && *p != '\n')
            p++;
        if (*p == '\n') {
            p++;
            line++;
        }
    }
    return false;
}

/*
 * Extract the first single-quoted identifier from `msg` after the phrase
 * `after`. Writes up to buf_size-1 chars of the name into `buf`. Returns
 * true if a non-empty name was captured.
 */
static bool extract_quoted_name_after(const char *msg, const char *after, char *buf,
                                      size_t buf_size) {
    if (!msg || !after || !buf || buf_size < 2)
        return false;
    const char *marker = strstr(msg, after);
    if (!marker)
        return false;
    const char *start = marker + strlen(after);
    const char *end = strchr(start, '\'');
    if (!end || end == start)
        return false;
    size_t n = (size_t) (end - start);
    if (n >= buf_size)
        n = buf_size - 1;
    memcpy(buf, start, n);
    buf[n] = '\0';
    return n > 0;
}

/*
 * Emit a quick-fix action that replaces the `let`/`const` keyword of a
 * variable declaration with `new_prefix`. No-op if the declaration
 * cannot be located in the document text.
 */
static void push_decl_rewrite_action(XrJsonValue *actions, const char *uri, const char *content,
                                     const char *var_name, const char *new_prefix,
                                     const char *title) {
    int dline = 0, dsc = 0, dec = 0;
    if (!find_plain_decl_keyword(content, var_name, &dline, &dsc, &dec))
        return;

    XrJsonValue *action = xjson_new_object();
    xjson_object_set(action, "title", xjson_new_string(title));
    xjson_object_set(action, "kind", xjson_new_string("quickfix"));

    XrJsonValue *edit = xjson_new_object();
    XrJsonValue *changes = xjson_new_object();
    XrJsonValue *edits = xjson_new_array();

    XrJsonValue *text_edit = xjson_new_object();
    xjson_object_set(text_edit, "newText", xjson_new_string(new_prefix));
    xjson_object_set(text_edit, "range", xjson_make_range(dline, dsc, dline, dec));

    xjson_array_push(edits, text_edit);
    xjson_object_set(changes, uri, edits);
    xjson_object_set(edit, "changes", changes);
    xjson_object_set(action, "edit", edit);
    xjson_array_push(actions, action);
}

XrJsonValue *xlsp_handle_code_action(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *context = xjson_get_object(params, "context");
    XrJsonValue *range_obj = xjson_get_object(params, "range");
    if (!textDocument)
        return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->content)
        return xjson_new_array();

    XrJsonValue *actions = xjson_new_array();

    // Get selection range
    int sel_start_line = 0, sel_start_char = 0, sel_end_line = 0, sel_end_char = 0;
    bool has_selection = false;
    if (range_obj) {
        XrJsonValue *start = xjson_get_object(range_obj, "start");
        XrJsonValue *end = xjson_get_object(range_obj, "end");
        if (start && end) {
            sel_start_line = xjson_get_int(start, "line");
            sel_start_char = xjson_get_int(start, "character");
            sel_end_line = xjson_get_int(end, "line");
            sel_end_char = xjson_get_int(end, "character");
            has_selection = (sel_start_line != sel_end_line || sel_start_char != sel_end_char);
        }
    }

    // Check for diagnostics with undefined symbols or unused variables
    XrJsonValue *diagnostics = xjson_get(context, "diagnostics");
    if (diagnostics && diagnostics->type == XR_JSON_ARRAY) {
        for (int i = 0; i < xjson_array_len(diagnostics); i++) {
            XrJsonValue *diag = xjson_array_get(diagnostics, i);
            const char *msg = xjson_get_string(diag, "message");

            // QuickFix: Remove unused variable
            if (msg && (strstr(msg, "unused") || strstr(msg, "never used"))) {
                XrJsonValue *diag_range = xjson_get_object(diag, "range");
                if (diag_range) {
                    XrJsonValue *diag_start = xjson_get_object(diag_range, "start");
                    int diag_line = xjson_get_int(diag_start, "line");

                    XrJsonValue *action = xjson_new_object();
                    xjson_object_set(action, "title", xjson_new_string("Remove unused variable"));
                    xjson_object_set(action, "kind", xjson_new_string("quickfix"));

                    XrJsonValue *edit = xjson_new_object();
                    XrJsonValue *changes = xjson_new_object();
                    XrJsonValue *edits = xjson_new_array();

                    XrJsonValue *text_edit = xjson_new_object();
                    xjson_object_set(text_edit, "newText", xjson_new_string(""));
                    xjson_object_set(text_edit, "range",
                                     xjson_make_range(diag_line, 0, diag_line + 1, 0));

                    xjson_array_push(edits, text_edit);
                    xjson_object_set(changes, uri, edits);
                    xjson_object_set(edit, "changes", changes);
                    xjson_object_set(action, "edit", edit);

                    xjson_array_push(actions, action);
                }
            }

            // QuickFix: Auto import
            if (msg && strstr(msg, "undefined")) {
                for (int m = 0; STDLIB_MODULES[m]; m++) {
                    if (strstr(msg, STDLIB_MODULES[m])) {
                        char title[128];
                        snprintf(title, sizeof(title), "Import '%s' module", STDLIB_MODULES[m]);

                        XrJsonValue *action = xjson_new_object();
                        xjson_object_set(action, "title", xjson_new_string(title));
                        xjson_object_set(action, "kind", xjson_new_string("quickfix"));

                        XrJsonValue *edit = xjson_new_object();
                        XrJsonValue *changes = xjson_new_object();
                        XrJsonValue *edits = xjson_new_array();

                        XrJsonValue *text_edit = xjson_new_object();
                        char import_text[64];
                        snprintf(import_text, sizeof(import_text), "import %s\n",
                                 STDLIB_MODULES[m]);
                        xjson_object_set(text_edit, "newText", xjson_new_string(import_text));
                        xjson_object_set(text_edit, "range", xjson_make_range(0, 0, 0, 0));

                        xjson_array_push(edits, text_edit);
                        xjson_object_set(changes, uri, edits);
                        xjson_object_set(edit, "changes", changes);
                        xjson_object_set(action, "edit", edit);

                        xjson_array_push(actions, action);
                        break;
                    }
                }
            }

            // QuickFix: E0363 go closure captures mutable variable -> shared const
            // Matches: "go closure cannot capture mutable variable 'NAME'"
            // (skips the 'shared let' sub-variant on purpose; that one needs
            // call-site `move` which is not a simple decl rewrite.)
            if (msg && strstr(msg, "go closure cannot capture mutable variable '")) {
                char var_name[128];
                if (extract_quoted_name_after(msg, "go closure cannot capture mutable variable '",
                                              var_name, sizeof(var_name))) {
                    char title[192];
                    snprintf(title, sizeof(title),
                             "Declare '%s' as 'shared const' (allow concurrent reads)", var_name);
                    push_decl_rewrite_action(actions, uri, doc->content, var_name, "shared const",
                                             title);
                }
            }

            // QuickFix: E0363 move requires shared let -> insert 'shared'
            // Matches: "'move' requires 'NAME' to be declared as 'shared let'"
            if (msg && strstr(msg, "'move' requires '")) {
                char var_name[128];
                if (extract_quoted_name_after(msg, "'move' requires '", var_name,
                                              sizeof(var_name))) {
                    char title[192];
                    snprintf(title, sizeof(title),
                             "Declare '%s' as 'shared let' (allow ownership transfer)", var_name);
                    push_decl_rewrite_action(actions, uri, doc->content, var_name, "shared let",
                                             title);
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
            if (*cur_line_start == '\n')
                cl++;
            cur_line_start++;
        }
        const char *trimmed = cur_line_start;
        while (*trimmed == ' ' || *trimmed == '\t')
            trimmed++;

        if (strncmp(trimmed, "let ", 4) == 0) {
            // Extract variable name
            const char *name_start = trimmed + 4;
            while (*name_start == ' ' || *name_start == '\t')
                name_start++;
            const char *name_end = name_start;
            while ((*name_end >= 'a' && *name_end <= 'z') ||
                   (*name_end >= 'A' && *name_end <= 'Z') ||
                   (*name_end >= '0' && *name_end <= '9') || *name_end == '_')
                name_end++;

            if (name_end > name_start) {
                size_t name_len = name_end - name_start;
                char var_name[128];
                if (name_len < sizeof(var_name)) {
                    memcpy(var_name, name_start, name_len);
                    var_name[name_len] = '\0';

                    // Check if variable is reassigned (simple: look for "var_name =" not "var_name
                    // ==")
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
                        int let_col = (int) (trimmed - cur_line_start);

                        XrJsonValue *action = xjson_new_object();
                        xjson_object_set(action, "title",
                                         xjson_new_string("Convert 'let' to 'const'"));
                        xjson_object_set(action, "kind", xjson_new_string("refactor.rewrite"));

                        XrJsonValue *edit = xjson_new_object();
                        XrJsonValue *changes = xjson_new_object();
                        XrJsonValue *edits_arr = xjson_new_array();

                        XrJsonValue *text_edit = xjson_new_object();
                        xjson_object_set(text_edit, "newText", xjson_new_string("const"));
                        xjson_object_set(
                            text_edit, "range",
                            xjson_make_range(sel_start_line, let_col, sel_start_line, let_col + 3));

                        xjson_array_push(edits_arr, text_edit);
                        xjson_object_set(changes, uri, edits_arr);
                        xjson_object_set(edit, "changes", changes);
                        xjson_object_set(action, "edit", edit);

                        xjson_array_push(actions, action);
                    }
                }
            }
        }
    }

    // Refactor: Extract Variable (only if there's a selection)
    if (has_selection) {
        XrLspPosition start_pos = {.line = sel_start_line, .character = sel_start_char};
        XrLspPosition end_pos = {.line = sel_end_line, .character = sel_end_char};
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
                    XrJsonValue *action = xjson_new_object();
                    xjson_object_set(action, "title", xjson_new_string("Extract to variable"));
                    xjson_object_set(action, "kind", xjson_new_string("refactor.extract"));

                    XrJsonValue *edit = xjson_new_object();
                    XrJsonValue *changes = xjson_new_object();
                    XrJsonValue *edits = xjson_new_array();

                    XrJsonValue *decl_edit = xjson_new_object();
                    char decl_text[256];
                    snprintf(decl_text, sizeof(decl_text), "let extracted = %s\n", selected);
                    xjson_object_set(decl_edit, "newText", xjson_new_string(decl_text));
                    xjson_object_set(decl_edit, "range",
                                     xjson_make_range(sel_start_line, 0, sel_start_line, 0));
                    xjson_array_push(edits, decl_edit);

                    XrJsonValue *repl_edit = xjson_new_object();
                    xjson_object_set(repl_edit, "newText", xjson_new_string("extracted"));
                    xjson_object_set(repl_edit, "range",
                                     xjson_make_range(sel_start_line + 1, sel_start_char,
                                                      sel_end_line + 1, sel_end_char));
                    xjson_array_push(edits, repl_edit);

                    xjson_object_set(changes, uri, edits);
                    xjson_object_set(edit, "changes", changes);
                    xjson_object_set(action, "edit", edit);

                    xjson_array_push(actions, action);
                }
                xr_free(selected);
            }
        }
    }

    // Refactor: Extract Function (multi-line selection with statements)
    if (has_selection && sel_end_line > sel_start_line) {
        XrLspPosition start_pos = {.line = sel_start_line, .character = sel_start_char};
        XrLspPosition end_pos = {.line = sel_end_line, .character = sel_end_char};
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
                    if (selected[i] == '\n' || selected[i] == ';') {
                        has_stmt = true;
                        break;
                    }
                }

                if (has_stmt) {
                    // Find indent of first selected line
                    const char *first_line = doc->content;
                    int fl = 0;
                    while (fl < sel_start_line && *first_line) {
                        if (*first_line == '\n')
                            fl++;
                        first_line++;
                    }
                    int indent = 0;
                    while (first_line[indent] == ' ' || first_line[indent] == '\t')
                        indent++;

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
                        if (*lp == '\n')
                            lp++;
                    }
                    pos += snprintf(func_def + pos, sel_len + 256 - pos, "}\n");
                    func_def[pos] = '\0';

                    // Build indent string for call site
                    char indent_str[128];
                    int ni = indent < (int) sizeof(indent_str) - 1 ? indent
                                                                   : (int) sizeof(indent_str) - 1;
                    memset(indent_str, ' ', ni);
                    indent_str[ni] = '\0';

                    char call_text[256];
                    snprintf(call_text, sizeof(call_text), "%sextracted()\n", indent_str);

                    XrJsonValue *action = xjson_new_object();
                    xjson_object_set(action, "title", xjson_new_string("Extract to function"));
                    xjson_object_set(action, "kind", xjson_new_string("refactor.extract"));

                    XrJsonValue *edit = xjson_new_object();
                    XrJsonValue *changes = xjson_new_object();
                    XrJsonValue *edits_arr = xjson_new_array();

                    // Replace selection with function call
                    XrJsonValue *repl = xjson_new_object();
                    xjson_object_set(repl, "newText", xjson_new_string(call_text));
                    xjson_object_set(
                        repl, "range",
                        xjson_make_range(sel_start_line, 0, sel_end_line, sel_end_char));
                    xjson_array_push(edits_arr, repl);

                    // Insert function definition at end of file
                    int last_line = 0;
                    const char *cp = doc->content;
                    while (*cp) {
                        if (*cp == '\n')
                            last_line++;
                        cp++;
                    }

                    XrJsonValue *ins = xjson_new_object();
                    xjson_object_set(ins, "newText", xjson_new_string(func_def));
                    xjson_object_set(ins, "range", xjson_make_range(last_line, 0, last_line, 0));
                    xjson_array_push(edits_arr, ins);

                    xjson_object_set(changes, uri, edits_arr);
                    xjson_object_set(edit, "changes", changes);
                    xjson_object_set(action, "edit", edit);

                    xjson_array_push(actions, action);

                    xr_free(func_def);
                }
                xr_free(selected);
            }
        }
    }

    // Always offer "Organize Imports" action
    XrJsonValue *organize = xjson_new_object();
    xjson_object_set(organize, "title", xjson_new_string("Organize Imports"));
    xjson_object_set(organize, "kind", xjson_new_string("source.organizeImports"));
    xjson_array_push(actions, organize);

    return actions;
}
