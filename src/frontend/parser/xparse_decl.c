/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_decl.c - Function and container declaration parsing
 *
 * KEY CONCEPT:
 *   Parsing for function declarations, class declarations,
 *   array/map/set literals, destructuring patterns, and
 *   enum/type declarations.
 */

#include "xparse_internal.h"
#include "xtype_ref.h"
#include "../../base/xchecks.h"
#include "../../base/xarena.h"
#include "../../runtime/xerror_codes.h"
#include "../../runtime/xisolate_api.h"
#include "xtype_scope.h"
#include "../xdiag_fmt.h"

static bool current_can_start_top_level_after_recovery(Parser *parser) {
    return xr_parser_check(parser, TK_AT) || xr_parser_check(parser, TK_CLASS) ||
           xr_parser_check(parser, TK_STRUCT) || xr_parser_check(parser, TK_UNION) ||
           xr_parser_check(parser, TK_PACKED) || xr_parser_check(parser, TK_INTERFACE) ||
           xr_parser_check(parser, TK_ENUM) || xr_parser_check(parser, TK_FN) ||
           xr_parser_check(parser, TK_VAR) || xr_parser_check(parser, TK_CONST) ||
           xr_parser_check(parser, TK_COMPTIME) || xr_parser_check(parser, TK_TYPE_ALIAS) ||
           xr_parser_check(parser, TK_IMPORT) || xr_parser_check(parser, TK_EXPORT) ||
           xr_parser_check_name(parser, "asm") || xr_parser_check_name(parser, "extern");
}

/* ========== Function Parsing ========== */

// Extract the content of the just-consumed string-literal token (quotes
// stripped) into an arena-allocated, NUL-terminated string. Used for
// string arguments carried by internal attributes synthesized from extern blocks.
static const char *xr_attr_string_arg(Parser *parser) {
    Token t = parser->previous;
    size_t len = t.length >= 2 ? (size_t) (t.length - 2) : 0;
    char *s = (char *) ast_alloc(parser->compiler_session, len + 1);
    if (len > 0)
        memcpy(s, t.start + 1, len);
    s[len] = '\0';
    return s;
}

static bool xr_is_global_asm_start(Parser *parser) {
    if (!xr_parser_check_name(parser, "asm"))
        return false;
    Scanner saved = parser->scanner;
    Token next = xr_scanner_scan(&saved);
    return next.type == TK_LBRACE;
}

typedef struct XrParsedGlobalAsmFragment {
    char *text;
    size_t len;
} XrParsedGlobalAsmFragment;

static XrParsedGlobalAsmFragment xr_decode_previous_string_literal(Parser *parser) {
    Token t = parser->previous;
    size_t src_len = t.length >= 2 ? (size_t) (t.length - 2) : 0;
    const char *src = t.start + 1;
    char *tmp = (char *) xr_malloc(src_len + 1);
    XR_CHECK(tmp != NULL, "parser: global asm string decode OOM");
    size_t len = xr_process_escapes(src, src_len, tmp);
    tmp[len] = '\0';
    char *text = ast_strdup(parser->compiler_session, tmp);
    xr_free(tmp);
    return (XrParsedGlobalAsmFragment) {.text = text, .len = len};
}

static AstNode *xr_parse_global_asm_declaration(Parser *parser) {
    int line = parser->previous.line;
    int column = parser->previous.column;
    if (parser->scope_depth > 0) {
        xr_parser_error(parser, "global asm must appear at module top level");
        return NULL;
    }

    xr_parser_consume(parser, TK_LBRACE, "expected '{' after asm");

    XrParsedGlobalAsmFragment *fragments = NULL;
    int fragment_count = 0;
    int fragment_capacity = 0;
    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        if (!xr_parser_check(parser, TK_LITERAL_STRING)) {
            xr_parser_error_at_current(parser, "global asm expects string literal fragments");
            return NULL;
        }
        xr_parser_advance(parser);
        XrParsedGlobalAsmFragment fragment = xr_decode_previous_string_literal(parser);
        XR_PARSE_PUSH(parser, fragments, fragment_count, fragment_capacity, fragment);
    }

    if (fragment_count == 0) {
        xr_parser_error(parser, "global asm block requires at least one string literal");
        return NULL;
    }

    xr_parser_consume(parser, TK_RBRACE, "expected '}' after global asm block");
    Token close = parser->previous;

    size_t total = 0;
    for (int i = 0; i < fragment_count; i++) {
        total += fragments[i].len;
        if (i + 1 < fragment_count &&
            (fragments[i].len == 0 || fragments[i].text[fragments[i].len - 1] != '\n')) {
            total++;
        }
    }

    char *text = (char *) ast_alloc(parser->compiler_session, total + 1);
    size_t pos = 0;
    for (int i = 0; i < fragment_count; i++) {
        if (fragments[i].len > 0) {
            memcpy(text + pos, fragments[i].text, fragments[i].len);
            pos += fragments[i].len;
        }
        if (i + 1 < fragment_count &&
            (fragments[i].len == 0 || fragments[i].text[fragments[i].len - 1] != '\n')) {
            text[pos++] = '\n';
        }
    }
    text[pos] = '\0';

    AstNode *node = xr_ast_global_asm(parser->compiler_session, text, line);
    node->column = column;
    node->end_line = close.line;
    node->end_column = close.column + 1;
    return node;
}

static bool xr_derive_target_bit(Token token, uint32_t *bit_out) {
    if (token.length == 7 && memcmp(token.start, "Inspect", 7) == 0) {
        *bit_out = XR_DERIVE_INSPECT;
        return true;
    }
    if (token.length == 4 && memcmp(token.start, "Json", 4) == 0) {
        *bit_out = XR_DERIVE_JSON;
        return true;
    }
    if (token.length == 2 && memcmp(token.start, "Eq", 2) == 0) {
        *bit_out = XR_DERIVE_EQ;
        return true;
    }
    if (token.length == 4 && memcmp(token.start, "Hash", 4) == 0) {
        *bit_out = XR_DERIVE_HASH;
        return true;
    }
    if (token.length == 5 && memcmp(token.start, "Clone", 5) == 0) {
        *bit_out = XR_DERIVE_CLONE;
        return true;
    }
    return false;
}

// Parse single attribute: @test, @test(skip), @test(timeout: 30), etc.
XrAttribute *xr_parse_single_attribute(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_single_attribute: NULL parser");
    xr_parser_advance(parser);  // Consume @

    xr_parser_consume(parser, TK_NAME, "expected attribute name");
    Token name_token = parser->previous;

    XrAttribute *attr = (XrAttribute *) ast_alloc(parser->compiler_session, sizeof(XrAttribute));
    attr->kind = ATTR_NONE;
    attr->timeout = 0;
    attr->str_arg = NULL;
    attr->derive_flags = 0;
    if (name_token.length == 4 && memcmp(name_token.start, "test", 4) == 0) {
        attr->kind = ATTR_TEST;

        // Check for params: @test(skip) or @test(timeout: 30)
        if (xr_parser_match(parser, TK_LPAREN)) {
            if (xr_parser_check(parser, TK_NAME)) {
                Token param = parser->current;
                xr_parser_advance(parser);

                if (param.length == 4 && memcmp(param.start, "skip", 4) == 0) {
                    attr->kind = ATTR_TEST_SKIP;
                } else if (param.length == 7 && memcmp(param.start, "timeout", 7) == 0) {
                    attr->kind = ATTR_TEST_TIMEOUT;
                    xr_parser_consume(parser, TK_COLON, "expected ':' after timeout");
                    xr_parser_consume(parser, TK_LITERAL_INT, "expected timeout seconds");
                    Token timeout_token = parser->previous;
                    char buf[32];
                    int len = timeout_token.length < 31 ? timeout_token.length : 31;
                    memcpy(buf, timeout_token.start, len);
                    buf[len] = '\0';
                    attr->timeout = atoi(buf);
                }
            }
            xr_parser_consume(parser, TK_RPAREN, "expected ')' to close attribute params");
        }
    } else if (name_token.length == 11 && memcmp(name_token.start, "before_each", 11) == 0) {
        attr->kind = ATTR_BEFORE_EACH;
    } else if (name_token.length == 10 && memcmp(name_token.start, "after_each", 10) == 0) {
        attr->kind = ATTR_AFTER_EACH;
    } else if (name_token.length == 10 && memcmp(name_token.start, "before_all", 10) == 0) {
        attr->kind = ATTR_BEFORE_ALL;
    } else if (name_token.length == 9 && memcmp(name_token.start, "after_all", 9) == 0) {
        attr->kind = ATTR_AFTER_ALL;
    } else if (name_token.length == 6 && memcmp(name_token.start, "native", 6) == 0) {
        attr->kind = ATTR_NATIVE;
    } else if (name_token.length == 10 && memcmp(name_token.start, "deprecated", 10) == 0) {
        attr->kind = ATTR_DEPRECATED;
        // Optional message: @deprecated("use X instead")
        if (xr_parser_match(parser, TK_LPAREN)) {
            // Consume and ignore the message string for now
            if (xr_parser_check(parser, TK_LITERAL_STRING)) {
                xr_parser_advance(parser);
            }
            xr_parser_consume(parser, TK_RPAREN, "expected ')' to close @deprecated");
        }
    } else if (name_token.length == 6 && memcmp(name_token.start, "extern", 6) == 0) {
        xr_parser_error(parser, "@extern was removed; use extern \"C\" { fn name(...) -> Type }");
        attr->kind = ATTR_EXTERN;
        attr->str_arg = "C";
        if (xr_parser_match(parser, TK_LPAREN)) {
            if (xr_parser_check(parser, TK_LITERAL_STRING)) {
                xr_parser_advance(parser);
                attr->str_arg = xr_attr_string_arg(parser);
            }
            xr_parser_consume(parser, TK_RPAREN, "expected ')' after removed @extern syntax");
        }
    } else if (name_token.length == 5 && memcmp(name_token.start, "dylib", 5) == 0) {
        xr_parser_error(parser, "@dylib was removed; use extern \"C\" dylib(\"name\") { ... }");
        attr->kind = ATTR_DYLIB;
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after removed @dylib syntax");
        if (xr_parser_check(parser, TK_LITERAL_STRING)) {
            xr_parser_advance(parser);
            attr->str_arg = xr_attr_string_arg(parser);
        } else {
            xr_parser_error(parser, "removed @dylib syntax requires a library name string");
        }
        xr_parser_consume(parser, TK_RPAREN, "expected ')' after removed @dylib syntax");
    } else if (name_token.length == 9 && memcmp(name_token.start, "link_name", 9) == 0) {
        attr->kind = ATTR_LINK_NAME;
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after @link_name");
        if (xr_parser_check(parser, TK_LITERAL_STRING)) {
            xr_parser_advance(parser);
            attr->str_arg = xr_attr_string_arg(parser);
        } else {
            xr_parser_error(parser, "@link_name requires a C symbol string");
        }
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to close @link_name");
    } else if (name_token.length == 8 && memcmp(name_token.start, "c_export", 8) == 0) {
        // @c_export("name") — expose a top-level function through an AOT C ABI
        // wrapper. The exported symbol is open text, so it uses a string arg.
        attr->kind = ATTR_C_EXPORT;
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after @c_export");
        if (xr_parser_check(parser, TK_LITERAL_STRING)) {
            xr_parser_advance(parser);
            attr->str_arg = xr_attr_string_arg(parser);
        } else {
            xr_parser_error(parser,
                            "@c_export requires a C symbol string, e.g. @c_export(\"xray_add\")");
        }
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to close @c_export");
    } else if (name_token.length == 7 && memcmp(name_token.start, "section", 7) == 0) {
        // @section("name") — place a module-level AOT function/C export or
        // freestanding static data object into a named linker section. The
        // section name is open text, so it uses a string arg just like
        // @c_export.
        attr->kind = ATTR_SECTION;
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after @section");
        if (xr_parser_check(parser, TK_LITERAL_STRING)) {
            xr_parser_advance(parser);
            attr->str_arg = xr_attr_string_arg(parser);
        } else {
            xr_parser_error(
                parser, "@section requires a section name string, e.g. @section(\".text.boot\")");
        }
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to close @section");
    } else if (name_token.length == 4 && memcmp(name_token.start, "weak", 4) == 0) {
        attr->kind = ATTR_WEAK;
    } else if (name_token.length == 4 && memcmp(name_token.start, "used", 4) == 0) {
        attr->kind = ATTR_USED;
    } else if (name_token.length == 5 && memcmp(name_token.start, "naked", 5) == 0) {
        attr->kind = ATTR_NAKED;
    } else if (name_token.length == 9 && memcmp(name_token.start, "interrupt", 9) == 0) {
        attr->kind = ATTR_INTERRUPT;
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after @interrupt");
        if (xr_parser_check(parser, TK_LITERAL_STRING)) {
            xr_parser_advance(parser);
            attr->str_arg = xr_attr_string_arg(parser);
        } else {
            xr_parser_error(parser, "@interrupt requires an ABI string, e.g. @interrupt(\"irq\")");
        }
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to close @interrupt");
    } else if (name_token.length == 8 && memcmp(name_token.start, "no_alloc", 8) == 0) {
        attr->kind = ATTR_NO_ALLOC;
    } else if (name_token.length == 9 && memcmp(name_token.start, "intrinsic", 9) == 0) {
        attr->kind = ATTR_INTRINSIC;
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after @intrinsic");
        if (xr_parser_check(parser, TK_LITERAL_STRING)) {
            xr_parser_advance(parser);
            attr->str_arg = xr_attr_string_arg(parser);
        } else {
            xr_parser_error(parser, "@intrinsic requires a canonical identity string");
        }
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to close @intrinsic");
    } else if (name_token.length == 6 && memcmp(name_token.start, "derive", 6) == 0) {
        attr->kind = ATTR_DERIVE;
        xr_parser_consume(parser, TK_LPAREN, "expected '(' after @derive");
        if (xr_parser_check(parser, TK_RPAREN)) {
            xr_parser_error(
                parser, "@derive requires at least one target: Inspect, Json, Eq, Hash, or Clone");
        } else {
            uint32_t seen_flags = 0;
            do {
                xr_parser_consume(parser, TK_NAME, "expected derive target name");
                Token target = parser->previous;
                uint32_t bit = 0;
                char name_buf[64];
                int n = target.length < (int) sizeof(name_buf) - 1 ? target.length
                                                                   : (int) sizeof(name_buf) - 1;
                memcpy(name_buf, target.start, (size_t) n);
                name_buf[n] = '\0';
                if (!xr_derive_target_bit(target, &bit)) {
                    char msg[160];
                    snprintf(msg, sizeof(msg),
                             "unknown derive target '%s'; expected Inspect, Json, "
                             "Eq, Hash, or Clone",
                             name_buf);
                    xr_parser_error(parser, msg);
                } else if ((seen_flags & bit) != 0) {
                    char msg[160];
                    snprintf(msg, sizeof(msg), "duplicate derive target '%s'", name_buf);
                    xr_parser_error(parser, msg);
                } else {
                    seen_flags |= bit;
                    attr->derive_flags |= bit;
                }
            } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RPAREN));
        }
        xr_parser_consume(parser, TK_RPAREN, "expected ')' to close @derive");
    } else {
        xr_parser_error(parser, "unknown attribute name");
        return NULL;
    }

    return attr;
}

