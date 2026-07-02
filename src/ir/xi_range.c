/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_range.c - Integer value range analysis implementation
 *
 * Propagates signed [lo, hi] ranges through the SSA graph using a
 * forward dataflow worklist.  Constants yield exact ranges; phi nodes
 * use union (join); branch conditions narrow ranges on true/false edges.
 * Arithmetic transfer functions model overflow conservatively (widen to
 * TOP on potential overflow).
 *
 * Results are stored in a function-level side-table indexed by value id.
 * The xi_range_of() query reads from this table.
 */

#include "xi_range.h"
#include "xi_analysis.h"
#include "xi_loop.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include <string.h>

/* ========== Lattice Operations ========== */

XR_FUNC XiRange xi_range_union(XiRange a, XiRange b) {
    if (a.is_bot)
        return b;
    if (b.is_bot)
        return a;
    if (a.is_top || b.is_top)
        return xi_range_top();

    int64_t lo = a.lo < b.lo ? a.lo : b.lo;
    int64_t hi = a.hi > b.hi ? a.hi : b.hi;
    return xi_range_make(lo, hi);
}

XR_FUNC XiRange xi_range_intersect(XiRange a, XiRange b) {
    if (a.is_bot || b.is_bot)
        return xi_range_bot();
    if (a.is_top)
        return b;
    if (b.is_top)
        return a;

    int64_t lo = a.lo > b.lo ? a.lo : b.lo;
    int64_t hi = a.hi < b.hi ? a.hi : b.hi;
    if (lo > hi)
        return xi_range_bot();
    return xi_range_make(lo, hi);
}

/* ========== Queries ========== */

XR_FUNC bool xi_range_known_nonneg(XiRange r) {
    return !r.is_top && !r.is_bot && r.lo >= 0;
}

XR_FUNC bool xi_range_known_positive(XiRange r) {
    return !r.is_top && !r.is_bot && r.lo > 0;
}

XR_FUNC bool xi_range_known_less_than(XiRange r, int64_t k) {
    return !r.is_top && !r.is_bot && r.hi < k;
}

XR_FUNC bool xi_range_known_ge(XiRange r, int64_t k) {
    return !r.is_top && !r.is_bot && r.lo >= k;
}

XR_FUNC bool xi_range_contains(XiRange outer, XiRange inner) {
    if (inner.is_bot)
        return true;
    if (outer.is_top)
        return true;
    if (inner.is_top || outer.is_bot)
        return false;
    return outer.lo <= inner.lo && inner.hi <= outer.hi;
}

/* ========== Arithmetic Transfer Functions ========== */

static bool xi_range_for_native_int_width(uint8_t native_width, XiRange *out) {
    if (!out)
        return false;
    switch (native_width) {
        case XR_NATIVE_I8:
            *out = xi_range_make(-128, 127);
            return true;
        case XR_NATIVE_U8:
            *out = xi_range_make(0, 255);
            return true;
        case XR_NATIVE_I16:
            *out = xi_range_make(-32768, 32767);
            return true;
        case XR_NATIVE_U16:
            *out = xi_range_make(0, 65535);
            return true;
        case XR_NATIVE_I32:
            *out = xi_range_make(INT32_MIN, INT32_MAX);
            return true;
        case XR_NATIVE_U32:
            *out = xi_range_make(0, UINT32_MAX);
            return true;
        default:
            return false;
    }
}

static XiRange xi_range_bound_by_type(const XrType *type, XiRange range) {
    XiRange type_range = xi_range_top();
    if (!type || type->kind != XR_KIND_INT ||
        !xi_range_for_native_int_width(type->native_width, &type_range))
        return range;
    return xi_range_intersect(range, type_range);
}

/* Overflow-safe addition check. */
static bool add_overflows(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return true;
    if (b < 0 && a < INT64_MIN - b)
        return true;
    return false;
}

/* Overflow-safe subtraction check. */
static bool sub_overflows(int64_t a, int64_t b) {
    if (b < 0 && a > INT64_MAX + b)
        return true;
    if (b > 0 && a < INT64_MIN + b)
        return true;
    return false;
}

/* Overflow-safe multiplication check (conservative). */
static bool mul_overflows(int64_t a, int64_t b) {
    if (a == 0 || b == 0)
        return false;
    if (a > 0 && b > 0 && a > INT64_MAX / b)
        return true;
    if (a < 0 && b < 0 && a < INT64_MAX / b)
        return true;
    if (a > 0 && b < 0 && b < INT64_MIN / a)
        return true;
    if (a < 0 && b > 0 && a < INT64_MIN / b)
        return true;
    return false;
}

