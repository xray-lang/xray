/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_regalloc.c - Linear scan register allocation with Active/Inactive state machine
 *
 * KEY CONCEPT:
 *   Position-ordered linear scan with Active/Inactive lists,
 *   fixed intervals for CALL clobbers, LiveRange bundles for Phi
 *   coalescing, and spill-then-reload for minimal spill ranges.
 *
 * ALGORITHM:
 *   1. Number instructions (2 positions per ins: use + def)
 *   2. Build live ranges with UseInterval chains (backward scan)
 *   3. Build fixed intervals for CALL clobbers (caller-saved regs)
 *   4. Compute bundles (Phi/MOV-connected non-overlapping ranges)
 *   5. Compute hints (bundles, params, MOV/Phi coalescing)
 *   6. Sort by start position ascending
 *   7. Linear scan with Active/Inactive state machine:
 *      - ForwardStateTo: maintain active/inactive lists
 *      - AllocFreeReg: scan active+inactive for conflicts
 *      - AllocBlockedReg: evict with fixed-range protection
 *      - SpillRange: spill-then-reload (split tail at next use)
 *   8. Output: per-segment mapping, gap moves, edge copies
 */

#include "xir_regalloc.h"
#include "xir_target.h"
#include "xir_looptree.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../coro/xcoroutine.h"  // XIR_SUSPEND_SPILL_MAX (suspend-bridge capacity)
#include <stdlib.h>
#include <string.h>

/* ========== Pool Allocator (chunk-based, stable pointers) ========== */

#define LS_CHUNK_SIZE 65536

typedef struct LsChunk {
    struct LsChunk *next;
    uint32_t used;
    uint32_t _pad;  // align data[] to 8 bytes (next=8 + used=4 + _pad=4 = 16)
    _Alignas(8) uint8_t data[LS_CHUNK_SIZE];
} LsChunk;

typedef struct {
    LsChunk *cur;
} LsPool;

static void pool_init(LsPool *p) {
    p->cur = xr_calloc(1, sizeof(LsChunk));
}

static void pool_free(LsPool *p) {
    for (LsChunk *c = p->cur; c;) {
        LsChunk *n = c->next;
        xr_free(c);
        c = n;
    }
    p->cur = NULL;
}

static void *pool_alloc(LsPool *p, uint32_t sz) {
    sz = (sz + 7u) & ~7u;
    if (p->cur->used + sz > LS_CHUNK_SIZE) {
        LsChunk *nc = xr_calloc(1, sizeof(LsChunk));
        nc->next = p->cur;
        p->cur = nc;
    }
    void *r = p->cur->data + p->cur->used;
    p->cur->used += sz;
    return r;
}

/* ========== Internal Data Structures ========== */

typedef struct LsInterval {
    int32_t start, end;
    struct LsInterval *next;
} LsInterval;

typedef struct LsUsePos {
    int32_t pos;
    struct LsUsePos *next;
} LsUsePos;

typedef struct LsRange {
    uint32_t vreg;
    uint8_t rep;
    bool is_fp;
    bool is_fixed;  // fixed interval (CALL clobber), never evicted
    int8_t assigned;
    int16_t spill;
    LsInterval *first_iv, *last_iv;
    LsUsePos *uses;
    int8_t hint;
    uint32_t bundle_id;  // union-find root vreg, or UINT32_MAX if none
    struct LsRange *next_sibling, *parent;
} LsRange;

// Dynamic list for Active/Inactive tracking
typedef struct {
    LsRange **items;
    uint32_t len, cap;
} LsList;

typedef struct {
    XirFunc *func;
    XirLive live;
    int32_t max_pos;
    int32_t *blk_start, *blk_end;
    bool *blk_deferred;  // true for cold blocks (catch, deopt, throw)
    LsRange **vreg_ranges;
    uint32_t nvreg;
    LsRange **all_ranges;
    uint32_t nall, all_cap;
    int16_t next_spill;
    int32_t slot_end[XIR_MAX_SPILL_SLOTS];  // last-end position per slot
    bool slot_is_ptr[XIR_MAX_SPILL_SLOTS];  // true if slot last used by PTR vreg

    // Active/Inactive state machine
    LsList active;    // ranges assigned + covering current pos
    LsList inactive;  // ranges assigned + in a hole at current pos

    // LiveRange bundles (Phi/MOV-connected non-overlapping ranges)
    uint32_t *bundle_parent;  // union-find parent array [nvreg]
    int8_t *bundle_hint;      // per-bundle hint register [nvreg]
    int16_t *bundle_spill;    // per-bundle spill slot [nvreg]

    // Block ID → internal index mapping for O(1) lookup
    uint32_t *blk_id_to_idx;   // [max_blk_id+1] block.id → index in blocks[]
    uint32_t blk_id_map_size;  // size of blk_id_to_idx array

    LsPool pool;
} LsCtx;

/* ========== Helpers ========== */

// O(1) block ID → internal index lookup
static uint32_t blk_idx(const LsCtx *ctx, uint32_t blk_id) {
    if (blk_id < ctx->blk_id_map_size)
        return ctx->blk_id_to_idx[blk_id];
    return 0;
}

static bool vfp(XirFunc *f, uint32_t v) {
    return v < f->nvreg && f->vregs[v].rep == XR_REP_F64;
}

static bool is_remat(XirFunc *f, uint32_t v) {
    if (v >= f->nvreg)
        return false;
    XirIns *d = f->vregs[v].def;
    if (!d)
        return false;
    /* Constants: single-instruction immediate load. */
    if (d->op == XIR_CONST_I64 || d->op == XIR_CONST_F64 || d->op == XIR_CONST_PTR)
        return true;
    /* LOAD_CORO_BYTE with a const offset: single LDRB from the
     * callee-saved jit_ctx register (x28), always available. */
    if (d->op == XIR_LOAD_CORO_BYTE && xir_ref_is_const(d->args[0]))
        return true;
    /* I2F / F2I with a const operand: 2-instruction sequence
     * (load const + SCVTF/FCVTZS).  Still cheaper than a spill
     * round-trip through the stack. */
    if ((d->op == XIR_I2F || d->op == XIR_F2I) && xir_ref_is_const(d->args[0]))
        return true;
    return false;
}

static void ctx_track(LsCtx *ctx, LsRange *r) {
    if (ctx->nall >= ctx->all_cap) {
        ctx->all_cap *= 2;
        XR_REALLOC_OR_ABORT(ctx->all_ranges, ctx->all_cap * sizeof(LsRange *),
                            "regalloc ctx_track");
    }
    ctx->all_ranges[ctx->nall++] = r;
}

// Forward declarations for functions used before their definitions
static int32_t range_start(const LsRange *r);
static int32_t range_end(const LsRange *r);
static int32_t first_isect(const LsRange *a, const LsRange *b);
static bool isects(const LsRange *a, const LsRange *b);
static void wl_insert(LsRange ***wl, uint32_t *len, uint32_t *cap, LsRange *r);
static LsRange *split_between(LsCtx *ctx, LsRange *r, int32_t from, int32_t to);
static int32_t next_use_after(const LsRange *r, int32_t pos);

/* ========== LsList Operations ========== */

static void ls_list_init(LsList *l, uint32_t cap) {
    XR_DCHECK(l != NULL, "ls_list_init: NULL list");
    l->items = xr_malloc(cap * sizeof(LsRange *));
    l->len = 0;
    l->cap = cap;
}

static void ls_list_free(LsList *l) {
    XR_DCHECK(l != NULL, "ls_list_free: NULL list");
    xr_free(l->items);
    l->items = NULL;
    l->len = l->cap = 0;
}

static void ls_list_add(LsList *l, LsRange *r) {
    XR_DCHECK(l != NULL, "ls_list_add: NULL list");
    XR_DCHECK(r != NULL, "ls_list_add: NULL range");
    if (l->len >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        XR_REALLOC_OR_ABORT(l->items, l->cap * sizeof(LsRange *), "regalloc ls_list_add");
    }
    l->items[l->len++] = r;
}

static void ls_list_remove_at(LsList *l, uint32_t idx) {
    XR_DCHECK(idx < l->len, "ls_list_remove_at: index out of bounds");
    l->items[idx] = l->items[--l->len];
}

// Does the range cover a specific position? (not in a hole)
static bool covers(const LsRange *r, int32_t pos) {
    for (LsInterval *iv = r->first_iv; iv; iv = iv->next) {
        if (iv->start <= pos && pos < iv->end)
            return true;
        if (iv->start > pos)
            break;
    }
    return false;
}

// Is this a CALL-family opcode that clobbers caller-saved registers?
static bool is_call_op(uint16_t op) {
    return op == XIR_CALL || op == XIR_CALL_C || op == XIR_CALL_C_LEAF ||
           op == XIR_CALL_SELF_DIRECT || op == XIR_CALL_DIRECT || op == XIR_CALL_KNOWN ||
           op == XIR_CALL_KNOWN_REG;
}

/*
 * Advance Active/Inactive state machine to position `pos`.
 *
 * Active → Handled:   range ended before pos
 * Active → Inactive:  range has a hole at pos
 * Inactive → Handled: range ended before pos
 * Inactive → Active:  range covers pos again
 */
