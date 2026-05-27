/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_ivsr.c - Induction Variable Strength Reduction
 *
 * Loop-local rewrite that turns multiplications by a loop-invariant
 * constant into per-iteration additions.  Only touches the value
 * stream — block kinds, edges, and phi arity are unchanged, so the
 * dom / loop caches stay valid.
 *
 * The pattern handled is:
 *
 *   preheader:
 *     i_start = ...                (basic IV start)
 *   header:
 *     i_phi = phi(i_start, i_next) (basic IV with constant step `s`)
 *     j     = i_phi * c (+ k)      (derived IV: const scale, optional const offset)
 *   latch:
 *     i_next = i_phi + s
 *
 * is rewritten as:
 *
 *   preheader:
 *     j_start = i_start * c (+ k)
 *   header:
 *     j_phi  = phi(j_start, j_next)
 *     j      = COPY(j_phi)         (copy_prop / dce removes the multiply)
 *   latch:
 *     j_next = j_phi + (s * c)
 */

#include "xi_opt_ivsr.h"
#include "xi_loop.h"
#include "xi_analysis.h"
#include "../base/xchecks.h"
#include "../runtime/value/xtype.h"

/* ========== Eligibility ========== */

/* Required preconditions for the MVP rewrite.  Any pattern outside
 * this envelope is left untouched. */
static bool basic_iv_eligible(const XiBasicIV *biv) {
    if (!biv || !biv->phi || !biv->next || !biv->start)
        return false;
    /* Step must be a known constant — we materialise s*c at compile time. */
    return biv->has_const_step;
}

static bool derived_iv_eligible(const XiDerivedIV *div) {
    if (!div || !div->value || !div->base)
        return false;
    /* Scale must be a known constant. Offset is optional but, when
     * present, must also be constant — otherwise we cannot fold it
     * into the preheader expression. */
    if (!div->has_const_scale)
        return false;
    if (div->offset && !div->has_const_offset)
        return false;
    /* The original derived value must still be a multiplication-bearing
     * expression; if a previous pass already folded it (e.g. into a
     * COPY) there is nothing to reduce. */
    if (div->value->op != XI_MUL && div->value->op != XI_ADD && div->value->op != XI_SUB)
        return false;
    return true;
}

/* The loop must have a unique preheader and a single back-edge so the
 * generated phi has unambiguous slots. */
static bool loop_shape_eligible(const XiLoop *L) {
    if (!L || !L->header || !L->preheader || !L->latch)
        return false;
    /* MVP: header must have exactly two predecessors so j_phi is a
     * 2-arg phi — preheader and latch.  Multi-latch loops are skipped. */
    if (L->header->npreds != 2)
        return false;
    bool has_pre = false, has_latch = false;
    for (uint16_t p = 0; p < L->header->npreds; p++) {
        if (L->header->preds[p] == L->preheader)
            has_pre = true;
        else if (L->header->preds[p] == L->latch)
            has_latch = true;
    }
    return has_pre && has_latch;
}

/* ========== Rewrite ========== */

/* Build `j_start = i_start * c (+ k)` at the tail of the preheader.
 * Returns NULL on allocation failure. */
static XiValue *build_preheader_init(XiFunc *f, XiBlock *preheader, const XiBasicIV *biv,
                                     const XiDerivedIV *div) {
    XrType *ty = div->value->type;
    if (!ty)
        return NULL;
    /* j_start = i_start * scale_const */
    XiValue *scale_c = xi_const_int(f, preheader, div->scale_const, ty);
    if (!scale_c)
        return NULL;
    XiValue *mul = xi_binary(f, preheader, XI_MUL, ty, biv->start, scale_c);
    if (!mul)
        return NULL;
    if (!div->offset)
        return mul;
    /* j_start += offset_const */
    XiValue *off_c = xi_const_int(f, preheader, div->offset_const, ty);
    if (!off_c)
        return NULL;
    return xi_binary(f, preheader, XI_ADD, ty, mul, off_c);
}

