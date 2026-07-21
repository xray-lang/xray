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
#include <limits.h>
#include <stdio.h>
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

static int alias_param_index(const XrTypeAlias *alias, const char *name) {
    if (!alias || !name)
        return -1;
    for (int i = 0; i < alias->type_param_count; i++) {
        if (alias->type_param_names && alias->type_param_names[i] &&
            strcmp(alias->type_param_names[i], name) == 0)
            return i;
    }
    return -1;
}

static XrTypeRef *clone_subst_type_ref(Parser *parser, const XrTypeRef *src,
                                       const XrTypeAlias *subst_alias, XrTypeRef **type_args);

static void report_removed_source_type_name(Parser *parser, const char *message) {
    if (!parser || !message)
        return;
    if (parser->panic_mode && parser->had_error)
        return;

    int saved_panic_mode = parser->panic_mode;
    parser->panic_mode = 0;
    xr_parser_error(parser, message);
    if (saved_panic_mode)
        parser->panic_mode = 1;
}

static bool reject_removed_source_type_name(Parser *parser, const char *name) {
    if (!name)
        return false;
    if (strcmp(name, "JsonValue") == 0) {
        report_removed_source_type_name(parser,
                                        "Type 'JsonValue' has been removed. Use 'Json' instead.");
        return true;
    }
    if (strcmp(name, "any") == 0) {
        report_removed_source_type_name(
            parser,
            "'any' type is not supported. Use a concrete type or 'Json' for dynamic values.");
        return true;
    }
    if (strcmp(name, "unknown") == 0) {
        report_removed_source_type_name(
            parser,
            "'unknown' type has been removed. Use a concrete type, a generic type parameter, or "
            "'Json' for JSON-domain values.");
        return true;
    }
    return false;
}

static XrTypeRef *expand_type_alias(Parser *parser, XrTypeAlias *alias, XrTypeRef **type_args,
                                    int type_arg_count) {
    if (!alias)
        return xr_tref_error(parser->compiler_session);

    if (!alias->type_ref) {
        char msg[256];
        snprintf(msg, sizeof(msg), "circular type alias '%s'", alias->name ? alias->name : "?");
        xr_parser_error(parser, msg);
        return xr_tref_error(parser->compiler_session);
    }

    if (alias->type_param_count != type_arg_count) {
        char msg[256];
        if (alias->type_param_count == 0) {
            snprintf(msg, sizeof(msg), "type alias '%s' does not take type arguments",
                     alias->name ? alias->name : "?");
        } else {
            snprintf(msg, sizeof(msg), "generic type alias '%s' expects %d type arguments, got %d",
                     alias->name ? alias->name : "?", alias->type_param_count, type_arg_count);
        }
        xr_parser_error(parser, msg);
        return xr_tref_error(parser->compiler_session);
    }

    if (alias->is_expanding) {
        char msg[256];
        snprintf(msg, sizeof(msg), "circular type alias '%s'", alias->name ? alias->name : "?");
        xr_parser_error(parser, msg);
        return xr_tref_error(parser->compiler_session);
    }

    alias->is_expanding = true;
    XrTypeRef *result = clone_subst_type_ref(parser, alias->type_ref, alias, type_args);
    alias->is_expanding = false;
    return result;
}

static XrTypeRef *expand_named_or_clone(Parser *parser, const char *name) {
    if (parser->type_scope) {
        XrTypeAlias *alias = xr_type_scope_lookup(parser->type_scope, name);
        if (alias)
            return expand_type_alias(parser, alias, NULL, 0);
    }
    return xr_tref_named(parser->compiler_session, name);
}

static XrTypeRef *expand_generic_or_clone(Parser *parser, const char *name, XrTypeRef **args,
                                          int arg_count) {
    if (parser->type_scope) {
        XrTypeAlias *alias = xr_type_scope_lookup(parser->type_scope, name);
        if (alias)
            return expand_type_alias(parser, alias, args, arg_count);
    }
    return xr_tref_generic(parser->compiler_session, name, args, arg_count);
}

XR_FUNC XrTypeRef *xr_parse_type_name_ref(Parser *parser, const char *name) {
    XR_DCHECK(parser != NULL, "xr_parse_type_name_ref: NULL parser");
    if (reject_removed_source_type_name(parser, name))
        return xr_tref_error(parser->compiler_session);
    return expand_named_or_clone(parser, name);
}

