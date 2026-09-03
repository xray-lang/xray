/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_loop_inv_branch.c - Conservative loop-invariant branch unswitching
 *
 * Version a canonical loop around an invariant body branch:
 *
 *   preheader -> loop { if (invariant) then_path else else_path }
 *
 * becomes:
 *
 *                 /-> loop_true  { then_path }
 *   preheader - if
 *                 \-> loop_false { else_path }
 *
 * This is intentionally fail-closed.  The first implementation accepted an
 * invariant condition but only moved its calculation; an already-hoisted
 * condition produced no change and the branch still executed every iteration.
 * This pass performs the control-flow transformation its name promises, but
 * only for a small, top-tested, single-latch loop with a unique preheader,
 * header-only phis, one header exit, and no nested loop.  Those constraints
 * make predecessor and phi reconstruction exact and keep code growth bounded.
 */

#include "xi_opt_loop_inv_branch.h"
#include "xi_analysis.h"
#include "xi_cfg_edit.h"
#include "xi_loop.h"
#include "../base/xchecks.h"

#define LOOP_UNSWITCH_MAX_VALUES 48

typedef struct UnswitchMap {
    XiValue **old_values;
    XiValue **new_values;
    uint32_t count;
    uint32_t cap;
} UnswitchMap;

typedef struct UnswitchVersion {
    XiBlock **blocks;
    bool *reachable;
    UnswitchMap values;
    uint32_t added_values;
} UnswitchVersion;

static int loop_block_index(const XiLoop *loop, const XiBlock *block) {
    if (!loop || !block)
        return -1;
    for (uint32_t i = 0; i < loop->nbody; i++) {
        if (loop->body[i] == block)
            return (int) i;
    }
    return -1;
}

static bool map_init(XiFunc *f, UnswitchMap *map, uint32_t cap) {
    map->old_values = NULL;
    map->new_values = NULL;
    map->count = 0;
    map->cap = cap;
    if (cap == 0)
        return true;
    map->old_values = (XiValue **) xi_func_arena_alloc(f, cap * sizeof(XiValue *));
    map->new_values = (XiValue **) xi_func_arena_alloc(f, cap * sizeof(XiValue *));
    return map->old_values != NULL && map->new_values != NULL;
}

static bool map_add(UnswitchMap *map, XiValue *old_value, XiValue *new_value) {
    if (!map || !old_value || !new_value || map->count >= map->cap)
        return false;
    map->old_values[map->count] = old_value;
    map->new_values[map->count] = new_value;
    map->count++;
    return true;
}

static XiValue *map_find(const UnswitchMap *map, const XiValue *old_value) {
    if (!map || !old_value)
        return NULL;
    for (uint32_t i = 0; i < map->count; i++) {
        if (map->old_values[i] == old_value)
            return map->new_values[i];
    }
    return NULL;
}

static XiValue *resolve_value(const UnswitchMap *map, XiValue *old_value) {
    XiValue *mapped = map_find(map, old_value);
    return mapped ? mapped : old_value;
}

static uint32_t count_loop_values(const XiLoop *loop) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < loop->nbody; i++) {
        XiBlock *block = loop->body[i];
        if (!block)
            continue;
        count += block->nvalues;
        count += xi_cfg_phi_count(block);
    }
    return count;
}

static bool value_is_loop_invariant(const XiLoop *loop, const XiValue *value) {
    return !value || !value->block || !xi_loop_contains_block(loop, value->block);
}

/* Only the canonical header may leave the loop.  This excludes break/return
 * side exits whose exit phis would otherwise need path-sensitive rebuilding. */
