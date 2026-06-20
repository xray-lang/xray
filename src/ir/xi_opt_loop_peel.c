/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_loop_peel.c - Loop peeling for Xi IR
 *
 * Peels the first iteration of a loop into straight-line code before
 * the loop header.  After peeling, the loop body starts from the
 * second iteration, which enables downstream passes (BCE, LICM,
 * guard motion) to remove first-iteration-only checks from the hot
 * loop body.
 *
 * The pattern handled is:
 *
 *   preheader → header → body → latch → header
 *                   ↓
 *                 exit
 *
 * After peeling:
 *
 *   preheader → peeled_header (first iter check)
 *                   ↓ true              ↓ false
 *               peeled_body          exit
 *                   ↓
 *               header → body → latch → header
 *                   ↓
 *                 exit
 *
 * Only loops with:
 *   - A single latch (back-edge source)
 *   - A unique preheader
 *   - An IF-terminated header (condition check at top)
 *   - Small body (total values <= threshold)
 * are eligible for peeling.
 */

#include "xi_opt_loop_peel.h"
#include "xi_cfg_edit.h"
#include "xi_loop.h"
#include "xi_analysis.h"
#include "../base/xchecks.h"

#define PEEL_MAX_BODY_VALUES 30

/* ========== Value Cloning Map ========== */

typedef struct PeelMap {
    XiValue **old_vals;
    XiValue **new_vals;
    uint32_t count;
    uint32_t cap;
} PeelMap;

static bool peel_map_init(XiFunc *f, PeelMap *m, uint32_t cap) {
    m->old_vals = NULL;
    m->new_vals = NULL;
    m->count = 0;
    m->cap = cap;
    if (cap == 0)
        return true;
    m->old_vals = (XiValue **) xi_func_arena_alloc(f, cap * sizeof(XiValue *));
    m->new_vals = (XiValue **) xi_func_arena_alloc(f, cap * sizeof(XiValue *));
    return m->old_vals != NULL && m->new_vals != NULL;
}

static bool peel_map_add(PeelMap *m, XiValue *old_val, XiValue *new_val) {
    if (m->count >= m->cap)
        return false;
    m->old_vals[m->count] = old_val;
    m->new_vals[m->count] = new_val;
    m->count++;
    return true;
}

static XiValue *peel_map_find(const PeelMap *m, const XiValue *old_val) {
    if (!m || !old_val)
        return NULL;
    for (uint32_t i = 0; i < m->count; i++) {
        if (m->old_vals[i] == old_val)
            return m->new_vals[i];
    }
    return NULL;
}

/* ========== Eligibility ========== */

static uint32_t count_loop_values(const XiLoop *loop) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < loop->nbody; i++) {
        XiBlock *blk = loop->body[i];
        if (blk)
            total += blk->nvalues;
    }
    return total;
}

static bool has_bounds_check_or_guard(const XiLoop *loop) {
    for (uint32_t i = 0; i < loop->nbody; i++) {
        XiBlock *blk = loop->body[i];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (v && v->op == XI_BOUNDS_CHECK)
                return true;
        }
    }
    return false;
}

static bool loop_eligible_for_peel(const XiLoop *loop) {
    if (!loop || !loop->header || !loop->preheader || !loop->latch)
        return false;
    XiBlock *header = loop->header;
    if (header->kind != XI_BLOCK_IF || !header->control)
        return false;
    if (header->npreds != 2)
        return false;

    bool has_pre = false, has_latch = false;
    for (uint16_t p = 0; p < header->npreds; p++) {
        if (header->preds[p] == loop->preheader)
            has_pre = true;
        else if (header->preds[p] == loop->latch)
            has_latch = true;
    }
    if (!has_pre || !has_latch)
        return false;

    uint32_t body_values = count_loop_values(loop);
    if (body_values > PEEL_MAX_BODY_VALUES)
        return false;

    if (!has_bounds_check_or_guard(loop))
        return false;

    return true;
}

/* ========== Cloning ========== */

static void clone_metadata(XiValue *dst, const XiValue *src) {
    dst->flags = src->flags;
    dst->var_id = src->var_id;
    dst->rep = src->rep;
    dst->escape = src->escape;
    dst->mem_group = src->mem_group;
    dst->aux_int = src->aux_int;
    dst->aux = src->aux;
    dst->line = src->line;
}

/* Resolve an operand in the peeled copy: if it was defined inside the
 * loop, use the cloned version; otherwise use the original. */