XR_FUNC XrTypeRef *xr_parse_generic_type_name_ref(Parser *parser, const char *name,
                                                  XrTypeRef **args, int arg_count) {
    XR_DCHECK(parser != NULL, "xr_parse_generic_type_name_ref: NULL parser");
    if (reject_removed_source_type_name(parser, name))
        return xr_tref_error(parser->compiler_session);
    return expand_generic_or_clone(parser, name, args, arg_count);
}

static XrTypeRef *clone_subst_type_ref(Parser *parser, const XrTypeRef *src,
                                       const XrTypeAlias *subst_alias, XrTypeRef **type_args) {
    if (!src)
        return xr_tref_error(parser->compiler_session);

    switch ((XrTypeRefKind) src->kind) {
        case XR_TREF_INT:
            return xr_tref_int(parser->compiler_session);
        case XR_TREF_FLOAT:
            return xr_tref_float(parser->compiler_session);
        case XR_TREF_STRING:
            return xr_tref_string(parser->compiler_session);
        case XR_TREF_BOOL:
            return xr_tref_bool(parser->compiler_session);
        case XR_TREF_RUNE:
            return xr_tref_char(parser->compiler_session);
        case XR_TREF_UNIT:
            return xr_tref_unit(parser->compiler_session);
        case XR_TREF_NULL:
            return xr_tref_null(parser->compiler_session);
        case XR_TREF_ERROR:
            return xr_tref_error(parser->compiler_session);
        case XR_TREF_INT_WIDTH:
            return xr_tref_int_width(parser->compiler_session, src->native_width);
        case XR_TREF_FLOAT_WIDTH:
            return xr_tref_float_width(parser->compiler_session, src->native_width);
        case XR_TREF_TYPE_PARAM: {
            int idx = alias_param_index(subst_alias, src->name);
            if (idx >= 0 && type_args)
                return clone_subst_type_ref(parser, type_args[idx], NULL, NULL);
            return xr_tref_type_param(parser->compiler_session, src->name ? src->name : "?");
        }
        case XR_TREF_NAMED:
            return expand_named_or_clone(parser, src->name ? src->name : "?");
        case XR_TREF_GENERIC: {
            XrTypeRef *args[256];
            int count = src->nchildren;
            for (int i = 0; i < count; i++)
                args[i] = clone_subst_type_ref(parser, src->children[i], subst_alias, type_args);
            return expand_generic_or_clone(parser, src->name ? src->name : "?", args, count);
        }
        case XR_TREF_OPTIONAL: {
            XrTypeRef *inner = src->nchildren > 0 ? clone_subst_type_ref(parser, src->children[0],
                                                                         subst_alias, type_args)
                                                  : xr_tref_error(parser->compiler_session);
            return xr_tref_optional(parser->compiler_session, inner);
        }
        case XR_TREF_UNION: {
            XrTypeRef *members[256];
            int count = src->nchildren;
            for (int i = 0; i < count; i++)
                members[i] = clone_subst_type_ref(parser, src->children[i], subst_alias, type_args);
            return xr_tref_union(parser->compiler_session, members, count);
        }
        case XR_TREF_FUNCTION: {
            int total = src->nchildren;
            if (total <= 0)
                return xr_tref_function(parser->compiler_session, NULL, 0,
                                        xr_tref_unit(parser->compiler_session));
            XrTypeRef *params[256];
            XrParamMode modes[256];
            int nparam = total - 1;
            for (int i = 0; i < nparam; i++) {
                params[i] = clone_subst_type_ref(parser, src->children[i], subst_alias, type_args);
                modes[i] =
                    src->function_param_modes ? src->function_param_modes[i] : XR_PARAM_VALUE;
            }
            XrTypeRef *ret =
                clone_subst_type_ref(parser, src->children[total - 1], subst_alias, type_args);
            return xr_tref_function_with_modes(parser->compiler_session, params, modes, nparam,
                                               ret);
        }
        case XR_TREF_TUPLE: {
            XrTypeRef *elems[256];
            int count = src->nchildren;
            for (int i = 0; i < count; i++)
                elems[i] = clone_subst_type_ref(parser, src->children[i], subst_alias, type_args);
            return xr_tref_tuple(parser->compiler_session, elems, count);
        }
        case XR_TREF_OBJECT: {
            XrTypeRef *fields[256];
            int count = src->nchildren;
            for (int i = 0; i < count; i++)
                fields[i] = clone_subst_type_ref(parser, src->children[i], subst_alias, type_args);
            XrTypeRef *obj = xr_tref_object(parser->compiler_session, src->field_names, fields,
                                            src->field_readonly, count, src->extensible);
            if (src->name)
                obj->name = ast_strdup(parser->compiler_session, src->name);
            return obj;
        }
        case XR_TREF_FIXED_ARRAY: {
            XrTypeRef *elem = src->nchildren > 0 ? clone_subst_type_ref(parser, src->children[0],
                                                                        subst_alias, type_args)
                                                 : xr_tref_error(parser->compiler_session);
            return xr_tref_fixed_array_expr(parser->compiler_session, elem, src->fixed_length_expr,
                                            src->fixed_length);
        }
    }
    return xr_tref_error(parser->compiler_session);
}

