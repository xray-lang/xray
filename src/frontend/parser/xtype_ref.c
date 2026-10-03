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

static bool tref_count(struct XrCompilerSession *session, int count, unsigned maximum) {
    if (count >= 0 && (unsigned) count <= maximum) return true;
    xr_compile_state_fail(xr_compile_session_compile_state(session),
                          count < 0 ? XR_COMPILE_RESOURCE_BAD_ARGUMENT : XR_COMPILE_RESOURCE_BUDGET);
    return false;
}

/* Allocate a zeroed XrTypeRef from the parse arena. */
static XrTypeRef *tref_alloc(struct XrCompilerSession *session) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    XR_DCHECK(session != NULL, "tref_alloc: NULL isolate");
    XrTypeRef *t = (XrTypeRef *) ast_alloc(session, sizeof(XrTypeRef));
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    if (!t) return NULL;
    t->scalar_rep = XR_SCALAR_REP_NONE;
    return t;
}

/* Clone a NUL-terminated string into the parse arena. */
static const char *tref_strdup(struct XrCompilerSession *session, const char *s) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    if (!s)
        return NULL;
    return ast_strdup(session, s);
}

/* Allocate a children array in the arena and copy |src| into it. */
static XrTypeRef **tref_copy_children(struct XrCompilerSession *session, XrTypeRef **src,
                                      int count) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    if (!tref_count(session, count, INT_MAX)) return NULL;
    if (!count) return NULL;
    if (!src) {
        xr_compile_state_fail(xr_compile_session_compile_state(session), XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return NULL;
    }
    XrTypeRef **arr = (XrTypeRef **) ast_alloc_array(session, sizeof(XrTypeRef *), (size_t) count);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    if (!ast_copy(session, arr, src, (size_t) count * sizeof(*arr))) return NULL;
    return arr;
}

XR_FUNC XrTypeRef **xr_tref_array_copy(struct XrCompilerSession *session, XrTypeRef **refs,
                                       int count) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    return tref_copy_children(session, refs, count);
}

/* ========== Primitive Constructors ========== */

XR_FUNC XrTypeRef *xr_tref_i64(struct XrCompilerSession *session) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    return xr_tref_scalar(session, XR_NATIVE_I64);
}

XR_FUNC XrTypeRef *xr_tref_f64(struct XrCompilerSession *session) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    return xr_tref_scalar(session, XR_NATIVE_F64);
}

XR_FUNC XrTypeRef *xr_tref_string(struct XrCompilerSession *session) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_STRING;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_bool(struct XrCompilerSession *session) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_BOOL;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_char(struct XrCompilerSession *session) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_RUNE;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_unit(struct XrCompilerSession *session) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_UNIT;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_null(struct XrCompilerSession *session) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_NULL;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_error(struct XrCompilerSession *session) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_ERROR;
    return t;
}

/* ========== Exact Scalars ========== */

XR_FUNC XrTypeRef *xr_tref_scalar(struct XrCompilerSession *session, uint8_t scalar_rep) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    XR_DCHECK(xr_exact_scalar_by_native_type(scalar_rep) != NULL,
              "xr_tref_scalar: unknown scalar representation");
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_SCALAR;
    t->scalar_rep = scalar_rep;
    return t;
}

/* ========== Composite Constructors ========== */

XR_FUNC XrTypeRef *xr_tref_named(struct XrCompilerSession *session, const char *name) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    XR_DCHECK(name != NULL, "xr_tref_named: NULL name");
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_NAMED;
    do {
        t->name = tref_strdup(session, name);
        if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    } while (0);
    return t;
}

XR_FUNC XrTypeRef *xr_tref_generic(struct XrCompilerSession *session, const char *name,
                                   XrTypeRef **args, int nargs) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    if (!tref_count(session, nargs, UINT8_MAX)) return NULL;
    XR_DCHECK(name != NULL, "xr_tref_generic: NULL name");
    XR_DCHECK(nargs > 0, "xr_tref_generic: zero args — use xr_tref_named");
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_GENERIC;
    do {
        t->name = tref_strdup(session, name);
        if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    } while (0);
    t->nchildren = (uint8_t) nargs;
    do {
        t->children = tref_copy_children(session, args, nargs);
        if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    } while (0);
    return t;
}

