/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompile_type_format.c - One metered syntax-type formatter
 */
#include "xtype_ref.h"
#include "xast.h"

static void tref_append(XrCompileState *state, char *buf, int *pos, int cap, const char *s) {
    if (!s) return;
    while (*pos < cap - 1) {
        if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return;
        char c = *s++;
        if (!c) return;
        if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return;
        buf[(*pos)++] = c;
    }
}

static void tref_to_str_impl(XrCompileState *state, const XrTypeRef *t, char *buf, int *pos, int cap) {
    if (*pos >= cap - 1 || xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return;
    if (!t) {
        do { tref_append(state, buf, pos, cap, "?"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
        return;
    }
    switch ((XrTypeRefKind) t->kind) {
        case XR_TREF_CONST:
            do { tref_append(state, buf, pos, cap, "const "); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            do { tref_to_str_impl(state, t->nchildren > 0 ? t->children[0] : NULL, buf, pos, cap); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            break;
        case XR_TREF_SCALAR: {
            const char *source_name = xr_scalar_rep_name(t->scalar_rep);
            do { tref_append(state, buf, pos, cap, source_name ? source_name : "<scalar?>"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            break;
        }
        case XR_TREF_STRING:
            do { tref_append(state, buf, pos, cap, "string"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            break;
        case XR_TREF_BOOL:
            do { tref_append(state, buf, pos, cap, "bool"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            break;
        case XR_TREF_RUNE:
            do { tref_append(state, buf, pos, cap, "rune"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            break;
        case XR_TREF_UNIT:
            do { tref_append(state, buf, pos, cap, "()"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            break;
        case XR_TREF_NULL:
            do { tref_append(state, buf, pos, cap, "null"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            break;
        case XR_TREF_ERROR:
            do { tref_append(state, buf, pos, cap, "<error>"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            break;

        case XR_TREF_NAMED:
            do { tref_append(state, buf, pos, cap, t->name ? t->name : "?"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            break;

        case XR_TREF_GENERIC:
            do { tref_append(state, buf, pos, cap, t->name ? t->name : "?"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            do { tref_append(state, buf, pos, cap, "<"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            for (int i = 0; i < t->nchildren && *pos < cap - 1 && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK; i++) {
                if (i > 0)
                    do { tref_append(state, buf, pos, cap, ", "); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                do { tref_to_str_impl(state, t->children[i], buf, pos, cap); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            }
            do { tref_append(state, buf, pos, cap, ">"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            break;

        case XR_TREF_OPTIONAL:
            do { tref_to_str_impl(state, t->children[0], buf, pos, cap); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            do { tref_append(state, buf, pos, cap, "?"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            break;

        case XR_TREF_UNION:
            for (int i = 0; i < t->nchildren && *pos < cap - 1 && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK; i++) {
                if (i > 0)
                    do { tref_append(state, buf, pos, cap, " | "); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                do { tref_to_str_impl(state, t->children[i], buf, pos, cap); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            }
            break;

        case XR_TREF_FUNCTION: {
            do { tref_append(state, buf, pos, cap, "fn("); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            int nparam = t->nchildren > 0 ? t->nchildren - 1 : 0;
            for (int i = 0; i < nparam && *pos < cap - 1 && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK; i++) {
                if (i > 0)
                    do { tref_append(state, buf, pos, cap, ", "); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                XrParamMode mode =
                    t->function_param_modes ? t->function_param_modes[i] : XR_PARAM_READ;
                if (t->function_param_names && t->function_param_names[i]) {
                    do { tref_append(state, buf, pos, cap, t->function_param_names[i]); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                    do { tref_append(state, buf, pos, cap, ": "); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                }
                if (mode != XR_PARAM_READ) {
                    do { tref_append(state, buf, pos, cap, xr_param_mode_label(mode)); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                    do { tref_append(state, buf, pos, cap, " "); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                }
                do { tref_to_str_impl(state, t->children[i], buf, pos, cap); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            }
            do { tref_append(state, buf, pos, cap, ")"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            /* A unit return is spelled by omission: `fn(A)`, not `fn(A) -> ()`. */
            XrTypeRef *fn_ret = t->nchildren > 0 ? t->children[t->nchildren - 1] : NULL;
            if (fn_ret && fn_ret->kind != XR_TREF_UNIT) {
                do { tref_append(state, buf, pos, cap, " -> "); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                do { tref_to_str_impl(state, fn_ret, buf, pos, cap); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                if (t->borrow_origin_syntax == XR_BORROW_ORIGIN_EXPLICIT_SET &&
                    t->borrow_origin_count > 0) {
                    do { tref_append(state, buf, pos, cap, " from "); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                    for (int i = 0; i < t->borrow_origin_count && *pos < cap - 1 && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK; i++) {
                        if (i > 0)
                            do { tref_append(state, buf, pos, cap, " | "); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                        const AstBorrowOriginRef *origin = &t->borrow_origins[i];
                        if (origin->kind == AST_BORROW_ORIGIN_RECEIVER)
                            do { tref_append(state, buf, pos, cap, "this"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                        else if (origin->kind == AST_BORROW_ORIGIN_STATIC)
                            do { tref_append(state, buf, pos, cap, "static"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                        else
                            do { tref_append(state, buf, pos, cap, origin->name ? origin->name : "<error>"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                    }
                }
            }
            break;
        }

        case XR_TREF_TUPLE:
            do { tref_append(state, buf, pos, cap, "("); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            for (int i = 0; i < t->nchildren && *pos < cap - 1 && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK; i++) {
                if (i > 0)
                    do { tref_append(state, buf, pos, cap, ", "); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                do { tref_to_str_impl(state, t->children[i], buf, pos, cap); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            }
            do { tref_append(state, buf, pos, cap, ")"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
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
                do { tref_append(state, buf, pos, cap, t->name); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                break;
            }
            do { tref_append(state, buf, pos, cap, "{ "); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            for (int i = 0; i < t->nchildren && *pos < cap - 1 && xr_compile_state_status(state) == XR_COMPILE_RESOURCE_OK; i++) {
                if (i > 0)
                    do { tref_append(state, buf, pos, cap, ", "); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                if (t->field_names && t->field_names[i])
                    do { tref_append(state, buf, pos, cap, t->field_names[i]); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                do { tref_append(state, buf, pos, cap, ": "); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
                do { tref_to_str_impl(state, t->children[i], buf, pos, cap); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            }
            do { tref_append(state, buf, pos, cap, " }"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            break;

        case XR_TREF_FIXED_ARRAY: {
            char lenbuf[16];
            do { tref_append(state, buf, pos, cap, "["); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            if (t->nchildren > 0)
                do { tref_to_str_impl(state, t->children[0], buf, pos, cap); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            else
                do { tref_append(state, buf, pos, cap, "unknown"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            do { tref_append(state, buf, pos, cap, "; "); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            if (t->fixed_length > 0) {
                unsigned length = (unsigned) t->fixed_length, digits = 0;
                do {
                    if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return;
                    lenbuf[digits++] = (char) ('0' + length % 10);
                    length /= 10;
                } while (length);
                while (digits && *pos < cap - 1) {
                    if (xr_compile_state_work(state, 2) != XR_COMPILE_RESOURCE_OK) return;
                    buf[(*pos)++] = lenbuf[--digits];
                }
            } else {
                do { tref_append(state, buf, pos, cap, "?"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            }
            do { tref_append(state, buf, pos, cap, "]"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            break;
        }

        case XR_TREF_TYPE_PARAM:
            do { tref_append(state, buf, pos, cap, t->name ? t->name : "?"); if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return; } while (0);
            break;
    }
}

XR_FUNC int xr_compile_tref_to_string_buf(XrCompileState *state, const XrTypeRef *t,
                                            char *buf, int cap) {
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return -1;
    if (!buf || cap <= 0) {
        xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return -1;
    }
    int pos = 0;
    tref_to_str_impl(state, t, buf, &pos, cap);
    if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return -1;
    buf[pos] = '\0';
    return pos;
}

XR_FUNC int xr_compile_tref_to_string_structural(XrCompileState *state, const XrTypeRef *t,
                                                   char *buf, int cap) {
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK) return -1;
    if (!buf || cap <= 0) {
        xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return -1;
    }
    if (t) {
        if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return -1;
        if (t->kind == XR_TREF_OBJECT && t->name) {
            XrTypeRef anonymous;
            if (xr_compile_state_copy(state, &anonymous, t, sizeof(anonymous)) != XR_COMPILE_RESOURCE_OK) return -1;
            anonymous.name = NULL;
            return xr_compile_tref_to_string_buf(state, &anonymous, buf, cap);
        }
    }
    return xr_compile_tref_to_string_buf(state, t, buf, cap);
}
