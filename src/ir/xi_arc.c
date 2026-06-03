/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_arc.c - Precise dup/drop (retain/release) insertion pass
 *
 * Consumes the ownership annotations produced by xi_own (backward
 * ownership inference) and inserts XI_RETAIN (dup) / XI_RELEASE (drop)
 * ops at precise points, following the Perceus owned/borrow model:
 *
 *   - Each RC value has a single owning reference (its definition).
 *   - A consuming use (store/return/throw/call-arg/aggregate element)
 *     takes ownership. The LAST consuming use moves the value out for
 *     free; every EARLIER consuming use needs a `dup` before it so the
 *     owner still has a reference to give away later.
 *   - A value that is never consumed (only borrowed, or dead) must be
 *     dropped after its last use (or right after its definition if dead).
 *
 * This pass MUST run BEFORE backend lowering, while ops are still
 * semantic (STORE_FIELD/ARRAY_NEW/...), because the owned/borrow split
 * is keyed on those ops. Stack/region values (XI_STACK_ALLOC, REGION)
 * carry frame lifetime and get no dup/drop.
 *
 * References: Koka Backend/C/Parc.hs, Roc mono/src/inc_dec.rs.
 */

#include "xi_arc.h"
#include "xi_own.h"
#include "xi_escape.h"
#include "xi_analysis.h"
#include "xi_core_api.h"
#include "../runtime/value/xtype.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

#include <string.h>

/* ========== Stack Alloc Rewrite (unchanged) ========== */

static void rewrite_to_stack(XiValue *v) {
    XR_DCHECK(v != NULL, "rewrite_to_stack: NULL value");
    XR_DCHECK(v->escape == XI_ESC_NONE, "rewrite_to_stack: not NO_ESCAPE");
    v->aux_int = (int32_t) v->op; /* save original op for codegen */
    v->op = XI_STACK_ALLOC;
}

XR_FUNC void xi_stack_alloc_rewrite(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_stack_alloc_rewrite: NULL func");

    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_stack_alloc_rewrite(f->children[i]);
    }

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            if (v->escape != XI_ESC_NONE)
                continue;
            if (!xi_own_type_is_rc(v->type))
                continue; /* scalars: no alloc */
            if (!xi_op_is_heap_alloc(v->op))
                continue;
            rewrite_to_stack(v);
        }
    }
}

/* ========== dup/drop Insertion ========== */

/* A consuming use site, in program order (block RPO, then index). */
typedef struct {
    XiBlock *blk;
    XiValue *user;  /* the value whose arg consumes the tracked value */
    uint32_t order; /* sort key: (rpo << 16) | index_in_block */
} ConsumeSite;

/* Does this op PRODUCE a fresh owning RC reference as its result?
 * Side-effecting ops (stores, print, assert, control) have no meaningful
 * RC result even if their static type is RC-ish, so they must not be
 * tracked for dup/drop (otherwise a phantom drop is inserted). */
static bool op_produces_owned_value(uint16_t op) {
    switch (op) {
        /* Stores / sets: side effects, no owning result. */
        case XI_STORE_FIELD:
        case XI_STRUCT_SET:
        case XI_INDEX_SET:
        case XI_JSON_INIT_F:
        case XI_JSON_SET_F:
        case XI_STORE_UPVAL:
        case XI_SET_SHARED:
        case XI_SET_GLOBAL:
        /* Consumers / effects with no usable RC result. */
        case XI_PRINT:
        case XI_ASSERT:
        case XI_ASSERT_EQ:
        case XI_ASSERT_NE:
        case XI_ASSERT_THROWS:
        case XI_THROW:
        case XI_CHAN_SEND:
        case XI_CHAN_TRY_SEND:
        case XI_GO:
        case XI_YIELD:
        case XI_DEFER:
        case XI_RETAIN:
        case XI_RELEASE:
        case XI_DROP_REUSE:
        case XI_MOVE:
        case XI_BOUNDS_CHECK:
            return false;
        default:
            return true;
    }
}

/* Does this op produce a BORROWED reference — i.e. a value loaded from a
 * location the current function does not own (shared/global/upvalue slot,
 * or a field/element of some other object)? Such a value must get neither
 * dup nor drop: the function only borrows it; the real owner is the slot or
 * the container. Dropping it would free an object still owned elsewhere
 * (use-after-free). Missing a dup at most leaks (safe), never UAFs. */
