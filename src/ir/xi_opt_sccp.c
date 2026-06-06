/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_sccp.c - Sparse Conditional Constant Propagation for Xi IR
 *
 * ALGORITHM (Wegman-Zadeck):
 *   Lattice:   TOP > { INT(k), FLOAT(k), BOOL(k) } > BOT
 *   State:     per-value cell + per-block reachable flag
 *   Worklists: CFG edges + SSA value IDs
 *   Init:      entry block reachable; every value = TOP
 *   Iterate:
 *     - CFG edge: mark target reachable, evaluate all values in target
 *     - SSA use:  recompute cell, push uses if lattice moves down
 *   Rewrite:
 *     - CONST cells -> rewrite defining value to XI_CONST
 *     - Unreachable blocks -> remove from function
 *     - IF block with constant condition -> convert to PLAIN
 */

#include "xi_opt_sccp.h"
#include "xi_analysis.h"
#include "xi_cfg_edit.h"
#include "xi_pass.h"
#include "xi_range.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include <string.h>
#include <stdio.h>

/* ========== Lattice Cell ========== */

typedef enum {
    SCCP_TOP = 0, /* unseen (default after calloc) */
    SCCP_CONST_INT,
    SCCP_CONST_FLOAT,
    SCCP_CONST_BOOL,
    SCCP_RANGE_INT, /* bounded integer range [lo, hi] (not a single const) */
    SCCP_BOT,       /* overdefined */
} SccpKind;

typedef struct {
    SccpKind kind;
    int64_t ival;   /* payload for INT / BOOL; lo for RANGE_INT */
    double fval;    /* payload for FLOAT */
    int64_t hi_val; /* hi bound for RANGE_INT */
} SccpCell;

static SccpCell sccp_top(void) {
    return (SccpCell) {SCCP_TOP, 0, 0.0, 0};
}
static SccpCell sccp_bot(void) {
    return (SccpCell) {SCCP_BOT, 0, 0.0, 0};
}
static SccpCell sccp_int(int64_t v) {
    return (SccpCell) {SCCP_CONST_INT, v, 0.0, v};
}
static SccpCell sccp_float(double v) {
    return (SccpCell) {SCCP_CONST_FLOAT, 0, v, 0};
}
static SccpCell sccp_range(int64_t lo, int64_t hi) {
    XR_DCHECK(lo <= hi, "sccp_range: lo > hi");
    if (lo == hi)
        return sccp_int(lo);
    return (SccpCell) {SCCP_RANGE_INT, lo, 0.0, hi};
}
static SccpCell sccp_bool(bool v) {
    return (SccpCell) {SCCP_CONST_BOOL, v ? 1 : 0, 0.0, 0};
}

static bool sccp_eq(SccpCell a, SccpCell b) {
    if (a.kind != b.kind)
        return false;
    switch (a.kind) {
        case SCCP_CONST_INT:
            return a.ival == b.ival;
        case SCCP_CONST_FLOAT:
            return a.fval == b.fval;
        case SCCP_CONST_BOOL:
            return a.ival == b.ival;
        case SCCP_RANGE_INT:
            return a.ival == b.ival && a.hi_val == b.hi_val;
        default:
            return true;
    }
}

static SccpCell sccp_meet(SccpCell a, SccpCell b) {
    if (a.kind == SCCP_TOP)
        return b;
    if (b.kind == SCCP_TOP)
        return a;
    if (a.kind == SCCP_BOT || b.kind == SCCP_BOT)
        return sccp_bot();
    if (sccp_eq(a, b))
        return a;

    /* Range-aware meet: two different int-typed cells produce a range
     * instead of immediately going to BOT. */
    int64_t a_lo, a_hi, b_lo, b_hi;
    bool a_int = false, b_int = false;
    if (a.kind == SCCP_CONST_INT) {
        a_lo = a_hi = a.ival;
        a_int = true;
    } else if (a.kind == SCCP_RANGE_INT) {
        a_lo = a.ival;
        a_hi = a.hi_val;
        a_int = true;
    }
    if (b.kind == SCCP_CONST_INT) {
        b_lo = b_hi = b.ival;
        b_int = true;
    } else if (b.kind == SCCP_RANGE_INT) {
        b_lo = b.ival;
        b_hi = b.hi_val;
        b_int = true;
    }
    if (a_int && b_int) {
        int64_t lo = a_lo < b_lo ? a_lo : b_lo;
        int64_t hi = a_hi > b_hi ? a_hi : b_hi;
        return sccp_range(lo, hi);
    }

    return sccp_bot();
}