static bool loop_shape_is_supported(const XiLoop *loop) {
    if (!loop || !loop->header || !loop->preheader || !loop->latch || loop->child)
        return false;
    if (loop->nbody < 3 || count_loop_values(loop) > LOOP_UNSWITCH_MAX_VALUES)
        return false;

    XiBlock *header = loop->header;
    XiBlock *preheader = loop->preheader;
    if (header->kind != XI_BLOCK_IF || !header->control || header->npreds != 2)
        return false;
    if (preheader->kind != XI_BLOCK_PLAIN || preheader->succs[0] != header ||
        preheader->succs[1] != NULL)
        return false;

    bool has_preheader = false;
    bool has_latch = false;
    for (uint16_t p = 0; p < header->npreds; p++) {
        has_preheader |= header->preds[p] == preheader;
        has_latch |= header->preds[p] == loop->latch;
    }
    if (!has_preheader || !has_latch)
        return false;

    int internal_header_succs = 0;
    for (uint16_t s = 0; s < 2; s++) {
        if (header->succs[s] && xi_loop_contains_block(loop, header->succs[s]))
            internal_header_succs++;
    }
    if (internal_header_succs != 1)
        return false;

    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        XiBlock *block = loop->body[bi];
        if (!block)
            return false;
        if (block != header && block->phis)
            return false;

        for (uint16_t p = 0; p < block->npreds; p++) {
            XiBlock *pred = block->preds[p];
            if (block == header && pred == preheader)
                continue;
            if (!xi_loop_contains_block(loop, pred))
                return false;
        }

        uint16_t nsucc = block->kind == XI_BLOCK_IF ? 2 : block->kind == XI_BLOCK_PLAIN ? 1 : 0;
        if (nsucc == 0)
            return false;
        for (uint16_t s = 0; s < nsucc; s++) {
            XiBlock *succ = block->succs[s];
            if (!succ)
                return false;
            if (!xi_loop_contains_block(loop, succ) && block != header)
                return false;
        }
    }
    return true;
}

static XiBlock *find_invariant_branch(const XiLoop *loop) {
    if (!loop_shape_is_supported(loop))
        return NULL;
    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        XiBlock *block = loop->body[bi];
        if (!block || block == loop->header || block->kind != XI_BLOCK_IF || !block->control)
            continue;
        if (!block->succs[0] || !block->succs[1] ||
            !xi_loop_contains_block(loop, block->succs[0]) ||
            !xi_loop_contains_block(loop, block->succs[1]))
            continue;
        /* LICM runs before this pass.  Requiring the final branch value to be
         * outside the loop prevents us from duplicating a phi selection or
         * speculating a computation while unswitching. */
        if (value_is_loop_invariant(loop, block->control))
            return block;
    }
    return NULL;
}

static bool mark_version_reachable(XiFunc *f, const XiLoop *loop, XiBlock *branch_block,
                                   bool take_true, bool **out_reachable) {
    bool *reachable = (bool *) xi_func_arena_alloc(f, loop->nbody * sizeof(bool));
    uint32_t *work = (uint32_t *) xi_func_arena_alloc(f, loop->nbody * sizeof(uint32_t));
    if (!reachable || !work)
        return false;

    int header_index = loop_block_index(loop, loop->header);
    if (header_index < 0)
        return false;
    uint32_t head = 0, tail = 0;
    reachable[header_index] = true;
    work[tail++] = (uint32_t) header_index;

    while (head < tail) {
        uint32_t bi = work[head++];
        XiBlock *block = loop->body[bi];
        uint16_t first = 0;
        uint16_t end = block->kind == XI_BLOCK_IF ? 2 : 1;
        if (block == branch_block) {
            first = take_true ? 0 : 1;
            end = (uint16_t) (first + 1);
        }
        for (uint16_t s = first; s < end; s++) {
            XiBlock *succ = block->succs[s];
            int si = loop_block_index(loop, succ);
            if (si < 0 || reachable[si])
                continue;
            reachable[si] = true;
            work[tail++] = (uint32_t) si;
        }
    }

    int latch_index = loop_block_index(loop, loop->latch);
    if (latch_index < 0 || !reachable[latch_index])
        return false;
    *out_reachable = reachable;
    return true;
}

