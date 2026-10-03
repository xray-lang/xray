/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_import.c - Import/export declaration parsing
 *
 * KEY CONCEPT:
 *   Parses import and export declarations for the module system.
 *   Extracted from xparse.c for maintainability.
 */

#include "xparse_internal.h"
#include "../../base/xchecks.h"
#include "../../base/xutf8.h"
#include "../lexer/xquoted_literal.h"

/*
 * Validate a QUOTED import specifier and report errors for disallowed
 * patterns. Returns true if valid, false if an error was reported.
 *
 * What a specifier means is decided by its shape, not by whether it is quoted:
 *
 *     import math                 named module -- stdlib or .xrd native
 *     import "./util"             path, relative to the importing file
 *     import "owner/name"         package, resolved through xray.toml
 *
 * Quoting is only how text that is not an identifier gets written, which is
 * why a path or a package needs it and a module name does not. Treating the
 * quote itself as the signal never worked: the analyzer re-derived it from the
 * first character of the specifier, so `"math"` read as bare and resolved to
 * the standard library anyway -- twenty imports in this tree were written that
 * way and every one of them silently worked.
 */
static bool validate_import_specifier(Parser *parser, const char *path) {
    if (!xr_parser_healthy(parser)) return false;
    size_t len = xr_parser_string_length(parser, path);
    if (!xr_parser_healthy(parser)) return false;

    /* Reject .xr extension */
    if (len >= 3 && xr_parser_compare_string(parser, path + len - 3, ".xr") == 0) {
        do {
            xr_parser_error(parser, "do not include '.xr' extension in import path");
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
        return false;
    }

    /* Reject trailing slash */
    if (len > 0 && xr_parser_read_byte(parser, path + len - 1) == '/') {
        do {
            xr_parser_error(parser, "do not include trailing '/' in import path");
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
        return false;
    }

    /* Reject explicit /index suffix */
    if (len >= 6 && xr_parser_compare_string(parser, path + len - 6, "/index") == 0) {
        do {
            xr_parser_error(parser, "do not specify 'index' explicitly in import path");
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
        return false;
    }

    /* Reject absolute paths */
    if (xr_parser_read_byte(parser, path) == '/' ||
        (len >= 2 && xr_parser_read_byte(parser, path + 1) == ':') ||
        (len >= 3 && xr_parser_read_byte(parser, path + 2) == ':')) {
        do {
            xr_parser_error(parser, "absolute paths are not supported in imports");
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
        return false;
    }

    /* A quoted specifier must have the shape of a path or of a package. One
     * segment with no separator is a module name, and those are written bare. */
    if (xr_parser_find_byte(parser, path, '/', len) == NULL) {
        if (!xr_parser_healthy(parser)) return false;
        char msg[192];
        xr_parser_format(parser, msg, sizeof(msg),
                 "quoted import must be a path ('./%s') or a package "
                 "('owner/%s'); write a module name without quotes: import %s",
                 path, path, path);
        do {
            xr_parser_error(parser, msg);
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
        return false;
    }

    return true;
}

static char *extract_quoted_path(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XrParsedQuoted payload = {0};
    const char *error = NULL;
    bool decode_escapes = parser->previous.escape_mode == XR_LITERAL_ESCAPED;
    if (xr_parser_decode_quoted(parser, &parser->previous, decode_escapes, &payload, &error) != XR_QUOTED_OK) {
        do {
            xr_parser_error_at_previous(parser, error ? error : "invalid import path literal");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    if (xr_parser_find_byte(parser, payload.bytes, '\0', payload.length) != NULL ||
        !xr_parser_utf8_validate(parser, (const char *) payload.bytes, payload.length)) {

        do {
            xr_parser_error_at_previous(parser, "import path must be valid UTF-8 without NUL bytes");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    char *path = (char *) ast_alloc(parser->compiler_session, payload.length + 1);
    if (!xr_parser_healthy(parser)) return NULL;
    do { if (!ast_copy(parser->compiler_session, path, payload.bytes, payload.length)) return NULL; } while (0);
    do { if (!ast_work(parser->compiler_session, 1)) return NULL; path[payload.length] = '\0'; } while (0);

    return path;
}

/*
 * Parse unquoted module name (bare stdlib identifier).
 * Rejects bare owner/name form; packages must use quoted paths.
 */
static void parse_bare_module_name(Parser *parser, char **out_name) {
    if (!xr_parser_healthy(parser)) return;
    do {
        xr_parser_consume(parser, TK_NAME, "expected module name");
        if (!xr_parser_healthy(parser)) return;
    } while (0);

    char *first_part = ast_strndup(parser->compiler_session, parser->previous.start,
                                  (size_t) parser->previous.length);
    if (!xr_parser_healthy(parser)) return;

    /* Reject bare owner/name form — use import "owner/name" instead */
    if (xr_parser_check(parser, TK_SLASH)) {
        do {
            xr_parser_error(parser, "bare 'import owner/name' is not supported; "
                                "use 'import \"owner/name\"' with quotes");
            if (!xr_parser_healthy(parser)) return;
        } while (0);
        do {
            *out_name = ast_strdup(parser->compiler_session, first_part);
            if (!xr_parser_healthy(parser)) return;
        } while (0);
        return;
    }
    do {
        *out_name = ast_strdup(parser->compiler_session, first_part);
        if (!xr_parser_healthy(parser)) return;
    } while (0);
}

/*
 * Extract default alias from module path
 *
 * Extraction rules:
 * 1. Take last segment of path (/ separated)
 * 2. Convert - and . to _
 *
 * Examples:
 * - "time"           -> time
 * - "alice/utils"    -> utils
 * - "./helper"       -> helper
 * - "models/user"    -> user
 */
static char *extract_default_alias(Parser *parser, const char *module_name) {
    if (!xr_parser_healthy(parser)) return NULL;
    const char *name_start = module_name;
    const char *name_end = module_name + xr_parser_string_length(parser, module_name);
    if (!xr_parser_healthy(parser)) return NULL;

    // Find last path separator
    const char *last_sep = NULL;
    for (const char *p = module_name; xr_parser_step(parser) && (p < name_end); ++p) {
        char value = xr_parser_read_byte(parser, p);
        if (!xr_parser_healthy(parser)) return NULL;
        if (value == '/') last_sep = p;
    }
    if (last_sep) {
        name_start = last_sep + 1;
    }

    // Remove .xr extension
    const char *ext = NULL;
    for (const char *p = name_start; xr_parser_step(parser) && (name_end - p >= 3); ++p) {
        int comparison = xr_parser_compare_bytes(parser, p, ".xr", 3);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!comparison) { ext = p; break; }
    }
    if (ext) {
        name_end = ext;
    }

    // Calculate name length
    int name_len = (int) (name_end - name_start);
    if (name_len <= 0) {
        return NULL;
    }

    char *alias = (char *) ast_alloc(parser->compiler_session, (size_t) name_len + 1);
    if (!xr_parser_healthy(parser)) return NULL;
    do { if (!ast_copy(parser->compiler_session, alias, name_start, name_len)) return NULL; } while (0);
    do { if (!ast_work(parser->compiler_session, 1)) return NULL; alias[name_len] = '\0'; } while (0);

    // Convert illegal characters to underscore (e.g. my-utils -> my_utils)
    for (int i = 0; xr_parser_step(parser) && (i < name_len); i++) {
        char value = xr_parser_read_byte(parser, alias + i);
        if (!xr_parser_healthy(parser)) return NULL;
        if (value == '-' || value == '.') {
            do { if (!ast_work(parser->compiler_session, 1)) return NULL; alias[i] = '_'; } while (0);
        }
    }

    return alias;
}

/*
 * Parse named import member list
 *
 * Syntax: { name1, name2 as alias2, name3 }
 *
 * @param parser        Parser
 * @param out_members   Output: member array
 * @param out_count     Output: member count
 * @return              Returns true on success
 */
static bool parse_import_members(Parser *parser, ImportMember **out_members, int *out_count) {
    if (!xr_parser_healthy(parser)) return false;
    XR_DCHECK(parser != NULL, "parse_import_members: NULL parser");
    int capacity = 8;
    ImportMember *members = (ImportMember *) ast_alloc_array(
        parser->compiler_session, sizeof(ImportMember), (size_t) capacity);
    if (!xr_parser_healthy(parser)) return false;
    int count = 0;

    do {
        if (xr_parser_check(parser, TK_RBRACE))
            break;

        // Expand capacity
        if (count >= capacity) {
            capacity *= 2;
            ImportMember *_new_members = (ImportMember *) ast_alloc_array(
                parser->compiler_session, sizeof(ImportMember), (size_t) capacity);
            if (!xr_parser_healthy(parser)) return false;
            do { if (!ast_copy(parser->compiler_session, _new_members, members, sizeof(ImportMember) * (size_t) count)) return false; } while (0);
            members = _new_members;
        }

        // Parse member name
        do {
            xr_parser_consume(parser, TK_NAME, "expected import member name");
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
        int name_len = parser->previous.length;
        do {
            members[count].name = (char *) ast_alloc(parser->compiler_session, (size_t) name_len + 1);
            if (!xr_parser_healthy(parser)) return false;
        } while (0);
        do { if (!ast_copy(parser->compiler_session, members[count].name, parser->previous.start, name_len)) return false; } while (0);
        if (!ast_work(parser->compiler_session, 1)) return false;
        members[count].name[name_len] = '\0';
        members[count].alias = NULL;

        // Check if has alias: import { foo as bar }
        if (xr_parser_match(parser, TK_AS)) {
            do {
                xr_parser_consume(parser, TK_NAME, "expected alias");
                if (!xr_parser_healthy(parser)) return false;
            } while (0);
            int alias_len = parser->previous.length;
            do {
                members[count].alias =
                (char *) ast_alloc(parser->compiler_session, (size_t) alias_len + 1);
                if (!xr_parser_healthy(parser)) return false;
            } while (0);
            do { if (!ast_copy(parser->compiler_session, members[count].alias, parser->previous.start, alias_len)) return false; } while (0);
            members[count].alias[alias_len] = '\0';
        }

        count++;
    } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA));

    *out_members = members;
    *out_count = count;
    return true;
}

/*
 * Parse import declaration
 *
 * Three orthogonal import forms:
 *
 * 1. Bare name (stdlib only):
 *    import time
 *    import json as j
 *
 * 2. Quoted path (file, directory, or package):
 *    import "./helper" as h
 *    import "models/user"
 *    import "alice/utils"
 *
 * 3. Named / selective import:
 *    import { add, multiply } from "utils"
 *    import { greet as sayHello } from time
 */
AstNode *xr_parse_import_declaration(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_import_declaration: NULL parser");
    int line = parser->previous.line;  // import keyword already consumed
    int column = parser->previous.column;

    char *module_name = NULL;
    char *alias = NULL;
    bool is_quoted = false;
    ImportMember *members = NULL;
    int member_count = 0;

    // ========== 1. Named import: import { a, b } from "module" ==========
    if (xr_parser_check(parser, TK_LBRACE)) {
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);  // Consume {

        // Parse member list
        parse_import_members(parser, &members, &member_count);

        do {
            xr_parser_consume(parser, TK_RBRACE, "expected '}'");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        if (!xr_parser_match_name(parser, "from")) {
            do {
                xr_parser_error(parser, "expected 'from'");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);

            return NULL;
        }

        // Parse module path (can be quoted path or bare module name)
        if (xr_parser_check(parser, TK_LITERAL_STRING)) {
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                module_name = extract_quoted_path(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (!module_name)
                return NULL;
            if (!validate_import_specifier(parser, module_name))
                return NULL;
            is_quoted = true;
        } else {
            parse_bare_module_name(parser, &module_name);
        }

        // Named import doesn't need overall alias
        alias = NULL;
    }
    // ========== 2/3. Quoted import (file, directory, or package) ==========
    else if (xr_parser_check(parser, TK_LITERAL_STRING)) {
        do {
            xr_parser_advance(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        do {
            module_name = extract_quoted_path(parser);
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        if (!module_name)
            return NULL;
        if (!validate_import_specifier(parser, module_name))
            return NULL;
        is_quoted = true;
    }
    // ========== 4. Bare import (stdlib only) ==========
    else {
        parse_bare_module_name(parser, &module_name);
    }

    // Detect JS-style default import: import fs from "fs"
    // In Xray, use: import "fs" or import { readFile } from "fs"
    if (xr_parser_check_name(parser, "from")) {
        do {
            xr_parser_error_at_current(
            parser, "JS-style 'import name from \"module\"' is not supported. "
                    "Use 'import \"module\"' or 'import { name } from \"module\"' in Xray");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    // ========== Parse alias for whole import ==========
    if (member_count == 0) {
        if (xr_parser_match(parser, TK_AS)) {
            // Explicit alias: import xxx as alias
            do {
                xr_parser_consume(parser, TK_NAME, "expected alias");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do {
                alias =
                (char *) ast_alloc(parser->compiler_session, (size_t) parser->previous.length + 1);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do { if (!ast_copy(parser->compiler_session, alias, parser->previous.start, parser->previous.length)) return NULL; } while (0);
            do { if (!ast_work(parser->compiler_session, 1)) return NULL; alias[parser->previous.length] = '\0'; } while (0);
        } else {
            // Auto-extract alias from module path
            do {
                alias = extract_default_alias(parser, module_name);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
        }

        // Check if alias is valid
        if (!alias || alias[0] == '\0') {
            do {
                xr_parser_error(
                parser, "cannot extract variable name from module path, use 'as alias' to specify");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
    }

    // ========== Create AST node ==========
    AstNode *node = xr_ast_import_stmt_ex(parser->compiler_session, module_name, alias, is_quoted,
                                          members, member_count, line);
    if (!xr_parser_healthy(parser)) return NULL;
    if (node) {
        node->column = column;
        node->end_line = parser->previous.line;
        node->end_column = parser->previous.column + parser->previous.length;
    }

    // Clean up temporary memory (members are taken over by AST node)

    return node;
}

/*
 * Parse export declaration
 * Supported syntax:
 * 1. export fn add() {}
 * 2. export const PI = 3.14
 * 3. export class User {}
 * 4. export { a, b as c } from "./file" (re-export)
 * 5. export * from "./file" (re-export all)
 */
AstNode *xr_parse_export_declaration(Parser *parser) {
    if (!xr_parser_healthy(parser)) return NULL;
    XR_DCHECK(parser != NULL, "parse_export_declaration: NULL parser");
    int line = parser->previous.line;  // export keyword already consumed

    // Check re-export: export * from module / "..."
    if (xr_parser_match(parser, TK_STAR)) {
        // export * from "./file"
        if (!xr_parser_match_name(parser, "from")) {
            do {
                xr_parser_error(parser, "expected 'from' after 'export *'");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }
        bool from_is_quoted = xr_parser_match(parser, TK_LITERAL_STRING);
        if (!xr_parser_healthy(parser)) return NULL;
        if (!from_is_quoted && !xr_parser_match(parser, TK_NAME)) {
            do {
                xr_parser_error(parser, "expected module name or string path after 'from'");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
        }

        char *from_path;
        if (from_is_quoted) {
            do {
                from_path = extract_quoted_path(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            if (!from_path)
                return NULL;
        } else {
            size_t from_len = parser->previous.length;
            do {
                from_path = (char *) ast_alloc(parser->compiler_session, from_len + 1);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            do { if (!ast_copy(parser->compiler_session, from_path, parser->previous.start, from_len)) return NULL; } while (0);
            do { if (!ast_work(parser->compiler_session, 1)) return NULL; from_path[from_len] = '\0'; } while (0);
        }

        // xr_ast_export_reexport strdups from_path; release our copy.
        AstNode *node = xr_ast_export_reexport(parser->compiler_session, from_path, from_is_quoted,
                                               NULL, 0, true, line);
        if (!xr_parser_healthy(parser)) return NULL;
        return node;
    }

    // Check re-export: export { a, b as c } from "..."
    if (xr_parser_match(parser, TK_LBRACE)) {
        // Parse member list
        int capacity = 4;
        int count = 0;
        ReexportMember *members = (ReexportMember *) ast_alloc_array(
            parser->compiler_session, sizeof(ReexportMember), (size_t) capacity);
        if (!xr_parser_healthy(parser)) return NULL;

        do {
            if (xr_parser_check(parser, TK_RBRACE))
                break;
            if (!xr_parser_match(parser, TK_NAME)) {
                do {
                    xr_parser_error_expected_name(parser, "expected member name in export { }");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);

                return NULL;
            }

            // Expand capacity
            if (count >= capacity) {
                capacity *= 2;
                ReexportMember *new_members = (ReexportMember *) ast_alloc_array(
                    parser->compiler_session, sizeof(ReexportMember), (size_t) capacity);
                if (!xr_parser_healthy(parser)) return NULL;
                do { if (!ast_copy(parser->compiler_session, new_members, members, count * sizeof(ReexportMember))) return NULL; } while (0);
                members = new_members;
            }

            // Copy member name
            size_t len = parser->previous.length;
            char *name = (char *) ast_alloc(parser->compiler_session, (size_t) len + 1);
            if (!xr_parser_healthy(parser)) return NULL;
            do { if (!ast_copy(parser->compiler_session, name, parser->previous.start, len)) return NULL; } while (0);
            do { if (!ast_work(parser->compiler_session, 1)) return NULL; name[len] = '\0'; } while (0);
            members[count].name = name;
            members[count].alias = NULL;

            // Check alias
            if (xr_parser_match(parser, TK_AS)) {
                if (!xr_parser_match(parser, TK_NAME)) {
                    do {
                        xr_parser_error_expected_name(parser, "expected alias after 'as'");
                        if (!xr_parser_healthy(parser)) return NULL;
                    } while (0);

                    return NULL;
                }
                len = parser->previous.length;
                char *alias = (char *) ast_alloc(parser->compiler_session, (size_t) len + 1);
                if (!xr_parser_healthy(parser)) return NULL;
                do { if (!ast_copy(parser->compiler_session, alias, parser->previous.start, len)) return NULL; } while (0);
                do { if (!ast_work(parser->compiler_session, 1)) return NULL; alias[len] = '\0'; } while (0);
                members[count].alias = alias;
            }

            count++;
        } while (xr_parser_healthy(parser) && xr_parser_match(parser, TK_COMMA));

        if (!xr_parser_match(parser, TK_RBRACE)) {
            do {
                xr_parser_error(parser, "expected '}' in export { }");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);

            return NULL;
        }

        /* A braced export is exclusively a cross-module re-export. */
        if (xr_parser_check_name(parser, "from")) {
            /* Re-export: export { a, b as c } from module / "..." */
            do {
                xr_parser_advance(parser);
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            bool from_is_quoted = xr_parser_match(parser, TK_LITERAL_STRING);
            if (!xr_parser_healthy(parser)) return NULL;
            if (!from_is_quoted && !xr_parser_match(parser, TK_NAME)) {
                do {
                    xr_parser_error(parser, "expected module name or string path after 'from'");
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);

                return NULL;
            }
            char *from_path;
            if (from_is_quoted) {
                do {
                    from_path = extract_quoted_path(parser);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                if (!from_path) {

                    return NULL;
                }
            } else {
                size_t path_len = parser->previous.length;
                do {
                    from_path = (char *) ast_alloc(parser->compiler_session, path_len + 1);
                    if (!xr_parser_healthy(parser)) return NULL;
                } while (0);
                do { if (!ast_copy(parser->compiler_session, from_path, parser->previous.start, path_len)) return NULL; } while (0);
                do { if (!ast_work(parser->compiler_session, 1)) return NULL; from_path[path_len] = '\0'; } while (0);
            }
            AstNode *node = xr_ast_export_reexport(parser->compiler_session, from_path,
                                                   from_is_quoted, members, count, false, line);
            if (!xr_parser_healthy(parser)) return NULL;
            return node;
        }

        do {
            xr_parser_error(parser,
                        "post-hoc export was removed; put 'export' on the declaration or add "
                        "'from' for a re-export");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }

    if (xr_parser_check(parser, TK_VAR)) {
        do {
            xr_parser_error(parser, "mutable export is not supported; use 'export const' instead");
            if (!xr_parser_healthy(parser)) return NULL;
        } while (0);
        return NULL;
    }
    switch (parser->current.type) {
        case TK_FN:
        case TK_CLASS:
        case TK_FINAL:
        case TK_PACKED:
        case TK_STRUCT:
        case TK_UNION:
        case TK_INTERFACE:
        case TK_ENUM:
        case TK_CONST:
        case TK_TYPE_ALIAS:
            break;
        default:
            do {
                xr_parser_error_at_current(
                parser, "expected a function, type, or const declaration after 'export'");
                if (!xr_parser_healthy(parser)) return NULL;
            } while (0);
            return NULL;
    }

    AstNode *declaration = xr_parse_declaration(parser);
    if (!xr_parser_healthy(parser)) return NULL;
    if (!declaration)
        return NULL;
    declaration->is_exported = true;
    return declaration;
}
