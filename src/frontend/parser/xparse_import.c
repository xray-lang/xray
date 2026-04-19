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
 * Extract path content from double-quoted string
 * Input: "path/to/module" (with quotes)
 * Output: path/to/module (without quotes)
 */
static char *extract_quoted_path(Parser *parser) {
    int len = parser->previous.length - 2;  // Remove two quotes
    char *path = (char *)ast_alloc(parser->X, (size_t)len + 1);
    memcpy(path, parser->previous.start + 1, len);
    path[len] = '\0';
    return path;
}

/*
 * Classify quoted import type based on path
 *
 * Classification rules:
 * - Starts with "./" or "../" -> IMPORT_FILE (single file import, script mode only)
 * - Otherwise                  -> IMPORT_DIR (directory import, project mode only)
 */
static ImportType classify_quoted_import(const char *path) {
    if (strncmp(path, "./", 2) == 0 || strncmp(path, "../", 3) == 0) {
        return IMPORT_FILE;
    }
    return IMPORT_DIR;
}

/*
 * Parse unquoted module name (stdlib or third-party package)
 *
 * Classification rules:
 * - Word form (no /)       -> IMPORT_STDLIB (standard library)
 * - owner/name format      -> IMPORT_PACKAGE (third-party package)
 *
 * @param parser       Parser
 * @param out_name     Output: module name (caller must free)
 * @param out_type     Output: import type
 */
static void parse_unquoted_module(Parser *parser, char **out_name, ImportType *out_type) {
    xr_parser_consume(parser, TK_NAME, "expected module name");

    char first_part[256];
    memcpy(first_part, parser->previous.start, parser->previous.length);
    first_part[parser->previous.length] = '\0';

    // Check if has / indicating third-party package (owner/name)
    if (xr_parser_match(parser, TK_SLASH)) {
        xr_parser_consume(parser, TK_NAME, "expected package name");
        int name_len = parser->previous.length;
        int total_len = strlen(first_part) + 1 + name_len;
        *out_name = (char *)ast_alloc(parser->X, (size_t)total_len + 1);
        snprintf(*out_name, total_len + 1, "%s/%.*s",
                 first_part, name_len, parser->previous.start);
        *out_type = IMPORT_PACKAGE;
    } else {
        *out_name = ast_strdup(parser->X, first_part);
        *out_type = IMPORT_STDLIB;
    }
}

/*
 * Extract default alias from module path
 *
 * Extraction rules:
 * 1. Take last segment of path (/ separated)
 * 2. Remove .xr extension
 * 3. Convert - and . to _
 *
 * Examples:
 * - "time"           -> time
 * - "alice/utils"    -> utils
 * - "./helper.xr"    -> helper
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
    int name_len = (int)(name_end - name_start);
    if (name_len <= 0) {
        return NULL;
    }

    char *alias = (char *)ast_alloc(parser->X, (size_t)name_len + 1);
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
    if (!members) return;
    for (int i = 0; i < count; i++) {
    }
}

// Free a ReexportMember array (same ownership as ImportMember).
static void free_reexport_members(ReexportMember *members, int count) {
    if (!members) return;
    for (int i = 0; i < count; i++) {
    }
}

static bool parse_import_members(Parser *parser, ImportMember **out_members, int *out_count) {
    XR_DCHECK(parser != NULL, "parse_import_members: NULL parser");
    int capacity = 8;
    ImportMember *members = (ImportMember *)ast_alloc_array(parser->X, sizeof(ImportMember), (size_t)capacity);
    int count = 0;

    do {
        if (xr_parser_check(parser, TK_RBRACE)) break;

        // Expand capacity
        if (count >= capacity) {
            capacity *= 2;
            ImportMember *_new_members = (ImportMember *)ast_alloc_array(
                parser->X, sizeof(ImportMember), (size_t)capacity);
            memcpy(_new_members, members, sizeof(ImportMember) * (size_t)count);
            members = _new_members;

        }

        // Parse member name
        xr_parser_consume(parser, TK_NAME, "expected import member name");
        int name_len = parser->previous.length;
        members[count].name = (char *)ast_alloc(parser->X, (size_t)name_len + 1);
        memcpy(members[count].name, parser->previous.start, name_len);
        members[count].name[name_len] = '\0';
        members[count].alias = NULL;

        // Check if has alias: import { foo as bar }
        if (xr_parser_match(parser, TK_AS)) {
            xr_parser_consume(parser, TK_NAME, "expected alias");
            int alias_len = parser->previous.length;
            members[count].alias = (char *)ast_alloc(parser->X, (size_t)alias_len + 1);
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
 * Supports five import syntaxes:
 *
 * 1. Named import (for tree-shaking)
 *    import { add, multiply } from "utils"
 *    import { greet as sayHello } from time
 *
 * 2. Single file import (script mode only, needs quotes, starts with ./ or ../)
 *    import "./helper.xr"
 *    import "../utils/math.xr" as math
 *
 * 3. Directory import (project mode only, needs quotes, relative to project root)
 *    import "models/user"
 *    import "services/auth" as auth
 *
 * 4. Standard library (both modes, no quotes, word form)
 *    import time
 *    import json as j
 *
 * 5. Third-party package (both modes, no quotes, owner/name format)
 *    import alice/utils
 *    import bob/http-client as http
 */
