/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_backend_lower.c - Lower high-level Xi IR ops to backend-legal form
 *
 * Rewrites semantic sugar ops (XI_PRINT, XI_STR_CONCAT, XI_ITER_*,
 * XI_ARRAY_NEW, etc.) into XI_CALL_BUILTIN or XI_CALL_METHOD so that
 * the function satisfies the STAGE_BACKEND contract.
 *
 * This pass runs after select_rep (STAGE_REPPED) and advances the
 * function to STAGE_BACKEND.  It does NOT allocate new values — it
 * only mutates v->op and v->aux/aux_int in place, which is safe
 * because XI_CALL_BUILTIN has the same value shape (nargs preserved).
 */

#include "xi_backend.h"
#include "xi_effect.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"

/* Rewrite a single value from high-level op to XI_CALL_BUILTIN.
 * aux is set to a static name string; aux_int carries extra info. */
static inline void rewrite_to_builtin(XiValue *v, const char *name) {
    v->op = XI_CALL_BUILTIN;
    v->aux = (void *) name;
    /* Keep existing aux_int (may carry flags/field_index) */
}

/* Lower one value if it's not backend-legal. Returns true if changed. */
static bool lower_value(XiValue *v) {
    if (!v)
        return false;
    if (xi_op_is_backend_legal(v->op))
        return false;

    switch ((XiOp) v->op) {
        case XI_PRINT:
            rewrite_to_builtin(v, "print");
            break;

        case XI_STR_CONCAT:
            rewrite_to_builtin(v, "str_concat");
            break;

        case XI_ARRAY_NEW:
            rewrite_to_builtin(v, "array_new");
            break;

        case XI_MAP_NEW:
            rewrite_to_builtin(v, "map_new");
            break;

        case XI_SET_NEW:
            rewrite_to_builtin(v, "set_new");
            break;

        case XI_JSON_INIT_F:
            rewrite_to_builtin(v, "json_init_f");
            break;

        case XI_JSON_GET_F:
            rewrite_to_builtin(v, "json_get_f");
            break;

        case XI_JSON_SET_F:
            rewrite_to_builtin(v, "json_set_f");
            break;

        case XI_ITER_NEW:
            rewrite_to_builtin(v, "iter_new");
            break;

        case XI_ITER_NEXT:
            rewrite_to_builtin(v, "iter_next");
            break;

        case XI_ITER_VALID:
            rewrite_to_builtin(v, "iter_valid");
            break;

        case XI_SLICE:
            rewrite_to_builtin(v, "slice");
            break;

        case XI_RANGE:
            rewrite_to_builtin(v, "range");
            break;

        case XI_TYPEOF:
            rewrite_to_builtin(v, "typeof");
            break;

        case XI_REGEX_COMPILE:
            rewrite_to_builtin(v, "regex_compile");
            break;

        default:
            /* Unknown non-legal op — leave as-is (verifier will catch it). */
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

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            lower_value(blk->values[vi]);
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
