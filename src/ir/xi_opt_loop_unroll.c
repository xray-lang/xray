/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_loop_unroll.c - Loop unrolling for Xi IR
 *
 * Full unroll:  when the trip count is known at compile time and
 *               small (≤ UNROLL_FULL_MAX_TRIP), the entire loop is
 *               replaced by trip_count copies of the body.
 *
 * The pattern handled is:
 *
 *   preheader:
 *     i_start = C_start (const)
 *   header:
 *     i_phi = phi(i_start, i_next)
 *     cond  = i_phi < C_limit (const)
 *     if cond → body, exit
 *   body:
 *     ...
 *   latch:
 *     i_next = i_phi + C_step (const)
 *     → header
 *
 * After full unroll (trip_count = 4, step = 1):
 *
 *   preheader:
 *     body_clone_0 (i = C_start + 0*step)
 *     body_clone_1 (i = C_start + 1*step)
 *     body_clone_2 (i = C_start + 2*step)
 *     body_clone_3 (i = C_start + 3*step)
 *     → exit
 */

#include "xi_opt_loop_unroll.h"
#include "xi_cfg_edit.h"
#include "xi_cleanup.h"
#include "xi_loop.h"
#include "xi_analysis.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"

#define UNROLL_FULL_MAX_TRIP 16
#define UNROLL_MAX_BODY_VALUES 20

/* ========== Value Cloning Map ========== */

typedef struct UnrollMap {
    XiValue **old_vals;
    XiValue **new_vals;
    uint32_t count;
    uint32_t cap;
} UnrollMap;

static bool umap_init(XiFunc *f, UnrollMap *m, uint32_t cap) {
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

static bool umap_add(UnrollMap *m, XiValue *old_val, XiValue *new_val) {
    if (m->count >= m->cap)
        return false;
    m->old_vals[m->count] = old_val;
    m->new_vals[m->count] = new_val;
    m->count++;
    return true;
}

static XiValue *umap_find(const UnrollMap *m, const XiValue *old_val) {
    if (!m || !old_val)
        return NULL;
    for (uint32_t i = 0; i < m->count; i++) {
        if (m->old_vals[i] == old_val)
            return m->new_vals[i];
    }
    return NULL;
}

/* ========== Eligibility ========== */

static uint32_t count_body_values(const XiLoop *loop) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < loop->nbody; i++) {
        XiBlock *blk = loop->body[i];
        if (blk && blk != loop->header)
            total += blk->nvalues;
    }
    return total;
}

/* Full unroll clones loop body values into a straight-line chain.  That is
 * only correct when the loop body CFG is itself a single plain path from the
 * header's true edge to the latch back-edge.  Internal branches, early exits,
 * or non-local break/continue edges must stay in the normal CFG. */
static bool collect_linear_body_order(XiFunc *f, const XiLoop *loop, XiBlock ***out_blocks,
                                      uint32_t *out_count, uint32_t *out_value_count) {
    if (!f || !loop || !loop->header || !loop->latch)
        return false;

    XiBlock *header = loop->header;
    if (header->kind != XI_BLOCK_IF || !header->succs[0] || !header->succs[1])
        return false;
    if (!xi_loop_contains_block(loop, header->succs[0]))
        return false;
    if (xi_loop_contains_block(loop, header->succs[1]))
        return false;

    uint32_t max_blocks = (loop->nbody > 0) ? loop->nbody - 1 : 0;
    if (max_blocks == 0)
        return false;

    XiBlock **ordered = NULL;
    if (out_blocks) {
        ordered = (XiBlock **) xi_func_arena_alloc(f, max_blocks * sizeof(XiBlock *));
        if (!ordered)
            return false;
    }

    XiBlock *expected_pred = header;
    XiBlock *cur = header->succs[0];
    uint32_t count = 0;
    uint32_t values = 0;
    while (cur && cur != header) {
        if (!xi_loop_contains_block(loop, cur))
            return false;
        if (cur == header || cur->kind != XI_BLOCK_PLAIN || !cur->succs[0])
            return false;
        if (cur->phis)
            return false;
        if (cur->npreds != 1 || cur->preds[0] != expected_pred)
            return false;
        if (count >= max_blocks)
            return false;

        if (ordered)
            ordered[count] = cur;
        count++;
        values += cur->nvalues;

        XiBlock *next = cur->succs[0];
        if (next != header && !xi_loop_contains_block(loop, next))
            return false;
        expected_pred = cur;
        cur = next;
    }

    if (count != max_blocks)
        return false;
    if (!ordered || count == 0 || ordered[count - 1] != loop->latch)
        return false;

    if (out_blocks)
        *out_blocks = ordered;
    if (out_count)
        *out_count = count;
    if (out_value_count)
        *out_value_count = values;
    return true;
}

