/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_type.c - Type annotation parsing (syntax-only)
 *
 * KEY CONCEPT:
 *   Parses type annotations and produces XrTypeRef (lightweight,
 *   arena-allocated AST type references).  No runtime XrType* objects
 *   are created here — resolution happens in the analyzer.
 */

#include "xparse_internal.h"
#include "xtype_ref.h"
#include "xtype_scope.h"
#include "../../runtime/xerror_codes.h"
#include "../../base/xchecks.h"
#include "../../base/xarena.h"
#include <stdlib.h>
#include <string.h>
#include "../../base/xmalloc.h"

/* ========== Helper Functions ========== */

/* Consume '>' in generic type context, handling '>>' (TK_RSHIFT) split.
 * When parsing Array<Array<int>>, the lexer tokenizes '>>' as TK_RSHIFT;
 * this function splits it into two '>' tokens. */
static bool consume_gt_in_generic(Parser *parser) {
    XR_DCHECK(parser != NULL, "consume_gt_in_generic: NULL parser");
    if (xr_parser_match(parser, TK_GT))
        return true;

    if (parser->current.type == TK_RSHIFT) {
        parser->previous = parser->current;
        parser->previous.type = TK_GT;
        parser->current.type = TK_GT;
        parser->current.start++;
        parser->current.length = 1;
        return true;
    }

    xr_parser_error(parser, "expected '>' (at '>>')");
    return false;
}

/* ========== Type Annotation Parsing (returns XrTypeRef) ========== */

static XrTypeRef *parse_type_annotation_base(Parser *parser);

/* ---- Top-level: base + optional '?' + optional '|' union ---- */

XR_FUNC XrTypeRef *xr_parse_type_annotation(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_type_annotation: NULL parser");
    XrTypeRef *base = parse_type_annotation_base(parser);

    /* Optional type suffix: T? */
    if (xr_parser_match(parser, TK_QUESTION))
        base = xr_tref_optional(parser->X, base);

    /* Union type: T | U | ... */
    if (xr_parser_check(parser, TK_PIPE)) {
        XrTypeRef *members[XR_TREF_UNION_MAX + 1];
        int count = 0;
        members[count++] = base;

        while (xr_parser_match(parser, TK_PIPE) && count < XR_TREF_UNION_MAX + 1) {
            XrTypeRef *next = parse_type_annotation_base(parser);
            if (xr_parser_match(parser, TK_QUESTION))
                next = xr_tref_optional(parser->X, next);
            if (count < XR_TREF_UNION_MAX + 1)
                members[count++] = next;
        }

        if (count > XR_TREF_UNION_MAX) {
            xr_parser_error(parser, "union type exceeds maximum of 6 members");
            return xr_tref_unknown(parser->X);
        }

        return xr_tref_union(parser->X, members, count);
    }

    return base;
}

/* ---- Base type (no trailing ? or |) ---- */

