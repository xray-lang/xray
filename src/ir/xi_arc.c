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
 *   - A value that is never consumed (only borrowed, or dead) is dropped
 *     at its death point, found by backward liveness: after the last use
 *     in the block where it dies, or on the CFG edge where it goes dead
 *     (see insert_drops_at_death). Loop-body temporaries therefore die
 *     once per iteration, not at function exit.
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
#include "xi_effect.h"
#include "xi_analysis.h"
#include "xi_cfg_edit.h"
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
    v->flags |= XI_FLAG_SIDE_EFFECT;
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

/* A consuming use site, in program order (block RPO, then index).
 *
 * For a phi use, the consume happens on the CFG edge preds[i] → join, so the
 * site is attributed to the PREDECESSOR block (blk = pred, user = the phi):
 * a dup for a non-last consume must execute only on that incoming path, and
 * the move for a last consume conceptually happens as control leaves the
 * predecessor. */
typedef struct {
    XiBlock *blk;
    XiValue *user;  /* the value whose arg consumes the tracked value */
    uint32_t order; /* sort key: (rpo << 16) | index_in_block */
} ConsumeSite;

/* Side-effecting ops can have RC-ish static types while carrying no usable
 * result. Borrowed loads and unknown call results need distinct ARC modes. */
static uint8_t op_result_ownership(uint16_t op) {
    return xi_op_result_ownership(op);
}

static bool op_has_trackable_result(uint16_t op) {
    return op_result_ownership(op) != XI_GEN_RESULT_OWNERSHIP_NONE;
}

/* A borrowed reference is loaded from a location owned by another object or
 * module slot. Dropping it would free storage still owned elsewhere. */
static bool op_produces_borrow(uint16_t op) {
    return op_result_ownership(op) == XI_GEN_RESULT_OWNERSHIP_BORROWED;
}

/* Is this op a call whose result ownership we cannot determine without a
 * per-callee return-ownership summary? */
static bool op_is_call(uint16_t op) {
    return op_result_ownership(op) == XI_GEN_RESULT_OWNERSHIP_CALL_RESULT;
}

/* ========== Per-callee return-ownership ==========
 *
 * A call result can be promoted from CALL_RESULT to OWNED when the callee
 * is statically known to return a FRESH (+1) reference that aliases none
 * of its arguments. CALL_RESULT mode never inserts an unconsumed drop
 * (the result might alias an argument), so every discarded fresh return
 * leaks; OWNED mode lets the death-point placement reclaim it.
 *
 * Channel receive/try methods materialize a fresh wrapper per call
 * (Recv<T> / SendResult enum instance plus the payload copied into the
 * receiving coroutine's heap). Receive loops would otherwise leak one
 * wrapper per message. Extend this table as more native return-ownership
 * facts are encoded. */
static bool call_returns_fresh(const XiValue *v) {
    if (v->op != XI_CALL_METHOD || v->nargs < 1 || !v->args[0])
        return false;
    const struct XrType *recv_type = v->args[0]->type;
    if (!recv_type || recv_type->kind != XR_KIND_CHANNEL)
        return false;
    const char *method = (const char *) v->aux;
    if (!method)
        return false;
    return strcmp(method, "recv") == 0 || strcmp(method, "tryRecv") == 0 ||
           strcmp(method, "recvTimeout") == 0 || strcmp(method, "trySend") == 0 ||
           strcmp(method, "sendTimeout") == 0;
}

/* Is this value a candidate for dup/drop? RC type, not a stack/region
 * alloc, not a scalar, produces an owning reference (not a borrow). */
