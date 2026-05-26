/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt_tail_call.c - Self-tail-call to loop transformation
 *
 * ALGORITHM:
 *   For each RETURN block whose control is the result of an XI_CALL:
 *     1. Resolve the callee.  If it is the same XiFunc (self-recursive),
 *        and the call result is used only as the return value, it is
 *        a self-tail-call.
 *     2. Replace the call with XI_COPY ops that write new argument
 *        values into phi nodes at the entry block.
 *     3. Change the block from RETURN to PLAIN with a back-edge to
 *        a loop header (inserted between entry and original successors).
 *
 *   The transformation introduces a loop header with phi nodes that
 *   merge the initial parameter values (from the original entry) with
 *   the updated values (from the tail-call site).
 *
 * LIMITATIONS:
 *   - Only self-recursion (callee == current function).
 *   - Call result must be the sole return value (no post-call ops).
 *   - Does not handle mutual recursion.
 */

#include "xi_opt_tail_call.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

/* ========== Callee Resolution ========== */

/* Trace callee to an XI_CLOSURE_NEW with a known XiFunc*. */
static XiFunc *resolve_callee(const XiValue *v) {
    if (!v)
        return NULL;
    if (v->op == XI_CLOSURE_NEW && v->aux)
        return (XiFunc *) v->aux;
    if (v->op == XI_COPY && v->nargs >= 1)
        return resolve_callee(v->args[0]);
    return NULL;
}

/* ========== Tail-Call Detection ========== */

/* Check if blk is a RETURN block whose control is the result of an
 * XI_CALL to 'self', and that call is the last value in the block.
 * Returns the call XiValue, or NULL if not a self-tail-call. */
static XiValue *find_self_tail_call(const XiBlock *blk, const XiFunc *self) {
    if (blk->kind != XI_BLOCK_RETURN || !blk->control)
        return NULL;

    XiValue *ret_val = blk->control;

    /* The return value must be an XI_CALL defined in the same block. */
    if (ret_val->op != XI_CALL)
        return NULL;
    if (ret_val->nargs < 1)
        return NULL;

    /* Must be the last value in the block (no post-call ops). */
    if (blk->nvalues == 0 || blk->values[blk->nvalues - 1] != ret_val)
        return NULL;

    /* Callee must be self. */
    XiFunc *callee = resolve_callee(ret_val->args[0]);
    if (callee != self)
        return NULL;

    return ret_val;
}

static bool is_param_value(const XiFunc *f, const XiValue *v) {
    if (!f || !v)
        return false;
    for (uint16_t p = 0; p < f->nparams; p++) {
        if (f->params[p] == v)
            return true;
    }
    return false;
}

/* ========== Loop Header Construction ========== */

/* Insert a loop header between entry and its original successors.
 * The header has phi nodes for each parameter.  Entry jumps to header.
 * Returns the header block, or NULL on failure.
 *
 * Before:  entry -> (original successors)
 * After:   entry -> header -> (original successors)
 *          tail-call blocks jump back to header. */
static XiBlock *insert_loop_header(XiFunc *f) {
    XR_DCHECK(f != NULL && f->entry != NULL, "insert_loop_header: NULL func/entry");

    XiBlock *entry = f->entry;
    XiBlock *header = xi_block_new(f);
    if (!header)
        return NULL;

    /* Header inherits entry's terminator and successors. */
    header->kind = entry->kind;
    header->control = entry->control;
    header->succs[0] = entry->succs[0];
    header->succs[1] = entry->succs[1];

    header->values = (XiValue **) xi_func_arena_alloc(f, entry->values_cap * sizeof(XiValue *));
    if (!header->values)
        return NULL;
    header->values_cap = entry->values_cap;
    header->nvalues = 0;

    uint32_t entry_write = 0;
    for (uint32_t i = 0; i < entry->nvalues; i++) {
        XiValue *v = entry->values[i];
        if (is_param_value(f, v)) {
            entry->values[entry_write++] = v;
        } else {
            header->values[header->nvalues++] = v;
            if (v)
                v->block = header;
        }
    }
    entry->nvalues = entry_write;

    /* Update predecessor lists: replace 'entry' with 'header' in
     * each original successor's pred list. */
    for (int s = 0; s < 2; s++) {
        XiBlock *succ = entry->succs[s];
        if (!succ)
            continue;
        for (uint32_t p = 0; p < succ->npreds; p++) {
            if (succ->preds[p] == entry)
                succ->preds[p] = header;
        }
    }

    /* Entry becomes a plain jump to header. */
    entry->kind = XI_BLOCK_PLAIN;
    entry->control = NULL;
    entry->succs[0] = header;
    entry->succs[1] = NULL;
    xi_block_add_pred(header, entry);

    /* Create phi nodes in header for each parameter.
     * Initially one incoming edge: entry.  Tail-call edges are added later. */
    for (uint16_t p = 0; p < f->nparams; p++) {
        XiValue *param = f->params[p];
        if (!param)
            continue;
        XiPhi *phi = xi_phi_new(f, header, param->type, 1);
        if (!phi)
            continue;
        phi->value.aux_int = p;
        phi->value.args[0] = param; /* from entry */
    }

    header->sealed = true;
    return header;
}