static XrTypeRef *parse_type_annotation_base(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_type_annotation_base: NULL parser");

    /* Fixed-length array type: [N]T */
    if (xr_parser_check(parser, TK_LBRACKET)) {
        Scanner saved = parser->scanner;
        Token saved_current = parser->current;
        xr_parser_advance(parser);
        if (parser->current.type == TK_LITERAL_INT) {
            int length = (int) strtol(parser->current.start, NULL, 10);
            xr_parser_advance(parser);
            if (xr_parser_match(parser, TK_RBRACKET)) {
                XrTypeRef *elem = parse_type_annotation_base(parser);
                return xr_tref_fixed_array(parser->X, elem, length);
            }
        }
        parser->scanner = saved;
        parser->current = saved_current;
    }

    /* Primitive type keywords */
    if (xr_parser_match(parser, TK_INT))
        return xr_tref_int(parser->X);
    if (xr_parser_match(parser, TK_FLOAT))
        return xr_tref_float(parser->X);
    if (xr_parser_match(parser, TK_STRING))
        return xr_tref_string(parser->X);
    if (xr_parser_match(parser, TK_BOOL))
        return xr_tref_bool(parser->X);
    if (xr_parser_match(parser, TK_NULL))
        return xr_tref_null(parser->X);

    /* Native-width integer types */
    if (xr_parser_match(parser, TK_INT8))
        return xr_tref_int_width(parser->X, XR_TREF_NW_I8);
    if (xr_parser_match(parser, TK_INT16))
        return xr_tref_int_width(parser->X, XR_TREF_NW_I16);
    if (xr_parser_match(parser, TK_INT32))
        return xr_tref_int_width(parser->X, XR_TREF_NW_I32);
    if (xr_parser_match(parser, TK_INT64))
        return xr_tref_int_width(parser->X, XR_TREF_NW_I64);
    if (xr_parser_match(parser, TK_UINT8))
        return xr_tref_int_width(parser->X, XR_TREF_NW_U8);
    if (xr_parser_match(parser, TK_UINT16))
        return xr_tref_int_width(parser->X, XR_TREF_NW_U16);
    if (xr_parser_match(parser, TK_UINT32))
        return xr_tref_int_width(parser->X, XR_TREF_NW_U32);
    if (xr_parser_match(parser, TK_UINT64))
        return xr_tref_int_width(parser->X, XR_TREF_NW_U64);

    /* Native-width float types */
    if (xr_parser_match(parser, TK_FLOAT32))
        return xr_tref_float_width(parser->X, XR_TREF_NW_F32);
    if (xr_parser_match(parser, TK_FLOAT64))
        return xr_tref_float_width(parser->X, XR_TREF_NW_F64);

    /* Struct type literal: { x: float, y: float } or { x: float, ... } */
    if (xr_parser_match(parser, TK_LBRACE)) {
        int capacity = 16;
        int field_count = 0;
        bool allow_extension = false;
        const char **fnames = xr_malloc((size_t) capacity * sizeof(const char *));
        XrTypeRef **ftypes = xr_malloc((size_t) capacity * sizeof(XrTypeRef *));
        bool *freadonly = xr_malloc((size_t) capacity * sizeof(bool));

        XR_CHECK(fnames != NULL && ftypes != NULL && freadonly != NULL,
                 "parse_type: alloc failed for struct literal fields");

        while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
            if (xr_parser_match(parser, TK_DOT_DOT_DOT)) {
                allow_extension = true;
                xr_parser_match(parser, TK_COMMA);
                continue;
            }
            if (field_count >= capacity) {
                int new_cap = capacity * 2;
                XR_REALLOC_OR_ABORT(fnames, (size_t) new_cap * sizeof(const char *),
                                    "parse_type field_names grow");
                XR_REALLOC_OR_ABORT(ftypes, (size_t) new_cap * sizeof(XrTypeRef *),
                                    "parse_type field_types grow");
                XR_REALLOC_OR_ABORT(freadonly, (size_t) new_cap * sizeof(bool),
                                    "parse_type field_readonly grow");
                capacity = new_cap;
            }
            bool is_const = xr_parser_match(parser, TK_CONST);
            xr_parser_consume(parser, TK_NAME, "expected field name");
            fnames[field_count] = strndup(parser->previous.start, parser->previous.length);

            bool is_optional = xr_parser_match(parser, TK_QUESTION);
            xr_parser_consume(parser, TK_COLON, "expected ':'");
            XrTypeRef *ftype = xr_parse_type_annotation(parser);
            if (is_optional)
                ftype = xr_tref_optional(parser->X, ftype);
            ftypes[field_count] = ftype;
            freadonly[field_count] = is_const;
            field_count++;
            xr_parser_match(parser, TK_COMMA);
        }
        xr_parser_consume(parser, TK_RBRACE, "expected '}'");

        XrTypeRef *result =
            xr_tref_object(parser->X, fnames, ftypes, freadonly, field_count, allow_extension);
        for (int i = 0; i < field_count; i++)
            xr_free((void *) fnames[i]);
        xr_free(fnames);
        xr_free(ftypes);
        xr_free(freadonly);
        return result;
    }

    /* Legacy `fn(T1, T2): R` form — removed by task 082. Function types are
     * now written `(T1, T2) -> R` (no `fn` prefix). Emit a clear migration
     * hint and recover by parsing the rest as a function type. */
    if (xr_parser_check(parser, TK_FN)) {
        xr_parser_error(parser,
                        "function types are written `(T1, T2) -> R` (drop the `fn` prefix); "
                        "see task 082");
        xr_parser_advance(parser);  // consume 'fn' so caller can keep parsing
        /* fall through to the `(...)` path below */
    }

    /* Parenthesized type starting with `(` covers three grammar forms:
     *   ()             -> unit (canonical procedure return type)
     *   (T1, T2)       -> tuple
     *   (T1, T2) -> R  -> function type
     *
     * The empty form `()` is *not* an empty tuple: xr_tref_tuple asserts
     * count > 0, so we explicitly decode it to xr_tref_unit, matching
     * what the function-return path in xparse_decl manufactures when the
     * colon is omitted (e.g. `fn deep(): () { throw "boom" }`). Tuple
     * and function type share the leading `(` and the same internal
     * type-list grammar, so we collect the list once and branch on the
     * trailing `->`. */
    if (xr_parser_match(parser, TK_LPAREN)) {
        if (xr_parser_match(parser, TK_RPAREN)) {
            // `() -> R` is a zero-arity function type; bare `()` is unit.
            if (xr_parser_match(parser, TK_ARROW)) {
                XrTypeRef *ret = xr_parse_type_annotation(parser);
                return xr_tref_function(parser->X, NULL, 0, ret);
            }
            return xr_tref_unit(parser->X);
        }
        XrTypeRef *elems[16];
        int count = 0;
        while (!xr_parser_check(parser, TK_RPAREN) && !xr_parser_check(parser, TK_EOF)) {
            if (count > 0)
                xr_parser_match(parser, TK_COMMA);
            if (count < 16)
                elems[count++] = xr_parse_type_annotation(parser);
        }
        xr_parser_consume(parser, TK_RPAREN, "expected ')'");
        if (xr_parser_match(parser, TK_ARROW)) {
            XrTypeRef *ret = xr_parse_type_annotation(parser);
            return xr_tref_function(parser->X, elems, count, ret);
        }
        // `()` with no trailing `->` is the unit type, not an empty tuple.
        if (count == 0)
            return xr_tref_unit(parser->X);
        return xr_tref_tuple(parser->X, elems, count);
    }

    /* Identifier: class / enum / prelude / alias / generic params */
    if (xr_parser_match(parser, TK_NAME)) {
        Token name_token = parser->previous;
        char temp_name[256];
        int name_len = name_token.length < 255 ? name_token.length : 255;
        strncpy(temp_name, name_token.start, (size_t) name_len);
        temp_name[name_len] = '\0';

        /* Misspelling detection (purely syntactic, kept in parser) */
        if (strcmp(temp_name, "JsonValue") == 0) {
            xr_parser_error(parser, "Type 'JsonValue' has been removed. Use 'Json' instead.");
            return xr_tref_named(parser->X, "Json");
        }
        if (strcmp(temp_name, "any") == 0) {
            xr_parser_error(parser, "'any' type is not supported. "
                                    "Use a concrete type or 'Json' for dynamic values.");
            return xr_tref_unknown(parser->X);
        }
        if (strcmp(temp_name, "String") == 0 || strcmp(temp_name, "str") == 0) {
            xr_parser_error(parser, "type 'string' must be lowercase in Xray");
            return xr_tref_string(parser->X);
        }
        if (strcmp(temp_name, "Int") == 0 || strcmp(temp_name, "Integer") == 0 ||
            strcmp(temp_name, "integer") == 0) {
            xr_parser_error(parser, "use 'int' (lowercase) for integer type in Xray");
            return xr_tref_int(parser->X);
        }
        if (strcmp(temp_name, "Float") == 0 || strcmp(temp_name, "Double") == 0 ||
            strcmp(temp_name, "double") == 0) {
            xr_parser_error(parser, "use 'float' (lowercase) for floating-point type in Xray");
            return xr_tref_float(parser->X);
        }
        if (strcmp(temp_name, "Bool") == 0 || strcmp(temp_name, "Boolean") == 0 ||
            strcmp(temp_name, "boolean") == 0) {
            xr_parser_error(parser, "use 'bool' (lowercase) for boolean type in Xray");
            return xr_tref_bool(parser->X);
        }
        if (strcmp(temp_name, "char") == 0 || strcmp(temp_name, "Char") == 0) {
            xr_parser_error(parser, "there is no 'char' type in Xray. Use 'string' for characters");
            return xr_tref_string(parser->X);
        }
        if (strcmp(temp_name, "void") == 0) {
            xr_parser_emit_removed_syntax(
                parser, &name_token, XR_ERR_SYN_VOID_REMOVED, "`void` keyword was removed",
                "use Unit type `()` instead - xray uses 0-arity tuple as Unit");
            return xr_tref_unit(parser->X);
        }

        /* Generic type arguments: Name<T1, T2, ...> */
        if (xr_parser_match(parser, TK_LT)) {
            XrTypeRef *type_args[16];
            int type_arg_count = 0;
            do {
                if (type_arg_count < 16)
                    type_args[type_arg_count++] = xr_parse_type_annotation(parser);
            } while (xr_parser_match(parser, TK_COMMA));
            consume_gt_in_generic(parser);
            return xr_tref_generic(parser->X, temp_name, type_args, type_arg_count);
        }

        /* Check parser's type scope for aliases and generic params */
        if (parser->type_scope) {
            XrTypeRef *alias = xr_type_scope_resolve(parser->type_scope, temp_name);
            if (alias)
                return alias;
        }

        /* Plain named type (class, enum, prelude — resolved in analyzer) */
        return xr_tref_named(parser->X, temp_name);
    }

    /* Error recovery */
    xr_parser_error_expected_name(parser, "expected type name");
    return xr_tref_unknown(parser->X);
}

/* Parse one or more interface constraints joined by '&'.
 *
 *   T: Comparable                         -> [Comparable]
 *   T: Comparable & Hashable              -> [Comparable, Hashable]
 *   T: Comparable & Hashable & Stringable -> [Comparable, Hashable, Stringable]
 *
 * The leading ':' must already have been matched by the caller.  All
 * constraints are intersected: a type satisfies the parameter only when it
 * satisfies every listed constraint. */
XrTypeRef **xr_parse_constraint_list(Parser *parser, int *out_count) {
    XR_DCHECK(parser != NULL, "xr_parse_constraint_list: NULL parser");
    XR_DCHECK(out_count != NULL, "xr_parse_constraint_list: NULL out_count");

    XrTypeRef **list = NULL;
    int count = 0;
    int capacity = 0;

    do {
        XrTypeRef *constraint = xr_parse_type_annotation(parser);
        if (constraint == NULL)
            break;
        XR_PARSE_PUSH(parser, list, count, capacity, constraint);
    } while (xr_parser_match(parser, TK_AMP));

    *out_count = count;
    return list;
}
