/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_loop_rotate.c - Loop rotation for Xi IR
 */

#include "xi_opt_loop_rotate.h"
#include "xi_cfg_edit.h"
#include "xi_effect.h"
#include "xi_loop.h"
#include "../base/xchecks.h"

#define XI_ROTATE_CLONE_EFFECT_MASK                                                                \
    (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND | XI_FLAG_READS_MEM |           \
     XI_FLAG_WRITES_MEM)

typedef struct RotateMap {
    XiValue **old_vals;
    XiValue **new_vals;
    uint32_t count;
    uint32_t cap;
} RotateMap;

static bool map_init(XiFunc *f, RotateMap *m, uint32_t cap) {
    XR_DCHECK(f != NULL, "map_init: NULL func");
    XR_DCHECK(m != NULL, "map_init: NULL map");
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

static bool map_add(RotateMap *m, XiValue *old_val, XiValue *new_val) {
    XR_DCHECK(m != NULL, "map_add: NULL map");
    XR_DCHECK(old_val != NULL, "map_add: NULL old value");
    XR_DCHECK(new_val != NULL, "map_add: NULL new value");
    if (m->count >= m->cap)
        return false;
    m->old_vals[m->count] = old_val;
    m->new_vals[m->count] = new_val;
    m->count++;
    return true;
}

static XiValue *map_find(const RotateMap *m, const XiValue *old_val) {
    if (!m || !old_val)
        return NULL;
    for (uint32_t i = 0; i < m->count; i++) {
        if (m->old_vals[i] == old_val)
            return m->new_vals[i];
    }
    return NULL;
}

static bool map_contains(const RotateMap *m, const XiValue *old_val) {
    return map_find(m, old_val) != NULL;
}

static bool value_can_clone_in_guard(const XiValue *v) {
    if (!v || !v->type)
        return false;
    if ((v->flags & XI_ROTATE_CLONE_EFFECT_MASK) != 0)
        return false;
    return xi_op_can_speculate(v->op);
}

static bool header_value_available_before(const XiBlock *header, const XiValue *v, uint32_t limit) {
    if (!header || !v || v->block != header)
        return false;
    for (uint32_t i = 0; i < limit && i < header->nvalues; i++) {
        if (header->values[i] == v)
            return true;
    }
    return false;
}

static bool guard_operand_available(const XiLoop *loop, const XiBlock *header,
                                    const RotateMap *start_map, const XiValue *arg,
                                    uint32_t header_limit) {
    if (!arg)
        return false;
    if (map_contains(start_map, arg))
        return true;
    if (arg->block == header)
        return header_value_available_before(header, arg, header_limit);
    if (arg->block && xi_loop_contains_block(loop, arg->block))
        return false;
    return true;
}

static bool header_defs_are_guardable(const XiLoop *loop, const RotateMap *start_map) {
    XR_DCHECK(loop != NULL, "header_defs_are_guardable: NULL loop");
    XiBlock *header = loop->header;
    for (uint32_t i = 0; i < header->nvalues; i++) {
        XiValue *v = header->values[i];
        if (!value_can_clone_in_guard(v))
            return false;
        for (uint16_t a = 0; a < v->nargs; a++) {
            if (!guard_operand_available(loop, header, start_map, v->args[a], i))
                return false;
        }
    }
    return guard_operand_available(loop, header, start_map, header->control, header->nvalues);
}

static bool loop_shape_eligible(const XiLoop *loop) {
    if (!loop || !loop->header || !loop->preheader || !loop->latch)
        return false;
    XiBlock *header = loop->header;
    XiBlock *body = header->succs[0];
    XiBlock *exit_blk = header->succs[1];
    if (header->kind != XI_BLOCK_IF || !header->control || !body || !exit_blk || body == exit_blk)
        return false;
    if (body == header || exit_blk == header || header->npreds != 2)
        return false;
    if (!xi_loop_contains_block(loop, body) || xi_loop_contains_block(loop, exit_blk))
        return false;
    if (!xi_loop_contains_block(loop, loop->latch) || xi_loop_contains_block(loop, loop->preheader))
        return false;
    if (body->npreds != 1 || body->preds[0] != header || body->phis != NULL)
        return false;
    return xi_cfg_pred_index(header, loop->preheader) < header->npreds &&
           xi_cfg_pred_index(header, loop->latch) < header->npreds;
}

static bool build_start_map(XiFunc *f, XiBlock *header, XiBlock *preheader, RotateMap *start_map) {
    uint16_t pre_idx = xi_cfg_pred_index(header, preheader);
    if (pre_idx >= header->npreds)
        return false;
    for (XiPhi *phi = header->phis; phi; phi = phi->next) {
        if (pre_idx >= phi->value.nargs || !phi->value.args[pre_idx])
            return false;
        if (!map_add(start_map, &phi->value, phi->value.args[pre_idx]))
            return false;
    }
    (void) f;
    return true;
}

static bool collect_header_defs(XiBlock *header, RotateMap *defs) {
    for (XiPhi *phi = header->phis; phi; phi = phi->next) {
        if (!map_add(defs, &phi->value, &phi->value))
            return false;
    }
    for (uint32_t i = 0; i < header->nvalues; i++) {
        if (!map_add(defs, header->values[i], header->values[i]))
            return false;
    }
    return true;
}

static bool outside_use_is_safe_phi(const XiBlock *blk, const XiBlock *header,
                                    const XiBlock *exit_blk, uint16_t arg_idx) {
    return blk == exit_blk && arg_idx < blk->npreds && blk->preds[arg_idx] == header;
}

static bool has_unsafe_outside_uses(XiFunc *f, const XiLoop *loop, const RotateMap *defs,
                                    const XiBlock *exit_blk) {
    XiBlock *header = loop->header;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk || xi_loop_contains_block(loop, blk))
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (map_contains(defs, v->args[a]))
                    return true;
            }
        }
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (map_contains(defs, phi->value.args[a]) &&
                    !outside_use_is_safe_phi(blk, header, exit_blk, a))
                    return true;
            }
        }
        if (map_contains(defs, blk->control))
            return true;
    }
    return false;
}