XR_FUNC XiRange xi_range_add(XiRange a, XiRange b) {
    if (a.is_top || b.is_top)
        return xi_range_top();
    if (a.is_bot || b.is_bot)
        return xi_range_bot();

    if (add_overflows(a.lo, b.lo) || add_overflows(a.hi, b.hi))
        return xi_range_top();

    return xi_range_make(a.lo + b.lo, a.hi + b.hi);
}

XR_FUNC XiRange xi_range_sub(XiRange a, XiRange b) {
    if (a.is_top || b.is_top)
        return xi_range_top();
    if (a.is_bot || b.is_bot)
        return xi_range_bot();

    /* a - b: lo = a.lo - b.hi, hi = a.hi - b.lo */
    if (sub_overflows(a.lo, b.hi) || sub_overflows(a.hi, b.lo))
        return xi_range_top();

    return xi_range_make(a.lo - b.hi, a.hi - b.lo);
}

XR_FUNC XiRange xi_range_mul(XiRange a, XiRange b) {
    if (a.is_top || b.is_top)
        return xi_range_top();
    if (a.is_bot || b.is_bot)
        return xi_range_bot();

    /* For multiplication, compute all 4 corner products. */
    int64_t corners[4];
    if (mul_overflows(a.lo, b.lo) || mul_overflows(a.lo, b.hi) || mul_overflows(a.hi, b.lo) ||
        mul_overflows(a.hi, b.hi)) {
        return xi_range_top();
    }

    corners[0] = a.lo * b.lo;
    corners[1] = a.lo * b.hi;
    corners[2] = a.hi * b.lo;
    corners[3] = a.hi * b.hi;

    int64_t lo = corners[0], hi = corners[0];
    for (int i = 1; i < 4; i++) {
        if (corners[i] < lo)
            lo = corners[i];
        if (corners[i] > hi)
            hi = corners[i];
    }
    return xi_range_make(lo, hi);
}

XR_FUNC XiRange xi_range_neg(XiRange a) {
    if (a.is_top)
        return xi_range_top();
    if (a.is_bot)
        return xi_range_bot();

    /* Negating INT64_MIN overflows. */
    if (a.lo == INT64_MIN || a.hi == INT64_MIN)
        return xi_range_top();

    return xi_range_make(-a.hi, -a.lo);
}

/* ========== Per-Function Range Table ========== */

/* Side-table stored in XiFunc.range_table (allocated by analysis pass). */
typedef struct {
    XiRange *ranges; /* indexed by value id */
    uint32_t cap;
} RangeTable;

static RangeTable *get_range_table(const XiFunc *f) {
    return (RangeTable *) f->analysis_data[0];
}

/* ========== IR Query ========== */

XR_FUNC XiRange xi_range_of(const XiValue *v) {
    XR_DCHECK(v != NULL, "xi_range_of: NULL value");

    /* Only integer values have meaningful ranges. */
    if (!v->type || v->type->kind != XR_KIND_INT)
        return xi_range_top();

    /* Constants always have exact range. */
    if (v->op == XI_CONST)
        return xi_range_const(v->aux_int);

    /* Look up in the function's range table. */
    if (!v->block || !v->block->func)
        return xi_range_top();

    const XiFunc *f = v->block->func;
    RangeTable *rt = get_range_table(f);
    if (!rt || v->id >= rt->cap)
        return xi_range_top();

    return rt->ranges[v->id];
}

/* ========== Analysis Pass ========== */

static bool is_int_value(const XiValue *v) {
    return v && v->type && v->type->kind == XR_KIND_INT;
}

static XiRange eval_counted_iv_bound(const XiValue *phi, const RangeTable *rt,
                                     const XiLoopInfo *li);