static void forward_state_to(LsCtx *ctx, int32_t pos) {
    // Active list
    for (uint32_t i = 0; i < ctx->active.len;) {
        LsRange *r = ctx->active.items[i];
        if (range_end(r) <= pos) {
            ls_list_remove_at(&ctx->active, i);
            continue;
        }
        if (!covers(r, pos)) {
            ls_list_remove_at(&ctx->active, i);
            ls_list_add(&ctx->inactive, r);
            continue;
        }
        i++;
    }

    // Inactive list
    for (uint32_t i = 0; i < ctx->inactive.len;) {
        LsRange *r = ctx->inactive.items[i];
        if (range_end(r) <= pos) {
            ls_list_remove_at(&ctx->inactive, i);
            continue;
        }
        if (covers(r, pos)) {
            ls_list_remove_at(&ctx->inactive, i);
            ls_list_add(&ctx->active, r);
            continue;
        }
        i++;
    }
}

/* ========== Interval Operations ========== */

static LsInterval *iv_new(LsCtx *c, int32_t s, int32_t e) {
    LsInterval *iv = pool_alloc(&c->pool, sizeof(LsInterval));
    iv->start = s;
    iv->end = e;
    iv->next = NULL;
    return iv;
}

static void range_add_iv(LsCtx *c, LsRange *r, int32_t s, int32_t e) {
    if (s >= e)
        return;
    if (!r->first_iv) {
        LsInterval *iv = iv_new(c, s, e);
        r->first_iv = r->last_iv = iv;
        return;
    }
    LsInterval *prev = NULL, *cur = r->first_iv;
    while (cur && cur->end < s) {
        prev = cur;
        cur = cur->next;
    }
    int32_t ns = s, ne = e;
    while (cur && cur->start <= e) {
        if (cur->start < ns)
            ns = cur->start;
        if (cur->end > ne)
            ne = cur->end;
        cur = cur->next;
    }
    LsInterval *m = iv_new(c, ns, ne);
    m->next = cur;
    if (prev)
        prev->next = m;
    else
        r->first_iv = m;
    LsInterval *l = m;
    while (l->next)
        l = l->next;
    r->last_iv = l;
}

static void range_shorten(LsRange *r, int32_t ns) {
    /* Only shorten if ns falls within the first interval.
     * Back-edge predecessors may create intervals before the def position;
     * shortening past the end would create an invalid interval (start > end). */
    if (r->first_iv && r->first_iv->start < ns && ns < r->first_iv->end)
        r->first_iv->start = ns;
}

static int32_t range_start(const LsRange *r) {
    return r->first_iv ? r->first_iv->start : INT32_MAX;
}
static int32_t range_end(const LsRange *r) {
    return r->last_iv ? r->last_iv->end : -1;
}

/* ========== UsePosition Operations ========== */

static void range_add_use(LsCtx *c, LsRange *r, int32_t pos) {
    LsUsePos *prev = NULL, *cur = r->uses;
    while (cur && cur->pos < pos) {
        prev = cur;
        cur = cur->next;
    }
    if (cur && cur->pos == pos)
        return;
    LsUsePos *u = pool_alloc(&c->pool, sizeof(LsUsePos));
    u->pos = pos;
    u->next = cur;
    if (prev)
        prev->next = u;
    else
        r->uses = u;
}

static int32_t next_use_after(const LsRange *r, int32_t pos) {
    for (LsUsePos *u = r->uses; u; u = u->next)
        if (u->pos >= pos)
            return u->pos;
    return INT32_MAX;
}

/* ========== Intersection ========== */

static int32_t first_isect(const LsRange *a, const LsRange *b) {
    /* Hole-aware intersection.
     *
     * Both ranges expose their live extent as a sorted, non-overlapping
     * linked list of LsInterval records.  A range is only live during
     * its intervals; the gaps between intervals are "holes" during
     * which the register is logically free.  Treating the whole
     * [range_start, range_end) span as live (the previous conservative
     * behaviour) prevented two ranges from sharing a register whenever
     * they overlapped at the outer bounds but never simultaneously at
     * any real live point.
     *
     * The scan below walks both lists in a merge-style fashion and
     * returns the first position where an A-interval and a B-interval
     * actually overlap.  Complexity is O(|A| + |B|) in the number of
     * interval fragments; these lists are small for typical functions
     * (one-to-three intervals per vreg), so the extra work is
     * negligible compared to the correctness win for wide phi ranges
     * and post-split tails. */
    const LsInterval *ia = a->first_iv;
    const LsInterval *ib = b->first_iv;
    while (ia && ib) {
        if (ia->start < ib->end && ib->start < ia->end) {
            return (ia->start > ib->start) ? ia->start : ib->start;
        }
        /* Advance whichever list finishes earlier so we make
         * monotonic progress through the timeline. */
        if (ia->end <= ib->start)
            ia = ia->next;
        else
            ib = ib->next;
    }
    return INT32_MAX;
}

static bool isects(const LsRange *a, const LsRange *b) {
    return first_isect(a, b) < INT32_MAX;
}

/* PHI-safe conflict check: can PHI dst and arg share a register?
 *
 * The PHI dst's range may be artificially wide due to live-in extensions
 * (needed for linear scan correctness). But if the PHI arg is defined
 * AFTER the dst's last actual use, sharing is safe: the arg overwrites
 * the dst's stale value, and at the back-edge the PHI copy becomes a no-op.
 *
 * Returns true if they genuinely conflict (cannot share). */
static bool phi_ranges_conflict(const LsRange *dst, const LsRange *arg) {
    int32_t arg_start = range_start(arg);
    // Check if dst has any actual use at or after arg's definition
    for (const LsUsePos *u = dst->uses; u; u = u->next) {
        if (u->pos >= arg_start)
            return true;  // dst still needed when arg is defined
    }
    return false;  // arg defined after dst's last use → safe to coalesce
}

/* ========== Deferred Block Analysis ========== */

/*
 * Mark cold/deferred blocks: catch handlers, unreachable paths,
 * and blocks whose ALL predecessors are deferred.
 * Deferred blocks don't block hot-path register allocation.
 */
static void compute_deferred(LsCtx *ctx) {
    XirFunc *f = ctx->func;
    uint32_t n = f->nblk;
    ctx->blk_deferred = xr_calloc(n, sizeof(bool));

    // Pass 1: seed — catch targets and unreachable blocks
    for (uint32_t i = 0; i < n; i++) {
        XirBlock *b = f->blocks[i];
        // Blocks ending in unreachable (deopt/throw)
        if (b->jmp.type == XIR_JMP_UNREACHABLE) {
            ctx->blk_deferred[i] = true;
        }
    }
    // Mark catch handler targets
    for (uint32_t i = 0; i < n; i++) {
        XirBlock *b = f->blocks[i];
        if (b->exception_handler) {
            uint32_t j = blk_idx(ctx, b->exception_handler->id);
            ctx->blk_deferred[j] = true;
        }
    }

    /* Pass 2: propagate — block is deferred if ALL preds are deferred.
     * Entry block (block 0) is never deferred. Iterate to fixpoint. */
    for (int round = 0; round < 4; round++) {
        bool changed = false;
        for (uint32_t i = 1; i < n; i++) {
            if (ctx->blk_deferred[i])
                continue;
            XirBlock *b = f->blocks[i];
            if (b->npred == 0)
                continue;
            bool all_def = true;
            for (uint32_t p = 0; p < b->npred; p++) {
                uint32_t pi = blk_idx(ctx, b->preds[p]->id);
                if (!ctx->blk_deferred[pi]) {
                    all_def = false;
                    break;
                }
            }
            if (all_def) {
                ctx->blk_deferred[i] = true;
                changed = true;
            }
        }
        if (!changed)
            break;
    }
}

/* ========== Position Numbering ========== */

static void number_pos(LsCtx *ctx) {
    int32_t pos = 0;
    for (uint32_t i = 0; i < ctx->func->nblk; i++) {
        ctx->blk_start[i] = pos;
        pos += 2 + 2 * (int32_t) ctx->func->blocks[i]->nins + 2;
        ctx->blk_end[i] = pos;
    }
    ctx->max_pos = pos;
}

// Binary search: blk_start[] is sorted ascending by construction
static uint32_t pos_to_blk(const LsCtx *ctx, int32_t pos) {
    uint32_t lo = 0, hi = ctx->func->nblk;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ctx->blk_end[mid] <= pos)
            lo = mid + 1;
        else if (ctx->blk_start[mid] > pos)
            hi = mid;
        else
            return mid;
    }
    return 0;
}

static bool pos_is_deferred(const LsCtx *ctx, int32_t pos) {
    uint32_t bi = pos_to_blk(ctx, pos);
    return ctx->blk_deferred[bi];
}

/* ========== Build Live Ranges ========== */

