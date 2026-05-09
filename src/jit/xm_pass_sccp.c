/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_pass_sccp.c - Sparse Conditional Constant Propagation.
 *
 * KEY CONCEPT:
 *   A single pass that folds constants, kills unreachable blocks and
 *   simplifies branches in one fixed-point computation.  This
 *   replaces the old trio xm_pass_const_prop / xm_pass_branch_simp /
 *   xm_pass_remove_unreachable that used to require multiple pipeline
 *   rounds and whose results were frequently redundant with each
 *   other.
 *
 * ALGORITHM (Wegman-Zadeck):
 *   Lattice:   TOP > { I64(k), F64(k), BOOL(k), PTR(null) } > BOT
 *   State:     per-vreg cell + per-block reachable flag
 *   Worklists: two, for CFG edges and SSA uses
 *   Init:      entry reachable; every vreg = TOP
 *   Iterate:
 *     - CFG edge:  mark target reachable; enqueue every phi and
 *       every instruction of the target on the SSA worklist.
 *     - SSA use:   recompute the cell for that vreg.  If it moves
 *       down the lattice we push its uses and, for terminators, any
 *       newly pickable CFG edges.
 *   Rewrite:
 *     - CONST cells → emit CONST_* and rewrite defining ins to MOV.
 *     - Unreachable blocks → NOP all instructions, drop terminator.
 *     - BR with constant cond → JMP to the chosen successor.
 *
 * SIMPLIFICATIONS:
 *   - Only a conservative subset of opcodes is interpreted here:
 *     CONST_*, MOV, NEG/NOT, ADD/SUB/MUL/DIV/MOD/AND/OR/XOR/SHL/SHR,
 *     comparisons, LT/LE/EQ etc.  Unknown opcodes demote the dst to
 *     BOT, which is safe — it just misses a folding opportunity.
 *   - Side-effectful or impure ops always produce BOT regardless of
 *     their operands, so they are never rewritten.
 */

#include "xm_pass_sccp.h"

#include <string.h>
#include <stdlib.h>

#include "xm.h"
#include "xm_pass.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

/* ========== Lattice cell ========== */

typedef enum {
    SCCP_TOP = 0,  // unseen — default after xr_calloc
    SCCP_CONST_I64,
    SCCP_CONST_F64,
    SCCP_CONST_BOOL,
    SCCP_CONST_PTR,
    SCCP_BOT,
} SccpKind;

typedef struct {
    SccpKind kind;
    int64_t i64;  // payload for I64 / BOOL
    double f64;   // payload for F64
    void *ptr;    // payload for PTR (only meaningful when ==NULL: null pointer)
} SccpVal;

/* ========== Worklists ========== */

typedef struct {
    uint32_t from_bi;
    uint32_t to_bi;
} CfgEdge;

typedef struct {
    CfgEdge *edges;
    uint32_t len;
    uint32_t cap;
} CfgWl;

typedef struct {
    uint32_t *vregs;  // vreg indices pending re-evaluation
    uint32_t len;
    uint32_t cap;
} SsaWl;

typedef struct {
    XmFunc *func;
    SccpVal *cells;   // [nvreg]
    bool *reachable;  // [nblk]
    bool *exec_edge;  // [nblk*2]  (ei*2+0 = s1 edge, +1 = s2 edge)
    CfgWl cfg;
    SsaWl ssa;
} SccpCtx;

/* ========== Helpers ========== */

static void cfg_push(CfgWl *w, uint32_t from, uint32_t to) {
    if (w->len >= w->cap) {
        w->cap = w->cap ? w->cap * 2 : 64;
        XR_REALLOC_OR_ABORT(w->edges, w->cap * sizeof(CfgEdge), "sccp cfg wl");
    }
    w->edges[w->len++] = (CfgEdge) {from, to};
}

static bool cfg_pop(CfgWl *w, CfgEdge *out) {
    if (w->len == 0)
        return false;
    *out = w->edges[--w->len];
    return true;
}