static XiValue *peel_resolve_arg(const PeelMap *map, XiValue *arg) {
    if (!arg)
        return NULL;
    XiValue *mapped = peel_map_find(map, arg);
    if (mapped)
        return mapped;
    return arg;
}

/* Clone all values from a loop body block into a peeled block. */
static bool clone_block_values(XiFunc *f, XiBlock *src, XiBlock *dst, PeelMap *map) {
    for (uint32_t i = 0; i < src->nvalues; i++) {
        XiValue *orig = src->values[i];
        XiValue *clone = xi_value_new(f, dst, orig->op, orig->type, orig->nargs);
        if (!clone)
            return false;
        clone_metadata(clone, orig);
        for (uint16_t a = 0; a < orig->nargs; a++) {
            clone->args[a] = peel_resolve_arg(map, orig->args[a]);
        }
        if (!peel_map_add(map, orig, clone))
            return false;
    }
    return true;
}

/* ========== Peeling Core ========== */

/* Find the peeled-copy block for a given original block. */
static XiBlock *find_peeled_block(const XiLoop *loop, XiBlock *const *peeled_bodies,
                                  const XiBlock *orig) {
    if (orig == loop->header)
        return NULL;
    for (uint32_t j = 0; j < loop->nbody; j++) {
        if (loop->body[j] == orig)
            return peeled_bodies[j];
    }
    return NULL;
}

/* Map a successor block reference: loop-internal blocks map to their
 * peeled counterparts; the header maps to itself (the second iteration);
 * out-of-loop blocks map to themselves. */
static XiBlock *map_peel_successor(const XiLoop *loop, XiBlock *const *peeled_bodies,
                                   XiBlock *succ) {
    if (succ == loop->header)
        return loop->header;
    XiBlock *mapped = find_peeled_block(loop, peeled_bodies, succ);
    return mapped ? mapped : succ;
}

/* Wire one peeled body block's terminator edges. */
static void wire_peeled_body_block(XiFunc *f, const XiLoop *loop, XiBlock *src, XiBlock *dst,
                                   XiBlock *const *peeled_bodies, const PeelMap *map) {
    if (src->kind == XI_BLOCK_PLAIN && src->succs[0]) {
        XiBlock *target = map_peel_successor(loop, peeled_bodies, src->succs[0]);
        xi_block_set_jump(dst, target);
        dst->line = src->line;
    } else if (src->kind == XI_BLOCK_IF && src->control) {
        XiValue *cond = peel_resolve_arg(map, src->control);
        XiBlock *then_t = map_peel_successor(loop, peeled_bodies, src->succs[0]);
        XiBlock *else_t = map_peel_successor(loop, peeled_bodies, src->succs[1]);
        if (cond && then_t && else_t) {
            xi_block_set_if(dst, cond, then_t, else_t);
            dst->line = src->line;
        }
    } else if (src->kind == XI_BLOCK_RETURN && src->control) {
        XiValue *ret = peel_resolve_arg(map, src->control);
        if (ret) {
            xi_block_set_return(dst, ret);
            dst->line = src->line;
        }
    }
    (void) f;
    dst->sealed = true;
}

/* Build exit phi args for the peeled path (first-iteration early exit). */
static XiValue **build_peeled_exit_args(XiFunc *f, XiBlock *header, XiBlock *exit_blk,
                                        const PeelMap *map, uint32_t *out_count) {
    uint32_t exit_phi_count = xi_cfg_phi_count(exit_blk);
    *out_count = exit_phi_count;
    if (exit_phi_count == 0)
        return NULL;
    uint16_t header_exit_idx = xi_cfg_pred_index(exit_blk, header);
    if (header_exit_idx >= exit_blk->npreds)
        return NULL;
    XiValue **args = (XiValue **) xi_func_arena_alloc(f, exit_phi_count * sizeof(XiValue *));
    if (!args)
        return NULL;
    uint32_t ei = 0;
    for (XiPhi *phi = exit_blk->phis; phi; phi = phi->next, ei++) {
        if (header_exit_idx >= phi->value.nargs) {
            *out_count = 0;
            return NULL;
        }
        args[ei] = peel_resolve_arg(map, phi->value.args[header_exit_idx]);
        if (!args[ei]) {
            *out_count = 0;
            return NULL;
        }
    }
    return args;
}