static XiValue *map_guard_arg(const XiLoop *loop, const RotateMap *start_map,
                              const RotateMap *guard_map, XiValue *arg) {
    XiValue *mapped = map_find(start_map, arg);
    if (mapped)
        return mapped;
    mapped = map_find(guard_map, arg);
    if (mapped)
        return mapped;
    if (!arg || arg->block == loop->header)
        return NULL;
    if (arg->block && xi_loop_contains_block(loop, arg->block))
        return NULL;
    return arg;
}

static void copy_clone_metadata(XiValue *dst, const XiValue *src) {
    dst->flags = src->flags;
    dst->var_id = src->var_id;
    dst->rep = src->rep;
    dst->escape = src->escape;
    dst->mem_group = src->mem_group;
    dst->aux_int = src->aux_int;
    dst->aux = src->aux;
    dst->line = src->line;
}

static bool clone_header_values(XiFunc *f, const XiLoop *loop, XiBlock *guard,
                                const RotateMap *start_map, RotateMap *guard_map) {
    XiBlock *header = loop->header;
    for (uint32_t i = 0; i < header->nvalues; i++) {
        XiValue *orig = header->values[i];
        XiValue *clone = xi_value_new(f, guard, orig->op, orig->type, orig->nargs);
        if (!clone || (orig->nargs > 0 && !clone->args))
            return false;
        copy_clone_metadata(clone, orig);
        for (uint16_t a = 0; a < orig->nargs; a++) {
            XiValue *mapped = map_guard_arg(loop, start_map, guard_map, orig->args[a]);
            if (!mapped)
                return false;
            clone->args[a] = mapped;
        }
        if (!map_add(guard_map, orig, clone))
            return false;
    }
    return true;
}

