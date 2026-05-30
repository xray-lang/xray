/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_loop.c - Natural-loop forest for Xi IR
 *
 * ALGORITHM:
 *   1. Use the dominator tree (xi_compute_dominators).
 *   2. Scan CFG edges; (src -> hdr) is a back-edge iff hdr dominates src.
 *   3. For each header, collect the natural-loop body via reverse BFS
 *      from latch(es) to header.
 *   4. Wire parent/child/sibling by header dominance + body containment.
 *   5. Sort innermost-first by descending depth.
 */

#include "xi_loop.h"
#include "xi_analysis.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include <string.h>

/* ========== Helpers ========== */

/* Collect all blocks in the natural loop body rooted at hdr_bi,
 * given latch latch_bi.  Merges into caller-supplied bitmap. */
static void collect_loop_body(XiFunc *f, uint32_t hdr_bi, uint32_t latch_bi, uint8_t *in_loop) {
    XR_DCHECK(f != NULL, "collect_loop_body: NULL func");
    XR_DCHECK(hdr_bi < f->nblocks, "collect_loop_body: header OOB");
    XR_DCHECK(latch_bi < f->nblocks, "collect_loop_body: latch OOB");

    in_loop[hdr_bi] = 1;

    uint32_t *stack = (uint32_t *) xr_malloc(f->nblocks * sizeof(uint32_t));
    if (!stack)
        return;
    uint32_t sp = 0;

    if (!in_loop[latch_bi]) {
        in_loop[latch_bi] = 1;
        stack[sp++] = latch_bi;
    }

    while (sp > 0) {
        uint32_t b = stack[--sp];
        XiBlock *blk = f->blocks[b];
        for (uint16_t p = 0; p < blk->npreds; p++) {
            XiBlock *pred = blk->preds[p];
            if (!pred || pred->id >= f->nblocks)
                continue;
            if (in_loop[pred->id])
                continue;
            in_loop[pred->id] = 1;
            stack[sp++] = pred->id;
        }
    }
    xr_free(stack);
}

/* Find the unique out-of-loop predecessor of the header, if any. */
static XiBlock *find_preheader(XiBlock *hdr, const uint8_t *in_loop) {
    XR_DCHECK(hdr != NULL, "find_preheader: NULL header");
    XiBlock *found = NULL;
    for (uint16_t p = 0; p < hdr->npreds; p++) {
        XiBlock *cand = hdr->preds[p];
        if (!cand)
            continue;
        if (in_loop[cand->id])
            continue;
        if (found)
            return NULL; /* more than one out-of-loop pred */
        found = cand;
    }
    return found;
}

static bool loop_contains_block_raw(const XiLoop *loop, const XiBlock *blk) {
    if (!loop || !blk)
        return false;
    for (uint32_t i = 0; i < loop->nbody; i++)
        if (loop->body[i] == blk)
            return true;
    return false;
}

static int pred_index(const XiBlock *blk, const XiBlock *pred) {
    if (!blk || !pred)
        return -1;
    for (uint16_t i = 0; i < blk->npreds; i++)
        if (blk->preds[i] == pred)
            return (int) i;
    return -1;
}

static bool is_const_i64(const XiValue *v, int64_t *out) {
    if (!v || v->op != XI_CONST)
        return false;
    if (out)
        *out = v->aux_int;
    return true;
}

static bool value_is_loop_invariant(const XiLoop *loop, const XiValue *v) {
    if (!v)
        return false;
    if (v->op == XI_CONST)
        return true;
    return !loop_contains_block_raw(loop, v->block);
}

static bool loop_has_single_latch_pred(const XiLoop *loop) {
    if (!loop || !loop->header || !loop->preheader || !loop->latch)
        return false;

    uint32_t in_loop_preds = 0;
    uint32_t out_loop_preds = 0;
    for (uint16_t i = 0; i < loop->header->npreds; i++) {
        XiBlock *pred = loop->header->preds[i];
        if (!pred)
            continue;
        if (loop_contains_block_raw(loop, pred)) {
            if (pred != loop->latch)
                return false;
            in_loop_preds++;
        } else {
            if (pred != loop->preheader)
                return false;
            out_loop_preds++;
        }
    }
    return in_loop_preds == 1 && out_loop_preds == 1;
}