static void ssa_push(SsaWl *w, uint32_t v) {
    if (w->len >= w->cap) {
        w->cap = w->cap ? w->cap * 2 : 64;
        XR_REALLOC_OR_ABORT(w->vregs, w->cap * sizeof(uint32_t), "sccp ssa wl");
    }
    w->vregs[w->len++] = v;
}

static bool ssa_pop(SsaWl *w, uint32_t *out) {
    if (w->len == 0)
        return false;
    *out = w->vregs[--w->len];
    return true;
}

/* Locate a block's index in func->blocks[].  Linear scan is fine
 * because it only runs during edge bookkeeping for terminators. */
static uint32_t block_index(XmFunc *func, const XmBlock *blk) {
    for (uint32_t i = 0; i < func->nblk; i++)
        if (func->blocks[i] == blk)
            return i;
    return UINT32_MAX;
}

/* Convert a ref into a lattice value without touching cells[]. */
static SccpVal ref_value(SccpCtx *ctx, XmRef ref) {
    SccpVal v = {SCCP_TOP, 0, 0.0, NULL};
    XmFunc *func = ctx->func;
    if (xm_ref_is_none(ref)) {
        v.kind = SCCP_BOT;
        return v;
    }
    if (xm_ref_is_const(ref)) {
        uint32_t ci = XM_REF_INDEX(ref);
        if (ci >= func->nconst) {
            v.kind = SCCP_BOT;
            return v;
        }
        XmConst *c = &func->consts[ci];
        switch (c->rep) {
            case XR_REP_I64:
                v.kind = SCCP_CONST_I64;
                v.i64 = c->val.i64;
                break;
            case XR_REP_F64:
                v.kind = SCCP_CONST_F64;
                v.f64 = c->val.f64;
                break;
            case XR_REP_PTR:
                v.kind = SCCP_CONST_PTR;
                v.ptr = c->val.ptr;
                break;
            default:
                v.kind = SCCP_BOT;
                break;
        }
        return v;
    }
    if (xm_ref_is_vreg(ref)) {
        uint32_t vi = XM_REF_INDEX(ref);
        if (vi < func->nvreg)
            return ctx->cells[vi];
    }
    v.kind = SCCP_BOT;
    return v;
}

static bool sccp_val_eq(SccpVal a, SccpVal b) {
    if (a.kind != b.kind)
        return false;
    switch (a.kind) {
        case SCCP_CONST_I64:
            return a.i64 == b.i64;
        case SCCP_CONST_F64:
            return a.f64 == b.f64;
        case SCCP_CONST_BOOL:
            return a.i64 == b.i64;
        case SCCP_CONST_PTR:
            return a.ptr == b.ptr;
        default:
            return true;
    }
}

/* Lattice meet: same const stays, otherwise the result is BOT. */
static SccpVal sccp_meet(SccpVal a, SccpVal b) {
    if (a.kind == SCCP_TOP)
        return b;
    if (b.kind == SCCP_TOP)
        return a;
    if (a.kind == SCCP_BOT || b.kind == SCCP_BOT) {
        SccpVal r = {SCCP_BOT, 0, 0.0, NULL};
        return r;
    }
    if (sccp_val_eq(a, b))
        return a;
    SccpVal r = {SCCP_BOT, 0, 0.0, NULL};
    return r;
}

/* ========== Evaluate an instruction ========== */

static bool both_i64(SccpVal a, SccpVal b) {
    return a.kind == SCCP_CONST_I64 && b.kind == SCCP_CONST_I64;
}
static bool both_f64(SccpVal a, SccpVal b) {
    return a.kind == SCCP_CONST_F64 && b.kind == SCCP_CONST_F64;
}

static SccpVal sccp_bot(void) {
    SccpVal r = {SCCP_BOT, 0, 0.0, NULL};
    return r;
}
static SccpVal sccp_i64(int64_t v) {
    SccpVal r = {SCCP_CONST_I64, v, 0.0, NULL};
    return r;
}
static SccpVal sccp_f64(double v) {
    SccpVal r = {SCCP_CONST_F64, 0, v, NULL};
    return r;
}
static SccpVal sccp_bool(bool b) {
    SccpVal r = {SCCP_CONST_BOOL, b ? 1 : 0, 0.0, NULL};
    return r;
}