static bool tracks_rc(const XiValue *v) {
    if (!v)
        return false;
    if (v->op == XI_STACK_ALLOC)
        return false; /* frame lifetime, no RC */
    if (v->op != XI_PARAM && !op_has_trackable_result(v->op))
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
        /* Phi uses consume the incoming value on its edge. Phis live on
         * blk->phis (not in blk->values[]), so scan them explicitly. The
         * same value may flow in on several edges; each edge is an
         * independent consume site (no dedup). */
        for (XiPhi *phi = blk->phis; phi && n < cap; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs && n < cap; a++) {
                if (phi->value.args[a] != target)
                    continue;
                XiBlock *pred = (a < blk->npreds) ? blk->preds[a] : NULL;
                if (!pred)
                    continue;
                sites[n].blk = pred;
                sites[n].user = &phi->value;
                /* 0xFFFE = end of the predecessor block: after every value
                 * index, before a return terminator's 0xFFFF. */
                sites[n].order = (pred->rpo << 16) | 0xFFFE;
                n++;
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

/* ========== Death-point drop placement (unconsumed owned values) ==========
 *
 * A value that is owned but never consumed must be released exactly once on
 * every path that executes its definition. Releasing at function returns is
 * wrong twice over: a definition inside a loop body or branch arm does not
 * dominate the returns (a dominance-gated return drop then silently skips
 * it, leaking one object per loop iteration), and even where it would be
 * sound it pins the object far past its last use.
 *
 * Placement is classic backward liveness over the CFG:
 *
 *   live_out(B) = OR over successors S of live_in(S)
 *   live_in(B)  = has_use(B) || live_out(B)        for B != def block
 *   live_in(def block) = false                      (the def kills upward)
 *
 * SSA guarantees every use is dominated by the def, and the def-block kill
 * stops backward propagation there, so the live region stays inside the
 * def's dominance region: a drop can never execute before the definition.
 *
 * The live→dead frontier is crossed exactly once per execution (liveness is
 * monotone along any path), and a release is placed on each frontier point:
 *
 *   - death in block: live at entry (or the def block) but dead at exit →
 *     release right after the last use in that block (right after the def
 *     itself when the value is never used).
 *   - death on edge: B is live-out only because of another successor →
 *     release at the head of the dead successor when this edge is its only
 *     entry; otherwise the edge is split so the release cannot execute on
 *     paths that never ran the definition. */

typedef struct {
    uint8_t has_use;    /* target is used by a value in this block */
    uint8_t live_in;    /* target live at block entry */
    uint8_t live_out;   /* target live at block exit */
    uint8_t use_at_end; /* last use is the block terminator (control) */
    uint32_t last_use;  /* index in blk->values of the last use */
} ArcLive;

/* Insert a XI_RELEASE(target) as the first instruction of `blk`. Needed for
 * edge deaths, where the release must run before anything else in the dead
 * successor. */
static void insert_drop_at_head(XiFunc *f, XiBlock *blk, XiValue *target) {
    XiValue *drop = xi_value_insert_after(f, blk, NULL, XI_RELEASE, target->type, 1);
    XR_DCHECK(drop != NULL, "xi_arc: failed to create RELEASE");
    drop->args[0] = target;
    drop->flags = XI_FLAG_SIDE_EFFECT;
    drop->escape = target->escape;
    /* xi_value_insert_after(anchor=NULL) appends; rotate it to the front. */
    for (uint32_t i = blk->nvalues - 1; i > 0; i--)
        blk->values[i] = blk->values[i - 1];
    blk->values[0] = drop;
}

/* Split the CFG edge pred→succ with a fresh PLAIN block. Phi args in succ
 * keep their positions (the pred slot is replaced in place). Returns the new
 * block, or NULL on failure. */
static XiBlock *arc_split_edge(XiFunc *f, XiBlock *pred, XiBlock *succ) {
    XiBlock *mid = xi_block_new(f);
    if (!mid)
        return NULL;
    mid->kind = XI_BLOCK_PLAIN;
    mid->succs[0] = succ;
    bool ok = xi_cfg_replace_successor(pred, succ, mid);
    ok = ok && xi_cfg_replace_pred(succ, pred, mid);
    ok = ok && xi_cfg_append_pred(mid, pred, NULL, 0);
    XR_DCHECK(ok, "xi_arc: edge split failed");
    return ok ? mid : NULL;
}

/* Collect `target` plus the transitive closure of its RC-typed borrowed
 * projections (GETFIELD and friends). A borrow reads through the owner
 * without holding a reference, so the owner must outlive the projection's
 * last use — otherwise the release would free storage a live borrow still
 * points into. Returns a heap array (caller frees) with the count in
 * *out_count, or NULL on OOM. */
static XiValue **arc_collect_borrow_closure(XiFunc *f, XiValue *target, uint32_t *out_count) {
    enum {
        ARC_TRACK_INIT = 16
    };
    XiValue **tracked = (XiValue **) xr_malloc(ARC_TRACK_INIT * sizeof(XiValue *));
    if (!tracked)
        return NULL;
    uint32_t ntracked = 0, track_cap = ARC_TRACK_INIT;
    tracked[ntracked++] = target;
    for (uint32_t scan = 0; scan < ntracked; scan++) {
        XiValue *member = tracked[scan];
        for (uint32_t b = 0; b < f->nblocks; b++) {
            XiBlock *blk = f->blocks[b];
            if (!blk)
                continue;
            for (uint32_t i = 0; i < blk->nvalues; i++) {
                XiValue *u = blk->values[i];
                if (!u || !op_produces_borrow(u->op) || !xi_own_type_is_rc(u->type))
                    continue;
                bool uses_member = false;
                for (uint16_t a = 0; a < u->nargs; a++) {
                    if (u->args[a] == member) {
                        uses_member = true;
                        break;
                    }
                }
                if (!uses_member)
                    continue;
                bool seen = false;
                for (uint32_t t = 0; t < ntracked; t++) {
                    if (tracked[t] == u) {
                        seen = true;
                        break;
                    }
                }
                if (seen)
                    continue;
                if (ntracked == track_cap) {
                    uint32_t new_cap = track_cap * 2;
                    XiValue **grown = (XiValue **) xr_realloc(tracked, new_cap * sizeof(XiValue *));
                    if (!grown) {
                        xr_free(tracked);
                        return NULL;
                    }
                    tracked = grown;
                    track_cap = new_cap;
                }
                tracked[ntracked++] = u;
            }
        }
    }
    *out_count = ntracked;
    return tracked;
}

/* Seed per-block use flags for the tracked set (owner + borrowed
 * projections). A projection consumed by a phi extends the owner's live
 * range to the predecessor's end; a block terminator use is a use at
 * end-of-block. */
static void arc_seed_uses(XiFunc *f, XiValue **tracked, uint32_t ntracked, ArcLive *live,
                          const uint32_t *pos_by_id) {
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        ArcLive *li = &live[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *u = blk->values[i];
            if (!u || u->op == XI_RETAIN || u->op == XI_RELEASE)
                continue;
            bool uses_tracked = false;
            for (uint16_t a = 0; a < u->nargs && !uses_tracked; a++) {
                for (uint32_t t = 0; t < ntracked; t++) {
                    if (u->args[a] == tracked[t]) {
                        uses_tracked = true;
                        break;
                    }
                }
            }
            if (uses_tracked) {
                li->has_use = 1;
                li->last_use = i; /* ascending i: ends at the latest use */
            }
        }
        /* Phi consumes of a projection happen on the incoming edge: treat
         * as a use at the end of the predecessor block. */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                XiBlock *pred = (a < blk->npreds) ? blk->preds[a] : NULL;
                if (!pred || !pos_by_id[pred->id])
                    continue;
                for (uint32_t t = 0; t < ntracked; t++) {
                    if (phi->value.args[a] == tracked[t]) {
                        ArcLive *pl = &live[pos_by_id[pred->id] - 1];
                        pl->has_use = 1;
                        pl->use_at_end = 1;
                        break;
                    }
                }
            }
        }
        for (uint32_t t = 0; t < ntracked; t++) {
            if (blk->control == tracked[t]) {
                li->has_use = 1;
                li->use_at_end = 1;
                break;
            }
        }
    }
}