static bool match_basic_iv(const XiLoop *loop, const XiPhi *phi, XiBasicIV *out) {
    if (!loop || !phi || !out || !loop_has_single_latch_pred(loop))
        return false;

    const XiValue *phi_val = &phi->value;
    int pre_idx = pred_index(loop->header, loop->preheader);
    int latch_idx = pred_index(loop->header, loop->latch);
    if (pre_idx < 0 || latch_idx < 0 || pre_idx == latch_idx)
        return false;
    if ((uint16_t) pre_idx >= phi_val->nargs || (uint16_t) latch_idx >= phi_val->nargs)
        return false;

    XiValue *start = phi_val->args[pre_idx];
    XiValue *next = phi_val->args[latch_idx];
    if (!start || !next || !loop_contains_block_raw(loop, next->block))
        return false;
    if ((next->op != XI_ADD && next->op != XI_SUB) || next->nargs != 2)
        return false;

    XiValue *step = NULL;
    if (next->op == XI_ADD) {
        if (next->args[0] == phi_val)
            step = next->args[1];
        else if (next->args[1] == phi_val)
            step = next->args[0];
    } else if (next->args[0] == phi_val) {
        step = next->args[1];
    }
    if (!step || !value_is_loop_invariant(loop, step))
        return false;

    memset(out, 0, sizeof(*out));
    out->phi = (XiValue *) phi_val;
    out->start = start;
    out->next = next;
    out->step = step;
    out->step_op = (XiOp) next->op;

    int64_t step_const = 0;
    if (is_const_i64(step, &step_const)) {
        out->has_const_step = true;
        out->step_const = next->op == XI_SUB ? -step_const : step_const;
    }
    return true;
}

static XiBasicIV *find_basic_iv(const XiLoop *loop, const XiValue *v) {
    if (!loop || !v)
        return NULL;
    for (uint32_t i = 0; i < loop->nbasic_ivs; i++)
        if (loop->basic_ivs[i].phi == v)
            return &loop->basic_ivs[i];
    return NULL;
}

static bool is_basic_iv_update(const XiLoop *loop, const XiValue *v) {
    if (!loop || !v)
        return false;
    for (uint32_t i = 0; i < loop->nbasic_ivs; i++)
        if (loop->basic_ivs[i].next == v)
            return true;
    return false;
}

static bool match_linear_term(const XiLoop *loop, XiValue *v, XiValue **base, XiValue **scale,
                              bool *has_const_scale, int64_t *scale_const) {
    if (!loop || !v || !base || !scale || !has_const_scale || !scale_const)
        return false;

    XiBasicIV *basic = find_basic_iv(loop, v);
    if (basic) {
        *base = basic->phi;
        *scale = NULL;
        *has_const_scale = true;
        *scale_const = 1;
        return true;
    }

    if (v->op != XI_MUL || v->nargs != 2)
        return false;

    XiValue *lhs = v->args[0];
    XiValue *rhs = v->args[1];
    XiBasicIV *lhs_basic = find_basic_iv(loop, lhs);
    XiBasicIV *rhs_basic = find_basic_iv(loop, rhs);
    XiValue *scale_candidate = NULL;
    XiValue *base_candidate = NULL;

    if (lhs_basic && value_is_loop_invariant(loop, rhs)) {
        base_candidate = lhs_basic->phi;
        scale_candidate = rhs;
    } else if (rhs_basic && value_is_loop_invariant(loop, lhs)) {
        base_candidate = rhs_basic->phi;
        scale_candidate = lhs;
    } else {
        return false;
    }

    *base = base_candidate;
    *scale = scale_candidate;
    int64_t c = 0;
    if (is_const_i64(scale_candidate, &c)) {
        *has_const_scale = true;
        *scale_const = c;
    } else {
        *has_const_scale = false;
        *scale_const = 0;
    }
    return true;
}

static bool fill_derived_iv(const XiLoop *loop, XiValue *value, XiValue *base, XiValue *scale,
                            XiValue *offset, int offset_sign, bool has_const_scale,
                            int64_t scale_const, XiDerivedIV *out) {
    if (!loop || !value || !base || !out)
        return false;

    memset(out, 0, sizeof(*out));
    out->value = value;
    out->base = base;
    out->scale = scale;
    out->offset = offset;
    out->has_const_scale = has_const_scale;
    out->scale_const = has_const_scale ? scale_const : 0;
    if (!offset) {
        out->has_const_offset = true;
        out->offset_const = 0;
        return true;
    }

    int64_t c = 0;
    if (is_const_i64(offset, &c)) {
        out->has_const_offset = true;
        out->offset_const = offset_sign < 0 ? -c : c;
    }
    return true;
}

