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

/*
 * Validate an import specifier and report errors for disallowed patterns.
 * Returns true if valid, false if an error was reported.
 */
static bool validate_import_specifier(Parser *parser, const char *path) {
    size_t len = strlen(path);

    /* Reject .xr extension */
    if (len >= 3 && strcmp(path + len - 3, ".xr") == 0) {
        xr_parser_error(parser, "do not include '.xr' extension in import path");
        return false;
    }

    /* Reject trailing slash */
    if (len > 0 && path[len - 1] == '/') {
        xr_parser_error(parser, "do not include trailing '/' in import path");
        return false;
    }

    /* Reject explicit /index suffix */
    if (len >= 6 && strcmp(path + len - 6, "/index") == 0) {
        xr_parser_error(parser, "do not specify 'index' explicitly in import path");
        return false;
    }

    /* Reject absolute paths */
    if (path[0] == '/' || (len >= 2 && path[1] == ':') || (len >= 3 && path[2] == ':')) {
        xr_parser_error(parser, "absolute paths are not supported in imports");
        return false;
    }

    return true;
}

/*
 * Extract path content from double-quoted string
 * Input: "path/to/module" (with quotes)
 * Output: path/to/module (without quotes)
 */
static char *extract_quoted_path(Parser *parser) {
    int len = parser->previous.length - 2;  // Remove two quotes
    char *path = (char *) ast_alloc(parser->X, (size_t) len + 1);
    memcpy(path, parser->previous.start + 1, len);
    path[len] = '\0';
    return path;
}

/*
 * Parse unquoted module name (bare stdlib identifier).
 * Rejects bare owner/name form; packages must use quoted paths.
 */