/* Place releases on the live→dead frontier for `target`. Returns true if a
 * CFG edge was split (caller must invalidate analyses). */
static bool arc_place_frontier_drops(XiFunc *f, XiValue *target, const ArcLive *live,
                                     const uint32_t *pos_by_id, const XiBlock *def_blk) {
    bool split_any = false;
    uint32_t def_pos = pos_by_id[def_blk->id] ? pos_by_id[def_blk->id] - 1 : 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        const ArcLive *li = &live[b];
        bool holds_value = li->live_in || b == def_pos;
        if (!holds_value)
            continue;

        if (!li->live_out) {
            /* Death in block: release after the last use (after the def
             * itself when the block never uses the value). */
            XiValue *anchor;
            if (li->has_use)
                anchor = li->use_at_end ? (blk->nvalues > 0 ? blk->values[blk->nvalues - 1] : NULL)
                                        : blk->values[li->last_use];
            else if (b == def_pos && target->op != XI_PARAM)
                anchor = target;
            else {
                /* Unused owned parameter: dead on entry. */
                insert_drop_at_head(f, blk, target);
                continue;
            }
            insert_drop_after(f, blk, anchor, target);
            continue;
        }

        /* Death on edge: live out of this block, but a successor is dead. */
        for (int s = 0; s < 2; s++) {
            XiBlock *sb = blk->succs[s];
            if (!sb || !pos_by_id[sb->id])
                continue;
            if (live[pos_by_id[sb->id] - 1].live_in)
                continue;
            if (sb->npreds == 1) {
                insert_drop_at_head(f, sb, target);
            } else {
                XiBlock *mid = arc_split_edge(f, blk, sb);
                if (mid) {
                    insert_drop_after(f, mid, NULL, target);
                    split_any = true;
                }
            }
        }
    }
    return split_any;
}