/* Evaluate a single instruction under the current cell state.
 * Returns the lattice value the dst vreg should take.  Side-effectful
 * or unmodeled instructions default to BOT so they stay in place. */
static SccpVal eval_ins(SccpCtx *ctx, XmIns *ins) {
    /* Never fold side-effecting or may-throw ops; their dst is
     * observable through world effects. */
    if (ins->flags & (XM_FLAG_SIDE_EFFECT | XM_FLAG_MAY_THROW))
        return sccp_bot();

    SccpVal a = ref_value(ctx, ins->args[0]);
    SccpVal b = ref_value(ctx, ins->args[1]);

    switch (ins->op) {
        /* Literal materialisers: dst takes the const arg value. */
        case XM_CONST_I64:
            return a.kind == SCCP_CONST_I64 ? a : sccp_bot();
        case XM_CONST_F64:
            return a.kind == SCCP_CONST_F64 ? a : sccp_bot();
        case XM_CONST_PTR:
            return a.kind == SCCP_CONST_PTR ? a : sccp_bot();

        case XM_MOV:
            /* Propagate the argument cell unchanged. */
            return a;

        /* --- Integer arithmetic --- */
        case XM_NEG:
            if (a.kind == SCCP_CONST_I64)
                return sccp_i64(-a.i64);
            if (a.kind == SCCP_CONST_F64)
                return sccp_f64(-a.f64);
            return sccp_bot();

        case XM_NOT:
            if (a.kind == SCCP_CONST_I64)
                return sccp_i64(~a.i64);
            return sccp_bot();

        case XM_ADD:
            if (both_i64(a, b))
                return sccp_i64((int64_t) ((uint64_t) a.i64 + (uint64_t) b.i64));
            return sccp_bot();
        case XM_SUB:
            if (both_i64(a, b))
                return sccp_i64((int64_t) ((uint64_t) a.i64 - (uint64_t) b.i64));
            return sccp_bot();
        case XM_MUL:
            if (both_i64(a, b))
                return sccp_i64((int64_t) ((uint64_t) a.i64 * (uint64_t) b.i64));
            return sccp_bot();
        case XM_DIV:
            if (both_i64(a, b) && b.i64 != 0)
                return sccp_i64(a.i64 / b.i64);
            return sccp_bot();
        case XM_MOD:
            if (both_i64(a, b) && b.i64 != 0)
                return sccp_i64(a.i64 % b.i64);
            return sccp_bot();

        case XM_AND:
            if (both_i64(a, b))
                return sccp_i64(a.i64 & b.i64);
            return sccp_bot();
        case XM_OR:
            if (both_i64(a, b))
                return sccp_i64(a.i64 | b.i64);
            return sccp_bot();
        case XM_XOR:
            if (both_i64(a, b))
                return sccp_i64(a.i64 ^ b.i64);
            return sccp_bot();
        case XM_SHL:
            if (both_i64(a, b) && b.i64 >= 0 && b.i64 < 64)
                return sccp_i64((int64_t) ((uint64_t) a.i64 << b.i64));
            return sccp_bot();
        case XM_SHR:
            if (both_i64(a, b) && b.i64 >= 0 && b.i64 < 64)
                return sccp_i64((int64_t) ((uint64_t) a.i64 >> b.i64));
            return sccp_bot();

        /* --- Float arithmetic --- */
        case XM_FADD:
            if (both_f64(a, b))
                return sccp_f64(a.f64 + b.f64);
            return sccp_bot();
        case XM_FSUB:
            if (both_f64(a, b))
                return sccp_f64(a.f64 - b.f64);
            return sccp_bot();
        case XM_FMUL:
            if (both_f64(a, b))
                return sccp_f64(a.f64 * b.f64);
            return sccp_bot();
        case XM_FDIV:
            if (both_f64(a, b) && b.f64 != 0.0)
                return sccp_f64(a.f64 / b.f64);
            return sccp_bot();
        case XM_FNEG:
            if (a.kind == SCCP_CONST_F64)
                return sccp_f64(-a.f64);
            return sccp_bot();

        /* --- Integer comparisons (produce BOOL) --- */
        case XM_LT:
            if (both_i64(a, b))
                return sccp_bool(a.i64 < b.i64);
            return sccp_bot();
        case XM_LE:
            if (both_i64(a, b))
                return sccp_bool(a.i64 <= b.i64);
            return sccp_bot();
        case XM_EQ:
            if (both_i64(a, b))
                return sccp_bool(a.i64 == b.i64);
            return sccp_bot();

        case XM_FLT:
            if (both_f64(a, b))
                return sccp_bool(a.f64 < b.f64);
            return sccp_bot();
        case XM_FLE:
            if (both_f64(a, b))
                return sccp_bool(a.f64 <= b.f64);
            return sccp_bot();
        case XM_FEQ:
            if (both_f64(a, b))
                return sccp_bool(a.f64 == b.f64);
            return sccp_bot();

        default:
            /* Unmodeled pure op with known-constant inputs still
             * becomes BOT.  Post-SCCP passes (canonicalize, GVN,
             * specialize) can fold these on their own terms. */
            return sccp_bot();
    }
}

