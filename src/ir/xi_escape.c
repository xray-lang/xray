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
 * escape level cannot be determined precisely, it defaults to
 * GLOBAL_ESCAPE (safe for correctness, pessimistic for optimization).
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
#include "../base/xchecks.h"

/* ========== Helpers ========== */

/* Raise a value's escape level to at least `level`. */
static inline void raise_esc(XiValue *v, XiEscapeLevel level) {
    if (!v)
        return;
    XiEscapeLevel cur = (XiEscapeLevel) v->escape;
    if (level > cur)
        v->escape = (uint8_t) level;
}

/* Raise escape level of source value that flows into a destination.
 * If dst is a heap-allocating value, it needs at least HEAP_ESCAPE
 * because its contents are stored into dst's heap object. */
static inline void raise_arg_esc(XiValue *arg, XiEscapeLevel level) {
    if (!arg)
        return;
    raise_esc(arg, level);
    /* Transitivity: if arg was produced by a PHI or COPY, we need to
     * propagate through those in the fixpoint loop, not here. */
}

/* ========== Use-Site Escape Rules ========== */

/* Determine the escape level that a given use-site imposes on the
 * value being used. Returns the minimum escape level required. */
static XiEscapeLevel use_escape_level(const XiValue *user, uint16_t arg_idx) {
    XR_DCHECK(user != NULL, "use_escape_level: NULL user");

    switch (user->op) {
        /* ---- GLOBAL_ESCAPE: leaves the function's world ---- */
        case XI_SET_SHARED: /* stored to module-level shared array */
        case XI_SET_GLOBAL: /* stored to module-level globals dict */
        case XI_GO:         /* launched as goroutine */
        case XI_CHAN_SEND:  /* sent through channel */
        case XI_CHAN_TRY_SEND:
            return XI_ESC_GLOBAL;

        /* ---- HEAP_ESCAPE: stored into a heap object ---- */
        case XI_STORE_FIELD: /* obj.field = value */
        case XI_STRUCT_SET:  /* struct.field = value */
        case XI_INDEX_SET:   /* obj[key] = value */
        case XI_JSON_INIT_F: /* json field init */
        case XI_JSON_SET_F:  /* json field set */
        case XI_STORE_UPVAL: /* captured variable mutation */
            /* arg 0 is the container (already heap-escaped by being alive);
             * arg 1+ is the stored value. */
            return (arg_idx == 0) ? XI_ESC_HEAP : XI_ESC_HEAP;

        /* ---- HEAP_ESCAPE: closure captures ---- */
        case XI_CLOSURE_NEW:
            /* arg_idx 0 is the function ref (not a real capture),
             * all other args are captured values → HEAP_ESCAPE. */
            return (arg_idx >= 1) ? XI_ESC_HEAP : XI_ESC_NONE;

        /* ---- HEAP_ESCAPE: tuple stores every element ---- */
        case XI_TUPLE_NEW:
            /* Every arg is a tuple element retained by the new tuple.
             * Tuples are immutable, so this is the only writer. */
            return XI_ESC_HEAP;

        /* ---- ARG_ESCAPE: passed out of function ---- */
        case XI_THROW: /* exception leaves the function */
            return XI_ESC_ARG;

        /* ---- Call arguments: conservative HEAP_ESCAPE ---- */
        case XI_CALL:
        case XI_CALL_METHOD:
        case XI_CALL_BUILTIN:
            /* Callee might store the argument in a heap object.
             * A more precise analysis would use callee summaries. */
            return XI_ESC_HEAP;

        /* ---- Print consumes value but doesn't store it ---- */
        case XI_PRINT:
            return XI_ESC_NONE;

        /* ---- Arithmetic / comparison: no escape ---- */
        case XI_ADD:
        case XI_SUB:
        case XI_MUL:
        case XI_DIV:
        case XI_MOD:
        case XI_NEG:
        case XI_BAND:
        case XI_BOR:
        case XI_BXOR:
        case XI_BNOT:
        case XI_SHL:
        case XI_SHR:
        case XI_EQ:
        case XI_NE:
        case XI_LT:
        case XI_LE:
        case XI_GT:
        case XI_GE:
        case XI_EQ_STRICT:
        case XI_NE_STRICT:
        case XI_NOT:
        case XI_ISNULL:
        case XI_IS:
        case XI_AS:
        case XI_CONVERT:
        case XI_TYPEOF:
        case XI_NARROW_I8:
        case XI_NARROW_U8:
        case XI_NARROW_I16:
        case XI_NARROW_U16:
        case XI_NARROW_I32:
        case XI_NARROW_U32:
        case XI_NARROW_F32:
        case XI_WIDEN_I8:
        case XI_WIDEN_U8:
        case XI_WIDEN_I16:
        case XI_WIDEN_U16:
        case XI_WIDEN_I32:
        case XI_WIDEN_U32:
        case XI_WIDEN_F32:
            return XI_ESC_NONE;

        /* ---- BOX / UNBOX: transparent ---- */
        case XI_BOX:
        case XI_UNBOX:
        case XI_COPY:
        case XI_EXTRACT:
            return XI_ESC_NONE;

        /* ---- Container reads: no escape of the key/index arg ---- */
        case XI_INDEX_GET:
        case XI_LOAD_FIELD:
        case XI_STRUCT_GET:
        case XI_JSON_GET_F:
        case XI_TUPLE_GET:
            return XI_ESC_NONE;

        /* ---- Assertions: consume value for checking, no escape ---- */
        case XI_ASSERT:
        case XI_ASSERT_EQ:
        case XI_ASSERT_NE:
        case XI_ASSERT_THROWS:
            return XI_ESC_NONE;

        /* ---- Iteration: iterator value stays local typically ---- */
        case XI_ITER_NEW:
        case XI_ITER_NEXT:
        case XI_ITER_VALID:
            return XI_ESC_NONE;

        /* ---- Slice / range: creates new value but doesn't escape arg ---- */
        case XI_SLICE:
        case XI_RANGE:
            return XI_ESC_NONE;

        /* ---- Await: result comes back, arg is the awaited task ---- */
        case XI_AWAIT:
            return XI_ESC_NONE;

        /* ---- Channel receive / scope / defer ---- */
        case XI_CHAN_RECV:
        case XI_CHAN_TRY_RECV:
        case XI_CHAN_NEW:
        case XI_SCOPE_ENTER:
        case XI_SCOPE_EXIT:
        case XI_DEFER:
        case XI_YIELD:
            return XI_ESC_NONE;

        /* ---- PHI / structural: handled in fixpoint ---- */
        case XI_PHI:
        case XI_PARAM:
        case XI_CONST:
        case XI_GET_SHARED:
        case XI_LOAD_UPVAL:
        case XI_GET_BUILTIN:
        case XI_IMPORT_REF:
        case XI_MULTI_RET:
            return XI_ESC_NONE;

        /* ---- Exception handling ---- */
        case XI_TRY:
        case XI_CATCH:
        case XI_FINALLY:
        case XI_END_TRY:
            return XI_ESC_NONE;

        /* ---- Containers: args are elements stored in heap ---- */
        case XI_ARRAY_NEW:
        case XI_MAP_NEW:
        case XI_SET_NEW:
        case XI_JSON_NEW:
            return XI_ESC_NONE; /* args are capacity hints, not stored */

        case XI_STR_CONCAT:
            return XI_ESC_NONE; /* inputs consumed, new string produced */

        case XI_STRUCT_NEW: /* args[0] = class ref, not stored */
        case XI_CLASS_CREATE:
        case XI_REGEX_COMPILE:
            return XI_ESC_NONE;

        default:
            /* Unknown op: conservative — assume it escapes to heap */
            return XI_ESC_HEAP;
    }
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

/* Propagate escape through COPY / BOX / UNBOX / EXTRACT chains:
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
            if (v->op == XI_COPY || v->op == XI_BOX || v->op == XI_UNBOX || v->op == XI_EXTRACT) {
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