static bool version_operand_is_available(const XiLoop *loop, const bool *reachable,
                                         const XiValue *value) {
    if (!value || !value->block || !xi_loop_contains_block(loop, value->block))
        return true;
    int bi = loop_block_index(loop, value->block);
    return bi >= 0 && reachable[bi];
}

static bool version_uses_are_supported(const XiLoop *loop, XiBlock *branch_block,
                                       const bool *reachable) {
    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        if (!reachable[bi])
            continue;
        XiBlock *block = loop->body[bi];
        for (XiPhi *phi = block->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (!version_operand_is_available(loop, reachable, phi->value.args[a]))
                    return false;
            }
        }
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XiValue *value = block->values[vi];
            for (uint16_t a = 0; a < value->nargs; a++) {
                if (!version_operand_is_available(loop, reachable, value->args[a]))
                    return false;
            }
        }
        if (block != branch_block && !version_operand_is_available(loop, reachable, block->control))
            return false;
    }
    return true;
}

static bool create_version(XiFunc *f, const XiLoop *loop, XiBlock *branch_block, bool take_true,
                           bool *reachable, UnswitchVersion *version) {
    version->blocks = (XiBlock **) xi_func_arena_alloc(f, loop->nbody * sizeof(XiBlock *));
    version->reachable = reachable;
    version->added_values = 0;
    if (!version->blocks || !version->reachable ||
        !map_init(f, &version->values, count_loop_values(loop)))
        return false;

    /* Create the complete block and value namespace before resolving any SSA
     * operands.  This is independent of loop->body storage order. */
    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        if (!version->reachable[bi])
            continue;
        XiBlock *clone = xi_block_new(f);
        if (!clone)
            return false;
        clone->line = loop->body[bi]->line;
        clone->frequency = loop->body[bi]->frequency;
        version->blocks[bi] = clone;
    }

    XiBlock *header_clone = version->blocks[loop_block_index(loop, loop->header)];
    for (XiPhi *phi = loop->header->phis; phi; phi = phi->next) {
        XiPhi *clone = xi_phi_new(f, header_clone, phi->value.type, 2);
        if (!clone)
            return false;
        if (!xi_value_clone_metadata(f, &clone->value, &phi->value))
            return false;
        if (!map_add(&version->values, &phi->value, &clone->value))
            return false;
        version->added_values++;
    }

    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        if (!version->reachable[bi])
            continue;
        XiBlock *source = loop->body[bi];
        XiBlock *clone_block = version->blocks[bi];
        for (uint32_t vi = 0; vi < source->nvalues; vi++) {
            XiValue *source_value = source->values[vi];
            XiValue *clone = xi_value_new(f, clone_block, source_value->op, source_value->type,
                                          source_value->nargs);
            if (!clone || !map_add(&version->values, source_value, clone))
                return false;
            version->added_values++;
        }
    }

    for (uint32_t mi = 0; mi < version->values.count; mi++) {
        XiValue *source = version->values.old_values[mi];
        XiValue *clone = version->values.new_values[mi];
        if (!xi_value_clone_metadata(f, clone, source))
            return false;
        for (uint16_t a = 0; a < source->nargs; a++)
            clone->args[a] = resolve_value(&version->values, source->args[a]);
        if (!xi_value_clone_call_plan(f, clone, source))
            return false;
    }

    /* Clone terminators.  The selected invariant branch becomes a jump. */
    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        if (!version->reachable[bi])
            continue;
        XiBlock *source = loop->body[bi];
        XiBlock *clone = version->blocks[bi];
        clone->line = source->line;
        if (source == branch_block) {
            XiBlock *target = source->succs[take_true ? 0 : 1];
            int ti = loop_block_index(loop, target);
            if (ti < 0 || !version->blocks[ti])
                return false;
            clone->kind = XI_BLOCK_PLAIN;
            clone->control = NULL;
            clone->succs[0] = version->blocks[ti];
            clone->succs[1] = NULL;
        } else {
            clone->kind = source->kind;
            clone->control = resolve_value(&version->values, source->control);
            uint16_t nsucc = source->kind == XI_BLOCK_IF ? 2 : 1;
            for (uint16_t s = 0; s < nsucc; s++) {
                XiBlock *target = source->succs[s];
                int ti = loop_block_index(loop, target);
                clone->succs[s] = ti >= 0 ? version->blocks[ti] : target;
                if (!clone->succs[s])
                    return false;
            }
        }
        clone->sealed = true;
    }

    /* Build predecessor lists from the specialized successors. */
    xi_block_add_pred(header_clone, loop->preheader);
    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        if (!version->reachable[bi])
            continue;
        XiBlock *source = loop->body[bi];
        XiBlock *clone = version->blocks[bi];
        uint16_t nsucc = clone->kind == XI_BLOCK_IF ? 2 : 1;
        for (uint16_t s = 0; s < nsucc; s++) {
            XiBlock *target = clone->succs[s];
            if (target && xi_loop_contains_block(loop, source->succs[s]))
                xi_block_add_pred(target, clone);
        }
    }

    if (header_clone->npreds != 2)
        return false;
    uint16_t old_pre = xi_cfg_pred_index(loop->header, loop->preheader);
    uint16_t old_latch = xi_cfg_pred_index(loop->header, loop->latch);
    uint16_t new_pre = xi_cfg_pred_index(header_clone, loop->preheader);
    XiBlock *latch_clone = version->blocks[loop_block_index(loop, loop->latch)];
    uint16_t new_latch = xi_cfg_pred_index(header_clone, latch_clone);
    if (old_pre >= loop->header->npreds || old_latch >= loop->header->npreds ||
        new_pre >= header_clone->npreds || new_latch >= header_clone->npreds)
        return false;
    for (XiPhi *source = loop->header->phis; source; source = source->next) {
        XiValue *clone = map_find(&version->values, &source->value);
        if (!clone || clone->nargs != 2)
            return false;
        clone->args[new_pre] = resolve_value(&version->values, source->value.args[old_pre]);
        clone->args[new_latch] = resolve_value(&version->values, source->value.args[old_latch]);
    }
    return true;
}