/* Check if a value has side effects that prevent unrolling. */
static bool has_unrollable_side_effects(const XiLoop *loop) {
    for (uint32_t i = 0; i < loop->nbody; i++) {
        XiBlock *blk = loop->body[i];
        if (!blk || blk == loop->header)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (v && (v->flags & XI_FLAG_MAY_THROW))
                return true;
        }
    }
    return false;
}

static bool unroll_cleanup_member_is_cloned(const XiLoop *loop, const XiValue *value) {
    return !value || (value->block != loop->header && xi_loop_contains_block(loop, value->block));
}

static uint32_t unroll_cleanup_position(XiBlock *const *body_order, uint32_t body_count,
                                        const XiValue *value) {
    uint32_t position = 0u;
    for (uint32_t bi = 0u; bi < body_count; ++bi) {
        const XiBlock *block = body_order[bi];
        for (uint32_t vi = 0u; vi < block->nvalues; ++vi, ++position) {
            if (block->values[vi] == value)
                return position;
        }
    }
    return UINT32_MAX;
}

/* Flattening introduces same-block ordering constraints. Prove them against
 * the exact
 * CFG-ordered clone sequence, not source block storage order. */
static bool unroll_cleanup_order_is_supported(XiBlock *const *body_order, uint32_t body_count,
                                              const XiCleanupBoundary *boundary) {
    uint32_t enter = unroll_cleanup_position(body_order, body_count, boundary->enter);
    uint32_t head = unroll_cleanup_position(body_order, body_count, boundary->frontier);
    if (enter == UINT32_MAX || head > enter)
        return false;
    uint32_t completion = enter;
    if (boundary->leave) {
        completion = unroll_cleanup_position(body_order, body_count, boundary->leave);
        if (completion == UINT32_MAX || enter >= completion)
            return false;
    }
    if (boundary->remaining) {
        uint32_t remaining = unroll_cleanup_position(body_order, body_count, boundary->remaining);
        if (remaining == UINT32_MAX || completion >= remaining)
            return false;
    }
    return true;
}

/* A full unroll retires the header as well as the body, but clones only the
 * body. Every
 * surviving identity must form a complete per-iteration frontier;
 * a header boundary or a
 * relation outside that subset cannot be duplicated. */
static bool unroll_cleanup_frontiers_are_closed(XiFunc *f, const XiLoop *loop,
                                                XiBlock *const *body_order, uint32_t body_count) {
    bool verified = false;
    for (uint32_t bi = 0u; bi < loop->nbody; ++bi) {
        const XiBlock *block = loop->body[bi];
        for (uint32_t vi = 0u; vi < block->nvalues; ++vi) {
            const XiValue *value = block->values[vi];
            if (!value || (!value->cleanup_boundary && value->op != XI_CLEANUP_ENTER &&
                           value->op != XI_CLEANUP_LEAVE))
                continue;
            if (!verified && !xi_cleanup_verify(f, NULL, 0u))
                return false;
            verified = true;
            const XiCleanupBoundary *boundary = value->cleanup_boundary;
            if (!unroll_cleanup_member_is_cloned(loop, boundary->enter) ||
                !unroll_cleanup_member_is_cloned(loop, boundary->leave) ||
                !unroll_cleanup_member_is_cloned(loop, boundary->remaining) ||
                !unroll_cleanup_member_is_cloned(loop, boundary->frontier) ||
                !unroll_cleanup_order_is_supported(body_order, body_count, boundary))
                return false;
        }
    }
    return true;
}

/* Trip count is now computed centrally by xi_loop_trip_count()
 * and cached on XiLoop.trip_count / has_trip_count during loop
 * analysis.  The unroll pass reads the cached value directly. */

/* Replace every use of old_val (across all blocks' instructions, phis, and
 * block control values) with new_val. Used after unrolling to retarget
 * out-of-loop uses of a header phi to its exit value. */
static void lu_replace_all_uses(XiFunc *f, XiValue *old_val, XiValue *new_val) {
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] == old_val)
                    v->args[a] = new_val;
            }
        }
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == old_val)
                    phi->value.args[a] = new_val;
            }
        }
        if (blk->control == old_val)
            blk->control = new_val;
    }
}

/* ========== Full Unroll ========== */

static XiValue *resolve_arg(const UnrollMap *map, XiValue *arg) {
    if (!arg)
        return NULL;
    XiValue *mapped = umap_find(map, arg);
    return mapped ? mapped : arg;
}