static bool build_exit_phi_args(XiFunc *f, const XiLoop *loop, XiBlock *exit_blk,
                                const RotateMap *start_map, const RotateMap *guard_map,
                                XiValue ***out_args, uint32_t *out_nargs) {
    uint32_t nphis = xi_cfg_phi_count(exit_blk);
    *out_args = NULL;
    *out_nargs = nphis;
    if (nphis == 0)
        return true;
    uint16_t header_idx = xi_cfg_pred_index(exit_blk, loop->header);
    if (header_idx >= exit_blk->npreds)
        return false;
    XiValue **args = (XiValue **) xi_func_arena_alloc(f, nphis * sizeof(XiValue *));
    if (!args)
        return false;
    uint32_t i = 0;
    for (XiPhi *phi = exit_blk->phis; phi; phi = phi->next, i++) {
        if (header_idx >= phi->value.nargs)
            return false;
        args[i] = map_guard_arg(loop, start_map, guard_map, phi->value.args[header_idx]);
        if (!args[i])
            return false;
    }
    *out_args = args;
    return true;
}

static bool wire_guard_edges(XiBlock *guard, XiValue *guard_cond, XiBlock *body, XiBlock *exit_blk,
                             XiValue **exit_args, uint32_t n_exit_args) {
    guard->kind = XI_BLOCK_IF;
    guard->control = guard_cond;
    guard->succs[0] = body;
    guard->succs[1] = exit_blk;
    if (!xi_cfg_append_pred(body, guard, NULL, 0))
        return false;
    if (!xi_cfg_append_pred(exit_blk, guard, exit_args, n_exit_args)) {
        xi_cfg_remove_pred(body, guard);
        guard->kind = XI_BLOCK_UNREACHABLE;
        guard->control = NULL;
        guard->line = 0;
        guard->succs[0] = NULL;
        guard->succs[1] = NULL;
        return false;
    }
    return true;
}

static bool value_used_in_loop_body(const XiLoop *loop, const XiBlock *skip, XiValue *old_val) {
    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        XiBlock *blk = loop->body[bi];
        if (!blk || blk == skip)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] == old_val)
                    return true;
            }
        }
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == old_val)
                    return true;
            }
        }
        if (blk->control == old_val)
            return true;
    }
    return false;
}

static XiValue *guard_equivalent(const RotateMap *start_map, const RotateMap *guard_map,
                                 XiValue *old_val) {
    XiValue *v = map_find(start_map, old_val);
    return v ? v : map_find(guard_map, old_val);
}

static bool create_body_phis(XiFunc *f, const XiLoop *loop, XiBlock *body, XiBlock *guard,
                             const RotateMap *defs, const RotateMap *start_map,
                             const RotateMap *guard_map, RotateMap *body_map) {
    for (uint32_t i = 0; i < defs->count; i++) {
        XiValue *old_val = defs->old_vals[i];
        if (!value_used_in_loop_body(loop, loop->header, old_val))
            continue;
        XiValue *guard_val = guard_equivalent(start_map, guard_map, old_val);
        if (!guard_val)
            return false;
        XiPhi *phi = xi_phi_new(f, body, old_val->type, body->npreds);
        if (!phi || !phi->value.args)
            return false;
        for (uint16_t p = 0; p < body->npreds; p++) {
            if (body->preds[p] == guard)
                phi->value.args[p] = guard_val;
            else if (body->preds[p] == loop->header)
                phi->value.args[p] = old_val;
            else
                return false;
        }
        if (!map_add(body_map, old_val, &phi->value))
            return false;
    }
    return true;
}

static void replace_loop_body_uses(const XiLoop *loop, const XiBlock *skip,
                                   const RotateMap *body_map) {
    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        XiBlock *blk = loop->body[bi];
        if (!blk || blk == skip)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                XiValue *mapped = map_find(body_map, v->args[a]);
                if (mapped)
                    v->args[a] = mapped;
            }
        }
        if (blk != loop->header->succs[0]) {
            for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
                for (uint16_t a = 0; a < phi->value.nargs; a++) {
                    XiValue *mapped = map_find(body_map, phi->value.args[a]);
                    if (mapped)
                        phi->value.args[a] = mapped;
                }
            }
        }
        XiValue *mapped = map_find(body_map, blk->control);
        if (mapped)
            blk->control = mapped;
    }
}