static XiBlock *loop_exit_block(const XiLoop *loop) {
    if (!loop || !loop->header)
        return NULL;
    for (uint16_t s = 0; s < 2; s++) {
        XiBlock *succ = loop->header->succs[s];
        if (succ && !xi_loop_contains_block(loop, succ))
            return succ;
    }
    return NULL;
}

static bool collect_direct_exit_value(XiValue **values, uint32_t *count, uint32_t cap,
                                      XiValue *value) {
    if (!value)
        return true;
    for (uint32_t i = 0; i < *count; i++) {
        if (values[i] == value)
            return true;
    }
    if (*count >= cap)
        return false;
    values[(*count)++] = value;
    return true;
}

/* A loop-header value may be used directly after the loop because the original
 * header dominates its unique exit.  Two version headers do not individually
 * dominate that exit, so materialize one exit phi per such value before adding
 * the cloned exit edges.  Existing edge phis need no extra merge: their args
 * are extended by append_version_exit_edges(). */
static bool collect_direct_exit_values(XiFunc *f, const XiLoop *loop, XiValue ***out_direct,
                                       uint32_t *out_count) {
    XiBlock *exit = loop_exit_block(loop);
    uint32_t cap = count_loop_values(loop);
    if (!exit || exit->npreds != 1 || !xi_loop_contains_block(loop, exit->preds[0]))
        return false;
    for (XiPhi *phi = exit->phis; phi; phi = phi->next) {
        if (phi->value.nargs != exit->npreds)
            return false;
    }

    XiValue **direct = (XiValue **) xi_func_arena_alloc(f, cap * sizeof(XiValue *));
    if (cap > 0 && !direct)
        return false;
    uint32_t ndirect = 0;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *block = f->blocks[bi];
        if (!block || xi_loop_contains_block(loop, block))
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XiValue *user = block->values[vi];
            for (uint16_t a = 0; a < user->nargs; a++) {
                XiValue *arg = user->args[a];
                if (!arg || !arg->block || !xi_loop_contains_block(loop, arg->block))
                    continue;
                if (!xi_dominates(exit, block) ||
                    !collect_direct_exit_value(direct, &ndirect, cap, arg))
                    return false;
            }
        }
        XiValue *control = block->control;
        if (control && control->block && xi_loop_contains_block(loop, control->block)) {
            if (!xi_dominates(exit, block) ||
                !collect_direct_exit_value(direct, &ndirect, cap, control))
                return false;
        }
        if (block == exit)
            continue;
        for (XiPhi *phi = block->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                XiValue *arg = phi->value.args[a];
                if (!arg || !arg->block || !xi_loop_contains_block(loop, arg->block))
                    continue;
                if (a >= block->npreds || !xi_dominates(exit, block->preds[a]) ||
                    !collect_direct_exit_value(direct, &ndirect, cap, arg))
                    return false;
            }
        }
    }

    *out_direct = direct;
    *out_count = ndirect;
    return true;
}