/* ========== Worklists ========== */

typedef struct {
    uint32_t from;
    uint32_t to;
} CfgEdge;

typedef struct {
    CfgEdge *buf;
    uint32_t len, cap;
} CfgWl;

typedef struct {
    uint32_t *buf;
    uint32_t len, cap;
} SsaWl;

static void cfg_push(CfgWl *w, uint32_t from, uint32_t to) {
    if (w->len >= w->cap) {
        w->cap = w->cap ? w->cap * 2 : 64;
        XR_REALLOC_OR_ABORT(w->buf, w->cap * sizeof(CfgEdge), "sccp cfg wl");
    }
    w->buf[w->len++] = (CfgEdge) {from, to};
}

static bool cfg_pop(CfgWl *w, CfgEdge *out) {
    if (!w->len)
        return false;
    *out = w->buf[--w->len];
    return true;
}

static void ssa_push(SsaWl *w, uint32_t v) {
    if (w->len >= w->cap) {
        w->cap = w->cap ? w->cap * 2 : 64;
        XR_REALLOC_OR_ABORT(w->buf, w->cap * sizeof(uint32_t), "sccp ssa wl");
    }
    w->buf[w->len++] = v;
}

static bool ssa_pop(SsaWl *w, uint32_t *out) {
    if (!w->len)
        return false;
    *out = w->buf[--w->len];
    return true;
}

/* ========== SCCP Context ========== */

typedef struct {
    XiFunc *func;
    SccpCell *cells; /* [next_value_id] */
    bool *reachable; /* [block_count] */
    bool *exec_edge; /* [block_count * 2]: [bi*2] = succs[0] edge, [bi*2+1] = succs[1] */
    uint32_t max_id; /* = func->next_value_id */
    uint32_t block_count;
    CfgWl cfg;
    SsaWl ssa;
} SccpCtx;

static bool block_index(const SccpCtx *ctx, const XiBlock *blk, uint32_t *out) {
    if (!ctx || !blk || !out)
        return false;
    if (blk->id < ctx->block_count && ctx->func->blocks[blk->id] == blk) {
        *out = blk->id;
        return true;
    }
    for (uint32_t i = 0; i < ctx->block_count; i++) {
        if (ctx->func->blocks[i] == blk) {
            *out = i;
            return true;
        }
    }
    return false;
}

/* ========== Value → Cell ========== */

/* Get the cell for an XiValue. Constants evaluate immediately;
 * other values use the cells[] array. */
static SccpCell value_cell(SccpCtx *ctx, const XiValue *v) {
    if (!v)
        return sccp_bot();
    if (v->op == XI_CONST) {
        if (!v->type)
            return sccp_bot();
        if (v->type->kind == XR_KIND_INT)
            return sccp_int(v->aux_int);
        if (v->type->kind == XR_KIND_FLOAT) {
            double d;
            memcpy(&d, &v->aux_int, sizeof(double));
            return sccp_float(d);
        }
        if (v->type->kind == XR_KIND_BOOL)
            return sccp_bool(v->aux_int != 0);
        return sccp_bot();
    }
    if (v->id < ctx->max_id)
        return ctx->cells[v->id];
    return sccp_bot();
}

/* ========== Evaluate Instructions ========== */

static bool both_int(SccpCell a, SccpCell b) {
    return a.kind == SCCP_CONST_INT && b.kind == SCCP_CONST_INT;
}

/* True if the cell carries integer range info (const or range). */
static bool has_int_bounds(SccpCell c) {
    return c.kind == SCCP_CONST_INT || c.kind == SCCP_RANGE_INT;
}

static void cell_bounds(SccpCell c, int64_t *lo, int64_t *hi) {
    if (c.kind == SCCP_CONST_INT) {
        *lo = *hi = c.ival;
    } else {
        *lo = c.ival;
        *hi = c.hi_val;
    }
}
static bool both_float(SccpCell a, SccpCell b) {
    return a.kind == SCCP_CONST_FLOAT && b.kind == SCCP_CONST_FLOAT;
}