// Check if any attribute in the list has the given kind.
static bool attrs_has(XrAttribute **attrs, int count, AttributeKind kind) {
    for (int i = 0; i < count; i++) {
        if (attrs[i] && attrs[i]->kind == kind)
            return true;
    }
    return false;
}

static uint32_t attrs_derive_flags(XrAttribute **attrs, int count) {
    uint32_t flags = 0;
    for (int i = 0; i < count; i++) {
        if (attrs[i] && attrs[i]->kind == ATTR_DERIVE)
            flags |= attrs[i]->derive_flags;
    }
    return flags;
}

static void validate_decl_derive_contract(Parser *parser, uint32_t derive_flags) {
    if ((derive_flags & XR_DERIVE_HASH) != 0 && (derive_flags & XR_DERIVE_EQ) == 0)
        xr_parser_error_at_previous(parser, "@derive(Hash) requires Eq in the same @derive(...)");
}

static bool attrs_has_symbol_layout_attr(XrAttribute **attrs, int count) {
    for (int i = 0; i < count; i++) {
        if (!attrs[i])
            continue;
        switch (attrs[i]->kind) {
            case ATTR_SECTION:
            case ATTR_WEAK:
            case ATTR_USED:
                return true;
            default:
                break;
        }
    }
    return false;
}

static bool attrs_are_static_data_attrs(XrAttribute **attrs, int count) {
    if (count <= 0)
        return false;
    for (int i = 0; i < count; i++) {
        if (!attrs[i])
            continue;
        switch (attrs[i]->kind) {
            case ATTR_SECTION:
            case ATTR_WEAK:
            case ATTR_USED:
                break;
            default:
                return false;
        }
    }
    return true;
}

static AstNode *mark_direct_visibility(AstNode *declaration, bool is_exported) {
    if (declaration)
        declaration->is_exported = is_exported;
    return declaration;
}

// Parse attributed declaration: @test fn ..., @native class ..., etc.
static AstNode *xr_parse_attributed_declaration(Parser *parser) {
    XrAttribute **attributes = NULL;
    int attr_count = 0;
    int attr_capacity = 0;

    while (xr_parser_check(parser, TK_AT)) {
        XrAttribute *attr = xr_parse_single_attribute(parser);
        if (!attr)
            return NULL;
        XR_PARSE_PUSH(parser, attributes, attr_count, attr_capacity, attr);
    }

    bool is_exported = xr_parser_match(parser, TK_EXPORT);
    if (is_exported && parser->scope_depth > 0) {
        xr_parser_error(parser, "'export' must appear at module top level");
        return NULL;
    }

    bool is_native = attrs_has(attributes, attr_count, ATTR_NATIVE);
    bool is_c_export = attrs_has(attributes, attr_count, ATTR_C_EXPORT);
    bool is_naked = attrs_has(attributes, attr_count, ATTR_NAKED);
    bool is_interrupt = attrs_has(attributes, attr_count, ATTR_INTERRUPT);
    bool is_no_alloc = attrs_has(attributes, attr_count, ATTR_NO_ALLOC);
    bool has_symbol_layout_attr = attrs_has_symbol_layout_attr(attributes, attr_count);
    uint32_t derive_flags = attrs_derive_flags(attributes, attr_count);
    if (is_c_export && parser->scope_depth > 0) {
        xr_parser_error(parser, "@c_export can only annotate a module-level function");
        return NULL;
    }
    if (has_symbol_layout_attr && parser->scope_depth > 0) {
        xr_parser_error(parser,
                        "@section/@weak/@used can only annotate a module-level function or const "
                        "data declaration");
        return NULL;
    }
    if (is_no_alloc && !xr_parser_check(parser, TK_FN)) {
        xr_parser_error(parser, "@no_alloc can only annotate a function");
        return NULL;
    }
    if (is_naked && !xr_parser_check(parser, TK_FN)) {
        xr_parser_error(parser, "@naked can only annotate a function");
        return NULL;
    }
    if (is_naked && parser->scope_depth > 0) {
        xr_parser_error(parser, "@naked can only annotate a module-level function");
        return NULL;
    }
    if (is_interrupt && !xr_parser_check(parser, TK_FN)) {
        xr_parser_error(parser, "@interrupt can only annotate a function");
        return NULL;
    }
    if (is_interrupt && parser->scope_depth > 0) {
        xr_parser_error(parser, "@interrupt can only annotate a module-level function");
        return NULL;
    }

    // @native class / @native final class
    if (xr_parser_check(parser, TK_CLASS) || xr_parser_check(parser, TK_FINAL)) {
        if (is_c_export) {
            xr_parser_error(parser, "@c_export can only annotate a module-level function");
            return NULL;
        }
        if (has_symbol_layout_attr) {
            xr_parser_error(parser, "@section/@weak/@used can only annotate a function");
            return NULL;
        }
        bool explicit_final = xr_parser_match(parser, TK_FINAL);
        if (explicit_final) {
            if (!xr_parser_match(parser, TK_CLASS)) {
                xr_parser_error_at_current(parser, "expected 'class' after 'final'");
                return NULL;
            }
        } else {
            xr_parser_advance(parser);  // consume TK_CLASS
        }
        // Set flag so method body parsing is skipped for @native classes
        parser->parsing_native_class = is_native;
        AstNode *cls = xr_parse_class_declaration(parser);
        parser->parsing_native_class = false;
        if (!cls)
            return NULL;
        cls->as.class_decl.explicit_final = explicit_final;
        cls->as.class_decl.is_native = is_native;
        cls->as.class_decl.attributes = attributes;
        cls->as.class_decl.attr_count = attr_count;
        validate_decl_derive_contract(parser, derive_flags);
        return mark_direct_visibility(cls, is_exported);
    }

    // @native struct / @native packed struct
    bool is_packed_struct = false;
    if (xr_parser_match(parser, TK_PACKED)) {
        is_packed_struct = true;
        xr_parser_consume(parser, TK_STRUCT, "expected 'struct' after 'packed'");
    }
    if (is_packed_struct || xr_parser_match(parser, TK_STRUCT)) {
        if (is_c_export) {
            xr_parser_error(parser, "@c_export can only annotate a module-level function");
            return NULL;
        }
        if (has_symbol_layout_attr) {
            xr_parser_error(parser, "@section/@weak/@used can only annotate a function");
            return NULL;
        }
        parser->parsing_native_class = is_native;
        AstNode *st = xr_parse_struct_declaration(parser);
        parser->parsing_native_class = false;
        if (!st)
            return NULL;
        st->as.struct_decl.is_packed = is_packed_struct;
        st->as.class_decl.is_native = is_native;
        st->as.class_decl.attributes = attributes;
        st->as.class_decl.attr_count = attr_count;
        validate_decl_derive_contract(parser, derive_flags);
        return mark_direct_visibility(st, is_exported);
    }

    if (xr_parser_match(parser, TK_UNION)) {
        if (derive_flags != 0) {
            xr_parser_error(parser, "@derive can only annotate class, struct, or enum");
            return NULL;
        }
        if (is_c_export) {
            xr_parser_error(parser, "@c_export can only annotate a module-level function");
            return NULL;
        }
        if (has_symbol_layout_attr) {
            xr_parser_error(parser, "@section/@weak/@used can only annotate a function");
            return NULL;
        }
        AstNode *un = xr_parse_union_declaration(parser);
        if (!un)
            return NULL;
        un->as.union_decl.is_native = is_native;
        un->as.union_decl.attributes = attributes;
        un->as.union_decl.attr_count = attr_count;
        return mark_direct_visibility(un, is_exported);
    }

    if (xr_parser_match(parser, TK_ENUM)) {
        if (is_native) {
            xr_parser_error(parser, "@native can only annotate class, struct, union, or function");
            return NULL;
        }
        if (is_c_export) {
            xr_parser_error(parser, "@c_export can only annotate a module-level function");
            return NULL;
        }
        if (has_symbol_layout_attr) {
            xr_parser_error(parser, "@section/@weak/@used can only annotate a function");
            return NULL;
        }
        AstNode *en = xr_parse_enum_declaration(parser);
        if (!en)
            return NULL;
        en->as.enum_decl.attributes = attributes;
        en->as.enum_decl.attr_count = attr_count;
        validate_decl_derive_contract(parser, derive_flags);
        return mark_direct_visibility(en, is_exported);
    }

    if (xr_parser_match(parser, TK_CONST)) {
        if (!attrs_are_static_data_attrs(attributes, attr_count)) {
            xr_parser_error(
                parser,
                "attributes can only annotate declaration items; use 'fn name(...)' for function "
                "item attributes");
            return NULL;
        }
        if (xr_parser_check(parser, TK_LBRACKET) || xr_parser_check(parser, TK_LBRACE) ||
            xr_parser_check(parser, TK_LPAREN)) {
            xr_parser_error(parser, "@section/@weak/@used require a single named const binding");
            return NULL;
        }
        AstNode *decl = xr_parse_single_var_declaration(parser, 1);
        if (!decl)
            return NULL;
        decl->as.var_decl.attributes = attributes;
        decl->as.var_decl.attr_count = attr_count;
        return mark_direct_visibility(decl, is_exported);
    }

    if (xr_parser_match(parser, TK_VAR)) {
        if (is_exported) {
            xr_parser_error(parser, "mutable export is not supported; use 'export const' instead");
            return NULL;
        }
        if (!attrs_are_static_data_attrs(attributes, attr_count)) {
            xr_parser_error(
                parser,
                "attributes can only annotate declaration items; use 'fn name(...)' for function "
                "item attributes");
            return NULL;
        }
        if (xr_parser_check(parser, TK_LBRACKET) || xr_parser_check(parser, TK_LBRACE) ||
            xr_parser_check(parser, TK_LPAREN)) {
            xr_parser_error(parser, "@section/@weak/@used require a single named var binding");
            return NULL;
        }
        AstNode *decl = xr_parse_single_var_declaration(parser, 0);
        if (!decl)
            return NULL;
        decl->as.var_decl.attributes = attributes;
        decl->as.var_decl.attr_count = attr_count;
        return mark_direct_visibility(decl, false);
    }

    if (xr_parser_check(parser, TK_SHARED)) {
        if (is_exported) {
            xr_parser_error(parser, "attributes cannot annotate exported shared module storage");
            return NULL;
        }
        xr_parser_error(parser,
                        "attributes cannot annotate shared module storage; use const data or a "
                        "freestanding aggregate var static object");
        return NULL;
    }

    if (xr_parser_check(parser, TK_OWNED)) {
        xr_parser_error(parser, "attributes cannot annotate owned bindings");
        return NULL;
    }

    // @test fn ..., @native fn ..., @c_export("sym") fn ...
    if (xr_parser_match(parser, TK_FN)) {
        if (derive_flags != 0) {
            xr_parser_error(parser, "@derive can only annotate class, struct, or enum");
            return NULL;
        }
        bool saved_extern_context = parser->parsing_extern_fn;
        bool is_extern = saved_extern_context || attrs_has(attributes, attr_count, ATTR_EXTERN);
        parser->parsing_extern_fn = is_extern;
        AstNode *func = xr_parse_function_declaration(parser);
        parser->parsing_extern_fn = saved_extern_context;
        if (!func)
            return NULL;
        func->as.function_decl.attributes = attributes;
        func->as.function_decl.attr_count = attr_count;
        return mark_direct_visibility(func, is_exported);
    }

    xr_parser_error_at_current(
        parser,
        "attributes can only annotate declaration items; use 'fn name(...)' for function item "
        "attributes");
    return NULL;
}