static bool materialize_direct_exit_merges(XiFunc *f, const XiLoop *loop, XiValue **direct,
                                           uint32_t ndirect) {
    XiBlock *exit = loop_exit_block(loop);
    XiValue **merges = (XiValue **) xi_func_arena_alloc(f, ndirect * sizeof(XiValue *));
    if (!exit || (ndirect > 0 && (!direct || !merges)))
        return false;

    for (uint32_t i = 0; i < ndirect; i++) {
        XiValue *source = direct[i];
        XiPhi *merge = xi_phi_new(f, exit, source->type, exit->npreds);
        if (!merge)
            return false;
        if (!xi_value_clone_metadata(f, &merge->value, source))
            return false;
        for (uint16_t a = 0; a < merge->value.nargs; a++)
            merge->value.args[a] = source;
        merges[i] = &merge->value;
    }

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *block = f->blocks[bi];
        if (!block || xi_loop_contains_block(loop, block))
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XiValue *user = block->values[vi];
            for (uint16_t a = 0; a < user->nargs; a++) {
                for (uint32_t i = 0; i < ndirect; i++) {
                    if (user->args[a] == direct[i])
                        user->args[a] = merges[i];
                }
            }
        }
        for (uint32_t i = 0; i < ndirect; i++) {
            if (block->control == direct[i])
                block->control = merges[i];
        }
        /* Exit-phi inputs still describe the original loop edge.  They are
         * intentionally left untouched so each cloned edge can map them. */
        if (block == exit)
            continue;
        for (XiPhi *phi = block->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                for (uint32_t i = 0; i < ndirect; i++) {
                    if (phi->value.args[a] == direct[i])
                        phi->value.args[a] = merges[i];
                }
            }
        }
    }
    return true;
}

static bool append_version_exit_edges(XiFunc *f, const XiLoop *loop,
                                      const UnswitchVersion *version) {
    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        if (!version->reachable[bi])
            continue;
        XiBlock *source = loop->body[bi];
        XiBlock *clone = version->blocks[bi];
        uint16_t nsucc = source->kind == XI_BLOCK_IF ? 2 : 1;
        for (uint16_t s = 0; s < nsucc; s++) {
            XiBlock *exit = source->succs[s];
            if (!exit || xi_loop_contains_block(loop, exit))
                continue;
            uint16_t old_index = xi_cfg_pred_index(exit, source);
            uint32_t nphis = xi_cfg_phi_count(exit);
            if (old_index >= exit->npreds)
                return false;
            XiValue **args = NULL;
            if (nphis > 0) {
                args = (XiValue **) xi_func_arena_alloc(f, nphis * sizeof(XiValue *));
                if (!args)
                    return false;
                uint32_t pi = 0;
                for (XiPhi *phi = exit->phis; phi; phi = phi->next, pi++) {
                    if (old_index >= phi->value.nargs)
                        return false;
                    args[pi] = resolve_value(&version->values, phi->value.args[old_index]);
                }
            }
            if (!xi_cfg_append_pred(exit, clone, args, nphis))
                return false;
        }
    }
    return true;
}

