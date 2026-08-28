/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_backend_lower.c - Lower high-level Xi IR ops to backend-legal form
 *
 * Rewrites remaining semantic sugar ops according to generated backend
 * rewrite metadata so that the function satisfies the STAGE_BACKEND contract.
 *
 * This pass runs on an exact STAGE_REPPED input. The consuming stage API
 * verifies and publishes STAGE_BACKEND. It does NOT allocate new values — it
 * only mutates v->op and v->aux/aux_int in place, which is safe
 * because XI_CALL_BUILTIN has the same value shape (nargs preserved).
 */

#include "xi_backend.h"
#include "xi_effect.h"
#include "xi_value_query.h"
#include "xi_vec_scalar_lower.h"
#include "../runtime/value/xtype.h"
#include "../frontend/analyzer/xa_intrinsic_registry.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include <string.h>

/* Rewrite a single value from high-level op to XI_CALL_BUILTIN.
 * aux is set to a static name string; aux_int carries extra info. */
static inline void rewrite_to_builtin(XiValue *v, const char *name) {
    v->op = XI_CALL_BUILTIN;
    v->aux = (void *) name;
    /* Keep existing aux_int (may carry flags/field_index) */
}

static bool backend_import_ref_is_module(const XiImportRef *ref, const char *module_name) {
    return ref && module_name && ref->module_path && strcmp(ref->module_path, module_name) == 0 &&
           !ref->member_name;
}

static bool backend_math_call_arity_ok(const char *name, int nargs) {
    if (!name || nargs < 0)
        return false;
    char key[192];
    int key_len = snprintf(key, sizeof(key), "math.%s", name);
    if (key_len <= 0 || (size_t) key_len >= sizeof(key))
        return false;
    const XaIntrinsicDesc *desc = xa_intrinsic_by_key(key);
    if (!desc || desc->family != XA_INTRINSIC_FAMILY_MATH)
        return false;
    if (desc->id == XA_INTRINSIC_MATH_MIN || desc->id == XA_INTRINSIC_MATH_MAX)
        return nargs >= 1;
    return nargs == (int) desc->max_arity;
}

static const char *backend_math_builtin_name(XiFunc *f, const char *member) {
    if (!f || !member)
        return NULL;
    size_t member_len = strlen(member);
    char *name = (char *) xi_func_arena_alloc(f, (uint32_t) (5 + member_len + 1));
    if (!name)
        return NULL;
    memcpy(name, "math.", 5);
    memcpy(name + 5, member, member_len + 1);
    return name;
}

static bool rewrite_call_drop_first_arg_to_math_builtin(XiFunc *f, XiValue *v, const char *member) {
    if (!v || v->nargs < 1 || !backend_math_call_arity_ok(member, (int) v->nargs - 1))
        return false;
    const char *builtin_name = backend_math_builtin_name(f, member);
    if (!builtin_name)
        return false;
    if (v->nargs > 1)
        memmove(v->args, v->args + 1, (size_t) (v->nargs - 1) * sizeof(v->args[0]));
    v->nargs--;
    v->op = XI_CALL_BUILTIN;
    v->aux = (void *) builtin_name;
    v->aux_int = 0;
    v->flags |= xi_op_default_effects(XI_CALL_BUILTIN);
    return true;
}

static bool lower_math_import_call(XiFunc *f, XiValue *v) {
    if (!f || !v)
        return false;

    if (v->op == XI_CALL_METHOD && v->nargs >= 1) {
        const XiImportRef *recv_ref = xi_value_import_ref(f, v->args[0]);
        const char *method = (const char *) v->aux;
        if (backend_import_ref_is_module(recv_ref, "math"))
            return rewrite_call_drop_first_arg_to_math_builtin(f, v, method);
    }

    if (v->op == XI_CALL && v->nargs >= 1) {
        const XiImportRef *callee_ref = xi_value_import_ref(f, v->args[0]);
        if (callee_ref && callee_ref->module_path && strcmp(callee_ref->module_path, "math") == 0 &&
            callee_ref->member_name)
            return rewrite_call_drop_first_arg_to_math_builtin(f, v, callee_ref->member_name);
    }

    return false;
}

static bool lower_semantic_intrinsic_for_aot(XiValue *v) {
    if (!v || v->xa_intrinsic_id == XA_INTRINSIC_NONE)
        return false;
    const XaIntrinsicDesc *desc = xa_intrinsic_by_id((XaIntrinsicId) v->xa_intrinsic_id);
    XR_CHECK(desc != NULL, "Xi value carries unknown canonical intrinsic id");
    bool changed = false;
    if ((desc->flags & XA_INTRINSIC_FLAG_STATIC_RECEIVER) != 0 &&
        desc->lowering != XA_INTRINSIC_LOWERING_SCALAR_PARSE) {
        XR_CHECK(v->nargs >= 1, "static canonical intrinsic is missing receiver identity");
        if (v->nargs > 1)
            memmove(v->args, v->args + 1, (size_t) (v->nargs - 1) * sizeof(v->args[0]));
        v->nargs--;
        changed = true;
    }
    if ((desc->flags & XA_INTRINSIC_FLAG_EXPLICIT_SHUFFLE) != 0) {
        XR_CHECK(v->nargs >= 1, "shuffle canonical intrinsic is missing vector receiver");
        v->nargs = 1;
        changed = true;
    }
    return changed;
}

/* Lower one value if it's not backend-legal. Returns true if changed. */
static bool lower_value(XiFunc *f, XiValue *v) {
    if (!v)
        return false;
    bool intrinsic_changed = lower_semantic_intrinsic_for_aot(v);
    if (lower_math_import_call(f, v))
        return true;
    if (xi_op_is_backend_legal(v->op))
        return intrinsic_changed;

    uint8_t rewrite = xi_op_backend_rewrite(v->op);
    switch (rewrite) {
        case XI_GEN_BACKEND_REWRITE_BUILTIN: {
            const char *name = xi_op_backend_rewrite_name(v->op);
            if (!name || !name[0])
                return false;
            rewrite_to_builtin(v, name);
            break;
        }

        case XI_GEN_BACKEND_REWRITE_NONE:
        case XI_GEN_BACKEND_REWRITE__COUNT:
            return false;

        default:
            return false;
    }

    /* Update effect flags to match new opcode (XI_CALL_BUILTIN has
     * side-effect + may-throw by default). */
    v->flags |= xi_op_default_effects(XI_CALL_BUILTIN);
    return true;
}

/* Lower all non-backend-legal ops in a function. Recurses into children. */
XR_FUNC void xi_backend_lower(XiFunc *f) {
    if (!f)
        return;
    XR_DCHECK(f->stage == XI_STAGE_REPPED, "xi_backend_lower: requires exact STAGE_REPPED");

    /* Scalar-expand vector ops before other backend lowering. */
    (void) xi_vec_scalar_lower(f);

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            lower_value(f, blk->values[vi]);
        }
    }

    /* Recurse into children */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_backend_lower(f->children[i]);
    }
}