XR_FUNC XrTypeRef *xr_tref_const(struct XrCompilerSession *session, XrTypeRef *inner) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    XR_DCHECK(inner != NULL, "xr_tref_const: NULL inner");
    if (inner->kind == XR_TREF_CONST)
        return inner;
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_CONST;
    t->nchildren = 1;
    do {
        t->children = (XrTypeRef **) ast_alloc_array(session, sizeof(XrTypeRef *), 1);
        if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    } while (0);
    if (!ast_work(session, sizeof(*t->children))) return NULL;
    t->children[0] = inner;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_optional(struct XrCompilerSession *session, XrTypeRef *inner) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    XR_DCHECK(inner != NULL, "xr_tref_optional: NULL inner");
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_OPTIONAL;
    t->nchildren = 1;
    do {
        t->children = (XrTypeRef **) ast_alloc_array(session, sizeof(XrTypeRef *), 1);
        if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    } while (0);
    if (!ast_work(session, sizeof(*t->children))) return NULL;
    t->children[0] = inner;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_union(struct XrCompilerSession *session, XrTypeRef **members,
                                 int count) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    if (!tref_count(session, count, UINT8_MAX)) return NULL;
    XR_DCHECK(count >= 2, "xr_tref_union: need at least 2 members");
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_UNION;
    t->nchildren = (uint8_t) count;
    do {
        t->children = tref_copy_children(session, members, count);
        if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    } while (0);
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
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    if (!tref_count(session, nparam, UINT8_MAX - 1)) return NULL;
    XR_DCHECK(ret != NULL, "xr_tref_function: NULL return type");
    XR_DCHECK(nparam >= 0, "xr_tref_function: negative parameter count");
    XR_DCHECK(borrow_origin_count >= 0, "xr_tref_function: negative origin count");
    if (!tref_count(session, borrow_origin_count, INT_MAX)) return NULL;
    if ((nparam && !params) || (borrow_origin_count && !borrow_origins)) {
        xr_compile_state_fail(xr_compile_session_compile_state(session), XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return NULL;
    }
    int total = nparam + 1; /* params + return type at the end */
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_FUNCTION;
    t->nchildren = (uint8_t) total;
    do {
        t->children = (XrTypeRef **) ast_alloc_array(session, sizeof(XrTypeRef *), (size_t) total);
        if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    } while (0);
    if (nparam > 0) {
        do {
            t->function_param_modes =
            (XrParamMode *) ast_alloc_array(session, sizeof(XrParamMode), (size_t) nparam);
            if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
        } while (0);
        do {
            t->function_param_names =
            (const char **) ast_alloc_array(session, sizeof(const char *), (size_t) nparam);
            if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
        } while (0);
    }
    for (int i = 0; i < nparam; i++) {
        if (!ast_work(session, sizeof(*t->children) + sizeof(*t->function_param_modes))) return NULL;
        t->children[i] = params[i];
        XrParamMode mode = param_modes ? param_modes[i] : XR_PARAM_READ;
        t->function_param_modes[i] = xr_param_mode_is_valid(mode) ? mode : XR_PARAM_READ;
        const char *name = param_names && param_names[i] ? tref_strdup(session, param_names[i]) : NULL;
        if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
        if (!ast_work(session, sizeof(*t->function_param_names))) return NULL;
        t->function_param_names[i] = name;
    }
    if (!ast_work(session, sizeof(*t->children))) return NULL;
    t->children[nparam] = ret;
    t->borrow_origin_syntax = borrow_origin_syntax;
    t->borrow_origin_count = borrow_origin_count;
    if (borrow_origin_count > 0) {
        do {
            t->borrow_origins = (AstBorrowOriginRef *) ast_alloc_array(
            session, sizeof(AstBorrowOriginRef), (size_t) borrow_origin_count);
            if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
        } while (0);
        for (int i = 0; i < borrow_origin_count; i++) {
            if (!ast_copy(session, &t->borrow_origins[i], &borrow_origins[i], sizeof(*t->borrow_origins))) return NULL;
            if (borrow_origins[i].name)
                do {
                    t->borrow_origins[i].name = tref_strdup(session, borrow_origins[i].name);
                    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
                } while (0);
        }
    }
    return t;
}