/* Compute backward liveness of `target` (plus its RC-typed borrowed-projection
 * closure) over the CFG. On success allocates *out_live (one ArcLive per block
 * position) and *out_pos_by_id (block-id -> position+1, 0 = absent); the caller
 * frees both. The def block's live_in is forced false (the definition kills
 * everything above it), bounding the live region inside the def's dominance
 * region. Returns false on OOM. */
static bool arc_compute_liveness(XiFunc *f, XiValue *target, ArcLive **out_live,
                                 uint32_t **out_pos_by_id) {
    XiBlock *def_blk = target->block ? target->block : f->entry;
    if (!def_blk)
        return false;

    /* Dense block-id → position map (ids survive block compaction, so the
     * id space can be larger than nblocks). */
    uint32_t max_id = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        if (f->blocks[b] && f->blocks[b]->id > max_id)
            max_id = f->blocks[b]->id;
    }
    uint32_t *pos_by_id = (uint32_t *) xr_calloc(max_id + 1, sizeof(uint32_t));
    ArcLive *live = (ArcLive *) xr_calloc(f->nblocks, sizeof(ArcLive));
    if (!pos_by_id || !live) {
        if (pos_by_id)
            xr_free(pos_by_id);
        if (live)
            xr_free(live);
        return false;
    }
    for (uint32_t b = 0; b < f->nblocks; b++) {
        if (f->blocks[b])
            pos_by_id[f->blocks[b]->id] = b + 1; /* 0 = absent */
    }

    uint32_t ntracked = 0;
    XiValue **tracked = arc_collect_borrow_closure(f, target, &ntracked);
    if (!tracked) {
        /* OOM: skip placement for this value (leak, never a wrong drop). */
        xr_free(pos_by_id);
        xr_free(live);
        return false;
    }
    arc_seed_uses(f, tracked, ntracked, live, pos_by_id);
    xr_free(tracked);

    /* Backward liveness to fixpoint. The def block's live_in is forced
     * false: the definition kills everything above it, which both bounds
     * the live region and makes per-iteration loop deaths land inside the
     * loop body instead of leaking across the back edge. */
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t b = f->nblocks; b-- > 0;) {
            XiBlock *blk = f->blocks[b];
            if (!blk)
                continue;
            bool out = false;
            for (int s = 0; s < 2 && !out; s++) {
                XiBlock *sb = blk->succs[s];
                if (sb && pos_by_id[sb->id])
                    out = live[pos_by_id[sb->id] - 1].live_in;
            }
            bool in = (blk == def_blk) ? false : (live[b].has_use || out);
            if (out != (bool) live[b].live_out || in != (bool) live[b].live_in) {
                live[b].live_out = out;
                live[b].live_in = in;
                changed = true;
            }
        }
    }

    *out_live = live;
    *out_pos_by_id = pos_by_id;
    return true;
}