static bool peel_loop(XiFunc *f, XiLoop *loop) {
    XiBlock *header = loop->header;
    XiBlock *preheader = loop->preheader;
    XiBlock *body_entry = header->succs[0];
    XiBlock *exit_blk = header->succs[1];

    if (!body_entry || !exit_blk)
        return false;
    if (!xi_loop_contains_block(loop, body_entry))
        return false;
    if (xi_loop_contains_block(loop, exit_blk))
        return false;

    uint32_t total_values = count_loop_values(loop);
    uint32_t phi_count = xi_cfg_phi_count(header);
    PeelMap map;
    if (!peel_map_init(f, &map, total_values + phi_count + 16))
        return false;

    uint16_t pre_idx = xi_cfg_pred_index(header, preheader);
    if (pre_idx >= header->npreds)
        return false;
    for (XiPhi *phi = header->phis; phi; phi = phi->next) {
        if (pre_idx >= phi->value.nargs || !phi->value.args[pre_idx])
            return false;
        if (!peel_map_add(&map, &phi->value, phi->value.args[pre_idx]))
            return false;
    }

    XiBlock *peeled_header = xi_block_new(f);
    if (!peeled_header || !clone_block_values(f, header, peeled_header, &map))
        return false;
    XiValue *peeled_cond = peel_resolve_arg(&map, header->control);
    if (!peeled_cond)
        return false;

    XiBlock **peeled_bodies = (XiBlock **) xi_func_arena_alloc(f, loop->nbody * sizeof(XiBlock *));
    if (!peeled_bodies)
        return false;
    for (uint32_t i = 0; i < loop->nbody; i++) {
        if (loop->body[i] == header) {
            peeled_bodies[i] = peeled_header;
        } else {
            peeled_bodies[i] = xi_block_new(f);
            if (!peeled_bodies[i])
                return false;
            if (!clone_block_values(f, loop->body[i], peeled_bodies[i], &map))
                return false;
        }
    }

    XiBlock *peeled_body_entry = find_peeled_block(loop, peeled_bodies, body_entry);
    if (!peeled_body_entry)
        peeled_body_entry = peeled_bodies[0];
    if (!peeled_body_entry)
        return false;

    uint32_t exit_phi_count = 0;
    XiValue **peeled_exit_args = build_peeled_exit_args(f, header, exit_blk, &map, &exit_phi_count);

    peeled_header->kind = XI_BLOCK_IF;
    peeled_header->control = peeled_cond;
    peeled_header->succs[0] = peeled_body_entry;
    peeled_header->succs[1] = exit_blk;
    if (!xi_cfg_append_pred(peeled_body_entry, peeled_header, NULL, 0))
        return false;
    if (!xi_cfg_append_pred(exit_blk, peeled_header, peeled_exit_args, exit_phi_count))
        return false;

    for (uint32_t i = 0; i < loop->nbody; i++) {
        if (loop->body[i] != header)
            wire_peeled_body_block(f, loop, loop->body[i], peeled_bodies[i], peeled_bodies, &map);
    }

    if (!xi_cfg_redirect_edge(preheader, header, peeled_header, NULL, 0))
        return false;

    /* Update header phis for the peeled latch predecessor. */
    XiBlock *peeled_latch = find_peeled_block(loop, peeled_bodies, loop->latch);
    if (peeled_latch) {
        uint16_t pl_idx = xi_cfg_pred_index(header, peeled_latch);
        uint16_t latch_idx = xi_cfg_pred_index(header, loop->latch);
        if (pl_idx < header->npreds && latch_idx < header->npreds) {
            for (XiPhi *phi = header->phis; phi; phi = phi->next) {
                if (latch_idx < phi->value.nargs) {
                    XiValue *v = peel_resolve_arg(&map, phi->value.args[latch_idx]);
                    if (v && pl_idx < phi->value.nargs)
                        phi->value.args[pl_idx] = v;
                }
            }
        }
    }

    peeled_header->sealed = true;
    return true;
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_loop_peel(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_loop_peel: NULL func");
    if (f->nblocks < 3)
        return xi_pass_no_change();

    XiLoopInfo *loops = xi_ensure_loops(f);
    if (!loops || loops->nloop == 0)
        return xi_pass_no_change();

    XiPassChange chg = xi_pass_no_change();

    for (uint32_t li = 0; li < loops->nloop; li++) {
        XiLoop *loop = loops->all_loops[li];
        if (!loop_eligible_for_peel(loop))
            continue;
        if (peel_loop(f, loop)) {
            chg.cfg_changed = true;
            chg.values_changed = true;
            chg.n_added += count_loop_values(loop);
            return chg;
        }
    }
    return chg;
}
