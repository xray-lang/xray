/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_inline.c - Function Inlining for Xi IR
 *
 * ALGORITHM:
 *   1. Scan for XI_CALL where callee is traceable to XI_CLOSURE_NEW
 *      with a known XiFunc* in aux.
 *   2. Compute callee cost (total value count).  Skip if too large.
 *   3. Clone callee blocks into caller's arena:
 *      a. Create value_map[callee_id] → cloned XiValue* for remapping.
 *      b. Map callee params → call arguments.
 *      c. Clone each block's values, phis, and terminators.
 *      d. Replace callee RETURN blocks with JMP to continuation.
 *   4. Split caller's call_block at the call site.
 *   5. Wire: pre_call → callee_entry, callee_returns → continuation.
 *   6. Create phi in continuation for return value (if multiple returns).
 *
 * LIMITATIONS:
 *   - Single-level inlining per pass invocation (no recursive inlining).
 *   - No inlining of calls with variadic args.
 *   - Callee must not capture upvalues that alias caller locals.
 */

#include "xi_opt_inline.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

/* ========== Cost Model ========== */

/* Analyze callee to build a detailed cost model. */
static XiInlineCostModel analyze_callee(const XiFunc *callee) {
    XiInlineCostModel m = {0};
    for (uint32_t b = 0; b < callee->nblocks; b++) {
        const XiBlock *blk = callee->blocks[b];
        m.value_count += blk->nvalues;
        for (const XiPhi *p = blk->phis; p; p = p->next)
            m.value_count++;

        if (blk->kind == XI_BLOCK_IF)
            m.branch_count++;

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (v->op == XI_CALL || v->op == XI_CALL_METHOD || v->op == XI_CALL_BUILTIN)
                m.call_count++;
            if (v->op == XI_THROW)
                m.has_throw = true;
        }

        /* Back-edge detection: successor with lower block id indicates loop. */
        for (int s = 0; s < 2; s++) {
            if (blk->succs[s] && blk->succs[s]->id <= blk->id)
                m.has_loop = true;
        }
    }
    m.calls_self = false; /* checked separately at call site */
    return m;
}

/* Compute caller's current total value count. */
static uint32_t caller_total_values(const XiFunc *f) {
    uint32_t total = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        total += f->blocks[b]->nvalues;
        for (const XiPhi *p = f->blocks[b]->phis; p; p = p->next)
            total++;
    }
    return total;
}

/* Check if all call arguments (excluding callee itself) are constants. */
static bool all_args_are_const(const XiValue *call_val) {
    for (uint16_t a = 1; a < call_val->nargs; a++) {
        if (!call_val->args[a] || call_val->args[a]->op != XI_CONST)
            return false;
    }
    return true;
}

/* Benefit scoring:
 *   base = THRESHOLD - value_count   (bigger callee = lower base)
 *   + all_args_const * 15             (constant args enable specialization)
 *   + single_call_site * 10           (no code duplication)
 *   - call_count * 3                  (nested calls reduce benefit)
 *   - branch_count * 2                (complex control flow)
 *   - has_loop * 20                   (loop body expansion)
 *   - has_throw * 5                   (exception path pollution)
 *   - (caller_size > 300) * 15        (cap growth in large functions) */
XR_FUNC int xi_inline_benefit(const XiInlineCostModel *cost, const XiInlineCallSiteInfo *site) {
    XR_DCHECK(cost != NULL && site != NULL, "inline_benefit: NULL arg");

    if (cost->calls_self)
        return -1000; /* never inline recursion */

    int score = (int) XI_INLINE_BASE_THRESHOLD - (int) cost->value_count;

    if (site->all_args_const)
        score += 15;
    if (site->single_call_site)
        score += 10;

    score -= (int) cost->call_count * 3;
    score -= (int) cost->branch_count * 2;
    if (cost->has_loop)
        score -= 20;
    if (cost->has_throw)
        score -= 5;
    if (site->caller_size > 300)
        score -= 15;

    return score;
}

/* ========== Callee Resolution ========== */

/* Trace a call's callee value back to an XI_CLOSURE_NEW to find
 * the callee's XiFunc*.  Returns NULL if not resolvable. */
static XiFunc *resolve_callee(const XiValue *callee_val) {
    if (!callee_val)
        return NULL;
    /* Direct closure: XI_CLOSURE_NEW stores XiFunc* in aux. */
    if (callee_val->op == XI_CLOSURE_NEW && callee_val->aux)
        return (XiFunc *) callee_val->aux;
    /* Through a copy chain */
    if (callee_val->op == XI_COPY && callee_val->nargs >= 1)
        return resolve_callee(callee_val->args[0]);
    return NULL;
}

/* ========== Block/Value Cloning ========== */