/* Place releases for an owned value with no consuming use. Returns true if
 * an edge was split (CFG analyses must be invalidated by the caller). */
static bool insert_drops_at_death(XiFunc *f, XiValue *target) {
    XiBlock *def_blk = target->block ? target->block : f->entry;
    if (!def_blk)
        return false;
    ArcLive *live = NULL;
    uint32_t *pos_by_id = NULL;
    if (!arc_compute_liveness(f, target, &live, &pos_by_id))
        return false;
    bool split_any = arc_place_frontier_drops(f, target, live, pos_by_id, def_blk);
    xr_free(pos_by_id);
    xr_free(live);
    return split_any;
}

/* Is `target` still live immediately after the consume at `site`? A consuming
 * use is a MOVE (no dup) only when the value is dead right after it on every
 * path; otherwise the owner still needs a reference afterwards (a later use in
 * the same block, or a use in a successor) so the consume must dup first.
 *
 * Deciding by liveness rather than static program order is what makes mutually
 * exclusive branch arms each MOVE: a consume in one arm is not "live after"
 * just because the other arm also consumes — the arms are not on a common path.
 * `live`/`pos_by_id` come from arc_compute_liveness (uses = consume + borrow
 * closure), so "live after" correctly accounts for borrows-after-consume too. */
static bool consume_is_live_after(const ConsumeSite *site, const ArcLive *live,
                                  const uint32_t *pos_by_id) {
    if (!site->blk || !pos_by_id[site->blk->id])
        return true; /* unknown: conservatively dup (never a wrong move) */
    const ArcLive *li = &live[pos_by_id[site->blk->id] - 1];
    uint32_t idx = site->order & 0xFFFF;
    /* Phi-edge (0xFFFE) and return (0xFFFF) consumes sit at block end: the
     * value is only still live afterwards if a successor uses it. */
    if (idx >= 0xFFFE)
        return li->live_out;
    /* Regular consume at index idx: live after iff a later use exists in this
     * block, or the value escapes into a successor. */
    return (li->has_use && li->last_use > idx) || li->live_out;
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

static bool process_value_ex(XiFunc *f, XiValue *target, XiArcOwnMode mode) {
    enum {
        MAX_SITES = 256
    };
    ConsumeSite sites[MAX_SITES];
    int n = collect_consume_sites(f, target, sites, MAX_SITES);

    if (n == 0) {
        /* Never consumed. Only an OWNED value is dropped (at its death
         * point). A borrowed value is owned by the caller; a call result
         * may be an alias — in both cases we must not drop it here. */
        if (mode == OWN_OWNED)
            return insert_drops_at_death(f, target);
        return false;
    }

    qsort(sites, (size_t) n, sizeof(ConsumeSite), cmp_site);

    if (mode == OWN_BORROWED) {
        /* Borrowed: the function owns no reference, so every consuming use
         * must dup first; nothing is moved out and nothing is dropped. */
        for (int i = 0; i < n; i++) {
            if (sites[i].user == NULL || sites[i].user->op == XI_PHI) {
                /* Return terminator, or a phi edge consume (site->blk is the
                 * predecessor): dup at the end of that block. */
                XiValue *last = sites[i].blk->nvalues > 0
                                    ? sites[i].blk->values[sites[i].blk->nvalues - 1]
                                    : NULL;
                insert_dup_after(f, sites[i].blk, last, target);
                continue;
            }
            insert_dup_before(f, sites[i].blk, sites[i].user, target);
        }
        return false;
    }

    if (mode == OWN_CALL_RESULT) {
        /* Alias-uncertain call result: dup before every consuming use except
         * the last (program order), never drop. We cannot prove the result is
         * a fresh +1 vs an alias of an argument, so the conservative
         * count-raising placement stays — turning a non-live-after consume into
         * a move could transfer the only reference of an aliased argument. */
        for (int i = 0; i < n - 1; i++) {
            if (sites[i].user == NULL)
                continue; /* return terminator can only be the single last site */
            if (sites[i].user->op == XI_PHI) {
                XiValue *last = sites[i].blk->nvalues > 0
                                    ? sites[i].blk->values[sites[i].blk->nvalues - 1]
                                    : NULL;
                insert_dup_after(f, sites[i].blk, last, target);
                continue;
            }
            insert_dup_before(f, sites[i].blk, sites[i].user, target);
        }
        return false;
    }

    /* OWNED: a consuming use moves the owned reference out only when the value
     * is dead immediately after it (last use on every path through it);
     * otherwise the owner still needs a reference afterwards, so dup first.
     * The decision is by liveness, not static program order, so two mutually
     * exclusive branch arms that each consume the value both MOVE — the old
     * "dup all but the last in program order" wrongly dup'd the earlier arm,
     * leaking that reference whenever that arm ran (it was consumed without the
     * value ever being live afterwards). */
    ArcLive *live = NULL;
    uint32_t *pos_by_id = NULL;
    bool have_live = arc_compute_liveness(f, target, &live, &pos_by_id);
    for (int i = 0; i < n; i++) {
        /* Without liveness (OOM) fall back to the safe count-raising rule: dup
         * every consuming use except the last in program order — never a wrong
         * move. With liveness, dup only when the value is live after the use. */
        bool need_dup = have_live ? consume_is_live_after(&sites[i], live, pos_by_id) : (i < n - 1);
        if (!need_dup)
            continue; /* MOVE: the consume transfers the owned reference */
        if (sites[i].user == NULL)
            continue; /* return terminator: last use, never live after */
        if (sites[i].user->op == XI_PHI) {
            /* Edge consume: dup at the end of the predecessor block so the
             * +1 executes only on this incoming path. */
            XiValue *last =
                sites[i].blk->nvalues > 0 ? sites[i].blk->values[sites[i].blk->nvalues - 1] : NULL;
            insert_dup_after(f, sites[i].blk, last, target);
            continue;
        }
        insert_dup_before(f, sites[i].blk, sites[i].user, target);
    }
    if (have_live) {
        xr_free(pos_by_id);
        xr_free(live);
    }
    return false;
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

    bool cfg_changed = false;
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
            /* Known fresh-returning callees produce an owned reference;
             * everything else stays in the alias-safe CALL_RESULT mode. */
            mode = call_returns_fresh(targets[i]) ? OWN_OWNED : OWN_CALL_RESULT;
        }
        cfg_changed |= process_value_ex(f, targets[i], mode);
    }

    if (cfg_changed)
        xi_cfg_invalidate(f);

    if (have_own)
        xi_own_free(&own);
}