/* ========== Type Annotation Parsing (returns XrTypeRef) ========== */

static XrTypeRef *parse_type_annotation_base(Parser *parser);

static bool tref_is_json(const XrTypeRef *t) {
    return t && t->kind == XR_TREF_NAMED && t->name && strcmp(t->name, "Json") == 0;
}

static bool tref_is_null(const XrTypeRef *t) {
    return t && t->kind == XR_TREF_NULL;
}

static bool tref_intrinsically_includes_null(const XrTypeRef *t) {
    return tref_is_json(t);
}

static XrTypeRef *parse_nullable_suffix(Parser *parser, XrTypeRef *base) {
    if (tref_intrinsically_includes_null(base)) {
        xr_parser_error(parser, "Json already includes null; use 'Json' instead of 'Json?'");
        return base;
    }
    return xr_tref_optional(parser->compiler_session, base);
}

static void reject_redundant_null_union(Parser *parser, XrTypeRef **members, int count) {
    bool has_null = false;
    bool has_intrinsic_null = false;
    for (int i = 0; i < count; i++) {
        if (tref_is_null(members[i]))
            has_null = true;
        if (tref_intrinsically_includes_null(members[i]))
            has_intrinsic_null = true;
    }
    if (has_null && has_intrinsic_null) {
        xr_parser_error(parser, "Json already includes null; use 'Json' instead of 'Json | null'");
    }
}

/* ---- Top-level: base + optional '?' + optional '|' union ---- */

// Inner implementation; public xr_parse_type_annotation wraps this with the
// recursion-depth guard. All nested type arguments recurse through the
// public entry, so the guard bounds type nesting depth.
static XrTypeRef *parse_type_annotation_inner(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_type_annotation: NULL parser");
    XrTypeRef *base = parse_type_annotation_base(parser);

    /* Optional type suffix: T? */
    if (xr_parser_match(parser, TK_QUESTION))
        base = parse_nullable_suffix(parser, base);

    /* Union type: T | U | ... */
    if (xr_parser_check(parser, TK_PIPE)) {
        XrTypeRef *members[XR_TREF_UNION_MAX + 1];
        int count = 0;
        members[count++] = base;

        while (xr_parser_match(parser, TK_PIPE) && count < XR_TREF_UNION_MAX + 1) {
            XrTypeRef *next = parse_type_annotation_base(parser);
            if (xr_parser_match(parser, TK_QUESTION))
                next = parse_nullable_suffix(parser, next);
            if (count < XR_TREF_UNION_MAX + 1)
                members[count++] = next;
        }

        if (count > XR_TREF_UNION_MAX) {
            xr_parser_error(parser, "union type exceeds maximum of 6 members");
            return xr_tref_error(parser->compiler_session);
        }

        reject_redundant_null_union(parser, members, count);
        return xr_tref_union(parser->compiler_session, members, count);
    }

    return base;
}

XR_FUNC XrTypeRef *xr_parse_type_annotation(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_type_annotation: NULL parser");
    if (++parser->recursion_depth > XR_PARSER_MAX_DEPTH) {
        parser->recursion_depth--;
        xr_parser_error(parser, "type nesting too deep (max 1000 levels)");
        return xr_tref_error(parser->compiler_session);
    }
    XrTypeRef *result = parse_type_annotation_inner(parser);
    parser->recursion_depth--;
    return result;
}