/* Clone a single value into dst_blk, remapping args via value_map.
 * Constants are cloned as new constants in the caller. */
static XiValue *clone_value(XiFunc *caller, XiBlock *dst_blk, const XiValue *src,
                            XiValue **value_map, uint32_t map_size) {
    XiValue *cloned = xi_value_new(caller, dst_blk, src->op, src->type, src->nargs);
    if (!cloned)
        return NULL;

    cloned->flags = src->flags;
    cloned->aux_int = src->aux_int;
    cloned->aux = src->aux;
    cloned->line = src->line;

    /* Remap args */
    for (uint16_t a = 0; a < src->nargs; a++) {
        XiValue *orig_arg = src->args[a];
        if (orig_arg && orig_arg->id < map_size && value_map[orig_arg->id])
            cloned->args[a] = value_map[orig_arg->id];
        else
            cloned->args[a] = orig_arg; /* external reference (e.g. caller value) */
    }

    /* Register in value_map */
    if (src->id < map_size)
        value_map[src->id] = cloned;

    return cloned;
}

static bool callee_result_shape(const XiFunc *callee, bool *out_multi_ret, uint16_t *out_elems) {
    if (!callee || !out_multi_ret || !out_elems)
        return false;

    bool seen_return = false;
    bool multi_ret = false;
    uint16_t elems = 0;

    for (uint32_t b = 0; b < callee->nblocks; b++) {
        const XiBlock *blk = callee->blocks[b];
        if (!blk || blk->kind != XI_BLOCK_RETURN)
            continue;

        const XiValue *ret = blk->control;
        bool cur_multi_ret = ret && ret->op == XI_MULTI_RET;
        uint16_t cur_elems = ret ? 1 : 0;
        if (cur_multi_ret)
            cur_elems = ret->nargs;

        if (cur_elems > 16)
            return false;
        if (!seen_return) {
            seen_return = true;
            multi_ret = cur_multi_ret;
            elems = cur_elems;
            continue;
        }
        if (multi_ret != cur_multi_ret || elems != cur_elems)
            return false;
    }

    *out_multi_ret = multi_ret;
    *out_elems = elems;
    return true;
}

static bool call_extracts_fit_result_shape(const XiFunc *caller, const XiValue *call_val,
                                           uint16_t nresult_elems) {
    if (!caller || !call_val)
        return false;

    for (uint32_t b = 0; b < caller->nblocks; b++) {
        const XiBlock *blk = caller->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *v = blk->values[i];
            if (!v || v->op != XI_EXTRACT || v->nargs < 1 || v->args[0] != call_val)
                continue;
            if ((uint32_t) v->aux_int >= nresult_elems)
                return false;
        }
    }

    return true;
}

/* ========== Single Call Site Inlining ========== */