static bool cleanup_failed_rotation(XiFunc *f, XiBlock *guard, uint32_t saved_nblocks,
                                    XiBlock *body, XiBlock *exit_blk, XiPhi *saved_body_phis) {
    if (body)
        body->phis = saved_body_phis;
    if (body && guard) {
        while (xi_cfg_remove_pred(body, guard)) {
        }
    }
    if (exit_blk && guard) {
        while (xi_cfg_remove_pred(exit_blk, guard)) {
        }
    }
    if (guard) {
        guard->kind = XI_BLOCK_UNREACHABLE;
        guard->control = NULL;
        guard->line = 0;
        guard->succs[0] = NULL;
        guard->succs[1] = NULL;
        if (f && saved_nblocks < f->nblocks && f->blocks[saved_nblocks] == guard)
            f->nblocks = saved_nblocks;
    }
    return false;
}

static bool rotate_loop(XiFunc *f, XiLoop *loop) {
    XiBlock *header = loop->header;
    XiBlock *preheader = loop->preheader;
    XiBlock *body = header->succs[0];
    XiBlock *exit_blk = header->succs[1];
    uint32_t nphis = xi_cfg_phi_count(header);
    uint32_t ndefs = nphis + header->nvalues;
    RotateMap start_map, defs, guard_map, body_map;
    if (!map_init(f, &start_map, nphis) || !map_init(f, &defs, ndefs) ||
        !map_init(f, &guard_map, header->nvalues) || !map_init(f, &body_map, ndefs))
        return false;
    if (!build_start_map(f, header, preheader, &start_map) || !collect_header_defs(header, &defs))
        return false;
    if (!header_defs_are_guardable(loop, &start_map) ||
        has_unsafe_outside_uses(f, loop, &defs, exit_blk))
        return false;

    uint32_t saved_nblocks = f->nblocks;
    XiBlock *guard = xi_block_new(f);
    if (!guard)
        return false;
    XiPhi *saved_body_phis = body->phis;
    if (!clone_header_values(f, loop, guard, &start_map, &guard_map))
        return cleanup_failed_rotation(f, guard, saved_nblocks, body, exit_blk, saved_body_phis);
    XiValue *guard_cond = map_guard_arg(loop, &start_map, &guard_map, header->control);
    if (!guard_cond)
        return cleanup_failed_rotation(f, guard, saved_nblocks, body, exit_blk, saved_body_phis);

    XiValue **exit_args = NULL;
    uint32_t n_exit_args = 0;
    if (!build_exit_phi_args(f, loop, exit_blk, &start_map, &guard_map, &exit_args, &n_exit_args))
        return cleanup_failed_rotation(f, guard, saved_nblocks, body, exit_blk, saved_body_phis);
    if (!wire_guard_edges(guard, guard_cond, body, exit_blk, exit_args, n_exit_args))
        return cleanup_failed_rotation(f, guard, saved_nblocks, body, exit_blk, saved_body_phis);
    guard->line = header->line;
    if (guard->line == 0 && header->control)
        guard->line = header->control->line;
    if (!create_body_phis(f, loop, body, guard, &defs, &start_map, &guard_map, &body_map))
        return cleanup_failed_rotation(f, guard, saved_nblocks, body, exit_blk, saved_body_phis);
    if (!xi_cfg_redirect_edge(preheader, header, guard, NULL, 0))
        return cleanup_failed_rotation(f, guard, saved_nblocks, body, exit_blk, saved_body_phis);
    replace_loop_body_uses(loop, header, &body_map);

    guard->sealed = true;
    body->sealed = true;
    header->sealed = true;
    exit_blk->sealed = true;
    return true;
}

XR_FUNC XiPassChange xi_opt_loop_rotate(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_loop_rotate: NULL func");
    if (f->nblocks < 3)
        return xi_pass_no_change();

    XiLoopInfo *loops = xi_ensure_loops(f);
    if (!loops || loops->nloop == 0)
        return xi_pass_no_change();

    for (uint32_t li = 0; li < loops->nloop; li++) {
        XiLoop *loop = loops->all_loops[li];
        if (!loop_shape_eligible(loop))
            continue;
        if (rotate_loop(f, loop)) {
            XiPassChange chg = xi_pass_no_change();
            chg.cfg_changed = true;
            chg.values_changed = true;
            chg.n_added = 1;
            return chg;
        }
    }
    return xi_pass_no_change();
}