static void build_ranges(LsCtx *ctx) {
    XirFunc *func = ctx->func;
    XirLive *live = &ctx->live;

    for (uint32_t v = 0; v < ctx->nvreg; v++) {
        LsRange *r = pool_alloc(&ctx->pool, sizeof(LsRange));
        memset(r, 0, sizeof(LsRange));
        r->vreg = v;
        r->rep = (v < func->nvreg) ? func->vregs[v].rep : 0;
        r->is_fp = vfp(func, v);
        r->is_fixed = false;
        r->assigned = -1;
        r->spill = XRA_SPILL_NONE;
        r->hint = -1;
        r->bundle_id = UINT32_MAX;
        ctx->vreg_ranges[v] = r;
        ctx_track(ctx, r);
    }

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        int32_t bs = ctx->blk_start[bi], be = ctx->blk_end[bi];

        // Live-out: cover entire block
        if (bi < live->nblk) {
            const XirBSet *lo = &live->blocks[bi].live_out;
            int iter = 0, bit;
            while ((bit = xir_bset_iter(lo, &iter)) >= 0) {
                if ((uint32_t) bit < ctx->nvreg)
                    range_add_iv(ctx, ctx->vreg_ranges[bit], bs, be);
            }
        }

        /*
         * Phi args BEFORE backward scan: ensures self-loop phi args
         * create full predecessor intervals, then backward scan correctly
         * shortens them at def points without re-extension.
         */
        for (XirPhi *p = blk->phis; p; p = p->next) {
            for (uint32_t a = 0; a < p->narg; a++) {
                if (!xir_ref_is_vreg(p->args[a]))
                    continue;
                uint32_t v = XIR_REF_INDEX(p->args[a]);
                if (v >= ctx->nvreg)
                    continue;
                XirBlock *pred = (a < blk->npred) ? blk->preds[a] : NULL;
                if (!pred)
                    continue;
                uint32_t pi = blk_idx(ctx, pred->id);
                range_add_iv(ctx, ctx->vreg_ranges[v], ctx->blk_start[pi], ctx->blk_end[pi]);
                range_add_use(ctx, ctx->vreg_ranges[v], ctx->blk_end[pi] - 1);
            }
        }

        // Backward scan instructions
        for (int32_t ii = (int32_t) blk->nins - 1; ii >= 0; ii--) {
            XirIns *ins = &blk->ins[ii];
            int32_t upos = bs + 2 + 2 * ii;      // use (even)
            int32_t dpos = bs + 2 + 2 * ii + 1;  // def (odd)

            if (xir_ref_is_vreg(ins->dst)) {
                uint32_t v = XIR_REF_INDEX(ins->dst);
                if (v < ctx->nvreg) {
                    range_shorten(ctx->vreg_ranges[v], dpos);
                    range_add_use(ctx, ctx->vreg_ranges[v], dpos);
                }
            }
            for (int a = 0; a < 2; a++) {
                if (xir_ref_is_vreg(ins->args[a])) {
                    uint32_t v = XIR_REF_INDEX(ins->args[a]);
                    if (v < ctx->nvreg) {
                        range_add_iv(ctx, ctx->vreg_ranges[v], bs, upos + 1);
                        range_add_use(ctx, ctx->vreg_ranges[v], upos);
                    }
                }
            }
            // Pool args: extend live range for call argument vregs
            if (xir_ref_is_vreg(ins->dst)) {
                uint32_t dvi = XIR_REF_INDEX(ins->dst);
                if (dvi < func->nvreg && func->vregs[dvi].call_nargs > 0) {
                    XirVReg *dvr = &func->vregs[dvi];
                    for (uint16_t pi = 0; pi < dvr->call_nargs; pi++) {
                        XirRef pa = func->call_arg_pool[dvr->call_arg_start + pi];
                        if (xir_ref_is_vreg(pa)) {
                            uint32_t v = XIR_REF_INDEX(pa);
                            if (v < ctx->nvreg) {
                                range_add_iv(ctx, ctx->vreg_ranges[v], bs, upos + 1);
                                range_add_use(ctx, ctx->vreg_ranges[v], upos);
                            }
                        }
                    }
                }
            }
        }

        // Terminator arg
        if (xir_ref_is_vreg(blk->jmp.arg)) {
            uint32_t v = XIR_REF_INDEX(blk->jmp.arg);
            if (v < ctx->nvreg) {
                int32_t tp = be - 2;
                range_add_iv(ctx, ctx->vreg_ranges[v], bs, tp + 1);
                range_add_use(ctx, ctx->vreg_ranges[v], tp);
            }
        }

        // Phi defs: shorten start to block entry
        for (XirPhi *p = blk->phis; p; p = p->next) {
            if (xir_ref_is_vreg(p->dst)) {
                uint32_t v = XIR_REF_INDEX(p->dst);
                if (v < ctx->nvreg) {
                    range_shorten(ctx->vreg_ranges[v], bs);
                    range_add_use(ctx, ctx->vreg_ranges[v], bs);
                }
            }
        }

        /* Live-in: ensure covered from block start.
         * This is needed for linear scan correctness: without full-block
         * coverage, the allocator may reuse a register during the gap
         * between last-use and block-end on a linearized path. */
        if (bi < live->nblk) {
            const XirBSet *li = &live->blocks[bi].live_in;
            int iter = 0, bit;
            while ((bit = xir_bset_iter(li, &iter)) >= 0) {
                if ((uint32_t) bit < ctx->nvreg)
                    range_add_iv(ctx, ctx->vreg_ranges[bit], bs, be);
            }
        }
    }

    /* Bug fix: dead vregs (defined but never used, e.g. void calls)
     * still need a register for codegen to emit the instruction. */
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        int32_t bs = ctx->blk_start[bi];
        for (uint32_t ii = 0; ii < blk->nins; ii++) {
            XirIns *ins = &blk->ins[ii];
            if (xir_ref_is_vreg(ins->dst)) {
                uint32_t v = XIR_REF_INDEX(ins->dst);
                if (v < ctx->nvreg && !ctx->vreg_ranges[v]->first_iv) {
                    int32_t dpos = bs + 2 + 2 * (int32_t) ii + 1;
                    range_add_iv(ctx, ctx->vreg_ranges[v], dpos, dpos + 1);
                    range_add_use(ctx, ctx->vreg_ranges[v], dpos);
                }
            }
        }
    }
}

/* ========== Fixed Intervals for CALL Clobbers ========== */

/*
 * Create fixed intervals for caller-saved registers at CALL points.
 * alloc_regs[0..14] = X1-X15 are all caller-saved in ARM64 ABI.
 * Fixed intervals participate in conflict detection: they prevent the
 * allocator from assigning live-across-call vregs to caller-saved regs,
 * triggering split/spill automatically instead of relying on codegen
 * hard-coding scratch registers.
 *
 * Fixed intervals are pre-assigned and never evicted or spilled.
 */

// alloc_regs[0..14] = X1-X15 are caller-saved; alloc_regs[15..22] = X20-X27 callee-saved
#define NUM_CALLER_SAVED_GP 15
// v0-v7 are caller-saved; v8-v15 are callee-saved
#define NUM_CALLER_SAVED_FP 8

static void build_fixed_intervals(LsCtx *ctx) {
    XirFunc *f = ctx->func;

    /* Pre-create one fixed range per caller-saved register (15 GP + 8 FP).
     * Each accumulates intervals from all CALL points. NULL = no CALL uses this class. */
    LsRange *gp_fixed[NUM_CALLER_SAVED_GP] = {0}, *fp_fixed[NUM_CALLER_SAVED_FP] = {0};

    for (uint32_t bi = 0; bi < f->nblk; bi++) {
        XirBlock *blk = f->blocks[bi];
        int32_t bs = ctx->blk_start[bi];

        for (uint32_t ii = 0; ii < blk->nins; ii++) {
            XirIns *ins = &blk->ins[ii];
            if (!is_call_op(ins->op))
                continue;

            int32_t call_pos = bs + 2 + 2 * (int32_t) ii;

            // GP caller-saved: alloc_regs[0..14] = X1-X15
            for (int ci = 0; ci < NUM_CALLER_SAVED_GP; ci++) {
                if (!gp_fixed[ci]) {
                    LsRange *fr = pool_alloc(&ctx->pool, sizeof(LsRange));
                    memset(fr, 0, sizeof(LsRange));
                    fr->vreg = UINT32_MAX;
                    fr->is_fp = false;
                    fr->is_fixed = true;
                    fr->assigned = (int8_t) ci;
                    fr->spill = XRA_SPILL_NONE;
                    fr->hint = -1;
                    fr->bundle_id = UINT32_MAX;
                    gp_fixed[ci] = fr;
                    ctx_track(ctx, fr);
                }
                range_add_iv(ctx, gp_fixed[ci], call_pos, call_pos + 2);
            }

            // FP caller-saved: v0-v7 (8 registers, separate from GP count)
            for (int ci = 0; ci < NUM_CALLER_SAVED_FP; ci++) {
                if (!fp_fixed[ci]) {
                    LsRange *fr = pool_alloc(&ctx->pool, sizeof(LsRange));
                    memset(fr, 0, sizeof(LsRange));
                    fr->vreg = UINT32_MAX;
                    fr->is_fp = true;
                    fr->is_fixed = true;
                    fr->assigned = (int8_t) ci;
                    fr->spill = XRA_SPILL_NONE;
                    fr->hint = -1;
                    fr->bundle_id = UINT32_MAX;
                    fp_fixed[ci] = fr;
                    ctx_track(ctx, fr);
                }
                range_add_iv(ctx, fp_fixed[ci], call_pos, call_pos + 2);
            }
        }
    }
}

/* ========== LiveRange Bundles ========== */

/*
 * Union-Find for grouping Phi/MOV-connected non-overlapping ranges.
 * Bundled ranges share register hints and spill slots, reducing
 * unnecessary MOVs at Phi edges and enabling TryReuseSpillForPhi.
 */