/* Evaluate a Phi node: meet over args whose incoming edge is reachable. */
static SccpVal eval_phi(SccpCtx *ctx, XmBlock *blk, XmPhi *phi) {
    SccpVal acc = {SCCP_TOP, 0, 0.0, NULL};
    for (uint32_t i = 0; i < phi->narg; i++) {
        if (i >= blk->npred)
            break;
        XmBlock *pred = blk->preds[i];
        uint32_t pi = block_index(ctx->func, pred);
        if (pi == UINT32_MAX)
            continue;
        /* Only consider arguments reaching us through an executable
         * edge.  This is the "conditional" part of SCCP. */
        bool edge_exec = false;
        if (pred->s1 == blk && ctx->exec_edge[pi * 2 + 0])
            edge_exec = true;
        if (pred->s2 == blk && ctx->exec_edge[pi * 2 + 1])
            edge_exec = true;
        if (!edge_exec)
            continue;
        acc = sccp_meet(acc, ref_value(ctx, phi->args[i]));
        if (acc.kind == SCCP_BOT)
            break;
    }
    return acc;
}

/* Mark an edge executable; queue the target for re-visit if newly
 * reachable or if the edge transition changes anything. */
static void mark_edge(SccpCtx *ctx, uint32_t from_bi, uint32_t to_bi, bool take_s1) {
    uint32_t slot = from_bi * 2 + (take_s1 ? 0 : 1);
    if (ctx->exec_edge[slot])
        return;  // already executable
    ctx->exec_edge[slot] = true;
    if (!ctx->reachable[to_bi]) {
        ctx->reachable[to_bi] = true;
    }
    cfg_push(&ctx->cfg, from_bi, to_bi);
}

/* After updating a cell, enqueue every use of |v| on the SSA
 * worklist so the propagation continues.  We do not maintain an
 * explicit use list; a block-level re-scan of users happens via the
 * cfg-edge-driven path instead.  The SSA worklist here stores vreg
 * indices whose defining instructions should be re-evaluated, and
 * the main driver also re-evaluates every reachable use when
 * draining it. */