static XrAttribute *xr_make_string_attribute(Parser *parser, AttributeKind kind,
                                             const char *value) {
    XrAttribute *attr = (XrAttribute *) ast_alloc(parser->compiler_session, sizeof(XrAttribute));
    attr->kind = kind;
    attr->timeout = 0;
    attr->str_arg = value;
    attr->derive_flags = 0;
    return attr;
}

static void xr_function_append_attribute(Parser *parser, AstNode *node, XrAttribute *attr) {
    XR_DCHECK(node && node->type == AST_FUNCTION_DECL,
              "function_append_attribute: expected function declaration");
    FunctionDeclNode *fn = &node->as.function_decl;
    XrAttribute **attrs = (XrAttribute **) ast_alloc_array(
        parser->compiler_session, sizeof(XrAttribute *), (size_t) fn->attr_count + 1);
    if (fn->attr_count > 0 && fn->attributes)
        memcpy(attrs, fn->attributes, sizeof(XrAttribute *) * (size_t) fn->attr_count);
    attrs[fn->attr_count++] = attr;
    fn->attributes = attrs;
}

/* Parse the canonical foreign-declaration surface:
 *
 *   extern "C" dylib("m") {
 *       fn cos(x: float64) -> float64
 *       struct Header { tag: uint8 size: uint32 }
 *   }
 *
 * `link("m")` shares the existing descriptor field with `dylib("m")`:
 * AOT already distinguishes a link name from a concrete library path, while
 * VM resolves both through the same typed FFI descriptor.  The transient
 * AST_PROGRAM is flattened by xr_ast_program_add. */
static AstNode *xr_parse_extern_block_declaration(Parser *parser) {
    int line = parser->previous.line;
    if (parser->scope_depth > 0) {
        xr_parser_error(parser, "extern blocks must appear at module top level");
        return NULL;
    }

    xr_parser_consume(parser, TK_LITERAL_STRING,
                      "expected ABI string after extern, e.g. extern \"C\"");
    const char *abi = xr_attr_string_arg(parser);
    if (strcmp(abi, "C") != 0)
        xr_parser_error(parser, "only extern \"C\" is supported");

    const char *library = NULL;
    bool library_is_link = false;
    if (xr_parser_check(parser, TK_NAME)) {
        bool is_dylib = xr_parser_check_name(parser, "dylib");
        bool is_link = xr_parser_check_name(parser, "link");
        if (is_dylib || is_link) {
            library_is_link = is_link;
            xr_parser_advance(parser);
            xr_parser_consume(parser, TK_LPAREN,
                              is_dylib ? "expected '(' after dylib" : "expected '(' after link");
            xr_parser_consume(parser, TK_LITERAL_STRING,
                              is_dylib ? "dylib requires a library string"
                                       : "link requires a library string");
            library = xr_attr_string_arg(parser);
            xr_parser_consume(parser, TK_RPAREN,
                              is_dylib ? "expected ')' after dylib" : "expected ')' after link");
        }
    }

    xr_parser_consume(parser, TK_LBRACE, "expected '{' to open extern block");
    AstNode *group = xr_ast_program(parser->compiler_session);
    XrAttribute *extern_attr = xr_make_string_attribute(parser, ATTR_EXTERN, abi);
    XrAttribute *library_attr =
        library
            ? xr_make_string_attribute(parser, library_is_link ? ATTR_LINK : ATTR_DYLIB, library)
            : NULL;

    while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
        bool saved_extern_context = parser->parsing_extern_fn;
        parser->parsing_extern_fn = true;

        AstNode *decl = NULL;
        bool is_extern_layout = false;
        bool is_exported = xr_parser_match(parser, TK_EXPORT);
        if (is_exported && xr_parser_check(parser, TK_AT)) {
            xr_parser_error_at_current(parser,
                                       "attributes must appear before 'export' in a declaration");
            return NULL;
        }
        if (xr_parser_check(parser, TK_AT)) {
            decl = xr_parse_attributed_declaration(parser);
        } else if (xr_parser_match(parser, TK_FN)) {
            decl = xr_parse_function_declaration(parser);
        } else if (xr_parser_match(parser, TK_STRUCT)) {
            decl = xr_parse_struct_declaration(parser);
            is_extern_layout = true;
        } else if (xr_parser_match(parser, TK_UNION)) {
            decl = xr_parse_union_declaration(parser);
            is_extern_layout = true;
        } else if (xr_parser_match(parser, TK_PACKED)) {
            xr_parser_consume(parser, TK_STRUCT,
                              "expected 'struct' after 'packed' in extern block");
            decl = xr_parse_struct_declaration(parser);
            if (decl && decl->type == AST_STRUCT_DECL)
                decl->as.struct_decl.is_packed = true;
            is_extern_layout = true;
        } else {
            xr_parser_error_at_current(
                parser, "extern blocks contain fn, struct, union, or packed struct declarations");
        }
        parser->parsing_extern_fn = saved_extern_context;

        if (!decl)
            return NULL;
        if (is_exported)
            decl->is_exported = true;
        if (decl->type == AST_FUNCTION_DECL) {
            if (xr_parser_check(parser, TK_LBRACE)) {
                xr_parser_error_at_current(
                    parser, "extern function declarations cannot have a function body");
                return NULL;
            }
            xr_function_append_attribute(parser, decl, extern_attr);
            if (library_attr)
                xr_function_append_attribute(parser, decl, library_attr);
        } else if (is_extern_layout && decl->type == AST_STRUCT_DECL) {
            decl->as.struct_decl.is_extern_layout = true;
        } else if (is_extern_layout && decl->type == AST_UNION_DECL) {
            decl->as.union_decl.is_extern_layout = true;
        } else {
            xr_parser_error_at_current(
                parser, "extern blocks contain fn, struct, union, or packed struct declarations");
            return NULL;
        }
        xr_ast_program_add(parser->compiler_session, group, decl);
        xr_parser_match(parser, TK_SEMICOLON);
    }

    xr_parser_consume(parser, TK_RBRACE, "expected '}' to close extern block");
    if (group->as.program.count == 0)
        xr_parser_error(parser, "extern block requires at least one declaration");
    group->line = line;
    group->end_line = parser->previous.line;
    group->end_column = parser->previous.column + 1;
    return group;
}

