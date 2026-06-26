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
 * This pass runs after select_rep (STAGE_REPPED) and advances the
 * function to STAGE_BACKEND.  It does NOT allocate new values — it
 * only mutates v->op and v->aux/aux_int in place, which is safe
 * because XI_CALL_BUILTIN has the same value shape (nargs preserved).
 */

#include "xi_backend.h"
#include "xi_effect.h"
#include "xi_module.h"
#include "xi_vec_scalar_lower.h"
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

static const XiValue *backend_unwrap_identity_value(const XiValue *v) {
    while (v &&
           (v->op == XI_BOX || v->op == XI_UNBOX || xi_copy_is_identity_alias(v) ||
            v->op == XI_MOVE) &&
           v->nargs >= 1)
        v = v->args[0];
    return v;
}

static const XiImportRef *backend_value_import_ref(const XiValue *v) {
    v = backend_unwrap_identity_value(v);
    if (!v || v->op != XI_IMPORT_REF || !v->aux)
        return NULL;
    return (const XiImportRef *) v->aux;
}

static const XiImportRef *backend_shared_slot_import_ref_in_func(const XiFunc *f, int slot) {
    if (!f || slot < 0)
        return NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_SET_SHARED || (int) v->aux_int != slot || v->nargs < 1)
                continue;
            const XiImportRef *ref = backend_value_import_ref(v->args[0]);
            if (ref)
                return ref;
        }
    }
    return NULL;
}

static const XiImportRef *backend_shared_slot_import_ref(const XiFunc *f, int slot) {
    for (const XiFunc *cur = f; cur; cur = cur->parent_func) {
        const XiImportRef *ref = backend_shared_slot_import_ref_in_func(cur, slot);
        if (ref)
            return ref;
    }
    if (f && f->module && f->module->init) {
        const XiImportRef *ref = backend_shared_slot_import_ref_in_func(f->module->init, slot);
        if (ref)
            return ref;
    }
    return NULL;
}

static const XiImportRef *backend_import_ref_from_value(const XiFunc *f, const XiValue *v) {
    v = backend_unwrap_identity_value(v);
    if (!v)
        return NULL;
    if (v->op == XI_IMPORT_REF && v->aux)
        return (const XiImportRef *) v->aux;
    if (v->op == XI_GET_SHARED)
        return backend_shared_slot_import_ref(f, (int) v->aux_int);
    return NULL;
}

static bool backend_import_ref_is_module(const XiImportRef *ref, const char *module_name) {
    return ref && module_name && ref->module_path && strcmp(ref->module_path, module_name) == 0 &&
           !ref->member_name;
}

static bool backend_math_call_arity_ok(const char *name, int nargs) {
    if (!name || nargs < 0)
        return false;
    if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0)
        return true;
    if (strcmp(name, "pow") == 0 || strcmp(name, "atan2") == 0 || strcmp(name, "hypot") == 0 ||
        strcmp(name, "fmod") == 0)
        return nargs == 2;
    if (strcmp(name, "clamp") == 0 || strcmp(name, "lerp") == 0)
        return nargs == 3;
    static const char *unary[] = {
        "abs",  "floor", "ceil",  "round", "sqrt",     "sin",      "cos",  "tan",   "asin",
        "acos", "atan",  "log",   "log10", "log2",     "exp",      "sinh", "cosh",  "tanh",
        "cbrt", "trunc", "log1p", "expm1", "degToRad", "radToDeg", "sign", "isNaN", "isFinite",
    };
    for (int i = 0; i < (int) (sizeof(unary) / sizeof(unary[0])); i++) {
        if (strcmp(name, unary[i]) == 0)
            return nargs == 1;
    }
    return false;
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
        const XiImportRef *recv_ref = backend_import_ref_from_value(f, v->args[0]);
        const char *method = (const char *) v->aux;
        if (backend_import_ref_is_module(recv_ref, "math"))
            return rewrite_call_drop_first_arg_to_math_builtin(f, v, method);
    }

    if (v->op == XI_CALL && v->nargs >= 1) {
        const XiImportRef *callee_ref = backend_import_ref_from_value(f, v->args[0]);
        if (callee_ref && callee_ref->module_path && strcmp(callee_ref->module_path, "math") == 0 &&
            callee_ref->member_name)
            return rewrite_call_drop_first_arg_to_math_builtin(f, v, callee_ref->member_name);
    }

    return false;
}

/* Lower one value if it's not backend-legal. Returns true if changed. */
static bool lower_value(XiFunc *f, XiValue *v) {
    if (!v)
        return false;
    if (lower_math_import_call(f, v))
        return true;
    if (xi_op_is_backend_legal(v->op))
        return false;

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

/* Lower all non-backend-legal ops in a function and advance stage.
 * Recurses into children. */
XR_FUNC void xi_backend_lower(XiFunc *f) {
    if (!f)
        return;
    if (f->stage >= XI_STAGE_BACKEND)
        return;

    XR_DCHECK(f->stage >= XI_STAGE_REPPED, "xi_backend_lower: requires STAGE_REPPED");

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

    f->stage = XI_STAGE_BACKEND;
    f->invariant_mask |= xi_stage_invariants(XI_STAGE_BACKEND);

    /* Recurse into children */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_backend_lower(f->children[i]);
    }
}
