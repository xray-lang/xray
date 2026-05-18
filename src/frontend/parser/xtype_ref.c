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
static XrTypeRef *tref_alloc(struct XrayIsolate *X) {
    XR_DCHECK(X != NULL, "tref_alloc: NULL isolate");
    XrTypeRef *t = (XrTypeRef *) ast_alloc(X, sizeof(XrTypeRef));
    memset(t, 0, sizeof(XrTypeRef));
    return t;
}

/* Clone a NUL-terminated string into the parse arena. */
static const char *tref_strdup(struct XrayIsolate *X, const char *s) {
    if (!s)
        return NULL;
    return ast_strdup(X, s);
}

/* Allocate a children array in the arena and copy |src| into it. */
static XrTypeRef **tref_copy_children(struct XrayIsolate *X, XrTypeRef **src, int count) {
    if (count <= 0 || !src)
        return NULL;
    XrTypeRef **arr = (XrTypeRef **) ast_alloc_array(X, sizeof(XrTypeRef *), (size_t) count);
    for (int i = 0; i < count; i++)
        arr[i] = src[i];
    return arr;
}

/* ========== Primitive Constructors ========== */

XR_FUNC XrTypeRef *xr_tref_int(struct XrayIsolate *X) {
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_INT;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_float(struct XrayIsolate *X) {
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_FLOAT;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_string(struct XrayIsolate *X) {
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_STRING;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_bool(struct XrayIsolate *X) {
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_BOOL;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_unit(struct XrayIsolate *X) {
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_UNIT;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_null(struct XrayIsolate *X) {
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_NULL;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_unknown(struct XrayIsolate *X) {
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_UNKNOWN;
    return t;
}

/* ========== Native-Width Scalars ========== */

XR_FUNC XrTypeRef *xr_tref_int_width(struct XrayIsolate *X, uint8_t nw) {
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_INT_WIDTH;
    t->native_width = nw;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_float_width(struct XrayIsolate *X, uint8_t nw) {
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_FLOAT_WIDTH;
    t->native_width = nw;
    return t;
}

/* ========== Composite Constructors ========== */

XR_FUNC XrTypeRef *xr_tref_named(struct XrayIsolate *X, const char *name) {
    XR_DCHECK(name != NULL, "xr_tref_named: NULL name");
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_NAMED;
    t->name = tref_strdup(X, name);
    return t;
}

XR_FUNC XrTypeRef *xr_tref_generic(struct XrayIsolate *X, const char *name, XrTypeRef **args,
                                   int nargs) {
    XR_DCHECK(name != NULL, "xr_tref_generic: NULL name");
    XR_DCHECK(nargs > 0, "xr_tref_generic: zero args — use xr_tref_named");
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_GENERIC;
    t->name = tref_strdup(X, name);
    t->nchildren = (uint8_t) nargs;
    t->children = tref_copy_children(X, args, nargs);
    return t;
}

XR_FUNC XrTypeRef *xr_tref_optional(struct XrayIsolate *X, XrTypeRef *inner) {
    XR_DCHECK(inner != NULL, "xr_tref_optional: NULL inner");
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_OPTIONAL;
    t->nchildren = 1;
    t->children = (XrTypeRef **) ast_alloc_array(X, sizeof(XrTypeRef *), 1);
    t->children[0] = inner;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_union(struct XrayIsolate *X, XrTypeRef **members, int count) {
    XR_DCHECK(count >= 2, "xr_tref_union: need at least 2 members");
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_UNION;
    t->nchildren = (uint8_t) count;
    t->children = tref_copy_children(X, members, count);
    return t;
}

XR_FUNC XrTypeRef *xr_tref_function(struct XrayIsolate *X, XrTypeRef **params, int nparam,
                                    XrTypeRef *ret) {
    XR_DCHECK(ret != NULL, "xr_tref_function: NULL return type");
    int total = nparam + 1; /* params + return type at the end */
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_FUNCTION;
    t->nchildren = (uint8_t) total;
    t->children = (XrTypeRef **) ast_alloc_array(X, sizeof(XrTypeRef *), (size_t) total);
    for (int i = 0; i < nparam; i++)
        t->children[i] = params[i];
    t->children[nparam] = ret;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_tuple(struct XrayIsolate *X, XrTypeRef **elems, int count) {
    XR_DCHECK(count > 0, "xr_tref_tuple: empty tuple");
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_TUPLE;
    t->nchildren = (uint8_t) count;
    t->children = tref_copy_children(X, elems, count);
    return t;
}

XR_FUNC XrTypeRef *xr_tref_object(struct XrayIsolate *X, const char **field_names_src,
                                  XrTypeRef **field_types, const bool *field_readonly, int count,
                                  bool extensible) {
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_OBJECT;
    t->nchildren = (uint8_t) count;
    t->extensible = extensible;
    if (count > 0) {
        t->children = tref_copy_children(X, field_types, count);
        t->field_names = (const char **) ast_alloc_array(X, sizeof(const char *), (size_t) count);
        for (int i = 0; i < count; i++)
            t->field_names[i] = tref_strdup(X, field_names_src[i]);
        if (field_readonly) {
            t->field_readonly = (bool *) ast_alloc_array(X, sizeof(bool), (size_t) count);
            for (int i = 0; i < count; i++)
                t->field_readonly[i] = field_readonly[i];
        }
    }
    return t;
}

XR_FUNC XrTypeRef *xr_tref_fixed_array(struct XrayIsolate *X, XrTypeRef *elem, int length) {
    XR_DCHECK(elem != NULL, "xr_tref_fixed_array: NULL element type");
    XR_DCHECK(length > 0, "xr_tref_fixed_array: non-positive length");
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_FIXED_ARRAY;
    t->fixed_length = (int16_t) length;
    t->nchildren = 1;
    t->children = (XrTypeRef **) ast_alloc_array(X, sizeof(XrTypeRef *), 1);
    t->children[0] = elem;
    return t;
}

XR_FUNC XrTypeRef *xr_tref_type_param(struct XrayIsolate *X, const char *name) {
    XR_DCHECK(name != NULL, "xr_tref_type_param: NULL name");
    XrTypeRef *t = tref_alloc(X);
    t->kind = XR_TREF_TYPE_PARAM;
    t->name = ast_strdup(X, name);
    return t;
}

/* ========== String Conversion ========================================
 *
 * Produces human-readable type strings like "int", "Array<string>",
 * "(int) -> bool", etc.  Arena-allocated — no free needed.
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
        case XR_TREF_INT:
            tref_append(buf, pos, cap, "int");
            break;
        case XR_TREF_FLOAT:
            tref_append(buf, pos, cap, "float");
            break;
        case XR_TREF_STRING:
            tref_append(buf, pos, cap, "string");
            break;
        case XR_TREF_BOOL:
            tref_append(buf, pos, cap, "bool");
            break;
        case XR_TREF_UNIT:
            tref_append(buf, pos, cap, "()");
            break;
        case XR_TREF_NULL:
            tref_append(buf, pos, cap, "null");
            break;
        case XR_TREF_UNKNOWN:
            tref_append(buf, pos, cap, "unknown");
            break;

        case XR_TREF_INT_WIDTH: {
            static const char *names[] = {
                [XR_TREF_NW_I64] = "int64",  [XR_TREF_NW_I8] = "int8",
                [XR_TREF_NW_I16] = "int16",  [XR_TREF_NW_I32] = "int32",
                [XR_TREF_NW_U8] = "uint8",   [XR_TREF_NW_U16] = "uint16",
                [XR_TREF_NW_U32] = "uint32", [XR_TREF_NW_U64] = "uint64",
            };
            uint8_t nw = t->native_width;
            if (nw <= XR_TREF_NW_U64)
                tref_append(buf, pos, cap, names[nw]);
            else
                tref_append(buf, pos, cap, "int??");
            break;
        }
        case XR_TREF_FLOAT_WIDTH:
            tref_append(buf, pos, cap, t->native_width == XR_TREF_NW_F32 ? "float32" : "float64");
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
            tref_append(buf, pos, cap, "(");
            int nparam = t->nchildren > 0 ? t->nchildren - 1 : 0;
            for (int i = 0; i < nparam; i++) {
                if (i > 0)
                    tref_append(buf, pos, cap, ", ");
                tref_to_str_impl(t->children[i], buf, pos, cap);
            }
            tref_append(buf, pos, cap, ") -> ");
            if (t->nchildren > 0)
                tref_to_str_impl(t->children[t->nchildren - 1], buf, pos, cap);
            else
                tref_append(buf, pos, cap, "()");
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
            tref_append(buf, pos, cap, "{ ");
            for (int i = 0; i < t->nchildren; i++) {
                if (i > 0)
                    tref_append(buf, pos, cap, ", ");
                if (t->field_names && t->field_names[i])
                    tref_append(buf, pos, cap, t->field_names[i]);
                tref_append(buf, pos, cap, ": ");
                tref_to_str_impl(t->children[i], buf, pos, cap);
            }
            if (t->extensible)
                tref_append(buf, pos, cap, ", ...");
            tref_append(buf, pos, cap, " }");
            break;

        case XR_TREF_FIXED_ARRAY: {
            char lenbuf[16];
            snprintf(lenbuf, sizeof(lenbuf), "[%d]", (int) t->fixed_length);
            tref_append(buf, pos, cap, lenbuf);
            if (t->nchildren > 0)
                tref_to_str_impl(t->children[0], buf, pos, cap);
            break;
        }

        case XR_TREF_TYPE_PARAM:
            tref_append(buf, pos, cap, t->name ? t->name : "?");
            break;
    }
}

XR_FUNC const char *xr_tref_to_string(struct XrayIsolate *X, const XrTypeRef *t) {
    XR_DCHECK(X != NULL, "xr_tref_to_string: NULL isolate");
    if (!t)
        return "?";
    char buf[TREF_STR_BUF];
    int pos = 0;
    tref_to_str_impl(t, buf, &pos, TREF_STR_BUF);
    buf[pos] = '\0';
    return ast_strdup(X, buf);
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
