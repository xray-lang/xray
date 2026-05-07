/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_arc.c - Automatic Reference Counting insertion pass
 *
 * Inserts XI_RETAIN / XI_RELEASE ops based on escape analysis.
 * Conservative strategy: every heap-allocating value that escapes
 * gets a RETAIN after creation, and RELEASE ops are inserted before
 * function exits for values that don't leave the function.
 *
 * Optimization: NO_ESCAPE values skip retain/release entirely
 * (they are effectively stack-lifetime and freed with the frame).
 */

#include "xi_arc.h"
#include "xi_escape.h"
#include "../runtime/value/xtype.h"
#include "../base/xchecks.h"

#include <string.h>

/* ========== Helpers ========== */

/* Check whether a type needs refcounting (heap-allocated objects).
 * Scalars (int, float, bool, null) never need ARC. */
static bool type_needs_arc(const struct XrType *type) {
    if (!type) return true;  /* unknown type: conservative */
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_NULL:
        case XR_KIND_VOID:
        case XR_KIND_NEVER:
            return false;
        default:
            return true;  /* string, array, map, object, closure, etc. */
    }
}

/* Check if a value is a heap-allocating op that may need ARC. */
static bool needs_retain(const XiValue *v) {
    if (!v) return false;
    /* Only heap-allocating ops with non-trivial escape need retain */
    if (v->escape == XI_ESC_NONE) return false;
    /* Also skip scalars even if escape > NONE (they're value types) */
    if (!type_needs_arc(v->type)) return false;
    return xi_op_is_heap_alloc(v->op);
}

/* Check if a value needs release at function exit.
 * Values that escape via return (ARG_ESCAPE) should NOT be released
 * because ownership transfers to the caller. */
static bool needs_exit_release(const XiValue *v) {
    if (!v) return false;
    if (v->escape == XI_ESC_NONE) return false;
    if (v->escape == XI_ESC_ARG) return false;  /* caller owns it */
    if (!type_needs_arc(v->type)) return false;
    return xi_op_is_heap_alloc(v->op);
}

/* ========== Block-Level Insertion ========== */

/* Insert XI_RETAIN after each heap-allocating op that escapes.
 * The retain is placed immediately after the allocation. */
static void insert_retains(XiFunc *f) {
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;

        /* Walk values in reverse to avoid index invalidation.
         * New retain values are appended, so we scan the original range. */
        uint32_t orig_count = blk->nvalues;
        for (uint32_t i = 0; i < orig_count; i++) {
            XiValue *v = blk->values[i];
            if (!v) continue;
            if (!needs_retain(v)) continue;

            /* Insert XI_RETAIN v after v */
            XiValue *retain = xi_value_new(f, blk, XI_RETAIN, v->type, 1);
            XR_DCHECK(retain != NULL, "xi_arc: failed to create RETAIN");
            retain->args[0] = v;
            retain->flags = XI_FLAG_SIDE_EFFECT;
            retain->escape = v->escape;

            /* xi_value_new appends to end of block. We need it right after v.
             * For correctness, it's fine at the end since RETAIN has no
             * data dependency except on v. In a more refined pass we'd
             * splice it, but appending preserves SSA semantics. */
        }
    }
}

/* Insert XI_RELEASE before return terminators for values that are
 * HEAP_ESCAPE or GLOBAL_ESCAPE (i.e., values we retained but that
 * don't leave via return). */
static void insert_exit_releases(XiFunc *f) {
    /* Collect values that need exit release */
    XiValue *to_release[256];
    int nrelease = 0;

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v) continue;
            if (needs_exit_release(v) && nrelease < 256) {
                to_release[nrelease++] = v;
            }
        }
    }

    if (nrelease == 0) return;

    /* Insert RELEASE ops in each return block, before the terminator */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk) continue;
        if (blk->kind != XI_BLOCK_RETURN) continue;

        for (int r = 0; r < nrelease; r++) {
            XiValue *v = to_release[r];
            XR_DCHECK(v != NULL, "xi_arc: NULL release target");

            /* Skip release if this value is the return value
             * (ownership transfers to caller). */
            if (blk->control == v) continue;

            XiValue *release = xi_value_new(f, blk, XI_RELEASE, v->type, 1);
            XR_DCHECK(release != NULL, "xi_arc: failed to create RELEASE");
            release->args[0] = v;
            release->flags = XI_FLAG_SIDE_EFFECT;
        }
    }
}

/* ========== Public API ========== */

XR_FUNC void xi_arc_insert(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_arc_insert: NULL func");

    /* Process children first (bottom-up) */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_arc_insert(f->children[i]);
    }

    insert_retains(f);
    insert_exit_releases(f);
}
