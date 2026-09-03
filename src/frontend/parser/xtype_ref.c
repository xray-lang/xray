/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_ref.c - XrTypeRef arena constructors and string conversion
 *
 * All allocations go through the parse arena so XrTypeRef values share
 * the AST lifetime — no manual free needed.
 */

#include "xtype_ref.h"
#include "xparse_internal.h"
#include "../../base/xchecks.h"
#include <string.h>
#include <stdio.h>

/* ========== Internal Helpers ========== */

/* Allocate a zeroed XrTypeRef from the parse arena. */
static XrTypeRef *tref_alloc(struct XrCompilerSession *session) {
    XR_DCHECK(session != NULL, "tref_alloc: NULL isolate");
    XrTypeRef *t = (XrTypeRef *) ast_alloc(session, sizeof(XrTypeRef));
    memset(t, 0, sizeof(XrTypeRef));
    t->scalar_rep = XR_SCALAR_REP_NONE;
    return t;
}

/* Clone a NUL-terminated string into the parse arena. */
static const char *tref_strdup(struct XrCompilerSession *session, const char *s) {
    if (!s)
        return NULL;
    return ast_strdup(session, s);
}

/* Allocate a children array in the arena and copy |src| into it. */
static XrTypeRef **tref_copy_children(struct XrCompilerSession *session, XrTypeRef **src,
                                      int count) {
    if (count <= 0 || !src)
        return NULL;
    XrTypeRef **arr = (XrTypeRef **) ast_alloc_array(session, sizeof(XrTypeRef *), (size_t) count);
    for (int i = 0; i < count; i++)
        arr[i] = src[i];
    return arr;
}

XR_FUNC XrTypeRef **xr_tref_array_copy(struct XrCompilerSession *session, XrTypeRef **refs,
                                       int count) {
    return tref_copy_children(session, refs, count);
}

/* ========== Primitive Constructors ========== */

XR_FUNC XrTypeRef *xr_tref_i64(struct XrCompilerSession *session) {
    return xr_tref_scalar(session, XR_NATIVE_I64);
}

XR_FUNC XrTypeRef *xr_tref_f64(struct XrCompilerSession *session) {
    return xr_tref_scalar(session, XR_NATIVE_F64);
}

