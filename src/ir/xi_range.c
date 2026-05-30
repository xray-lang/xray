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

/* Evaluate the range of a value from its operands' ranges. */
static XiRange eval_range(const XiValue *v, const RangeTable *rt) {
    XR_DCHECK(v != NULL, "eval_range: NULL value");

    if (v->op == XI_CONST && is_int_value(v))
        return xi_range_const(v->aux_int);

    if (v->op == XI_PARAM)
        return xi_range_top(); /* No info for params (yet). */

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
        case XI_PHI: {
            /* Union of all phi inputs. */
            XiRange result = xi_range_bot();
            for (uint16_t i = 0; i < v->nargs; i++) {
                if (v->args[i] && v->args[i]->id < rt->cap)
                    result = xi_range_union(result, rt->ranges[v->args[i]->id]);
                else
                    return xi_range_top();
            }
            return result;
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
        default:
            return xi_range_top();
    }
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

                XiRange new_r = eval_range(v, rt);
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