static bool xr_parse_optional_param_mode(Parser *parser, bool allow_mode, XrParamMode *out_mode) {
    if (out_mode)
        *out_mode = XR_PARAM_VALUE;
    if (!allow_mode)
        return false;

    const char *mode = NULL;
    if (xr_parser_check(parser, TK_IN)) {
        mode = "in";
        if (out_mode)
            *out_mode = XR_PARAM_IN;
    } else if (xr_parser_check(parser, TK_REF) || xr_parser_check_name(parser, "ref")) {
        mode = "ref";
        if (out_mode)
            *out_mode = XR_PARAM_REF;
    } else if (xr_parser_check_name(parser, "out")) {
        mode = "out";
        if (out_mode)
            *out_mode = XR_PARAM_OUT;
    }

    if (mode) {
        xr_parser_advance(parser);
        if (xr_parser_check(parser, TK_MOVE) || xr_parser_check_name(parser, "move")) {
            Token move_token = parser->current;
            xr_parser_advance(parser);
            xr_parser_emit_removed_syntax(
                parser, &move_token, XR_ERR_SYN_PARAM_MOVE_MODE_REMOVED,
                "`move` is not a parameter mode",
                "Use a value parameter and write `move value` at the call site when transferring "
                "ownership.");
        } else {
            const char *next_mode = NULL;
            if (xr_parser_check(parser, TK_IN)) {
                next_mode = "in";
            } else if (xr_parser_check(parser, TK_REF) || xr_parser_check_name(parser, "ref")) {
                next_mode = "ref";
            } else if (xr_parser_check_name(parser, "out")) {
                next_mode = "out";
            }
            if (next_mode) {
                Token mode_token = parser->current;
                xr_parser_advance(parser);
                (void) next_mode;
                xr_parser_emit_removed_syntax(
                    parser, &mode_token, XR_ERR_SYN_PARAM_MODE_COMBINED_REMOVED,
                    "parameter modes cannot be combined",
                    "Use exactly one parameter mode after the colon, for example `name: ref T`.");
            }
        }
        return true;
    }

    if (xr_parser_check(parser, TK_MOVE) || xr_parser_check_name(parser, "move")) {
        Token move_token = parser->current;
        xr_parser_advance(parser);
        xr_parser_emit_removed_syntax(
            parser, &move_token, XR_ERR_SYN_PARAM_MOVE_MODE_REMOVED,
            "`move` is not a parameter mode",
            "Use a value parameter and write `move value` at the call site when transferring "
            "ownership.");
    }
    return false;
}

static void xr_parse_reject_postfix_param_mode(Parser *parser) {
    const char *mode = NULL;
    if (xr_parser_check(parser, TK_IN)) {
        mode = "in";
    } else if (xr_parser_check(parser, TK_REF) || xr_parser_check_name(parser, "ref")) {
        mode = "ref";
    } else if (xr_parser_check_name(parser, "out")) {
        mode = "out";
    }

    if (mode) {
        Token mode_token = parser->current;
        xr_parser_advance(parser);
        char title[128];
        snprintf(title, sizeof(title), "parameter mode '%s' after parameter type was removed",
                 mode);
        xr_parser_emit_removed_syntax(
            parser, &mode_token, XR_ERR_SYN_PARAM_MODE_POSTFIX_REMOVED, title,
            "Write parameter modes immediately after the colon, for example `name: ref T`.");
        return;
    }

    if (xr_parser_check(parser, TK_MOVE) || xr_parser_check_name(parser, "move")) {
        Token move_token = parser->current;
        xr_parser_advance(parser);
        xr_parser_emit_removed_syntax(
            parser, &move_token, XR_ERR_SYN_PARAM_MOVE_MODE_REMOVED,
            "`move` is not a parameter mode",
            "Use a value parameter and write `move value` at the call site when transferring "
            "ownership.");
    }
}

XR_FUNC bool xr_parse_optional_param_type_annotation(Parser *parser, bool allow_mode,
                                                     XrParamMode *out_mode, XrTypeRef **out_type) {
    XR_DCHECK(parser != NULL, "xr_parse_optional_param_type_annotation: NULL parser");
    if (out_mode)
        *out_mode = XR_PARAM_VALUE;
    if (out_type)
        *out_type = NULL;
    if (!xr_parser_match(parser, TK_COLON))
        return false;

    XrParamMode mode = XR_PARAM_VALUE;
    if (allow_mode) {
        xr_parse_optional_param_mode(parser, true, &mode);
    } else {
        XrParamMode rejected_mode = XR_PARAM_VALUE;
        if (xr_parse_optional_param_mode(parser, true, &rejected_mode) &&
            rejected_mode != XR_PARAM_VALUE) {
            char message[128];
            snprintf(message, sizeof(message),
                     "parameter mode '%s' is not allowed in this parameter position",
                     xr_param_mode_label(rejected_mode));
            xr_parser_error_at_previous(parser, message);
        }
    }

    XrTypeRef *type = xr_parse_type_annotation(parser);
    xr_parse_reject_postfix_param_mode(parser);
    if (out_mode)
        *out_mode = mode;
    if (out_type)
        *out_type = type;
    return true;
}