/* ========== Dup/Drop Elimination (copy→move optimization) ========== */

/* Remove value at index 'idx' from block, shifting subsequent values down. */
static void arc_remove_value(XiBlock *blk, uint32_t idx) {
    XR_DCHECK(blk != NULL, "arc_remove_value: NULL block");
    XR_DCHECK(idx < blk->nvalues, "arc_remove_value: index out of bounds");
    for (uint32_t j = idx; j + 1 < blk->nvalues; j++)
        blk->values[j] = blk->values[j + 1];
    blk->nvalues--;
}

/* Count how many times value `v` is used as an arg across the entire function,
 * excluding uses in XI_RETAIN and XI_RELEASE ops (those are the RC machinery
 * we are trying to eliminate). */
static int count_real_uses(const XiFunc *f, const XiValue *v) {
    int count = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        const XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *user = blk->values[i];
            if (!user)
                continue;
            if (user->op == XI_RETAIN || user->op == XI_RELEASE)
                continue;
            for (uint16_t a = 0; a < user->nargs; a++) {
                if (user->args[a] == v)
                    count++;
            }
        }
        /* Phi uses (phis live on blk->phis, not in blk->values[]). */
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == v)
                    count++;
            }
        }
        /* Return terminator */
        if (blk->control == v)
            count++;
    }
    return count;
}