XR_FUNC XrTypeRef *xr_tref_function_with_modes(struct XrCompilerSession *session,
                                               XrTypeRef **params, const XrParamMode *param_modes,
                                               int nparam, XrTypeRef *ret) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    return xr_tref_function_signature(session, params, param_modes, NULL, nparam, ret,
                                      XR_BORROW_ORIGIN_OMITTED, NULL, 0);
}

XR_FUNC XrTypeRef *xr_tref_function(struct XrCompilerSession *session, XrTypeRef **params,
                                    int nparam, XrTypeRef *ret) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    return xr_tref_function_with_modes(session, params, NULL, nparam, ret);
}

XR_FUNC XrTypeRef *xr_tref_tuple(struct XrCompilerSession *session, XrTypeRef **elems, int count) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    if (!tref_count(session, count, UINT8_MAX)) return NULL;
    XR_DCHECK(count > 0, "xr_tref_tuple: empty tuple");
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_TUPLE;
    t->nchildren = (uint8_t) count;
    do {
        t->children = tref_copy_children(session, elems, count);
        if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    } while (0);
    return t;
}

XR_FUNC XrTypeRef *xr_tref_object(struct XrCompilerSession *session, const char **field_names_src,
                                  XrTypeRef **field_types, const bool *field_readonly, int count) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    if (!tref_count(session, count, UINT8_MAX)) return NULL;
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_OBJECT;
    t->nchildren = (uint8_t) count;
    if (count > 0) {
        do {
            t->children = tref_copy_children(session, field_types, count);
            if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
        } while (0);
        do {
            t->field_names =
            (const char **) ast_alloc_array(session, sizeof(const char *), (size_t) count);
            if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
        } while (0);
        for (int i = 0; i < count; i++)
            do {
                if (!ast_work(session, sizeof(*t->field_names))) return NULL;
                t->field_names[i] = tref_strdup(session, field_names_src[i]);
                if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
            } while (0);
        if (field_readonly) {
            do {
                t->field_readonly = (bool *) ast_alloc_array(session, sizeof(bool), (size_t) count);
                if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
            } while (0);
            if (!ast_copy(session, t->field_readonly, field_readonly, (size_t) count * sizeof(bool))) return NULL;
        }
    }
    return t;
}

XR_FUNC XrTypeRef *xr_tref_fixed_array(struct XrCompilerSession *session, XrTypeRef *elem,
                                       int length) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    XR_DCHECK(elem != NULL, "xr_tref_fixed_array: NULL element type");
    XR_DCHECK(length > 0, "xr_tref_fixed_array: non-positive length");
    return xr_tref_fixed_array_expr(session, elem, NULL, length);
}

XR_FUNC XrTypeRef *xr_tref_fixed_array_expr(struct XrCompilerSession *session, XrTypeRef *elem,
                                            struct AstNode *length_expr, int literal_length) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    XR_DCHECK(elem != NULL, "xr_tref_fixed_array_expr: NULL element type");
    XR_DCHECK(literal_length >= 0, "xr_tref_fixed_array_expr: negative literal length");
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_FIXED_ARRAY;
    t->fixed_length = literal_length;
    t->fixed_length_expr = length_expr;
    t->nchildren = 1;
    do {
        t->children = (XrTypeRef **) ast_alloc_array(session, sizeof(XrTypeRef *), 1);
        if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    } while (0);
    if (!ast_work(session, sizeof(*t->children))) return NULL;
    t->children[0] = elem;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_type_param(struct XrCompilerSession *session, const char *name) {
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    XR_DCHECK(name != NULL, "xr_tref_type_param: NULL name");
    XrTypeRef *t = tref_alloc(session);
    if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    t->kind = XR_TREF_TYPE_PARAM;
    do {
        t->name = ast_strdup(session, name);
        if (xr_compile_session_resource_status(session) != XR_COMPILE_RESOURCE_OK) return NULL;
    } while (0);
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

XR_FUNC const char *xr_tref_to_string(struct XrCompilerSession *session, const XrTypeRef *t) {
    XrCompileState *state = xr_compile_session_compile_state(session);
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return NULL;
    char buf[TREF_STR_BUF];
    int length = xr_compile_tref_to_string_buf(state, t, buf, TREF_STR_BUF);
    return length >= 0 ? ast_strndup(session, buf, (size_t) length) : NULL;
}