/* Evaluate a unary op given the operand's lattice cell. */
static SccpCell eval_unary(uint16_t op, SccpCell a) {
    switch (op) {
        case XI_NEG:
            if (a.kind == SCCP_CONST_INT)
                return sccp_int(-a.ival);
            if (a.kind == SCCP_CONST_FLOAT)
                return sccp_float(-a.fval);
            return sccp_bot();
        case XI_BNOT:
            if (a.kind == SCCP_CONST_INT)
                return sccp_int(~a.ival);
            return sccp_bot();
        case XI_NOT:
            if (a.kind == SCCP_CONST_BOOL)
                return sccp_bool(a.ival == 0);
            return sccp_bot();
        case XI_ISNULL:
            /* A non-null constant is never null. */
            if (a.kind == SCCP_CONST_INT || a.kind == SCCP_CONST_FLOAT || a.kind == SCCP_CONST_BOOL)
                return sccp_bool(false);
            return sccp_bot();
        default:
            return sccp_bot();
    }
}

/* Integer / float arithmetic on two operand cells. */
static SccpCell eval_arith(uint16_t op, SccpCell a, SccpCell b) {
    switch (op) {
        case XI_ADD:
            if (both_int(a, b))
                return sccp_int((int64_t) ((uint64_t) a.ival + (uint64_t) b.ival));
            if (both_float(a, b))
                return sccp_float(a.fval + b.fval);
            return sccp_bot();
        case XI_SUB:
            if (both_int(a, b))
                return sccp_int((int64_t) ((uint64_t) a.ival - (uint64_t) b.ival));
            if (both_float(a, b))
                return sccp_float(a.fval - b.fval);
            return sccp_bot();
        case XI_MUL:
            if (both_int(a, b))
                return sccp_int((int64_t) ((uint64_t) a.ival * (uint64_t) b.ival));
            if (both_float(a, b))
                return sccp_float(a.fval * b.fval);
            return sccp_bot();
        case XI_DIV:
            if (both_int(a, b) && b.ival != 0)
                return sccp_int(a.ival / b.ival);
            if (both_float(a, b) && b.fval != 0.0)
                return sccp_float(a.fval / b.fval);
            return sccp_bot();
        case XI_MOD:
            if (both_int(a, b) && b.ival != 0)
                return sccp_int(a.ival % b.ival);
            return sccp_bot();
        default:
            return sccp_bot();
    }
}

/* Bitwise / shift operations on integer cells. */
static SccpCell eval_bitwise(uint16_t op, SccpCell a, SccpCell b) {
    if (!both_int(a, b))
        return sccp_bot();
    switch (op) {
        case XI_BAND:
            return sccp_int(a.ival & b.ival);
        case XI_BOR:
            return sccp_int(a.ival | b.ival);
        case XI_BXOR:
            return sccp_int(a.ival ^ b.ival);
        case XI_SHL:
            if (b.ival >= 0 && b.ival < 64)
                return sccp_int((int64_t) ((uint64_t) a.ival << b.ival));
            return sccp_bot();
        case XI_SHR:
            if (b.ival >= 0 && b.ival < 64)
                return sccp_int((int64_t) ((uint64_t) a.ival >> b.ival));
            return sccp_bot();
        default:
            return sccp_bot();
    }
}

/* Range-aware integer comparison.  Returns a constant SCCP_CONST_BOOL when
 * the ordering between [a_lo,a_hi] and [b_lo,b_hi] is provable, otherwise
 * the SCCP_BOT cell carried in `fallback`. */
static SccpCell range_compare(uint16_t op, SccpCell a, SccpCell b, SccpCell fallback) {
    if (!has_int_bounds(a) || !has_int_bounds(b))
        return fallback;
    int64_t alo, ahi, blo, bhi;
    cell_bounds(a, &alo, &ahi);
    cell_bounds(b, &blo, &bhi);
    switch (op) {
        case XI_LT:
            if (ahi < blo)
                return sccp_bool(true);
            if (alo >= bhi)
                return sccp_bool(false);
            break;
        case XI_LE:
            if (ahi <= blo)
                return sccp_bool(true);
            if (alo > bhi)
                return sccp_bool(false);
            break;
        case XI_GT:
            if (alo > bhi)
                return sccp_bool(true);
            if (ahi <= blo)
                return sccp_bool(false);
            break;
        case XI_GE:
            if (alo >= bhi)
                return sccp_bool(true);
            if (ahi < blo)
                return sccp_bool(false);
            break;
        case XI_EQ:
            if (ahi < blo || bhi < alo)
                return sccp_bool(false);
            break;
        case XI_NE:
            if (ahi < blo || bhi < alo)
                return sccp_bool(true);
            break;
    }
    return fallback;
}