/* Single-block dup/drop pair elimination.
 *
 * Pattern 1 — "Redundant bracket":
 *   XI_RETAIN(v) at position i
 *   XI_RELEASE(v) at position j  (j > i)
 *   v is NOT used by any instruction between i and j (exclusive)
 *   → The pair is dead: the retain increments RC only for the release to
 *     decrement it back. Remove both.
 *
 * Pattern 2 — "Single-consumer forward":
 *   XI_RETAIN(v) at position i
 *   A single consuming use of v at position k (i < k)
 *   XI_RELEASE(v) does NOT exist elsewhere (no double-drop)
 *   The value v has exactly 1 real (non-RC) use in the function
 *   → The retain is redundant because there is only one consumer: it already
 *     receives ownership via the last-use move rule. Remove the retain.
 *
 * We apply Pattern 1 iteratively (removing a pair may expose new pairs). */
static int elim_block(XiBlock *blk, const XiFunc *f) {
    int eliminated = 0;
    bool changed = true;

    while (changed) {
        changed = false;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *dup = blk->values[i];
            if (!dup || dup->op != XI_RETAIN)
                continue;
            XR_DCHECK(dup->nargs >= 1 && dup->args[0] != NULL, "arc_elim: malformed RETAIN");
            XiValue *target = dup->args[0];

            /* Pattern 1: look for a matching XI_RELEASE(target) in the same
             * block with no intervening use of target. */
            for (uint32_t j = i + 1; j < blk->nvalues; j++) {
                XiValue *drop = blk->values[j];
                if (!drop)
                    continue;
                /* If target is used between i and j, the retain is needed. */
                if (drop->op != XI_RELEASE) {
                    /* Check if this instruction uses target. */
                    bool uses_target = false;
                    for (uint16_t a = 0; a < drop->nargs; a++) {
                        if (drop->args[a] == target) {
                            uses_target = true;
                            break;
                        }
                    }
                    if (uses_target)
                        break; /* target is consumed between dup/drop — keep */
                    continue;
                }
                /* Found a RELEASE. Check if it drops the same target. */
                if (drop->nargs < 1 || drop->args[0] != target)
                    continue;
                /* Match! Remove both the RETAIN (at i) and RELEASE (at j).
                 * Remove j first (higher index) to keep i valid. */
                arc_remove_value(blk, j);
                arc_remove_value(blk, i);
                eliminated++;
                changed = true;
                break;
            }
            if (changed)
                break; /* restart scan from the beginning */
        }
    }

    /* Pattern 2: single-consumer retain elimination.
     * After removing redundant bracket pairs, check remaining RETAINs:
     * if the target value has exactly 1 real use (excluding RC ops) in the
     * function, the retain is unnecessary — ownership flows directly. */
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *dup = blk->values[i];
        if (!dup || dup->op != XI_RETAIN)
            continue;
        XiValue *target = dup->args[0];
        if (!target)
            continue;

        int real_uses = count_real_uses(f, target);
        if (real_uses <= 1) {
            /* The value flows to at most one real consumer; the retain is
             * dead weight because xi_arc already handles single-consumer
             * ownership transfer via the last-use move rule. */
            arc_remove_value(blk, i);
            eliminated++;
            i--; /* re-examine the slot */
        }
    }

    return eliminated;
}

XR_FUNC int xi_arc_elim(XiFunc *f) {
    if (!f)
        return 0;

    int total = 0;

    /* Process children first (bottom-up). */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            total += xi_arc_elim(f->children[i]);
    }

    /* Eliminate within each block. */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk || blk->nvalues == 0)
            continue;
        total += elim_block(blk, f);
    }

    return total;
}
