/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_escape.c - Escape analysis for Xi IR
 *
 * Single-pass forward dataflow analysis that computes escape levels
 * for every SSA value. The analysis is conservative: if a value's
 * use-site policies come from generated Xi op metadata. Invalid op
 * values fall back to HEAP_ESCAPE, which is safe for correctness and
 * pessimistic for optimization.
 *
 * Algorithm:
 *   1. Initialize all heap-allocating values to NO_ESCAPE.
 *   2. Walk all instructions in RPO. For each use of a value,
 *      raise the value's escape level based on how it is used:
 *        - Returned from function → ARG_ESCAPE
 *        - Stored to field/index → HEAP_ESCAPE
 *        - Captured by closure → HEAP_ESCAPE
 *        - Sent to channel / stored to shared → GLOBAL_ESCAPE
 *        - Passed as call argument → HEAP_ESCAPE (conservative)
 *   3. Propagate: if value A flows into value B (e.g. A = PHI(B,C)),
 *      then A.escape = join(B.escape, C.escape).
 *   4. Repeat until fixpoint (bounded by lattice height = 4).
 *
 * Children are analyzed first (bottom-up) so that closure captures
 * can propagate escape info from inner to outer functions.
 */

#include "xi_escape.h"
#include "xi_analysis.h"
#include "xi_loop.h"
#include "xi_value_query.h"
#include "../base/xchecks.h"

#include <string.h>

/* ========== Helpers ========== */

/* Raise a value's escape level to at least `level`. */
static inline void raise_esc(XiValue *v, XiEscapeLevel level) {
    if (!v)
        return;
    XiEscapeLevel cur = (XiEscapeLevel) v->escape;
    if (level > cur)
        v->escape = (uint8_t) level;
}

/* ========== Use-Site Escape Rules ========== */

static bool is_channel_send_payload_arg(const XiValue *user, uint16_t arg_idx) {
    if (!user || arg_idx != 1)
        return false;
    if (user->op == XI_CHAN_SEND || user->op == XI_CHAN_TRY_SEND)
        return true;
    if (user->op != XI_CALL_METHOD || user->nargs < 2 || !xi_value_type_is_channel(user->args[0]))
        return false;
    const char *method = (const char *) user->aux;
    return method && (strcmp(method, "send") == 0 || strcmp(method, "trySend") == 0 ||
                      strcmp(method, "sendTimeout") == 0);
}

/* Determine the escape level that a given use-site imposes on the
 * value being used. Returns the minimum escape level required. */
static XiEscapeLevel use_escape_level(const XiValue *user, uint16_t arg_idx) {
    XR_DCHECK(user != NULL, "use_escape_level: NULL user");
    if (user->op == XI_CALL && arg_idx == 0)
        return XI_ESC_NONE; /* calling a closure does not make the closure escape */
    if (is_channel_send_payload_arg(user, arg_idx) &&
        xi_chan_send_transfer_mode(user) != XR_TRANSFER_MOVE)
        return XI_ESC_NONE;
    /* Subscript store `c[k] = v` (INDEX_SET) escapes only the STORED VALUE (and
     * key) into the heap collection; the collection itself (arg 0) is merely
     * mutated and does not escape through the write. This mirrors the per-arg
     * owned/borrow split ownership analysis already uses for the same op
     * (XI_GEN_OWN_USE_STORED_VALUE: arg 0 = container, arg 1+ = stored value),
     * so escape and ownership agree on which argument leaves the scope. Without
     * this a fresh local array/map filled via INDEX_SET is pinned at
     * HEAP_ESCAPE and can never be stack-allocated even when it stays local.
     *
     * Scoped to INDEX_SET (arrays/maps) on purpose: only collection allocations
     * (ARRAY_NEW/MAP_NEW/...) are heap-alloc ops eligible for stack allocation,
     * so this is exactly where dropping the container to NO_ESCAPE pays off.
     * STORE_FIELD/STRUCT_SET targets (instances/value structs) are not
     * heap-alloc ops, so refining their escape would change reported levels
     * without enabling any stack allocation; left conservative. */
    if (user->op == XI_INDEX_SET && arg_idx == 0)
        return XI_ESC_NONE;
    return xi_op_use_escape_level(user->op);
}

/* ========== Return Escape ========== */

/* Scan block terminators: if a block returns a value, that value
 * must be at least ARG_ESCAPE (it leaves the function boundary). */