static void parse_bare_module_name(Parser *parser, char **out_name) {
    xr_parser_consume(parser, TK_NAME, "expected module name");

    char first_part[256];
    memcpy(first_part, parser->previous.start, parser->previous.length);
    first_part[parser->previous.length] = '\0';

    /* Reject bare owner/name form — use import "owner/name" instead */
    if (xr_parser_check(parser, TK_SLASH)) {
        xr_parser_error(parser, "bare 'import owner/name' is not supported; "
                                "use 'import \"owner/name\"' with quotes");
        *out_name = ast_strdup(parser->X, first_part);
        return;
    }
    *out_name = ast_strdup(parser->X, first_part);
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
    const char *name_start = module_name;
    const char *name_end = module_name + strlen(module_name);

    // Find last path separator
    const char *last_sep = strrchr(module_name, '/');
    if (last_sep) {
        name_start = last_sep + 1;
    }

    // Remove .xr extension
    const char *ext = strstr(name_start, ".xr");
    if (ext) {
        name_end = ext;
    }

    // Calculate name length
    int name_len = (int) (name_end - name_start);
    if (name_len <= 0) {
        return NULL;
    }

    char *alias = (char *) ast_alloc(parser->X, (size_t) name_len + 1);
    memcpy(alias, name_start, name_len);
    alias[name_len] = '\0';

    // Convert illegal characters to underscore (e.g. my-utils -> my_utils)
    for (int i = 0; i < name_len; i++) {
        if (alias[i] == '-' || alias[i] == '.') {
            alias[i] = '_';
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
// Free an ImportMember array (each entry owns heap-allocated name / alias).
static void free_import_members(ImportMember *members, int count) {
    if (!members)
        return;
    for (int i = 0; i < count; i++) {
    }
}

// Free a ReexportMember array (same ownership as ImportMember).
static void free_reexport_members(ReexportMember *members, int count) {
    if (!members)
        return;
    for (int i = 0; i < count; i++) {
    }
}

static bool parse_import_members(Parser *parser, ImportMember **out_members, int *out_count) {
    XR_DCHECK(parser != NULL, "parse_import_members: NULL parser");
    int capacity = 8;
    ImportMember *members =
        (ImportMember *) ast_alloc_array(parser->X, sizeof(ImportMember), (size_t) capacity);
    int count = 0;

    do {
        if (xr_parser_check(parser, TK_RBRACE))
            break;

        // Expand capacity
        if (count >= capacity) {
            capacity *= 2;
            ImportMember *_new_members = (ImportMember *) ast_alloc_array(
                parser->X, sizeof(ImportMember), (size_t) capacity);
            memcpy(_new_members, members, sizeof(ImportMember) * (size_t) count);
            members = _new_members;
        }

        // Parse member name
        xr_parser_consume(parser, TK_NAME, "expected import member name");
        int name_len = parser->previous.length;
        members[count].name = (char *) ast_alloc(parser->X, (size_t) name_len + 1);
        memcpy(members[count].name, parser->previous.start, name_len);
        members[count].name[name_len] = '\0';
        members[count].alias = NULL;

        // Check if has alias: import { foo as bar }
        if (xr_parser_match(parser, TK_AS)) {
            xr_parser_consume(parser, TK_NAME, "expected alias");
            int alias_len = parser->previous.length;
            members[count].alias = (char *) ast_alloc(parser->X, (size_t) alias_len + 1);
            memcpy(members[count].alias, parser->previous.start, alias_len);
            members[count].alias[alias_len] = '\0';
        }

        count++;
    } while (xr_parser_match(parser, TK_COMMA));

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
    XR_DCHECK(parser != NULL, "parse_import_declaration: NULL parser");
    int line = parser->previous.line;  // import keyword already consumed

    char *module_name = NULL;
    char *alias = NULL;
    bool is_quoted = false;
    ImportMember *members = NULL;
    int member_count = 0;

    // ========== 1. Named import: import { a, b } from "module" ==========
    if (xr_parser_check(parser, TK_LBRACE)) {
        xr_parser_advance(parser);  // Consume {

        // Parse member list
        parse_import_members(parser, &members, &member_count);

        xr_parser_consume(parser, TK_RBRACE, "expected '}'");
        if (!xr_parser_match_name(parser, "from")) {
            xr_parser_error(parser, "expected 'from'");
            free_import_members(members, member_count);
            return NULL;
        }

        // Parse module path (can be quoted path or bare module name)
        if (xr_parser_check(parser, TK_LITERAL_STRING)) {
            xr_parser_advance(parser);
            module_name = extract_quoted_path(parser);
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
        xr_parser_advance(parser);
        module_name = extract_quoted_path(parser);
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
        xr_parser_error_at_current(
            parser, "JS-style 'import name from \"module\"' is not supported. "
                    "Use 'import \"module\"' or 'import { name } from \"module\"' in Xray");
        return NULL;
    }

    // ========== Parse alias for whole import ==========
    if (member_count == 0) {
        if (xr_parser_match(parser, TK_AS)) {
            // Explicit alias: import xxx as alias
            xr_parser_consume(parser, TK_NAME, "expected alias");
            alias = (char *) ast_alloc(parser->X, (size_t) parser->previous.length + 1);
            memcpy(alias, parser->previous.start, parser->previous.length);
            alias[parser->previous.length] = '\0';
        } else {
            // Auto-extract alias from module path
            alias = extract_default_alias(parser, module_name);
        }

        // Check if alias is valid
        if (!alias || alias[0] == '\0') {
            xr_parser_error(
                parser, "cannot extract variable name from module path, use 'as alias' to specify");
            return NULL;
        }
    }

    // ========== Create AST node ==========
    AstNode *node = xr_ast_import_stmt_ex(parser->X, module_name, alias, is_quoted, members,
                                          member_count, line);

    // Clean up temporary memory (members are taken over by AST node)

    return node;
}

/*
 * Parse export declaration
 * Supported syntax:
 * 1. export fn add() {}
 * 2. export let PI = 3.14
 * 3. export class User {}
 * 4. export { a, b as c } from "./file" (re-export)
 * 5. export * from "./file" (re-export all)
 */
AstNode *xr_parse_export_declaration(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_export_declaration: NULL parser");
    int line = parser->previous.line;  // export keyword already consumed

    // Check re-export: export * from "..."
    if (xr_parser_match(parser, TK_STAR)) {
        // export * from "./file"
        if (!xr_parser_match_name(parser, "from")) {
            xr_parser_error(parser, "expected 'from' after 'export *'");
            return NULL;
        }
        if (!xr_parser_match(parser, TK_LITERAL_STRING)) {
            xr_parser_error(parser, "expected string path after 'from'");
            return NULL;
        }

        // Extract path
        size_t len = parser->previous.length - 2;  // Remove quotes
        char *from_path = (char *) ast_alloc(parser->X, (size_t) len + 1);
        memcpy(from_path, parser->previous.start + 1, len);
        from_path[len] = '\0';

        // xr_ast_export_reexport strdups from_path; release our copy.
        AstNode *node = xr_ast_export_reexport(parser->X, from_path, NULL, 0, true, line);
        return node;
    }

    // Check re-export: export { a, b as c } from "..."
    if (xr_parser_match(parser, TK_LBRACE)) {
        // Parse member list
        int capacity = 4;
        int count = 0;
        ReexportMember *members = (ReexportMember *) ast_alloc_array(
            parser->X, sizeof(ReexportMember), (size_t) capacity);

        do {
            if (!xr_parser_match(parser, TK_NAME)) {
                xr_parser_error_expected_name(parser, "expected member name in export { }");
                free_reexport_members(members, count);
                return NULL;
            }

            // Expand capacity
            if (count >= capacity) {
                capacity *= 2;
                ReexportMember *new_members = (ReexportMember *) ast_alloc_array(
                    parser->X, sizeof(ReexportMember), (size_t) capacity);
                memcpy(new_members, members, count * sizeof(ReexportMember));
                members = new_members;
            }

            // Copy member name
            size_t len = parser->previous.length;
            char *name = (char *) ast_alloc(parser->X, (size_t) len + 1);
            memcpy(name, parser->previous.start, len);
            name[len] = '\0';
            members[count].name = name;
            members[count].alias = NULL;

            // Check alias
            if (xr_parser_match(parser, TK_AS)) {
                if (!xr_parser_match(parser, TK_NAME)) {
                    xr_parser_error_expected_name(parser, "expected alias after 'as'");
                    free_reexport_members(members, count + 1);
                    return NULL;
                }
                len = parser->previous.length;
                char *alias = (char *) ast_alloc(parser->X, (size_t) len + 1);
                memcpy(alias, parser->previous.start, len);
                alias[len] = '\0';
                members[count].alias = alias;
            }

            count++;
        } while (xr_parser_match(parser, TK_COMMA));

        if (!xr_parser_match(parser, TK_RBRACE)) {
            xr_parser_error(parser, "expected '}' in export { }");
            free_reexport_members(members, count);
            return NULL;
        }

        /* Decide: re-export (has 'from') or post-hoc export list (no 'from') */
        if (xr_parser_check_name(parser, "from")) {
            /* Re-export: export { a, b as c } from "..." */
            xr_parser_advance(parser);
            if (!xr_parser_match(parser, TK_LITERAL_STRING)) {
                xr_parser_error(parser, "expected string path after 'from'");
                free_reexport_members(members, count);
                return NULL;
            }
            size_t path_len = parser->previous.length - 2;
            char *from_path = (char *) ast_alloc(parser->X, (size_t) path_len + 1);
            memcpy(from_path, parser->previous.start + 1, path_len);
            from_path[path_len] = '\0';
            AstNode *node =
                xr_ast_export_reexport(parser->X, from_path, members, count, false, line);
            return node;
        }

        /* Post-hoc export list: export { a, b, c } */
        for (int i = 0; i < count; i++) {
            if (members[i].alias) {
                xr_parser_error(parser, "'as' alias in 'export { }' requires 'from' "
                                        "(re-export); for post-hoc export, use the original name");
                free_reexport_members(members, count);
                return NULL;
            }
        }
        char **names = (char **) ast_alloc_array(parser->X, sizeof(char *), (size_t) count);
        for (int i = 0; i < count; i++)
            names[i] = members[i].name;
        return xr_ast_export_list(parser->X, names, count, line);
    }

    // Parse exported declaration
    AstNode *declaration = NULL;
    char *export_name = NULL;

    if (xr_parser_match(parser, TK_FN)) {
        // export fn add() {}
        declaration = xr_parse_function_declaration(parser);

        // Extract function name from declaration
        if (declaration && declaration->type == AST_FUNCTION_DECL) {
            export_name = declaration->as.function_decl.name;
        }
    } else if (xr_parser_match(parser, TK_CLASS)) {
        // export class MyClass {}
        declaration = xr_parse_class_declaration(parser);

        // Extract class name from declaration
        if (declaration && declaration->type == AST_CLASS_DECL) {
            export_name = declaration->as.class_decl.name;
        }
    } else if (xr_parser_match(parser, TK_STRUCT)) {
        // export struct Point {}
        declaration = xr_parse_struct_declaration(parser);

        if (declaration && declaration->type == AST_STRUCT_DECL) {
            export_name = declaration->as.struct_decl.name;
        }
    } else if (xr_parser_check(parser, TK_LET)) {
        xr_parser_error(parser, "mutable export is not supported; use 'export const' instead");
        return NULL;
    } else if (xr_parser_match(parser, TK_CONST)) {
        // export const PI = 3.14
        declaration = xr_parse_single_var_declaration(parser, 1);

        // Extract variable name from constant declaration
        if (declaration && declaration->type == AST_CONST_DECL) {
            export_name = declaration->as.var_decl.name;
        }
    } else if (xr_parser_match(parser, TK_TYPE_ALIAS)) {
        // export type Point = { x: float, y: float }
        declaration = xr_parse_type_alias_declaration(parser);

        if (declaration && declaration->type == AST_TYPE_ALIAS) {
            export_name = declaration->as.type_alias.name;
        }
    } else if (xr_parser_check(parser, TK_NAME)) {
        xr_parser_error(parser, "bare 'export name' is not supported; "
                                "use 'export { name1, name2 }' with braces");
        return NULL;
    } else {
        xr_parser_error_expected_name(
            parser, "expected fn, class, let, const or variable name after 'export'");
        return NULL;
    }

    if (!declaration) {
        xr_parser_error(parser, "failed to parse export declaration");
        return NULL;
    }

    return xr_ast_export_stmt(parser->X, declaration, export_name, line);
}