// Parse function declaration: fn add(a, b) { return a + b }
AstNode *xr_parse_function_declaration(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_function_declaration: NULL parser");
    int line = parser->previous.line;

    xr_parser_consume(parser, TK_NAME, "expected function name");
    Token name_token = parser->previous;
    int name_column = name_token.column;

    char *func_name = (char *) ast_alloc(parser->compiler_session, (size_t) name_token.length + 1);
    memcpy(func_name, name_token.start, name_token.length);
    func_name[name_token.length] = '\0';

    // Parse generic type params <T: Constraint, U>
    XrGenericParam **type_params = NULL;
    int type_param_count = 0;
    int type_param_capacity = 0;

    if (xr_parser_match(parser, TK_LT)) {
        do {
            xr_parser_consume(parser, TK_NAME, "expected type parameter name");
            Token param_token = parser->previous;

            char *param_name =
                (char *) ast_alloc(parser->compiler_session, (size_t) param_token.length + 1);
            memcpy(param_name, param_token.start, param_token.length);
            param_name[param_token.length] = '\0';

            // Parse optional intersection constraint <T: Interface1 & Interface2 & ...>
            XrTypeRef **constraints = NULL;
            int constraint_count = 0;
            if (xr_parser_match(parser, TK_COLON)) {
                constraints = xr_parse_constraint_list(parser, &constraint_count);
            }

            XrGenericParam *gp =
                (XrGenericParam *) ast_alloc(parser->compiler_session, sizeof(XrGenericParam));
            gp->name = param_name;
            gp->constraints = constraints;
            gp->constraint_count = constraint_count;
            XR_PARSE_PUSH(parser, type_params, type_param_count, type_param_capacity, gp);

        } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_GT));

        xr_parser_consume(parser, TK_GT, "expected '>' to close generic params");
    }

    // Register generic type params in type_scope for type annotation parsing
    // This allows T in "fn identity<T>(x: T): T" to be recognised as type param.
    XrTypeScope *saved_scope = parser->type_scope;
    if (type_param_count > 0) {
        XrTypeScope *generic_scope = xr_type_scope_new(parser->type_scope);
        for (int i = 0; i < type_param_count; i++) {
            XrTypeRef *type_param =
                xr_tref_type_param(parser->compiler_session, type_params[i]->name);
            xr_type_scope_define(generic_scope, type_params[i]->name, type_param);
        }
        parser->type_scope = generic_scope;
    }

    xr_parser_consume(parser, TK_LPAREN, "expected '(' after function name");

    XrParamNode **params = NULL;
    int param_count = 0;
    int param_capacity = 0;
    int required_count = 0;
    bool seen_default = false;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            if (param_count >= param_capacity) {
                int _old_cap_param_capacity = (int) param_capacity;
                param_capacity = _old_cap_param_capacity == 0 ? 4 : _old_cap_param_capacity * 2;
                XrParamNode **_new_params = (XrParamNode **) ast_alloc_array(
                    parser->compiler_session, sizeof(XrParamNode *), (size_t) param_capacity);
                if (_old_cap_param_capacity > 0 && params)
                    memcpy(_new_params, params,
                           sizeof(XrParamNode *) * (size_t) _old_cap_param_capacity);
                params = _new_params;
            }

            XrParamNode *param = xr_parse_parameter_at(parser,
                                                       XR_PARSE_PARAMETER_ALLOW_MODE |
                                                           XR_PARSE_PARAMETER_ALLOW_REST |
                                                           XR_PARSE_PARAMETER_ALLOW_DESTRUCTURE,
                                                       param_count);
            if (!param)
                goto fail;

            if (param->is_rest) {
                params[param_count++] = param;
                if (xr_parser_check(parser, TK_COMMA)) {
                    xr_parser_error(parser, "rest parameter must be last");
                    goto fail;
                }
                break;
            }

            if (param->pattern) {
                params[param_count++] = param;
                required_count++;
            } else {
                // Parse optional default value
                if (xr_parser_match(parser, TK_ASSIGN)) {
                    xr_parse_reject_ref_out_default_param(parser, param);
                    param->default_value = xr_parse_expression(parser);
                    seen_default = true;

                    // Infer type from default value if no explicit type annotation
                    if (param->type == NULL && param->default_value != NULL) {
                        AstNode *dv = param->default_value;
                        switch (dv->type) {
                            case AST_LITERAL_INT:
                                param->type = xr_tref_int(parser->compiler_session);
                                break;
                            case AST_LITERAL_FLOAT:
                                param->type = xr_tref_float(parser->compiler_session);
                                break;
                            case AST_LITERAL_STRING:
                            case AST_TEMPLATE_STRING:
                                param->type = xr_tref_string(parser->compiler_session);
                                break;
                            case AST_LITERAL_TRUE:
                            case AST_LITERAL_FALSE:
                                param->type = xr_tref_bool(parser->compiler_session);
                                break;
                            case AST_ARRAY_LITERAL:
                                param->type = xr_tref_named(parser->compiler_session, "Array");
                                break;
                            case AST_MAP_LITERAL:
                                param->type = xr_tref_named(parser->compiler_session, "Map");
                                break;
                            case AST_SET_LITERAL:
                                param->type = xr_tref_named(parser->compiler_session, "Set");
                                break;
                            case AST_OBJECT_LITERAL:
                                param->type = xr_tref_named(parser->compiler_session, "Json");
                                break;
                            default:
                                break;
                        }
                    }
                } else if (seen_default) {
                    xr_parser_error(parser, "required parameter cannot follow optional parameter");
                    params[param_count++] = param;
                    goto fail;
                } else {
                    required_count++;
                }

                params[param_count++] = param;
            }

        } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RPAREN));
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')' after parameter list");

    // Parse optional return type annotation: `fn foo(...) -> T { ... }`.
    // The unified arrow `->` is the only legal separator.
    XrTypeRef *return_type = NULL;
    if (xr_parser_match(parser, TK_ARROW)) {
        return_type = xr_parse_type_annotation(parser);
    } else if (xr_parser_check(parser, TK_COLON)) {
        // Legacy syntax `fn foo(): T` is no longer accepted. Emit a clear
        // migration hint and recover by parsing the type so the rest of
        // the function still parses.
        xr_parser_advance(parser);  // consume ':'
        xr_parser_error(parser, "use '->' instead of ':' for function return type, "
                                "e.g. fn foo() -> int");
        parser->panic_mode = 0;
        return_type = xr_parse_type_annotation(parser);
    }

    // Parse function body. Functions declared in an extern block are bodyless: the implementation
    // lives in a foreign C library, so the signature stands alone (optionally
    // followed by a `;`). All other functions require a `{ }` block.
    AstNode *body = NULL;
    if (parser->parsing_extern_fn) {
        xr_parser_match(parser, TK_SEMICOLON);  // optional trailing ';'
    } else {
        xr_parser_consume(parser, TK_LBRACE, "function body must use braces { }");
        parser->scope_depth++;
        body = xr_parse_block(parser);
        parser->scope_depth--;
    }

    // If has destructure params, insert destructure code at function body start
    for (int i = 0; body && i < param_count; i++) {
        if (params[i] && params[i]->pattern != NULL) {
            AstNode *param_var = xr_ast_variable(parser->compiler_session, params[i]->name, line);

            // Create destructure decl: var [x, y] = __param0
            AstNode *destructure_decl = xr_ast_destructure_decl(
                parser->compiler_session, params[i]->pattern, param_var, false, line);
            // Don't free pattern here, it's now owned by destructure_decl
            params[i]->pattern = NULL;

            BlockNode *block = &body->as.block;
            if (block->count >= block->capacity) {
                int _old_cap_block__capacity = (int) block->capacity;
                block->capacity = _old_cap_block__capacity == 0 ? 4 : _old_cap_block__capacity * 2;
                AstNode **_new_block_statements = (AstNode **) ast_alloc_array(
                    parser->compiler_session, sizeof(AstNode *), (size_t) block->capacity);
                if (_old_cap_block__capacity > 0 && block->statements)
                    memcpy(_new_block_statements, block->statements,
                           sizeof(AstNode *) * (size_t) _old_cap_block__capacity);
                block->statements = _new_block_statements;
            }

            // Shift existing statements back
            for (int j = block->count; j > 0; j--) {
                block->statements[j] = block->statements[j - 1];
            }

            block->statements[0] = destructure_decl;
            block->count++;
        }
    }

    AstNode *func_decl =
        xr_ast_function_decl(parser->compiler_session, func_name, params, param_count, body, line);
    func_decl->column = name_column;
    if (body) {
        func_decl->end_line = body->end_line;
        func_decl->end_column = body->end_column;
    } else {
        func_decl->end_line = parser->previous.line;
        func_decl->end_column = parser->previous.column;
    }

    func_decl->as.function_decl.return_type = return_type;
    func_decl->as.function_decl.required_count = required_count;
    func_decl->as.function_decl.type_params = type_params;
    func_decl->as.function_decl.type_param_count = type_param_count;

    // Restore original type_scope after parsing generic function
    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
    }

    return func_decl;

fail:
    // Local parser allocations are arena-owned and released at parse end.
    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
    }
    return NULL;
}

// Parse one call argument, optionally a spread `...expr`. The spread
// source must be a tuple value; the analyzer expands its static arity
// into individual positional arguments.
//
// A bare `_` argument is accepted as a wildcard placeholder so that
// ADT pattern parsing (`R.Err(_)`) can later detect it; in normal call
// position the analyzer rejects it.
static bool xr_parse_call_argument_access_marker_starts(Parser *parser,
                                                        XrCallArgAccess *out_access) {
    if (!parser)
        return false;
    XrCallArgAccess access = XR_CALL_ARG_VALUE;
    if (xr_parser_check(parser, TK_REF) || xr_parser_check_name(parser, "ref")) {
        access = XR_CALL_ARG_REF;
    } else if (xr_parser_check_name(parser, "out")) {
        access = XR_CALL_ARG_OUT;
    } else {
        return false;
    }

    Scanner saved_scan = parser->scanner;
    Token saved_cur = parser->current;
    Token saved_prev = parser->previous;
    xr_parser_advance(parser);
    bool marker = xr_parser_check(parser, TK_NAME) || xr_parser_check(parser, TK_THIS);
    parser->scanner = saved_scan;
    parser->current = saved_cur;
    parser->previous = saved_prev;
    if (marker && out_access)
        *out_access = access;
    return marker;
}

static bool xr_parse_call_argument_in_marker_removed_starts(Parser *parser) {
    if (!parser || !xr_parser_check(parser, TK_IN))
        return false;

    Scanner saved_scan = parser->scanner;
    Token saved_cur = parser->current;
    Token saved_prev = parser->previous;
    xr_parser_advance(parser);
    bool marker = xr_parser_check(parser, TK_NAME) || xr_parser_check(parser, TK_THIS);
    parser->scanner = saved_scan;
    parser->current = saved_cur;
    parser->previous = saved_prev;
    return marker;
}

AstNode *xr_parse_call_argument_with_access(Parser *parser, XrCallArgAccess *out_access) {
    if (out_access)
        *out_access = XR_CALL_ARG_VALUE;
    int line = parser->current.line;
    XrCallArgAccess marker_access = XR_CALL_ARG_VALUE;
    if (xr_parse_call_argument_access_marker_starts(parser, &marker_access)) {
        xr_parser_advance(parser);
        if (out_access)
            *out_access = marker_access;
        AstNode *place = xr_parse_expression(parser);
        if (!place)
            return NULL;
        return place;
    }
    if (xr_parse_call_argument_in_marker_removed_starts(parser)) {
        Token in_token = parser->current;
        xr_parser_advance(parser);
        xr_parser_emit_removed_syntax(
            parser, &in_token, XR_ERR_SYN_CALL_IN_MARKER_REMOVED,
            "call-site `in` marker was removed",
            "Pass the argument directly, for example `f(value)`; `in` is a declaration-side "
            "parameter mode only.");
        return xr_parse_expression(parser);
    }
    if (xr_parser_match(parser, TK_DOT_DOT_DOT)) {
        AstNode *inner = xr_parse_expression(parser);
        if (!inner)
            return NULL;
        return xr_ast_spread_expr(parser->compiler_session, inner, line);
    }
    if (xr_parser_match(parser, TK_UNDERSCORE)) {
        return xr_ast_variable(parser->compiler_session, "_", line);
    }
    /* Bare lambda: `name -> expr` as a call argument. Unambiguous here
     * because call arguments are delimited by '(' and ')'. The analyzer
     * infers the parameter type from the callee's parameter signature. */
    if (xr_parser_check(parser, TK_NAME)) {
        Scanner saved_scan = parser->scanner;
        Token saved_cur = parser->current;
        Token saved_prev = parser->previous;
        xr_parser_advance(parser);
        if (xr_parser_check(parser, TK_ARROW)) {
            Token name_tok = parser->previous;
            char *pname =
                (char *) ast_alloc(parser->compiler_session, (size_t) name_tok.length + 1);
            memcpy(pname, name_tok.start, name_tok.length);
            pname[name_tok.length] = '\0';
            xr_parser_advance(parser); /* consume '->' */
            XrParamNode **params = (XrParamNode **) ast_alloc_array(parser->compiler_session,
                                                                    sizeof(XrParamNode *), 1);
            params[0] =
                xr_param_node_new(parser->compiler_session, pname, name_tok.line, name_tok.column);
            return xr_parse_arrow_function_body(parser, params, 1, name_tok.line);
        }
        /* Not a bare lambda — restore and fall through to normal parsing. */
        parser->scanner = saved_scan;
        parser->current = saved_cur;
        parser->previous = saved_prev;
    }
    return xr_parse_expression(parser);
}

AstNode *xr_parse_call_argument(Parser *parser) {
    return xr_parse_call_argument_with_access(parser, NULL);
}

// Parse function call: add(1, 2) or add(...t, 3)
// Built-in heap types are constructed with `T(args)` (no `new`). These names
// have no callable function binding, so a call on them is a construction.
// User classes/structs already construct through the normal call path; only
// these built-ins need to be re-targeted to the new-expr construction node.
bool xr_is_construct_only_type_name(const char *name) {
    if (!name)
        return false;
    static const char *const names[] = {"Map",     "WeakMap", "Array",         "Set",
                                        "WeakSet", "Channel", "StringBuilder", NULL};
    for (const char *const *p = names; *p; p++) {
        if (strcmp(name, *p) == 0)
            return true;
    }
    return false;
}

AstNode *xr_parse_call_expr(Parser *parser, AstNode *callee) {
    XR_DCHECK(parser != NULL, "parse_call_expr: NULL parser");
    int line = parser->previous.line;

    AstNode **arguments = NULL;
    XrCallArgAccess *arg_accesses = NULL;
    int arg_count = 0;
    int arg_capacity = 0;
    int access_count = 0;
    int access_capacity = 0;

    if (!xr_parser_check(parser, TK_RPAREN)) {
        do {
            XrCallArgAccess access = XR_CALL_ARG_VALUE;
            AstNode *arg = xr_parse_call_argument_with_access(parser, &access);
            XR_PARSE_PUSH(parser, arguments, arg_count, arg_capacity, arg);
            XR_PARSE_PUSH(parser, arg_accesses, access_count, access_capacity, access);
        } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RPAREN));
    }

    xr_parser_consume(parser, TK_RPAREN, "expected ')' after argument list");

    // `Map()` / `Array()` / `Channel(n)` etc. construct built-in heap types.
    if (callee && callee->type == AST_VARIABLE &&
        xr_is_construct_only_type_name(callee->as.variable.name)) {
        return xr_ast_new_expr(parser->compiler_session, NULL, callee->as.variable.name, arguments,
                               arg_accesses, arg_count, NULL, 0, line);
    }

    return xr_ast_call_expr(parser->compiler_session, callee, arguments, arg_accesses, arg_count,
                            line);
}