static uint32_t bundle_find(uint32_t *parent, uint32_t x) {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

static void bundle_union(uint32_t *parent, uint32_t x, uint32_t y) {
    x = bundle_find(parent, x);
    y = bundle_find(parent, y);
    if (x == y)
        return;
    // Merge smaller vreg id into larger to keep stable roots
    if (x > y)
        parent[x] = y;
    else
        parent[y] = x;
}

static void compute_bundles(LsCtx *ctx) {
    uint32_t nv = ctx->nvreg;
    if (nv == 0)
        return;

    ctx->bundle_parent = xr_malloc(nv * sizeof(uint32_t));
    ctx->bundle_hint = xr_malloc(nv * sizeof(int8_t));
    ctx->bundle_spill = xr_malloc(nv * sizeof(int16_t));
    for (uint32_t i = 0; i < nv; i++) {
        ctx->bundle_parent[i] = i;
        ctx->bundle_hint[i] = -1;
        ctx->bundle_spill[i] = XRA_SPILL_NONE;
    }

    XirFunc *f = ctx->func;

    /* Connect Phi dst ↔ args if their live ranges don't overlap.
     * Use intervals_isect (interval-aware) instead of isects (overall bounds)
     * because the PHI dst may have a hole between the body use and exit use;
     * the PHI arg writing during this hole is safe (it IS the new PHI value). */
    for (uint32_t bi = 0; bi < f->nblk; bi++) {
        for (XirPhi *p = f->blocks[bi]->phis; p; p = p->next) {
            if (!xir_ref_is_vreg(p->dst))
                continue;
            uint32_t dv = XIR_REF_INDEX(p->dst);
            if (dv >= nv)
                continue;
            LsRange *dr = ctx->vreg_ranges[dv];
            if (!dr || !dr->first_iv)
                continue;

            for (uint32_t a = 0; a < p->narg; a++) {
                if (!xir_ref_is_vreg(p->args[a]))
                    continue;
                uint32_t av = XIR_REF_INDEX(p->args[a]);
                if (av >= nv || av == dv)
                    continue;
                LsRange *ar = ctx->vreg_ranges[av];
                if (!ar || !ar->first_iv)
                    continue;
                if (dr->is_fp != ar->is_fp)
                    continue;

                if (!phi_ranges_conflict(dr, ar))
                    bundle_union(ctx->bundle_parent, dv, av);
            }
        }
    }

    // Connect MOV dst ↔ src if their live ranges don't overlap
    for (uint32_t bi = 0; bi < f->nblk; bi++) {
        XirBlock *blk = f->blocks[bi];
        for (uint32_t ii = 0; ii < blk->nins; ii++) {
            XirIns *ins = &blk->ins[ii];
            if (!xir_op_is_copy(ins->op))
                continue;
            if (!xir_ref_is_vreg(ins->dst) || !xir_ref_is_vreg(ins->args[0]))
                continue;
            uint32_t dv = XIR_REF_INDEX(ins->dst);
            uint32_t sv = XIR_REF_INDEX(ins->args[0]);
            if (dv >= nv || sv >= nv || dv == sv)
                continue;
            LsRange *dr = ctx->vreg_ranges[dv], *sr = ctx->vreg_ranges[sv];
            if (!dr || !sr || !dr->first_iv || !sr->first_iv)
                continue;
            if (dr->is_fp != sr->is_fp)
                continue;

            if (!isects(dr, sr))
                bundle_union(ctx->bundle_parent, dv, sv);
        }
    }

    // Store bundle_id in each range
    for (uint32_t v = 0; v < nv; v++) {
        LsRange *r = ctx->vreg_ranges[v];
        if (r)
            r->bundle_id = bundle_find(ctx->bundle_parent, v);
    }
}

/* ========== Hints ========== */

/*
 * Multi-pass hint propagation with priority levels:
 *   0. Bundle hints (from already-hinted bundle members)
 *   1. ABI params (highest — eliminates prologue MOVs)
 *   2. Phi dst ↔ args (eliminates edge copies)
 *   3. MOV dst ↔ src (eliminates register copies)
 *   4. Iterative fixpoint (propagate through chains + bundles)
 */
static void compute_hints(LsCtx *ctx) {
    XirFunc *f = ctx->func;
    uint32_t nv = ctx->nvreg;

    // Pass 1: ABI parameter hints
    uint32_t gpi = 0, fpi = 0;
    for (uint32_t v = 0; v < f->num_params && v < nv; v++) {
        LsRange *r = ctx->vreg_ranges[v];
        if (r->is_fp) {
            if (fpi < xir_current_target->nfpr)
                r->hint = (int8_t) fpi++;
        } else {
            if (gpi < xir_current_target->ngpr)
                r->hint = (int8_t) gpi++;
        }
    }

    // Pass 2-4: iterative Phi + MOV + Bundle propagation (max 3 rounds)
    for (int round = 0; round < 3; round++) {
        bool changed = false;

        for (uint32_t bi = 0; bi < f->nblk; bi++) {
            XirBlock *blk = f->blocks[bi];

            // Phi hints: propagate between dst and args
            for (XirPhi *p = blk->phis; p; p = p->next) {
                if (!xir_ref_is_vreg(p->dst))
                    continue;
                uint32_t dv = XIR_REF_INDEX(p->dst);
                if (dv >= nv)
                    continue;
                LsRange *dr = ctx->vreg_ranges[dv];
                for (uint32_t a = 0; a < p->narg; a++) {
                    if (!xir_ref_is_vreg(p->args[a]))
                        continue;
                    uint32_t av = XIR_REF_INDEX(p->args[a]);
                    if (av >= nv)
                        continue;
                    LsRange *ar = ctx->vreg_ranges[av];
                    if (dr->hint >= 0 && ar->hint < 0) {
                        ar->hint = dr->hint;
                        changed = true;
                    } else if (ar->hint >= 0 && dr->hint < 0) {
                        dr->hint = ar->hint;
                        changed = true;
                    }
                }
            }

            // MOV hints: propagate between dst and src
            for (uint32_t ii = 0; ii < blk->nins; ii++) {
                XirIns *ins = &blk->ins[ii];
                if (!xir_op_is_copy(ins->op))
                    continue;
                if (!xir_ref_is_vreg(ins->dst) || !xir_ref_is_vreg(ins->args[0]))
                    continue;
                uint32_t dv = XIR_REF_INDEX(ins->dst), sv = XIR_REF_INDEX(ins->args[0]);
                if (dv >= nv || sv >= nv)
                    continue;
                LsRange *dr = ctx->vreg_ranges[dv], *sr = ctx->vreg_ranges[sv];
                if (dr->hint >= 0 && sr->hint < 0) {
                    sr->hint = dr->hint;
                    changed = true;
                } else if (sr->hint >= 0 && dr->hint < 0) {
                    dr->hint = sr->hint;
                    changed = true;
                }
            }
        }

        // Bundle hint propagation: share hints within bundles
        if (ctx->bundle_parent) {
            for (uint32_t v = 0; v < nv; v++) {
                LsRange *r = ctx->vreg_ranges[v];
                if (!r || r->hint < 0)
                    continue;
                uint32_t bid = bundle_find(ctx->bundle_parent, v);
                if (ctx->bundle_hint[bid] < 0) {
                    ctx->bundle_hint[bid] = r->hint;
                    changed = true;
                }
            }
            for (uint32_t v = 0; v < nv; v++) {
                LsRange *r = ctx->vreg_ranges[v];
                if (!r || r->hint >= 0)
                    continue;
                uint32_t bid = bundle_find(ctx->bundle_parent, v);
                if (ctx->bundle_hint[bid] >= 0) {
                    r->hint = ctx->bundle_hint[bid];
                    changed = true;
                }
            }
        }

        if (!changed)
            break;
    }
}

/* ========== Split ========== */

static LsRange *split_at(LsCtx *ctx, LsRange *range, int32_t pos) {
    XR_DCHECK(ctx != NULL, "split_at: NULL ctx");
    XR_DCHECK(range != NULL, "split_at: NULL range");
    LsRange *tail = pool_alloc(&ctx->pool, sizeof(LsRange));
    tail->vreg = range->vreg;
    tail->rep = range->rep;
    tail->is_fp = range->is_fp;
    tail->is_fixed = false;
    tail->assigned = -1;
    tail->spill = XRA_SPILL_NONE;
    tail->hint = range->hint;
    tail->bundle_id = range->bundle_id;
    tail->parent = range->parent ? range->parent : range;
    tail->next_sibling = range->next_sibling;
    range->next_sibling = tail;

    // Split intervals
    LsInterval *prev = NULL;
    for (LsInterval *iv = range->first_iv; iv;) {
        LsInterval *next = iv->next;
        if (iv->start >= pos) {
            if (!tail->first_iv)
                tail->first_iv = iv;
            if (prev)
                prev->next = NULL;
            else
                range->first_iv = NULL;
            range->last_iv = prev;
            LsInterval *l = iv;
            while (l->next)
                l = l->next;
            tail->last_iv = l;
            goto split_uses;
        }
        if (iv->start < pos && iv->end > pos) {
            LsInterval *ni = iv_new(ctx, pos, iv->end);
            ni->next = iv->next;
            iv->end = pos;
            iv->next = NULL;
            range->last_iv = iv;
            tail->first_iv = ni;
            LsInterval *l = ni;
            while (l->next)
                l = l->next;
            tail->last_iv = l;
            goto split_uses;
        }
        prev = iv;
        iv = next;
    }

split_uses:;
    LsUsePos *pu = NULL;
    for (LsUsePos *u = range->uses; u;) {
        LsUsePos *next = u->next;
        if (u->pos >= pos) {
            tail->uses = u;
            if (pu)
                pu->next = NULL;
            else
                range->uses = NULL;
            break;
        }
        pu = u;
        u = next;
    }

    ctx_track(ctx, tail);
    return tail;
}

/*
 * Find optimal split position in (from, to).
 * Prefer block boundaries with lowest loop depth; fall back to
 * aligned mid-block position when no boundary exists.
 * Mid-block splits produce gap moves in connect_ranges().
 */
static LsRange *split_between(LsCtx *ctx, LsRange *r, int32_t from, int32_t to) {
    XR_DCHECK(ctx != NULL, "split_between: NULL ctx");
    XR_DCHECK(r != NULL, "split_between: NULL range");
    int32_t sp = -1;
    int best_depth = INT32_MAX;

    for (uint32_t i = 0; i < ctx->func->nblk; i++) {
        int32_t bs = ctx->blk_start[i];
        if (bs <= from || bs >= to)
            continue;

        int depth = (int) xir_block_loop_depth(ctx->func, ctx->func->blocks[i]->id);

        if (sp < 0 || depth < best_depth || (depth == best_depth && bs > sp)) {
            sp = bs;
            best_depth = depth;
        }
    }

    // No block boundary: fall back to even-aligned position in (from, to)
    if (sp < 0) {
        sp = to;
        if (sp & 1)
            sp--;
        if (sp <= from)
            sp = from + 2;
        if (sp & 1)
            sp++;
        if (sp >= to)
            sp = to;
    }
    return split_at(ctx, r, sp);
}

/* ========== Spill ========== */

/*
 * Assign a spill slot, preferring bundle reuse (TryReuseSpillForPhi),
 * then slot reuse by lifetime, then fresh allocation.
 */
static void assign_spill_slot(LsCtx *ctx, LsRange *r) {
    /* TryReuseSpillForPhi: reuse spill slot from same bundle.
     * PHI-connected vregs must have the same rep, so type is always compatible. */
    if (ctx->bundle_parent && r->vreg < ctx->nvreg) {
        uint32_t bid = bundle_find(ctx->bundle_parent, r->vreg);
        if (ctx->bundle_spill[bid] != XRA_SPILL_NONE) {
            r->spill = ctx->bundle_spill[bid];
            // Assert type compatibility: bundle members share rep
            XR_DCHECK(r->spill >= XIR_MAX_SPILL_SLOTS ||
                          ctx->slot_is_ptr[r->spill] ==
                              (ctx->func->vregs[r->vreg].rep == XR_REP_PTR),
                      "bundle spill slot type mismatch (PTR vs non-PTR)");
            int32_t rend = range_end(r);
            if (r->spill >= 0 && r->spill < XIR_MAX_SPILL_SLOTS && rend > ctx->slot_end[r->spill])
                ctx->slot_end[r->spill] = rend;
            return;
        }
    }

    /* Try to reuse a slot whose last user ended before this range starts.
     * PTR and non-PTR vregs must not share slots: emit_ptr_spill_writeback
     * writes all live PTR regs to their spill slots before CALL_C, which
     * would corrupt a non-PTR value stored in a shared slot. */
    bool is_ptr = r->vreg < ctx->nvreg && ctx->func->vregs[r->vreg].rep == XR_REP_PTR;
    int32_t rstart = range_start(r);
    for (int16_t s = 0; s < ctx->next_spill && s < XIR_MAX_SPILL_SLOTS; s++) {
        if (ctx->slot_end[s] <= rstart && ctx->slot_is_ptr[s] == is_ptr) {
            r->spill = s;
            int32_t rend = range_end(r);
            if (rend > ctx->slot_end[s])
                ctx->slot_end[s] = rend;
            ctx->slot_is_ptr[s] = is_ptr;
            goto update_bundle;
        }
    }

    /* No reusable slot: allocate a new one.
     *
     * Set slot_end to the FULL vreg's last-live position, not just this
     * sibling's range_end. Reason: when a vreg is split into siblings
     * (head reg + tail spill), each sibling calls assign_spill_slot
     * independently, and the tail walks the bundle path
     * (TryReuseSpillForPhi) to land on the same slot the head used.
     * The bundle path doesn't go through reuse-by-lifetime, so it will
     * silently overlap with whatever vreg occupied the slot in between.
     * Pre-reserving the slot for the entire vreg's lifespan prevents
     * any later reuse-by-lifetime from picking it.
     */
    int32_t vreg_last = range_end(r);
    if (r->vreg < ctx->nvreg && ctx->vreg_ranges[r->vreg]) {
        for (LsRange *s = ctx->vreg_ranges[r->vreg]; s; s = s->next_sibling) {
            int32_t e = range_end(s);
            if (e > vreg_last)
                vreg_last = e;
        }
    }
    {
        int16_t slot = ctx->next_spill++;
        r->spill = slot;
        if (slot < XIR_MAX_SPILL_SLOTS) {
            ctx->slot_end[slot] = vreg_last;
            ctx->slot_is_ptr[slot] = is_ptr;
        }
    }

update_bundle:
    // Update bundle spill slot for TryReuseSpillForPhi
    if (ctx->bundle_parent && r->vreg < ctx->nvreg) {
        uint32_t bid = bundle_find(ctx->bundle_parent, r->vreg);
        if (ctx->bundle_spill[bid] == XRA_SPILL_NONE)
            ctx->bundle_spill[bid] = r->spill;
    }
}

/*
 * Spill a range with spill-then-reload: if the range has future use
 * positions, split at the first use and re-enqueue the tail for a
 * second chance at register allocation. This ensures each spill is
 * "minimal range spill" rather than "permanent spill".
 */
static void spill_range(LsCtx *ctx, LsRange *r, LsRange ***wl, uint32_t *wl_len, uint32_t *wl_cap) {
    r->assigned = -1;
    if (is_remat(ctx->func, r->vreg)) {
        r->spill = XRA_SPILL_REMAT;
        return;
    }

    // Spill-then-reload: split at first future use, re-enqueue tail
    int32_t rstart = range_start(r);
    for (LsUsePos *u = r->uses; u; u = u->next) {
        if (u->pos > rstart + 2) {
            LsRange *tail = split_between(ctx, r, rstart, u->pos);
            wl_insert(wl, wl_len, wl_cap, tail);
            break;
        }
    }

    assign_spill_slot(ctx, r);
}

/* ========== Worklist ========== */

/*
 * Start-ascending sort: canonical linear scan order (V8/Dart/LLVM).
 * Ranges with earlier start positions are processed first.
 * Tie-break: longer range first (more constrained), then by vreg.
 */
static int wl_cmp(const void *a, const void *b) {
    const LsRange *ra = *(const LsRange *const *) a;
    const LsRange *rb = *(const LsRange *const *) b;
    int32_t sa = range_start(ra), sb = range_start(rb);
    if (sa != sb)
        return (sa < sb) ? -1 : 1;
    int32_t la = range_end(ra) - sa;
    int32_t lb = range_end(rb) - sb;
    if (la != lb)
        return (la > lb) ? -1 : 1;  // longer first
    return (int) ra->vreg - (int) rb->vreg;
}

static void wl_insert(LsRange ***wl, uint32_t *len, uint32_t *cap, LsRange *r) {
    if (*len >= *cap) {
        uint32_t new_cap = (*cap) * 2;
        LsRange **buf = *wl;
        XR_REALLOC_OR_ABORT(buf, new_cap * sizeof(LsRange *), "regalloc wl_insert");
        *wl = buf;
        *cap = new_cap;
    }
    uint32_t i = *len;
    while (i > 0 && wl_cmp(&r, &(*wl)[i - 1]) < 0) {
        (*wl)[i] = (*wl)[i - 1];
        i--;
    }
    (*wl)[i] = r;
    (*len)++;
}

/* ========== Core Allocation (Active/Inactive State Machine) ========== */

/*
 * Scan Active + Inactive lists to compute free_until positions per register.
 * This is O(active + inactive) instead of O(all_ranges) — the key improvement.
 *
 * Two-tier metric: free_hot (non-deferred conflicts) and free_any (all conflicts).
 * SpillMode: deferred-only conflicts don't block hot-path allocation.
 */
static bool alloc_free_reg(LsCtx *ctx, LsRange *range, LsRange ***wl, uint32_t *wl_len,
                           uint32_t *wl_cap) {
    bool isfp = range->is_fp;
    int mx = isfp ? xir_current_target->nfpr : xir_current_target->ngpr;
    int32_t rend = range_end(range);
    int32_t rstart = range_start(range);

    int32_t free_hot[32], free_any[32];
    for (int i = 0; i < mx; i++) {
        free_hot[i] = INT32_MAX;
        free_any[i] = INT32_MAX;
    }

    // Bundle ID for same-bundle skipping (PHI coalescing)
    uint32_t rbid = (ctx->bundle_parent && range->vreg < ctx->nvreg)
                        ? bundle_find(ctx->bundle_parent, range->vreg)
                        : UINT32_MAX;

    // Active ranges: definitely conflict at rstart (they cover current pos)
    for (uint32_t i = 0; i < ctx->active.len; i++) {
        LsRange *o = ctx->active.items[i];
        if (o->is_fp != isfp)
            continue;
        int8_t reg = o->assigned;
        if (reg < 0 || reg >= mx)
            continue;
        /* Skip same-bundle ranges: PHI dst/arg verified non-conflicting
         * at use-position level by phi_ranges_conflict() */
        if (rbid != UINT32_MAX && o->vreg < ctx->nvreg &&
            bundle_find(ctx->bundle_parent, o->vreg) == rbid)
            continue;
        // Active range is alive at rstart, conflict is immediate
        free_any[reg] = rstart;
        if (!pos_is_deferred(ctx, rstart))
            free_hot[reg] = rstart;
    }

    // Inactive ranges: may conflict at intersection points
    for (uint32_t i = 0; i < ctx->inactive.len; i++) {
        LsRange *o = ctx->inactive.items[i];
        if (o->is_fp != isfp)
            continue;
        int8_t reg = o->assigned;
        if (reg < 0 || reg >= mx)
            continue;
        // Skip same-bundle ranges
        if (rbid != UINT32_MAX && o->vreg < ctx->nvreg &&
            bundle_find(ctx->bundle_parent, o->vreg) == rbid)
            continue;
        int32_t is = first_isect(range, o);
        if (is < free_any[reg])
            free_any[reg] = is;
        if (is < free_hot[reg] && !pos_is_deferred(ctx, is))
            free_hot[reg] = is;
    }

    /* Dynamic bundle hint: if no static hint, inherit from already-allocated
     * bundle member. This completes PHI coalescing: even when the PHI dst
     * has no ABI param hint, once it's allocated the PHI arg picks up
     * its register via the bundle. */
    int8_t hint = range->hint;
    if (hint < 0 && rbid != UINT32_MAX) {
        for (uint32_t i = 0; i < ctx->active.len; i++) {
            LsRange *o = ctx->active.items[i];
            if (o->is_fp != isfp || o->assigned < 0)
                continue;
            if (o->vreg < ctx->nvreg && bundle_find(ctx->bundle_parent, o->vreg) == rbid) {
                hint = o->assigned;
                break;
            }
        }
    }

    // Try hint register first
    if (hint >= 0 && hint < mx && free_hot[hint] >= rend) {
        range->assigned = hint;
        if (free_any[hint] < rend && free_any[hint] > rstart + 2) {
            LsRange *tail = split_between(ctx, range, rstart, free_any[hint]);
            wl_insert(wl, wl_len, wl_cap, tail);
        }
        return true;
    }

    // Pick register free longest on hot path
    int8_t best = -1;
    int32_t best_hot = 0;
    for (int r = 0; r < mx; r++) {
        if (free_hot[r] > best_hot) {
            best_hot = free_hot[r];
            best = (int8_t) r;
        }
    }
    if (best < 0)
        return false;

    if (best_hot >= rend) {
        range->assigned = best;
        // SpillMode: split at deferred conflict if needed
        if (free_any[best] < rend && free_any[best] > rstart + 2) {
            LsRange *tail = split_between(ctx, range, rstart, free_any[best]);
            wl_insert(wl, wl_len, wl_cap, tail);
        }
        return true;
    }

    // Partial free: split at conflict, assign head
    if (best_hot > rstart + 2) {
        int32_t sp = (free_any[best] < best_hot) ? free_any[best] : best_hot;
        LsRange *tail = split_between(ctx, range, rstart, sp);
        range->assigned = best;
        wl_insert(wl, wl_len, wl_cap, tail);
        return true;
    }
    return false;
}

/*
 * All registers blocked: find best eviction candidate from Active/Inactive.
 * Fixed intervals are never evicted (CALL clobber constraints).
 */
static void alloc_blocked_reg(LsCtx *ctx, LsRange *range, LsRange ***wl, uint32_t *wl_len,
                              uint32_t *wl_cap) {
    bool isfp = range->is_fp;
    int mx = isfp ? xir_current_target->nfpr : xir_current_target->ngpr;
    int32_t rstart = range_start(range);
    int32_t rend = range_end(range);

    // next_use_after for each register (from all conflicting ranges)
    int32_t nuse[32];
    bool has_fixed[32];
    for (int i = 0; i < mx; i++) {
        nuse[i] = INT32_MAX;
        has_fixed[i] = false;
    }

    // Scan active ranges
    for (uint32_t i = 0; i < ctx->active.len; i++) {
        LsRange *o = ctx->active.items[i];
        if (o->is_fp != isfp)
            continue;
        int8_t reg = o->assigned;
        if (reg < 0 || reg >= mx)
            continue;
        if (o->is_fixed) {
            has_fixed[reg] = true;
            nuse[reg] = 0;
            continue;
        }
        int32_t nu = next_use_after(o, rstart);
        if (nu < nuse[reg])
            nuse[reg] = nu;
    }

    // Scan inactive ranges
    for (uint32_t i = 0; i < ctx->inactive.len; i++) {
        LsRange *o = ctx->inactive.items[i];
        if (o->is_fp != isfp)
            continue;
        int8_t reg = o->assigned;
        if (reg < 0 || reg >= mx)
            continue;
        if (!isects(range, o))
            continue;
        if (o->is_fixed) {
            has_fixed[reg] = true;
            nuse[reg] = 0;
            continue;
        }
        int32_t nu = next_use_after(o, rstart);
        if (nu < nuse[reg])
            nuse[reg] = nu;
    }

    // Pick register with latest next-use, excluding fixed-blocked registers
    int8_t best = -1;
    int32_t best_pos = 0;
    for (int r = 0; r < mx; r++) {
        if (has_fixed[r])
            continue;
        if (nuse[r] > best_pos) {
            best_pos = nuse[r];
            best = (int8_t) r;
        }
    }

    int32_t our_first = next_use_after(range, rstart);
    if (best_pos <= our_first || best < 0) {
        spill_range(ctx, range, wl, wl_len, wl_cap);
        return;
    }

    /* Split-and-evict conflicting ranges on the chosen register.
     * Check intersection before removing; don't increment after swap-remove.
     *
     * Use max(ostart, rstart) as split lower bound: if ostart < rstart,
     * split_between could pick a block boundary before rstart, causing
     * the tail to enter the worklist at a position earlier than the
     * current scan position. The Active/Inactive state machine only
     * moves forward, so a backward worklist entry would miss ranges
     * that were already expired, leading to register conflicts. */
    for (uint32_t i = 0; i < ctx->active.len;) {
        LsRange *o = ctx->active.items[i];
        if (o->is_fp != isfp || o->assigned != best || o->is_fixed) {
            i++;
            continue;
        }
        int32_t isect = first_isect(range, o);
        if (isect >= INT32_MAX) {
            i++;
            continue;
        }
        ls_list_remove_at(&ctx->active, i);
        int32_t ostart = range_start(o);
        int32_t split_lo = (rstart > ostart) ? rstart : ostart;
        if (isect > split_lo + 2) {
            LsRange *tail = split_between(ctx, o, split_lo, isect);
            tail->assigned = -1;
            wl_insert(wl, wl_len, wl_cap, tail);
        } else if (isect > ostart + 2) {
            LsRange *tail = split_at(ctx, o, rstart);
            tail->assigned = -1;
            wl_insert(wl, wl_len, wl_cap, tail);
        } else {
            o->assigned = -1;
            spill_range(ctx, o, wl, wl_len, wl_cap);
        }
    }
    for (uint32_t i = 0; i < ctx->inactive.len;) {
        LsRange *o = ctx->inactive.items[i];
        if (o->is_fp != isfp || o->assigned != best || o->is_fixed) {
            i++;
            continue;
        }
        int32_t isect = first_isect(range, o);
        if (isect >= INT32_MAX) {
            i++;
            continue;
        }
        ls_list_remove_at(&ctx->inactive, i);
        int32_t ostart = range_start(o);
        int32_t split_lo = (rstart > ostart) ? rstart : ostart;
        if (isect > split_lo + 2) {
            LsRange *tail = split_between(ctx, o, split_lo, isect);
            tail->assigned = -1;
            wl_insert(wl, wl_len, wl_cap, tail);
        } else if (isect > ostart + 2) {
            LsRange *tail = split_at(ctx, o, rstart);
            tail->assigned = -1;
            wl_insert(wl, wl_len, wl_cap, tail);
        } else {
            o->assigned = -1;
            spill_range(ctx, o, wl, wl_len, wl_cap);
        }
    }

    range->assigned = best;

    // If our range extends beyond best_pos, split ourselves too
    if (best_pos < rend && best_pos > rstart + 2) {
        LsRange *tail = split_between(ctx, range, rstart, best_pos);
        wl_insert(wl, wl_len, wl_cap, tail);
    }
}

/* ========== Main Loop (Active/Inactive State Machine) ========== */

static void ls_allocate(LsCtx *ctx) {
    XR_DCHECK(ctx != NULL, "ls_allocate: NULL ctx");
    uint32_t wl_cap = ctx->nvreg + 64;
    LsRange **wl = xr_malloc(wl_cap * sizeof(LsRange *));
    uint32_t wl_len = 0;

    // Populate worklist: all vreg ranges with live intervals
    for (uint32_t v = 0; v < ctx->nvreg; v++) {
        LsRange *r = ctx->vreg_ranges[v];
        if (r->first_iv)
            wl[wl_len++] = r;
    }
    qsort(wl, wl_len, sizeof(LsRange *), wl_cmp);

    // Initialize Active/Inactive lists
    ls_list_init(&ctx->active, 32);
    ls_list_init(&ctx->inactive, 64);

    // Seed fixed intervals into inactive (they start pre-assigned)
    for (uint32_t i = 0; i < ctx->nall; i++) {
        LsRange *r = ctx->all_ranges[i];
        if (r->is_fixed && r->assigned >= 0)
            ls_list_add(&ctx->inactive, r);
    }

    while (wl_len > 0) {
        LsRange *r = wl[0];
        memmove(wl, wl + 1, (wl_len - 1) * sizeof(LsRange *));
        wl_len--;
        if (!r->first_iv || r->assigned >= 0)
            continue;

        // Advance state machine to current range's start position
        int32_t pos = range_start(r);
        forward_state_to(ctx, pos);

        // Try to allocate a free register, then try eviction
        if (!alloc_free_reg(ctx, r, &wl, &wl_len, &wl_cap))
            alloc_blocked_reg(ctx, r, &wl, &wl_len, &wl_cap);

        // If successfully assigned, add to active list
        if (r->assigned >= 0)
            ls_list_add(&ctx->active, r);
    }
    xr_free(wl);
}

/* ========== Result Output (Per-Segment Architecture) ========== */

// Convert RA position to instruction index within its block
static uint16_t pos_to_ins_idx(const LsCtx *ctx, int32_t pos) {
    uint32_t bi = pos_to_blk(ctx, pos);
    int32_t rel = pos - ctx->blk_start[bi];
    if (rel < 2)
        return 0;
    uint32_t nins = ctx->func->blocks[bi]->nins;
    uint16_t idx = (uint16_t) ((rel - 2) / 2);
    return (idx < nins) ? idx : (uint16_t) nins;
}

static int gap_cmp(const void *a, const void *b) {
    const XraGapMove *ga = (const XraGapMove *) a;
    const XraGapMove *gb = (const XraGapMove *) b;
    if (ga->gap_blk != gb->gap_blk)
        return (ga->gap_blk < gb->gap_blk) ? -1 : 1;
    return (int) ga->gap_ins_idx - (int) gb->gap_ins_idx;
}

/*
 * Build per-segment allocation from sibling chains.
 *
 * Each live interval within each sibling becomes one XraSegment.
 * A sibling range with holes (multiple non-contiguous intervals) must
 * emit separate segments so that xra_reg_at_pos() returns -1 inside
 * holes where another vreg may legitimately share the same physical
 * register.  The old code emitted one [range_start, range_end) per
 * sibling, which reported false conflicts in the verifier and — more
 * critically — could confuse codegen into emitting gap moves for
 * positions where the value isn't live.
 */
static void build_result(LsCtx *ctx, XraResult *res) {
    res->valloc = xr_calloc(ctx->nvreg, sizeof(XraVRegAlloc));

    for (uint32_t v = 0; v < ctx->nvreg; v++) {
        LsRange *r = ctx->vreg_ranges[v];
        XraVRegAlloc *va = &res->valloc[v];
        va->spill = XRA_SPILL_NONE;

        if (!r)
            continue;

        // Count total intervals across all siblings
        uint16_t niv = 0;
        int16_t fspill = XRA_SPILL_NONE;
        for (LsRange *s = r; s; s = s->next_sibling) {
            for (LsInterval *iv = s->first_iv; iv; iv = iv->next)
                niv++;
            if (s->spill != XRA_SPILL_NONE && fspill == XRA_SPILL_NONE)
                fspill = s->spill;
            if (s->assigned < 0 && s->spill != XRA_SPILL_NONE)
                fspill = s->spill;
        }

        va->segs = xr_malloc(niv * sizeof(XraSegment));
        if (!va->segs) {
            va->nseg = 0;
            continue;
        }
        va->nseg = niv;

        // Fill one segment per interval
        uint16_t si = 0;
        for (LsRange *s = r; s; s = s->next_sibling) {
            for (LsInterval *iv = s->first_iv; iv; iv = iv->next) {
                XR_DCHECK(si < niv, "build_result: segment index overflow");
                va->segs[si].start = iv->start;
                va->segs[si].end = iv->end;
                va->segs[si].assigned = s->assigned;
                va->segs[si].is_fp = s->is_fp;
                si++;
            }
        }
        va->spill = fspill;
    }
}

/*
 * ConnectRanges: scan all sibling chains and generate gap moves
 * for mid-block split transitions where the register changes.
 * Block-boundary transitions are handled by existing edge copies.
 */
static void connect_ranges(LsCtx *ctx, XraResult *res) {
    uint32_t cap = 16;
    uint32_t n = 0;
    XraGapMove *moves = xr_malloc(cap * sizeof(XraGapMove));

    for (uint32_t v = 0; v < ctx->nvreg; v++) {
        LsRange *r = ctx->vreg_ranges[v];
        if (!r || !r->next_sibling)
            continue;

        for (LsRange *s = r; s; s = s->next_sibling) {
            LsRange *next = s->next_sibling;
            if (!next)
                break;

            int32_t s_end = range_end(s);
            int32_t n_start = range_start(next);
            if (s_end != n_start)
                continue;  // not adjacent at split point

            // Both spilled or same register: no move needed
            if (s->assigned == next->assigned)
                continue;
            if (s->assigned < 0 && next->assigned < 0)
                continue;

            // Check if split is at a block boundary (O(log n) via binary search)
            uint32_t bi = pos_to_blk(ctx, s_end);
            if (ctx->blk_start[bi] == s_end)
                continue;  // handled by edge copies

            // Mid-block split: generate gap move
            uint16_t ins_idx = pos_to_ins_idx(ctx, s_end);

            if (n >= cap) {
                cap *= 2;
                XR_REALLOC_OR_ABORT(moves, cap * sizeof(XraGapMove),
                                    "regalloc connect_ranges moves");
            }
            XraGapMove *gm = &moves[n++];
            gm->gap_blk = ctx->func->blocks[bi]->id;
            gm->gap_ins_idx = ins_idx;
            gm->vreg = (uint16_t) v;
            gm->src_reg = s->assigned;
            gm->dst_reg = next->assigned;
            gm->spill_slot = (s->assigned < 0)      ? s->spill
                             : (next->assigned < 0) ? next->spill
                                                    : XRA_SPILL_NONE;
            gm->is_fp = s->is_fp;
        }
    }

    if (n > 1)
        qsort(moves, n, sizeof(XraGapMove), gap_cmp);
    res->gap_moves = moves;
    res->ngap_move = n;
}

static uint32_t compute_callee_saved(LsCtx *ctx) {
    uint32_t cs = 0;
    for (uint32_t i = 0; i < ctx->nall; i++) {
        LsRange *r = ctx->all_ranges[i];
        if (r->assigned < 0)
            continue;
        if (!r->is_fp) {
            if (r->assigned >= 15)
                cs |= (1u << (r->assigned - 15));
        } else {
            if (r->assigned >= 8)
                cs |= (1u << (16 + r->assigned - 8));
        }
    }
    return cs;
}

static void compute_blk_masks(LsCtx *ctx, XraResult *res) {
    for (uint32_t bi = 0; bi < ctx->func->nblk; bi++) {
        uint32_t bid = ctx->func->blocks[bi]->id;
        if (bid >= res->nblk)
            continue;
        int32_t bs = ctx->blk_start[bi], be = ctx->blk_end[bi];
        uint32_t gp = 0, fp = 0, pt = 0;
        for (uint32_t ri = 0; ri < ctx->nall; ri++) {
            LsRange *r = ctx->all_ranges[ri];
            if (r->assigned < 0)
                continue;
            bool active = false;
            for (LsInterval *iv = r->first_iv; iv; iv = iv->next) {
                if (iv->start < be && iv->end > bs) {
                    active = true;
                    break;
                }
                if (iv->start >= be)
                    break;
            }
            if (!active)
                continue;
            int8_t reg = r->assigned;
            if (r->is_fp) {
                fp |= (1u << reg);
            } else {
                gp |= (1u << reg);
                if (r->vreg < ctx->func->nvreg && ctx->func->vregs[r->vreg].rep == XR_REP_PTR)
                    pt |= (1u << reg);
            }
        }
        res->blk_gp_live[bid] = gp;
        res->blk_fp_live[bid] = fp;
        res->blk_ptr_live[bid] = pt;
    }
}

/* ========== Main Entry ========== */

XraResult *xra_run(XirFunc *func) {
    if (!func || func->nblk == 0)
        return NULL;

    LsCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.func = func;
    ctx.nvreg = func->nvreg;

    pool_init(&ctx.pool);
    xir_live_compute(&ctx.live, func);

    ctx.blk_start = xr_calloc(func->nblk, sizeof(int32_t));
    ctx.blk_end = xr_calloc(func->nblk, sizeof(int32_t));
    number_pos(&ctx);

    // Build block ID → internal index mapping for O(1) lookup
    {
        uint32_t max_bid = 0;
        for (uint32_t i = 0; i < func->nblk; i++)
            if (func->blocks[i]->id > max_bid)
                max_bid = func->blocks[i]->id;
        ctx.blk_id_map_size = max_bid + 1;
        ctx.blk_id_to_idx = xr_calloc(ctx.blk_id_map_size, sizeof(uint32_t));
        for (uint32_t i = 0; i < func->nblk; i++)
            ctx.blk_id_to_idx[func->blocks[i]->id] = i;
    }

    compute_deferred(&ctx);

    ctx.vreg_ranges = xr_calloc(ctx.nvreg, sizeof(LsRange *));
    ctx.all_cap = ctx.nvreg * 2 + 64;
    ctx.all_ranges = xr_malloc(ctx.all_cap * sizeof(LsRange *));

    build_ranges(&ctx);
    build_fixed_intervals(&ctx);
    compute_bundles(&ctx);
    compute_hints(&ctx);
    ls_allocate(&ctx);

    uint32_t max_id = 0;
    for (uint32_t i = 0; i < func->nblk; i++)
        if (func->blocks[i]->id > max_id)
            max_id = func->blocks[i]->id;
    uint32_t msz = max_id + 1;

    XraResult *res = xr_calloc(1, sizeof(XraResult));
    if (!res)
        goto done;
    res->nvreg = ctx.nvreg;
    res->nblk = msz;

    // Export RA position mapping (indexed by block ID)
    res->blk_start = xr_calloc(msz, sizeof(int32_t));
    res->blk_end = xr_calloc(msz, sizeof(int32_t));
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        uint32_t bid = func->blocks[bi]->id;
        if (bid < msz) {
            res->blk_start[bid] = ctx.blk_start[bi];
            res->blk_end[bid] = ctx.blk_end[bi];
        }
    }

    res->blk_gp_live = xr_calloc(msz, sizeof(uint32_t));
    res->blk_fp_live = xr_calloc(msz, sizeof(uint32_t));
    res->blk_ptr_live = xr_calloc(msz, sizeof(uint32_t));

    build_result(&ctx, res);
    connect_ranges(&ctx, res);
    res->nspill = (uint32_t) ctx.next_spill;
    res->callee_saved = compute_callee_saved(&ctx);
    compute_blk_masks(&ctx, res);

    /* Spill-slot capacity check.
     *
     * Two independent upper bounds must hold:
     *   1. res->nspill <= XIR_MAX_SPILL_SLOTS:
     *      Slots beyond bit 31 cannot be reported to GC via the 32-bit
     *      spill_bitmap, so a PTR spilled there would be invisible during
     *      stack scanning and could lead to use-after-free.
     *   2. If the function contains any XIR_SUSPEND, also:
     *        res->nspill <= XIR_SUSPEND_SPILL_MAX
     *      because the suspend/resume path bridges old→new frame spill
     *      values through XrCoroutine::jit_suspend.spill[], whose
     *      capacity is fixed by the runtime struct layout.
     *
     * When either bound is exceeded we refuse to produce valid code for
     * this function; codegen will check had_error and abort compilation
     * with a diagnostic, letting the interpreter continue executing. */
    if (res->nspill > XIR_MAX_SPILL_SLOTS)
        res->had_error = true;
    if (func->nsuspend > 0 && res->nspill > XIR_SUSPEND_SPILL_MAX)
        res->had_error = true;

#ifndef NDEBUG
    /* V1: Regalloc overlap verifier.
     *
     * For each RA position, verify no two vregs are assigned the same
     * physical register. This catches the class of bugs where the
     * linear scan's Active/Inactive state machine fails to detect
     * conflicts (e.g. backward position movement after eviction split).
     *
     * Complexity: O(blocks * vregs²) but only in debug builds.
     * Typically < 1ms even for large functions. */
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        int32_t bs = ctx.blk_start[bi];
        int32_t be = ctx.blk_end[bi];
        // Check at block start and at each even position (use points)
        for (int32_t pos = bs; pos < be; pos += 2) {
            // Collect assigned GP and FP registers at this position
            for (uint32_t va = 0; va < ctx.nvreg; va++) {
                int8_t ra = xra_reg_at_pos(res, va, pos);
                if (ra < 0)
                    continue;
                bool fa = (func->vregs[va].rep == XR_REP_F64);
                for (uint32_t vb = va + 1; vb < ctx.nvreg; vb++) {
                    int8_t rb = xra_reg_at_pos(res, vb, pos);
                    if (rb < 0 || rb != ra)
                        continue;
                    bool fb = (func->vregs[vb].rep == XR_REP_F64);
                    if (fa != fb)
                        continue;  // GP vs FP — no conflict
                    /* Same-bundle ranges are intentionally allowed to share
                     * a register: phi_ranges_conflict verified no actual
                     * use conflict at overlapping positions. */
                    if (ctx.bundle_parent) {
                        uint32_t ba = bundle_find(ctx.bundle_parent, va);
                        uint32_t bb = bundle_find(ctx.bundle_parent, vb);
                        if (ba == bb)
                            continue;
                    }
                    fprintf(stderr,
                            "[REGALLOC VERIFY] %s: v%u and v%u both assigned "
                            "%s reg %d at pos %d (block %u)\n",
                            func->name ? func->name : "?", va, vb, fa ? "FP" : "GP", ra, pos, bi);
                    XR_DCHECK(false, "regalloc: overlapping register assignment");
                }
            }
        }
    }