static bool match_derived_iv(const XiLoop *loop, XiValue *v, XiDerivedIV *out) {
    if (!loop || !v || !out || find_basic_iv(loop, v) || is_basic_iv_update(loop, v))
        return false;

    XiValue *base = NULL;
    XiValue *scale = NULL;
    bool has_const_scale = false;
    int64_t scale_const = 0;
    if (match_linear_term(loop, v, &base, &scale, &has_const_scale, &scale_const))
        return fill_derived_iv(loop, v, base, scale, NULL, 1, has_const_scale, scale_const, out);

    if ((v->op != XI_ADD && v->op != XI_SUB) || v->nargs != 2)
        return false;

    XiValue *lhs = v->args[0];
    XiValue *rhs = v->args[1];
    if (match_linear_term(loop, lhs, &base, &scale, &has_const_scale, &scale_const) &&
        value_is_loop_invariant(loop, rhs)) {
        if (v->op == XI_SUB && !is_const_i64(rhs, NULL))
            return false;
        return fill_derived_iv(loop, v, base, scale, rhs, v->op == XI_SUB ? -1 : 1, has_const_scale,
                               scale_const, out);
    }

    if (v->op == XI_ADD &&
        match_linear_term(loop, rhs, &base, &scale, &has_const_scale, &scale_const) &&
        value_is_loop_invariant(loop, lhs)) {
        return fill_derived_iv(loop, v, base, scale, lhs, 1, has_const_scale, scale_const, out);
    }

    return false;
}

static bool match_polynomial_iv(const XiLoop *loop, XiValue *v, XiPolynomialIV *out) {
    if (!loop || !v || !out || v->op != XI_MUL || v->nargs != 2)
        return false;
    XiBasicIV *lhs = find_basic_iv(loop, v->args[0]);
    XiBasicIV *rhs = find_basic_iv(loop, v->args[1]);
    if (!lhs || !rhs || lhs->phi != rhs->phi)
        return false;

    memset(out, 0, sizeof(*out));
    out->value = v;
    out->base = lhs->phi;
    return true;
}

static void analyze_basic_ivs(XiLoop *loop) {
    if (!loop || !loop->header)
        return;

    uint32_t count = 0;
    XiBasicIV tmp;
    for (XiPhi *phi = loop->header->phis; phi; phi = phi->next)
        if (match_basic_iv(loop, phi, &tmp))
            count++;
    if (count == 0)
        return;

    loop->basic_ivs = (XiBasicIV *) xr_calloc(count, sizeof(XiBasicIV));
    if (!loop->basic_ivs)
        return;
    for (XiPhi *phi = loop->header->phis; phi; phi = phi->next)
        if (match_basic_iv(loop, phi, &tmp))
            loop->basic_ivs[loop->nbasic_ivs++] = tmp;
}

static uint32_t loop_value_count(const XiLoop *loop) {
    if (!loop)
        return 0;
    uint32_t n = 0;
    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        XiBlock *blk = loop->body[bi];
        if (blk)
            n += blk->nvalues;
    }
    return n;
}

static void analyze_derived_ivs(XiLoop *loop) {
    if (!loop || loop->nbasic_ivs == 0)
        return;

    uint32_t max_values = loop_value_count(loop);
    if (max_values == 0)
        return;

    loop->derived_ivs = (XiDerivedIV *) xr_calloc(max_values, sizeof(XiDerivedIV));
    loop->polynomial_ivs = (XiPolynomialIV *) xr_calloc(max_values, sizeof(XiPolynomialIV));
    if (!loop->derived_ivs || !loop->polynomial_ivs) {
        xr_free(loop->derived_ivs);
        xr_free(loop->polynomial_ivs);
        loop->derived_ivs = NULL;
        loop->polynomial_ivs = NULL;
        return;
    }

    XiDerivedIV derived_tmp;
    XiPolynomialIV poly_tmp;
    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        XiBlock *blk = loop->body[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (match_polynomial_iv(loop, v, &poly_tmp)) {
                loop->polynomial_ivs[loop->npolynomial_ivs++] = poly_tmp;
                continue;
            }
            if (match_derived_iv(loop, v, &derived_tmp))
                loop->derived_ivs[loop->nderived_ivs++] = derived_tmp;
        }
    }
}

static void analyze_loop_ivs(XiLoop *loop) {
    analyze_basic_ivs(loop);
    analyze_derived_ivs(loop);
    loop->trip_count = xi_loop_trip_count(loop);
    loop->has_trip_count = (loop->trip_count > 0);
}