/* ========== Array Parsing ========== */

// Parse one array-literal element: either `...spread` or a plain expression.
// Spread elements splice an array's contents into the surrounding literal at
// runtime (`[...a, x]`), mirroring tuple-literal spread.
static AstNode *parse_array_element(Parser *parser) {
    if (xr_parser_check(parser, TK_DOT_DOT_DOT)) {
        int elem_line = parser->current.line;
        xr_parser_advance(parser);  // consume '...'
        AstNode *inner = xr_parse_expression(parser);
        if (!inner)
            return NULL;
        return xr_ast_spread_expr(parser->compiler_session, inner, elem_line);
    }
    return xr_parse_expression(parser);
}

// Parse array literal or Map literal (smart detection)
// [1, 2, 3] -> array, ["key": value, ...] -> Map, [...a, x] -> array spread
AstNode *xr_parse_array_literal(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_array_literal: NULL parser");
    int line = parser->previous.line;

    if (xr_parser_match(parser, TK_RBRACKET)) {
        return xr_ast_array_literal(parser->compiler_session, NULL, 0, line);
    }

    // A leading spread `[...a` is unambiguously an array literal (a Map key
    // can never be a spread), so commit to the array branch immediately.
    if (xr_parser_check(parser, TK_DOT_DOT_DOT)) {
        AstNode **elements = NULL;
        int count = 0;
        int capacity = 0;
        XR_PARSE_PUSH(parser, elements, count, capacity, parse_array_element(parser));
        while (xr_parser_match(parser, TK_COMMA)) {
            if (xr_parser_check(parser, TK_RBRACKET))
                break;
            XR_PARSE_PUSH(parser, elements, count, capacity, parse_array_element(parser));
        }
        xr_parser_consume(parser, TK_RBRACKET, "expected ']' at end of array");
        return xr_ast_array_literal(parser->compiler_session, elements, count, line);
    }

    // Parse first expression, then check ':' for Map or ',' for array
    AstNode *first_expr = xr_parse_expression(parser);
    if (xr_parser_match(parser, TK_SEMICOLON)) {
        AstNode *repeat_count = xr_parse_expression(parser);
        xr_parser_consume(parser, TK_RBRACKET, "expected ']' after array repeat count");
        return xr_ast_array_repeat_literal(parser->compiler_session, first_expr, repeat_count,
                                           line);
    }

    if (xr_parser_match(parser, TK_COLON)) {
        // Map: ["key": value, ...]
        AstNode **keys = NULL;
        AstNode **values = NULL;
        int count = 0;
        int capacity = 4;

        keys = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *),
                                            (size_t) capacity);
        values = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *),
                                              (size_t) capacity);

        keys[0] = first_expr;
        values[0] = xr_parse_expression(parser);
        count = 1;

        while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RBRACKET)) {
            if (count >= capacity) {
                int old_capacity = capacity;
                capacity *= 2;

                AstNode **_new_keys = (AstNode **) ast_alloc_array(
                    parser->compiler_session, sizeof(AstNode *), (size_t) capacity);
                if (old_capacity > 0 && keys)
                    memcpy(_new_keys, keys, sizeof(AstNode *) * (size_t) old_capacity);
                keys = _new_keys;

                AstNode **_new_values = (AstNode **) ast_alloc_array(
                    parser->compiler_session, sizeof(AstNode *), (size_t) capacity);
                if (old_capacity > 0 && values)
                    memcpy(_new_values, values, sizeof(AstNode *) * (size_t) old_capacity);
                values = _new_values;
            }

            keys[count] = xr_parse_expression(parser);

            if (!xr_parser_match(parser, TK_COLON)) {
                xr_parser_error(parser, "expected ':' after Map key");
                return xr_ast_map_literal(parser->compiler_session, NULL, NULL, 0, line);
            }

            values[count] = xr_parse_expression(parser);
            count++;
        }

        xr_parser_consume(parser, TK_RBRACKET, "expected ']' at end of Map literal");

        return xr_ast_map_literal(parser->compiler_session, keys, values, count, line);

    } else {
        // Array: [1, 2, 3]
        AstNode **elements = NULL;
        int count = 0;
        int capacity = 4;

        elements = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *),
                                                (size_t) capacity);

        elements[0] = first_expr;
        count = 1;

        while (xr_parser_match(parser, TK_COMMA)) {
            if (xr_parser_check(parser, TK_RBRACKET)) {
                break;
            }

            XR_PARSE_PUSH(parser, elements, count, capacity, parse_array_element(parser));
        }

        xr_parser_consume(parser, TK_RBRACKET, "expected ']' at end of array");

        return xr_ast_array_literal(parser->compiler_session, elements, count, line);
    }
}

/*
 * Parse Json/Object literal `{ key: value }`.
 * Map literals use the prefixed `#{ key: value }` form.
 * Supports computed property syntax: `{ [expr]: value }`.
 */
AstNode *xr_parse_object_literal(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_object_literal: NULL parser");
    int line = parser->previous.line;

    // '{' already consumed

    // Empty object {} -> Json
    if (xr_parser_match(parser, TK_RBRACE)) {
        return xr_ast_object_literal(parser->compiler_session, NULL, NULL, NULL, 0, line);
    }

    // Collect key-value pairs
    AstNode **keys = NULL;
    AstNode **values = NULL;
    bool *computed = NULL;
    bool has_computed = false;
    int count = 0;
    int capacity = 0;

    do {
        // Expand capacity
        if (count >= capacity) {
            int _old_cap_capacity = (int) capacity;
            capacity = _old_cap_capacity == 0 ? 4 : _old_cap_capacity * 2;
            AstNode **_new_keys = (AstNode **) ast_alloc_array(
                parser->compiler_session, sizeof(AstNode *), (size_t) capacity);
            if (_old_cap_capacity > 0 && keys)
                memcpy(_new_keys, keys, sizeof(AstNode *) * (size_t) _old_cap_capacity);
            keys = _new_keys;

            AstNode **_new_values = (AstNode **) ast_alloc_array(
                parser->compiler_session, sizeof(AstNode *), (size_t) capacity);
            if (_old_cap_capacity > 0 && values)
                memcpy(_new_values, values, sizeof(AstNode *) * (size_t) _old_cap_capacity);
            values = _new_values;

            bool *_new_computed =
                (bool *) ast_alloc_array(parser->compiler_session, sizeof(bool), (size_t) capacity);
            if (_old_cap_capacity > 0 && computed)
                memcpy(_new_computed, computed, sizeof(bool) * (size_t) _old_cap_capacity);
            computed = _new_computed;
        }

        // Spread entry: `{ ...base }` splices another object's fields in.
        // Represented as a NULL key with an AST_SPREAD_EXPR value; later
        // entries override earlier ones at merge time.
        if (xr_parser_check(parser, TK_DOT_DOT_DOT)) {
            int spread_line = parser->current.line;
            xr_parser_advance(parser);  // consume '...'
            AstNode *src = xr_parse_expression(parser);
            if (!src)
                return xr_ast_literal_null(parser->compiler_session, line);
            keys[count] = NULL;
            values[count] = xr_ast_spread_expr(parser->compiler_session, src, spread_line);
            computed[count] = false;
            count++;
            continue;
        }

        // Parse key
        AstNode *key = NULL;
        bool is_computed = false;
        const char *shorthand_name = NULL;

        // Computed property syntax: [expr]
        if (xr_parser_match(parser, TK_LBRACKET)) {
            key = xr_parse_expression(parser);
            xr_parser_consume(parser, TK_RBRACKET, "expected ']' after computed property");
            is_computed = true;
            has_computed = true;
        }
        // String literal as key
        else if (xr_parser_check(parser, TK_LITERAL_STRING)) {
            Token key_token = parser->current;
            xr_parser_advance(parser);

            char *key_str =
                (char *) ast_alloc(parser->compiler_session, (size_t) key_token.length - 1);
            memcpy(key_str, key_token.start + 1, key_token.length - 2);
            key_str[key_token.length - 2] = '\0';
            key = xr_ast_literal_string(parser->compiler_session, key_str, line);
            is_computed = false;
        }
        // Numeric literal as key: only Map allows this, and Map literals must
        // use the `#{ ... }` prefix form.
        else if (xr_parser_check(parser, TK_LITERAL_INT) ||
                 xr_parser_check(parser, TK_LITERAL_FLOAT)) {
            xr_parser_error(
                parser, "Json object does not support numeric keys; use `#{ key: value }` for Map");
            return xr_ast_literal_null(parser->compiler_session, line);
        }
        // Identifier or keyword as key (allow keywords like 'type', 'int', etc.)
        else if (xr_parser_check(parser, TK_NAME) ||
                 // Type keywords
                 xr_parser_check(parser, TK_TYPE_ALIAS) || xr_parser_check(parser, TK_INT) ||
                 xr_parser_check(parser, TK_FLOAT) || xr_parser_check(parser, TK_STRING) ||
                 xr_parser_check(parser, TK_BOOL) ||
                 // Common keywords
                 xr_parser_check(parser, TK_CLASS) || xr_parser_check(parser, TK_ENUM) ||
                 xr_parser_check(parser, TK_STATIC) || xr_parser_check(parser, TK_AS) ||
                 xr_parser_check(parser, TK_IN) || xr_parser_check(parser, TK_IS)) {
            Token key_token = parser->current;
            xr_parser_advance(parser);

            char *key_str =
                (char *) ast_alloc(parser->compiler_session, (size_t) key_token.length + 1);
            memcpy(key_str, key_token.start, key_token.length);
            key_str[key_token.length] = '\0';
            key = xr_ast_literal_string(parser->compiler_session, key_str, line);
            is_computed = false;
            if (key_token.type == TK_NAME)
                shorthand_name = key_str;
        } else {
            xr_parser_error(
                parser,
                "literal key must be identifier, string, number or [expr] computed property");
            return xr_ast_literal_null(parser->compiler_session, line);
        }

        keys[count] = key;
        computed[count] = is_computed;

        // `{ ... }` is always a Json/Object literal in xray.
        // The only legal key-value separator is `:`. Map literals must use
        // the `#{ k: v }` prefix form. The unified arrow `->` is reserved
        // for function / branch arrows and is rejected here with a hint.
        if (xr_parser_match(parser, TK_COLON)) {
        } else if (xr_parser_check(parser, TK_ARROW)) {
            xr_parser_error(
                parser,
                "`->` is not a valid separator in Json literal; use `#{ key: value }` for Map");
            return xr_ast_literal_null(parser->compiler_session, line);
        } else if (shorthand_name &&
                   (xr_parser_check(parser, TK_COMMA) || xr_parser_check(parser, TK_RBRACE))) {
            values[count] = xr_ast_variable(parser->compiler_session, shorthand_name, line);
            count++;
            continue;
        } else {
            xr_parser_error(parser, "expected ':' after key in Json literal");
            return xr_ast_literal_null(parser->compiler_session, line);
        }

        // Parse value expression
        values[count] = xr_parse_expression(parser);
        count++;

    } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RBRACE));

    // Expect closing brace
    xr_parser_consume(parser, TK_RBRACE, "expected '}' at end of literal");

    AstNode *result = xr_ast_object_literal(parser->compiler_session, keys, values,
                                            has_computed ? computed : NULL, count, line);

    // Free temporary array

    return result;
}

/*
 * Parse Map literal.
 * Syntax: #{} or #{"key": value, ...}
 */