static bool inline_call_site(XiFunc *caller, XiBlock *call_blk, uint32_t call_idx,
                             XiValue *call_val, XiFunc *callee) {
    bool shape_multi_ret = false;
    uint16_t shape_elems = 0;
    if (!callee_result_shape(callee, &shape_multi_ret, &shape_elems))
        return false;
    if (!call_extracts_fit_result_shape(caller, call_val, shape_elems))
        return false;

    uint32_t callee_max_id = callee->next_value_id;
    XiValue **value_map = (XiValue **) xr_calloc(callee_max_id, sizeof(XiValue *));
    if (!value_map)
        return false;

    /* Map callee params → call arguments (args[1..n] of XI_CALL) */
    uint16_t nparams = callee->nparams;
    for (uint16_t p = 0; p < nparams; p++) {
        XiValue *param = callee->params[p];
        if (!param)
            continue;
        /* call_val->args[0] = callee, args[1..] = actual arguments */
        uint16_t arg_idx = p + 1;
        if (arg_idx < call_val->nargs && param->id < callee_max_id)
            value_map[param->id] = call_val->args[arg_idx];
    }

    /* Create continuation block (values after the call). */
    XiBlock *cont_blk = xi_block_new(caller);
    if (!cont_blk) {
        xr_free(value_map);
        return false;
    }
    cont_blk->kind = call_blk->kind;
    cont_blk->control = call_blk->control;
    cont_blk->succs[0] = call_blk->succs[0];
    cont_blk->succs[1] = call_blk->succs[1];

    /* Move post-call values to continuation block. */
    uint32_t post_start = call_idx + 1;
    uint32_t post_count = call_blk->nvalues - post_start;
    if (post_count > 0) {
        if (post_count > cont_blk->values_cap) {
            XiValue **new_values =
                (XiValue **) xi_func_arena_alloc(caller, post_count * sizeof(XiValue *));
            if (!new_values) {
                xr_free(value_map);
                return false;
            }
            cont_blk->values = new_values;
            cont_blk->values_cap = post_count;
        }
        for (uint32_t i = 0; i < post_count; i++) {
            cont_blk->values[i] = call_blk->values[post_start + i];
            if (cont_blk->values[i])
                cont_blk->values[i]->block = cont_blk;
        }
        cont_blk->nvalues = post_count;
    }

    /* Truncate call_blk: remove the call and everything after it. */
    call_blk->nvalues = call_idx;

    /* Clone callee blocks into caller. */
    uint32_t callee_nblk = callee->nblocks;
    XiBlock **cloned_blks = (XiBlock **) xr_calloc(callee_nblk, sizeof(XiBlock *));
    if (!cloned_blks) {
        xr_free(value_map);
        return false;
    }

    for (uint32_t bi = 0; bi < callee_nblk; bi++) {
        XiBlock *new_blk = xi_block_new(caller);
        if (!new_blk) {
            xr_free(value_map);
            xr_free(cloned_blks);
            return false;
        }
        cloned_blks[bi] = new_blk;
    }

    /* Collect return values and blocks for the join phi. */
    XiValue *ret_values[32];
    XiBlock *ret_blocks[32];
    uint32_t nret = 0;

    for (uint32_t bi = 0; bi < callee_nblk; bi++) {
        XiBlock *src_blk = callee->blocks[bi];
        XiBlock *dst_blk = cloned_blks[bi];

        /* Clone values */
        for (uint32_t vi = 0; vi < src_blk->nvalues; vi++) {
            XiValue *src_v = src_blk->values[vi];
            if (!src_v)
                continue;
            clone_value(caller, dst_blk, src_v, value_map, callee_max_id);
        }

        /* Clone terminator */
        switch (src_blk->kind) {
            case XI_BLOCK_PLAIN:
                dst_blk->kind = XI_BLOCK_PLAIN;
                if (src_blk->succs[0]) {
                    uint32_t target = src_blk->succs[0]->id;
                    /* Find the cloned block by callee block index */
                    for (uint32_t t = 0; t < callee_nblk; t++) {
                        if (callee->blocks[t]->id == target) {
                            dst_blk->succs[0] = cloned_blks[t];
                            break;
                        }
                    }
                }
                break;

            case XI_BLOCK_IF:
                dst_blk->kind = XI_BLOCK_IF;
                if (src_blk->control && src_blk->control->id < callee_max_id)
                    dst_blk->control = value_map[src_blk->control->id];
                for (int s = 0; s < 2; s++) {
                    if (!src_blk->succs[s])
                        continue;
                    uint32_t target = src_blk->succs[s]->id;
                    for (uint32_t t = 0; t < callee_nblk; t++) {
                        if (callee->blocks[t]->id == target) {
                            dst_blk->succs[s] = cloned_blks[t];
                            break;
                        }
                    }
                }
                break;

            case XI_BLOCK_RETURN:
                /* Replace RETURN with JMP to continuation. */
                dst_blk->kind = XI_BLOCK_PLAIN;
                dst_blk->succs[0] = cont_blk;
                if (nret < 32) {
                    XiValue *ret_val = NULL;
                    if (src_blk->control && src_blk->control->id < callee_max_id)
                        ret_val = value_map[src_blk->control->id];
                    ret_values[nret] = ret_val;
                    ret_blocks[nret] = dst_blk;
                    nret++;
                }
                break;

            default:
                dst_blk->kind = XI_BLOCK_UNREACHABLE;
                break;
        }

        /* Clone phi nodes */
        for (const XiPhi *src_phi = src_blk->phis; src_phi; src_phi = src_phi->next) {
            XiValue *phi_clone =
                xi_value_new(caller, dst_blk, XI_PHI, src_phi->value.type, src_phi->value.nargs);
            if (!phi_clone)
                continue;
            for (uint16_t a = 0; a < src_phi->value.nargs; a++) {
                XiValue *orig = src_phi->value.args[a];
                if (orig && orig->id < callee_max_id && value_map[orig->id])
                    phi_clone->args[a] = value_map[orig->id];
                else
                    phi_clone->args[a] = orig;
            }
            if (src_phi->value.id < callee_max_id)
                value_map[src_phi->value.id] = phi_clone;
        }
    }

    /* Wire call_blk → callee entry block. */
    call_blk->kind = XI_BLOCK_PLAIN;
    call_blk->control = NULL;
    call_blk->succs[0] = cloned_blks[0];
    call_blk->succs[1] = NULL;

    /* Set up cont_blk predecessors so phi nodes (if any) are well-formed. */
    for (uint32_t r = 0; r < nret; r++)
        xi_block_add_pred(cont_blk, ret_blocks[r]);

    bool is_multi_ret = shape_multi_ret;
    uint16_t nresult_elems = shape_elems;
    XR_DCHECK(nresult_elems <= 16, "inline: too many multi-return elements");

    /* Build per-element result values.
     * Single return block  → use value directly.
     * Multiple return blocks → create a phi per element in cont_blk. */
    XiValue *results[16];
    memset(results, 0, sizeof(results));

    for (uint16_t ei = 0; ei < nresult_elems; ei++) {
        if (nret == 0) {
            results[ei] = NULL;
        } else if (nret == 1) {
            results[ei] = is_multi_ret ? ret_values[0]->args[ei] : ret_values[0];
        } else {
            /* Multiple return paths: merge via phi in cont_blk. */
            XiValue *first = is_multi_ret ? ret_values[0]->args[ei] : ret_values[0];
            struct XrType *phi_type = first ? first->type : call_val->type;
            XiPhi *join = phi_type ? xi_phi_new(caller, cont_blk, phi_type, (uint16_t) nret) : NULL;
            if (join) {
                for (uint32_t r = 0; r < nret; r++) {
                    join->value.args[r] = is_multi_ret ? ret_values[r]->args[ei] : ret_values[r];
                }
                results[ei] = &join->value;
            } else {
                results[ei] = first; /* fallback: first path */
            }
        }
    }

    /* Replace uses of call_val and its XI_EXTRACT dependents.
     *
     * For multi-return: XI_EXTRACT(call_val, idx) → results[idx],
     *                   direct uses of call_val   → results[0].
     * For single-return: all uses of call_val     → results[0]. */
    for (uint32_t b = 0; b < caller->nblocks; b++) {
        XiBlock *blk = caller->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;

            /* Rewrite XI_EXTRACT into XI_COPY forwarding the element value.
             * DCE will clean up the now-redundant copy. */
            if (v->op == XI_EXTRACT && v->nargs >= 1 && v->args[0] == call_val) {
                uint32_t idx = (uint32_t) v->aux_int;
                XiValue *elem = (idx < nresult_elems) ? results[idx] : NULL;
                if (elem) {
                    v->op = XI_COPY;
                    v->args[0] = elem;
                    v->nargs = 1;
                    v->aux_int = 0;
                }
                continue;
            }

            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] == call_val && results[0])
                    v->args[a] = results[0];
            }
        }
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == call_val && results[0])
                    phi->value.args[a] = results[0];
            }
        }
        if (blk->control == call_val && results[0])
            blk->control = results[0];
    }

    xr_free(value_map);
    xr_free(cloned_blks);
    return true;
}