/* Allocate and populate an XiLoop from header + body bitmap. */
static XiLoop *alloc_loop(XiFunc *f, uint32_t hdr_bi, uint32_t latch_bi, const uint8_t *in_loop) {
    XR_DCHECK(f != NULL, "alloc_loop: NULL func");

    uint32_t body_count = 0;
    for (uint32_t b = 0; b < f->nblocks; b++)
        if (in_loop[b])
            body_count++;

    XiLoop *loop = (XiLoop *) xr_calloc(1, sizeof(XiLoop));
    if (!loop)
        return NULL;

    loop->header = f->blocks[hdr_bi];
    loop->latch = f->blocks[latch_bi];
    loop->preheader = find_preheader(loop->header, in_loop);

    if (body_count > 0) {
        loop->body = (XiBlock **) xr_malloc(body_count * sizeof(XiBlock *));
        if (!loop->body) {
            xr_free(loop);
            return NULL;
        }
        uint32_t w = 0;
        for (uint32_t b = 0; b < f->nblocks; b++)
            if (in_loop[b])
                loop->body[w++] = f->blocks[b];
        loop->nbody = body_count;
    }
    return loop;
}

static void free_loop(XiLoop *loop) {
    if (!loop)
        return;
    xr_free(loop->basic_ivs);
    xr_free(loop->derived_ivs);
    xr_free(loop->polynomial_ivs);
    xr_free(loop->body);
    xr_free(loop);
}

/* ========== Public API ========== */