AstNode *xr_parse_empty_map_literal(Parser *parser) {
    int line = parser->previous.line;

    // '#{' already consumed

    // Empty Map: #{}
    if (xr_parser_match(parser, TK_RBRACE)) {
        return xr_ast_map_literal(parser->compiler_session, NULL, NULL, 0, line);
    }

    // Non-empty Map: #{"key": value, ...}
    AstNode **keys = NULL;
    AstNode **values = NULL;
    int count = 0;
    int capacity = 0;

    do {
        // Expand capacity
        if (count >= capacity) {
            int _old_cap_capacity = (int) capacity;
            capacity = _old_cap_capacity == 0 ? 4 : _old_cap_capacity * 2;
            AstNode **_new_keys = (AstNode **) ast_alloc_array(
                parser->compiler_session, sizeof(AstNode *), (size_t) capacity);
            if (_old_cap_capacity > 0 && keys)
                memcpy(_new_keys, keys, sizeof(AstNode *) * (size_t) _old_cap_capacity);
            keys = _new_keys;

            AstNode **_new_values = (AstNode **) ast_alloc_array(
                parser->compiler_session, sizeof(AstNode *), (size_t) capacity);
            if (_old_cap_capacity > 0 && values)
                memcpy(_new_values, values, sizeof(AstNode *) * (size_t) _old_cap_capacity);
            values = _new_values;
        }

        // Parse key expression
        keys[count] = xr_parse_expression(parser);

        // Expect ':' as the key-value separator inside `#{ ... }` Map literal.
        // The `#` prefix already disambiguates a Map from a Json/Object literal.
        xr_parser_consume(parser, TK_COLON, "expected ':' after Map key in #{...}");

        // Parse value expression
        values[count] = xr_parse_expression(parser);
        count++;

    } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RBRACE));

    // Expect '}'
    xr_parser_consume(parser, TK_RBRACE, "expected '}' at end of Map literal");

    // Create Map literal node
    return xr_ast_map_literal(parser->compiler_session, keys, values, count, line);
}

/*
 * Parse new Set literal #[]
 * Syntax: #[] or #[element, ...]
 */
AstNode *xr_parse_set_literal_new(Parser *parser) {
    int line = parser->previous.line;

    // '#[' already consumed

    // Empty Set: #[]
    if (xr_parser_match(parser, TK_RBRACKET)) {
        // Create empty Set literal
        return xr_ast_set_literal(parser->compiler_session, NULL, 0, line);
    }

    // Collect elements
    AstNode **elements = NULL;
    int count = 0;
    int capacity = 0;

    do {
        // Expand capacity
        if (count >= capacity) {
            int _old_cap_capacity = (int) capacity;
            capacity = _old_cap_capacity == 0 ? 4 : _old_cap_capacity * 2;
            AstNode **_new_elements = (AstNode **) ast_alloc_array(
                parser->compiler_session, sizeof(AstNode *), (size_t) capacity);
            if (_old_cap_capacity > 0 && elements)
                memcpy(_new_elements, elements, sizeof(AstNode *) * (size_t) _old_cap_capacity);
            elements = _new_elements;
        }

        // Parse element expression
        elements[count++] = xr_parse_expression(parser);

    } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_RBRACKET));

    // Expect ']'
    xr_parser_consume(parser, TK_RBRACKET, "expected ']' at end of Set literal");

    // Create Set literal node
    return xr_ast_set_literal(parser->compiler_session, elements, count, line);
}

/*
 * Parse index access or slice expression (infix)
 * Index access: arr[0]
 * Slice syntax: arr[start:end], arr[:end], arr[start:], arr[:]
 */
AstNode *xr_parse_index_access(Parser *parser, AstNode *array) {
    XR_DCHECK(parser != NULL, "parse_index_access: NULL parser");
    int line = parser->previous.line;

    // '[' already consumed

    AstNode *start = NULL;
    AstNode *end = NULL;
    bool is_slice = false;

    // Check if [:...] form (omitted start index)
    if (parser->current.type == TK_COLON) {
        is_slice = true;
        xr_parser_advance(parser);  // Consume ':'

        // Check if there's an end index
        if (parser->current.type != TK_RBRACKET) {
            end = xr_parse_expression(parser);
        }
    } else {
        // Parse start index or regular index
        start = xr_parse_expression(parser);

        // Check if slice syntax
        if (parser->current.type == TK_COLON) {
            is_slice = true;
            xr_parser_advance(parser);  // Consume ':'

            // Check if there's an end index
            if (parser->current.type != TK_RBRACKET) {
                end = xr_parse_expression(parser);
            }
        }
    }

    // Expect ']'
    xr_parser_consume(parser, TK_RBRACKET, "expected ']' after index or slice");

    if (is_slice) {
        // Create slice expression node
        return xr_ast_slice_expr(parser->compiler_session, array, start, end, line);
    } else {
        // Create index access node
        return xr_ast_index_get(parser->compiler_session, array, start, line);
    }
}

/*
 * Parse member access (infix)
 * arr.length, arr.push
 *
 * Note: Keywords are allowed as member names in member access (e.g. .get, .set, .new)
 */
AstNode *xr_parse_member_access(Parser *parser, AstNode *object) {
    XR_DCHECK(parser != NULL, "parse_member_access: NULL parser");
    int line = parser->previous.line;

    // '.' already consumed

    // In member access, all keywords are allowed as member names (no restrictions)
    // This supports JSON-style key names like j.float, j.int, j.class
    const char *name = NULL;
    int name_len = 0;

    XrTokenType t = parser->current.type;

    // Accept identifier, any keyword (keyword range TK_FIRST_KEYWORD..TK_LAST_KEYWORD),
    // or an integer literal for tuple field access (`tuple.0`, `tuple.1`).
    // The integer text is stored verbatim as the member name; the analyzer
    // recognises digit-only names on tuple-typed receivers.
    if (t == TK_NAME || t == TK_LITERAL_INT || (t >= TK_FIRST_KEYWORD && t <= TK_LAST_KEYWORD)) {
        xr_parser_advance(parser);
        name = parser->previous.start;
        name_len = parser->previous.length;
    } else {
        xr_parser_error(parser, "expected member name");
        return NULL;
    }

    // Copy member name (xr_ast_member_access deep-copies, so release our copy)
    char *member_name = (char *) ast_alloc(parser->compiler_session, (size_t) name_len + 1);
    strncpy(member_name, name, name_len);
    member_name[name_len] = '\0';

    AstNode *node = xr_ast_member_access(parser->compiler_session, object, member_name, line);
    node->column = parser->previous.column;
    return node;
}

/*
 * Parse return statement. Multi-value returns are no longer
 * supported: a function returns at most one value, and that value
 * may be a tuple expression (`return (a, b)`) when multiple results
 * are needed.
 */
AstNode *xr_parse_return_statement(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_return_statement: NULL parser");
    int line = parser->current.line;
    xr_parser_advance(parser);  // Consume 'return'

    // Bare `return` or `return` followed by block-close: void return.
    if (parser->current.type == TK_RBRACE ||  // Block end
        parser->current.type == TK_EOF) {     // File end
        return xr_ast_return_stmt(parser->compiler_session, NULL, 0, line);
    }

    AstNode *value = xr_parse_expression(parser);

    // Comma after the return expression is the obsolete multi-value
    // form. Redirect users to the tuple equivalent.
    if (xr_parser_check(parser, TK_COMMA)) {
        xr_parser_error(parser, "[E0801] multi-value return is not supported; "
                                "use a tuple: `return (a, b)`");
        return NULL;
    }

    AstNode **values = (AstNode **) ast_alloc_array(parser->compiler_session, sizeof(AstNode *), 1);
    values[0] = value;
    return xr_ast_return_stmt(parser->compiler_session, values, 1, line);
}

/*
 * Parse type alias declaration
 * type Point = { x: float, y: float }
 * type BinaryOp = (int, int) -> int
 * type Points = Array<Point>
 *
 * Type aliases are registered in parser's type alias table for parse-time resolution.
 * Also generates AST_TYPE_ALIAS node so analyzer/LSP can see the declaration.
 */
AstNode *xr_parse_type_alias_declaration(Parser *parser) {
    int line = parser->previous.line;

    // Parse alias name
    xr_parser_consume(parser, TK_NAME, "expected type name after 'type'");
    char *alias_name = xr_strndup(parser->previous.start, parser->previous.length);

    XrGenericParam **type_params = NULL;
    int type_param_count = 0;
    int type_param_capacity = 0;

    if (xr_parser_match(parser, TK_LT)) {
        do {
            xr_parser_consume(parser, TK_NAME, "expected type parameter name");
            Token param_token = parser->previous;

            char *param_name =
                (char *) ast_alloc(parser->compiler_session, (size_t) param_token.length + 1);
            memcpy(param_name, param_token.start, (size_t) param_token.length);
            param_name[param_token.length] = '\0';

            for (int i = 0; i < type_param_count; i++) {
                if (type_params[i] && type_params[i]->name &&
                    strcmp(type_params[i]->name, param_name) == 0) {
                    xr_parser_error(parser, "duplicate type parameter name in type alias");
                }
            }

            XrTypeRef **constraints = NULL;
            int constraint_count = 0;
            if (xr_parser_match(parser, TK_COLON)) {
                xr_parser_error(parser, "type alias type parameters do not support constraints");
                constraints = xr_parse_constraint_list(parser, &constraint_count);
            }

            XrGenericParam *gp =
                (XrGenericParam *) ast_alloc(parser->compiler_session, sizeof(XrGenericParam));
            gp->name = param_name;
            gp->constraints = constraints;
            gp->constraint_count = constraint_count;
            XR_PARSE_PUSH(parser, type_params, type_param_count, type_param_capacity, gp);
        } while (xr_parser_match(parser, TK_COMMA) && !xr_parser_check(parser, TK_GT));

        xr_parser_consume(parser, TK_GT, "expected '>' to close type alias parameters");
    }

    // Expect '='
    xr_parser_consume(parser, TK_ASSIGN, "expected '=' in type alias definition");

    // Pre-register with NULL to block recursive self-reference (type A = A).
    // We retain the entry pointer so we can patch its `type` field below
    // once the RHS is fully parsed.
    XrTypeAlias *alias_entry = xr_type_scope_define(parser->type_scope, alias_name, NULL);
    if (!alias_entry) {
        xr_parser_error(parser, "duplicate type alias definition");
        xr_free(alias_name);
        return NULL;
    }
    if (type_param_count > 0) {
        const char **param_names = (const char **) ast_alloc_array(
            parser->compiler_session, sizeof(const char *), (size_t) type_param_count);
        for (int i = 0; i < type_param_count; i++)
            param_names[i] = type_params[i]->name;
        alias_entry->type_param_names = param_names;
        alias_entry->type_param_count = type_param_count;
    }

    XrTypeScope *saved_scope = parser->type_scope;
    XrTypeScope *generic_scope = NULL;
    if (type_param_count > 0) {
        generic_scope = xr_type_scope_new(parser->type_scope);
        for (int i = 0; i < type_param_count; i++) {
            XrTypeRef *type_param =
                xr_tref_type_param(parser->compiler_session, type_params[i]->name);
            xr_type_scope_define(generic_scope, type_params[i]->name, type_param);
        }
        parser->type_scope = generic_scope;
    }

    // Parse type definition; alias expansion catches recursive aliases while
    // the placeholder is still unresolved.
    XrTypeRef *type_definition = xr_parse_type_annotation(parser);
    if (type_param_count > 0) {
        parser->type_scope = saved_scope;
        xr_type_scope_free(generic_scope);
    }
    if (!type_definition) {
        xr_parser_error(parser, "invalid type definition");
        xr_free(alias_name);
        return NULL;
    }

    // Patch the placeholder with the actual type definition.
    alias_entry->type_ref = type_definition;
    if (type_definition->kind == XR_TREF_OBJECT) {
        char *type_name = (char *) ast_alloc(parser->compiler_session, strlen(alias_name) + 1);
        strcpy(type_name, alias_name);
        type_definition->name = type_name;
    }

    // Create AST node so analyzer/LSP can see the declaration.
    // Stash the resolved type in TypeAliasNode::resolved_type so that
    // the analyzer can read it without going through any backchannel
    // on AstNode itself.
    AstNode *node =
        xr_ast_type_alias(parser->compiler_session, alias_name, NULL, NULL, NULL, 0, line);
    xr_free(alias_name);  // xr_ast_type_alias copies the name, so we can free the original
    if (!node) {
        return NULL;
    }
    node->as.type_alias.type_params = type_params;
    node->as.type_alias.type_param_count = type_param_count;
    node->as.type_alias.resolved_type = type_definition;

    return node;
}

/*
 * Parse declaration (variable declaration, constant declaration, class declaration or statement)
 */