/* Comparison ops produce a boolean cell, with range-aware fallback. */
static SccpCell eval_compare(uint16_t op, SccpCell a, SccpCell b) {
    switch (op) {
        case XI_LT:
            if (both_int(a, b))
                return sccp_bool(a.ival < b.ival);
            if (both_float(a, b))
                return sccp_bool(a.fval < b.fval);
            return range_compare(op, a, b, sccp_bot());
        case XI_LE:
            if (both_int(a, b))
                return sccp_bool(a.ival <= b.ival);
            if (both_float(a, b))
                return sccp_bool(a.fval <= b.fval);
            return range_compare(op, a, b, sccp_bot());
        case XI_GT:
            if (both_int(a, b))
                return sccp_bool(a.ival > b.ival);
            if (both_float(a, b))
                return sccp_bool(a.fval > b.fval);
            return range_compare(op, a, b, sccp_bot());
        case XI_GE:
            if (both_int(a, b))
                return sccp_bool(a.ival >= b.ival);
            if (both_float(a, b))
                return sccp_bool(a.fval >= b.fval);
            return range_compare(op, a, b, sccp_bot());
        case XI_EQ:
            if (both_int(a, b))
                return sccp_bool(a.ival == b.ival);
            if (both_float(a, b))
                return sccp_bool(a.fval == b.fval);
            if (a.kind == SCCP_CONST_BOOL && b.kind == SCCP_CONST_BOOL)
                return sccp_bool(a.ival == b.ival);
            return range_compare(op, a, b, sccp_bot());
        case XI_NE:
            if (both_int(a, b))
                return sccp_bool(a.ival != b.ival);
            if (both_float(a, b))
                return sccp_bool(a.fval != b.fval);
            if (a.kind == SCCP_CONST_BOOL && b.kind == SCCP_CONST_BOOL)
                return sccp_bool(a.ival != b.ival);
            return range_compare(op, a, b, sccp_bot());
        default:
            return sccp_bot();
    }
}

/* True for ops dispatched to eval_compare (produce a boolean). */
static bool is_compare_op(uint16_t op) {
    return op == XI_LT || op == XI_LE || op == XI_GT || op == XI_GE || op == XI_EQ || op == XI_NE;
}

/* True for ops dispatched to eval_bitwise. */
static bool is_bitwise_op(uint16_t op) {
    return op == XI_BAND || op == XI_BOR || op == XI_BXOR || op == XI_SHL || op == XI_SHR;
}

/* Evaluate a single XiValue under current cell state. */
static SccpCell eval_value(SccpCtx *ctx, const XiValue *v) {
    if (!v)
        return sccp_bot();

    /* Side-effecting ops always produce BOT */
    if (v->flags & XI_FLAG_SIDE_EFFECT)
        return sccp_bot();
    if (v->op == XI_CONST)
        return value_cell(ctx, v);
    if (v->op == XI_COPY && v->nargs >= 1)
        return value_cell(ctx, v->args[0]);
    if (v->op == XI_PARAM)
        return sccp_bot();

    if (v->nargs == 1) {
        SccpCell a = value_cell(ctx, v->args[0]);
        if (a.kind == SCCP_TOP)
            return sccp_top();
        return eval_unary(v->op, a);
    }

    if (v->nargs == 2) {
        SccpCell a = value_cell(ctx, v->args[0]);
        SccpCell b = value_cell(ctx, v->args[1]);
        /* Optimistic: TOP propagates so a not-yet-evaluated branch doesn't
         * pollute the lattice. */
        if (a.kind == SCCP_TOP || b.kind == SCCP_TOP)
            return sccp_top();
        if (is_compare_op(v->op))
            return eval_compare(v->op, a, b);
        if (is_bitwise_op(v->op))
            return eval_bitwise(v->op, a, b);
        return eval_arith(v->op, a, b);
    }

    /* Unmodeled ops default to BOT */
    return sccp_bot();
}