static bool op_produces_borrow(uint16_t op) {
    switch (op) {
        case XI_GET_SHARED:  /* read module-level shared slot */
        case XI_GET_GLOBAL:  /* read top-level global (REPL) */
        case XI_LOAD_UPVAL:  /* read captured upvalue */
        case XI_IMPORT_REF:  /* cross-module import reference */
        case XI_GET_BUILTIN: /* builtin global */
        case XI_LOAD_FIELD:  /* obj.field — owned by obj */
        case XI_STRUCT_GET:
        case XI_INDEX_GET: /* arr[i] / map[k] — owned by the container */
        case XI_JSON_GET_F:
        case XI_TUPLE_GET:
            return true;
        default:
            return false;
    }
}

/* Is this op a call whose result ownership we cannot determine without a
 * per-callee return-ownership summary? */
static bool op_is_call(uint16_t op) {
    switch (op) {
        case XI_CALL:
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
        case XI_TAIL_CALL:
        case XI_CALL_BUILTIN:
            return true;
        default:
            return false;
    }
}

/* Is this value a candidate for dup/drop? RC type, not a stack/region
 * alloc, not a scalar, produces an owning reference (not a borrow). */
static bool tracks_rc(const XiValue *v) {
    if (!v)
        return false;
    if (v->op == XI_STACK_ALLOC)
        return false; /* frame lifetime, no RC */
    if (v->op != XI_PARAM && !op_produces_owned_value(v->op))
        return false; /* side-effect op: no owning result */
    /* Call results: a callee may return either a fresh (+1) reference or an
     * alias of one of its arguments (e.g. `arr.push(x)` returns `self`).
     * Without per-callee return-ownership summaries we cannot tell them
     * apart. We DO track them (so a result consumed by more than one use
     * gets a dup before each non-last use, preventing a use-after-free when
     * the first consumer moves/frees it), but process_value_ex uses the
     * CALL_RESULT mode which never inserts an unconsumed drop — dropping an
     * aliased borrow would be a use-after-free. Adding dups is always sound;
     * the only cost is leaking a discarded fresh return until a per-callee
     * return-ownership summary (Roc/Koka style) refines this. */
    if (op_is_call(v->op))
        return xi_own_type_is_rc(v->type);
    if (v->escape == XI_ESC_NONE && xi_op_is_heap_alloc(v->op))
        return false; /* will be (or is) stack-allocated */
    return xi_own_type_is_rc(v->type);
}

/* Collect consuming uses of `target` across the function, in program
 * order. Returns count; fills sites[] up to cap. */
static int collect_consume_sites(XiFunc *f, XiValue *target, ConsumeSite *sites, int cap) {
    int n = 0;
    for (uint32_t b = 0; b < f->nblocks && n < cap; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues && n < cap; i++) {
            XiValue *user = blk->values[i];
            if (!user)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] != target)
                    continue;
                if (!xi_own_use_is_consuming(user->op, a))
                    continue;
                sites[n].blk = blk;
                sites[n].user = user;
                sites[n].order = (blk->rpo << 16) | (i & 0xFFFF);
                n++;
                break; /* one consume record per user is enough */
            }
        }
        /* Block control (return value) consumes the value. */
        if (blk->control == target && blk->kind == XI_BLOCK_RETURN && n < cap) {
            sites[n].blk = blk;
            sites[n].user = NULL; /* NULL = return terminator */
            sites[n].order = (blk->rpo << 16) | 0xFFFF;
            n++;
        }
    }
    return n;
}

static int cmp_site(const void *pa, const void *pb) {
    const ConsumeSite *a = pa, *b = pb;
    if (a->order < b->order)
        return -1;
    if (a->order > b->order)
        return 1;
    return 0;
}

/* Insert a XI_RETAIN(target) immediately before `user` in `blk`.
 * Since dup has no result dependency, placing it just before the user is
 * correct; we insert after the value preceding `user`. */
static void insert_dup_before(XiFunc *f, XiBlock *blk, XiValue *user, XiValue *target) {
    /* Find the slot before `user` to use as anchor (insert-after). */
    XiValue *anchor = NULL;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] == user) {
            anchor = (i > 0) ? blk->values[i - 1] : NULL;
            break;
        }
    }
    XiValue *dup = xi_value_insert_after(f, blk, anchor, XI_RETAIN, target->type, 1);
    XR_DCHECK(dup != NULL, "xi_arc: failed to create RETAIN");
    dup->args[0] = target;
    dup->flags = XI_FLAG_SIDE_EFFECT;
    dup->escape = target->escape;
}