XR_FUNC XiLoopInfo *xi_compute_loops(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_compute_loops: NULL func");
    if (f->nblocks == 0)
        return NULL;

    uint32_t n = f->nblocks;

    /* Back-edge detection: (src -> hdr) where hdr dominates src */
    uint8_t *headers = (uint8_t *) xr_calloc(n, sizeof(uint8_t));
    uint32_t *first_latch = (uint32_t *) xr_malloc(n * sizeof(uint32_t));
    uint8_t *latch_seen = (uint8_t *) xr_calloc(n, sizeof(uint8_t));
    if (!headers || !first_latch || !latch_seen) {
        xr_free(headers);
        xr_free(first_latch);
        xr_free(latch_seen);
        return NULL;
    }

    for (uint32_t s = 0; s < n; s++) {
        XiBlock *src = f->blocks[s];
        if (!src)
            continue;
        for (int k = 0; k < 2; k++) {
            XiBlock *tgt = src->succs[k];
            if (!tgt)
                continue;
            XR_DCHECK(tgt->id < n, "successor block ID out of range");
            if (!xi_dominates(tgt, src))
                continue;
            /* tgt dominates src: back-edge found */
            headers[tgt->id] = 1;
            if (!latch_seen[tgt->id]) {
                latch_seen[tgt->id] = 1;
                first_latch[tgt->id] = s;
            }
        }
    }

    uint32_t nloop = 0;
    for (uint32_t b = 0; b < n; b++)
        if (headers[b])
            nloop++;

    if (nloop == 0) {
        xr_free(headers);
        xr_free(first_latch);
        xr_free(latch_seen);
        return NULL;
    }

    /* Allocate XiLoopInfo */
    XiLoopInfo *info = (XiLoopInfo *) xr_calloc(1, sizeof(XiLoopInfo));
    if (!info) {
        xr_free(headers);
        xr_free(first_latch);
        xr_free(latch_seen);
        return NULL;
    }
    info->nblocks = n;
    info->block_to_loop = (XiLoop **) xr_calloc(n, sizeof(XiLoop *));
    info->all_loops = (XiLoop **) xr_calloc(nloop, sizeof(XiLoop *));
    if (!info->block_to_loop || !info->all_loops) {
        xi_loopinfo_free(info);
        xr_free(headers);
        xr_free(first_latch);
        xr_free(latch_seen);
        return NULL;
    }

    /* Per-loop body bitmaps for parent/child wiring */
    uint8_t *body_bitmaps = (uint8_t *) xr_calloc((size_t) nloop * n, sizeof(uint8_t));
    if (!body_bitmaps) {
        xi_loopinfo_free(info);
        xr_free(headers);
        xr_free(first_latch);
        xr_free(latch_seen);
        return NULL;
    }

    /* Build each loop's body by merging all back-edges to the same header */
    uint32_t idx = 0;
    for (uint32_t hbi = 0; hbi < n; hbi++) {
        if (!headers[hbi])
            continue;
        uint8_t *bitmap = body_bitmaps + (size_t) idx * n;

        /* Collect body from all back-edges to this header */
        for (uint32_t s = 0; s < n; s++) {
            XiBlock *src = f->blocks[s];
            if (!src)
                continue;
            for (int k = 0; k < 2; k++) {
                XiBlock *tgt = src->succs[k];
                if (!tgt || tgt->id != hbi)
                    continue;
                if (!xi_dominates(tgt, src))
                    continue;
                collect_loop_body(f, hbi, s, bitmap);
            }
        }

        XiLoop *L = alloc_loop(f, hbi, first_latch[hbi], bitmap);
        if (!L) {
            xr_free(body_bitmaps);
            xr_free(headers);
            xr_free(first_latch);
            xr_free(latch_seen);
            xi_loopinfo_free(info);
            return NULL;
        }
        L->id = idx;
        info->all_loops[idx++] = L;
    }
    info->nloop = idx;
    XR_DCHECK(idx == nloop, "loop count mismatch");

    /* Parent/child wiring: for each loop L, find the smallest
     * containing loop M (M.header dominates L.header AND
     * M.body contains L.header AND M != L). */
    for (uint32_t li = 0; li < nloop; li++) {
        XiLoop *L = info->all_loops[li];
        uint32_t hL = L->header->id;
        XiLoop *best = NULL;
        uint32_t best_size = UINT32_MAX;

        for (uint32_t mi = 0; mi < nloop; mi++) {
            if (mi == li)
                continue;
            XiLoop *M = info->all_loops[mi];
            if (!xi_dominates(M->header, L->header))
                continue;
            uint8_t *mbm = body_bitmaps + (size_t) mi * n;
            if (!mbm[hL])
                continue;
            if (M->nbody < best_size) {
                best_size = M->nbody;
                best = M;
            }
        }
        L->parent = best;
    }

    /* Link children into parent lists and compute depth */
    for (uint32_t li = 0; li < nloop; li++) {
        XiLoop *L = info->all_loops[li];
        if (!L->parent) {
            L->sibling = info->root_list;
            info->root_list = L;
        } else {
            L->sibling = L->parent->child;
            L->parent->child = L;
        }
    }
    for (uint32_t li = 0; li < nloop; li++) {
        XiLoop *L = info->all_loops[li];
        uint32_t d = 1;
        for (XiLoop *p = L->parent; p; p = p->parent)
            d++;
        L->depth = d;
    }

    /* Populate block_to_loop: innermost (deepest) enclosing loop */
    for (uint32_t b = 0; b < n; b++) {
        XiLoop *best = NULL;
        uint32_t best_depth = 0;
        for (uint32_t li = 0; li < nloop; li++) {
            if (body_bitmaps[(size_t) li * n + b]) {
                XiLoop *L = info->all_loops[li];
                if (L->depth > best_depth) {
                    best_depth = L->depth;
                    best = L;
                }
            }
        }
        info->block_to_loop[b] = best;
    }

    /* Sort innermost-first by descending depth (insertion sort) */
    for (uint32_t i = 1; i < nloop; i++) {
        XiLoop *tmp = info->all_loops[i];
        uint32_t j = i;
        while (j > 0 && info->all_loops[j - 1]->depth < tmp->depth) {
            info->all_loops[j] = info->all_loops[j - 1];
            j--;
        }
        info->all_loops[j] = tmp;
    }
    /* Re-stamp IDs to match sorted order */
    for (uint32_t i = 0; i < nloop; i++)
        info->all_loops[i]->id = i;

    for (uint32_t i = 0; i < nloop; i++)
        analyze_loop_ivs(info->all_loops[i]);

    xr_free(body_bitmaps);
    xr_free(headers);
    xr_free(first_latch);
    xr_free(latch_seen);
    f->loop_recomputes++;
    return info;
}

XR_FUNC XiLoopInfo *xi_ensure_loops(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_ensure_loops: NULL func");
    /* Cache hit only when the loop forest has already been computed
     * against the current CFG.  loop_version == 0 means "never run",
     * so we always recompute on the first call. */
    if (f->loop_version == f->cfg_version && f->loop_version != 0)
        return f->loop_cache;
    /* xi_compute_loops requires RPO + dominators; chain through the
     * cached entry points so the caller doesn't have to. */
    xi_ensure_dominators(f);
    /* Stale or first call: release whatever may be cached and rebuild. */
    if (f->loop_cache) {
        xi_loopinfo_free(f->loop_cache);
        f->loop_cache = NULL;
    }
    f->loop_cache = xi_compute_loops(f);
    f->loop_version = f->cfg_version;
    return f->loop_cache;
}