/* ========== Pass Driver ========== */

XR_FUNC uint32_t xi_inline_budget(uint32_t caller_size) {
    if (caller_size < 100)
        return XI_INLINE_MAX_PER_PASS + 2;
    if (caller_size > 300)
        return XI_INLINE_MAX_PER_PASS - 2;
    return XI_INLINE_MAX_PER_PASS;
}

XR_FUNC XiPassChange xi_opt_inline(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_inline: NULL func");

    bool any_inlined = false;

    /* Single scan: inline up to budget call sites per pass invocation
     * to limit code growth.  Budget scales with caller size. */
    uint32_t inlined_count = 0;
    uint32_t caller_size = caller_total_values(f);
    uint32_t budget = xi_inline_budget(caller_size);

    for (uint32_t bi = 0; bi < f->nblocks && inlined_count < budget; bi++) {
        XiBlock *blk = f->blocks[bi];
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->op != XI_CALL)
                continue;
            if (v->nargs < 1)
                continue;

            XiFunc *callee = resolve_callee(v->args[0]);
            if (!callee)
                continue;
            if (callee == f)
                continue; /* no self-recursion */
            if (callee->nblocks == 0)
                continue;

            XiInlineCostModel cm = analyze_callee(callee);
            if (cm.value_count > XI_INLINE_MAX_COST)
                continue;
            /* callee == f is filtered above; calls_self stays false here.
             * The benefit function still honours calls_self, so any future
             * indirect-recursion analysis can set it before scoring. */

            XiInlineCallSiteInfo si = {
                .all_args_const = all_args_are_const(v),
                .single_call_site = false, /* conservative: unknown */
                .caller_size = caller_size,
            };

            if (xi_inline_benefit(&cm, &si) <= 0)
                continue;

            if (inline_call_site(f, blk, vi, v, callee)) {
                any_inlined = true;
                inlined_count++;
                break; /* restart block scan (block was split) */
            }
        }
    }

    if (!any_inlined)
        return xi_pass_no_change();

    XiPassChange chg = xi_pass_no_change();
    chg.cfg_changed = true;
    chg.values_changed = true;
    return chg;
}