static void insert_dup_after(XiFunc *f, XiBlock *blk, XiValue *anchor, XiValue *target) {
    XiValue *dup = xi_value_insert_after(f, blk, anchor, XI_RETAIN, target->type, 1);
    XR_DCHECK(dup != NULL, "xi_arc: failed to create RETAIN");
    dup->args[0] = target;
    dup->flags = XI_FLAG_SIDE_EFFECT;
    dup->escape = target->escape;
}

/* Insert a XI_RELEASE(target) right after `anchor` in `blk`. */
static void insert_drop_after(XiFunc *f, XiBlock *blk, XiValue *anchor, XiValue *target) {
    XiValue *drop = xi_value_insert_after(f, blk, anchor, XI_RELEASE, target->type, 1);
    XR_DCHECK(drop != NULL, "xi_arc: failed to create RELEASE");
    drop->args[0] = target;
    drop->flags = XI_FLAG_SIDE_EFFECT;
    drop->escape = target->escape;
}

/* Insert drops before the terminators of all RETURN blocks where the drop is
 * statically sound — used for a value that is owned but never consumed.
 *
 * The drop is emitted at a RETURN block only when the value's defining block
 * DOMINATES that return block. Otherwise the value's register is not
 * guaranteed live (the def is on a path that may not have executed), and the
 * drop would release a stale/borrowed value — the 1130_linked_scope crash,
 * where a dead ERR_CATCH value defined only in the catch block was dropped at
 * the common return, freeing the borrowed closure on the no-catch path. */
static void insert_drop_at_returns(XiFunc *f, XiValue *target) {
    XiBlock *def_blk = target->block;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk || blk->kind != XI_BLOCK_RETURN)
            continue;
        if (blk->control == target)
            continue; /* returned: caller takes ownership */
        /* Soundness: only drop where the definition dominates the return. */
        if (def_blk && !xi_dominates(def_blk, blk))
            continue;
        XiValue *last = blk->nvalues > 0 ? blk->values[blk->nvalues - 1] : NULL;
        insert_drop_after(f, blk, last, target);
    }
}

/* Process one tracked value: insert dup before every consuming use except
 * the last, and a drop if it is never consumed.
 *
 * `mode` selects the ownership convention:
 *   OWN_OWNED   — the function holds an owning reference (a fresh local
 *                 allocation, or an owned parameter). The last consuming use
 *                 moves it out (no dup); earlier consuming uses dup first; if
 *                 never consumed it is dropped at function exit.
 *   OWN_BORROWED — the value is owned by someone else (a method's `this`
 *                 receiver, operator operands, a captured-by-value the caller
 *                 keeps). EVERY consuming use dups first; never dropped.
 *   OWN_CALL_RESULT — a call result whose ownership we cannot statically
 *                 prove: it may be a fresh (+1) reference or an alias of an
 *                 argument (e.g. arr.push(x) returns self). We dup before
 *                 every consuming use EXCEPT the last (so a value consumed
 *                 more than once is not freed out from under a later use) but
 *                 we never insert an unconsumed drop (dropping an aliased
 *                 borrow would be a use-after-free). Adding dups only raises
 *                 the count, so this is sound for both fresh and aliased
 *                 returns; it can leak a discarded fresh return (safe).
 *                 A future per-callee return-ownership summary refines this. */
typedef enum {
    OWN_OWNED = 0,
    OWN_BORROWED,
    OWN_CALL_RESULT,
} XiArcOwnMode;

static void process_value_ex(XiFunc *f, XiValue *target, XiArcOwnMode mode) {
    enum {
        MAX_SITES = 256
    };
    ConsumeSite sites[MAX_SITES];
    int n = collect_consume_sites(f, target, sites, MAX_SITES);

    if (n == 0) {
        /* Never consumed. Only an OWNED value is dropped at exit. A borrowed
         * value is owned by the caller; a call result may be an alias — in
         * both cases we must not drop it here. */
        if (mode == OWN_OWNED)
            insert_drop_at_returns(f, target);
        return;
    }

    qsort(sites, (size_t) n, sizeof(ConsumeSite), cmp_site);

    if (mode == OWN_BORROWED) {
        /* Borrowed: the function owns no reference, so every consuming use
         * must dup first; nothing is moved out and nothing is dropped. */
        for (int i = 0; i < n; i++) {
            if (sites[i].user == NULL) {
                XiValue *last = sites[i].blk->nvalues > 0
                                    ? sites[i].blk->values[sites[i].blk->nvalues - 1]
                                    : NULL;
                insert_dup_after(f, sites[i].blk, last, target);
                continue;
            }
            insert_dup_before(f, sites[i].blk, sites[i].user, target);
        }
        return;
    }

    /* OWNED and CALL_RESULT share the same dup placement: the last consuming
     * use moves the value out (no dup), every earlier consuming use dups
     * first so the owner keeps a reference to pass on. They differ only in
     * the n==0 case above (CALL_RESULT never inserts an unconsumed drop). */
    for (int i = 0; i < n - 1; i++) {
        if (sites[i].user == NULL)
            continue; /* return terminator can only be the single last site */
        insert_dup_before(f, sites[i].blk, sites[i].user, target);
    }
}