#endif

done:
    pool_free(&ctx.pool);
    ls_list_free(&ctx.active);
    ls_list_free(&ctx.inactive);
    xr_free(ctx.bundle_parent);
    xr_free(ctx.bundle_hint);
    xr_free(ctx.bundle_spill);
    xr_free(ctx.blk_id_to_idx);
    xr_free(ctx.blk_start);
    xr_free(ctx.blk_end);
    xr_free(ctx.blk_deferred);
    xr_free(ctx.vreg_ranges);
    xr_free(ctx.all_ranges);
    xir_live_free(&ctx.live);
    return res;
}

void xra_result_free(XraResult *r) {
    if (!r)
        return;
    if (r->valloc) {
        for (uint32_t i = 0; i < r->nvreg; i++)
            xr_free(r->valloc[i].segs);
        xr_free(r->valloc);
    }
    xr_free(r->blk_start);
    xr_free(r->blk_end);
    xr_free(r->blk_gp_live);
    xr_free(r->blk_fp_live);
    xr_free(r->blk_ptr_live);
    xr_free(r->gap_moves);
    xr_free(r);
}

/* ========== Edge Copies (Phi + Split Transitions) ========== */

uint32_t xra_edge_copies(const XraResult *r, XirFunc *func, XirBlock *target, XirBlock *from,
                         XraEdgeCopy *out, uint32_t mx) {
    uint32_t n = 0;
    uint32_t from_bid = from->id;
    uint32_t to_bid = target->id;

    // Part 1: Phi resolution copies
    int pi = -1;
    for (uint32_t i = 0; i < target->npred; i++)
        if (target->preds[i] == from) {
            pi = (int) i;
            break;
        }

    if (pi >= 0) {
        for (XirPhi *p = target->phis; p; p = p->next) {
            if (!xir_ref_is_vreg(p->dst))
                continue;
            if ((uint32_t) pi >= p->narg)
                continue;
            XirRef sr = p->args[pi];
            if (xir_ref_is_none(sr) || !xir_ref_is_vreg(sr))
                continue;
            uint32_t dv = XIR_REF_INDEX(p->dst);
            uint32_t sv = XIR_REF_INDEX(sr);
            if (dv >= r->nvreg || sv >= r->nvreg)
                continue;
            /* Use per-block registers: dst at target start, src at from END.
             * Source must be queried at block end because the PHI arg vreg
             * may be defined mid-block (e.g. loop increment before back-edge);
             * at block start it would not be live yet → returns -1.
             *
             * DEFENSIVE CHECK: if source vreg has a register at block start
             * but NOT at block end, something is wrong with liveness/splitting.
             * This catches the original bug where blk_start was used instead
             * of blk_end, causing missing edge copies and infinite loops. */
            int8_t d = xra_vreg_reg_at(r, to_bid, dv);
            int8_t s = xra_vreg_reg_at_end(r, from_bid, sv);
#if XR_DEBUG
            {
                int8_t s_start = xra_vreg_reg_at(r, from_bid, sv);
                if (s < 0 && s_start >= 0 && d >= 0) {
                    fprintf(stderr,
                            "[JIT WARN] PHI edge copy: v%u has reg %d "
                            "at blk %u start but none at end (dst v%u reg %d "
                            "at blk %u). Possible liveness/split bug.\n",
                            sv, s_start, from_bid, dv, d, to_bid);
                }
            }
#endif
            if (d >= 0 && s >= 0 && d != s && n < mx) {
                bool fp = (p->rep == XR_REP_F64);
                out[n++] = (XraEdgeCopy){(uint8_t) d, (uint8_t) s, fp, false, 0};
            }
        }
    }

    // Part 2: Split transition copies (vreg changes register across edge)
    for (uint32_t v = 0; v < r->nvreg && n < mx; v++) {
        int8_t from_reg = xra_vreg_reg_at_end(r, from_bid, v);
        int8_t to_reg = xra_vreg_reg_at(r, to_bid, v);
        if (from_reg == to_reg)
            continue;
        if (from_reg < 0 && to_reg < 0)
            continue;

        // Skip vregs handled by Phi copies above
        bool is_phi = false;
        if (pi >= 0) {
            for (XirPhi *p = target->phis; p && !is_phi; p = p->next) {
                if (xir_ref_is_vreg(p->dst) && XIR_REF_INDEX(p->dst) == v)
                    is_phi = true;
            }
        }
        if (is_phi)
            continue;

        bool fp = (v < func->nvreg && func->vregs[v].rep == XR_REP_F64);

        if (from_reg >= 0 && to_reg >= 0) {
            out[n++] = (XraEdgeCopy){(uint8_t) to_reg, (uint8_t) from_reg, fp, false, 0};
        } else if (from_reg < 0 && to_reg >= 0) {
            int16_t slot = xra_vreg_spill(r, v);
            if (slot >= 0)
                out[n++] = (XraEdgeCopy){(uint8_t) to_reg, 0, fp, true, slot};
        }
    }

    return n;
}