/* Evaluate a phi node: meet over args whose incoming edge is executable. */
static SccpCell eval_phi(SccpCtx *ctx, const XiBlock *blk, const XiPhi *phi) {
    SccpCell acc = sccp_top();
    for (uint16_t i = 0; i < phi->value.nargs; i++) {
        if (i >= blk->npreds)
            break;
        XiBlock *pred = blk->preds[i];
        if (!pred)
            continue;
        uint32_t pred_idx = 0;
        if (!block_index(ctx, pred, &pred_idx))
            continue;

        /* Check if the edge from pred to blk is executable */
        bool edge_exec = false;
        if (pred->succs[0] == blk && ctx->exec_edge[pred_idx * 2])
            edge_exec = true;
        if (pred->succs[1] == blk && ctx->exec_edge[pred_idx * 2 + 1])
            edge_exec = true;
        if (!edge_exec)
            continue;

        acc = sccp_meet(acc, value_cell(ctx, phi->value.args[i]));
        if (acc.kind == SCCP_BOT)
            break;
    }
    return acc;
}

/* ========== Edge Management ========== */

static void mark_edge(SccpCtx *ctx, uint32_t from_bi, uint32_t to_bi, int slot) {
    if (!ctx || from_bi >= ctx->block_count || to_bi >= ctx->block_count || slot < 0 || slot >= 2)
        return;
    uint32_t eidx = from_bi * 2 + slot;
    if (ctx->exec_edge[eidx])
        return;
    ctx->exec_edge[eidx] = true;
    if (!ctx->reachable[to_bi])
        ctx->reachable[to_bi] = true;
    cfg_push(&ctx->cfg, from_bi, to_bi);
}

/* ========== Use Propagation ========== */

/* Enqueue all uses of value |vid| for re-evaluation. */
static void enqueue_uses(SccpCtx *ctx, uint32_t vid) {
    XiFunc *f = ctx->func;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        if (!ctx->reachable[bi])
            continue;
        XiBlock *blk = f->blocks[bi];

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] && v->args[a]->id == vid) {
                    ssa_push(&ctx->ssa, v->id);
                    break;
                }
            }
        }

        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] && phi->value.args[a]->id == vid) {
                    ssa_push(&ctx->ssa, phi->value.id);
                    break;
                }
            }
        }
    }
}

/* ========== Terminator Evaluation ========== */

static void visit_terminator(SccpCtx *ctx, XiBlock *blk) {
    uint32_t bi = 0;
    if (!block_index(ctx, blk, &bi))
        return;

    if (blk->kind == XI_BLOCK_PLAIN) {
        uint32_t succ_idx = 0;
        if (blk->succs[0] && block_index(ctx, blk->succs[0], &succ_idx))
            mark_edge(ctx, bi, succ_idx, 0);
    } else if (blk->kind == XI_BLOCK_IF) {
        SccpCell c = value_cell(ctx, blk->control);
        if (c.kind == SCCP_CONST_BOOL || c.kind == SCCP_CONST_INT) {
            if (c.ival != 0) {
                uint32_t succ_idx = 0;
                if (blk->succs[0] && block_index(ctx, blk->succs[0], &succ_idx))
                    mark_edge(ctx, bi, succ_idx, 0);
            } else {
                uint32_t succ_idx = 0;
                if (blk->succs[1] && block_index(ctx, blk->succs[1], &succ_idx))
                    mark_edge(ctx, bi, succ_idx, 1);
            }
        } else {
            /* Unknown: both edges may execute */
            uint32_t succ_idx = 0;
            if (blk->succs[0] && block_index(ctx, blk->succs[0], &succ_idx))
                mark_edge(ctx, bi, succ_idx, 0);
            if (blk->succs[1] && block_index(ctx, blk->succs[1], &succ_idx))
                mark_edge(ctx, bi, succ_idx, 1);
        }
    }
    /* RETURN / UNREACHABLE: no outgoing edges */
}

/* ========== Find Value by ID ========== */

/* Find the XiValue* and its block given a value ID.  Used for rewriting. */
typedef struct {
    XiValue *v;
    XiBlock *blk;
    bool is_phi;
} ValLoc;