/* Does the borrow signature prove parameter `pv` is only borrowed (never
 * stored/returned/forwarded)? Maps the param SSA value to its index via
 * f->params[], then reads own->sig.param_own[idx]. The signature is computed
 * by xi_own from real liveness + consume classification, fixed-pointed across
 * (mutually) recursive functions. Returns false when out of range or unproven
 * (conservative: treat as owned). */
static bool param_is_borrowed(const XiFunc *f, const XiValue *pv, const XiOwnResult *own) {
    if (!own || !own->sig.valid)
        return false;
    for (uint16_t p = 0; p < f->nparams && p < own->sig.nparams; p++) {
        if (f->params[p] == pv)
            return own->sig.param_own[p] == XI_OWN_BORROWED;
    }
    return false;
}

XR_FUNC void xi_arc_insert(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_arc_insert: NULL func");

    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_arc_insert(f->children[i]);
    }

    /* RPO + dominators are needed to order consume sites across blocks and to
     * gate drop placement (a drop at a return block is only sound where the
     * value's definition dominates it). */
    xi_ensure_rpo(f);
    xi_ensure_dominators(f);

    /* Borrow signature: which parameters does this function only borrow (never
     * store/return/forward)? A borrowed parameter must NOT be dropped by the
     * callee — the caller retains ownership (Perceus borrowed-parameter rule).
     * Without this, a function like `swap(arr, i, j)` that only indexes `arr`
     * would drop it at exit, freeing an array the caller still uses after the
     * call (1602_generic_container). This generalizes the ad-hoc
     * receiver_borrowed / operator_borrowed flags to every parameter. */
    XiOwnResult own;
    bool have_own = xi_own_analyze(f, &own);

    /* Snapshot the set of tracked values first: we mutate blocks while
     * inserting, so collect targets up front. */
    enum {
        MAX_TARGETS = 4096
    };
    XiValue *targets[MAX_TARGETS];
    int nt = 0;

    for (uint16_t p = 0; p < f->nparams && nt < MAX_TARGETS; p++) {
        if (f->params[p] && tracks_rc(f->params[p]))
            targets[nt++] = f->params[p];
    }
    for (uint32_t b = 0; b < f->nblocks && nt < MAX_TARGETS; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues && nt < MAX_TARGETS; i++) {
            XiValue *v = blk->values[i];
            if (v && tracks_rc(v))
                targets[nt++] = v;
        }
    }

    /* The borrowed method receiver (params[0] when receiver_borrowed) is
     * passed without a caller dup, so it must be processed under the
     * borrowed convention: dup at consuming uses, never drop. Operator
     * methods receive ALL their params borrowed (the VM operator dispatch
     * leaves operands live in the caller's registers). */
    XiValue *borrowed_recv = (f->receiver_borrowed && f->nparams > 0) ? f->params[0] : NULL;

    for (int i = 0; i < nt; i++) {
        XiArcOwnMode mode = OWN_OWNED;
        if (targets[i] == borrowed_recv) {
            mode = OWN_BORROWED;
        } else if (f->operator_borrowed && targets[i]->op == XI_PARAM) {
            mode = OWN_BORROWED;
        } else if (targets[i]->op == XI_PARAM && have_own &&
                   param_is_borrowed(f, targets[i], &own)) {
            /* The borrow signature proved this parameter is only borrowed:
             * dup at consuming uses, never drop (caller keeps ownership). */
            mode = OWN_BORROWED;
        } else if (op_produces_borrow(targets[i]->op)) {
            mode = OWN_BORROWED;
        } else if (op_is_call(targets[i]->op)) {
            mode = OWN_CALL_RESULT;
        }
        process_value_ex(f, targets[i], mode);
    }

    if (have_own)
        xi_own_free(&own);
}
