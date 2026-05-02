/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_liveness2.c - Dataflow-based liveness analysis for Xm
 *
 * KEY CONCEPT:
 *   Standard backward dataflow analysis computing live-in/live-out
 *   sets per basic block. Phi arguments are treated as live-out of
 *   the corresponding predecessor block (not the block containing
 *   the phi).
 */

#include "xm_liveness2.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>

/*
 * Pass 1: compute local def/use sets for each block.
 *
 * For each instruction in program order:
 *   - args that are vregs and NOT already in def → add to use
 *   - dst that is a vreg → add to def
 *
 * Phi nodes are handled separately in the dataflow iteration
 * (phi args are live-out of predecessor, not use of this block).
 * Phi dsts ARE definitions of this block.
 */
static void compute_local_sets(XmLive *live, XmFunc *func) {
    XR_DCHECK(live != NULL, "compute_local_sets: NULL live");
    XR_DCHECK(func != NULL, "compute_local_sets: NULL func");
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        XmBlockLive *bl = &live->blocks[bi];

        // Phi destinations are defs
        for (XmPhi *phi = blk->phis; phi; phi = phi->next) {
            if (xm_ref_is_vreg(phi->dst)) {
                uint32_t idx = XM_REF_INDEX(phi->dst);
                if (idx < live->nvreg)
                    xm_bset_set(&bl->def, idx);
            }
        }

        // Instructions
        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];

            // Uses: args that are vregs and not yet defined in this block
            for (int a = 0; a < 2; a++) {
                if (xm_ref_is_vreg(ins->args[a])) {
                    uint32_t idx = XM_REF_INDEX(ins->args[a]);
                    if (idx < live->nvreg && !xm_bset_has(&bl->def, idx))
                        xm_bset_set(&bl->use, idx);
                }
            }

            // Pool args: vregs used as call arguments (in call_arg_pool)
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t dvi = XM_REF_INDEX(ins->dst);
                if (dvi < func->nvreg && func->vregs[dvi].call_nargs > 0) {
                    XmVReg *dvr = &func->vregs[dvi];
                    for (uint16_t pi = 0; pi < dvr->call_nargs; pi++) {
                        XmRef pa = func->call_arg_pool[dvr->call_arg_start + pi];
                        if (xm_ref_is_vreg(pa)) {
                            uint32_t pidx = XM_REF_INDEX(pa);
                            if (pidx < live->nvreg && !xm_bset_has(&bl->def, pidx))
                                xm_bset_set(&bl->use, pidx);
                        }
                    }
                }
            }

            // Def: dst vreg
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t idx = XM_REF_INDEX(ins->dst);
                if (idx < live->nvreg)
                    xm_bset_set(&bl->def, idx);
            }
        }

        // Terminator arg is a use
        if (xm_ref_is_vreg(blk->jmp.arg)) {
            uint32_t idx = XM_REF_INDEX(blk->jmp.arg);
            if (idx < live->nvreg && !xm_bset_has(&bl->def, idx))
                xm_bset_set(&bl->use, idx);
        }
    }
}

/*
 * Pass 2: iterate backward dataflow equations until fixed point.
 *
 *   live_out[B] = ∪ live_in[S]  for each successor S of B
 *               + phi args in S that correspond to B as predecessor
 *
 *   live_in[B]  = use[B] ∪ (live_out[B] \ def[B])
 */