static void unroll_remap_cleanup(XiFunc *f, const UnrollMap *map) {
    uint32_t count = 0u;
    for (uint32_t index = 0u; index < map->count; ++index)
        count += map->old_vals[index]->cleanup_boundary != NULL;
    if (count == 0u)
        return;
    XiCleanupValueMapping *values = xr_calloc(count, sizeof(*values));
    XR_CHECK(values != NULL, "unroll cleanup mapping allocation failed after cloning");
    uint32_t cursor = 0u;
    for (uint32_t index = 0u; index < map->count; ++index) {
        if (map->old_vals[index]->cleanup_boundary)
            values[cursor++] = (XiCleanupValueMapping) {map->old_vals[index], map->new_vals[index]};
    }
    XiCleanupRemap remap = {values, count};
    char error[192] = {0};
    bool remapped = xi_cleanup_remap(f, f, &remap, error, sizeof(error));
    xr_free(values);
    XR_CHECK(remapped, error);
}

/* Emit one unrolled iteration into `dst_blk`. The IV is replaced by
 * a constant for this iteration. */
static bool emit_unrolled_body(XiFunc *f, XiBlock **body_order, uint32_t body_count,
                               XiBlock *dst_blk, const XiBasicIV *biv, int64_t iv_val,
                               UnrollMap *map) {
    XrType *int_type = biv->phi->type;

    /* Map the IV phi to its concrete constant value. */
    XiValue *iv_const = xi_const_int(f, dst_blk, iv_val, int_type);
    if (!iv_const)
        return false;
    if (!umap_add(map, biv->phi, iv_const))
        return false;

    /* Clone body block values in CFG order. */
    for (uint32_t bi = 0; bi < body_count; bi++) {
        XiBlock *src = body_order[bi];
        if (!src)
            return false;
        for (uint32_t vi = 0; vi < src->nvalues; vi++) {
            XiValue *orig = src->values[vi];
            XiValue *clone = xi_value_new(f, dst_blk, orig->op, orig->type, orig->nargs);
            if (!clone)
                return false;
            if (!xi_value_clone_metadata(f, clone, orig))
                return false;
            for (uint16_t a = 0; a < orig->nargs; a++) {
                clone->args[a] = resolve_arg(map, orig->args[a]);
            }
            if (orig->error_producer) {
                clone->error_producer = resolve_arg(map, orig->error_producer);
                if (!clone->error_producer)
                    return false;
            }
            if (!xi_value_clone_call_plan(f, clone, orig))
                return false;
            if (!umap_add(map, orig, clone))
                return false;
        }
    }

    /* Map the latch step value (i_next) for the next iteration. */
    XiValue *next_mapped = umap_find(map, biv->next);
    if (!next_mapped) {
        int64_t next_val =
            (biv->step_op == XI_ADD) ? iv_val + biv->step_const : iv_val - biv->step_const;
        XiValue *next_c = xi_const_int(f, dst_blk, next_val, int_type);
        if (!next_c)
            return false;
        if (!umap_add(map, biv->next, next_c))
            return false;
    }

    /* Forward enter/leave/remaining identities are available only after the
     * entire iteration
     * has its own value namespace. */
    unroll_remap_cleanup(f, map);
    return true;
}

/* Check if exit block uses header-defined values directly (not through
 * exit phis).  Such usage
 * patterns are unsafe for full unrolling because the header values become unreachable after
 * unrolling. */
static bool exit_uses_header_directly(const XiLoop *loop, const XiBlock *exit_blk) {
    XiBlock *header = loop->header;
    /* Check control value. */
    if (exit_blk->control && exit_blk->control->block == header)
        return true;
    /* Check instruction operands. */
    for (uint32_t vi = 0; vi < exit_blk->nvalues; vi++) {
        XiValue *v = exit_blk->values[vi];
        if (!v)
            continue;
        for (uint16_t a = 0; a < v->nargs; a++) {
            if (v->args[a] && v->args[a]->block == header)
                return true;
        }
    }
    /* Phi args that reference header values through the header pred slot
     * are fine — they get rewritten.  But non-phi direct refs are unsafe. */
    return false;
}