static void mark_return_escapes(XiFunc *f) {
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        if (blk->kind != XI_BLOCK_RETURN)
            continue;
        /* Return block's control value (if any) escapes via return. */
        if (blk->control)
            raise_esc(blk->control, XI_ESC_ARG);
    }
}

/* ========== Core Analysis ========== */

/* Single forward pass: for each instruction, raise arg escape levels
 * based on how the instruction uses its arguments. Returns true if
 * any escape level changed (for fixpoint iteration). */
static bool analyze_uses(XiFunc *f) {
    bool changed = false;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;

        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;

            for (uint16_t a = 0; a < v->nargs; a++) {
                XiValue *arg = v->args[a];
                if (!arg)
                    continue;
                XiEscapeLevel req = use_escape_level(v, a);
                XiEscapeLevel cur = (XiEscapeLevel) arg->escape;
                if (req > cur) {
                    arg->escape = (uint8_t) req;
                    changed = true;
                }
            }
        }

        /* Propagate through PHI nodes: phi.escape = join(all incoming) */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            XiValue *pv = &phi->value;
            for (uint16_t a = 0; a < pv->nargs; a++) {
                if (!pv->args[a])
                    continue;
                XiEscapeLevel src = (XiEscapeLevel) pv->args[a]->escape;
                XiEscapeLevel cur = (XiEscapeLevel) pv->escape;
                XiEscapeLevel join = xi_esc_join(cur, src);
                if (join > cur) {
                    pv->escape = (uint8_t) join;
                    changed = true;
                }
            }
            /* Also propagate phi's level back to incoming values
             * (if phi is used in a high-escape context). */
            XiEscapeLevel phi_esc = (XiEscapeLevel) pv->escape;
            for (uint16_t a = 0; a < pv->nargs; a++) {
                if (!pv->args[a])
                    continue;
                if (phi_esc > (XiEscapeLevel) pv->args[a]->escape) {
                    pv->args[a]->escape = (uint8_t) phi_esc;
                    changed = true;
                }
            }
        }
    }
    return changed;
}

/* Propagate escape through COPY / MOVE / BOX / UNBOX / EXTRACT chains:
 * if the output escapes, the input must escape at least as much. */
static bool propagate_transparent(XiFunc *f) {
    bool changed = false;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            /* Transparent ops: output escape ≥ input escape, and vice versa */
            if (v->op == XI_COPY || v->op == XI_MOVE || v->op == XI_BOX || v->op == XI_UNBOX) {
                if (v->nargs < 1 || !v->args[0])
                    continue;
                XiValue *src = v->args[0];
                XiEscapeLevel ve = (XiEscapeLevel) v->escape;
                XiEscapeLevel se = (XiEscapeLevel) src->escape;
                /* Forward: if src escapes, output escapes */
                if (se > ve) {
                    v->escape = (uint8_t) se;
                    changed = true;
                }
                /* Backward: if output escapes, src must escape */
                if (ve > se) {
                    src->escape = (uint8_t) ve;
                    changed = true;
                }
            }
        }
    }
    return changed;
}

/* Mark closure captures: values captured by a child closure
 * escape to the heap (they are stored in the closure's upval array). */
static void mark_capture_escapes(XiFunc *f) {
    for (uint16_t ci = 0; ci < f->ncaptures; ci++) {
        XiCapture *cap = &f->captures[ci];
        if (cap->value)
            raise_esc(cap->value, XI_ESC_HEAP);
    }
    /* Also mark captures from children: if a child closure captures
     * a value defined in f, that value escapes to heap. */
    for (uint16_t ch = 0; ch < f->nchildren; ch++) {
        XiFunc *child = f->children[ch];
        if (!child)
            continue;
        for (uint16_t ci = 0; ci < child->ncaptures; ci++) {
            XiCapture *cap = &child->captures[ci];
            if (cap->source == XI_CAPTURE_SRC_REG && cap->value)
                raise_esc(cap->value, XI_ESC_HEAP);
        }
    }
}

/* ========== Loop-Allocation Demotion ========== */