static void iterate_dataflow(XmLive *live, XmFunc *func, const uint32_t *id_to_idx,
                             uint32_t id_map_size) {
    uint32_t nv = live->nvreg;
    XmBSet tmp;
    xm_bset_init(&tmp, nv);

    bool changed = true;
    while (changed) {
        changed = false;

        // Process blocks in reverse order for faster convergence
        for (int bi = (int) func->nblk - 1; bi >= 0; bi--) {
            XmBlock *blk = func->blocks[bi];
            XmBlockLive *bl = &live->blocks[bi];

            // Save old live_in for change detection
            xm_bset_copy(&tmp, &bl->live_in);

            // Compute live_out = union of successor live_in sets
            xm_bset_zero(&bl->live_out);

            XmBlock *succs[2] = {blk->s1, blk->s2};
            for (int s = 0; s < 2; s++) {
                if (!succs[s])
                    continue;

                // Map successor block ID → layout index
                uint32_t sid = succs[s]->id;
                if (sid >= id_map_size)
                    continue;
                uint32_t si = id_to_idx[sid];
                if (si >= func->nblk)
                    continue;

                xm_bset_union(&bl->live_out, &live->blocks[si].live_in);
            }

            /* Add phi arguments: for each successor's phi, if B is
             * predecessor p, then phi->args[p] is live-out of B. */
            for (int s = 0; s < 2; s++) {
                if (!succs[s])
                    continue;
                XmBlock *succ = succs[s];

                // Find which predecessor index B is in succ
                for (XmPhi *phi = succ->phis; phi; phi = phi->next) {
                    for (uint32_t p = 0; p < phi->narg && p < succ->npred; p++) {
                        if (succ->preds[p] != blk)
                            continue;
                        if (xm_ref_is_vreg(phi->args[p])) {
                            uint32_t idx = XM_REF_INDEX(phi->args[p]);
                            if (idx < nv)
                                xm_bset_set(&bl->live_out, idx);
                        }
                    }
                }
            }

            // Compute live_in = use ∪ (live_out \ def)
            xm_bset_copy(&bl->live_in, &bl->live_out);
            xm_bset_diff(&bl->live_in, &bl->def);
            xm_bset_union(&bl->live_in, &bl->use);

            // Check for change
            if (!xm_bset_equal(&bl->live_in, &tmp))
                changed = true;
        }
    }

    xm_bset_free(&tmp);
}

void xm_live_compute(XmLive *live, XmFunc *func) {
    XR_DCHECK(live != NULL, "xm_live_compute: NULL live");
    if (!func || func->nblk == 0) {
        memset(live, 0, sizeof(*live));
        return;
    }

    live->nblk = func->nblk;
    live->nvreg = func->nvreg;
    live->blocks = xr_calloc(func->nblk, sizeof(XmBlockLive));

    // Initialize bitsets for each block
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlockLive *bl = &live->blocks[bi];
        xm_bset_init(&bl->def, func->nvreg);
        xm_bset_init(&bl->use, func->nvreg);
        xm_bset_init(&bl->live_in, func->nvreg);
        xm_bset_init(&bl->live_out, func->nvreg);
    }

    /* Build block ID → layout index mapping.
     * Block IDs may differ from layout indices after dead block
     * elimination or block reordering. Without this mapping,
     * iterate_dataflow would use block IDs to index live->blocks[]
     * (which is indexed by layout position), producing wrong results. */
    uint32_t max_bid = 0;
    for (uint32_t i = 0; i < func->nblk; i++)
        if (func->blocks[i]->id > max_bid)
            max_bid = func->blocks[i]->id;
    uint32_t id_map_size = max_bid + 1;
    uint32_t *id_to_idx = xr_calloc(id_map_size, sizeof(uint32_t));
    for (uint32_t i = 0; i < id_map_size; i++)
        id_to_idx[i] = UINT32_MAX;
    for (uint32_t i = 0; i < func->nblk; i++)
        id_to_idx[func->blocks[i]->id] = i;

    compute_local_sets(live, func);
    iterate_dataflow(live, func, id_to_idx, id_map_size);

    xr_free(id_to_idx);
}

void xm_live_free(XmLive *live) {
    if (!live->blocks)
        return;
    for (uint32_t bi = 0; bi < live->nblk; bi++) {
        XmBlockLive *bl = &live->blocks[bi];
        xm_bset_free(&bl->def);
        xm_bset_free(&bl->use);
        xm_bset_free(&bl->live_in);
        xm_bset_free(&bl->live_out);
    }
    xr_free(live->blocks);
    live->blocks = NULL;
    live->nblk = 0;
}