static bool unroll_exit_phis_are_supported(const XiLoop *loop, const XiBlock *exit_block) {
    uint16_t exit_index = xi_cfg_pred_index(exit_block, loop->header);
    uint16_t latch_index = xi_cfg_pred_index(loop->header, loop->latch);
    if (exit_index >= exit_block->npreds || latch_index >= loop->header->npreds)
        return false;
    for (const XiPhi *phi = exit_block->phis; phi; phi = phi->next) {
        if (phi->value.nargs != exit_block->npreds)
            return false;
        const XiValue *source = phi->value.args[exit_index];
        if (!source)
            return false;
        if (source->block == loop->header &&
            (source->op != XI_PHI || latch_index >= source->nargs || !source->args[latch_index]))
            return false;
    }
    return true;
}

static bool unroll_append_exit_edge(XiFunc *f, const XiLoop *loop, XiBlock *iteration,
                                    XiBlock *exit_block, const UnrollMap *map) {
    uint16_t exit_index = xi_cfg_pred_index(exit_block, loop->header);
    uint16_t latch_index = xi_cfg_pred_index(loop->header, loop->latch);
    uint32_t count = xi_cfg_phi_count(exit_block);
    XiValue **args = count ? xi_func_arena_alloc(f, count * sizeof(*args)) : NULL;
    if (count && !args)
        return false;
    uint32_t index = 0u;
    for (XiPhi *phi = exit_block->phis; phi; phi = phi->next) {
        XiValue *source = phi->value.args[exit_index];
        /* The exiting header observes the final latch inputs, not the values
         * with which
         * the last unrolled body iteration began. */
        if (source->block == loop->header)
            source = source->args[latch_index];
        args[index++] = resolve_arg(map, source);
    }
    /* append_pred owns both predecessor and phi payload growth. set_jump
     * would append the
     * predecessor a second time without a matching payload. */
    if (!xi_cfg_append_pred(exit_block, iteration, args, count))
        return false;
    iteration->kind = XI_BLOCK_PLAIN;
    iteration->control = NULL;
    iteration->line = 0u;
    iteration->succs[0] = exit_block;
    iteration->succs[1] = NULL;
    return true;
}

/* Retire a fully-unrolled loop's original blocks: mark them unreachable and
 * drop their now-dead values and phis. Marking a block isolates its
 * successors, so iterate to a fixpoint regardless of loop->body order.
 * Clearing the values matters because the post-pass verifier still scans
 * UNREACHABLE blocks, and the originals hold stale references (e.g. to
 * preheader constants) that no longer dominate them after the loop is
 * bypassed. */
static void retire_unrolled_loop_blocks(XiFunc *f, XiLoop *loop) {
    bool marked_any = true;
    while (marked_any) {
        marked_any = false;
        for (uint32_t bi = 0; bi < loop->nbody; bi++) {
            XiBlock *blk = loop->body[bi];
            if (blk && xi_cfg_mark_unreachable_if_isolated(f, blk)) {
                blk->nvalues = 0;
                blk->phis = NULL;
                marked_any = true;
            }
        }
    }
}