/* Evaluate the range of a value from its operands' ranges. */
static XiRange eval_range(const XiValue *v, const RangeTable *rt, const XiLoopInfo *li) {
    XR_DCHECK(v != NULL, "eval_range: NULL value");

    if (v->op == XI_CONST && is_int_value(v))
        return xi_range_bound_by_type(v->type, xi_range_const(v->aux_int));

    if (v->op == XI_PARAM)
        return xi_range_bound_by_type(v->type, xi_range_top());

    if (!is_int_value(v))
        return xi_range_top();

    /* Get operand ranges. */
    XiRange r0 = xi_range_top(), r1 = xi_range_top();
    if (v->nargs >= 1 && v->args[0] && v->args[0]->id < rt->cap)
        r0 = rt->ranges[v->args[0]->id];
    if (v->nargs >= 2 && v->args[1] && v->args[1]->id < rt->cap)
        r1 = rt->ranges[v->args[1]->id];

    switch (v->op) {
        case XI_ADD:
            return xi_range_add(r0, r1);
        case XI_SUB:
            return xi_range_sub(r0, r1);
        case XI_MUL:
            return xi_range_mul(r0, r1);
        case XI_NEG:
            return xi_range_neg(r0);
        case XI_COPY:
            return r0;
        case XI_CONVERT:
            return xi_range_bound_by_type(v->type, r0);
        case XI_PHI: {
            /* Union of all phi inputs, tightened by the counted-loop IV
             * bound when the phi is a canonical induction variable. */
            XiRange result = xi_range_bot();
            for (uint16_t i = 0; i < v->nargs; i++) {
                if (v->args[i] && v->args[i]->id < rt->cap)
                    result = xi_range_union(result, rt->ranges[v->args[i]->id]);
                else {
                    result = xi_range_top();
                    break;
                }
            }
            return xi_range_intersect(result, eval_counted_iv_bound(v, rt, li));
        }
        case XI_NARROW_I8:
            return xi_range_intersect(r0, xi_range_make(-128, 127));
        case XI_NARROW_U8:
            return xi_range_intersect(r0, xi_range_make(0, 255));
        case XI_NARROW_I16:
            return xi_range_intersect(r0, xi_range_make(-32768, 32767));
        case XI_NARROW_U16:
            return xi_range_intersect(r0, xi_range_make(0, 65535));
        case XI_NARROW_I32:
            return xi_range_intersect(r0, xi_range_make(INT32_MIN, INT32_MAX));
        case XI_NARROW_U32:
            return xi_range_intersect(r0, xi_range_make(0, UINT32_MAX));
        case XI_WIDEN_I8:
            return xi_range_make(-128, 127);
        case XI_WIDEN_U8:
            return xi_range_make(0, 255);
        case XI_WIDEN_I16:
            return xi_range_make(-32768, 32767);
        case XI_WIDEN_U16:
            return xi_range_make(0, 65535);
        case XI_WIDEN_I32:
            return xi_range_make(INT32_MIN, INT32_MAX);
        case XI_WIDEN_U32:
            return xi_range_make(0, UINT32_MAX);
        default:
            return xi_range_bound_by_type(v->type, xi_range_top());
    }
}

/* ========== Counted-Loop Induction Variable Bound ==========
 *
 * A loop-carried phi normally degrades to TOP (its latch input grows every
 * iteration and the plain union never converges below TOP).  For the
 * canonical counted form
 *
 *     header(IF): if (phi < limit) -> body ... latch: next = phi + step
 *     phi = [init from preheader, next from latch],  step const > 0
 *
 * the phi value is bounded: any path that reaches the latch leaves the
 * header through the true edge, so the value being incremented satisfied
 * `phi < limit`, and the first value that fails the test is at most
 * limit + step - 1.  When the loop runs zero times the phi only ever
 * holds init, giving
 *
 *     [init.lo,  max(init.hi, limit.hi + step - 1)]
 *
 * computed only when both init and limit have finite ranges.  Soundness
 * needs the loop shape verified structurally: the phi block must be a
 * natural-loop header, the increment must flow in over a back edge from
 * inside the loop, and the false edge must leave the loop (otherwise a
 * `while (true) { if (i < n) ... ; i += 1 }` body would re-enter the
 * latch without passing the test).  The bound is intersected with the
 * plain phi union inside eval_range, which keeps the fixed-point
 * iteration monotone and convergent. */
static const XiValue *unwrap_copy(const XiValue *v) {
    while (v && (xi_copy_is_identity_alias(v) || v->op == XI_MOVE) && v->nargs >= 1)
        v = v->args[0];
    return v;
}