AstNode *xr_parse_declaration(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_declaration: NULL parser");
    // Module system (only allowed at top level)
    if (xr_parser_match(parser, TK_IMPORT)) {
        if (parser->scope_depth > 0) {
            xr_parser_error(parser, "'import' must appear at module top level");
            return NULL;
        }
        return xr_parse_import_declaration(parser);
    }

    if (xr_parser_match(parser, TK_EXPORT)) {
        if (parser->scope_depth > 0) {
            xr_parser_error(parser, "'export' must appear at module top level");
            return NULL;
        }
        return xr_parse_export_declaration(parser);
    }

    if (xr_parser_check_name(parser, "extern")) {
        xr_parser_advance(parser);
        return xr_parse_extern_block_declaration(parser);
    }

    if (xr_is_global_asm_start(parser)) {
        xr_parser_advance(parser);  // consume asm
        return xr_parse_global_asm_declaration(parser);
    }

    if (xr_parser_check_name(parser, "abstract")) {
        int modifier_line = parser->current.line;
        xr_parser_error_at_current(parser,
                                   "'abstract' was removed; use an interface for contracts");
        xr_parser_advance(parser);
        xr_parser_skip_invalid_construct(parser, modifier_line,
                                         current_can_start_top_level_after_recovery, false);
        return NULL;
    }

    if (xr_parser_check_name(parser, "open") || xr_parser_check_name(parser, "virtual")) {
        int modifier_line = parser->current.line;
        const char *modifier = xr_parser_check_name(parser, "open") ? "open" : "virtual";
        char msg[128];
        snprintf(msg, sizeof(msg), "'%s' was removed; class dispatch strategy is inferred",
                 modifier);
        xr_parser_error_at_current(parser, msg);
        xr_parser_advance(parser);
        xr_parser_skip_invalid_construct(parser, modifier_line,
                                         current_can_start_top_level_after_recovery, false);
        return NULL;
    }

    // Class declaration (with optional 'final' prefix)
    if (xr_parser_match(parser, TK_FINAL)) {
        if (!xr_parser_match(parser, TK_CLASS)) {
            xr_parser_error_at_current(parser,
                                       "'final' can only be used before 'class' at top level");
            return NULL;
        }
        AstNode *cls = xr_parse_class_declaration(parser);
        if (cls)
            cls->as.class_decl.explicit_final = true;
        return cls;
    }

    if (xr_parser_match(parser, TK_CLASS)) {
        return xr_parse_class_declaration(parser);
    }

    // Struct declaration
    bool is_packed_struct = false;
    if (xr_parser_match(parser, TK_PACKED)) {
        is_packed_struct = true;
        xr_parser_consume(parser, TK_STRUCT, "expected 'struct' after 'packed'");
    }
    if (is_packed_struct || xr_parser_match(parser, TK_STRUCT)) {
        AstNode *st = xr_parse_struct_declaration(parser);
        if (st)
            st->as.struct_decl.is_packed = is_packed_struct;
        return st;
    }

    if (xr_parser_match(parser, TK_UNION)) {
        return xr_parse_union_declaration(parser);
    }

    // Interface declaration
    if (xr_parser_match(parser, TK_INTERFACE)) {
        return xr_parse_interface_declaration(parser);
    }

    // Enum declaration
    if (xr_parser_match(parser, TK_ENUM)) {
        return xr_parse_enum_declaration(parser);
    }

    // Type alias declaration: type Point = { x: float, y: float }
    if (xr_parser_match(parser, TK_TYPE_ALIAS)) {
        return xr_parse_type_alias_declaration(parser);
    }

    // ========== Coroutine syntax ==========

    // defer statement: defer fn() or defer { block }
    if (xr_parser_match(parser, TK_DEFER)) {
        return xr_parse_defer_statement(parser);
    }

    // select statement: select { ... }
    if (xr_parser_match(parser, TK_SELECT)) {
        return xr_parse_select_statement(parser);
    }

    // scope block: scope { ... }
    if (xr_parser_match(parser, TK_SCOPE)) {
        return xr_parse_scope_block(parser);
    }

    // yield statement: `yield expr` produces a generator value. Bare `yield`
    // (cooperative scheduling) was removed in favor of `Coro.yield()`.
    if (xr_parser_match(parser, TK_YIELD)) {
        int line = parser->previous.line;
        if (xr_parser_check(parser, TK_SEMICOLON) || xr_parser_check(parser, TK_RBRACE) ||
            xr_parser_check(parser, TK_EOF)) {
            xr_parser_error_at_current(
                parser, "`yield` requires a value (generator value production); use `Coro.yield()` "
                        "for cooperative scheduling");
            return NULL;
        }
        AstNode *value = xr_parse_expression(parser);
        if (!value)
            return NULL;
        return xr_ast_yield_stmt(parser->compiler_session, value, line);
    }

    // Attributed declaration: @test fn ..., @native class ..., etc.
    if (xr_parser_check(parser, TK_AT)) {
        return xr_parse_attributed_declaration(parser);
    }

    // Function declaration: only fn keyword supported
    if (xr_parser_match(parser, TK_FN)) {
        return xr_parse_function_declaration(parser);
    }

    // Context keywords: linked/supervisor before go/scope
    // These are identifiers that act as modifiers only when followed by go or scope.
    // Note: "monitored" prefix was removed — use task.monitor() API instead.
    if (xr_parser_check(parser, TK_NAME)) {
        Token name_token = parser->current;

        // "linked" go ... | "linked" scope ...
        if (name_token.length == 6 && memcmp(name_token.start, "linked", 6) == 0) {
            Scanner saved = parser->scanner;
            Token peek = xr_scanner_scan(&saved);
            if (peek.type == TK_GO) {
                xr_parser_advance(parser);  // consume "linked"
                xr_parser_advance(parser);  // consume "go"
                return xr_parse_go_expr_with_link(parser, XR_LINK_LINKED);
            }
            if (peek.type == TK_SCOPE) {
                xr_parser_advance(parser);  // consume "linked"
                xr_parser_advance(parser);  // consume "scope"
                return xr_parse_scope_block_with_mode(parser, XR_SCOPE_LINKED);
            }
        }

        // "supervisor" scope ...
        if (name_token.length == 10 && memcmp(name_token.start, "supervisor", 10) == 0) {
            Scanner saved = parser->scanner;
            Token peek = xr_scanner_scan(&saved);
            if (peek.type == TK_SCOPE) {
                xr_parser_advance(parser);  // consume "supervisor"
                xr_parser_advance(parser);  // consume "scope"
                return xr_parse_scope_block_with_mode(parser, XR_SCOPE_SUPERVISOR);
            }
        }
    }

    // Detect unsupported 'from ... import' syntax
    if (xr_parser_check_name(parser, "from")) {
        xr_parser_error_at_current(parser, "Python-style 'from ... import' is not supported. "
                                           "Use 'import { name } from \"module\"' in Xray");
        return NULL;
    }

    // Detect common wrong keywords from other languages.
    // Without this, ASI causes misleading "semicolon" errors instead of
    // reporting the actual problem (unknown keyword).
    if (xr_parser_check(parser, TK_NAME)) {
        Token name_token = parser->current;
        // Detect walrus-style short declaration: x := 1
        {
            Parser checkpoint = *parser;
            xr_parser_advance(parser);
            if (xr_parser_check(parser, TK_COLON)) {
                xr_parser_advance(parser);
                if (xr_parser_check(parser, TK_ASSIGN)) {
                    *parser = checkpoint;
                    char buf[128];
                    snprintf(
                        buf, sizeof(buf),
                        "':=' is not supported. Use 'var %.*s = ...' to declare variables in Xray",
                        name_token.length, name_token.start);
                    xr_parser_error_at_current(parser, buf);
                    return NULL;
                }
            }
            *parser = checkpoint;
        }
        if (name_token.length == 8 && memcmp(name_token.start, "function", 8) == 0) {
            xr_parser_error_at_current(
                parser, "unknown keyword 'function'. Use 'fn' to define functions in Xray");
            return NULL;
        }
        if (name_token.length == 3 && memcmp(name_token.start, "def", 3) == 0) {
            xr_parser_error_at_current(
                parser, "unknown keyword 'def'. Use 'fn' to define functions in Xray");
            return NULL;
        }
        if (name_token.length == 4 && memcmp(name_token.start, "func", 4) == 0) {
            xr_parser_error_at_current(
                parser, "unknown keyword 'func'. Use 'fn' to define functions in Xray");
            return NULL;
        }
        if (name_token.length == 4 && memcmp(name_token.start, "elif", 4) == 0) {
            xr_parser_error_at_current(parser, "unknown keyword 'elif'. Use 'else if' in Xray");
            return NULL;
        }
        if (name_token.length == 6 && memcmp(name_token.start, "switch", 6) == 0) {
            xr_parser_error_at_current(
                parser, "unknown keyword 'switch'. Use 'match' for pattern matching in Xray");
            return NULL;
        }
        if (name_token.length == 7 && memcmp(name_token.start, "foreach", 7) == 0) {
            xr_parser_error_at_current(
                parser, "unknown keyword 'foreach'. Use 'for (x in collection) { }' in Xray");
            return NULL;
        }
        if (name_token.length == 2 && memcmp(name_token.start, "do", 2) == 0) {
            xr_parser_error_at_current(
                parser, "'do...while' is not supported. Use 'while (condition) { }' in Xray");
            return NULL;
        }
        if (name_token.length == 6 && memcmp(name_token.start, "lambda", 6) == 0) {
            xr_parser_error_at_current(
                parser,
                "'lambda' is not supported. Use 'fn(params) { }' or '(params) -> expr' in Xray");
            return NULL;
        }
    }

    // Detect type-first declarations: int x = 5, string name = "hello"
    // Type keyword followed by identifier means user intended a declaration
    {
        XrTokenType t = parser->current.type;
        if (t == TK_INT || t == TK_INT8 || t == TK_INT16 || t == TK_INT32 || t == TK_INT64 ||
            t == TK_UINT8 || t == TK_UINT16 || t == TK_UINT32 || t == TK_UINT64 || t == TK_FLOAT ||
            t == TK_FLOAT32 || t == TK_FLOAT64 || t == TK_STRING || t == TK_BOOL) {
            // Peek ahead: if next token is TK_NAME, this is a C-style declaration
            Token saved = parser->current;
            Parser checkpoint = *parser;
            xr_parser_advance(parser);
            if (xr_parser_check(parser, TK_NAME)) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "C-style declaration not supported. Use 'var %.*s: %.*s = ...' in Xray",
                         parser->current.length, parser->current.start, saved.length, saved.start);
                *parser = checkpoint;
                xr_parser_error_at_current(parser, buf);
                return NULL;
            }
            *parser = checkpoint;
        }
    }

    /* Variable declaration
     *
     * Syntax design:
     * 1. var a                - No initialization, default value
     * 2. var a = 1            - Single variable declaration
     * 3. var (a, b) = value   - Tuple destructuring declaration
     * 4. var a, b = ...       - Forbidden obsolete multi-value declaration
     * 5. var a=1, b=2         - Forbidden; write separate declarations
     */
    if (xr_parser_match(parser, TK_VAR)) {
        // Check if destructure declaration
        if (xr_parser_check(parser, TK_LBRACKET) || xr_parser_check(parser, TK_LBRACE) ||
            xr_parser_check(parser, TK_LPAREN)) {
            return xr_parse_destructure_declaration(parser, false);
        }

        if (xr_parser_check(parser, TK_NAME)) {
            int saved_line = parser->current.line;
            int saved_column = parser->current.column;

            // Copy first identifier name
            char *first_name =
                (char *) ast_alloc(parser->compiler_session, (size_t) parser->current.length + 1);
            memcpy(first_name, parser->current.start, parser->current.length);
            first_name[parser->current.length] = '\0';
            xr_parser_advance(parser);

            // `var a, b = ...` (bare multi-variable) is the obsolete
            // multi-value form. Tuple destructure `var (a, b) = ...`
            // is the canonical replacement.
            if (xr_parser_check(parser, TK_COMMA)) {
                xr_parser_error(parser, "multi-variable declaration is not supported; "
                                        "use tuple destructure: `var (a, b) = ...`");
                return NULL;
            }
            {
                // Single variable declaration: var a or var a = expr or var a: Type = expr
                XrTypeRef *var_type = NULL;
                if (xr_parser_match(parser, TK_COLON)) {
                    var_type = xr_parse_type_annotation(parser);
                }

                // Optional initialization expression
                AstNode *initializer = NULL;
                if (xr_parser_match(parser, TK_ASSIGN)) {
                    initializer = xr_parse_expression(parser);
                }

                AstNode *decl = xr_ast_var_decl(parser->compiler_session, first_name, initializer,
                                                false, saved_line);
                decl->column = saved_column;
                if (initializer && initializer->end_line > 0) {
                    decl->end_line = initializer->end_line;
                    decl->end_column = initializer->end_column;
                } else {
                    decl->end_line = saved_line;
                    decl->end_column = saved_column + (int) strlen(first_name);
                }
                decl->as.var_decl.type_annotation = var_type;  // Set type annotation
                return decl;
            }
        }

        xr_parser_error_expected_name(parser, "expected variable name");
        return NULL;
    }

    /* Constant declaration.
     *
     * `const` uses the same single-binding shape as `var`. Destructuring is the
     * explicit multi-name form; comma-separated declarations are intentionally
     * rejected to keep declaration lowering uniform.
     */
    if (xr_parser_match(parser, TK_CONST)) {
        // Check if destructure declaration
        if (xr_parser_check(parser, TK_LBRACKET) || xr_parser_check(parser, TK_LBRACE) ||
            xr_parser_check(parser, TK_LPAREN)) {
            return xr_parse_destructure_declaration(parser, true);
        }

        AstNode *first_decl = xr_parse_single_var_declaration(parser, 1);  // 1 means constant

        if (xr_parser_check(parser, TK_COMMA)) {
            xr_parser_error(parser,
                            "multi-const declaration is not supported; write separate const "
                            "declarations or use tuple destructure: `const (a, b) = ...`");
            return NULL;
        }

        return first_decl;
    }

    if (xr_parser_match(parser, TK_SHARED)) {
        return xr_parse_shared_declaration(parser);
    }

    if (xr_parser_match(parser, TK_OWNED)) {
        return xr_parse_owned_declaration(parser);
    }

    // Code block
    if (xr_parser_check(parser, TK_LBRACE) && xr_lbrace_starts_destructure_assignment(parser)) {
        return xr_parse_statement(parser);
    }
    if (xr_parser_match(parser, TK_LBRACE)) {
        return xr_parse_block(parser);
    }

    // Otherwise parse as statement
    return xr_parse_statement(parser);
}