static bool full_unroll(XiFunc *f, XiLoop *loop, uint32_t trip_count) {
    if (loop->nbasic_ivs == 0)
        return false;
    const XiBasicIV *biv = &loop->basic_ivs[0];
    XiBlock *header = loop->header;
    XiBlock *preheader = loop->preheader;
    XiBlock *exit_blk = header->succs[1];
    if (!exit_blk || xi_loop_contains_block(loop, exit_blk))
        return false;

    /* Reject when exit block directly uses header-defined values
     * (e.g. return iphi without going through an exit phi). */
    if (exit_uses_header_directly(loop, exit_blk) ||
        !unroll_exit_phis_are_supported(loop, exit_blk))
        return false;

    XiBlock **body_order = NULL;
    uint32_t body_block_count = 0;
    uint32_t body_values = 0;
    if (!collect_linear_body_order(f, loop, &body_order, &body_block_count, &body_values))
        return false;
    if (!unroll_cleanup_frontiers_are_closed(f, loop, body_order, body_block_count))
        return false;

    /* Create one unroll block per iteration, chained linearly. */
    XiBlock **iter_blocks = (XiBlock **) xi_func_arena_alloc(f, trip_count * sizeof(XiBlock *));
    if (!iter_blocks)
        return false;

    for (uint32_t i = 0; i < trip_count; i++) {
        iter_blocks[i] = xi_block_new(f);
        if (!iter_blocks[i])
            return false;
    }

    int64_t iv_val = biv->start->aux_int;

    /* The last iteration's map is needed for exit phi rewriting. */
    UnrollMap last_map;
    if (!umap_init(f, &last_map, body_values + 8))
        return false;

    /* Header pred slots, for threading loop-carried (non-IV) phis. */
    uint16_t pre_idx = xi_cfg_pred_index(header, preheader);
    uint16_t latch_idx = xi_cfg_pred_index(header, loop->latch);

    /* Previous iteration's value map: iteration K resolves each accumulator
     * (non-IV) header phi to the value produced for its latch input in
     * iteration K-1. Starts empty; only read for iter > 0. */
    UnrollMap prev_map;
    if (!umap_init(f, &prev_map, 1))
        return false;

    for (uint32_t iter = 0; iter < trip_count; iter++) {
        UnrollMap iter_map;
        if (!umap_init(f, &iter_map, body_values + 8))
            return false;

        /* Thread non-IV loop-carried header phis (e.g. accumulators) into this
         * iteration's map so cloned body values reference the correct
         * per-iteration definition rather than the soon-to-be-removed header
         * phi. The IV phi is mapped to a constant inside emit_unrolled_body. */
        for (XiPhi *phi = header->phis; phi; phi = phi->next) {
            if (&phi->value == biv->phi)
                continue;
            XiValue *incoming;
            if (iter == 0) {
                incoming = (pre_idx < phi->value.nargs) ? phi->value.args[pre_idx] : NULL;
            } else {
                XiValue *latch_val =
                    (latch_idx < phi->value.nargs) ? phi->value.args[latch_idx] : NULL;
                incoming = resolve_arg(&prev_map, latch_val);
            }
            if (incoming && !umap_add(&iter_map, &phi->value, incoming))
                return false;
        }

        if (!emit_unrolled_body(f, body_order, body_block_count, iter_blocks[iter], biv, iv_val,
                                &iter_map))
            return false;

        /* Chain iteration blocks. */
        if (iter + 1 < trip_count) {
            xi_block_set_jump(iter_blocks[iter], iter_blocks[iter + 1]);
        } else {
            /* Save last iteration's map for exit phi rewriting. */
            last_map = iter_map;

            if (!unroll_append_exit_edge(f, loop, iter_blocks[iter], exit_blk, &iter_map))
                return false;
        }
        iter_blocks[iter]->sealed = true;

        /* Carry this iteration's map forward so the next iteration can thread
         * accumulator phis from the values it produced. */
        prev_map = iter_map;

        /* Advance IV. */
        iv_val = (biv->step_op == XI_ADD) ? iv_val + biv->step_const : iv_val - biv->step_const;
    }

    /* Retarget any out-of-loop use of a header phi to its exit value (the last
     * iteration's latch input). Covers loop-carried values consumed after the
     * loop without an exit phi (non-LCSSA), e.g. an accumulator read by a later
     * loop's header phi. The original loop blocks become unreachable below, so
     * their internal uses do not matter. */
    for (XiPhi *phi = header->phis; phi; phi = phi->next) {
        XiValue *latch_val = (latch_idx < phi->value.nargs) ? phi->value.args[latch_idx] : NULL;
        XiValue *exit_val = resolve_arg(&last_map, latch_val);
        if (exit_val && exit_val != &phi->value)
            lu_replace_all_uses(f, &phi->value, exit_val);
    }

    /* Redirect preheader → first iteration block, bypassing the loop. */
    if (!xi_cfg_redirect_edge(preheader, header, iter_blocks[0], NULL, 0))
        return false;

    /* Remove header's back-edge predecessor. */
    xi_cfg_remove_pred(header, loop->latch);

    /* Retire the original loop blocks now that the preheader bypasses them. */
    retire_unrolled_loop_blocks(f, loop);

    return true;
}

/* ========== Driver ========== */

XR_FUNC XiPassChange xi_opt_loop_unroll(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_loop_unroll: NULL func");
    if (f->nblocks < 3)
        return xi_pass_no_change();

    XiLoopInfo *loops = xi_ensure_loops(f);
    if (!loops || loops->nloop == 0)
        return xi_pass_no_change();

    XiPassChange chg = xi_pass_no_change();

    /* Process innermost loops first. */
    for (uint32_t li = 0; li < loops->nloop; li++) {
        XiLoop *loop = loops->all_loops[li];
        if (!xi_loop_shape_is_simple(loop))
            continue;

        uint32_t body_vals = count_body_values(loop);
        if (body_vals > UNROLL_MAX_BODY_VALUES)
            continue;

        if (has_unrollable_side_effects(loop))
            continue;

        uint32_t trip = loop->has_trip_count ? loop->trip_count : 0;
        if (trip == 0 || trip > UNROLL_FULL_MAX_TRIP)
            continue;

        if (full_unroll(f, loop, trip)) {
            chg.cfg_changed = true;
            chg.values_changed = true;
            chg.n_added += body_vals * trip;
            return chg;
        }
    }

    return chg;
}