static void enqueue_uses(SccpCtx *ctx, uint32_t v) {
    XmFunc *func = ctx->func;
    /* Dumb but fast: every reachable instruction that reads v is
     * pushed.  We push the dst vreg of such instructions so the
     * driver re-evaluates them.  Phi uses are covered at the block
     * level via the cfg worklist. */
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        if (!ctx->reachable[bi])
            continue;
        XmBlock *blk = func->blocks[bi];
        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];
            if (!xm_ref_is_vreg(ins->dst))
                continue;
            XmRef va = ins->args[0], vb = ins->args[1];
            if ((xm_ref_is_vreg(va) && XM_REF_INDEX(va) == v) ||
                (xm_ref_is_vreg(vb) && XM_REF_INDEX(vb) == v)) {
                ssa_push(&ctx->ssa, XM_REF_INDEX(ins->dst));
            }
        }
        for (XmPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!xm_ref_is_vreg(phi->dst))
                continue;
            for (uint32_t k = 0; k < phi->narg; k++) {
                XmRef a = phi->args[k];
                if (xm_ref_is_vreg(a) && XM_REF_INDEX(a) == v) {
                    ssa_push(&ctx->ssa, XM_REF_INDEX(phi->dst));
                    break;
                }
            }
        }
    }
}

/* Recompute the cell for a single vreg by finding its defining
 * instruction.  Phi definitions use eval_phi; instructions use
 * eval_ins.  If the cell moves down the lattice, queue its uses and
 * — for the terminator of the block — possibly reveal new CFG
 * edges. */
static void revisit_vreg(SccpCtx *ctx, uint32_t v) {
    XmFunc *func = ctx->func;
    if (v >= func->nvreg)
        return;
    XmIns *def = func->vregs[v].def;

    SccpVal old = ctx->cells[v];
    SccpVal neu = old;

    if (!def) {
        /* No defining instruction: parameter or phi.  Search phis
         * across reachable blocks. */
        bool found = false;
        for (uint32_t bi = 0; bi < func->nblk && !found; bi++) {
            if (!ctx->reachable[bi])
                continue;
            XmBlock *blk = func->blocks[bi];
            for (XmPhi *phi = blk->phis; phi; phi = phi->next) {
                if (xm_ref_is_vreg(phi->dst) && XM_REF_INDEX(phi->dst) == v) {
                    neu = eval_phi(ctx, blk, phi);
                    found = true;
                    break;
                }
            }
        }
        if (!found)
            neu = sccp_bot();  // parameters start BOT
    } else {
        neu = eval_ins(ctx, def);
    }

    if (!sccp_val_eq(old, neu)) {
        ctx->cells[v] = neu;
        enqueue_uses(ctx, v);
    }
}

/* Evaluate a block's terminator and queue CFG edges accordingly. */
static void visit_terminator(SccpCtx *ctx, XmBlock *blk, uint32_t bi) {
    if (blk->jmp.type == XM_JMP_JMP && blk->s1) {
        uint32_t to = block_index(ctx->func, blk->s1);
        if (to != UINT32_MAX)
            mark_edge(ctx, bi, to, true);
    } else if (blk->jmp.type == XM_JMP_BR && blk->s1 && blk->s2) {
        SccpVal c = ref_value(ctx, blk->jmp.arg);
        uint32_t to1 = block_index(ctx->func, blk->s1);
        uint32_t to2 = block_index(ctx->func, blk->s2);
        if (c.kind == SCCP_CONST_BOOL || c.kind == SCCP_CONST_I64) {
            if (c.i64 != 0) {
                if (to1 != UINT32_MAX)
                    mark_edge(ctx, bi, to1, true);
            } else {
                if (to2 != UINT32_MAX)
                    mark_edge(ctx, bi, to2, false);
            }
        } else {
            /* Unknown condition: both edges may execute. */
            if (to1 != UINT32_MAX)
                mark_edge(ctx, bi, to1, true);
            if (to2 != UINT32_MAX)
                mark_edge(ctx, bi, to2, false);
        }
    }
    /* RET / NONE: no out-edge. */
}

/* ========== Rewriting phase ========== */

/* Produce or reuse a const ref for the given i64 value. */
static XmRef emit_const_i64(XmFunc *func, int64_t v) {
    return xm_const_i64(func, v);
}
static XmRef emit_const_f64(XmFunc *func, double v) {
    return xm_const_f64(func, v);
}