XR_FUNC void xi_loopinfo_free(XiLoopInfo *info) {
    if (!info)
        return;
    if (info->all_loops) {
        for (uint32_t i = 0; i < info->nloop; i++)
            free_loop(info->all_loops[i]);
        xr_free(info->all_loops);
    }
    xr_free(info->block_to_loop);
    xr_free(info);
}

XR_FUNC uint32_t xi_block_loop_depth(const XiLoopInfo *info, uint32_t blk_id) {
    if (!info || blk_id >= info->nblocks)
        return 0;
    XiLoop *L = info->block_to_loop[blk_id];
    return L ? L->depth : 0;
}

XR_FUNC bool xi_loop_contains_block(const XiLoop *loop, const XiBlock *blk) {
    return loop_contains_block_raw(loop, blk);
}

XR_FUNC uint32_t xi_loop_trip_count(const XiLoop *loop) {
    if (!loop || loop->nbasic_ivs == 0)
        return 0;

    const XiBasicIV *biv = &loop->basic_ivs[0];
    if (!biv->has_const_step || biv->step_const == 0)
        return 0;
    if (!biv->start || biv->start->op != XI_CONST)
        return 0;
    if (!biv->start->type || biv->start->type->kind != XR_KIND_INT)
        return 0;

    XiBlock *header = loop->header;
    if (!header || header->kind != XI_BLOCK_IF)
        return 0;
    XiValue *cond = header->control;
    if (!cond || cond->nargs != 2)
        return 0;

    XiValue *limit_val = NULL;
    bool iv_is_lhs = false;
    if (cond->args[0] == biv->phi && cond->args[1] && cond->args[1]->op == XI_CONST) {
        limit_val = cond->args[1];
        iv_is_lhs = true;
    } else if (cond->args[1] == biv->phi && cond->args[0] && cond->args[0]->op == XI_CONST) {
        limit_val = cond->args[0];
        iv_is_lhs = false;
    }
    if (!limit_val || !limit_val->type || limit_val->type->kind != XR_KIND_INT)
        return 0;

    int64_t start = biv->start->aux_int;
    int64_t step = biv->step_const;
    int64_t limit = limit_val->aux_int;

    XiBlock *body_entry = header->succs[0];
    if (!body_entry || !loop_contains_block_raw(loop, body_entry))
        return 0;

    int64_t range = 0;
    bool valid = false;

    if (biv->step_op == XI_ADD && step > 0) {
        if (iv_is_lhs && cond->op == XI_LT) {
            range = limit - start;
            valid = range > 0;
        } else if (iv_is_lhs && cond->op == XI_LE) {
            range = limit - start + 1;
            valid = range > 0;
        } else if (!iv_is_lhs && cond->op == XI_GT) {
            range = limit - start;
            valid = range > 0;
        } else if (!iv_is_lhs && cond->op == XI_GE) {
            range = limit - start + 1;
            valid = range > 0;
        } else if (iv_is_lhs && cond->op == XI_NE) {
            range = limit - start;
            valid = range > 0 && (range % step == 0);
        }
    } else if (biv->step_op == XI_ADD && step < 0) {
        int64_t abs_step = -step;
        if (iv_is_lhs && cond->op == XI_GT) {
            range = start - limit;
            valid = range > 0;
        } else if (iv_is_lhs && cond->op == XI_GE) {
            range = start - limit + 1;
            valid = range > 0;
        } else if (!iv_is_lhs && cond->op == XI_LT) {
            range = start - limit;
            valid = range > 0;
        }
        if (valid)
            step = abs_step;
    } else if (biv->step_op == XI_SUB && step < 0) {
        int64_t abs_step = -step;
        if (iv_is_lhs && cond->op == XI_GT) {
            range = start - limit;
            valid = range > 0;
        } else if (iv_is_lhs && cond->op == XI_GE) {
            range = start - limit + 1;
            valid = range > 0;
        }
        if (valid)
            step = abs_step;
    }

    if (!valid || range <= 0 || step <= 0)
        return 0;

    uint32_t trip = (uint32_t) ((range + step - 1) / step);
    if (trip > 1000000)
        return 0;

    return trip;
}
