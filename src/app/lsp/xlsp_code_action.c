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
#include "xlsp_cycle_report.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../../base/xmalloc.h"

static const char *STDLIB_MODULES[] = {"time",    "json",   "http",  "log",    "os",  "fs",
                                       "math",    "crypto", "regex", "base64", "url", "path",
                                       "strings", "bytes",  "fmt",   NULL};

static bool extract_enum_variants_target(const char *message, char *buf, size_t buf_size) {
    if (!message || !buf || buf_size < 2)
        return false;
    const char *start = strstr(message, "use `");
    if (!start)
        return false;
    start += strlen("use `");
    const char *suffix = strstr(start, ".variants`");
    if (!suffix || suffix == start)
        return false;
    size_t len = (size_t) (suffix - start);
    if (len >= buf_size)
        return false;
    for (size_t i = 0; i < len; i++) {
        char ch = start[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '_'))
            return false;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';
    return true;
}

static void push_enum_variants_action(XrJsonValue *actions, const char *uri, const char *content,
                                      int diagnostic_line, const char *enum_name) {
    if (!actions || !uri || !content || diagnostic_line < 0 || !enum_name || !*enum_name)
        return;
    const char *line_start = content;
    for (int line = 0; line < diagnostic_line && *line_start; line++) {
        const char *newline = strchr(line_start, '\n');
        if (!newline)
            return;
        line_start = newline + 1;
    }
    const char *line_end = strchr(line_start, '\n');
    if (!line_end)
        line_end = line_start + strlen(line_start);

    size_t enum_len = strlen(enum_name);
    const char *match = line_start;
    while ((match = strstr(match, enum_name)) != NULL && match < line_end) {
        const char *before = match;
        while (before > line_start && (before[-1] == ' ' || before[-1] == '\t'))
            before--;
        bool after_in = before >= line_start + 2 && before[-2] == 'i' && before[-1] == 'n' &&
                        (before == line_start + 2 || before[-3] == ' ' || before[-3] == '\t' ||
                         before[-3] == '(');
        char after = match + enum_len < line_end ? match[enum_len] : '\0';
        bool target_end = !((after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z') ||
                            (after >= '0' && after <= '9') || after == '_' || after == '.');
        if (after_in && target_end)
            break;
        match += enum_len;
    }
    if (!match || match >= line_end)
        return;

    int column = (int) (match + enum_len - line_start);
    char title[256];
    snprintf(title, sizeof(title), "Iterate '%s.variants' descriptors", enum_name);
    XrJsonValue *action = xjson_new_object();
    xjson_object_set(action, "title", xjson_new_string(title));
    xjson_object_set(action, "kind", xjson_new_string("quickfix"));

    XrJsonValue *edit = xjson_new_object();
    XrJsonValue *changes = xjson_new_object();
    XrJsonValue *edits = xjson_new_array();
    XrJsonValue *text_edit = xjson_new_object();
    xjson_object_set(text_edit, "newText", xjson_new_string(".variants"));
    xjson_object_set(text_edit, "range",
                     xjson_make_range(diagnostic_line, column, diagnostic_line, column));
    xjson_array_push(edits, text_edit);
    xjson_object_set(changes, uri, edits);
    xjson_object_set(edit, "changes", changes);
    xjson_object_set(action, "edit", edit);
    xjson_array_push(actions, action);
}

/* ---------- Reference cycles (task 247 phase H) ----------
 *
 * Two rules from 247 section 2.5 shape what is offered here, and both are
 * absences rather than features:
 *
 *   - no "fix all". Breaking a cycle at the wrong edge does not leak, it
 *     nulls a field that was still being read. A batch operation would apply
 *     that mistake everywhere at once.
 *   - no default. Every candidate edge gets its own action, none is marked
 *     preferred, and each title says what the choice asserts about ownership.
 *
 * The language does not guess which reference is the owning one. The tooling
 * makes the edit cheap once a human has decided. */
static void push_weak_field_action(XrJsonValue *actions, const char *uri, XrJsonValue *diag,
                                   const char *field, const char *owner, const char *target) {
    XrJsonValue *diag_range = xjson_get_object(diag, "range");
    XrJsonValue *diag_start = diag_range ? xjson_get_object(diag_range, "start") : NULL;
    if (!diag_start)
        return;
    int line = (int) xjson_get_int(diag_start, "line");
    int character = (int) xjson_get_int(diag_start, "character");

    char title[256];
    snprintf(title, sizeof(title), "Mark `%s` weak — %s does not own the %s it points at", field,
             owner, target);

    XrJsonValue *action = xjson_new_object();
    xjson_object_set(action, "title", xjson_new_string(title));
    xjson_object_set(action, "kind", xjson_new_string("quickfix"));
    /* Deliberately no "isPreferred": that flag is what an editor uses to pick
     * one on the user's behalf, which is the decision the language refuses to
     * make. */
    XrJsonValue *diag_list = xjson_new_array();
    xjson_array_push(diag_list, xjson_clone(diag));
    xjson_object_set(action, "diagnostics", diag_list);

    XrJsonValue *edit = xjson_new_object();
    XrJsonValue *changes = xjson_new_object();
    XrJsonValue *edits = xjson_new_array();
    XrJsonValue *text_edit = xjson_new_object();
    xjson_object_set(text_edit, "newText", xjson_new_string("weak "));
    /* An insertion at the field name: `weak` is a storage modifier and sits
     * where the declaration starts, ahead of the name. */
    xjson_object_set(text_edit, "range", xjson_make_range(line, character, line, character));
    xjson_array_push(edits, text_edit);
    xjson_object_set(changes, uri, edits);
    xjson_object_set(edit, "changes", changes);
    xjson_object_set(action, "edit", edit);

    xjson_array_push(actions, action);
}

/* Pull `Class.field` and the target class out of the diagnostic the cycle
 * report produced. Reading them back beats threading a side channel through
 * the LSP: the diagnostic is the only thing the client is required to hand
 * back with a codeAction request. */
static bool parse_cycle_diagnostic(const char *msg, char *field, size_t field_size, char *owner,
                                   size_t owner_size, char *target, size_t target_size) {
    const char *open = strchr(msg, '`');
    if (!open)
        return false;
    const char *dot = strchr(open + 1, '.');
    const char *close = dot ? strchr(dot + 1, '`') : NULL;
    if (!dot || !close)
        return false;
    size_t olen = (size_t) (dot - open - 1);
    size_t flen = (size_t) (close - dot - 1);
    if (olen == 0 || olen >= owner_size || flen == 0 || flen >= field_size)
        return false;
    memcpy(owner, open + 1, olen);
    owner[olen] = '\0';
    memcpy(field, dot + 1, flen);
    field[flen] = '\0';

    /* "...does NOT own the <Target> it points at" */
    const char *marker = strstr(close, "does NOT own the ");
    if (!marker)
        return false;
    marker += strlen("does NOT own the ");
    const char *end = strstr(marker, " it points at");
    if (!end || (size_t) (end - marker) >= target_size)
        return false;
    memcpy(target, marker, (size_t) (end - marker));
    target[end - marker] = '\0';
    return true;
}

/* A capture edge (A.6). `weak` is a field modifier and cannot reach one, so
 * the only fix is clearing the field — which is what the analyzer's own hint
 * already spells out. Lift it into an edit rather than making the reader
 * retype it. */
static void push_defer_clear_action(XrJsonValue *actions, const char *uri, const char *content,
                                    XrJsonValue *diag) {
    const char *msg = xjson_get_string(diag, "message");
    const char *hint = msg ? strstr(msg, "defer ") : NULL;
    const char *tail = hint ? strstr(hint, " = null") : NULL;
    if (!hint || !tail)
        return;
    size_t expr_len = (size_t) (tail - (hint + 6));
    if (expr_len == 0 || expr_len >= 128)
        return;
    char expr[128];
    memcpy(expr, hint + 6, expr_len);
    expr[expr_len] = '\0';

    XrJsonValue *diag_range = xjson_get_object(diag, "range");
    XrJsonValue *diag_start = diag_range ? xjson_get_object(diag_range, "start") : NULL;
    if (!diag_start)
        return;
    int line = (int) xjson_get_int(diag_start, "line");

    /* Match the indentation of the assignment that created the cycle: the
     * defer belongs to the same scope, and an edit that reads as hand-written
     * is one the user does not have to clean up after. */
    char indent[64] = {0};
    const char *p = content;
    for (int l = 0; l < line && p; l++) {
        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : NULL;
    }
    size_t ind = 0;
    while (p && (p[ind] == ' ' || p[ind] == '\t') && ind + 1 < sizeof(indent)) {
        indent[ind] = p[ind];
        ind++;
    }
    indent[ind] = '\0';

    char new_text[256];
    snprintf(new_text, sizeof(new_text), "%sdefer { %s = null }\n", indent, expr);

    char title[192];
    snprintf(title, sizeof(title), "Clear the field when the scope ends: defer { %s = null }", expr);

    XrJsonValue *action = xjson_new_object();
    xjson_object_set(action, "title", xjson_new_string(title));
    xjson_object_set(action, "kind", xjson_new_string("quickfix"));
    XrJsonValue *diag_list = xjson_new_array();
    xjson_array_push(diag_list, xjson_clone(diag));
    xjson_object_set(action, "diagnostics", diag_list);

    XrJsonValue *edit = xjson_new_object();
    XrJsonValue *changes = xjson_new_object();
    XrJsonValue *edits = xjson_new_array();
    XrJsonValue *text_edit = xjson_new_object();
    xjson_object_set(text_edit, "newText", xjson_new_string(new_text));
    /* Right after the assignment: a defer runs at scope exit wherever it is
     * registered, and next to the line that created the cycle is where it
     * explains itself. */
    xjson_object_set(text_edit, "range", xjson_make_range(line + 1, 0, line + 1, 0));
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

            /* Two sources, one fix: a cycle the detector saw at runtime and a
             * contract the type graph cannot prove both come down to putting
             * `weak` on one field. The message tail is shared so the same
             * action serves both. */
            if (msg && (strncmp(msg, XLSP_CYCLE_DIAG_PREFIX, strlen(XLSP_CYCLE_DIAG_PREFIX)) == 0 ||
                        strncmp(msg, XLSP_CONTRACT_CYCLE_DIAG_PREFIX,
                                strlen(XLSP_CONTRACT_CYCLE_DIAG_PREFIX)) == 0)) {
                char field[128], owner[128], target[128];
                if (parse_cycle_diagnostic(msg, field, sizeof(field), owner, sizeof(owner), target,
                                           sizeof(target)))
                    push_weak_field_action(actions, uri, diag, field, owner, target);
            }

            if (msg && strncmp(msg, "closure cycle:", 14) == 0)
                push_defer_clear_action(actions, uri, doc->content, diag);

            if (msg && strstr(msg, "is not directly iterable") && strstr(msg, ".variants`")) {
                char enum_name[128];
                XrJsonValue *diag_range = xjson_get_object(diag, "range");
                XrJsonValue *diag_start = diag_range ? xjson_get_object(diag_range, "start") : NULL;
                if (diag_start && extract_enum_variants_target(msg, enum_name, sizeof(enum_name))) {
                    push_enum_variants_action(actions, uri, doc->content,
                                              xjson_get_int(diag_start, "line"), enum_name);
                }
            }
        }
    }

    // Refactor: Convert var → const (when variable is never reassigned)
    if (doc->content && doc->ast) {
        // Find if cursor is on a "var" declaration line
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

        if (strncmp(trimmed, "var ", 4) == 0) {
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
                        int var_col = (int) (trimmed - cur_line_start);

                        XrJsonValue *action = xjson_new_object();
                        xjson_object_set(action, "title",
                                         xjson_new_string("Convert 'var' to 'const'"));
                        xjson_object_set(action, "kind", xjson_new_string("refactor.rewrite"));

                        XrJsonValue *edit = xjson_new_object();
                        XrJsonValue *changes = xjson_new_object();
                        XrJsonValue *edits_arr = xjson_new_array();

                        XrJsonValue *text_edit = xjson_new_object();
                        xjson_object_set(text_edit, "newText", xjson_new_string("const"));
                        xjson_object_set(
                            text_edit, "range",
                            xjson_make_range(sel_start_line, var_col, sel_start_line, var_col + 3));

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
                    snprintf(decl_text, sizeof(decl_text), "var extracted = %s\n", selected);
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