static ValLoc find_value(XiFunc *f, uint32_t vid) {
    ValLoc loc = {NULL, NULL, false};
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            if (blk->values[vi] && blk->values[vi]->id == vid)
                return (ValLoc) {blk->values[vi], blk, false};
        }
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            if (phi->value.id == vid)
                return (ValLoc) {&phi->value, blk, true};
        }
    }
    return loc;
}

/* ========== Revisit a Value ========== */

static void revisit_value(SccpCtx *ctx, uint32_t vid) {
    if (vid >= ctx->max_id)
        return;

    ValLoc loc = find_value(ctx->func, vid);
    if (!loc.v)
        return;
    uint32_t loc_bi = 0;
    if (!block_index(ctx, loc.blk, &loc_bi))
        return;
    if (!ctx->reachable[loc_bi])
        return;

    SccpCell old = ctx->cells[vid];
    SccpCell neu;

    if (loc.is_phi) {
        XiPhi *phi = (XiPhi *) ((char *) loc.v - offsetof(XiPhi, value));
        neu = eval_phi(ctx, loc.blk, phi);
    } else {
        neu = eval_value(ctx, loc.v);
    }

    if (!sccp_eq(old, neu)) {
        ctx->cells[vid] = neu;
        enqueue_uses(ctx, vid);
    }
}

/* ========== Rewriting ========== */