AstNode *xr_parse_import_declaration(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_import_declaration: NULL parser");
    int line = parser->previous.line;  // import keyword already consumed

    char *module_name = NULL;
    char *alias = NULL;
    ImportType import_type = IMPORT_STDLIB;
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

        // Parse module path (can be quoted path or unquoted module name)
        if (xr_parser_check(parser, TK_LITERAL_STRING)) {
            xr_parser_advance(parser);
            module_name = extract_quoted_path(parser);
            import_type = classify_quoted_import(module_name);
        } else {
            parse_unquoted_module(parser, &module_name, &import_type);
        }

        // Named import doesn't need overall alias
        alias = NULL;
    }
    // ========== 2/3. Quoted import (single file or directory) ==========
    else if (xr_parser_check(parser, TK_LITERAL_STRING)) {
        xr_parser_advance(parser);
        module_name = extract_quoted_path(parser);
        import_type = classify_quoted_import(module_name);
    }
    // ========== 4/5. Unquoted import (stdlib or third-party package) ==========
    else {
        parse_unquoted_module(parser, &module_name, &import_type);
    }

    // Detect JS-style default import: import fs from "fs"
    // In Xray, use: import "fs" or import { readFile } from "fs"
    if (xr_parser_check_name(parser, "from")) {
        xr_parser_error_at_current(parser,
            "JS-style 'import name from \"module\"' is not supported. "
            "Use 'import \"module\"' or 'import { name } from \"module\"' in Xray");
        return NULL;
    }

    // ========== Parse alias for whole import ==========
    if (member_count == 0) {
        if (xr_parser_match(parser, TK_AS)) {
            // Explicit alias: import xxx as alias
            xr_parser_consume(parser, TK_NAME, "expected alias");
            alias = (char *)ast_alloc(parser->X, (size_t)parser->previous.length + 1);
            memcpy(alias, parser->previous.start, parser->previous.length);
            alias[parser->previous.length] = '\0';
        } else {
            // Auto-extract alias from module path
            alias = extract_default_alias(parser, module_name);
        }

        // Check if alias is valid
        if (!alias || alias[0] == '\0') {
            xr_parser_error(parser, "cannot extract variable name from module path, use 'as alias' to specify");
            return NULL;
        }
    }

    // ========== Create AST node ==========
    AstNode *node = xr_ast_import_stmt_ex(parser->X, module_name, alias, import_type,
                                          members, member_count, line);

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
        char *from_path = (char *)ast_alloc(parser->X, (size_t)len + 1);
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
        ReexportMember *members = (ReexportMember*)ast_alloc_array(parser->X, sizeof(ReexportMember), (size_t)capacity);

        do {
            if (!xr_parser_match(parser, TK_NAME)) {
                xr_parser_error_expected_name(parser, "expected member name in export { }");
                free_reexport_members(members, count);
                return NULL;
            }

            // Expand capacity
            if (count >= capacity) {
                capacity *= 2;
                ReexportMember *new_members = (ReexportMember*)ast_alloc_array(parser->X, sizeof(ReexportMember), (size_t)capacity);
                memcpy(new_members, members, count * sizeof(ReexportMember));
                members = new_members;
            }

            // Copy member name
            size_t len = parser->previous.length;
            char *name = (char *)ast_alloc(parser->X, (size_t)len + 1);
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
                char *alias = (char *)ast_alloc(parser->X, (size_t)len + 1);
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

        // Must have from
        if (!xr_parser_match_name(parser, "from")) {
            xr_parser_error(parser, "expected 'from \"path\"' after export { }");
            free_reexport_members(members, count);
            return NULL;
        }

        if (!xr_parser_match(parser, TK_LITERAL_STRING)) {
            xr_parser_error(parser, "expected string path after 'from'");
            free_reexport_members(members, count);
            return NULL;
        }

        // Extract path
        size_t path_len = parser->previous.length - 2;
        char *from_path = (char *)ast_alloc(parser->X, (size_t)path_len + 1);
        memcpy(from_path, parser->previous.start + 1, path_len);
        from_path[path_len] = '\0';

        // xr_ast_export_reexport strdups from_path; release our copy. The
        // members array is transferred into the AST node.
        AstNode *node = xr_ast_export_reexport(parser->X, from_path, members, count, false, line);
        return node;
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
    }
    else if (xr_parser_match(parser, TK_CLASS)) {
        // export class MyClass {}
        declaration = xr_parse_class_declaration(parser);

        // Extract class name from declaration
        if (declaration && declaration->type == AST_CLASS_DECL) {
            export_name = declaration->as.class_decl.name;
        }
    }
    else if (xr_parser_match(parser, TK_STRUCT)) {
        // export struct Point {}
        declaration = xr_parse_struct_declaration(parser);

        if (declaration && declaration->type == AST_STRUCT_DECL) {
            export_name = declaration->as.struct_decl.name;
        }
    }
    else if (xr_parser_match(parser, TK_LET)) {
        // export let PI = 3.14
        declaration = xr_parse_single_var_declaration(parser, 0);

        // Extract variable name from declaration
        if (declaration && declaration->type == AST_VAR_DECL) {
            export_name = declaration->as.var_decl.name;
        }
    }
    else if (xr_parser_match(parser, TK_CONST)) {
        // export const PI = 3.14
        declaration = xr_parse_single_var_declaration(parser, 1);

        // Extract variable name from constant declaration
        if (declaration && declaration->type == AST_CONST_DECL) {
            export_name = declaration->as.var_decl.name;
        }
    }
    else if (xr_parser_match(parser, TK_TYPE_ALIAS)) {
        // export type Point = { x: float, y: float }
        declaration = xr_parse_type_alias_declaration(parser);

        if (declaration && declaration->type == AST_TYPE_ALIAS) {
            export_name = declaration->as.type_alias.name;
        }
    }
    else if (xr_parser_check(parser, TK_NAME)) {
        // export a, b, c - export already defined variable list
        char **names = NULL;
        int count = 0;
        int capacity = 4;
        names = (char**)ast_alloc_array(parser->X, sizeof(char*), (size_t)capacity);

        do {
            if (!xr_parser_match(parser, TK_NAME)) {
                xr_parser_error_expected_name(parser, "expected variable name after 'export'");
                return NULL;
            }

            // Expand capacity
            if (count >= capacity) {
                capacity *= 2;
                char **new_names = (char**)ast_alloc_array(parser->X, sizeof(char*), (size_t)capacity);
                memcpy(new_names, names, count * sizeof(char*));
                names = new_names;
            }

            // Copy variable name
            size_t len = parser->previous.length;
            char *name = (char *)ast_alloc(parser->X, (size_t)len + 1);
            memcpy(name, parser->previous.start, len);
            name[len] = '\0';
            names[count++] = name;
        } while (xr_parser_match(parser, TK_COMMA));

        // Create export list node
        return xr_ast_export_list(parser->X, names, count, line);
    }
    else {
        xr_parser_error_expected_name(parser, "expected fn, class, let, const or variable name after 'export'");
        return NULL;
    }

    if (!declaration) {
        xr_parser_error(parser, "failed to parse export declaration");
        return NULL;
    }

    return xr_ast_export_stmt(parser->X, declaration, export_name, line);
}