/* Rewrite an instruction when its dst became a constant. */
static void rewrite_to_const(SccpCtx *ctx, XmIns *ins, SccpVal v) {
    switch (v.kind) {
        case SCCP_CONST_I64:
        case SCCP_CONST_BOOL: {
            XmRef k = emit_const_i64(ctx->func, v.i64);
            ins->op = XM_CONST_I64;
            ins->args[0] = k;
            ins->args[1] = XM_NONE;
            break;
        }
        case SCCP_CONST_F64: {
            XmRef k = emit_const_f64(ctx->func, v.f64);
            ins->op = XM_CONST_F64;
            ins->args[0] = k;
            ins->args[1] = XM_NONE;
            break;
        }
        case SCCP_CONST_PTR:
            /* Only NULL ptrs can be fully materialised; others are
             * already in the const pool, so we leave the ins alone. */
            if (v.ptr == NULL) {
                ins->op = XM_CONST_PTR;
                ins->args[0] = xm_const_ptr(ctx->func, NULL);
                ins->args[1] = XM_NONE;
            }
            break;
        default:
            break;
    }
}

static bool rewrite_function(SccpCtx *ctx) {
    XmFunc *func = ctx->func;
    bool any = false;

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (!blk)
            continue;

        if (!ctx->reachable[bi]) {
            /* Unreachable: NOP every instruction, drop terminator. */
            for (uint32_t i = 0; i < blk->nins; i++) {
                blk->ins[i].op = XM_NOP;
                blk->ins[i].dst = XM_NONE;
                blk->ins[i].args[0] = XM_NONE;
                blk->ins[i].args[1] = XM_NONE;
                blk->ins[i].flags = 0;
            }
            blk->phis = NULL;
            blk->jmp.type = XM_JMP_NONE;
            blk->jmp.arg = XM_NONE;
            blk->s1 = NULL;
            blk->s2 = NULL;
            any = true;
            continue;
        }

        /* Instruction-level rewrite: fold any vreg we now know to be a
         * literal. */
        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];
            if (!xm_ref_is_vreg(ins->dst))
                continue;
            uint32_t v = XM_REF_INDEX(ins->dst);
            if (v >= func->nvreg)
                continue;
            SccpVal cell = ctx->cells[v];
            if (cell.kind == SCCP_CONST_I64 || cell.kind == SCCP_CONST_BOOL ||
                cell.kind == SCCP_CONST_F64 || (cell.kind == SCCP_CONST_PTR && cell.ptr == NULL)) {
                rewrite_to_const(ctx, ins, cell);
                any = true;
            }
        }

        /* Simplify BR with a now-constant condition into an
         * unconditional JMP.  The unreached successor will be compacted
         * away by the driver after rewrite completes. */
        if (blk->jmp.type == XM_JMP_BR) {
            SccpVal c = ref_value(ctx, blk->jmp.arg);
            if (c.kind == SCCP_CONST_BOOL || c.kind == SCCP_CONST_I64) {
                XmBlock *taken = (c.i64 != 0) ? blk->s1 : blk->s2;
                blk->jmp.type = XM_JMP_JMP;
                blk->jmp.arg = XM_NONE;
                blk->s1 = taken;
                blk->s2 = NULL;
                any = true;
            }
        }
    }
    return any;
}

/* ========== Driver ========== */