static bool rewrite_function(SccpCtx *ctx) {
    XiFunc *f = ctx->func;
    bool any = false;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!ctx->reachable[bi])
            continue;

        /* Rewrite values with constant cells to XI_CONST */
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->op == XI_CONST || v->op == XI_PARAM)
                continue;
            if (v->flags & XI_FLAG_SIDE_EFFECT)
                continue;

            SccpCell cell = ctx->cells[v->id];
            switch (cell.kind) {
                case SCCP_CONST_INT:
                    v->op = XI_CONST;
                    v->aux_int = cell.ival;
                    v->nargs = 0;
                    any = true;
                    break;
                case SCCP_CONST_FLOAT: {
                    v->op = XI_CONST;
                    double d = cell.fval;
                    memcpy(&v->aux_int, &d, sizeof(double));
                    v->nargs = 0;
                    any = true;
                    break;
                }
                case SCCP_CONST_BOOL:
                    v->op = XI_CONST;
                    v->aux_int = cell.ival;
                    v->nargs = 0;
                    any = true;
                    break;
                case SCCP_TOP:
                case SCCP_BOT:
                case SCCP_RANGE_INT:
                    break;
            }
        }

        /* Simplify IF blocks with constant condition to PLAIN */
        if (blk->kind == XI_BLOCK_IF && blk->control) {
            bool edge0 = ctx->exec_edge[bi * 2];
            bool edge1 = ctx->exec_edge[bi * 2 + 1];
            if (edge0 != edge1) {
                XiBlock *taken = edge0 ? blk->succs[0] : blk->succs[1];
                XiBlock *dropped = edge0 ? blk->succs[1] : blk->succs[0];
                if (dropped) {
                    while (xi_cfg_remove_pred(dropped, blk)) {
                    }
                }
                blk->kind = XI_BLOCK_PLAIN;
                blk->succs[0] = taken;
                blk->succs[1] = NULL;
                blk->control = NULL;
                any = true;
                continue;
            }

            SccpCell c = value_cell(ctx, blk->control);
            if (c.kind == SCCP_CONST_BOOL || c.kind == SCCP_CONST_INT) {
                XiBlock *taken = (c.ival != 0) ? blk->succs[0] : blk->succs[1];
                XiBlock *dropped = (c.ival != 0) ? blk->succs[1] : blk->succs[0];
                if (dropped) {
                    while (xi_cfg_remove_pred(dropped, blk)) {
                    }
                }
                blk->kind = XI_BLOCK_PLAIN;
                blk->succs[0] = taken;
                blk->succs[1] = NULL;
                blk->control = NULL;
                any = true;
            }
        }
    }
    return any;
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_sccp(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_sccp: NULL func");
    if (f->nblocks == 0)
        return xi_pass_no_change();

    bool compacted_at_entry = false;
    for (uint32_t i = 0; i < f->nblocks; i++) {
        XiBlock *blk = f->blocks[i];
        if (!blk)
            continue;
        if (blk->id != i || (blk->kind == XI_BLOCK_UNREACHABLE && blk != f->entry)) {
            uint32_t removed = xi_cfg_compact_blocks(f);
            xi_cfg_invalidate(f);
            compacted_at_entry = removed > 0;
            break;
        }
    }

    uint32_t n = f->nblocks;
    uint32_t max_id = f->next_value_id;

    SccpCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.func = f;
    ctx.max_id = max_id;
    ctx.block_count = n;
    ctx.cells = (SccpCell *) xr_calloc(max_id > 0 ? max_id : 1, sizeof(SccpCell));
    ctx.reachable = (bool *) xr_calloc(n, sizeof(bool));
    ctx.exec_edge = (bool *) xr_calloc((size_t) n * 2, sizeof(bool));
    if (!ctx.cells || !ctx.reachable || !ctx.exec_edge) {
        xr_free(ctx.cells);
        xr_free(ctx.reachable);
        xr_free(ctx.exec_edge);
        return xi_pass_no_change();
    }

    /* Seed: entry block is reachable */
    ctx.reachable[0] = true;
    cfg_push(&ctx.cfg, 0, 0);

    /* Main loop: drain both worklists until convergence */
    while (ctx.cfg.len > 0 || ctx.ssa.len > 0) {
        CfgEdge ce;
        if (cfg_pop(&ctx.cfg, &ce)) {
            XiBlock *blk = f->blocks[ce.to];
            if (!blk)
                continue;

            /* Evaluate all phis in this block */
            for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
                uint32_t dv = phi->value.id;
                if (dv >= max_id)
                    continue;
                SccpCell neu = eval_phi(&ctx, blk, phi);
                if (!sccp_eq(ctx.cells[dv], neu)) {
                    ctx.cells[dv] = neu;
                    enqueue_uses(&ctx, dv);
                }
            }

            /* Evaluate all values in this block */
            for (uint32_t i = 0; i < blk->nvalues; i++) {
                XiValue *v = blk->values[i];
                if (!v)
                    continue;
                uint32_t dv = v->id;
                if (dv >= max_id)
                    continue;
                SccpCell neu = eval_value(&ctx, v);
                if (!sccp_eq(ctx.cells[dv], neu)) {
                    ctx.cells[dv] = neu;
                    enqueue_uses(&ctx, dv);
                }
            }

            visit_terminator(&ctx, blk);
            continue;
        }

        uint32_t vid;
        if (ssa_pop(&ctx.ssa, &vid)) {
            revisit_value(&ctx, vid);

            /* Re-visit terminators that read this value */
            for (uint32_t bi = 0; bi < n; bi++) {
                if (!ctx.reachable[bi])
                    continue;
                XiBlock *blk = f->blocks[bi];
                if (blk->kind == XI_BLOCK_IF && blk->control && blk->control->id == vid) {
                    visit_terminator(&ctx, blk);
                }
            }
        }
    }

    /* Rewrite phase */
    bool any_rewrite = rewrite_function(&ctx);

    /* Remove unreachable blocks */
    for (uint32_t i = 0; i < n; i++) {
        if (ctx.reachable[i])
            continue;
        XiBlock *dead = f->blocks[i];
        if (!dead || dead == f->entry)
            continue;
        for (uint16_t s = 0; s < 2; s++) {
            XiBlock *succ = dead->succs[s];
            if (!succ)
                continue;
            while (xi_cfg_remove_pred(succ, dead)) {
            }
        }
        dead->kind = XI_BLOCK_UNREACHABLE;
        dead->control = NULL;
        dead->succs[0] = NULL;
        dead->succs[1] = NULL;
    }
    uint32_t removed = xi_cfg_compact_blocks(f);
    bool blocks_removed = removed > 0;
    if (blocks_removed)
        xi_cfg_invalidate(f);

    /* Cleanup */
    xr_free(ctx.cells);
    xr_free(ctx.reachable);
    xr_free(ctx.exec_edge);
    xr_free(ctx.cfg.buf);
    xr_free(ctx.ssa.buf);

    XiPassChange chg = xi_pass_no_change();
    if (any_rewrite) {
        chg.values_changed = true;
    }
    if (blocks_removed || compacted_at_entry) {
        chg.cfg_changed = true;
    }
    return chg;
}