XR_FUNC XrTypeRef *xr_tref_string(struct XrCompilerSession *session) {
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_STRING;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_bool(struct XrCompilerSession *session) {
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_BOOL;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_char(struct XrCompilerSession *session) {
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_RUNE;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_unit(struct XrCompilerSession *session) {
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_UNIT;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_null(struct XrCompilerSession *session) {
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_NULL;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_error(struct XrCompilerSession *session) {
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_ERROR;
    return t;
}

/* ========== Exact Scalars ========== */

XR_FUNC XrTypeRef *xr_tref_scalar(struct XrCompilerSession *session, uint8_t scalar_rep) {
    XR_DCHECK(xr_exact_scalar_by_native_type(scalar_rep) != NULL,
              "xr_tref_scalar: unknown scalar representation");
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_SCALAR;
    t->scalar_rep = scalar_rep;
    return t;
}

/* ========== Composite Constructors ========== */

XR_FUNC XrTypeRef *xr_tref_named(struct XrCompilerSession *session, const char *name) {
    XR_DCHECK(name != NULL, "xr_tref_named: NULL name");
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_NAMED;
    t->name = tref_strdup(session, name);
    return t;
}

XR_FUNC XrTypeRef *xr_tref_generic(struct XrCompilerSession *session, const char *name,
                                   XrTypeRef **args, int nargs) {
    XR_DCHECK(name != NULL, "xr_tref_generic: NULL name");
    XR_DCHECK(nargs > 0, "xr_tref_generic: zero args — use xr_tref_named");
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_GENERIC;
    t->name = tref_strdup(session, name);
    t->nchildren = (uint8_t) nargs;
    t->children = tref_copy_children(session, args, nargs);
    return t;
}

XR_FUNC XrTypeRef *xr_tref_const(struct XrCompilerSession *session, XrTypeRef *inner) {
    XR_DCHECK(inner != NULL, "xr_tref_const: NULL inner");
    if (inner->kind == XR_TREF_CONST)
        return inner;
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_CONST;
    t->nchildren = 1;
    t->children = (XrTypeRef **) ast_alloc_array(session, sizeof(XrTypeRef *), 1);
    t->children[0] = inner;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_optional(struct XrCompilerSession *session, XrTypeRef *inner) {
    XR_DCHECK(inner != NULL, "xr_tref_optional: NULL inner");
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_OPTIONAL;
    t->nchildren = 1;
    t->children = (XrTypeRef **) ast_alloc_array(session, sizeof(XrTypeRef *), 1);
    t->children[0] = inner;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_union(struct XrCompilerSession *session, XrTypeRef **members,
                                 int count) {
    XR_DCHECK(count >= 2, "xr_tref_union: need at least 2 members");
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_UNION;
    t->nchildren = (uint8_t) count;
    t->children = tref_copy_children(session, members, count);
    return t;
}

XR_FUNC void xr_tref_set_source_position(XrTypeRef *tref, int line, int column) {
    if (!tref)
        return;
    tref->line = line;
    tref->column = column;
}

XR_FUNC XrTypeRef *xr_tref_function_signature(struct XrCompilerSession *session, XrTypeRef **params,
                                              const XrParamMode *param_modes,
                                              const char **param_names, int nparam, XrTypeRef *ret,
                                              XrBorrowOriginSyntaxState borrow_origin_syntax,
                                              const AstBorrowOriginRef *borrow_origins,
                                              int borrow_origin_count) {
    XR_DCHECK(ret != NULL, "xr_tref_function: NULL return type");
    XR_DCHECK(nparam >= 0, "xr_tref_function: negative parameter count");
    XR_DCHECK(borrow_origin_count >= 0, "xr_tref_function: negative origin count");
    int total = nparam + 1; /* params + return type at the end */
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_FUNCTION;
    t->nchildren = (uint8_t) total;
    t->children = (XrTypeRef **) ast_alloc_array(session, sizeof(XrTypeRef *), (size_t) total);
    if (nparam > 0) {
        t->function_param_modes =
            (XrParamMode *) ast_alloc_array(session, sizeof(XrParamMode), (size_t) nparam);
        t->function_param_names =
            (const char **) ast_alloc_array(session, sizeof(const char *), (size_t) nparam);
    }
    for (int i = 0; i < nparam; i++) {
        t->children[i] = params[i];
        XrParamMode mode = param_modes ? param_modes[i] : XR_PARAM_READ;
        t->function_param_modes[i] = xr_param_mode_is_valid(mode) ? mode : XR_PARAM_READ;
        t->function_param_names[i] =
            param_names && param_names[i] ? tref_strdup(session, param_names[i]) : NULL;
    }
    t->children[nparam] = ret;
    t->borrow_origin_syntax = borrow_origin_syntax;
    t->borrow_origin_count = borrow_origin_count;
    if (borrow_origin_count > 0) {
        t->borrow_origins = (AstBorrowOriginRef *) ast_alloc_array(
            session, sizeof(AstBorrowOriginRef), (size_t) borrow_origin_count);
        for (int i = 0; i < borrow_origin_count; i++) {
            t->borrow_origins[i] = borrow_origins[i];
            if (borrow_origins[i].name)
                t->borrow_origins[i].name = tref_strdup(session, borrow_origins[i].name);
        }
    }
    return t;
}

XR_FUNC XrTypeRef *xr_tref_function_with_modes(struct XrCompilerSession *session,
                                               XrTypeRef **params, const XrParamMode *param_modes,
                                               int nparam, XrTypeRef *ret) {
    return xr_tref_function_signature(session, params, param_modes, NULL, nparam, ret,
                                      XR_BORROW_ORIGIN_OMITTED, NULL, 0);
}

XR_FUNC XrTypeRef *xr_tref_function(struct XrCompilerSession *session, XrTypeRef **params,
                                    int nparam, XrTypeRef *ret) {
    return xr_tref_function_with_modes(session, params, NULL, nparam, ret);
}

XR_FUNC XrTypeRef *xr_tref_tuple(struct XrCompilerSession *session, XrTypeRef **elems, int count) {
    XR_DCHECK(count > 0, "xr_tref_tuple: empty tuple");
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_TUPLE;
    t->nchildren = (uint8_t) count;
    t->children = tref_copy_children(session, elems, count);
    return t;
}

XR_FUNC XrTypeRef *xr_tref_object(struct XrCompilerSession *session, const char **field_names_src,
                                  XrTypeRef **field_types, const bool *field_readonly, int count) {
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_OBJECT;
    t->nchildren = (uint8_t) count;
    if (count > 0) {
        t->children = tref_copy_children(session, field_types, count);
        t->field_names =
            (const char **) ast_alloc_array(session, sizeof(const char *), (size_t) count);
        for (int i = 0; i < count; i++)
            t->field_names[i] = tref_strdup(session, field_names_src[i]);
        if (field_readonly) {
            t->field_readonly = (bool *) ast_alloc_array(session, sizeof(bool), (size_t) count);
            for (int i = 0; i < count; i++)
                t->field_readonly[i] = field_readonly[i];
        }
    }
    return t;
}

XR_FUNC XrTypeRef *xr_tref_fixed_array(struct XrCompilerSession *session, XrTypeRef *elem,
                                       int length) {
    XR_DCHECK(elem != NULL, "xr_tref_fixed_array: NULL element type");
    XR_DCHECK(length > 0, "xr_tref_fixed_array: non-positive length");
    return xr_tref_fixed_array_expr(session, elem, NULL, length);
}

XR_FUNC XrTypeRef *xr_tref_fixed_array_expr(struct XrCompilerSession *session, XrTypeRef *elem,
                                            struct AstNode *length_expr, int literal_length) {
    XR_DCHECK(elem != NULL, "xr_tref_fixed_array_expr: NULL element type");
    XR_DCHECK(literal_length >= 0, "xr_tref_fixed_array_expr: negative literal length");
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_FIXED_ARRAY;
    t->fixed_length = literal_length;
    t->fixed_length_expr = length_expr;
    t->nchildren = 1;
    t->children = (XrTypeRef **) ast_alloc_array(session, sizeof(XrTypeRef *), 1);
    t->children[0] = elem;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_type_param(struct XrCompilerSession *session, const char *name) {
    XR_DCHECK(name != NULL, "xr_tref_type_param: NULL name");
    XrTypeRef *t = tref_alloc(session);
    t->kind = XR_TREF_TYPE_PARAM;
    t->name = ast_strdup(session, name);
    return t;
}

/* ========== String Conversion ========================================
 *
 * Produces human-readable type strings like "i64", "Array<string>",
 * "(i64) -> bool", etc.  Arena-allocated — no free needed.
 * Function types follow the unified arrow form (no leading `fn`).
 * ===================================================================== */

/* Max buffer for xr_tref_to_string scratch — handles deeply nested
 * generic types without heap allocation.  If a type string exceeds
 * this, it is silently truncated. */
#define TREF_STR_BUF 512

static void tref_append(char *buf, int *pos, int cap, const char *s) {
    if (!s)
        return;
    while (*s && *pos < cap - 1)
        buf[(*pos)++] = *s++;
}

static void tref_to_str_impl(const XrTypeRef *t, char *buf, int *pos, int cap) {
    if (!t) {
        tref_append(buf, pos, cap, "?");
        return;
    }
    switch ((XrTypeRefKind) t->kind) {
        case XR_TREF_CONST:
            tref_append(buf, pos, cap, "const ");
            tref_to_str_impl(t->nchildren > 0 ? t->children[0] : NULL, buf, pos, cap);
            break;
        case XR_TREF_SCALAR: {
            const char *source_name = xr_scalar_rep_name(t->scalar_rep);
            tref_append(buf, pos, cap, source_name ? source_name : "<scalar?>");
            break;
        }
        case XR_TREF_STRING:
            tref_append(buf, pos, cap, "string");
            break;
        case XR_TREF_BOOL:
            tref_append(buf, pos, cap, "bool");
            break;
        case XR_TREF_RUNE:
            tref_append(buf, pos, cap, "rune");
            break;
        case XR_TREF_UNIT:
            tref_append(buf, pos, cap, "()");
            break;
        case XR_TREF_NULL:
            tref_append(buf, pos, cap, "null");
            break;
        case XR_TREF_ERROR:
            tref_append(buf, pos, cap, "<error>");
            break;

        case XR_TREF_NAMED:
            tref_append(buf, pos, cap, t->name ? t->name : "?");
            break;

        case XR_TREF_GENERIC:
            tref_append(buf, pos, cap, t->name ? t->name : "?");
            tref_append(buf, pos, cap, "<");
            for (int i = 0; i < t->nchildren; i++) {
                if (i > 0)
                    tref_append(buf, pos, cap, ", ");
                tref_to_str_impl(t->children[i], buf, pos, cap);
            }
            tref_append(buf, pos, cap, ">");
            break;

        case XR_TREF_OPTIONAL:
            tref_to_str_impl(t->children[0], buf, pos, cap);
            tref_append(buf, pos, cap, "?");
            break;

        case XR_TREF_UNION:
            for (int i = 0; i < t->nchildren; i++) {
                if (i > 0)
                    tref_append(buf, pos, cap, " | ");
                tref_to_str_impl(t->children[i], buf, pos, cap);
            }
            break;

        case XR_TREF_FUNCTION: {
            tref_append(buf, pos, cap, "fn(");
            int nparam = t->nchildren > 0 ? t->nchildren - 1 : 0;
            for (int i = 0; i < nparam; i++) {
                if (i > 0)
                    tref_append(buf, pos, cap, ", ");
                XrParamMode mode =
                    t->function_param_modes ? t->function_param_modes[i] : XR_PARAM_READ;
                if (t->function_param_names && t->function_param_names[i]) {
                    tref_append(buf, pos, cap, t->function_param_names[i]);
                    tref_append(buf, pos, cap, ": ");
                }
                if (mode != XR_PARAM_READ) {
                    tref_append(buf, pos, cap, xr_param_mode_label(mode));
                    tref_append(buf, pos, cap, " ");
                }
                tref_to_str_impl(t->children[i], buf, pos, cap);
            }
            tref_append(buf, pos, cap, ")");
            /* A unit return is spelled by omission: `fn(A)`, not `fn(A) -> ()`. */
            XrTypeRef *fn_ret = t->nchildren > 0 ? t->children[t->nchildren - 1] : NULL;
            if (fn_ret && fn_ret->kind != XR_TREF_UNIT) {
                tref_append(buf, pos, cap, " -> ");
                tref_to_str_impl(fn_ret, buf, pos, cap);
                if (t->borrow_origin_syntax == XR_BORROW_ORIGIN_EXPLICIT_SET &&
                    t->borrow_origin_count > 0) {
                    tref_append(buf, pos, cap, " from ");
                    for (int i = 0; i < t->borrow_origin_count; i++) {
                        if (i > 0)
                            tref_append(buf, pos, cap, " | ");
                        const AstBorrowOriginRef *origin = &t->borrow_origins[i];
                        if (origin->kind == AST_BORROW_ORIGIN_RECEIVER)
                            tref_append(buf, pos, cap, "this");
                        else if (origin->kind == AST_BORROW_ORIGIN_STATIC)
                            tref_append(buf, pos, cap, "static");
                        else
                            tref_append(buf, pos, cap, origin->name ? origin->name : "<error>");
                    }
                }
            }
            break;
        }

        case XR_TREF_TUPLE:
            tref_append(buf, pos, cap, "(");
            for (int i = 0; i < t->nchildren; i++) {
                if (i > 0)
                    tref_append(buf, pos, cap, ", ");
                tref_to_str_impl(t->children[i], buf, pos, cap);
            }
            tref_append(buf, pos, cap, ")");
            break;

        case XR_TREF_OBJECT:
            /* A structural-object type introduced by `type Name = { ... }` carries the alias
             * name (stamped on by the type-alias parser), and every use of that
             * alias shares this very ref. Render the name: it is what the source
             * wrote, so the formatter round-trips `o: PageOpts` instead of
             * expanding it to `o: { limit: int?, cursor: string? }` — an
             * expansion the re-parsed AST no longer matches. Anonymous object
             * types have no name and still render structurally. */
            if (t->name) {
                tref_append(buf, pos, cap, t->name);
                break;
            }
            tref_append(buf, pos, cap, "{ ");
            for (int i = 0; i < t->nchildren; i++) {
                if (i > 0)
                    tref_append(buf, pos, cap, ", ");
                if (t->field_names && t->field_names[i])
                    tref_append(buf, pos, cap, t->field_names[i]);
                tref_append(buf, pos, cap, ": ");
                tref_to_str_impl(t->children[i], buf, pos, cap);
            }
            tref_append(buf, pos, cap, " }");
            break;

        case XR_TREF_FIXED_ARRAY: {
            char lenbuf[16];
            tref_append(buf, pos, cap, "[");
            if (t->nchildren > 0)
                tref_to_str_impl(t->children[0], buf, pos, cap);
            else
                tref_append(buf, pos, cap, "unknown");
            if (t->fixed_length > 0)
                snprintf(lenbuf, sizeof(lenbuf), "; %d]", (int) t->fixed_length);
            else
                snprintf(lenbuf, sizeof(lenbuf), "; ?]");
            tref_append(buf, pos, cap, lenbuf);
            break;
        }

        case XR_TREF_TYPE_PARAM:
            tref_append(buf, pos, cap, t->name ? t->name : "?");
            break;
    }
}

XR_FUNC const char *xr_tref_to_string(struct XrCompilerSession *session, const XrTypeRef *t) {
    XR_DCHECK(session != NULL, "xr_tref_to_string: NULL isolate");
    if (!t)
        return "?";
    char buf[TREF_STR_BUF];
    int pos = 0;
    tref_to_str_impl(t, buf, &pos, TREF_STR_BUF);
    buf[pos] = '\0';
    return ast_strdup(session, buf);
}

XR_FUNC int xr_tref_to_string_buf(const XrTypeRef *t, char *buf, int cap) {
    XR_DCHECK(buf != NULL, "xr_tref_to_string_buf: NULL buffer");
    XR_DCHECK(cap > 0, "xr_tref_to_string_buf: zero capacity");
    if (!t) {
        buf[0] = '?';
        buf[1] = '\0';
        return 1;
    }
    int pos = 0;
    tref_to_str_impl(t, buf, &pos, cap);
    buf[pos] = '\0';
    return pos;
}

XR_FUNC int xr_tref_to_string_buf_structural(const XrTypeRef *t, char *buf, int cap) {
    XR_DCHECK(buf != NULL, "xr_tref_to_string_buf_structural: NULL buffer");
    XR_DCHECK(cap > 0, "xr_tref_to_string_buf_structural: zero capacity");
    if (!t) {
        buf[0] = '?';
        buf[1] = '\0';
        return 1;
    }
    /* Only the outermost structural object expands: this is for the one place that must
     * print an object's structure rather than its name — the right-hand side of
     * `type Name = { ... }`, where the name would otherwise render as the
     * self-referential `type Name = Name`. Nested refs keep their names. */
    if (t->kind == XR_TREF_OBJECT && t->name) {
        XrTypeRef anonymous = *t;
        anonymous.name = NULL;
        int pos = 0;
        tref_to_str_impl(&anonymous, buf, &pos, cap);
        buf[pos] = '\0';
        return pos;
    }
    return xr_tref_to_string_buf(t, buf, cap);
}