XmPassChange xm_pass_sccp(XmFunc *func) {
    if (!func || func->nblk == 0 || func->nvreg == 0)
        return xm_pass_no_change();

    SccpCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.func = func;
    ctx.cells = (SccpVal *) xr_calloc(func->nvreg, sizeof(SccpVal));
    ctx.reachable = (bool *) xr_calloc(func->nblk, sizeof(bool));
    ctx.exec_edge = (bool *) xr_calloc((size_t) func->nblk * 2, sizeof(bool));
    if (!ctx.cells || !ctx.reachable || !ctx.exec_edge) {
        xr_free(ctx.cells);
        xr_free(ctx.reachable);
        xr_free(ctx.exec_edge);
        return xm_pass_no_change();
    }

    /* Kick off with the entry block executable.  Every other block
     * remains unreachable until proven otherwise. */
    ctx.reachable[0] = true;
    cfg_push(&ctx.cfg, 0, 0);  // sentinel: process entry's body

    /* Exception handler (catch) blocks are reachable via longjmp —
     * not through normal CFG edges.  Seed them now so the compaction
     * phase does not eliminate them. */
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (blk->exception_handler) {
            uint32_t eh_bi = block_index(func, blk->exception_handler);
            if (eh_bi < func->nblk && !ctx.reachable[eh_bi]) {
                ctx.reachable[eh_bi] = true;
                cfg_push(&ctx.cfg, bi, eh_bi);
            }
        }
    }

    /* Main driver: alternately drain both worklists until both are
     * empty.  Order does not matter for correctness. */
    while (ctx.cfg.len > 0 || ctx.ssa.len > 0) {
        CfgEdge ce;
        if (cfg_pop(&ctx.cfg, &ce)) {
            XmBlock *blk = func->blocks[ce.to_bi];
            if (!blk)
                continue;
            /* First visit: evaluate every phi + instruction + terminator. */
            for (XmPhi *phi = blk->phis; phi; phi = phi->next) {
                if (!xm_ref_is_vreg(phi->dst))
                    continue;
                uint32_t dv = XM_REF_INDEX(phi->dst);
                if (dv >= func->nvreg)
                    continue;
                SccpVal neu = eval_phi(&ctx, blk, phi);
                if (!sccp_val_eq(ctx.cells[dv], neu)) {
                    ctx.cells[dv] = neu;
                    enqueue_uses(&ctx, dv);
                }
            }
            for (uint32_t i = 0; i < blk->nins; i++) {
                XmIns *ins = &blk->ins[i];
                if (!xm_ref_is_vreg(ins->dst))
                    continue;
                uint32_t dv = XM_REF_INDEX(ins->dst);
                if (dv >= func->nvreg)
                    continue;
                SccpVal neu = eval_ins(&ctx, ins);
                if (!sccp_val_eq(ctx.cells[dv], neu)) {
                    ctx.cells[dv] = neu;
                    enqueue_uses(&ctx, dv);
                }
            }
            visit_terminator(&ctx, blk, ce.to_bi);
            continue;
        }

        uint32_t v;
        if (ssa_pop(&ctx.ssa, &v)) {
            revisit_vreg(&ctx, v);
            /* Re-visit every terminator that reads v, in case a BR
             * condition just resolved.  Block-level scan is cheap
             * enough at Phase 1 granularity; a dedicated use list
             * would tighten this. */
            for (uint32_t bi = 0; bi < func->nblk; bi++) {
                if (!ctx.reachable[bi])
                    continue;
                XmBlock *blk = func->blocks[bi];
                if (blk->jmp.type == XM_JMP_BR && xm_ref_is_vreg(blk->jmp.arg) &&
                    XM_REF_INDEX(blk->jmp.arg) == v) {
                    visit_terminator(&ctx, blk, bi);
                }
            }
        }
    }

    bool any_rewrite = rewrite_function(&ctx);

    /* Compact: physically remove unreachable blocks from the block
     * array so downstream passes never see them.  Entry (block 0) is
     * always reachable by construction. */
    uint32_t write = 0;
    for (uint32_t i = 0; i < func->nblk; i++) {
        if (ctx.reachable[i]) {
            func->blocks[write] = func->blocks[i];
            func->blocks[write]->id = write;
            write++;
        }
    }
    bool blocks_removed = (write < func->nblk);
    func->nblk = write;

    xr_free(ctx.cells);
    xr_free(ctx.reachable);
    xr_free(ctx.exec_edge);
    xr_free(ctx.cfg.edges);
    xr_free(ctx.ssa.vregs);
    return (any_rewrite || blocks_removed) ? (XmPassChange) {blocks_removed, true, true, 0, 0, 0}
                                           : xm_pass_no_change();
}