// ========== Exception handling parse functions ==========

static const char *catch_pattern_enum_head_name(AstNode *pattern) {
    if (!pattern)
        return NULL;
    if (pattern->type == AST_PATTERN_ADT) {
        AstNode *variant = pattern->as.pattern_adt.variant;
        if (!variant)
            return NULL;
        if (variant->type == AST_ENUM_ACCESS)
            return variant->as.enum_access.enum_name;
        if (variant->type == AST_MEMBER_ACCESS && variant->as.member_access.object &&
            variant->as.member_access.object->type == AST_VARIABLE)
            return variant->as.member_access.object->as.variable.name;
        return NULL;
    }
    if (pattern->type != AST_PATTERN_LITERAL || !pattern->as.pattern_literal.value)
        return NULL;
    AstNode *value = pattern->as.pattern_literal.value;
    if (value->type == AST_VARIABLE)
        return value->as.variable.name;
    if (value->type == AST_ENUM_ACCESS)
        return value->as.enum_access.enum_name;
    if (value->type == AST_MEMBER_ACCESS && value->as.member_access.object &&
        value->as.member_access.object->type == AST_VARIABLE)
        return value->as.member_access.object->as.variable.name;
    return NULL;
}

/*
 * Parse try-catch statement.
 * Supports multiple typed catch clauses and an optional panic boundary:
 *   try { ... }
 *   catch (e: NetErr)    { ... }
 *   catch (e: DiskErr)   { ... }
 *   catch (e)            { ... }   // catch-all
 *   catch NetErr.NotFound(path) { ... } // variant/payload pattern
 *   catch NetErr          { ... }   // typed enum catch without binding
 *   catch panic (p)      { ... }   // recoverable-fault boundary
 * There is no `finally`; use `defer` for cleanup (runs on all exits).
 */
AstNode *xr_parse_try_statement(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_try_statement: NULL parser");
    int line = parser->current.line;

    // Consume 'try'
    xr_parser_advance(parser);

    // Parse try block
    xr_parser_consume(parser, TK_LBRACE, "expected '{' after try");
    AstNode *try_body = xr_parse_block(parser);

    // Parse zero or more catch clauses
    XrCatchClause **clauses = NULL;
    int catch_count = 0;
    int catch_cap = 0;

    while (xr_parser_check(parser, TK_CATCH)) {
        xr_parser_advance(parser);  // consume 'catch'

        /* `catch panic (p)` — panic boundary (recoverable-fault channel).
         * `panic` is a contextual keyword here, not a reserved word. */
        bool is_panic = false;
        if (parser->current.type == TK_NAME && parser->current.length == 5 &&
            memcmp(parser->current.start, "panic", 5) == 0) {
            is_panic = true;
            xr_parser_advance(parser);  // consume 'panic'
        }

        char *var_name = NULL;
        int var_line = parser->current.line;
        int var_column = parser->current.column;
        XrTypeRef *type_ann = NULL;
        AstNode *pattern = NULL;

        if (is_panic && !xr_parser_check(parser, TK_LPAREN)) {
            xr_parser_consume(parser, TK_LPAREN, "expected '(' after catch panic");
            return NULL;
        }

        if (xr_parser_match(parser, TK_LPAREN)) {
            if (parser->current.type != TK_NAME) {
                xr_parser_error_expected_name(parser, "expected catch variable name");
                return NULL;
            }

            // Save variable name and position
            var_name =
                (char *) ast_alloc(parser->compiler_session, (size_t) parser->current.length + 1);
            memcpy(var_name, parser->current.start, parser->current.length);
            var_name[parser->current.length] = '\0';
            var_line = parser->current.line;
            var_column = parser->current.column;
            xr_parser_advance(parser);  // consume variable name

            // Optional enum error filter annotation: catch (e: NetErr)
            if (xr_parser_match(parser, TK_COLON)) {
                type_ann = xr_parse_type_annotation(parser);
            }

            xr_parser_consume(parser, TK_RPAREN, "expected ')' after catch variable");
        } else {
            pattern = xr_parse_match_pattern(parser);
            if (!pattern) {
                xr_parser_error(parser, "expected catch pattern");
                return NULL;
            }
            var_line = pattern->line;
            var_column = pattern->column;
            const char *head = catch_pattern_enum_head_name(pattern);
            if (head)
                type_ann = xr_tref_named(parser->compiler_session, head);
        }

        // Parse catch body
        xr_parser_consume(parser, TK_LBRACE, "expected '{' after catch");
        AstNode *body = xr_parse_block(parser);

        XrCatchClause *clause = xr_ast_catch_clause(parser->compiler_session, var_name, var_line,
                                                    var_column, type_ann, body);
        clause->pattern = pattern;
        clause->is_panic = is_panic;
        XR_PARSE_PUSH(parser, clauses, catch_count, catch_cap, clause);
    }

    // Need at least one catch clause
    if (catch_count == 0) {
        xr_parser_error(parser, "try statement requires a catch block");
        return NULL;
    }

    AstNode *node =
        xr_ast_try_catch(parser->compiler_session, try_body, clauses, catch_count, line);
    // Span ends at the last block present (last catch > try).
    AstNode *last_block = clauses[catch_count - 1]->body;
    if (!last_block)
        last_block = try_body;
    if (last_block) {
        node->end_line = last_block->end_line;
        node->end_column = last_block->end_column;
    }
    return node;
}

/*
 * Parse throw statement
 * throw expr
 */
AstNode *xr_parse_throw_statement(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_throw_statement: NULL parser");
    int line = parser->current.line;

    // Consume 'throw'
    xr_parser_advance(parser);

    // Parse expression to throw
    AstNode *expression = xr_parse_expression(parser);

    if (!expression) {
        xr_parser_error(parser, "throw statement requires an expression");
        return NULL;
    }

    return xr_ast_throw_stmt(parser->compiler_session, expression, line);
}

// ========== Destructuring assignment helpers ==========

/*
 * Convert array literal to destructure pattern (for destructuring assignment)
 * [a, b, c] -> destructure pattern
 * Can only convert when all elements are variable references
 */
XrDestructurePattern *convert_array_literal_to_pattern(XrCompilerSession *X,
                                                       AstNode *array_literal) {
    if (array_literal->type != AST_ARRAY_LITERAL) {
        return NULL;
    }
    if (array_literal->as.array_literal.is_repeat)
        return NULL;

    int count = array_literal->as.array_literal.count;
    XrDestructurePattern **elements = (XrDestructurePattern **) ast_alloc_array(
        X, sizeof(XrDestructurePattern *), (size_t) count);

    for (int i = 0; i < count; i++) {
        AstNode *element = array_literal->as.array_literal.elements[i];

        // Check if element is variable reference
        if (element->type == AST_VARIABLE) {
            // Create identifier pattern
            elements[i] = xr_pattern_identifier(X, element->as.variable.name, NULL);
        } else {
            // Not a variable reference, cannot convert to destructure pattern
            return NULL;
        }
    }

    return xr_pattern_array(X, elements, count);
}

/*
 * Convert tuple literal to destructure pattern (for destructuring assignment)
 * (a, b, c) -> tuple destructure pattern
 * Mirrors convert_array_literal_to_pattern; only succeeds when every element
 * is a bare variable reference, since assignment targets cannot evaluate
 * sub-expressions.
 */
XrDestructurePattern *convert_tuple_literal_to_pattern(XrCompilerSession *X,
                                                       AstNode *tuple_literal) {
    if (tuple_literal->type != AST_TUPLE_LITERAL) {
        return NULL;
    }

    int count = tuple_literal->as.tuple_literal.count;
    XrDestructurePattern **elements = (XrDestructurePattern **) ast_alloc_array(
        X, sizeof(XrDestructurePattern *), (size_t) count);

    for (int i = 0; i < count; i++) {
        AstNode *element = tuple_literal->as.tuple_literal.elements[i];
        if (element->type == AST_VARIABLE) {
            elements[i] = xr_pattern_identifier(X, element->as.variable.name, NULL);
        } else {
            return NULL;
        }
    }

    return xr_pattern_tuple(X, elements, count);
}

/*
 * Convert object literal to destructure pattern (for destructuring assignment).
 * `{a, b}` and `{a: local}` both become object patterns. Field keys drive
 * extraction; values must be bare variable references so assignment targets
 * never evaluate arbitrary expressions.
 */
XrDestructurePattern *convert_object_literal_to_pattern(XrCompilerSession *X,
                                                        AstNode *object_literal) {
    if (object_literal->type != AST_OBJECT_LITERAL) {
        return NULL;
    }

    int count = object_literal->as.object_literal.count;
    char **field_names = (char **) ast_alloc_array(X, sizeof(char *), (size_t) count);
    XrDestructurePattern **patterns = (XrDestructurePattern **) ast_alloc_array(
        X, sizeof(XrDestructurePattern *), (size_t) count);
    bool all_shorthand = true;

    for (int i = 0; i < count; i++) {
        AstNode *key_node = object_literal->as.object_literal.keys[i];
        AstNode *value_node = object_literal->as.object_literal.values[i];

        // Check if key is string literal or variable reference
        char *field_name = NULL;
        if (key_node->type == AST_LITERAL_STRING) {
            // Key is string literal
            field_name = ast_strdup(X, key_node->as.literal.raw_value.string_val);
        } else if (key_node->type == AST_VARIABLE) {
            // Key is variable reference (shorthand syntax: {x, y})
            field_name = ast_strdup(X, key_node->as.variable.name);
        } else {
            // Key is not string or variable, cannot convert
            return NULL;
        }

        if (value_node->type == AST_VARIABLE) {
            if (strcmp(field_name, value_node->as.variable.name) != 0)
                all_shorthand = false;
            field_names[i] = field_name;
            patterns[i] = xr_pattern_identifier(X, value_node->as.variable.name, NULL);
        } else {
            // Value is not variable reference, cannot convert to destructure pattern
            return NULL;
        }
    }

    return xr_pattern_object(X, field_names, patterns, count, all_shorthand);
}

// Destructuring declaration/pattern parsing moved to xparse_destructure.c