static bool xr_parse_current_is_param_mode_prefix(Parser *parser, const char **out_mode) {
    const char *mode = NULL;
    if (xr_parser_check(parser, TK_IN)) {
        mode = "in";
    } else if (xr_parser_check(parser, TK_REF) || xr_parser_check_name(parser, "ref")) {
        mode = "ref";
    } else if (xr_parser_check_name(parser, "out")) {
        mode = "out";
    } else {
        return false;
    }

    Scanner saved_scan = parser->scanner;
    Token saved_cur = parser->current;
    Token saved_prev = parser->previous;
    xr_parser_advance(parser);
    bool is_prefix = xr_parser_check(parser, TK_NAME);
    parser->scanner = saved_scan;
    parser->current = saved_cur;
    parser->previous = saved_prev;
    if (!is_prefix)
        return false;
    if (out_mode)
        *out_mode = mode;
    return true;
}

static bool xr_parse_current_is_move_param_prefix(Parser *parser) {
    if (!xr_parser_check(parser, TK_MOVE) && !xr_parser_check_name(parser, "move"))
        return false;

    Scanner saved_scan = parser->scanner;
    Token saved_cur = parser->current;
    Token saved_prev = parser->previous;
    xr_parser_advance(parser);
    bool is_prefix = xr_parser_check(parser, TK_NAME);
    parser->scanner = saved_scan;
    parser->current = saved_cur;
    parser->previous = saved_prev;
    return is_prefix;
}

XR_FUNC void xr_parse_reject_ref_out_default_param(Parser *parser, const XrParamNode *param) {
    if (!param || (param->passing_mode != XR_PARAM_REF && param->passing_mode != XR_PARAM_OUT))
        return;

    const char *mode = param->passing_mode == XR_PARAM_REF ? "ref" : "out";
    const char *name = param->name ? param->name : "<anonymous>";
    char message[192];
    snprintf(message, sizeof(message),
             "%s parameter '%s' cannot have a default value; callers must pass %s place "
             "explicitly",
             mode, name, mode);
    xr_parser_error_at_previous(parser, message);
}

static bool xr_parse_parameter_starts_destructure(Parser *parser) {
    return xr_parser_check(parser, TK_LBRACKET) || xr_parser_check(parser, TK_LBRACE) ||
           xr_parser_check(parser, TK_LPAREN);
}

XR_FUNC XrParamNode *xr_parse_parameter_at(Parser *parser, uint32_t flags, int param_index) {
    XR_DCHECK(parser != NULL, "xr_parse_parameter_at: NULL parser");
    bool is_rest = false;
    if (flags & XR_PARSE_PARAMETER_ALLOW_REST)
        is_rest = xr_parser_match(parser, TK_DOT_DOT_DOT);

    if (!is_rest && xr_parse_current_is_move_param_prefix(parser)) {
        Token move_token = parser->current;
        xr_parser_advance(parser);
        xr_parser_emit_removed_syntax(
            parser, &move_token, XR_ERR_SYN_PARAM_MOVE_MODE_REMOVED,
            "`move` is not a parameter mode",
            "Use a value parameter and write `move value` at the call site when transferring "
            "ownership.");
    }

    const char *prefix_mode = NULL;
    if (!is_rest && xr_parse_current_is_param_mode_prefix(parser, &prefix_mode)) {
        Token mode_token = parser->current;
        xr_parser_advance(parser);
        char title[128];
        snprintf(title, sizeof(title), "parameter mode '%s' before parameter name was removed",
                 prefix_mode ? prefix_mode : "?");
        xr_parser_emit_removed_syntax(
            parser, &mode_token, XR_ERR_SYN_PARAM_MODE_PREFIX_REMOVED, title,
            "Write parameter modes after the colon, for example `name: ref T`.");
    }

    if (!is_rest && (flags & XR_PARSE_PARAMETER_ALLOW_DESTRUCTURE) &&
        xr_parse_parameter_starts_destructure(parser)) {
        int pattern_line = parser->current.line;
        int pattern_column = parser->current.column;
        XrDestructurePattern *pattern = xr_parse_destructure_pattern(parser);
        if (!pattern) {
            xr_parser_error(parser, "failed to parse destructure parameter");
            return NULL;
        }

        char temp_name[32];
        snprintf(temp_name, sizeof(temp_name), "__param%d", param_index >= 0 ? param_index : 0);

        XrParamNode *param =
            xr_param_node_new(parser->compiler_session, temp_name, pattern_line, pattern_column);
        param->pattern = pattern;

        xr_parse_optional_param_type_annotation(parser, false, &param->passing_mode, &param->type);
        return param;
    }

    xr_parser_consume(parser, TK_NAME,
                      is_rest ? "expected parameter name after ..." : "expected parameter name");
    Token name_token = parser->previous;

    char param_name[256];
    snprintf(param_name, sizeof(param_name), "%.*s", name_token.length, name_token.start);

    XrParamNode *param =
        xr_param_node_new(parser->compiler_session, param_name, name_token.line, name_token.column);
    param->is_rest = is_rest;

    bool allow_mode = (flags & XR_PARSE_PARAMETER_ALLOW_MODE) && !is_rest;
    bool has_type = xr_parse_optional_param_type_annotation(parser, allow_mode,
                                                            &param->passing_mode, &param->type);
    if ((flags & XR_PARSE_PARAMETER_REQUIRE_TYPE) && !has_type)
        xr_parser_error(parser, "expected ':' and type annotation after parameter name");
    return param;
}