/* A heap allocation defined inside a loop must NOT be left at NO_ESCAPE.
 *
 *   - AOT would stack-allocate it (XI_STACK_ALLOC -> alloca), but alloca in a
 *     loop accumulates one frame slot per iteration until the function returns,
 *     overflowing the native stack for large trip counts.
 *   - A NO_ESCAPE heap value is skipped by ARC (tracks_rc in xi_arc), so in the
 *     VM, which never stack-allocates, the allocation would leak one object
 *     per iteration.
 *
 * Demote such allocations to ARG_ESCAPE so BOTH backends manage them as heap
 * objects with precise dup/drop (freed at the per-iteration death point). A
 * single-execution (non-loop) allocation is left at NO_ESCAPE and still becomes
 * a safe one-shot stack allocation in AOT. */
static void demote_loop_heap_allocs(XiFunc *f) {
    if (!f->nblocks)
        return;
    xi_ensure_dominators(f);
    XiLoopInfo *loops = xi_ensure_loops(f);
    if (!loops)
        return; /* function has no loops: nothing to demote */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk || xi_block_loop_depth(loops, blk->id) == 0)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (v && v->escape == (uint8_t) XI_ESC_NONE && xi_op_is_heap_alloc(v->op))
                v->escape = (uint8_t) XI_ESC_ARG;
        }
    }
}

/* ========== Single-Function Analysis ========== */

static void analyze_func(XiFunc *f) {
    XR_DCHECK(f != NULL, "analyze_func: NULL func");

    /* Initialize: all values start at NO_ESCAPE (0). */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (blk->values[i])
                blk->values[i]->escape = 0;
        }
        for (XiPhi *phi = blk->phis; phi; phi = phi->next)
            phi->value.escape = 0;
    }

    /* Parameters: assume ARG_ESCAPE (caller controls lifetime). */
    for (uint16_t p = 0; p < f->nparams; p++) {
        if (f->params[p])
            f->params[p]->escape = (uint8_t) XI_ESC_ARG;
    }

    /* GET_SHARED / LOAD_UPVAL: value comes from outside, GLOBAL/HEAP */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            if (v->op == XI_GET_SHARED || v->op == XI_IMPORT_REF)
                v->escape = (uint8_t) XI_ESC_GLOBAL;
            else if (v->op == XI_LOAD_UPVAL)
                v->escape = (uint8_t) XI_ESC_HEAP;
        }
    }

    /* Mark return escapes */
    mark_return_escapes(f);

    /* Mark closure capture escapes */
    mark_capture_escapes(f);

    /* Fixpoint iteration (lattice height = 4, converges fast) */
    for (int iter = 0; iter < 8; iter++) {
        bool c1 = analyze_uses(f);
        bool c2 = propagate_transparent(f);
        if (!c1 && !c2)
            break;
    }

    /* Heap allocations inside loops cannot be safely stack-allocated and must
     * stay ARC-managed (see demote_loop_heap_allocs). */
    demote_loop_heap_allocs(f);
}

/* ========== Public API ========== */

XR_FUNC void xi_escape_analyze(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_escape_analyze: NULL func");

    /* Analyze children first (bottom-up) so capture info is available */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_escape_analyze(f->children[i]);
    }

    analyze_func(f);
}

/* ========== Cross-Function Escape Summary ========== */

/* Compute a summary of how each parameter escapes, so callers
 * can use precise escape info instead of conservative HEAP_ESCAPE. */
XR_FUNC bool xi_escape_compute_summary(const XiFunc *f, XiEscapeSummary *summary) {
    if (!f || !summary)
        return false;

    summary->valid = false;
    summary->nparams = 0;
    summary->return_escape = (uint8_t) XI_ESC_NONE;

    if (f->nparams > XI_ESC_SUMMARY_MAX_PARAMS)
        return false;

    summary->nparams = (uint8_t) f->nparams;

    for (uint16_t p = 0; p < f->nparams; p++) {
        if (f->params[p])
            summary->param_escape[p] = f->params[p]->escape;
        else
            summary->param_escape[p] = (uint8_t) XI_ESC_HEAP;
    }

    /* Determine return escape: scan all RETURN blocks for the
     * escape level of their control value. */
    XiEscapeLevel ret_esc = XI_ESC_NONE;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk || blk->kind != XI_BLOCK_RETURN)
            continue;
        if (blk->control) {
            XiEscapeLevel esc = (XiEscapeLevel) blk->control->escape;
            if (esc > ret_esc)
                ret_esc = esc;
        }
    }
    summary->return_escape = (uint8_t) ret_esc;
    summary->valid = true;
    return true;
}