static XiRange eval_counted_iv_bound(const XiValue *phi, const RangeTable *rt,
                                     const XiLoopInfo *li) {
    const XiBlock *header = phi->block;
    if (!li || !header || header->kind != XI_BLOCK_IF || phi->nargs != 2 || header->npreds != 2)
        return xi_range_top();
    if (header->id >= li->nblocks)
        return xi_range_top();
    const XiLoop *loop = li->block_to_loop[header->id];
    if (!loop || loop->header != header)
        return xi_range_top();
    /* The false edge must exit the loop: otherwise the increment can be
     * reached without passing the `phi < limit` test. */
    if (header->succs[1] && xi_loop_contains_block(loop, header->succs[1]))
        return xi_range_top();

    /* Header condition must be `phi < limit` on the phi itself. */
    const XiValue *cond = unwrap_copy(header->control);
    if (!cond || cond->op != XI_LT || cond->nargs < 2)
        return xi_range_top();
    if (unwrap_copy(cond->args[0]) != phi)
        return xi_range_top();
    const XiValue *limit = cond->args[1];

    /* One incoming value must be `phi + const step` (step > 0) flowing in
     * from inside the loop; the other is the init value. */
    const XiValue *init = NULL;
    int64_t step = 0;
    for (uint16_t i = 0; i < 2; i++) {
        const XiValue *arg = unwrap_copy(phi->args[i]);
        if (!arg)
            return xi_range_top();
        if (arg->op == XI_ADD && arg->nargs >= 2) {
            const XiValue *a0 = unwrap_copy(arg->args[0]);
            const XiValue *a1 = unwrap_copy(arg->args[1]);
            const XiValue *step_v = NULL;
            if (a0 == phi)
                step_v = a1;
            else if (a1 == phi)
                step_v = a0;
            if (step_v && step_v->op == XI_CONST && is_int_value(step_v) && step_v->aux_int > 0 &&
                header->preds[i] && xi_loop_contains_block(loop, header->preds[i])) {
                if (step != 0)
                    return xi_range_top(); /* both inputs are increments */
                step = step_v->aux_int;
                continue;
            }
        }
        if (init)
            return xi_range_top();
        init = arg;
    }
    if (!init || step <= 0)
        return xi_range_top();

    XiRange init_r = (init->op == XI_CONST && is_int_value(init)) ? xi_range_const(init->aux_int)
                     : (init->id < rt->cap)                       ? rt->ranges[init->id]
                                                                  : xi_range_top();
    limit = unwrap_copy(limit);
    XiRange limit_r = (limit && limit->op == XI_CONST && is_int_value(limit))
                          ? xi_range_const(limit->aux_int)
                      : (limit && limit->id < rt->cap) ? rt->ranges[limit->id]
                                                       : xi_range_top();
    if (init_r.is_top || init_r.is_bot || limit_r.is_top || limit_r.is_bot)
        return xi_range_top();
    if (add_overflows(limit_r.hi, step - 1))
        return xi_range_top();

    int64_t hi = limit_r.hi + step - 1;
    if (init_r.hi > hi)
        hi = init_r.hi;
    if (init_r.lo > hi)
        return xi_range_top();
    return xi_range_make(init_r.lo, hi);
}

/* Compare two ranges for equality (used for fixed-point). */
static bool range_eq(XiRange a, XiRange b) {
    if (a.is_top && b.is_top)
        return true;
    if (a.is_bot && b.is_bot)
        return true;
    if (a.is_top != b.is_top || a.is_bot != b.is_bot)
        return false;
    return a.lo == b.lo && a.hi == b.hi;
}

XR_FUNC XiPassChange xi_range_analyze(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_range_analyze: NULL func");

    /* Count values for table sizing. */
    uint32_t max_id = 0;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (v && v->id >= max_id)
                max_id = v->id + 1;
        }
    }

    /* Allocate range table. */
    RangeTable *rt = (RangeTable *) xr_malloc(sizeof(RangeTable));
    if (!rt)
        return xi_pass_no_change();

    rt->cap = max_id;
    rt->ranges = (XiRange *) xr_malloc(max_id * sizeof(XiRange));
    if (!rt->ranges) {
        xr_free(rt);
        return xi_pass_no_change();
    }

    /* Initialize all ranges to TOP. */
    for (uint32_t i = 0; i < max_id; i++)
        rt->ranges[i] = xi_range_top();

    /* Loop forest for counted-IV phi bounds (NULL when no loops). */
    xi_ensure_rpo(f);
    xi_ensure_dominators(f);
    const XiLoopInfo *li = xi_ensure_loops(f);

    /* Seed constants with exact ranges. */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (v->op == XI_CONST && is_int_value(v))
                rt->ranges[v->id] = xi_range_const(v->aux_int);
        }
    }

    /* Fixed-point iteration (worklist-free for simplicity;
     * iterate RPO until no change, max 16 rounds). */
    for (int round = 0; round < 16; round++) {
        bool changed = false;

        for (uint32_t bi = 0; bi < f->nblocks; bi++) {
            XiBlock *blk = f->blocks[bi];
            if (!blk)
                continue;
            for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
                XiValue *v = blk->values[vi];
                if (!v || v->op == XI_CONST || v->op == XI_PARAM)
                    continue;
                if (!is_int_value(v))
                    continue;

                XiRange new_r = eval_range(v, rt, li);
                if (!range_eq(new_r, rt->ranges[v->id])) {
                    rt->ranges[v->id] = new_r;
                    changed = true;
                }
            }
        }

        if (!changed)
            break;
    }

    /* Free previous range table if re-running. */
    RangeTable *old = get_range_table(f);
    if (old) {
        xr_free(old->ranges);
        xr_free(old);
    }

    /* Store in function's analysis slot. */
    f->analysis_data[0] = rt;
    f->invariant_mask |= XI_INV_RANGE_ANNOTATED;

    XiPassChange chg = xi_pass_no_change();
    chg.values_changed = true;
    return chg;
}