XR_FUNC XrParamNode *xr_parse_parameter(Parser *parser, uint32_t flags) {
    return xr_parse_parameter_at(parser, flags, -1);
}

/* ---- Base type (no trailing ? or |) ---- */

static XrTypeRef *parse_type_annotation_base(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_type_annotation_base: NULL parser");

    /* Fixed-length array type: [T; N] */
    if (xr_parser_check(parser, TK_LBRACKET)) {
        xr_parser_advance(parser);
        if (parser->current.type == TK_LITERAL_INT) {
            xr_parser_error(parser, "fixed array type uses [T; N], e.g. [uint8; 64]");
            return xr_tref_error(parser->compiler_session);
        }
        XrTypeRef *elem = xr_parse_type_annotation(parser);
        xr_parser_consume(parser, TK_SEMICOLON, "expected ';' before fixed array length in [T; N]");
        AstNode *length_expr = xr_parse_expression(parser);
        if (!length_expr) {
            xr_parser_error(parser, "fixed array length must be a compile-time integer expression");
            return xr_tref_error(parser->compiler_session);
        }
        int literal_length = 0;
        if (length_expr->type == AST_LITERAL_INT) {
            int64_t raw = length_expr->as.literal.raw_value.int_val;
            if (raw > 0 && raw <= INT_MAX)
                literal_length = (int) raw;
        }
        xr_parser_consume(parser, TK_RBRACKET, "expected ']' after fixed array length");
        return xr_tref_fixed_array_expr(parser->compiler_session, elem, length_expr,
                                        literal_length);
    }

    /* Primitive type keywords */
    if (xr_parser_match(parser, TK_INT))
        return xr_tref_int(parser->compiler_session);
    if (xr_parser_match(parser, TK_FLOAT))
        return xr_tref_float(parser->compiler_session);
    if (xr_parser_match(parser, TK_STRING))
        return xr_tref_string(parser->compiler_session);
    if (xr_parser_match(parser, TK_BOOL))
        return xr_tref_bool(parser->compiler_session);
    if (xr_parser_match(parser, TK_RUNE))
        return xr_tref_char(parser->compiler_session);
    if (xr_parser_match(parser, TK_NULL))
        return xr_tref_null(parser->compiler_session);

    /* Native-width integer types */
    if (xr_parser_match(parser, TK_INT8))
        return xr_tref_int_width(parser->compiler_session, XR_TREF_NW_I8);
    if (xr_parser_match(parser, TK_INT16))
        return xr_tref_int_width(parser->compiler_session, XR_TREF_NW_I16);
    if (xr_parser_match(parser, TK_INT32))
        return xr_tref_int_width(parser->compiler_session, XR_TREF_NW_I32);
    if (xr_parser_match(parser, TK_INT64))
        return xr_tref_int_width(parser->compiler_session, XR_TREF_NW_I64);
    if (xr_parser_match(parser, TK_UINT8))
        return xr_tref_int_width(parser->compiler_session, XR_TREF_NW_U8);
    if (xr_parser_match(parser, TK_UINT16))
        return xr_tref_int_width(parser->compiler_session, XR_TREF_NW_U16);
    if (xr_parser_match(parser, TK_UINT32))
        return xr_tref_int_width(parser->compiler_session, XR_TREF_NW_U32);
    if (xr_parser_match(parser, TK_UINT64))
        return xr_tref_int_width(parser->compiler_session, XR_TREF_NW_U64);

    /* Native-width float types */
    if (xr_parser_match(parser, TK_FLOAT32))
        return xr_tref_float_width(parser->compiler_session, XR_TREF_NW_F32);
    if (xr_parser_match(parser, TK_FLOAT64))
        return xr_tref_float_width(parser->compiler_session, XR_TREF_NW_F64);

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
            if (!xr_parser_check(parser, TK_NAME)) {
                xr_parser_error(parser, "expected field name");
                break;
            }
            xr_parser_advance(parser);
            fnames[field_count] = strndup(parser->previous.start, parser->previous.length);

            bool is_optional = xr_parser_match(parser, TK_QUESTION);
            if (!xr_parser_match(parser, TK_COLON)) {
                xr_parser_error(parser, "expected ':'");
                xr_free((void *) fnames[field_count]);
                break;
            }
            XrTypeRef *ftype = xr_parse_type_annotation(parser);
            if (is_optional)
                ftype = xr_tref_optional(parser->compiler_session, ftype);
            ftypes[field_count] = ftype;
            freadonly[field_count] = is_const;
            field_count++;
            xr_parser_match(parser, TK_COMMA);
        }
        xr_parser_consume(parser, TK_RBRACE, "expected '}'");

        XrTypeRef *result = xr_tref_object(parser->compiler_session, fnames, ftypes, freadonly,
                                           field_count, allow_extension);
        for (int i = 0; i < field_count; i++)
            xr_free((void *) fnames[i]);
        xr_free(fnames);
        xr_free(ftypes);
        xr_free(freadonly);
        return result;
    }

    /* Legacy `fn(T1, T2): R` form is no longer accepted. Function types are
     * now written `(T1, T2) -> R` (no `fn` prefix). Emit a clear migration
     * hint and recover by parsing the rest as a function type. */
    if (xr_parser_check(parser, TK_FN)) {
        xr_parser_error(parser,
                        "function types are written `(T1, T2) -> R` (drop the `fn` prefix)");
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
                return xr_tref_function(parser->compiler_session, NULL, 0, ret);
            }
            return xr_tref_unit(parser->compiler_session);
        }
        int cap = 8;
        XrTypeRef **elems = (XrTypeRef **) xr_malloc((size_t) cap * sizeof(XrTypeRef *));
        XrParamMode *param_modes = (XrParamMode *) xr_malloc((size_t) cap * sizeof(XrParamMode));
        if (!elems || !param_modes) {
            if (elems)
                xr_free(elems);
            if (param_modes)
                xr_free(param_modes);
            xr_parser_error(parser, "out of memory while parsing type list");
            return xr_tref_error(parser->compiler_session);
        }
        int count = 0;
        bool saw_param_mode = false;
        while (!xr_parser_check(parser, TK_RPAREN) && !xr_parser_check(parser, TK_EOF)) {
            if (count > 0) {
                if (!xr_parser_match(parser, TK_COMMA)) {
                    xr_parser_error(parser,
                                    "expected ',' between tuple or function parameter types");
                    break;
                }
                if (xr_parser_check(parser, TK_RPAREN))
                    break;
            }
            if (count == cap) {
                int new_cap = cap * 2;
                XrTypeRef **resized =
                    (XrTypeRef **) xr_realloc(elems, (size_t) new_cap * sizeof(XrTypeRef *));
                XrParamMode *resized_modes =
                    (XrParamMode *) xr_realloc(param_modes, (size_t) new_cap * sizeof(XrParamMode));
                if (!resized || !resized_modes) {
                    if (resized)
                        elems = resized;
                    if (resized_modes)
                        param_modes = resized_modes;
                    xr_free(elems);
                    xr_free(param_modes);
                    xr_parser_error(parser, "out of memory while growing type list");
                    return xr_tref_error(parser->compiler_session);
                }
                elems = resized;
                param_modes = resized_modes;
                cap = new_cap;
            }
            param_modes[count] = XR_PARAM_VALUE;
            if (xr_parse_optional_param_mode(parser, true, &param_modes[count]))
                saw_param_mode = true;
            elems[count++] = xr_parse_type_annotation(parser);
        }
        xr_parser_consume(parser, TK_RPAREN, "expected ')'");
        if (xr_parser_match(parser, TK_ARROW)) {
            XrTypeRef *ret = xr_parse_type_annotation(parser);
            XrTypeRef *result = xr_tref_function_with_modes(parser->compiler_session, elems,
                                                            param_modes, count, ret);
            xr_free(elems);
            xr_free(param_modes);
            return result;
        }
        if (saw_param_mode)
            xr_parser_error(parser, "parameter modes are only valid in function types");
        // `()` with no trailing `->` is the unit type, not an empty tuple.
        if (count == 0) {
            xr_free(elems);
            xr_free(param_modes);
            return xr_tref_unit(parser->compiler_session);
        }
        XrTypeRef *result = xr_tref_tuple(parser->compiler_session, elems, count);
        xr_free(elems);
        xr_free(param_modes);
        return result;
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
            reject_removed_source_type_name(parser, temp_name);
            return xr_tref_named(parser->compiler_session, "Json");
        }
        if (reject_removed_source_type_name(parser, temp_name))
            return xr_tref_error(parser->compiler_session);
        if (strcmp(temp_name, "String") == 0 || strcmp(temp_name, "str") == 0) {
            xr_parser_error(parser, "type 'string' must be lowercase in Xray");
            return xr_tref_string(parser->compiler_session);
        }
        if (strcmp(temp_name, "Int") == 0 || strcmp(temp_name, "Integer") == 0 ||
            strcmp(temp_name, "integer") == 0) {
            xr_parser_error(parser, "use 'int' (lowercase) for integer type in Xray");
            return xr_tref_int(parser->compiler_session);
        }
        if (strcmp(temp_name, "Float") == 0 || strcmp(temp_name, "Double") == 0 ||
            strcmp(temp_name, "double") == 0) {
            xr_parser_error(parser, "use 'float' (lowercase) for floating-point type in Xray");
            return xr_tref_float(parser->compiler_session);
        }
        if (strcmp(temp_name, "Bool") == 0 || strcmp(temp_name, "Boolean") == 0 ||
            strcmp(temp_name, "boolean") == 0) {
            xr_parser_error(parser, "use 'bool' (lowercase) for boolean type in Xray");
            return xr_tref_bool(parser->compiler_session);
        }
        if (strcmp(temp_name, "Char") == 0) {
            xr_parser_error(parser, "use 'char' (lowercase) for character type in Xray");
            return xr_tref_char(parser->compiler_session);
        }
        if (strcmp(temp_name, "void") == 0) {
            xr_parser_emit_removed_syntax(
                parser, &name_token, XR_ERR_SYN_VOID_REMOVED, "`void` keyword was removed",
                "use Unit type `()` instead - xray uses 0-arity tuple as Unit");
            return xr_tref_unit(parser->compiler_session);
        }

        /* Platform-width integers (FFI: C size_t / ptrdiff_t). Contextual type
         * names (not lexer keywords) avoid reserving common identifiers. */
        if (strcmp(temp_name, "uintsize") == 0)
            return xr_tref_int_width(parser->compiler_session, XR_TREF_NW_USIZE);
        if (strcmp(temp_name, "intsize") == 0)
            return xr_tref_int_width(parser->compiler_session, XR_TREF_NW_ISIZE);

        /* Generic type arguments: Name<T1, T2, ...> */
        if (xr_parser_match(parser, TK_LT)) {
            XrTypeRef *type_args[16];
            int type_arg_count = 0;
            do {
                if (type_arg_count < 16)
                    type_args[type_arg_count++] = xr_parse_type_annotation(parser);
            } while (xr_parser_match(parser, TK_COMMA));
            consume_gt_in_generic(parser);
            return expand_generic_or_clone(parser, temp_name, type_args, type_arg_count);
        }

        /* Check parser's type scope for aliases and generic params */
        if (parser->type_scope) {
            XrTypeAlias *alias = xr_type_scope_lookup(parser->type_scope, temp_name);
            if (alias)
                return expand_type_alias(parser, alias, NULL, 0);
        }

        /* Plain named type (class, enum, prelude — resolved in analyzer) */
        return xr_tref_named(parser->compiler_session, temp_name);
    }

    /* Error recovery */
    xr_parser_error_expected_name(parser, "expected type name");
    return xr_tref_error(parser->compiler_session);
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