static void retire_original_loop(const XiLoop *loop) {
    /* Remove original exit predecessors first so exit phi argument positions
     * remain synchronized, then erase the now-unreachable old loop body. */
    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        XiBlock *block = loop->body[bi];
        uint16_t nsucc = block->kind == XI_BLOCK_IF ? 2 : block->kind == XI_BLOCK_PLAIN ? 1 : 0;
        for (uint16_t s = 0; s < nsucc; s++) {
            XiBlock *succ = block->succs[s];
            if (succ && !xi_loop_contains_block(loop, succ)) {
                while (xi_cfg_remove_pred(succ, block)) {
                }
            }
        }
    }
    for (uint32_t bi = 0; bi < loop->nbody; bi++) {
        XiBlock *block = loop->body[bi];
        block->kind = XI_BLOCK_UNREACHABLE;
        block->control = NULL;
        block->succs[0] = NULL;
        block->succs[1] = NULL;
        block->npreds = 0;
        block->phis = NULL;
        block->nvalues = 0;
        block->sealed = true;
    }
}

static bool unswitch_loop(XiFunc *f, XiLoop *loop, XiBlock *branch_block,
                          uint32_t *out_added_values) {
    UnswitchVersion true_version = {0};
    UnswitchVersion false_version = {0};
    bool *true_reachable = NULL;
    bool *false_reachable = NULL;
    XiValue **direct_exit_values = NULL;
    uint32_t direct_exit_value_count = 0;

    /* Complete every structural check before appending the first cloned block.
     * A rejected candidate must leave the original CFG byte-for-byte intact. */
    if (!mark_version_reachable(f, loop, branch_block, true, &true_reachable) ||
        !mark_version_reachable(f, loop, branch_block, false, &false_reachable) ||
        !version_uses_are_supported(loop, branch_block, true_reachable) ||
        !version_uses_are_supported(loop, branch_block, false_reachable) ||
        !collect_direct_exit_values(f, loop, &direct_exit_values, &direct_exit_value_count))
        return false;

    if (!create_version(f, loop, branch_block, true, true_reachable, &true_version) ||
        !create_version(f, loop, branch_block, false, false_reachable, &false_version))
        return false;

    XiBlock *true_header = true_version.blocks[loop_block_index(loop, loop->header)];
    XiBlock *false_header = false_version.blocks[loop_block_index(loop, loop->header)];
    if (!true_header || !false_header ||
        !materialize_direct_exit_merges(f, loop, direct_exit_values, direct_exit_value_count) ||
        !append_version_exit_edges(f, loop, &true_version) ||
        !append_version_exit_edges(f, loop, &false_version))
        return false;

    XiBlock *preheader = loop->preheader;
    preheader->kind = XI_BLOCK_IF;
    preheader->control = branch_block->control;
    preheader->succs[0] = true_header;
    preheader->succs[1] = false_header;
    retire_original_loop(loop);

    *out_added_values = true_version.added_values + false_version.added_values;
    return true;
}

XR_FUNC XiPassChange xi_opt_loop_inv_branch(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_loop_inv_branch: NULL func");
    if (f->nblocks < 4)
        return xi_pass_no_change();

    XiLoopInfo *loops = xi_ensure_loops(f);
    if (!loops || loops->nloop == 0)
        return xi_pass_no_change();

    for (uint32_t li = 0; li < loops->nloop; li++) {
        XiLoop *loop = loops->all_loops[li];
        XiBlock *branch_block = find_invariant_branch(loop);
        if (!branch_block)
            continue;
        uint32_t added_values = 0;
        if (unswitch_loop(f, loop, branch_block, &added_values)) {
            XiPassChange change = xi_pass_no_change();
            change.cfg_changed = true;
            change.values_changed = true;
            change.n_added = added_values;
            return change;
        }
    }
    return xi_pass_no_change();
}