/* ========== Tail-Call Rewriting ========== */

/* Rewrite a self-tail-call block to jump back to the loop header.
 * - Remove the XI_CALL value.
 * - Add phi args for the header's parameter phis.
 * - Change block to PLAIN with succs[0] = header. */
static bool rewrite_tail_call(XiFunc *f, XiBlock *blk, XiValue *call, XiBlock *header) {
    XR_DCHECK(blk != NULL && call != NULL && header != NULL, "rewrite_tail_call: NULL arg");

    uint16_t nparams = f->nparams;

    /* Add this block as a predecessor of header. */
    xi_block_add_pred(header, blk);

    /* Extend each phi in header with the new argument from the call. */
    for (XiPhi *phi = header->phis; phi; phi = phi->next) {
        uint16_t phi_idx = (uint16_t) phi->value.aux_int;
        if (phi_idx >= nparams)
            continue;
        /* call->args[0] = callee, args[1..] = actual arguments */
        uint16_t arg_idx = phi_idx + 1;
        XiValue *new_arg = (arg_idx < call->nargs) ? call->args[arg_idx] : NULL;

        /* Grow phi args array to accommodate the new predecessor. */
        uint16_t new_nargs = phi->value.nargs + 1;
        XiValue **new_args = (XiValue **) xi_func_arena_alloc(f, new_nargs * sizeof(XiValue *));
        if (!new_args)
            return false;
        if (phi->value.args)
            memcpy(new_args, phi->value.args, phi->value.nargs * sizeof(XiValue *));
        new_args[phi->value.nargs] = new_arg;
        phi->value.args = new_args;
        phi->value.nargs = new_nargs;
    }

    /* Remove the call from the block's values. */
    uint32_t write = 0;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        if (blk->values[i] != call)
            blk->values[write++] = blk->values[i];
    }
    blk->nvalues = write;

    /* Redirect uses of call result to the phi values in header.
     * (In a proper tail call, the only use is block->control which
     * we're removing, but be safe.) */

    /* Change block to PLAIN jumping to header. */
    blk->kind = XI_BLOCK_PLAIN;
    blk->control = NULL;
    blk->succs[0] = header;
    blk->succs[1] = NULL;

    return true;
}

/* Replace uses of original params with the header phi values
 * throughout the function (except in the entry block). */
static void replace_params_with_phis(XiFunc *f, XiBlock *header) {
    /* Build param→phi mapping. */
    XiValue *phi_map[64];
    memset(phi_map, 0, sizeof(phi_map));
    for (XiPhi *phi = header->phis; phi; phi = phi->next) {
        uint16_t pi = (uint16_t) phi->value.aux_int;
        if (pi < f->nparams && pi < 64)
            phi_map[pi] = &phi->value;
    }

    /* Replace in all blocks except entry. */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (blk == f->entry)
            continue;

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                for (uint16_t p = 0; p < f->nparams && p < 64; p++) {
                    if (v->args[a] == f->params[p] && phi_map[p])
                        v->args[a] = phi_map[p];
                }
            }
        }
        if (blk != header) {
            /* Also replace in phi args */
            for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
                for (uint16_t a = 0; a < phi->value.nargs; a++) {
                    for (uint16_t p = 0; p < f->nparams && p < 64; p++) {
                        if (phi->value.args[a] == f->params[p] && phi_map[p])
                            phi->value.args[a] = phi_map[p];
                    }
                }
            }
        }
        /* Replace in block control */
        if (blk->control) {
            for (uint16_t p = 0; p < f->nparams && p < 64; p++) {
                if (blk->control == f->params[p] && phi_map[p])
                    blk->control = phi_map[p];
            }
        }
    }
}

/* ========== Pass Driver ========== */

XR_FUNC XiPassChange xi_opt_tail_call(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_tail_call: NULL func");

    if (f->nblocks < 2 || f->nparams == 0 || f->nparams > 64)
        return xi_pass_no_change();

    /* Scan for self-tail-call sites. */
    uint32_t tail_count = 0;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        if (find_self_tail_call(f->blocks[b], f))
            tail_count++;
    }

    if (tail_count == 0)
        return xi_pass_no_change();

    /* Insert loop header. */
    XiBlock *header = insert_loop_header(f);
    if (!header)
        return xi_pass_no_change();

    /* Replace param references with header phi values. */
    replace_params_with_phis(f, header);

    /* Rewrite each tail-call block. */
    bool any = false;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        XiValue *call = find_self_tail_call(blk, f);
        if (!call)
            continue;
        if (rewrite_tail_call(f, blk, call, header))
            any = true;
    }

    if (!any)
        return xi_pass_no_change();

    XiPassChange chg = xi_pass_no_change();
    chg.cfg_changed = true;
    chg.values_changed = true;
    return chg;
}