/* Build `j_next = j_phi + (s * c)` at the tail of the latch. */
static XiValue *build_latch_step(XiFunc *f, XiBlock *latch, XiValue *j_phi, const XiBasicIV *biv,
                                 const XiDerivedIV *div) {
    XrType *ty = j_phi->type;
    if (!ty)
        return NULL;
    /* The composed step uses wrapping multiplication to match the
     * semantics of i_phi * scale_const inside the loop. */
    int64_t composed = (int64_t) ((uint64_t) biv->step_const * (uint64_t) div->scale_const);
    /* XI_SUB-style basic IVs (i_next = i_phi - s) negate the composed
     * step so j_next still tracks j_phi + (delta j per iteration). */
    if (biv->step_op == XI_SUB)
        composed = -composed;
    XiValue *step_c = xi_const_int(f, latch, composed, ty);
    if (!step_c)
        return NULL;
    return xi_binary(f, latch, XI_ADD, ty, j_phi, step_c);
}

/* Wire the new j_phi: args[i] mirror the slot order of header->preds[i]. */
static void wire_jphi(XiPhi *jphi, const XiBlock *header, const XiBlock *preheader,
                      XiValue *j_start, XiValue *j_next) {
    XR_DCHECK(jphi->value.nargs == header->npreds, "ivsr: phi arity mismatch");
    for (uint16_t p = 0; p < header->npreds; p++) {
        jphi->value.args[p] = (header->preds[p] == preheader) ? j_start : j_next;
    }
}

/* Rewrite a single derived IV.  Returns true on success. */
static bool reduce_derived_iv(XiFunc *f, XiLoop *L, const XiBasicIV *biv, const XiDerivedIV *div) {
    XiValue *orig = div->value;
    XrType *ty = orig->type;
    if (!ty)
        return false;

    /* Step 1: preheader init. */
    XiValue *j_start = build_preheader_init(f, L->preheader, biv, div);
    if (!j_start)
        return false;

    /* Step 2: header phi.  args wired below once j_next exists. */
    XiPhi *jphi = xi_phi_new(f, L->header, ty, L->header->npreds);
    if (!jphi)
        return false;

    /* Step 3: latch step.  Note this references j_phi (forward use) —
     * legal because j_next is defined after j_phi in dominator terms. */
    XiValue *j_next = build_latch_step(f, L->latch, &jphi->value, biv, div);
    if (!j_next)
        return false;

    /* Step 4: connect the phi to its operands. */
    wire_jphi(jphi, L->header, L->preheader, j_start, j_next);

    /* Step 5: collapse the original derived expression to a copy of
     * the new phi.  Subsequent copy_prop + dce purge the multiply. */
    orig->op = XI_COPY;
    orig->args[0] = &jphi->value;
    orig->nargs = 1;
    /* Inputs that used to feed the multiply are now unreferenced; dce
     * will reap them along with the original op. */
    return true;
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_ivsr(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_ivsr: NULL func");
    if (f->nblocks < 2)
        return xi_pass_no_change();

    /* Dominators are needed indirectly — xi_ensure_loops chains through
     * xi_ensure_dominators internally, so a single call covers both. */
    XiLoopInfo *loops = xi_ensure_loops(f);
    if (!loops || loops->nloop == 0)
        return xi_pass_no_change();

    XiPassChange chg = xi_pass_no_change();

    /* Innermost-first: outer loops never see derived IVs from inner
     * loops because XiLoopInfo segregates them per-loop. */
    for (uint32_t li = 0; li < loops->nloop; li++) {
        XiLoop *L = loops->all_loops[li];
        if (!loop_shape_eligible(L))
            continue;

        for (uint32_t di = 0; di < L->nderived_ivs; di++) {
            XiDerivedIV *div = &L->derived_ivs[di];
            if (!derived_iv_eligible(div))
                continue;
            /* Find the matching basic IV; the derived IV stores its
             * base as the IV phi value. */
            const XiBasicIV *biv = NULL;
            for (uint32_t bi = 0; bi < L->nbasic_ivs; bi++) {
                if (L->basic_ivs[bi].phi == div->base) {
                    biv = &L->basic_ivs[bi];
                    break;
                }
            }
            if (!basic_iv_eligible(biv))
                continue;

            if (reduce_derived_iv(f, L, biv, div)) {
                chg.values_changed = true;
                /* Each rewrite inserts one phi + at least three values
                 * (scale const, mul, latch step add) plus one more
                 * for the optional offset.  copy_prop / dce later
                 * remove the original mul, but the net delta tracked
                 * here reflects the insertion side. */
                chg.n_added += div->offset ? 6 : 4;
            }
        }
    }

    return chg;
}
